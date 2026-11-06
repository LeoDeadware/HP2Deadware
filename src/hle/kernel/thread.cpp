// Threads: Xbox KPCR/KTHREAD in guest memory (one KPCR per host thread, at
// fs_base), PsCreateSystemThreadEx on host threads, guest calls.
//
// KPCR (fs:): NtTib {ExceptionList @0, StackBase @4, StackLimit @8, ...,
// Self @0x18}, SelfPcr @0x1C, Prcb @0x20 (-> PrcbData), Irql @0x24 (u8),
// PrcbData @0x28 {CurrentThread @0x28, NextThread @0x2C, IdleThread @0x30,
// ..., DpcRoutineActive @0x28+0x250}.
// KTHREAD: Header @0 (type 6), TlsData @0x28; ETHREAD.UniqueThread @0x12C.
// TLS: TlsData = StackBase - TlsDataSize exactly; XAPI's TLS index is
// TlsDataSize / -4 relative to fs:[4] (NtTib.StackBase), and its thread
// startup (0x111D6D) stores [TlsData] = TlsData + 4 and copies the template.

#include <atomic>
#include <condition_variable>
#include <thread>
#include <pthread.h>

#include "hle/kernel/kernel_internal.h"

namespace hp2k {

namespace {

constexpr uint32_t kKpcrSize = 0x400, kKThreadSize = 0x200;
constexpr uint32_t kKThreadTlsData = 0x28, kEThreadUniqueThread = 0x12C;

thread_local uint32_t t_kpcr = 0;
thread_local bool t_created_by_kernel = false;
std::atomic<uint32_t> g_next_tid{0x10};

struct Prepared {
  uint32_t kpcr, kthread, esp;
};

Prepared Prepare(uint32_t stack_size, uint32_t tls_size) {
  if (stack_size < 0x4000) stack_size = 0x10000;
  stack_size = (stack_size + 0xFFFF) & ~0xFFFFu;
  // A spare page above StackBase: lifted code may read slightly past the top.
  uint32_t base = UserAlloc(stack_size + 0x1000, 0x10000);
  if (!base) Fatal("out of guest memory for a %u KB thread stack", stack_size >> 10);
  uint32_t top = base + stack_size;
  uint32_t tls = tls_size ? top - tls_size : 0;
  uint32_t kpcr = PoolAlloc(kKpcrSize), kthread = PoolAlloc(kKThreadSize);
  if (!kpcr || !kthread) Fatal("out of kernel pool for a thread");
  InitDispatcherHeader(kthread, kThreadObj, kKThreadSize / 4 > 255 ? 255 : kKThreadSize / 4, 0);
  Wr32(kthread + kKThreadTlsData, tls);
  Wr32(kthread + kEThreadUniqueThread, g_next_tid.fetch_add(4));
  Wr32(kpcr + 0x00, 0xFFFFFFFF);  // ExceptionList: end of chain
  Wr32(kpcr + 0x04, top);
  Wr32(kpcr + 0x08, base);
  Wr32(kpcr + 0x18, kpcr);
  Wr32(kpcr + 0x1C, kpcr);
  Wr32(kpcr + 0x20, kpcr + 0x28);
  Wr8(kpcr + 0x24, 0);
  Wr32(kpcr + 0x28, kthread);
  uint32_t esp = ((tls ? tls : top) - 0x10) & ~0xFu;
  return {kpcr, kthread, esp};
}

void Apply(Ctx& c, const Prepared& p) {
  c.esp = p.esp;
  c.ebp = 0;
  c.fs_base = p.kpcr;
  t_kpcr = p.kpcr;
}

struct ThreadObject : KObject {
  const char* Kind() const override { return "thread"; }
  std::mutex mu;
  std::condition_variable cv;
  bool suspended = false;
};

void MarkFinished(uint32_t kthread) {
  std::lock_guard<std::mutex> l(DispatcherLock());
  Wr32(kthread + 4, 1);
  SignalChanged();
  if (t_counted) {
    t_counted = false;
    RunningAdd(-1);
  }
}

}  // namespace

uint32_t CurrentKpcr() {
  if (t_kpcr) return t_kpcr;
  if (g_host.current_ctx)
    if (Ctx* c = g_host.current_ctx()) return c->fs_base;
  return 0;
}

uint32_t CurrentKThread() {
  uint32_t pcr = CurrentKpcr();
  return pcr ? Rd32(pcr + 0x28) : 0;
}

Ctx* CurrentCtx() { return g_host.current_ctx ? g_host.current_ctx() : nullptr; }

uint32_t CallGuest(Ctx& c, uint32_t va, std::initializer_list<uint32_t> args) {
  uint32_t a[16];
  int n = 0;
  for (uint32_t v : args) a[n++] = v;
  return g_host.call_guest(c, va, a, n);
}

void RunServiceThread(const char* name, std::function<void(Ctx&)> body) {
  Prepared p = Prepare(0x10000, 0);
  RunningAdd(+1);  // test clock: counted from now (see RunningAdd)
  g_host.start_thread([p, body, name](Ctx& c) {
    Apply(c, p);
    t_created_by_kernel = true;
    SetCounted();
    pthread_setname_np(name);
    body(c);
  });
}

// PsCreateSystemThreadEx(PHANDLE ThreadHandle, ThreadExtensionSize,
//   KernelStackSize, TlsDataSize, PHANDLE ThreadId, StartRoutine,
//   StartContext, CreateSuspended, DebuggerThread, SystemRoutine)
// The thread runs SystemRoutine(StartRoutine, StartContext) (XAPI passes
// its thread startup) or else StartRoutine(StartContext).
KEXPORT(255, PsCreateSystemThreadEx, 10) {
  uint32_t ph = Arg(c, 0), stack = Arg(c, 2), tls = Arg(c, 3), ptid = Arg(c, 4),
           start = Arg(c, 5), ctx = Arg(c, 6), sys = Arg(c, 9);
  bool suspended = Arg(c, 7) & 0xFF;
  Prepared p = Prepare(stack, tls);
  auto to = std::make_shared<ThreadObject>();
  to->disp_va = p.kthread;
  to->suspended = suspended;
  uint32_t h = NewHandle(to);
  if (ph) Wr32(ph, h);
  if (ptid) Wr32(ptid, Rd32(p.kthread + kEThreadUniqueThread));
  if (!suspended) RunningAdd(+1);  // test clock: counted from now; NtResumeThread otherwise
  g_host.start_thread([p, to, start, ctx, sys](Ctx& tc) {
    Apply(tc, p);
    t_created_by_kernel = true;
    {
      std::unique_lock<std::mutex> l(to->mu);
      to->cv.wait(l, [&] { return !to->suspended; });
    }
    SetCounted();
    try {
      if (sys) CallGuest(tc, sys, {start, ctx});
      else CallGuest(tc, start, {ctx});
    } catch (const ThreadExit&) {
    }
    MarkFinished(p.kthread);
  });
  return kSuccess;
}

// PsTerminateSystemThread(ExitStatus): never returns.
KEXPORT(258, PsTerminateSystemThread, 1) {
  if (t_created_by_kernel) throw ThreadExit{Arg(c, 0)};
  // The loader's entry thread: signal it and park this host thread.
  if (uint32_t kt = CurrentKThread()) MarkFinished(kt);
  for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
}

// NtResumeThread(Handle, PULONG PreviousSuspendCount)
KEXPORT(224, NtResumeThread, 2) {
  auto to = std::dynamic_pointer_cast<ThreadObject>(GetHandle(Arg(c, 0)));
  if (!to) return kInvalidHandle;
  std::lock_guard<std::mutex> l(to->mu);
  if (Arg(c, 1)) Wr32(Arg(c, 1), to->suspended ? 1 : 0);
  if (to->suspended) RunningAdd(+1);
  to->suspended = false;
  to->cv.notify_all();
  return kSuccess;
}

// LONG KeSetBasePriorityThread(PKTHREAD, LONG Increment): host threads keep
// their priority; returns the previous increment.
KEXPORT(143, KeSetBasePriorityThread, 2) {
  uint32_t kt = Arg(c, 0);
  uint32_t old = Rd32(kt + 0x1F0);
  Wr32(kt + 0x1F0, Arg(c, 1));
  return old;
}

}  // namespace hp2k

using namespace hp2k;

void hp2_kernel_thread_setup(recomp::Ctx& c, uint32_t stack_size, uint32_t tls_size) {
  AutoInit();
  Prepared p = Prepare(stack_size, tls_size);
  Apply(c, p);
  if (!t_counted) {  // the loader's entry thread
    SetCounted();
    RunningAdd(+1);
  }
}

uint32_t hp2_kernel_tls_size(uint32_t dir) {
  if (!dir) return 0;
  uint32_t raw_start = Rd32(dir), raw_end = Rd32(dir + 4), zero_fill = Rd32(dir + 0x10);
  return ((raw_end - raw_start + 0xF + zero_fill) & ~0xFu) + 4;
}
