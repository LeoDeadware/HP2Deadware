// Time: KeTickCount (1 ms), system/interrupt time, delays, the 733 MHz TSC
// and the 60 Hz vblank pacer.
//
// HP2_TEST_CLOCK=frames (opt-in experiment, tests/scripts/test_clock.sh; with
// Audio's mixer per vblank): no host timer drives the vblank; each
// hp2_vblank_wait (D3D Swap) advances it by one, at once. Kernel time
// (KeTickCount, system/interrupt time, waits, sleeps, timers) = vblank * 1/60 s
// exactly: it moves only at vblanks, so every timed wait in the game (the sound
// thread's 10 ms tick, stream I/O) completes at the same frame in every run.
// rdtsc (the sound code times itself with it, 0x12E780) adds 1 us per read on
// the reading thread since the vblank (capped below one frame): a spin loop on
// it still ends, and the values depend only on the thread's own reads.
// Swap barrier: hp2_vblank_wait first waits until every other counted thread
// is blocked in the kernel (RunningAdd), advances the vblank (waking the waits
// whose deadline passed, counted at once), then waits for quiescence again, so
// what the woken threads do lands in the same frame in every run. A thread that
// stays busy for 250 ms of host time ends the wait (logged).
// Boot (before the first Swap): a vblank is added whenever every counted guest
// thread is blocked (the same condition; boot waits on time for ~50 vblanks),
// so their number depends only on what the game waits for, not on host speed
// (the sound thread reads KeTickCount). 2 s without quiescence, or after the
// first Swap 2 s without a Swap, adds one anyway (a hang guard, logged: the run
// is only deterministic up to there). System time starts at a fixed date.

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <thread>
#include <vector>

#include "hle/kernel/kernel_internal.h"

namespace hp2k {

namespace {

using Clock = std::chrono::steady_clock;
Clock::time_point g_t0 = Clock::now();
// FILETIME of g_t0 (100 ns since 1601-01-01).
uint64_t g_systime0 = 0;
constexpr uint64_t kUnixToFileTime = 116444736000000000ull;
constexpr double kTscHz = 733333333.0;

std::atomic<uint32_t> g_tick_va{0};

bool TestClockEnv() {
  const char* v = std::getenv("HP2_TEST_CLOCK");
  return v && !std::strcmp(v, "frames");
}
const bool g_test_clock = TestClockEnv();
constexpr uint64_t kFrame100ns = 166667;  // 1/60 s
std::atomic<uint64_t> g_vb_count{0};
std::atomic<int64_t> g_vb_host_ns{0};
std::atomic<bool> g_vb_swapped_flag{false};  // first Swap seen (traces)      // host time of the latest vblank (test clock)

int64_t HostNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - g_t0).count();
}

uint64_t Now100ns() {
  if (!g_test_clock) return uint64_t(HostNs() / 100);
  return g_vb_count.load(std::memory_order_acquire) * kFrame100ns;
}

// rdtsc time: test clock adds 1 us per read on this thread since the vblank
// (capped below one frame), so a spin on the TSC ends without a Swap and the
// values do not depend on host speed.
uint64_t TscNow100ns() {
  if (!g_test_clock) return Now100ns();
  thread_local uint64_t t_vblank = ~0ull, t_reads = 0;
  uint64_t n = g_vb_count.load(std::memory_order_acquire);
  if (n != t_vblank) {
    t_vblank = n;
    t_reads = 0;
  }
  uint64_t in = std::min<uint64_t>(++t_reads * 10, kFrame100ns - 1);
  return n * kFrame100ns + in;
}

void TickThread() {
  auto next = Clock::now();
  for (;;) {
    next += std::chrono::milliseconds(1);
    std::this_thread::sleep_until(next);
    if (uint32_t va = g_tick_va.load(std::memory_order_relaxed))
      __atomic_store_n(reinterpret_cast<uint32_t*>(GP(va)), uint32_t(InterruptTime100ns() / 10000),
                       __ATOMIC_RELEASE);
  }
}

}  // namespace

uint64_t InterruptTime100ns() { return Now100ns(); }
uint64_t SystemTime100ns() { return g_systime0 + Now100ns(); }

