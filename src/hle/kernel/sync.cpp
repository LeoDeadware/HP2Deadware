// Synchronisation: dispatcher objects in guest memory (events, timers,
// threads), waits, DPCs, timers, IRQL, critical sections, handles.
//
// All dispatcher state (DISPATCHER_HEADER.SignalState) lives in guest
// memory, exactly where lifted code expects it; one host mutex plays the
// role of the dispatcher lock and one condition variable wakes waiters.

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <thread>
#include <unordered_map>
#include <vector>

#include "hle/kernel/kernel_internal.h"

namespace hp2k {

namespace {

std::mutex g_disp;
std::condition_variable g_disp_cv;
// NtPulseEvent: generation per event, so waiters already blocked see it.
std::unordered_map<uint32_t, uint64_t> g_pulse;

// ---- handles ----
std::mutex g_handle_mu;
std::unordered_map<uint32_t, std::shared_ptr<KObject>> g_handles;
uint32_t g_next_handle = 0x1000;

struct EventObject : KObject {
  const char* Kind() const override { return "event"; }
};

}  // namespace

// Test clock: blocked waiters, so a signal or a vblank can count the threads
// it wakes as running at once (see RunningAdd).
struct Waiter {
  const uint32_t* objs;
  uint32_t n;
  bool wait_all;
  const std::vector<uint64_t>* gens;
  uint64_t deadline;  // interrupt time; UINT64_MAX = none
  bool woken;
};
std::vector<Waiter*> g_waiters;
namespace {
bool Signalled(uint32_t obj, uint64_t pulse_gen);
}

// Under g_disp: marks the waiters that can now continue as running.
void WakeCheckLocked() {
  if (!TestClock() || g_waiters.empty()) return;
  uint64_t now = InterruptTime100ns();
  for (Waiter* w : g_waiters) {
    if (w->woken) continue;
    bool go = now >= w->deadline;
    if (!go && w->n) {
      go = w->wait_all;
      for (uint32_t i = 0; i < w->n; i++) {
        bool s = !w->objs[i] || Signalled(w->objs[i], (*w->gens)[i]);
        if (w->wait_all && !s) go = false;
        if (!w->wait_all && s) go = true;
      }
    }
    if (go) {
      w->woken = true;
      RunningAdd(+1);
    }
  }
}

std::mutex& DispatcherLock() { return g_disp; }
void SignalChanged() {
  WakeCheckLocked();
  g_disp_cv.notify_all();
}

void WakeOnTimeAdvance() {
  std::lock_guard<std::mutex> l(g_disp);
  WakeCheckLocked();
  g_disp_cv.notify_all();
}

uint32_t NewHandle(std::shared_ptr<KObject> o) {
  std::lock_guard<std::mutex> l(g_handle_mu);
  uint32_t h = g_next_handle;
  g_next_handle += 4;
  g_handles[h] = std::move(o);
  return h;
}

std::shared_ptr<KObject> GetHandle(uint32_t h) {
  std::lock_guard<std::mutex> l(g_handle_mu);
  auto it = g_handles.find(h);
  return it == g_handles.end() ? nullptr : it->second;
}

bool CloseHandle(uint32_t h) {
  std::lock_guard<std::mutex> l(g_handle_mu);
  return g_handles.erase(h) != 0;
}

void InitDispatcherHeader(uint32_t va, uint8_t type, uint8_t size_dw, int32_t signal) {
  Wr8(va + 0, type);
  Wr8(va + 1, 0);
  Wr8(va + 2, size_dw);
  Wr8(va + 3, 0);
  Wr32(va + 4, uint32_t(signal));
  Wr32(va + 8, va + 8);  // WaitListHead: empty list
  Wr32(va + 12, va + 8);
}

namespace {

bool Signalled(uint32_t obj, uint64_t pulse_gen) {
  if (int32_t(Rd32(obj + 4)) > 0) return true;
  auto it = g_pulse.find(obj);
  return it != g_pulse.end() && it->second != pulse_gen;
}

void Satisfy(uint32_t obj) {
  switch (Rd8(obj)) {
    case kEventSynchronization:
    case kTimerSynchronization:
      Wr32(obj + 4, 0);
      break;
    case kSemaphore:
      Wr32(obj + 4, Rd32(obj + 4) - 1);
      break;
    default:
      break;
  }
}

}  // namespace

uint32_t WaitObjects(const uint32_t* objs, uint32_t n, bool wait_all, const int64_t* timeout) {
  bool infinite = timeout == nullptr;
  uint64_t deadline = UINT64_MAX;  // interrupt time, 100 ns
  if (!infinite) {
    int64_t t = *timeout;
    int64_t rel = t < 0 ? -t : t - int64_t(SystemTime100ns());
    if (rel < 0) rel = 0;
    deadline = InterruptTime100ns() + uint64_t(rel);
  }
  std::unique_lock<std::mutex> l(g_disp);
  std::vector<uint64_t> gens(n);
  for (uint32_t i = 0; i < n; i++) {
    auto it = g_pulse.find(objs[i]);
    gens[i] = it == g_pulse.end() ? 0 : it->second;
  }
  // Test clock: registered while blocked (see WakeCheckLocked).
  Waiter w{objs, n, wait_all, &gens, deadline, false};
  bool blocked = false;
  auto done = [&](uint32_t status) {
    if (blocked) {
      g_waiters.erase(std::find(g_waiters.begin(), g_waiters.end(), &w));
      if (!w.woken) RunningAdd(+1);
    }
    return status;
  };
  for (;;) {
    if (wait_all) {
      bool all = n != 0;
      for (uint32_t i = 0; i < n && all; i++) all = !objs[i] || Signalled(objs[i], gens[i]);
      if (all) {
        for (uint32_t i = 0; i < n; i++)
          if (objs[i]) Satisfy(objs[i]);
        return done(kSuccess);
      }
    } else {
      for (uint32_t i = 0; i < n; i++)
        if (!objs[i] || Signalled(objs[i], gens[i])) {
          if (objs[i]) Satisfy(objs[i]);
          return done(kSuccess + i);
        }
    }
    uint64_t now = InterruptTime100ns();
    if (now >= deadline) return done(kTimeout);
    if (TestClock() && t_counted) {
      if (!blocked) {
        blocked = true;
        g_waiters.push_back(&w);
        RunningAdd(-1);
      } else if (w.woken) {  // woken, but another thread took the object
        w.woken = false;
        RunningAdd(-1);
      }
    }
    if (infinite) {
      g_disp_cv.wait(l);
    } else {
      // Test clock: kernel time moves only at vblanks (WakeOnTimeAdvance
      // notifies); the slice is only a safety net.
      uint64_t slice = TestClock() ? 10000 : deadline - now;
      g_disp_cv.wait_for(l, std::chrono::nanoseconds(slice * 100));
    }
  }
}

int32_t SetEventVa(uint32_t ev) {
  std::lock_guard<std::mutex> l(g_disp);
  int32_t prev = int32_t(Rd32(ev + 4));
  Wr32(ev + 4, 1);
  SignalChanged();
  return prev;
}

namespace {

const int64_t* TimeoutArg(uint32_t p, int64_t* store) {
  if (!p) return nullptr;
  *store = int64_t(Rd64(p));
  return store;
}

// Dispatcher object behind a handle; file handles and other non-waitable
// objects read as signalled (0 in the wait list).
bool HandleToObject(uint32_t h, uint32_t* obj) {
  if (h == kCurrentThreadHandle) {
    *obj = CurrentKThread();
    return true;
  }
  auto o = GetHandle(h);
  if (!o) return false;
  *obj = o->disp_va;
  return true;
}

}  // namespace

// ---- Ke waits -----------------------------------------------------------------------------
// KeWaitForSingleObject(Object, WaitReason, WaitMode, Alertable, PLARGE_INTEGER Timeout)
KEXPORT(159, KeWaitForSingleObject, 5) {
  uint32_t obj = Arg(c, 0);
  int64_t t;
  return WaitObjects(&obj, 1, false, TimeoutArg(Arg(c, 4), &t));
}

// KeWaitForMultipleObjects(Count, Object[], WaitType, WaitReason, WaitMode,
//   Alertable, PLARGE_INTEGER Timeout, PKWAIT_BLOCK WaitBlockArray)
KEXPORT(158, KeWaitForMultipleObjects, 8) {
  uint32_t n = Arg(c, 0), arr = Arg(c, 1);
  std::vector<uint32_t> objs(n);
  for (uint32_t i = 0; i < n; i++) objs[i] = Rd32(arr + 4 * i);
  int64_t t;
  return WaitObjects(objs.data(), n, Arg(c, 2) == 0, TimeoutArg(Arg(c, 6), &t));
}

// ---- Nt waits -------------------------------------------------------------------------------
// NtWaitForSingleObject(Handle, Alertable, PLARGE_INTEGER Timeout)
KEXPORT(233, NtWaitForSingleObject, 3) {
  uint32_t obj;
  if (!HandleToObject(Arg(c, 0), &obj)) return kInvalidHandle;
  int64_t t;
  return WaitObjects(&obj, 1, false, TimeoutArg(Arg(c, 2), &t));
}

// NtWaitForSingleObjectEx(Handle, WaitMode, Alertable, PLARGE_INTEGER Timeout)
KEXPORT(234, NtWaitForSingleObjectEx, 4) {
  uint32_t obj;
  if (!HandleToObject(Arg(c, 0), &obj)) return kInvalidHandle;
  int64_t t;
  return WaitObjects(&obj, 1, false, TimeoutArg(Arg(c, 3), &t));
}

// NtWaitForMultipleObjectsEx(Count, Handles[], WaitType, WaitMode, Alertable, Timeout)
KEXPORT(235, NtWaitForMultipleObjectsEx, 6) {
  uint32_t n = Arg(c, 0), arr = Arg(c, 1);
  std::vector<uint32_t> objs(n);
  for (uint32_t i = 0; i < n; i++)
    if (!HandleToObject(Rd32(arr + 4 * i), &objs[i])) return kInvalidHandle;
  int64_t t;
  return WaitObjects(objs.data(), n, Arg(c, 2) == 0, TimeoutArg(Arg(c, 5), &t));
}

// ---- events -------------------------------------------------------------------------------------
// KeSetEvent(Event, Increment, Wait) -> previous state
KEXPORT(145, KeSetEvent, 3) { return uint32_t(SetEventVa(Arg(c, 0))); }

// NtCreateEvent(PHANDLE, POBJECT_ATTRIBUTES, EVENT_TYPE, BOOLEAN InitialState)
KEXPORT(189, NtCreateEvent, 4) {
  uint32_t oa = Arg(c, 1);
  if (oa && Rd32(oa + 4)) Warn("NtCreateEvent: named event '%s' (names ignored)", AnsiString(Rd32(oa + 4)).c_str());
  uint32_t ev = PoolAlloc(0x10);
  if (!ev) return kInsufficientResources;
  InitDispatcherHeader(ev, Arg(c, 2) ? kEventSynchronization : kEventNotification, 4,
                       (Arg(c, 3) & 0xFF) ? 1 : 0);
  auto o = std::make_shared<EventObject>();
  o->disp_va = ev;
  Wr32(Arg(c, 0), NewHandle(o));
  return kSuccess;
}

namespace {
uint32_t EventOf(uint32_t h) {
  auto o = std::dynamic_pointer_cast<EventObject>(GetHandle(h));
  return o ? o->disp_va : 0;
}
}  // namespace

// NtSetEvent(Handle, PLONG PreviousState)
KEXPORT(225, NtSetEvent, 2) {
  uint32_t ev = EventOf(Arg(c, 0));
  if (!ev) return kInvalidHandle;
  int32_t prev = SetEventVa(ev);
  if (Arg(c, 1)) Wr32(Arg(c, 1), uint32_t(prev));
  return kSuccess;
}

KEXPORT(186, NtClearEvent, 1) {
  uint32_t ev = EventOf(Arg(c, 0));
  if (!ev) return kInvalidHandle;
  std::lock_guard<std::mutex> l(g_disp);
  Wr32(ev + 4, 0);
  return kSuccess;
}

// NtPulseEvent(Handle, PLONG PreviousState): wakes current waiters, leaves it reset.
KEXPORT(205, NtPulseEvent, 2) {
  uint32_t ev = EventOf(Arg(c, 0));
  if (!ev) return kInvalidHandle;
  std::lock_guard<std::mutex> l(g_disp);
  if (Arg(c, 1)) Wr32(Arg(c, 1), Rd32(ev + 4));
  g_pulse[ev]++;
  WakeCheckLocked();  // before the reset: the pulse is seen through the generation
  Wr32(ev + 4, 0);
  g_disp_cv.notify_all();
  return kSuccess;
}

// ---- objects -------------------------------------------------------------------------------------
KEXPORT(187, NtClose, 1) { return CloseHandle(Arg(c, 0)) ? kSuccess : kInvalidHandle; }

KDATA(16, ExEventObjectType, 0x20, [](uint32_t va) { Wr32(va, 'vevE'); });
KDATA(259, PsThreadObjectType, 0x20, [](uint32_t va) { Wr32(va, 'erhT'); });

// ObReferenceObjectByHandle(Handle, POBJECT_TYPE, PVOID* Object)
KEXPORT(246, ObReferenceObjectByHandle, 3) {
  uint32_t obj;
  if (!HandleToObject(Arg(c, 0), &obj)) return kInvalidHandle;
  if (!obj) {
    if (auto o = GetHandle(Arg(c, 0))) obj = PoolAlloc(0x10);  // non-dispatcher object
  }
  Wr32(Arg(c, 2), obj);
  return kSuccess;
}

// VOID FASTCALL ObfDereferenceObject(PVOID Object)
KEXPORT(250, ObfDereferenceObject, 0) { return 0; }

// ---- IRQL ------------------------------------------------------------------------------------------
// The Xbox is a uniprocessor: raising to DISPATCH_LEVEL excludes DPCs and
// other threads. Here a recursive mutex does that job.
namespace {
std::recursive_mutex g_dpc_level;
thread_local uint8_t t_irql = 0;

void SetIrql(uint8_t v) {
  t_irql = v;
  if (uint32_t pcr = CurrentKpcr()) Wr8(pcr + 0x24, v);
}

uint8_t RaiseTo(uint8_t n) {
  uint8_t old = t_irql;
  if (old < 2 && n >= 2) g_dpc_level.lock();
  SetIrql(n);
  return old;
}

void LowerTo(uint8_t n) {
  if (t_irql >= 2 && n < 2) g_dpc_level.unlock();
  SetIrql(n);
}
}  // namespace

KEXPORT(129, KeRaiseIrqlToDpcLevel, 0) { return RaiseTo(2); }
// KIRQL FASTCALL KfRaiseIrql(KIRQL NewIrql /* cl */)
KEXPORT(160, KfRaiseIrql, 0) { return RaiseTo(uint8_t(c.ecx)); }
// VOID FASTCALL KfLowerIrql(KIRQL NewIrql /* cl */)
KEXPORT(161, KfLowerIrql, 0) {
  LowerTo(uint8_t(c.ecx));
  return 0;
}

// ---- DPCs --------------------------------------------------------------------------------------------
// KDPC: Type(u16) Inserted(u8) pad, DpcListEntry(8) @4, DeferredRoutine @0xC,
// DeferredContext @0x10, SystemArgument1 @0x14, SystemArgument2 @0x18.
namespace {
std::mutex g_dpc_mu;
std::condition_variable g_dpc_cv;
std::deque<uint32_t> g_dpc_queue;
bool g_dpc_thread_started = false;
bool g_dpc_idle = false;  // test clock: the DPC thread is blocked on the queue

void DpcThread(Ctx& c) {
  for (;;) {
    uint32_t dpc;
    {
      std::unique_lock<std::mutex> l(g_dpc_mu);
      if (g_dpc_queue.empty()) {
        g_dpc_idle = true;
        RunningAdd(-1);
        g_dpc_cv.wait(l, [] { return !g_dpc_queue.empty(); });
        // QueueDpc counted this thread as running when it queued the DPC.
      }
      dpc = g_dpc_queue.front();
      g_dpc_queue.pop_front();
      Wr8(dpc + 2, 0);
    }
    uint32_t routine = Rd32(dpc + 0xC);
    std::lock_guard<std::recursive_mutex> dl(g_dpc_level);
    SetIrql(2);
    if (uint32_t pcr = CurrentKpcr()) Wr32(pcr + 0x28 + 0x250, 1);  // DpcRoutineActive
    CallGuest(c, routine, {dpc, Rd32(dpc + 0x10), Rd32(dpc + 0x14), Rd32(dpc + 0x18)});
    if (uint32_t pcr = CurrentKpcr()) Wr32(pcr + 0x28 + 0x250, 0);
    SetIrql(0);
  }
}
}  // namespace

void QueueDpc(uint32_t dpc, uint32_t a1, uint32_t a2) {
  std::lock_guard<std::mutex> l(g_dpc_mu);
  if (!g_dpc_thread_started) {
    g_dpc_thread_started = true;
    RunServiceThread("dpc", DpcThread);
  }
  if (Rd8(dpc + 2)) return;
  Wr8(dpc + 2, 1);
  Wr32(dpc + 0x14, a1);
  Wr32(dpc + 0x18, a2);
  g_dpc_queue.push_back(dpc);
  if (g_dpc_idle) {
    g_dpc_idle = false;
    RunningAdd(+1);
  }
  g_dpc_cv.notify_one();
}

// KeInitializeDpc(PKDPC, DeferredRoutine, DeferredContext)
KEXPORT(107, KeInitializeDpc, 3) {
  uint32_t d = Arg(c, 0);
  std::memset(GP(d), 0, 0x1C);
  Wr16(d, 19);  // DpcObject
  Wr32(d + 0xC, Arg(c, 1));
  Wr32(d + 0x10, Arg(c, 2));
  return 0;
}

// BOOLEAN KeInsertQueueDpc(PKDPC, SystemArgument1, SystemArgument2)
KEXPORT(119, KeInsertQueueDpc, 3) {
  uint32_t d = Arg(c, 0);
  {
    std::lock_guard<std::mutex> l(g_dpc_mu);
    if (Rd8(d + 2)) return 0;
  }
  QueueDpc(d, Arg(c, 1), Arg(c, 2));
  return 1;
}

// BOOLEAN KeRemoveQueueDpc(PKDPC)
KEXPORT(137, KeRemoveQueueDpc, 1) {
  uint32_t d = Arg(c, 0);
  std::lock_guard<std::mutex> l(g_dpc_mu);
  if (!Rd8(d + 2)) return 0;
  for (auto it = g_dpc_queue.begin(); it != g_dpc_queue.end(); ++it)
    if (*it == d) {
      g_dpc_queue.erase(it);
      break;
    }
  Wr8(d + 2, 0);
  return 1;
}

// ---- timers --------------------------------------------------------------------------------------------
// KTIMER: Header(0x10) DueTime(8) @0x10, TimerListEntry(8) @0x18, Dpc @0x20, Period @0x24.
namespace {
struct TimerState {
  uint64_t due = 0;      // interrupt time, 100 ns
  uint32_t period = 0;   // ms
  uint32_t dpc = 0;
  uint64_t gen = 0;
};
std::mutex g_timer_mu;
std::condition_variable g_timer_cv;
std::unordered_map<uint32_t, TimerState> g_timers;  // armed timers
uint64_t g_timer_gen = 0;
bool g_timer_thread = false;

bool g_timer_idle = false;  // test clock: blocked with no timer armed

void TimerThread() {
  SetCounted();  // counted as running by SetTimer, which started it
  std::unique_lock<std::mutex> l(g_timer_mu);
  for (;;) {
    uint64_t now = InterruptTime100ns();
    uint64_t next = UINT64_MAX;
    std::vector<std::pair<uint32_t, TimerState>> fired;
    for (auto it = g_timers.begin(); it != g_timers.end();) {
      if (it->second.due <= now) {
        fired.push_back(*it);
        if (it->second.period) {
          it->second.due += uint64_t(it->second.period) * 10000;
          if (it->second.due <= now) it->second.due = now + uint64_t(it->second.period) * 10000;
          next = std::min(next, it->second.due);
          ++it;
        } else {
          it = g_timers.erase(it);
        }
      } else {
        next = std::min(next, it->second.due);
        ++it;
      }
    }
    if (!fired.empty()) {
      l.unlock();
      for (auto& [t, s] : fired) {
        {
          std::lock_guard<std::mutex> dl(g_disp);
          Wr32(t + 4, 1);
          if (!s.period) Wr8(t + 3, 0);  // Inserted
          SignalChanged();
        }
        if (s.dpc) {
          uint64_t st = SystemTime100ns();
          QueueDpc(s.dpc, uint32_t(st), uint32_t(st >> 32));
        }
      }
      l.lock();
      continue;
    }
    if (next == UINT64_MAX) {
      g_timer_idle = true;
      RunningAdd(-1);
      g_timer_cv.wait(l, [] { return !g_timer_idle; });  // SetTimer counts us again
    }
    else {
      l.unlock();
      // Sleep in short slices so a newly armed earlier timer is not missed
      // for long (it also notifies the cv, but we are not waiting on it here).
      uint64_t wake = std::min(next, InterruptTime100ns() + 10000);
      SleepUntil100ns(wake);
      l.lock();
    }
  }
}

uint32_t SetTimer(uint32_t t, int64_t due, uint32_t period, uint32_t dpc) {
  uint64_t abs_due = due < 0 ? InterruptTime100ns() + uint64_t(-due)
                             : InterruptTime100ns() +
                                   uint64_t(std::max<int64_t>(0, due - int64_t(SystemTime100ns())));
  std::lock_guard<std::mutex> l(g_timer_mu);
  if (!g_timer_thread) {
    g_timer_thread = true;
    RunningAdd(+1);
    std::thread(TimerThread).detach();
  }
  bool was = g_timers.count(t) != 0;
  {
    std::lock_guard<std::mutex> dl(g_disp);
    Wr32(t + 4, 0);
    Wr8(t + 3, 1);
  }
  Wr64(t + 0x10, abs_due);
  Wr32(t + 0x20, dpc);
  Wr32(t + 0x24, period);
  g_timers[t] = TimerState{abs_due, period, dpc, ++g_timer_gen};
  if (g_timer_idle) {
    g_timer_idle = false;
    RunningAdd(+1);
  }
  g_timer_cv.notify_all();
  return was;
}
}  // namespace

// KeInitializeTimerEx(PKTIMER, TIMER_TYPE)
KEXPORT(113, KeInitializeTimerEx, 2) {
  uint32_t t = Arg(c, 0);
  {
    std::lock_guard<std::mutex> l(g_timer_mu);
    g_timers.erase(t);
  }
  InitDispatcherHeader(t, uint8_t(kTimerNotification + (Arg(c, 1) & 1)), 10, 0);
  std::memset(GP(t + 0x10), 0, 0x18);
  return 0;
}

// BOOLEAN KeSetTimer(PKTIMER, LARGE_INTEGER DueTime, PKDPC)
KEXPORT(149, KeSetTimer, 4) {
  int64_t due = int64_t(uint64_t(Arg(c, 1)) | uint64_t(Arg(c, 2)) << 32);
  return SetTimer(Arg(c, 0), due, 0, Arg(c, 3));
}

// BOOLEAN KeSetTimerEx(PKTIMER, LARGE_INTEGER DueTime, LONG Period, PKDPC)
KEXPORT(150, KeSetTimerEx, 5) {
  int64_t due = int64_t(uint64_t(Arg(c, 1)) | uint64_t(Arg(c, 2)) << 32);
  return SetTimer(Arg(c, 0), due, Arg(c, 3), Arg(c, 4));
}

// BOOLEAN KeCancelTimer(PKTIMER)
KEXPORT(97, KeCancelTimer, 1) {
  uint32_t t = Arg(c, 0);
  std::lock_guard<std::mutex> l(g_timer_mu);
  bool was = g_timers.erase(t) != 0;
  Wr8(t + 3, 0);
  return was;
}

// ---- critical sections ---------------------------------------------------------------------------
// RTL_CRITICAL_SECTION: Synchronization (DISPATCHER_HEADER, 0x10),
// LockCount @0x10 (-1 = free), RecursionCount @0x14, OwningThread @0x18.
namespace {
std::mutex g_cs_mu;
std::condition_variable g_cs_cv;
int g_cs_waiters = 0;
// Test clock, per critical section: counted waiters, and running counts handed
// to them by RtlLeaveCriticalSection (never more than there are waiters).
struct CsWait {
  int counted = 0;
  int tokens = 0;
};
std::unordered_map<uint32_t, CsWait> g_cs_wait;
thread_local uint32_t t_fake_owner = 0;
std::atomic<uint32_t> g_fake_owner_next{0xF0000000};

uint32_t OwnerId() {
  if (uint32_t t = CurrentKThread()) return t;
  if (!t_fake_owner) t_fake_owner = g_fake_owner_next++;
  return t_fake_owner;
}
}  // namespace

KEXPORT(291, RtlInitializeCriticalSection, 1) {
  uint32_t cs = Arg(c, 0);
  InitDispatcherHeader(cs, kEventSynchronization, 4, 0);
  Wr32(cs + 0x10, 0xFFFFFFFF);
  Wr32(cs + 0x14, 0);
  Wr32(cs + 0x18, 0);
  return 0;
}

KEXPORT(277, RtlEnterCriticalSection, 1) {
  uint32_t cs = Arg(c, 0), me = OwnerId();
  std::unique_lock<std::mutex> l(g_cs_mu);
  uint32_t owner = Rd32(cs + 0x18);
  if (owner && owner != me) {
    g_cs_waiters++;
    bool counted = TestClock() && t_counted;
    if (counted) {
      g_cs_wait[cs].counted++;
      RunningAdd(-1);
    }
    for (;;) {
      g_cs_cv.wait(l);
      bool mine = false;  // was a running count handed over for this wake-up?
      if (counted && g_cs_wait[cs].tokens > 0) {
        g_cs_wait[cs].tokens--;
        mine = true;
      }
      if (Rd32(cs + 0x18) == 0) {
        if (counted && !mine) RunningAdd(+1);
        break;
      }
      if (mine) RunningAdd(-1);
    }
    if (counted && --g_cs_wait[cs].counted == 0) {
      if (g_cs_wait[cs].tokens) RunningAdd(-g_cs_wait[cs].tokens);  // nobody left to take them
      g_cs_wait.erase(cs);
    }
    g_cs_waiters--;
  }
  Wr32(cs + 0x18, me);
  Wr32(cs + 0x14, Rd32(cs + 0x14) + 1);
  Wr32(cs + 0x10, Rd32(cs + 0x10) + 1);
  return 0;
}

KEXPORT(294, RtlLeaveCriticalSection, 1) {
  uint32_t cs = Arg(c, 0);
  std::lock_guard<std::mutex> l(g_cs_mu);
  if (Rd32(cs + 0x18) != OwnerId()) {
    Warn("RtlLeaveCriticalSection(%08X) by a non-owner", cs);
    return 0;
  }
  uint32_t rec = Rd32(cs + 0x14) - 1;
  Wr32(cs + 0x14, rec);
  Wr32(cs + 0x10, Rd32(cs + 0x10) - 1);
  if (rec == 0) {
    Wr32(cs + 0x18, 0);
    if (g_cs_waiters) {
      if (TestClock()) {  // one counted waiter will take it: count it as running now
        auto it = g_cs_wait.find(cs);
        if (it != g_cs_wait.end() && it->second.tokens < it->second.counted) {
          it->second.tokens++;
          RunningAdd(+1);
        }
      }
      g_cs_cv.notify_all();
    }
  }
  return 0;
}

// ---- interrupts (only the XDK's D3D/DSOUND/USB drivers connect them; those
// are replaced at API level, so the interrupts never fire) -----------------------
// KeInitializeInterrupt(PKINTERRUPT, ServiceRoutine, ServiceContext, Vector,
//   Irql, InterruptMode, ShareVector)
KEXPORT(109, KeInitializeInterrupt, 7) {
  uint32_t k = Arg(c, 0);
  std::memset(GP(k), 0, 0x70);
  Wr32(k + 0x00, Arg(c, 1));
  Wr32(k + 0x04, Arg(c, 2));
  Wr32(k + 0x08, Arg(c, 3));
  Wr8(k + 0x0C, uint8_t(Arg(c, 4)));
  return 0;
}
KEXPORT(98, KeConnectInterrupt, 1) {
  Warn("KeConnectInterrupt(vector %u): the XDK driver using it should be replaced by HLE",
       Rd32(Arg(c, 0) + 8));
  return 1;
}
KEXPORT(100, KeDisconnectInterrupt, 1) { return 1; }
// ULONG HalGetInterruptVector(BusInterruptLevel, PKIRQL Irql)
KEXPORT(44, HalGetInterruptVector, 2) {
  uint32_t level = Arg(c, 0);
  if (Arg(c, 1)) Wr8(Arg(c, 1), uint8_t(26 - level));
  return level + 0x30;
}
// BOOLEAN KeSynchronizeExecution(PKINTERRUPT, SynchronizeRoutine, Context)
KEXPORT(153, KeSynchronizeExecution, 3) {
  std::lock_guard<std::recursive_mutex> l(g_dpc_level);
  return CallGuest(c, Arg(c, 1), {Arg(c, 2)}) & 0xFF;
}

void SyncInit() {}

}  // namespace hp2k
