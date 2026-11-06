// Guest threads: a Ctx per host thread, with the Xbox per-thread state
// (KPCR at fs, KTHREAD, TLS) and a guest stack.
//
// Xbox TLS (from XAPI's code): TlsDataSize = ((raw_end - raw_start + 15 +
// zero_fill) & ~15) + 4, the TLS block sits right below NtTib.StackBase
// (KTHREAD.TlsData = StackBase - TlsDataSize) and XAPI's TLS index is
// negative relative to fs:[4]. XAPI's thread startup fills the block from
// the template itself.

#include <pthread.h>

#include <cstdio>
#include <mutex>

#include "runtime/hp2_internal.h"

// Kernel HLE: lays out stack/KPCR/KTHREAD/TLS exactly as its
// PsCreateSystemThreadEx does (kernel_exports.h). Optional.
void hp2_kernel_thread_setup(recomp::Ctx& c, uint32_t stack_size, uint32_t tls_size)
    __attribute__((weak));

namespace hp2rt {

namespace {
thread_local Thread* t_cur = nullptr;
std::mutex g_mu;
uint32_t g_stack_next = hp2_layout::kStackBase;
uint32_t g_next_id = 1;

constexpr uint32_t kKpcrSize = 0x300;
}  // namespace

uint32_t TlsDataSize() {
  if (!g_xbe.tls_va) return 0;
  uint32_t raw = g_xbe.tls_raw_end - g_xbe.tls_raw_start;
  return ((raw + 15 + g_xbe.tls_zero_fill) & ~15u) + 4;
}

namespace {
void InitCpu(Hp2Ctx& c) {
  recomp::x87::Init(c.fpu);
  recomp::x87::SetControlWord(c.fpu, 0x027F);  // 53-bit precision, as the kernel sets
  c.mxcsr = 0x1F80;
  c.eflags = 0x202;
}

void FillInfo(Thread* t) {
  Hp2ThreadInfo& i = t->info;
  i.kpcr = t->ctx.fs_base;
  i.kthread = hp2_rd32(i.kpcr + 0x28);
  i.tls_data = i.kthread ? hp2_rd32(i.kthread + 0x28) : 0;
  i.stack_hi = hp2_rd32(i.kpcr + 4);
  i.stack_lo = hp2_rd32(i.kpcr + 8);
}
}  // namespace

Thread* NewThread(uint32_t stack_size) {
  auto* t = new Thread;
  InitCpu(t->ctx);
  if (!stack_size) stack_size = g_xbe.stack_commit;
  if (stack_size < 0x10000) stack_size = 0x10000;
  stack_size = (stack_size + 0xFFFF) & ~0xFFFFu;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    t->info.id = g_next_id++;
    if (g_stack_next + uint64_t(stack_size) >
        uint64_t(hp2_layout::kStackBase) + hp2_layout::kStackSize)
      hp2_fatal("guest stack region full");
    t->info.stack_lo = g_stack_next;
    g_stack_next += stack_size + 0x10000;  // and an unmapped guard
  }
  t->info.stack_hi = t->info.stack_lo + stack_size;
  if (!hp2_commit(t->info.stack_lo, stack_size)) hp2_fatal("stack commit failed");

  const uint32_t kpcr = hp2_hle_alloc(kKpcrSize + hp2_kthread_size, 16);
  const uint32_t kthread = kpcr + kKpcrSize;
  const uint32_t tls = t->info.stack_hi - TlsDataSize();
  hp2_wr32(kpcr + 0x00, 0xFFFFFFFF);          // NtTib.ExceptionList: end
  hp2_wr32(kpcr + 0x04, t->info.stack_hi);    // NtTib.StackBase
  hp2_wr32(kpcr + 0x08, t->info.stack_lo);    // NtTib.StackLimit
  hp2_wr32(kpcr + 0x18, kpcr);                // NtTib.Self
  hp2_wr32(kpcr + 0x1C, kpcr);                // SelfPcr
  hp2_wr32(kpcr + 0x20, kpcr + 0x28);         // Prcb
  hp2_wr32(kpcr + 0x28, kthread);             // Prcb.CurrentThread
  hp2_wr32(kthread + 0x28, tls);              // KTHREAD.TlsData
  t->ctx.fs_base = kpcr;
  t->ctx.esp = tls & ~0xFu;
  t->ctx.ebp = 0;
  FillInfo(t);
  return t;
}

// The main thread, laid out by the kernel HLE when it is linked.
Thread* NewMainThread() {
  if (!hp2_kernel_thread_setup) return NewThread(0);
  auto* t = new Thread;
  InitCpu(t->ctx);
  t->info.id = g_next_id++;
  uint32_t stack = g_xbe.stack_commit < 0x10000 ? 0x10000 : g_xbe.stack_commit;
  hp2_kernel_thread_setup(t->ctx, stack, TlsDataSize());
  FillInfo(t);
  return t;
}

void BindThread(Thread* t) { t_cur = t; }

void RunOnBigStack(void (*body)(void*), void* arg, bool wait) {
  pthread_attr_t a;
  pthread_attr_init(&a);
  // Lifted calls nest like the guest's, each with a host frame: be generous
  // (address space only; pages are touched as used).
  pthread_attr_setstacksize(&a, size_t(256) << 20);
  pthread_t th;
  struct Box {
    void (*body)(void*);
    void* arg;
  };
  auto* box = new Box{body, arg};
  int rc = pthread_create(
      &th, &a,
      [](void* p) -> void* {
        auto* b = static_cast<Box*>(p);
        b->body(b->arg);
        delete b;
        return nullptr;
      },
      box);
  pthread_attr_destroy(&a);
  if (rc) hp2_fatal("pthread_create failed (%d)", rc);
  if (wait)
    pthread_join(th, nullptr);
  else
    pthread_detach(th);
}

}  // namespace hp2rt

using namespace hp2rt;

Hp2Ctx* hp2_current_ctx() { return t_cur ? &t_cur->ctx : nullptr; }
const Hp2ThreadInfo* hp2_current_thread() { return t_cur ? &t_cur->info : nullptr; }

uint32_t hp2_spawn_thread(uint32_t stack_size, Hp2ThreadBody body, void* user) {
  Thread* t = NewThread(stack_size);
  struct Start {
    Thread* t;
    Hp2ThreadBody body;
    void* user;
  };
  auto* s = new Start{t, body, user};
  const uint32_t id = t->info.id;
  RunOnBigStack(
      [](void* p) {
        auto* s = static_cast<Start*>(p);
        BindThread(s->t);
        char name[32];
        std::snprintf(name, sizeof name, "guest-%u", s->t->info.id);
        pthread_setname_np(name);
        s->body(s->t->ctx, s->user);
        delete s;
      },
      s, false);
  return id;
}