void SleepUntil100ns(uint64_t t) {
  if (!g_test_clock) {
    std::this_thread::sleep_until(g_t0 + std::chrono::nanoseconds(t * 100));
    return;
  }
  // Frame time only moves at vblanks: a registered timed wait (sync.cpp).
  int64_t abs = -int64_t(t - std::min(t, Now100ns()));
  WaitObjects(nullptr, 0, false, &abs);
}

bool TestClock() { return g_test_clock; }

thread_local bool t_counted = false;

namespace {
// Ends the count of a counted thread that exits without MarkFinished (the
// loader's entry thread returns from the XBE entry).
struct CountGuard {
  ~CountGuard() {
    if (t_counted) {
      t_counted = false;
      RunningAdd(-1);
    }
  }
};
thread_local CountGuard t_count_guard;
}  // namespace

void SetCounted() {
  (void)&t_count_guard;  // constructed (and destroyed at thread exit) once used
  t_counted = true;
}

namespace {
std::mutex g_run_mu;
std::condition_variable g_run_cv;
int g_running = 0;
bool g_any_counted = false;  // a guest thread was counted (the game started)
uint64_t g_barrier_timeouts = 0;
}  // namespace

bool TClockTrace() {
  static const bool on = [] {
    const char* v = std::getenv("HP2_TCLOCK_TRACE");
    return v && *v == '1';
  }();
  return on;
}

void RunningAdd(int d) {
  if (!g_test_clock) return;
  std::lock_guard<std::mutex> l(g_run_mu);
  if (TClockTrace() && !g_vb_swapped_flag.load()) {
    recomp::Ctx* c = g_host.current_ctx ? g_host.current_ctx() : nullptr;
    Log("tclock: vb %llu running %d%+d by %s %08X", (unsigned long long)g_vb_count.load(),
        g_running, d, t_counted ? "counted" : "other",
        c && c->esp ? Rd32(c->esp) : 0u);
  }
  g_running += d;
  if (d > 0) g_any_counted = true;
  if (g_running <= 0) g_run_cv.notify_all();
}

namespace {
// Boot (test clock, before the first Swap): true once the game has started
// and every counted thread is blocked; waits at most `max`.
bool WaitBootQuiescent(std::chrono::milliseconds max) {
  std::unique_lock<std::mutex> l(g_run_mu);
  return g_run_cv.wait_for(l, max, [] { return g_any_counted && g_running <= 0; });
}

// The calling (counted) thread blocks until no other counted thread runs.
void QuiesceOthers(uint64_t vblank, const char* when) {
  std::unique_lock<std::mutex> l(g_run_mu);
  g_running--;
  if (!g_run_cv.wait_for(l, std::chrono::milliseconds(250), [] { return g_running <= 0; })) {
    if (++g_barrier_timeouts <= 20 || g_barrier_timeouts % 100 == 0)
      Log("time: test clock: %d thread(s) still busy %s vblank %llu after 250 ms (#%llu)",
          g_running, when, (unsigned long long)vblank, (unsigned long long)g_barrier_timeouts);
  }
  g_running++;
}
}  // namespace

void TimeInit() {
  static bool started = false;
  if (started) return;
  started = true;
  g_t0 = Clock::now();
  auto wall = std::chrono::system_clock::now().time_since_epoch();
  g_systime0 = kUnixToFileTime + uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(wall).count() / 100);
  if (g_test_clock) {
    g_systime0 = kUnixToFileTime + 1035936000ull * 10000000;  // 2002-10-30 00:00 UTC
    Log("time: HP2_TEST_CLOCK=frames (vblank per Swap, kernel time per frame)");
  }
  if (!g_test_clock) std::thread(TickThread).detach();  // test clock: AdvanceLocked writes it
}

// KeTickCount: milliseconds since boot, updated every 1 ms.
KDATA(156, KeTickCount, 4, [](uint32_t va) {
  Wr32(va, 0);
  g_tick_va.store(va);
});

