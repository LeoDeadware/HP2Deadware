// Dispatch: every guest call reaches a lifted function through its slot
// (fp_<VA>, lifted with --overridable), an HLE override in that slot, a
// kernel export thunk or a host thunk. Plus the runtime services lifted code
// calls (lifted.h): fatal paths, CPUID, RDTSC, resume.

#include <dlfcn.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstring>
#include <exception>
#include <utility>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "runtime/hp2_internal.h"
#include "runtime/platform.h"

namespace recomp {
uint8_t* g_guest_base = nullptr;
// table.cpp of the published lift (lifted with --overridable).
extern GuestFn* const kLiftedSlots[];
}  // namespace recomp

// Kernel HLE (src/hle/kernel/kernel_exports.h). Weak: the runtime links and
// runs without it, stopping at the first kernel call it cannot serve.
HostFn hp2_kernel_lookup(uint32_t ordinal) __attribute__((weak));
uint32_t hp2_kernel_data_va(uint32_t ordinal) __attribute__((weak));
uint64_t hp2_kernel_rdtsc() __attribute__((weak));

using namespace hp2rt;

void hp2_game_fixes_link();  // hp2_game_fixes.cpp

namespace {

// Slot of every lifted function, by va - code_lo.
std::vector<recomp::GuestFn*> g_slots;
uint32_t g_code_lo = 0, g_code_size = 0;

struct Pending {
  uint32_t va;
  HostFn fn;
  const char* name;
};
std::vector<Pending>& PendingOverrides() {
  static std::vector<Pending> v;
  return v;
}

// Kernel exports: one per ordinal.
struct KernelExport {
  HostFn fn = nullptr;
  const char* name = "?";
};
KernelExport g_kernel[512];

// Host thunks.
struct HostThunk {
  HostFn fn;
  const char* name;
};
std::vector<HostThunk> g_host_thunks;
std::mutex g_host_thunk_mu;

recomp::GuestFn* SlotOf(uint32_t va) {
  if (va - g_code_lo >= g_code_size) return nullptr;
  return g_slots[va - g_code_lo];
}

thread_local uint32_t t_kord = 0;  // ordinal of the kernel call in progress

void KernelCall(Hp2Ctx& c, uint32_t ord) {
  const KernelExport& k = g_kernel[ord];
  const bool trace = hp2_ktrace_enabled();
  const uint32_t ret = hp2_rd32(c.esp);
  if (trace)
    hp2_log("K> %-32s (%08X %08X %08X %08X) from %08X", k.name, hp2_arg(c, 0),
            hp2_arg(c, 1), hp2_arg(c, 2), hp2_arg(c, 3), ret);
  t_kord = ord;
  k.fn(c);
  if (trace) hp2_log("K< %-32s = %08X", k.name, c.eax);
}

// A kernel function export nobody implements: stop with its name.
void MissingKernelExport(Hp2Ctx& c) {
  hp2_log("hp2: kernel export #%u %s not implemented (called from %08X)", t_kord,
          hp2_kernel_name(t_kord), hp2_rd32(c.esp));
  hp2_exit(10, "unimplemented kernel export");
}

// HP2_STOP_AT=<section>: every function of that section stops the run when
// called (phase-1 milestone: first call into D3D).
std::string g_stop_section;
void StopStub(Hp2Ctx& c) {
  uint32_t ret = hp2_rd32(c.esp);
  uint32_t callee = 0;
  if (hp2_rd8(ret - 5) == 0xE8) callee = ret + hp2_rd32(ret - 4);
  hp2_log("hp2: first call into %s: %08X from %08X (esp %08X)", g_stop_section.c_str(),
          callee, ret, c.esp);
  hp2_exit(0, ("reached " + g_stop_section).c_str());
}

// HP2_STUB=va:eax:nargs[,...] (dev switch, for bisecting boot blockers):
// the guest function at va returns eax at once, popping nargs stack args.
struct Stub {
  uint32_t va, eax, nargs;
};
Stub g_stubs[16];
template <int I>
void StubFn(Hp2Ctx& c) {
  hp2_log("hp2: stub %08X -> %08X", g_stubs[I].va, g_stubs[I].eax);
  hp2_ret_stdcall(c, g_stubs[I].eax, int(g_stubs[I].nargs));
}
template <int... I>
constexpr std::array<HostFn, sizeof...(I)> StubTable(std::integer_sequence<int, I...>) {
  return {StubFn<I>...};
}
constexpr auto kStubFns = StubTable(std::make_integer_sequence<int, 16>{});

void ApplyStubs() {
  std::string spec = Env("HP2_STUB");
  size_t n = 0;
  for (size_t pos = 0; pos < spec.size() && n < kStubFns.size();) {
    size_t end = spec.find(',', pos);
    if (end == std::string::npos) end = spec.size();
    Stub st{};
    if (std::sscanf(spec.substr(pos, end - pos).c_str(), "%x:%x:%u", &st.va, &st.eax,
                    &st.nargs) >= 2) {
      g_stubs[n] = st;
      if (!hp2_override(st.va, kStubFns[n])) hp2_fatal("HP2_STUB: %08X is not a function", st.va);
      hp2_log("hp2: HP2_STUB %08X -> eax %08X, %u args", st.va, st.eax, st.nargs);
      n++;
    }
    pos = end + 1;
  }
}

}  // namespace

