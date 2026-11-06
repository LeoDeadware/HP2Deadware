// Kernel HLE core: export registry, per-ordinal trampolines, data exports,
// initialisation and logging.

#include <array>
#include <cstdio>
#include <cstdlib>
#include <utility>

#include "hle/kernel/kernel_internal.h"

namespace hp2k {

Hp2KernelHost g_host;
Hp2KernelConfig g_cfg;
bool g_trace = false;

namespace {

constexpr uint32_t kMaxOrdinal = 400;

struct DataDef {
  const char* name = nullptr;
  uint32_t size = 0;
  void (*init)(uint32_t) = nullptr;
  uint32_t va = 0;
};

std::array<Export, kMaxOrdinal>& Exports() {
  static std::array<Export, kMaxOrdinal> e;
  return e;
}
std::array<DataDef, kMaxOrdinal>& Datas() {
  static std::array<DataDef, kMaxOrdinal> d;
  return d;
}

std::mutex g_log_mu;

void VLog(const char* tag, const char* fmt, va_list ap) {
  char line[1024];
  int len = std::snprintf(line, sizeof line, "[krnl]%s ", tag);
  std::vsnprintf(line + len, sizeof line - len, fmt, ap);
  size_t n = std::strlen(line);
  if (n && line[n - 1] == '\n') line[--n] = 0;
  {
    std::lock_guard<std::mutex> l(g_log_mu);
    std::fprintf(stderr, "%s\n", line);
  }
  CrashNote(line);
}

template <uint32_t Ord>
void Trampoline(Ctx& c) {
  const Export& e = Exports()[Ord];
  uint32_t ret_addr = Rd32(c.esp);
  if (g_trace) {
    char args[96] = "";
    int at = 0;
    for (int i = 0; i < e.nargs && i < 6; i++)
      at += std::snprintf(args + at, sizeof args - at, i ? ",%X" : "%X", Arg(c, i));
    Log("%s(%s) from %08X", e.name, args, ret_addr);
  }
  uint64_t r = e.fn(c);
  c.eax = uint32_t(r);
  if (e.ret64) c.edx = uint32_t(r >> 32);
  c.esp += 4 + (e.nargs > 0 ? 4 * uint32_t(e.nargs) : 0);
  if (g_trace) Log("  %s -> %llX", e.name, (unsigned long long)r);
}

template <size_t... I>
constexpr std::array<HostFn, sizeof...(I)> MakeTrampolines(std::index_sequence<I...>) {
  return {&Trampoline<uint32_t(I)>...};
}
const std::array<HostFn, kMaxOrdinal> kTrampolines =
    MakeTrampolines(std::make_index_sequence<kMaxOrdinal>());

}  // namespace

Registrar::Registrar(uint32_t ord, const char* name, int nargs, Impl fn, bool ret64) {
  if (ord >= kMaxOrdinal || Exports()[ord].fn) std::abort();
  Exports()[ord] = Export{name, fn, nargs, ret64};
}

DataRegistrar::DataRegistrar(uint32_t ord, const char* name, uint32_t size,
                             void (*init)(uint32_t)) {
  if (ord >= kMaxOrdinal || Datas()[ord].name) std::abort();
  Datas()[ord] = DataDef{name, size, init, 0};
}

const Export* FindExport(uint32_t ord) {
  return ord < kMaxOrdinal && Exports()[ord].fn ? &Exports()[ord] : nullptr;
}

void Log(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  VLog("", fmt, ap);
  va_end(ap);
}
void Warn(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  VLog(" warning:", fmt, ap);
  va_end(ap);
}
void Fatal(const char* fmt, ...) {
  char why[512];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(why, sizeof why, fmt, ap);
  va_end(ap);
  Log("FATAL: %s", why);
  hp2_crash_report(why);
  std::abort();
}

std::string GStr(uint32_t va) {
  std::string s;
  if (!va) return s;
  for (const char* p = reinterpret_cast<const char*>(GP(va)); *p; p++) s += *p;
  return s;
}

std::string AnsiString(uint32_t ps) {
  if (!ps) return {};
  uint16_t len = Rd16(ps);
  uint32_t buf = Rd32(ps + 4);
  if (!buf) return {};
  return std::string(reinterpret_cast<const char*>(GP(buf)), len);
}

}  // namespace hp2k

using namespace hp2k;

namespace {
bool g_inited = false;
}

bool hp2_kernel_init(const Hp2KernelHost& host, const Hp2KernelConfig& cfg) {
  g_inited = true;
  g_host = host;
  g_cfg = cfg;
  if (const char* t = std::getenv("HP2_KTRACE_KERNEL")) g_trace = *t && *t != '0';
  if (!g_host.guest_base || !g_host.commit) {
    Warn("hp2_kernel_init: guest_base/commit missing");
    return false;
  }
  MemoryInit();
  // Data exports: one page block, each variable 16-byte aligned.
  if (!g_host.commit(kKernelDataVa, 0x10000, true)) return false;
  uint32_t at = kKernelDataVa;
  for (uint32_t o = 0; o < kMaxOrdinal; o++) {
    DataDef& d = Datas()[o];
    if (!d.name) continue;
    d.va = at;
    at += (d.size + 15) & ~15u;
    if (at > kKernelDataVa + 0x10000) Fatal("kernel data page overflow");
  }
  TimeInit();
  SyncInit();
  FileInit();
  LanguagePickerInit();
  for (uint32_t o = 0; o < kMaxOrdinal; o++)
    if (Datas()[o].name && Datas()[o].init) Datas()[o].init(Datas()[o].va);
  VblankInit(cfg.vblank_hz);
  return true;
}

// With the Lifter's runtime linked in (runtime_glue.cpp), the kernel
// initialises itself on the loader's first query (also thread setup).
void hp2k::AutoInit() {
  if (g_inited) return;
  g_inited = true;
  if (!g_host.guest_base) {
    Hp2KernelHost h;
    Hp2KernelConfig cfg;
    if (hp2k_runtime_glue(&h, &cfg)) hp2_kernel_init(h, cfg);
  }
}

HostFn hp2_kernel_lookup(uint32_t ordinal) {
  AutoInit();
  return FindExport(ordinal) ? kTrampolines[ordinal] : nullptr;
}

uint32_t hp2_kernel_data_va(uint32_t ordinal) {
  AutoInit();
  return ordinal < 400 ? Datas()[ordinal].va : 0;
}

// Weak: the runtime (hp2_runtime.h) declares the same function.
__attribute__((weak)) const char* hp2_kernel_name(uint32_t ordinal) {
  if (const Export* e = FindExport(ordinal)) return e->name;
  if (ordinal < 400 && Datas()[ordinal].name) return Datas()[ordinal].name;
  return "?";
}

uint32_t hp2_kernel_thunk_va(uint32_t ordinal) { return 0xFE000000u + ordinal * 16; }