// KeQuerySystemTime(PLARGE_INTEGER CurrentTime)
KEXPORT(128, KeQuerySystemTime, 1) {
  Wr64(Arg(c, 0), SystemTime100ns());
  return 0;
}

// ULONGLONG KeQueryInterruptTime(void)
KEXPORT64(125, KeQueryInterruptTime, 0) { return InterruptTime100ns(); }

// KeDelayExecutionThread(WaitMode, Alertable, PLARGE_INTEGER Interval):
// negative = relative 100 ns units, positive = absolute system time.
KEXPORT(99, KeDelayExecutionThread, 3) {
  int64_t iv = int64_t(Rd64(Arg(c, 2)));
  if (iv < 0) {
    uint64_t d = uint64_t(-iv);
    if (d == 0) std::this_thread::yield();
    else SleepUntil100ns(InterruptTime100ns() + d);
  } else {
    int64_t rel = iv - int64_t(SystemTime100ns());
    if (rel > 0) SleepUntil100ns(InterruptTime100ns() + uint64_t(rel));
    else std::this_thread::yield();
  }
  return kSuccess;
}

// KeStallExecutionProcessor(MicroSeconds): busy wait.
KEXPORT(151, KeStallExecutionProcessor, 1) {
  auto end = Clock::now() + std::chrono::microseconds(Arg(c, 0));
  while (Clock::now() < end) {
  }
  return 0;
}

// ---- vblank pacer ---------------------------------------------------------------------
namespace {

std::mutex g_vb_mu;
std::condition_variable g_vb_cv;
double g_vb_hz = 60.0;
uint64_t g_vb_idle = 0;  // test clock: vblanks added because nothing presented
bool g_vb_swapped = false;  // test clock: hp2_vblank_wait was called
Clock::time_point g_vb_epoch;   // time of vblank g_vb_base
uint64_t g_vb_base = 0;
struct VbCallback {
  int id;
  void (*cb)(uint64_t, void*);
  void* user;
};
std::vector<VbCallback> g_vb_cbs;
int g_vb_next_id = 1;

Clock::time_point VblankTimeLocked(uint64_t n) {
  if (g_vb_hz <= 0) return Clock::now();
  return g_vb_epoch + std::chrono::nanoseconds(int64_t(double(int64_t(n) - int64_t(g_vb_base)) * 1e9 / g_vb_hz));
}

// Test clock: vblank `next` happens now (g_vb_mu held; callbacks run unlocked).
void AdvanceLocked(std::unique_lock<std::mutex>& l, uint64_t next) {
  g_vb_host_ns.store(HostNs(), std::memory_order_release);
  g_vb_count.store(next, std::memory_order_release);
  // KeTickCount is read straight from memory (the sound init seeds its
  // schedule with it, 0x117940): update it with the vblank, not 1 ms later.
  if (uint32_t va = g_tick_va.load(std::memory_order_relaxed))
    __atomic_store_n(reinterpret_cast<uint32_t*>(GP(va)), uint32_t(Now100ns() / 10000),
                     __ATOMIC_RELEASE);
  auto cbs = g_vb_cbs;
  l.unlock();
  for (auto& cb : cbs) cb.cb(next, cb.user);
  g_vb_cv.notify_all();
  l.lock();
}

void VblankThread() {
  std::unique_lock<std::mutex> l(g_vb_mu);
  if (g_test_clock) {
    for (;;) {
      uint64_t seen = g_vb_count.load();
      bool guard = false;  // advanced by the hang guard, not by quiescence
      if (g_vb_swapped) {
        if (g_vb_cv.wait_for(l, std::chrono::seconds(2), [&] { return g_vb_count.load() != seen; }))
          continue;
        guard = true;
      } else {
        l.unlock();
        guard = !WaitBootQuiescent(std::chrono::milliseconds(2000));
        l.lock();
        if (g_vb_swapped || g_vb_count.load() != seen) continue;
      }
      ++g_vb_idle;
      if (guard && (g_vb_idle <= 10 || g_vb_idle % 100 == 0))
        Log("time: test clock: %s for 2 s at vblank %llu, idle vblank (#%llu)",
            g_vb_swapped ? "no Swap" : "boot not quiescent", (unsigned long long)seen,
            (unsigned long long)g_vb_idle);
      AdvanceLocked(l, seen + 1);
      l.unlock();
      WakeOnTimeAdvance();
      l.lock();
    }
  }
  for (;;) {
    if (g_vb_hz <= 0) {
      g_vb_cv.wait(l);
      continue;
    }
    uint64_t next = g_vb_count.load() + 1;
    auto when = VblankTimeLocked(next);
    // Sleep until ~0.5 ms before, then spin for precision.
    l.unlock();
    std::this_thread::sleep_until(when - std::chrono::microseconds(500));
    while (Clock::now() < when) std::this_thread::yield();
    l.lock();
    if (g_vb_hz <= 0) continue;
    // A huge stall (debugger, sleep): resynchronise instead of bursting.
    if (Clock::now() - when > std::chrono::milliseconds(100)) {
      g_vb_base = next;
      g_vb_epoch = Clock::now();
    }
    g_vb_count.store(next);
    auto cbs = g_vb_cbs;
    l.unlock();
    for (auto& cb : cbs) cb.cb(next, cb.user);
    g_vb_cv.notify_all();
    l.lock();
  }
}

}  // namespace