void hp2_register_override_at_start(uint32_t va, HostFn fn, const char* name) {
  if (!g_slots.empty()) {
    if (!hp2_override(va, fn)) hp2_fatal("override %s: %08X is not a function", name, va);
    return;
  }
  PendingOverrides().push_back({va, fn, name});
}

bool hp2_override(uint32_t va, HostFn fn) {
  recomp::GuestFn* s = SlotOf(va);
  if (!s) return false;
  *s = fn;
  return true;
}

HostFn hp2_original(uint32_t va) {
  for (uint32_t i = 0; i < recomp::kLiftedFunctionCount; i++)
    if (recomp::kLiftedFunctions[i].va == va) return recomp::kLiftedFunctions[i].fn;
  return nullptr;
}

bool hp2_is_function(uint32_t va) { return SlotOf(va) != nullptr; }

uint32_t hp2_host_thunk(HostFn fn, const char* name) {
  std::lock_guard<std::mutex> lk(g_host_thunk_mu);
  uint32_t n = uint32_t(g_host_thunks.size());
  if ((n + 1) * 16 > hp2_layout::kHostThunkSize) hp2_fatal("host thunk region full");
  g_host_thunks.push_back({fn, name});
  return hp2_layout::kHostThunkBase + n * 16;
}

void hp2_dispatch(Hp2Ctx& c, uint32_t va) { recomp::Dispatch(c, va, 0); }

uint32_t hp2_call_guest(Hp2Ctx& c, uint32_t va, const uint32_t* args, int nargs) {
  const uint32_t esp0 = c.esp;
  for (int i = nargs - 1; i >= 0; i--) {
    c.esp -= 4;
    hp2_wr32(c.esp, args[i]);
  }
  c.esp -= 4;
  hp2_wr32(c.esp, 0xFEFEFEFE);  // return address: host
  recomp::Dispatch(c, va, 0);
  c.esp = esp0;
  return c.eax;
}