void VblankInit(double hz) {
  static bool started = false;
  std::lock_guard<std::mutex> l(g_vb_mu);
  if (started) return;
  started = true;
  g_vb_hz = hz;
  g_vb_epoch = Clock::now();
  g_vb_base = 0;
  std::thread(VblankThread).detach();
}

}  // namespace hp2k

using namespace hp2k;

uint64_t hp2_kernel_rdtsc() {
  return uint64_t(double(TscNow100ns()) * (kTscHz / 1e7));
}

uint64_t hp2_vblank_count() { return g_vb_count.load(); }

uint64_t hp2_vblank_wait(uint64_t after) {
  std::unique_lock<std::mutex> l(g_vb_mu);
  if (g_test_clock) {
    if (!g_vb_swapped) {
      g_vb_swapped = true;
      g_vb_swapped_flag = true;
      Log("time: test clock: first Swap at vblank %llu (%llu boot vblanks)",
          (unsigned long long)g_vb_count.load(), (unsigned long long)g_vb_idle);
    }
    if (g_vb_count.load() > after) return g_vb_count.load();
    uint64_t next = g_vb_count.load() + 1;
    bool counted = t_counted;
    l.unlock();
    if (counted) QuiesceOthers(next - 1, "before");
    l.lock();
    if (g_vb_count.load() < next) AdvanceLocked(l, next);
    l.unlock();
    WakeOnTimeAdvance();
    if (counted) QuiesceOthers(next, "after");
    return next;
  }
  if (g_vb_hz <= 0) {
    uint64_t n = std::max(g_vb_count.load(), after) + 1;
    g_vb_count.store(n);
    return n;
  }
  g_vb_cv.wait(l, [&] { return g_vb_count.load() > after || g_vb_hz <= 0; });
  return g_vb_count.load();
}

double hp2_vblank_time(uint64_t n) {
  std::lock_guard<std::mutex> l(g_vb_mu);
  return std::chrono::duration<double>(VblankTimeLocked(n).time_since_epoch()).count();
}

void hp2_vblank_set_rate(double hz) {
  {
    std::lock_guard<std::mutex> l(g_vb_mu);
    g_vb_base = g_vb_count.load();
    g_vb_epoch = Clock::now();
    g_vb_hz = hz;
  }
  g_vb_cv.notify_all();
}

int hp2_vblank_add_callback(void (*cb)(uint64_t, void*), void* user) {
  std::lock_guard<std::mutex> l(g_vb_mu);
  g_vb_cbs.push_back({g_vb_next_id, cb, user});
  return g_vb_next_id++;
}

void hp2_vblank_remove_callback(int id) {
  std::lock_guard<std::mutex> l(g_vb_mu);
  for (size_t i = 0; i < g_vb_cbs.size(); i++)
    if (g_vb_cbs[i].id == id) {
      g_vb_cbs.erase(g_vb_cbs.begin() + i);
      return;
    }
}