namespace hp2rt {

void InitDispatch() {
  recomp::g_guest_base = g_mem.base();
  g_code_lo = g_xbe.code_lo;
  g_code_size = g_xbe.code_hi - g_xbe.code_lo;
  g_slots.assign(g_code_size, nullptr);
  uint32_t n = 0;
  for (uint32_t i = 0; i < recomp::kLiftedFunctionCount; i++) {
    uint64_t va = recomp::kLiftedFunctions[i].va;
    if (va - g_code_lo < g_code_size) {
      g_slots[va - g_code_lo] = recomp::kLiftedSlots[i];
      n++;
    }
  }
  if (!SlotOf(g_xbe.entry))
    hp2_fatal("entry %08X is not in the lifted table (lift/LIFT_ID points at a lift of "
              "another XBE?)", g_xbe.entry);
  hp2_log("hp2: %u lifted functions in %08X..%08X", n, g_code_lo, g_xbe.code_hi);
  hp2_game_fixes_link();  // keeps hp2_game_fixes.cpp (its HP2_OVERRIDEs) linked
  for (const Pending& p : PendingOverrides())
    if (!hp2_override(p.va, p.fn))
      hp2_fatal("override %s: %08X is not a lifted function", p.name, p.va);
  if (!PendingOverrides().empty())
    hp2_log("hp2: %zu overrides applied", PendingOverrides().size());
  PendingOverrides().clear();

  ApplyStubs();
  g_stop_section = Env("HP2_STOP_AT");
  if (!g_stop_section.empty()) {
    uint32_t va, size, stubs = 0;
    if (!hp2_section(g_stop_section.c_str(), &va, &size))
      hp2_fatal("HP2_STOP_AT=%s: no such section", g_stop_section.c_str());
    for (uint32_t a = va; a < va + size; a++)
      if (recomp::GuestFn* s = SlotOf(a)) {
        *s = StopStub;
        stubs++;
      }
    hp2_log("hp2: stop at the first call into %s (%u functions)", g_stop_section.c_str(),
            stubs);
  }
}

void BindKernelImports() {
  uint32_t funcs = 0, data = 0, missing = 0;
  std::string missing_names;
  for (uint32_t slot = g_xbe.kernel_thunk;; slot += 4) {
    uint32_t v = hp2_rd32(slot);
    if (!v) break;
    uint32_t ord = v & 0x7FFFFFFF;
    if (ord >= 512) hp2_fatal("kernel import ordinal %u out of range", ord);
    KernelExport& k = g_kernel[ord];
    k.name = hp2_kernel_name(ord);
    uint32_t dva = hp2_kernel_data_va ? hp2_kernel_data_va(ord) : 0;
    if (dva) {
      hp2_wr32(slot, dva);
      data++;
      continue;
    }
    HostFn fn = hp2_kernel_lookup ? hp2_kernel_lookup(ord) : nullptr;
    // Known data exports must come from the kernel HLE; without it they get
    // a zeroed block so the run can go on until a function is missing.
    static const uint32_t kDataOrdinals[] = {16,  22,  40,  64,  71,  102, 120, 154, 156,
                                             157, 162, 164, 240, 259, 321, 322, 323, 324,
                                             325, 326, 353, 354, 355, 356, 357};
    bool is_data = false;
    for (uint32_t d : kDataOrdinals) is_data |= d == ord;
    if (!fn && is_data) {
      uint32_t blk = hp2_hle_alloc(0x1000);
      hp2_wr32(slot, blk);
      hp2_log("hp2: warning: kernel data export #%u %s not provided: zeroed block %08X", ord,
              k.name, blk);
      data++;
      continue;
    }
    if (!fn) {
      fn = MissingKernelExport;
      missing++;
      missing_names += std::string(" ") + k.name;
    }
    k.fn = fn;
    hp2_wr32(slot, hp2_layout::kThunkBase + ord * hp2_layout::kThunkStride);
    funcs++;
  }
  hp2_log("hp2: kernel imports: %u functions, %u data, %u not implemented%s", funcs, data,
          missing, missing ? ":" : "");
  if (missing) hp2_log("  %s", missing_names.c_str());
}

namespace {
// Guest faults in lifted code go to the guest's SEH chain as
// STATUS_ACCESS_VIOLATION, raised through the kernel HLE's RtlRaiseException
// (it walks fs:[0] and calls the lifted handlers):
// - a handler that continues: the faulting instruction is retried when the
//   signal handler returns (it may have committed the page);
// - an __except block: a resume site throws GuestResume. A signal frame
//   cannot be unwound on this host, so the handler catches it and rewrites
//   the signal context so the faulting function "calls" RethrowFault; the
//   exception then leaves through ordinary frames to the function owning the
//   __except frame (lifted code is built with -fnon-call-exceptions and
//   frame records in leaf functions);
// - no handler: the kernel reports the unhandled exception and stops.
// Lifted code keeps guest registers in locals, so Ctx.esp is the last
// spilled value and the live stack may reach below it by up to one
// function's frame: at most 0xC00 bytes in this XBE (no _chkstk; largest
// `sub esp, imm` is 0xC00 at 0x1112BA). The exception is raised kFaultGap
// below it.
constexpr uint32_t kFaultGap = 0x1800;
constexpr int kMaxFaultDepth = 8;
thread_local int t_fault_depth = 0;
thread_local std::exception_ptr t_fault_exception;

[[noreturn]] void RethrowFault() {
  std::exception_ptr e = std::move(t_fault_exception);
  t_fault_exception = nullptr;
  std::rethrow_exception(e);
}

// Guest VA of the lifted function containing host pc (its symbol is
// recomp::gen::f_<VA>), 0 if pc is not in lifted code.
uint32_t LiftedFunctionAt(uintptr_t pc) {
  Dl_info info{};
  if (!dladdr(reinterpret_cast<void*>(pc), &info) || !info.dli_sname) return 0;
  const char* f = std::strstr(info.dli_sname, "6recomp3gen");
  if (!f || !(f = std::strstr(f, "f_"))) return 0;
  char hex[9] = {};
  std::memcpy(hex, f + 2, 8);  // f_XXXXXXXX, then the mangled signature
  return uint32_t(std::strtoul(hex, nullptr, 16));
}

// Raises the access violation on the current guest thread; true if a
// handler continued execution.
bool RaiseAccessViolation(Hp2Ctx& c, bool write, uint32_t addr, uint32_t fn_va) {
  HostFn raise = hp2_kernel_lookup ? hp2_kernel_lookup(302) : nullptr;  // RtlRaiseException
  if (!raise) return false;
  const uint32_t stack_limit = hp2_rd32(c.fs_base + 8);
  const uint32_t saved_esp = c.esp;
  if (c.esp < stack_limit + kFaultGap + 0x1000)
    hp2_fatal("guest stack exhausted raising an access violation at %08X (esp %08X, limit %08X)",
              addr, c.esp, stack_limit);
  // EXCEPTION_RECORD: code, flags, record, address, nparams, info[15].
  const uint32_t rec = (c.esp - kFaultGap - 0x50) & ~0xFu;
  std::memset(hp2_ptr(rec), 0, 0x50);
  hp2_wr32(rec + 0x00, 0xC0000005);
  hp2_wr32(rec + 0x0C, fn_va);  // the faulting function (instruction unknown)
  hp2_wr32(rec + 0x10, 2);
  hp2_wr32(rec + 0x14, write ? 1 : 0);
  hp2_wr32(rec + 0x18, addr);
  c.esp = rec - 8;
  hp2_wr32(c.esp + 4, rec);    // RtlRaiseException(record)
  hp2_wr32(c.esp, fn_va);      // "return address"
  raise(c);                    // returns only if a handler continued
  c.esp = saved_esp;
  return true;
}

void FaultHandler(int sig, siginfo_t* si, void* uc) {
  uintptr_t a = reinterpret_cast<uintptr_t>(si->si_addr);
  uintptr_t base = reinterpret_cast<uintptr_t>(g_mem.base());
  const bool guest = a - base < (1ull << 32);
  recomp::platform::FaultContext fc = recomp::platform::ReadFaultContext(uc);
  Hp2Ctx* c = hp2_current_ctx();
  const uint32_t fn = guest && c ? LiftedFunctionAt(fc.pc) : 0;
  if (fn && t_fault_depth < kMaxFaultDepth) {
    const uint32_t gaddr = uint32_t(a - base);
    if (hp2_ktrace_enabled())
      hp2_log("hp2: access violation (%s %08X) in %08X: raising", fc.write ? "write" : "read",
              gaddr, fn);
    ++t_fault_depth;
    bool cont;
    try {
      cont = RaiseAccessViolation(*c, fc.write, gaddr, fn);
    } catch (...) {
      --t_fault_depth;
      t_fault_exception = std::current_exception();
      if (recomp::platform::RedirectToCall(uc, RethrowFault)) return;
      hp2_fatal("cannot resume after a guest exception from a fault on this host");
    }
    --t_fault_depth;
    if (cont) return;  // retry the faulting instruction
  }
  if (guest)
    hp2_fatal("%s: guest memory fault (%s) at guest %08lX in %s%08X", sig == SIGSEGV ? "SIGSEGV" : "SIGBUS",
              fc.write ? "write" : "read", static_cast<unsigned long>(a - base),
              fn ? "lifted f_" : "host code, near guest ", fn);
  hp2_fatal("%s: host fault at %p (runtime bug)", sig == SIGSEGV ? "SIGSEGV" : "SIGBUS",
            si->si_addr);
}
}  // namespace

void InstallFaultHandler() {
  constexpr size_t kSize = 1u << 20;
  stack_t ss{};
  ss.ss_sp = mmap(nullptr, kSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  ss.ss_size = kSize;
  sigaltstack(&ss, nullptr);
  struct sigaction sa{};
  sa.sa_sigaction = FaultHandler;
  // SA_NODEFER: a guest exception filter may fault again (nested raise).
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
  sigaction(SIGSEGV, &sa, nullptr);
  sigaction(SIGBUS, &sa, nullptr);
}

}  // namespace hp2rt

// ---- lifted.h services --------------------------------------------------
namespace recomp {

void Dispatch(Ctx& c, uint32_t target, uint32_t va) {
  if (GuestFn* s = SlotOf(target)) {
    (*s)(c);
    return;
  }
  if (target - hp2_layout::kThunkBase < hp2_layout::kThunkSize) {
    uint32_t ord = (target - hp2_layout::kThunkBase) / hp2_layout::kThunkStride;
    if (g_kernel[ord].fn && (target & (hp2_layout::kThunkStride - 1)) == 0) {
      KernelCall(c, ord);
      return;
    }
  }
  if (target - hp2_layout::kHostThunkBase < hp2_layout::kHostThunkSize) {
    uint32_t i = (target - hp2_layout::kHostThunkBase) / 16;
    if (i < g_host_thunks.size()) {
      g_host_thunks[i].fn(c);
      return;
    }
  }
  if (target - g_code_lo < g_code_size) {
    // Fixed format: lift/miss_roots.py collects these into manifest entries.
    hp2_log("hp2: dispatch-miss target=%08X from=%08X", target, va);
    hp2_fatal("indirect branch to unlifted address %08X from %08X (add it to "
              "lift/manifest.txt as an entry: lift/miss_roots.py <log>)", target, va);
  }
  hp2_fatal("indirect branch to %08X from %08X: not code", target, va);
}

void Resume(Ctx& c, GuestResume r) {
  for (;;) {
    try {
      Dispatch(c, uint32_t(r.target), uint32_t(r.from));
      return;
    } catch (const GuestResume& again) {
      if (again.ebp != r.ebp) throw;
      r = again;
    }
  }
}

void Unimplemented(Ctx&, uint32_t va, const char* what) {
  hp2_fatal("unimplemented instruction at %08X: %s", va, what);
}
void BadJump(Ctx&, uint32_t va, uint32_t target) {
  hp2_fatal("branch at %08X to %08X outside lifted code", va, target);
}
void Unreachable(Ctx&, uint32_t va) { hp2_fatal("returned from a no-return call at %08X", va); }
void Trap(Ctx&, uint32_t va) { hp2_fatal("breakpoint/trap instruction at %08X", va); }
void RaiseDivideError(Ctx&, uint32_t va) { hp2_fatal("integer divide error (#DE) at %08X", va); }
void Probe(Ctx& c, uint32_t va, const char* name) {
  hp2_log("probe: %s (%08X) ecx %08X args %08X %08X %08X %08X", name, va, c.ecx,
          hp2_arg(c, 0), hp2_arg(c, 1), hp2_arg(c, 2), hp2_arg(c, 3));
}

// The Xbox CPU: a 733 MHz Pentium III (Coppermine), family 6 model 8.
// MMX and SSE are claimed as on the console; SSE2 is not.
void Cpuid(Ctx& c) {
  switch (c.eax) {
    case 0:
      c.eax = 2;
      c.ebx = 0x756E6547;  // "GenuineIntel"
      c.edx = 0x49656E69;
      c.ecx = 0x6C65746E;
      break;
    case 1:
      c.eax = 0x00000683;
      c.ebx = 0;
      c.ecx = 0;
      c.edx = 0x0383F9FF;  // fpu .. cmov pat pse36 mmx fxsr sse
      break;
    default:
      c.eax = c.ebx = c.ecx = c.edx = 0;
      break;
  }
}

uint64_t Rdtsc(Ctx&) {
  if (hp2_kernel_rdtsc) return hp2_kernel_rdtsc();
  using namespace std::chrono;
  // 733.33 MHz, like the console.
  uint64_t ns = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
  return ns * 11 / 15;
}

// The x64-guest overloads lifted.h declares are never referenced by 32-bit
// lifted code; nothing to define.

}  // namespace recomp
