// Host unit tests of the kernel HLE: no lifted code. A fake 4 GiB guest
// arena, "guest functions" that are host lambdas at fake guest addresses,
// and std::thread-backed guest contexts. Silent unless something fails.
//
//   out/kernel/hp2_kernel_tests     (exit code 0 = all passed)

#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <sstream>
#include <thread>
#include <vector>

#if HP2_TEST_SDL
#include <SDL3/SDL.h>
#endif

#include "hle/kernel/kernel_exports.h"
#include "hle/kernel/kernel_internal.h"
#include "hle/kernel/vfs.h"
#include "hle/xinput/input_filter.h"
#include "hle/xinput/pad_script.h"
#include "hle/xinput/xinput.h"
#include "runtime/ctx.h"

using recomp::Ctx;

namespace {

std::atomic<int> g_failed{0}, g_checks{0};
#define CHECK(cond)                                                        \
  do {                                                                     \
    g_checks++;                                                            \
    if (!(cond)) {                                                         \
      g_failed++;                                                          \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                                      \
  } while (0)
#define CHECK_EQ(a, b)                                                                  \
  do {                                                                                  \
    g_checks++;                                                                         \
    auto va_ = (a);                                                                     \
    auto vb_ = (b);                                                                     \
    if (!(va_ == vb_)) {                                                                \
      g_failed++;                                                                       \
      std::fprintf(stderr, "FAIL %s:%d: %s == %s (%llX vs %llX)\n", __FILE__, __LINE__, \
                   #a, #b, (unsigned long long)va_, (unsigned long long)vb_);           \
    }                                                                                   \
  } while (0)

// ---- fake runtime ----
uint8_t* g_base;
size_t g_hpage;

bool Commit(uint32_t va, uint32_t size, bool) {
  uintptr_t a = uintptr_t(va) & ~(g_hpage - 1);
  uintptr_t b = (uintptr_t(va) + size + g_hpage - 1) & ~(g_hpage - 1);
  return mprotect(g_base + a, b - a, PROT_READ | PROT_WRITE) == 0;
}
// Zero instead of unmapping: host pages are larger than guest ones.
void Decommit(uint32_t va, uint32_t size) {
  Commit(va, size, true);
  std::memset(g_base + va, 0, size);
}

using FakeFn = std::function<uint32_t(Ctx&, const uint32_t*, int)>;
std::mutex g_fn_mu;
std::map<uint32_t, FakeFn> g_guest_fns;
uint32_t g_next_fn = 0x00500000;  // inside the "image": never allocated
uint32_t GuestFn(FakeFn f) {
  std::lock_guard<std::mutex> l(g_fn_mu);
  uint32_t va = g_next_fn += 0x10;
  g_guest_fns[va] = std::move(f);
  return va;
}

uint32_t CallGuestFake(Ctx& c, uint32_t va, const uint32_t* args, int n) {
  FakeFn f;
  {
    std::lock_guard<std::mutex> l(g_fn_mu);
    auto it = g_guest_fns.find(va);
    if (it == g_guest_fns.end()) {
      std::fprintf(stderr, "call to unknown fake guest fn %08X\n", va);
      std::abort();
    }
    f = it->second;
  }
  // Like a real call: arguments and a return address on the guest stack.
  uint32_t esp = c.esp;
  c.esp -= 4 * n + 4;
  for (int i = 0; i < n; i++) std::memcpy(g_base + c.esp + 4 + 4 * i, &args[i], 4);
  uint32_t r = f(c, args, n);
  c.esp = esp;
  return r;
}

thread_local Ctx* t_ctx = nullptr;
void StartThread(std::function<void(Ctx&)> body) {
  std::thread([body] {
    Ctx* c = new Ctx();
    t_ctx = c;
    body(*c);
  }).detach();
}
Ctx* CurrentCtx() { return t_ctx; }

// ---- calling HostFns ----
Ctx g_main;

uint64_t Call(Ctx& c, uint32_t ord, std::initializer_list<uint32_t> args) {
  HostFn fn = hp2_kernel_lookup(ord);
  if (!fn) {
    std::fprintf(stderr, "no export %u\n", ord);
    std::abort();
  }
  uint32_t esp0 = c.esp;
  uint32_t sp = esp0 - 4 * uint32_t(args.size()) - 4;
  uint32_t i = 0;
  for (uint32_t a : args) std::memcpy(g_base + sp + 4 + 4 * i++, &a, 4);
  uint32_t ret = 0xDEADBEEF;
  std::memcpy(g_base + sp, &ret, 4);
  c.esp = sp;
  fn(c);
  if (ord != 8 && c.esp != esp0) {
    std::fprintf(stderr, "FAIL: ordinal %u popped %d bytes, expected %d\n", ord,
                 int(c.esp - sp), int(esp0 - sp));
    g_failed++;
  }
  c.esp = esp0;
  return uint64_t(c.eax) | uint64_t(c.edx) << 32;
}
// Host threads without a guest context get a private stack slot in the
// committed "image" range (0x100000 + n * 0x4000).
std::atomic<uint32_t> g_slot{0};
Ctx& ThreadCtx() {
  if (t_ctx) return *t_ctx;
  thread_local Ctx own{};
  if (!own.esp) own.esp = 0x100000 + (g_slot++ + 1) * 0x4000 - 0x10;
  return own;
}
uint32_t Call32(uint32_t ord, std::initializer_list<uint32_t> args) {
  return uint32_t(Call(ThreadCtx(), ord, args));
}

// Test structures live in the kernel's own pool (committed).
uint32_t Scratch(uint32_t size) { return Call32(14, {size}); }
uint32_t R32(uint32_t va) { uint32_t v; std::memcpy(&v, g_base + va, 4); return v; }
uint64_t R64(uint32_t va) { uint64_t v; std::memcpy(&v, g_base + va, 8); return v; }
void W32(uint32_t va, uint32_t v) { std::memcpy(g_base + va, &v, 4); }
void W64(uint32_t va, uint64_t v) { std::memcpy(g_base + va, &v, 8); }
std::string RStr(uint32_t va, size_t n) { return std::string(reinterpret_cast<char*>(g_base + va), n); }
uint32_t AnsiStr(const std::string& s) {
  uint32_t p = Scratch(uint32_t(8 + s.size() + 1));
  std::memcpy(g_base + p + 8, s.c_str(), s.size() + 1);
  W32(p, uint32_t(s.size()) | uint32_t(s.size() + 1) << 16);
  W32(p + 4, p + 8);
  return p;
}
uint32_t ObjAttr(const std::string& path, uint32_t root = 0) {
  uint32_t oa = Scratch(12);
  W32(oa, root);
  W32(oa + 4, AnsiStr(path));
  W32(oa + 8, 0x40);
  return oa;
}
// A private guest stack for a helper host thread.
Ctx HelperCtx() {
  Ctx c{};
  c.esp = 0x100000 + (g_slot++ + 1) * 0x4000 - 0x10;
  return c;
}

std::string g_tmp;
void WriteFile(const std::string& p, const std::string& data) {
  std::string cmd = "mkdir -p '" + p.substr(0, p.rfind('/')) + "'";
  if (system(cmd.c_str()) != 0) std::abort();
  std::ofstream(p, std::ios::binary) << data;
}

// The 115 kernel imports of default.xbe (recon/ghidra/default.flat.json).
const uint32_t kImports[] = {
    1,   2,   3,   4,   8,   14,  15,  16,  17,  23,  24,  37,  40,  44,  46,  47,  49,  65,  67,
    74,  81,  83,  87,  95,  97,  98,  99,  100, 107, 109, 113, 119, 125, 128, 129, 137, 139, 142,
    143, 145, 149, 150, 151, 153, 156, 158, 159, 160, 161, 164, 165, 166, 168, 171, 173, 175, 176,
    178, 179, 180, 182, 184, 186, 187, 189, 190, 196, 198, 199, 200, 202, 203, 205, 207, 211, 215,
    217, 218, 219, 224, 225, 226, 233, 234, 235, 236, 246, 250, 255, 258, 259, 269, 277, 279, 289,
    291, 294, 301, 302, 312, 322, 323, 324, 325, 327, 328, 335, 336, 337, 354, 356, 357, 358, 359,
    360};

// ============================================================================
void TestCoverage() {
  CHECK_EQ(sizeof(kImports) / sizeof(kImports[0]), size_t(115));
  for (uint32_t o : kImports) {
    bool fn = hp2_kernel_lookup(o) != nullptr;
    bool data = hp2_kernel_data_va(o) != 0;
    if (fn == data) std::fprintf(stderr, "ordinal %u (%s): fn=%d data=%d\n", o, hp2_kernel_name(o), fn, data);
    CHECK(fn != data);
  }
  CHECK(std::string(hp2_kernel_name(255)) == "PsCreateSystemThreadEx");
  CHECK_EQ(hp2_kernel_thunk_va(1), 0xFE000010u);
}

void TestMemory() {
  uint32_t pb = Scratch(8), ps = pb + 4;
  // Reserve 1 MB, then commit inside it.
  W32(pb, 0);
  W32(ps, 0x100000);
  CHECK_EQ(Call32(184, {pb, 0, ps, 0x2000, 4}), 0u);
  uint32_t base = R32(pb);
  CHECK(base >= 0x00600000 && base % 0x10000 == 0);
  CHECK_EQ(R32(ps), 0x100000u);
  W32(pb, base + 0x3010);
  W32(ps, 0x1000);
  CHECK_EQ(Call32(184, {pb, 0, ps, 0x1000, 4}), 0u);
  CHECK_EQ(R32(pb), base + 0x3000);
  CHECK_EQ(R32(ps), 0x2000u);  // 0x3010..0x4010 spans two pages
  W32(base + 0x3000, 0x12345678);
  CHECK_EQ(R32(base + 0x3000), 0x12345678u);
  uint32_t mbi = Scratch(0x1C);
  CHECK_EQ(Call32(217, {base + 0x3000, mbi}), 0u);
  CHECK_EQ(R32(mbi + 0x04), base);
  CHECK_EQ(R32(mbi + 0x0C), 0x2000u);
  CHECK_EQ(R32(mbi + 0x10), 0x1000u);  // MEM_COMMIT
  CHECK_EQ(Call32(217, {base, mbi}), 0u);
  CHECK_EQ(R32(mbi + 0x10), 0x2000u);  // MEM_RESERVE
  CHECK_EQ(R32(mbi + 0x0C), 0x3000u);
  CHECK_EQ(Call32(180, {base}), 0x100000u);  // MmQueryAllocationSize
  W32(pb, base);
  W32(ps, 0);
  CHECK_EQ(Call32(199, {pb, ps, 0x8000}), 0u);  // MEM_RELEASE
  CHECK_EQ(R32(ps), 0x100000u);
  W32(pb, base);
  CHECK(Call32(199, {pb, ps, 0x8000}) != 0u);
  // Reserve+commit at a fixed address; overlap fails.
  W32(pb, 0x10000000);
  W32(ps, 0x20000);
  CHECK_EQ(Call32(184, {pb, 0, ps, 0x3000, 4}), 0u);
  CHECK_EQ(R32(pb), 0x10000000u);
  W32(0x1001FFFC, 7);
  W32(pb, 0x10000000);
  W32(ps, 0x1000);
  CHECK(Call32(184, {pb, 0, ps, 0x3000, 4}) != 0u);

  // Contiguous memory.
  uint32_t a = Call32(166, {0x3000, 0, 0x03FFFFFF, 0x10000, 4});
  CHECK(a >= 0x80000000 && a < 0x84000000 && a % 0x10000 == 0);
  CHECK_EQ(Call32(173, {a}), a - 0x80000000);
  CHECK_EQ(Call32(180, {a}), 0x3000u);
  W32(a + 0x2FFC, 1);
  uint32_t hi = Call32(166, {0x1000, 0x01000000, 0x01FFFFFF, 0, 4});
  CHECK(hi >= 0x81000000 && hi < 0x82000000);
  CHECK_EQ(Call32(171, {a}), 0u);
  CHECK(Call32(165, {0x1000}) != 0);
  // Pool.
  uint32_t p = Call32(14, {100});
  CHECK(p >= 0x90000000 && p % 16 == 0);
  CHECK_EQ(Call32(23, {p}), 112u);
  Call32(17, {p});
  CHECK_EQ(Call32(23, {p}), 0u);
}

void TestStrings() {
  uint32_t s = Scratch(8), txt = Scratch(16);
  std::memcpy(g_base + txt, "Hello", 6);
  Call32(289, {s, txt});
  CHECK_EQ(R32(s), 5u | 6u << 16);
  CHECK_EQ(R32(s + 4), txt);
  Call32(289, {s, 0});
  CHECK_EQ(R32(s), 0u);
  uint32_t a = AnsiStr("ABC"), b = AnsiStr("abc");
  CHECK_EQ(Call32(279, {a, b, 0}), 0u);
  CHECK_EQ(Call32(279, {a, b, 1}), 1u);
  uint32_t m = Scratch(16);
  W32(m, 7);
  W32(m + 4, 7);
  W32(m + 8, 8);
  CHECK_EQ(Call32(269, {m, 16, 7}), 8u);
  CHECK_EQ(Call32(301, {0xC0000034}), 2u);
  CHECK_EQ(Call32(301, {0}), 0u);
  CHECK_EQ(Call32(301, {0x80070057}), 0x57u);
  // SHA-1("abc") = a9993e36 4706816a ba3e2571 7850c26c 9cd0d89d
  uint32_t ctx = Scratch(0x80), dig = Scratch(20), in = Scratch(4);
  std::memcpy(g_base + in, "abc", 3);
  Call32(335, {ctx});
  Call32(336, {ctx, in, 3});
  Call32(337, {ctx, dig});
  const uint8_t want[20] = {0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e,
                            0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d};
  CHECK(std::memcmp(g_base + dig, want, 20) == 0);
}

void TestSettings() {
  // First run: the profile has hp2.ini (all commented out) and README.txt.
  struct stat st;
  CHECK(stat((g_tmp + "/data/README.txt").c_str(), &st) == 0);
  CHECK(stat((g_tmp + "/data/hp2.ini").c_str(), &st) == 0);
  uint32_t t = Scratch(4), v = Scratch(4), r = Scratch(4);
  CHECK_EQ(Call32(24, {7, t, v, 4, r}), 0u);
  CHECK_EQ(R32(v), 1u);  // English (the template sets nothing)
  CHECK_EQ(Call32(24, {0x104, t, v, 4, r}), 0u);
  CHECK_EQ(R32(v), 1u);  // North America -> _USA slot
  CHECK_EQ(Call32(24, {0x103, t, v, 4, r}), 0u);
  CHECK_EQ((R32(v) >> 8) & 0xFF, 1u);  // NTSC-M, never PAL (3)
  setenv("HP2_LANG", "uk", 1);
  CHECK_EQ(Call32(24, {0x104, t, v, 4, r}), 0u);
  CHECK_EQ(R32(v), 4u);  // non-NA region + English -> _UK slot
  unsetenv("HP2_LANG");
  // <profile data dir>/hp2.ini when HP2_LANG is unset; HP2_LANG wins.
  // Only en and uk exist in the port: fr / de fall back to English.
  WriteFile(g_tmp + "/data/hp2.ini", "# HP2Port\nlanguage=uk\n");
  CHECK_EQ(Call32(24, {0x104, t, v, 4, r}), 0u);
  CHECK_EQ(R32(v), 4u);  // _UK slot from hp2.ini
  // uk_text (Ukrainian text + English speech) boots as the _UK slot. The mods
  // have _UKT\text.bin (setup; the disc listing is cached, so the "no _UKT ->
  // uk" case is in tests/scripts/language.sh).
  WriteFile(g_tmp + "/data/hp2.ini", "language=uk_text\n");
  CHECK(hp2k::HybridSlotAvailable());
  CHECK(hp2k::LanguageKey() == "uk_text");
  CHECK_EQ(Call32(24, {0x104, t, v, 4, r}), 0u);
  CHECK_EQ(R32(v), 4u);
  hp2k::WriteLanguageToIni("en");
  CHECK(hp2k::LanguageKey() == "en");
  hp2k::WriteLanguageToIni("uk_text");
  CHECK(hp2k::LanguageKey() == "uk_text");
  WriteFile(g_tmp + "/data/hp2.ini", "# HP2Port\nlanguage=fr\n");
  CHECK_EQ(Call32(24, {7, t, v, 4, r}), 0u);
  CHECK_EQ(R32(v), 1u);
  CHECK_EQ(Call32(24, {0x104, t, v, 4, r}), 0u);
  CHECK_EQ(R32(v), 1u);  // -> _USA
  setenv("HP2_LANG", "de", 1);
  CHECK_EQ(Call32(24, {7, t, v, 4, r}), 0u);
  CHECK_EQ(R32(v), 1u);
  setenv("HP2_LANG", "xx", 1);
  CHECK_EQ(Call32(24, {7, t, v, 4, r}), 0u);
  CHECK_EQ(R32(v), 1u);  // unknown -> English
  unsetenv("HP2_LANG");
  WriteFile(g_tmp + "/data/hp2.ini", "language=en\n");
  CHECK_EQ(R32(hp2_kernel_data_va(40)), 3u);
  CHECK_EQ(R32(hp2_kernel_data_va(164)), 0u);
  CHECK_EQ(R32(hp2_kernel_data_va(322)) & 8, 0u);  // XAPI then links D:
  Call32(2, {0, 6, 0, r});                         // AV_QUERY_AV_CAPABILITIES
  CHECK_EQ((R32(r) >> 8) & 0xFF, 1u);
}

void TestTime() {
  uint32_t t = Scratch(8);
  Call32(128, {t});
  uint64_t ft = R64(t);
  CHECK(ft > 132223104000000000ull && ft < 157469184000000000ull);  // 2020..2100
  uint32_t tick_va = hp2_kernel_data_va(156);
  uint32_t t0 = R32(tick_va);
  uint64_t it0 = Call(g_main, 125, {});
  uint64_t tsc0 = hp2_kernel_rdtsc();
  W64(t, uint64_t(-500000ll));  // 50 ms relative
  auto h0 = std::chrono::steady_clock::now();
  Call32(99, {0, 0, t});
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - h0).count();
  CHECK(ms >= 49 && ms < 150);
  uint32_t t1 = R32(tick_va);
  CHECK(t1 - t0 >= 45 && t1 - t0 < 200);
  uint64_t it1 = Call(g_main, 125, {});
  CHECK(it1 - it0 >= 490000 && it1 - it0 < 2000000);
  double tsc_mhz = double(hp2_kernel_rdtsc() - tsc0) / (double(it1 - it0) / 1e7) / 1e6;
  CHECK(std::fabs(tsc_mhz - 733.33) < 15);
}

void TestEvents() {
  uint32_t ph = Scratch(4), to = Scratch(8);
  CHECK_EQ(Call32(189, {ph, 0, 1, 0}), 0u);  // synchronization event, clear
  uint32_t h = R32(ph);
  W64(to, 0);
  CHECK_EQ(Call32(233, {h, 0, to}), 0x102u);
  std::thread setter([h] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Ctx c = HelperCtx();
    Call(c, 225, {h, 0});
  });
  uint32_t to2 = Scratch(8);
  W64(to2, uint64_t(-10000000ll));
  CHECK_EQ(Call32(233, {h, 0, to2}), 0u);
  setter.join();
  CHECK_EQ(Call32(233, {h, 0, to}), 0x102u);  // auto-reset
  CHECK_EQ(Call32(189, {ph, 0, 0, 1}), 0u);   // notification event, set
  uint32_t n = R32(ph);
  CHECK_EQ(Call32(233, {n, 0, to}), 0u);
  CHECK_EQ(Call32(233, {n, 0, to}), 0u);
  CHECK_EQ(Call32(186, {n}), 0u);
  CHECK_EQ(Call32(233, {n, 0, to}), 0x102u);
  uint32_t arr = Scratch(8);
  W32(arr, h);
  W32(arr + 4, n);
  Call32(225, {n, 0});
  CHECK_EQ(Call32(235, {2, arr, 1, 0, 0, to}), 1u);      // WaitAny
  CHECK_EQ(Call32(235, {2, arr, 0, 0, 0, to}), 0x102u);  // WaitAll: h clear
  Call32(225, {h, 0});
  CHECK_EQ(Call32(235, {2, arr, 0, 0, 0, to}), 0u);
  // Pulse wakes a blocked waiter and leaves the event reset.
  Call32(186, {n});
  std::atomic<uint32_t> pulsed{0xFFFF};
  std::thread waiter([&] {
    Ctx c = HelperCtx();
    uint32_t t = c.esp - 0x100;
    W64(t, uint64_t(-10000000ll));
    pulsed = uint32_t(Call(c, 233, {n, 0, t}));
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  Call32(205, {n, 0});
  waiter.join();
  CHECK_EQ(pulsed.load(), 0u);
  CHECK_EQ(Call32(233, {n, 0, to}), 0x102u);
  // KeSetEvent on a raw KEVENT returns the previous state.
  uint32_t ev = Scratch(16);
  W32(ev, 0x00040000);
  W32(ev + 4, 0);
  CHECK_EQ(Call32(145, {ev, 1, 0}), 0u);
  CHECK_EQ(Call32(145, {ev, 1, 0}), 1u);
  CHECK_EQ(Call32(159, {ev, 0, 0, 0, to}), 0u);
  CHECK_EQ(Call32(187, {h}), 0u);
  CHECK(Call32(187, {h}) != 0u);
}

void TestTimersAndDpcs() {
  uint32_t timer = Scratch(0x28), dpc = Scratch(0x1C), to = Scratch(8);
  std::atomic<int> fired{0};
  std::atomic<uint32_t> got_ctx{0};
  std::atomic<int> irql{-1};
  uint32_t routine = GuestFn([&](Ctx& c, const uint32_t* a, int n) {
    if (n == 4 && a[0] == dpc) got_ctx = a[1];
    irql = g_base[c.fs_base + 0x24];
    fired++;
    return 0u;
  });
  Call32(113, {timer, 0});
  Call32(107, {dpc, routine, 0x1234});
  int64_t due = -200000;  // 20 ms
  CHECK_EQ(Call32(149, {timer, uint32_t(due), uint32_t(uint64_t(due) >> 32), dpc}), 0u);
  W64(to, 0);
  CHECK_EQ(Call32(159, {timer, 0, 0, 0, to}), 0x102u);
  W64(to, uint64_t(-10000000ll));
  CHECK_EQ(Call32(159, {timer, 0, 0, 0, to}), 0u);
  for (int i = 0; i < 100 && !fired; i++) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  CHECK_EQ(fired.load(), 1);
  CHECK_EQ(got_ctx.load(), 0x1234u);
  CHECK_EQ(irql.load(), 2);
  // Periodic 5 ms timer, then cancel.
  Call32(113, {timer, 1});
  fired = 0;
  CHECK_EQ(Call32(150, {timer, uint32_t(-50000), 0xFFFFFFFF, 5, dpc}), 0u);
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  CHECK_EQ(Call32(97, {timer}), 1u);
  int n = fired.load();
  CHECK(n >= 5 && n <= 14);
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  CHECK(fired.load() <= n + 1);
  fired = 0;
  CHECK_EQ(Call32(119, {dpc, 1, 2}), 1u);
  for (int i = 0; i < 100 && !fired; i++) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  CHECK_EQ(fired.load(), 1);
  CHECK_EQ(Call32(129, {}) & 0xFF, 0u);  // KeRaiseIrqlToDpcLevel -> old 0
  g_main.ecx = 0;
  Call32(161, {});                       // KfLowerIrql(0)
}

void TestCriticalSection() {
  uint32_t cs = Scratch(0x1C), counter = Scratch(4);
  Call32(291, {cs});
  CHECK_EQ(R32(cs + 0x10), 0xFFFFFFFFu);
  std::vector<std::thread> ts;
  for (int t = 0; t < 4; t++)
    ts.emplace_back([&] {
      Ctx c = HelperCtx();
      for (int i = 0; i < 2000; i++) {
        Call(c, 277, {cs});
        Call(c, 277, {cs});  // recursion
        W32(counter, R32(counter) + 1);
        Call(c, 294, {cs});
        Call(c, 294, {cs});
      }
    });
  for (auto& t : ts) t.join();
  CHECK_EQ(R32(counter), 8000u);
  CHECK_EQ(R32(cs + 0x18), 0u);
  CHECK_EQ(R32(cs + 0x10), 0xFFFFFFFFu);
}

void TestThreads() {
  uint32_t ph = Scratch(4), ptid = Scratch(4), to = Scratch(8);
  std::atomic<int> ran{0}, layout_ok{0};
  std::atomic<uint32_t> seen_start{0}, seen_ctx{0};
  uint32_t start = 0x00ABC000;  // only passed through to the system routine
  uint32_t system = GuestFn([&](Ctx& c, const uint32_t* a, int) {
    seen_start = a[0];
    seen_ctx = a[1];
    // XAPI's TLS layout: KTHREAD.TlsData == fs:[4] - TlsDataSize.
    uint32_t pcr = c.fs_base, kt = R32(pcr + 0x28);
    layout_ok = R32(kt + 0x28) == R32(pcr + 4) - 0x14 && c.esp < R32(kt + 0x28) &&
                c.esp > R32(pcr + 8) && R32(pcr + 0x18) == pcr && R32(pcr + 0x20) == pcr + 0x28;
    ran++;
    Call(c, 258, {0});  // PsTerminateSystemThread: does not return
    ran += 100;
    return 0u;
  });
  CHECK_EQ(Call32(255, {ph, 0, 0x10000, 0x14, ptid, start, 0x77, 1, 0, system}), 0u);
  uint32_t h = R32(ph);
  CHECK(R32(ptid) != 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  CHECK_EQ(ran.load(), 0);  // created suspended
  W64(to, 0);
  CHECK_EQ(Call32(233, {h, 0, to}), 0x102u);
  uint32_t prev = Scratch(4);
  CHECK_EQ(Call32(224, {h, prev}), 0u);
  CHECK_EQ(R32(prev), 1u);
  W64(to, uint64_t(-20000000ll));
  CHECK_EQ(Call32(233, {h, 0, to}), 0u);
  CHECK_EQ(ran.load(), 1);
  CHECK_EQ(layout_ok.load(), 1);
  CHECK_EQ(seen_start.load(), start);
  CHECK_EQ(seen_ctx.load(), 0x77u);
  uint32_t obj = Scratch(4);
  CHECK_EQ(Call32(246, {h, hp2_kernel_data_va(259), obj}), 0u);
  CHECK_EQ(g_base[R32(obj)], 6);  // ThreadObject
  std::atomic<uint32_t> arg{0};
  uint32_t direct = GuestFn([&](Ctx&, const uint32_t* a, int) {
    arg = a[0];
    return 0u;
  });
  CHECK_EQ(Call32(255, {ph, 0, 0, 0, 0, direct, 0x55, 0, 0, 0}), 0u);
  CHECK_EQ(Call32(233, {R32(ph), 0, to}), 0u);
  CHECK_EQ(arg.load(), 0x55u);
  uint32_t dir = Scratch(0x18);  // default.xbe's TLS directory: zero fill 0xC
  W32(dir + 0x10, 0xC);
  CHECK_EQ(hp2_kernel_tls_size(dir), 0x14u);
}

struct File {
  uint32_t h = 0, st = 0, info = 0;
};
File Open(const std::string& path, uint32_t disp, uint32_t opts) {
  uint32_t ph = Scratch(4), iosb = Scratch(8);
  File f;
  f.st = Call32(190, {ph, 0xC0100000, ObjAttr(path), iosb, 0, 0x80, 3, disp, opts});
  f.h = f.st == 0 ? R32(ph) : 0;
  f.info = R32(iosb + 4);
  return f;
}
std::string Read(uint32_t h, uint32_t n, int64_t off = -1, uint32_t* status = nullptr) {
  uint32_t buf = Scratch(n + 1), iosb = Scratch(8), po = 0;
  if (off >= 0) {
    po = Scratch(8);
    W64(po, uint64_t(off));
  }
  uint32_t st = Call32(219, {h, 0, 0, 0, iosb, buf, n, po});
  if (status) *status = st;
  return RStr(buf, R32(iosb + 4));
}
uint32_t Write(uint32_t h, const std::string& s, int64_t off = -1) {
  uint32_t buf = Scratch(uint32_t(s.size() + 1)), iosb = Scratch(8), po = 0;
  std::memcpy(g_base + buf, s.data(), s.size());
  if (off >= 0) {
    po = Scratch(8);
    W64(po, uint64_t(off));
  }
  return Call32(236, {h, 0, 0, 0, iosb, buf, uint32_t(s.size()), po});
}

void TestFiles() {
  // Disc: mods override, case-insensitive lookup, read-only.
  File f = Open("\\??\\D:\\_usa\\TEXT.BIN", 1, 0x40);
  CHECK_EQ(f.st, 0u);
  CHECK(Read(f.h, 100) == std::string("modded"));
  f = Open("\\??\\D:\\_USA\\other.bin", 1, 0x40);
  CHECK_EQ(f.st, 0u);
  CHECK(Read(f.h, 3, 2) == std::string("sc-"));
  uint32_t st;
  CHECK(Read(f.h, 10, 100, &st) == std::string());
  CHECK_EQ(st, 0xC0000011u);  // END_OF_FILE
  CHECK_EQ(Write(f.h, "x"), 0xC00000A2u);
  CHECK_EQ(Open("\\??\\D:\\_USA\\missing.bin", 1, 0x40).st, 0xC0000034u);
  CHECK_EQ(Open("\\??\\D:\\nodir\\missing.bin", 1, 0x40).st, 0xC000003Au);
  CHECK_EQ(Open("\\??\\D:\\_USA\\new.bin", 2, 0x40).st, 0xC00000A2u);
  CHECK_EQ(Open("\\Device\\CdRom0\\_USA", 1, 0x40).st, 0xC00000BAu);  // a directory
  uint32_t iosb = Scratch(8), info = Scratch(0x38);
  CHECK_EQ(Call32(211, {f.h, iosb, info, 0x38, 34}), 0u);
  CHECK_EQ(R64(info + 0x28), 9u);  // "disc-only"
  CHECK_EQ(R32(info + 0x30) & 0x10, 0u);
  // Directory enumeration merges mods + disc.
  File d = Open("\\??\\D:\\_USA", 1, 0x1);
  CHECK_EQ(d.st, 0u);
  uint32_t di = Scratch(0x140), mask = AnsiStr("*.bin");
  std::vector<std::string> names;
  for (int i = 0; i < 10; i++) {
    uint32_t s = Call32(207, {d.h, 0, 0, 0, iosb, di, 0x140, 1, i == 0 ? mask : 0, 0});
    if (s == 0x80000006u) break;
    CHECK_EQ(s, 0u);
    names.push_back(RStr(di + 0x40, R32(di + 0x3C)));
  }
  CHECK_EQ(names.size(), size_t(2));
  if (names.size() == 2) {
    CHECK(hp2k::vfs::Lower(names[0]) == "other.bin");
    CHECK(hp2k::vfs::Lower(names[1]) == "text.bin");
  }
  // HDD: XAPI's T: setup, then a save file.
  CHECK_EQ(Open("\\Device\\Harddisk0\\partition1\\TDATA", 3, 0x4021).st, 0u);
  File td = Open("\\Device\\Harddisk0\\partition1\\TDATA\\4541000E", 3, 0x4021);
  CHECK_EQ(td.st, 0u);
  CHECK_EQ(td.info, 2u);  // created
  CHECK_EQ(Call32(67, {AnsiStr("\\??\\T:"), AnsiStr("\\Device\\Harddisk0\\partition1\\TDATA\\4541000E")}), 0u);
  CHECK_EQ(Call32(67, {AnsiStr("\\??\\T:"), AnsiStr("\\x")}), 0xC0000035u);
  File s = Open("\\??\\T:\\save.dat", 5, 0x40);
  CHECK_EQ(s.st, 0u);
  CHECK_EQ(Write(s.h, "hello world"), 0u);
  CHECK(Read(s.h, 5, 6) == std::string("world"));
  uint32_t pos = Scratch(8);
  W64(pos, 0);
  CHECK_EQ(Call32(226, {s.h, iosb, pos, 8, 14}), 0u);
  CHECK(Read(s.h, 5) == std::string("hello"));
  CHECK_EQ(Call32(187, {s.h}), 0u);
  struct stat sb;
  std::string host = g_tmp + "/data/hdd/partition1/TDATA/4541000E/save.dat";
  CHECK(stat(host.c_str(), &sb) == 0 && sb.st_size == 11);
  CHECK_EQ(Open("\\??\\T:\\SAVE.DAT", 2, 0x40).st, 0xC0000035u);  // exists
  // Rename via FileRenameInformation.
  s = Open("\\??\\T:\\Save.dat", 1, 0x40);
  uint32_t ri = Scratch(16), nm = Scratch(32);
  std::memcpy(g_base + nm, "\\??\\T:\\renamed.dat", 18);
  W32(ri, 1);
  W32(ri + 4, 0);
  W32(ri + 8, 18 | 18 << 16);
  W32(ri + 12, nm);
  CHECK_EQ(Call32(226, {s.h, iosb, ri, 16, 10}), 0u);
  Call32(187, {s.h});
  CHECK_EQ(Open("\\??\\T:\\renamed.dat", 1, 0x40).st, 0u);
  // Delete via disposition.
  s = Open("\\??\\T:\\renamed.dat", 1, 0x40);
  uint32_t del = Scratch(4);
  W32(del, 1);
  CHECK_EQ(Call32(226, {s.h, iosb, del, 1, 13}), 0u);
  Call32(187, {s.h});
  CHECK_EQ(Open("\\??\\T:\\renamed.dat", 1, 0x40).st, 0xC0000034u);
  // Symbolic link query.
  uint32_t ph = Scratch(4);
  CHECK_EQ(Call32(203, {ph, ObjAttr("\\??\\T:")}), 0u);
  uint32_t target = Scratch(8), tbuf = Scratch(64), rl = Scratch(4);
  W32(target, 0 | 64 << 16);
  W32(target + 4, tbuf);
  CHECK_EQ(Call32(215, {R32(ph), target, rl}), 0u);
  CHECK(RStr(tbuf, R32(target) & 0xFFFF) == "\\Device\\Harddisk0\\partition1\\TDATA\\4541000E");
  // Z: cache partition: 16 KB clusters, no free space.
  File z = Open("\\Device\\Harddisk0\\Partition3", 1, 0x800021);
  CHECK_EQ(z.st, 0u);
  uint32_t fs = Scratch(0x18);
  CHECK_EQ(Call32(218, {z.h, iosb, fs, 0x18, 3}), 0u);
  CHECK_EQ(R32(fs + 0x10) * R32(fs + 0x14), 0x4000u);
  CHECK_EQ(R64(fs + 8), 0u);
  // XAPI cache DB in partition 0 at 0x800.
  File p0 = Open("\\Device\\Harddisk0\\partition0", 1, 0x10);
  CHECK_EQ(p0.st, 0u);
  CHECK(Read(p0.h, 4, 0x800) == std::string(4, '\0'));
  CHECK_EQ(Write(p0.h, "\x86\x52\x31\x97", 0x800), 0u);
  CHECK(Read(p0.h, 4, 0x800) == std::string("\x86\x52\x31\x97"));
  uint32_t geo = Scratch(0x20);
  CHECK_EQ(Call32(196, {z.h, 0, 0, 0, iosb, 0x70000, 0, 0, geo, 0x18}), 0u);
  CHECK_EQ(R32(geo + 0x14), 512u);
  CHECK_EQ(Call32(196, {z.h, 0, 0, 0, iosb, 0x74004, 0, 0, geo, 0x20}), 0u);
  CHECK(R64(geo + 8) > 0);
  // XAPI's DVD security check (FUN_0011131C): MODE SENSE(10) page 0x3E via
  // SCSI_PASS_THROUGH_DIRECT on \Device\CdRom0.
  File cd = Open("\\Device\\CdRom0", 1, 0x20);
  CHECK_EQ(cd.st, 0u);
  uint32_t spt = Scratch(0x2C), page = Scratch(0x1C);
  W32(spt, 0x2C);
  g_base[spt + 8] = 1;
  W32(spt + 0xC, 0x1C);
  W32(spt + 0x14, page);
  W32(spt + 0x1C, 0x003E005A);
  W32(spt + 0x24, 0x1C);
  CHECK_EQ(Call32(196, {cd.h, 0, 0, 0, iosb, 0x4D014, spt, 0x2C, 0, 0}), 0u);
  CHECK(g_base[page + 0xB] == 1 && g_base[page + 0xA] != 0 && g_base[page + 0xC] != 0);
  W32(spt + 0x1C, 0x0000005A);  // another page: refused
  CHECK_EQ(Call32(196, {cd.h, 0, 0, 0, iosb, 0x4D014, spt, 0x2C, 0, 0}), 0xC0000010u);
  CHECK_EQ(Call32(196, {z.h, 0, 0, 0, iosb, 0x4D014, spt, 0x2C, 0, 0}), 0xC0000010u);  // not the DVD
  CHECK(hp2k::vfs::MatchMask("*.*", "abc"));
  CHECK(hp2k::vfs::MatchMask("A?C.*", "abc.geo"));
  CHECK(!hp2k::vfs::MatchMask("*.xmv", "a.geo"));
  CHECK(hp2k::vfs::MatchMask("cs_*.geo", "CS_DY101_01.geo"));
}

void TestVblank() {
  uint64_t n0 = hp2_vblank_wait(hp2_vblank_count());
  auto t0 = std::chrono::steady_clock::now();
  uint64_t n = n0;
  for (int i = 0; i < 30; i++) n = hp2_vblank_wait(n);
  double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  CHECK_EQ(n - n0, 30u);
  CHECK(std::fabs(s - 0.5) < 0.03);
  CHECK(std::fabs(hp2_vblank_time(n0 + 60) - hp2_vblank_time(n0) - 1.0) < 1e-6);
  static std::atomic<int> cbs{0};
  int id = hp2_vblank_add_callback([](uint64_t, void*) { cbs++; }, nullptr);
  for (int i = 0; i < 5; i++) n = hp2_vblank_wait(n);
  hp2_vblank_remove_callback(id);
  CHECK(cbs.load() >= 4 && cbs.load() <= 6);
}

void TestSeh() {
  // Chain rec2 -> rec1 -> end; RtlUnwind(rec1) calls only rec2's handler.
  uint32_t top = g_main.esp;
  uint32_t rec1 = top - 0x100, rec2 = top - 0x200;
  std::atomic<int> h1{0}, h2{0}, unwinding{0};
  uint32_t fn1 = GuestFn([&](Ctx&, const uint32_t*, int) { h1++; return 1u; });
  uint32_t fn2 = GuestFn([&](Ctx&, const uint32_t* a, int) {
    unwinding = (R32(a[0] + 4) & 2) != 0;
    h2++;
    return 1u;
  });
  W32(rec1, 0xFFFFFFFF);
  W32(rec1 + 4, fn1);
  W32(rec2, rec1);
  W32(rec2 + 4, fn2);
  uint32_t pcr = g_main.fs_base, saved = R32(pcr);
  W32(pcr, rec2);
  g_main.esp = top - 0x300;
  CHECK_EQ(Call32(312, {rec1, 0xDEADBEEF, 0, 42}), 42u);  // TargetIp = return address
  CHECK_EQ(h2.load(), 1);
  CHECK_EQ(unwinding.load(), 1);
  CHECK_EQ(h1.load(), 0);
  CHECK_EQ(R32(pcr), rec1);
  W32(pcr, saved);
  g_main.esp = top;
}

void TestXInput() {
  size_t n;
  const Hp2Override* o = hp2_xinput_overrides(&n);
  CHECK_EQ(n, size_t(10));  // 8 XInput entry points + the game option defaults
  std::map<std::string, HostFn> fn;
  for (size_t i = 0; i < n; i++) fn[o[i].name] = o[i].fn;
  // Calls an override like lifted code would (pushes args + return address).
  auto call = [&](const char* name, std::initializer_list<uint32_t> args, uint32_t pops) {
    Ctx& c = ThreadCtx();
    uint32_t esp0 = c.esp, sp = esp0 - 4 * uint32_t(args.size()) - 4, i = 0;
    for (uint32_t a : args) W32(sp + 4 + 4 * i++, a);
    W32(sp, 0xDEADBEEF);
    c.esp = sp;
    fn[name](c);
    CHECK_EQ(c.esp - sp, 4 + 4 * pops);
    c.esp = esp0;
    return c.eax;
  };
  const uint32_t pad = 0x00176294, mu = 0x00176340;
  call("XInitDevices", {2, 0}, 2);
  CHECK_EQ(call("XGetDevices", {pad}, 1), 0u);
  Hp2PadState s;
  s.connected = true;
  hp2_xinput_set_pad(0, s);
  // Still enumerating after XInitDevices: the pad arrives as an insertion.
  CHECK_EQ(call("XGetDevices", {pad}, 1), 0u);
  uint32_t ins = Scratch(4), rem = Scratch(4);
  CHECK_EQ(call("XGetDeviceChanges", {pad, ins, rem}, 3), 1u);
  CHECK_EQ(R32(ins), 1u);
  CHECK_EQ(call("XGetDevices", {pad}, 1), 1u);
  CHECK_EQ(call("XGetDevices", {mu}, 1), 0u);
  hp2_xinput_set_pad(2, s);
  CHECK_EQ(call("XGetDeviceChanges", {pad, ins, rem}, 3), 1u);
  CHECK_EQ(R32(ins), 4u);
  CHECK_EQ(R32(rem), 0u);
  CHECK_EQ(call("XGetDeviceChanges", {pad, ins, rem}, 3), 0u);
  uint32_t h = call("XInputOpen", {pad, 0, 0, 0}, 4);
  CHECK(h != 0);
  CHECK_EQ(call("XInputOpen", {pad, 1, 0, 0}, 4), 0u);  // nothing in port 1
  uint32_t caps = Scratch(0x1C);
  CHECK_EQ(call("XInputGetCapabilities", {h, caps}, 2), 0u);
  CHECK_EQ(g_base[caps], 1);
  s.buttons = kPadStart | kPadDpadUp;
  s.analog[kPadA] = 255;
  s.ly = 32767;
  s.rx = -32768;
  hp2_xinput_set_pad(0, s);
  uint32_t st = Scratch(0x16);
  CHECK_EQ(call("XInputGetState", {h, st}, 2), 0u);
  uint32_t pkt = R32(st);
  CHECK_EQ(R32(st + 4) & 0xFFFF, uint32_t(kPadStart | kPadDpadUp));
  CHECK_EQ(g_base[st + 6], 255);  // A
  CHECK_EQ(int16_t(R32(st + 0x10) & 0xFFFF), int16_t(32767));  // LY
  CHECK_EQ(int16_t(R32(st + 0x12) & 0xFFFF), int16_t(-32768)); // RX
  hp2_xinput_set_pad(0, s);  // unchanged: same packet number
  CHECK_EQ(call("XInputGetState", {h, st}, 2), 0u);
  CHECK_EQ(R32(st), pkt);
  // Rumble: completes at once and signals the game's event.
  uint32_t ph = Scratch(4);
  Call32(189, {ph, 0, 0, 0});
  uint32_t fb = Scratch(0x48);
  W32(fb, 0x3E5);
  W32(fb + 4, R32(ph));
  W32(fb + 0x42, 0x8000 | 0x4000u << 16);
  CHECK_EQ(call("XInputSetState", {h, fb}, 2), 997u);
  CHECK_EQ(R32(fb), 0u);
  uint32_t to = Scratch(8);
  W64(to, 0);
  CHECK_EQ(Call32(233, {R32(ph), 0, to}), 0u);
  uint16_t l, r;
  hp2_xinput_get_rumble(0, &l, &r);
  CHECK_EQ(l, 0x8000);
  CHECK_EQ(r, 0x4000);
  s.connected = false;
  hp2_xinput_set_pad(0, s);
  CHECK_EQ(call("XInputGetState", {h, st}, 2), 1167u);
  CHECK_EQ(call("XGetDeviceChanges", {pad, ins, rem}, 3), 1u);
  CHECK_EQ(R32(rem), 1u);
  call("XInputClose", {h}, 1);
  // Keyboard-only (SDL backend): port 0 connected before the game's
  // XInitDevices still arrives as an insertion (the game's "live" flag).
  s = Hp2PadState{};
  s.connected = true;
  hp2_xinput_set_pad(0, s);
  hp2_xinput_set_pad(2, Hp2PadState{});
  call("XInitDevices", {2, 0}, 2);
  CHECK_EQ(call("XGetDevices", {pad}, 1), 0u);
  CHECK_EQ(call("XGetDeviceChanges", {pad, ins, rem}, 3), 1u);
  CHECK_EQ(R32(ins), 1u);
  CHECK_EQ(call("XGetDeviceChanges", {pad, ins, rem}, 3), 0u);  // stays connected
  hp2_xinput_set_pad(0, Hp2PadState{});
  call("XGetDeviceChanges", {pad, ins, rem}, 3);
}

void TestInputFilter() {
  using namespace hp2k::input;
  auto near = [](float a, float b) { return std::fabs(a - b) < 1e-3f; };
  // int16 <-> float: both ends are full deflection; no overflow at -32768.
  CHECK(AxisToFloat(-32768) == -1.0f);
  CHECK(AxisToFloat(32767) == 1.0f);
  CHECK(AxisToFloat(0) == 0.0f);
  CHECK_EQ(FloatToAxis(-1.0f), -32768);
  CHECK_EQ(FloatToAxis(1.0f), 32767);
  CHECK_EQ(FloatToAxis(2.0f), 32767);
  CHECK_EQ(FloatToAxis(0.0f), 0);
  InputSettings def;
  StickParams lin;  // linear, no dead zone
  lin.inner = 0.1f, lin.outer = 0.9f;
  float x, y;
  // Radial dead zone: inside -> 0, edges continuous, beyond outer -> 1.
  x = 0.09f, y = 0;
  ShapeStick(lin, x, y);
  CHECK(x == 0 && y == 0);
  x = 0.1001f, y = 0;
  ShapeStick(lin, x, y);
  CHECK(x >= 0 && x < 0.001f);  // no jump at the inner edge
  x = 0, y = 0.5f;
  ShapeStick(lin, x, y);
  CHECK(near(y, 0.5f));  // (0.5-0.1)/0.8
  x = -0.9f, y = 0;
  ShapeStick(lin, x, y);
  CHECK(near(x, -1.0f));
  x = AxisToFloat(-32768), y = 0;
  ShapeStick(lin, x, y);
  CHECK(near(x, -1.0f));
  x = 0.0f, y = 0.0f;
  ShapeStick(def.camera, x, y);
  CHECK(x == 0 && y == 0);
  // Round: a small diagonal is inside although each axis alone would be out.
  x = 0.08f, y = 0.08f;  // magnitude 0.113 < 0.12
  StickParams r12 = lin;
  r12.inner = 0.12f;
  ShapeStick(r12, x, y);
  CHECK(x == 0 && y == 0);
  // Diagonal normalisation: full corner -> magnitude 1, direction kept.
  x = 1, y = 1;
  ShapeStick(def.move, x, y);
  CHECK(near(std::sqrt(x * x + y * y), 1.0f) && near(x, y));
  x = AxisToFloat(-32768), y = AxisToFloat(32767);
  ShapeStick(def.camera, x, y);
  CHECK(std::sqrt(x * x + y * y) <= 1.0f + 1e-6f && x < 0 && y > 0);
  // Camera defaults: expo 1.6 and sensitivity Y 0.8.
  x = 1, y = 0;
  ShapeStick(def.camera, x, y);
  CHECK(near(x, 1.0f));
  x = 0, y = 1;
  ShapeStick(def.camera, x, y);
  CHECK(near(y, 0.8f));
  x = 0.515f, y = 0;  // half of the live range 0.08..0.95
  ShapeStick(def.camera, x, y);
  CHECK(near(x, std::pow(0.5f, 1.6f)));
  // Dual: continuous at the join, full at the end.
  StickParams dual = lin;
  dual.inner = 0, dual.outer = 1, dual.curve = Curve::kDual, dual.exponent = 2.0f;
  x = 0.75f, y = 0;
  ShapeStick(dual, x, y);
  CHECK(near(x, 0.5f));
  x = 1.0f, y = 0;
  ShapeStick(dual, x, y);
  CHECK(near(x, 1.0f));
  x = 0.375f, y = 0;
  ShapeStick(dual, x, y);
  CHECK(near(x, 0.125f));
  // Invert per axis, sensitivity clamps and the magnitude stays <= 1.
  StickParams inv = lin;
  inv.inner = 0, inv.outer = 1, inv.invert_y = true;
  x = 0.3f, y = 0.4f;
  ShapeStick(inv, x, y);
  CHECK(near(x, 0.3f) && near(y, -0.4f));
  inv.invert_y = false, inv.invert_x = true, inv.sens_x = 3.0f;
  x = 0.5f, y = 0;
  ShapeStick(inv, x, y);
  CHECK(near(x, -1.0f));
  x = 0.6f, y = 0.6f;
  ShapeStick(inv, x, y);
  CHECK(std::sqrt(x * x + y * y) <= 1.0f + 1e-6f);
  // Smoothing: frame-rate independent (one 16 ms step == four 4 ms steps),
  // settles to the target, off = pass-through.
  Smoother a, b;
  float ax = 0, ay = 0, bx = 0, by = 0;
  a.Apply(20, 0.004, ax, ay);
  b.Apply(20, 0.004, bx, by);
  ax = 1, ay = 0;
  a.Apply(20, 0.016, ax, ay);
  for (int i = 0; i < 4; i++) {
    bx = 1, by = 0;
    b.Apply(20, 0.004, bx, by);
  }
  CHECK(near(ax, bx));
  CHECK(near(ax, 1.0f - std::exp(-0.8f)));
  for (int i = 0; i < 100; i++) {
    ax = 1, ay = 0;
    a.Apply(20, 0.016, ax, ay);
  }
  CHECK(ax == 1.0f);
  Smoother off;
  float ox = 0.3f, oy = -0.2f;
  off.Apply(0, 0.016, ox, oy);
  ox = 0.9f;
  off.Apply(0, 0.016, ox, oy);
  CHECK(ox == 0.9f && oy == -0.2f);
  // Settings: defaults, the default file parses cleanly, values, warnings.
  CHECK(near(def.camera.inner, 0.08f) && near(def.camera.outer, 0.95f));
  CHECK(near(def.move.inner, 0.10f) && near(def.move.outer, 0.95f));
  CHECK(def.camera.curve == Curve::kExpo && near(def.camera.exponent, 1.6f));
  CHECK(near(def.camera.sens_x, 1.0f) && near(def.camera.sens_y, 0.8f));
  CHECK(near(def.camera.smoothing_ms, 8) && def.rumble_strength == 100);
  CHECK(def.camera_default_normal);
  CHECK(def.camera_modern && near(def.camera_yaw_speed, 330) && near(def.camera_recenter_delay, 4));
  InputSettings s;
  std::string w;
  ParseSettings(DefaultSettingsText(), s, &w);
  CHECK(w.empty());
  ParseSettings("[camera]\ninvert_y = true # comment\nsensitivity_x = 2.5\ncurve = \"dual\"\n"
                "sensitivity_y = 9\nbogus = 1\n[gamepad]\nnintendo_layout = 'label'\n"
                "rumble_strength = 40\n",
                s, &w);
  CHECK(s.camera.invert_y && near(s.camera.sens_x, 2.5f) && s.camera.curve == Curve::kDual);
  CHECK(near(s.camera.sens_y, 0.8f));  // out of range: kept
  CHECK(s.nintendo_layout == NintendoLayout::kLabel && s.rumble_strength == 40);
  ParseSettings("[camera]\ndefault_option = \"reverse\"\n", s, &w);
  CHECK(!s.camera_default_normal);
  ParseSettings("[camera]\nmodern = false\nyaw_speed = 400\nrecenter_delay = \"never\"\n", s, &w);
  CHECK(!s.camera_modern && near(s.camera_yaw_speed, 400) && s.camera_recenter_delay < 0);
  ParseSettings("[camera]\nyaw_speed = 5000\nrecenter_delay = 2.5\n", s, &w);
  CHECK(near(s.camera_yaw_speed, 400) && near(s.camera_recenter_delay, 2.5f));  // 5000: kept
  CHECK(w.find("yaw_speed = 5000") != std::string::npos);
  CHECK(w.find("sensitivity_y") != std::string::npos && w.find("bogus") != std::string::npos);
  w.clear();
  ApplyEnvOverrides(s, [](const char* n) -> const char* {
    if (!std::strcmp(n, "HP2_INPUT_CAMERA_INVERT_Y")) return "0";
    if (!std::strcmp(n, "HP2_INPUT_MOVE_DEADZONE_INNER")) return "0.2";
    if (!std::strcmp(n, "HP2_INPUT_GAMEPAD_RUMBLE_STRENGTH")) return "x";
    return nullptr;
  }, &w);
  CHECK(!s.camera.invert_y && near(s.move.inner, 0.2f) && s.rumble_strength == 40);
  CHECK(w.find("RUMBLE") != std::string::npos);
  // LoadSettings creates the commented default file in a fresh HOME.
  std::string home = g_tmp + "/input_home";
  mkdir(home.c_str(), 0755);
  std::string old_home = std::getenv("HOME") ? std::getenv("HOME") : "";
  setenv("HOME", home.c_str(), 1);
  InputSettings loaded = LoadSettings();
  CHECK(near(loaded.camera.sens_y, 0.8f));
  std::ifstream made(home + "/Library/Application Support/HP2/settings.toml");
  CHECK(made.good());
  setenv("HOME", old_home.c_str(), 1);
}

#if HP2_TEST_SDL
// SDL backend on virtual gamepads: mapping, shaping, hot-plug, active pad,
// Nintendo layout, rumble strength, input latency pipeline.
struct VirtualPad {
  SDL_JoystickID id = 0;
  SDL_Joystick* j = nullptr;
  uint16_t rumble[2] = {};
};
VirtualPad& AttachPad(const char* name, uint16_t vendor, uint16_t product) {
  VirtualPad* vp = new VirtualPad;  // userdata for the rumble callback (leaked: test)
  SDL_VirtualJoystickDesc d;
  SDL_INIT_INTERFACE(&d);
  d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
  d.vendor_id = vendor;
  d.product_id = product;
  d.naxes = SDL_GAMEPAD_AXIS_COUNT;
  d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
  d.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
  d.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
  d.name = name;
  d.userdata = vp;
  d.Rumble = [](void* u, Uint16 lo, Uint16 hi) {
    auto* v = static_cast<VirtualPad*>(u);
    v->rumble[0] = lo, v->rumble[1] = hi;
    return true;
  };
  vp->id = SDL_AttachVirtualJoystick(&d);
  vp->j = SDL_OpenJoystick(vp->id);
  return *vp;
}

void TestXInputSdl() {
  unsetenv("HP2_BACKGROUND");
  hp2_xinput_load_script(nullptr);
  std::string home = g_tmp + "/sdl_home";
  mkdir(home.c_str(), 0755);
  std::string old_home = std::getenv("HOME") ? std::getenv("HOME") : "";
  setenv("HOME", home.c_str(), 1);
  setenv("HP2_INPUT_CAMERA_SMOOTHING_MS", "0", 1);  // smoothing: TestInputFilter
  setenv("HP2_INPUT_GAMEPAD_RUMBLE_STRENGTH", "50", 1);
  setenv("HP2_INPUT_GAMEPAD_NINTENDO_LAYOUT", "label", 1);
  // Only virtual devices: no real pad on the test machine may interfere.
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
  SDL_SetHint(SDL_HINT_JOYSTICK_MFI, "0");
  SDL_SetHint(SDL_HINT_JOYSTICK_IOKIT, "0");
  CHECK(SDL_Init(SDL_INIT_EVENTS));
  CHECK(hp2_xinput_sdl_init());
  setenv("HOME", old_home.c_str(), 1);
  hp2_xinput_sdl_update();
  Hp2PadState p = hp2_xinput_get_pad(0);
  CHECK(p.connected);  // keyboard only: port 0 is there
  VirtualPad& xb = AttachPad("Virtual Xbox", 0x045E, 0x0B12);
  hp2_xinput_sdl_update();
  // Y flipped to up-positive, full range, camera sensitivity Y 0.8, X 1.0.
  SDL_SetJoystickVirtualAxis(xb.j, SDL_GAMEPAD_AXIS_RIGHTY, -32768);
  SDL_SetJoystickVirtualAxis(xb.j, SDL_GAMEPAD_AXIS_RIGHTX, 0);
  hp2_xinput_sdl_update();
  p = hp2_xinput_get_pad(0);
  CHECK_EQ(p.ry, hp2k::input::FloatToAxis(0.8f));
  CHECK_EQ(p.rx, 0);
  SDL_SetJoystickVirtualAxis(xb.j, SDL_GAMEPAD_AXIS_RIGHTY, 32767);
  SDL_SetJoystickVirtualAxis(xb.j, SDL_GAMEPAD_AXIS_RIGHTX, 32767);
  hp2_xinput_sdl_update();
  p = hp2_xinput_get_pad(0);
  CHECK(p.rx > 0 && p.ry < 0);
  CHECK(std::hypot(hp2k::input::AxisToFloat(p.rx), hp2k::input::AxisToFloat(p.ry)) <= 1.0001);
  // Left stick: full diagonal normalised (no square-gate boost); centre is 0.
  SDL_SetJoystickVirtualAxis(xb.j, SDL_GAMEPAD_AXIS_RIGHTY, 0);
  SDL_SetJoystickVirtualAxis(xb.j, SDL_GAMEPAD_AXIS_RIGHTX, 0);
  SDL_SetJoystickVirtualAxis(xb.j, SDL_GAMEPAD_AXIS_LEFTX, 32767);
  SDL_SetJoystickVirtualAxis(xb.j, SDL_GAMEPAD_AXIS_LEFTY, -32768);
  hp2_xinput_sdl_update();
  p = hp2_xinput_get_pad(0);
  CHECK(std::abs(p.lx - 23170) < 40 && std::abs(p.ly - 23170) < 40);
  CHECK(p.rx == 0 && p.ry == 0);
  SDL_SetJoystickVirtualAxis(xb.j, SDL_GAMEPAD_AXIS_LEFTX, 2000);  // inside 10%
  SDL_SetJoystickVirtualAxis(xb.j, SDL_GAMEPAD_AXIS_LEFTY, -2000);
  hp2_xinput_sdl_update();
  p = hp2_xinput_get_pad(0);
  CHECK(p.lx == 0 && p.ly == 0);
  SDL_SetJoystickVirtualButton(xb.j, SDL_GAMEPAD_BUTTON_SOUTH, true);
  hp2_xinput_sdl_update();
  CHECK_EQ(hp2_xinput_get_pad(0).analog[kPadA], 255);
  SDL_SetJoystickVirtualButton(xb.j, SDL_GAMEPAD_BUTTON_SOUTH, false);
  hp2_xinput_sdl_update();
  // Hot-plug a Switch Pro: it becomes active when used; "label" maps its
  // letters (east = A).
  VirtualPad& sw = AttachPad("Virtual Switch Pro", 0x057E, 0x2009);
  hp2_xinput_sdl_update();
  SDL_Gamepad* swg = SDL_GetGamepadFromID(sw.id);
  bool is_switch = swg && SDL_GetGamepadType(swg) == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO;
  if (!is_switch) std::printf("note: virtual 057E:2009 is not typed Switch Pro by SDL\n");
  SDL_SetJoystickVirtualButton(sw.j, SDL_GAMEPAD_BUTTON_EAST, true);
  hp2_xinput_sdl_update();
  p = hp2_xinput_get_pad(0);
  CHECK_EQ(p.analog[is_switch ? kPadA : kPadB], 255);
  CHECK_EQ(p.analog[is_switch ? kPadB : kPadA], 0);
  SDL_SetJoystickVirtualButton(sw.j, SDL_GAMEPAD_BUTTON_EAST, false);
  // The Xbox pad's stick moves: it is active again.
  SDL_SetJoystickVirtualAxis(xb.j, SDL_GAMEPAD_AXIS_LEFTX, 32767);
  SDL_SetJoystickVirtualAxis(xb.j, SDL_GAMEPAD_AXIS_LEFTY, 0);
  hp2_xinput_sdl_update();
  CHECK_EQ(hp2_xinput_get_pad(0).lx, 32767);
  // Rumble: the game's motors (port 0) at 50% to the active pad only.
  size_t n;
  const Hp2Override* o = hp2_xinput_overrides(&n);
  std::map<std::string, HostFn> fn;
  for (size_t i = 0; i < n; i++) fn[o[i].name] = o[i].fn;
  auto call = [&](const char* name, std::initializer_list<uint32_t> args) {
    Ctx& c = ThreadCtx();
    uint32_t esp0 = c.esp, sp = esp0 - 4 * uint32_t(args.size()) - 4, i = 0;
    for (uint32_t a : args) W32(sp + 4 + 4 * i++, a);
    W32(sp, 0xDEADBEEF);
    c.esp = sp;
    fn[name](c);
    c.esp = esp0;
    return c.eax;
  };
  const uint32_t padtype = 0x00176294;
  uint32_t ins = Scratch(4), rem = Scratch(4);
  call("XInitDevices", {2, 0});
  call("XGetDeviceChanges", {padtype, ins, rem});
  uint32_t h = call("XInputOpen", {padtype, 0, 0, 0});
  CHECK(h != 0);
  uint32_t fb = Scratch(0x48);
  W32(fb + 4, 0);
  W32(fb + 0x42, 0x40008000);  // left 0x8000, right 0x4000
  call("XInputSetState", {h, fb});
  hp2_xinput_sdl_update();
  SDL_UpdateJoysticks();
  CHECK_EQ(xb.rumble[0], 0x4000);
  CHECK_EQ(xb.rumble[1], 0x2000);
  CHECK_EQ(sw.rumble[0], 0);
  W32(fb + 0x42, 0);
  call("XInputSetState", {h, fb});
  hp2_xinput_sdl_update();
  CHECK_EQ(xb.rumble[0], 0);
  // Latency pipeline: device event -> port 0 -> the game's XInputGetState.
  uint32_t st = Scratch(0x16);
  call("XInputGetState", {h, st});
  Hp2InputLatency before = hp2_xinput_latency();
  for (int i = 0; i < 5; i++) {
    SDL_SetJoystickVirtualAxis(xb.j, SDL_GAMEPAD_AXIS_LEFTX, int16_t(i % 2 ? 32767 : -32768));
    hp2_xinput_sdl_update();
    CHECK_EQ(call("XInputGetState", {h, st}), 0u);
  }
  Hp2InputLatency after = hp2_xinput_latency();
  CHECK_EQ(after.count, before.count + 5);
  CHECK(after.max_us < 16667);
  // Unplug the active pad: port 0 stays connected, the other pad takes over.
  SDL_CloseJoystick(xb.j);
  SDL_DetachVirtualJoystick(xb.id);
  hp2_xinput_sdl_update();
  p = hp2_xinput_get_pad(0);
  CHECK(p.connected && p.lx == 0);
  SDL_SetJoystickVirtualButton(sw.j, SDL_GAMEPAD_BUTTON_START, true);
  hp2_xinput_sdl_update();
  CHECK(hp2_xinput_get_pad(0).buttons & kPadStart);
  SDL_CloseJoystick(sw.j);
  SDL_DetachVirtualJoystick(sw.id);
  hp2_xinput_sdl_update();
  CHECK(hp2_xinput_get_pad(0).connected);
  call("XInputClose", {h});
  unsetenv("HP2_INPUT_CAMERA_SMOOTHING_MS");
  unsetenv("HP2_INPUT_GAMEPAD_RUMBLE_STRENGTH");
  unsetenv("HP2_INPUT_GAMEPAD_NINTENDO_LAYOUT");
}
#endif

uint64_t g_fake_frame = 0;
uint64_t FakeFrame() { return g_fake_frame; }

void TestPadScript() {
  using namespace hp2k::xinput;
  std::vector<PadEvent> ev;
  std::string err;
  // Parser: every command, comments, relative frames, case-insensitivity.
  CHECK(ParsePadScript("# boot\n\n"
                       "100 press A\n"
                       "+20 hold dpad_down 30   # menu\n"
                       "150 stick L 0.0 1.0 60\n"
                       "150 trigger r 0.5\n"
                       "160 hold START\n"
                       "200 release start\n"
                       "210 release stickL\n"
                       "300 release all\n"
                       "400 disconnect\n"
                       "+10 connect\n",
                       &ev, &err));
  CHECK_EQ(ev.size(), size_t(10));
  CHECK_EQ(ev[0].frame, 100u);
  CHECK_EQ(ev[0].frames, 6u);  // press default
  CHECK_EQ(ev[1].frame, 120u);
  CHECK_EQ(ev[1].frames, 30u);
  CHECK(ev[1].text == "+20 hold dpad_down 30");
  CHECK_EQ(ev[4].frames, 0u);  // hold without duration: until release
  CHECK_EQ(ev[9].frame, 410u);
  CHECK_EQ(ev[9].line, 12);
  auto bad = [&](const char* text, const char* what) {
    bool ok = ParsePadScript(text, &ev, &err);
    CHECK(!ok);
    if (err.find(what) == std::string::npos) {
      std::printf("  unexpected error for \"%s\": %s\n", text, err.c_str());
      CHECK(false);
    }
  };
  bad("10 press Z", "line 1: unknown button");
  bad("10 press A\n5 press B", "line 2: frames must not decrease");
  bad("x press A", "bad frame");
  bad("10", "missing command");
  bad("10 jump", "unknown command");
  bad("10 stick L 1.5 0", "-1.0..1.0");
  bad("10 stick Q 0 0", "stick L|R");
  bad("10 hold A 0", "bad duration");
  bad("10 press A 5 6", "bad duration");
  bad("10 trigger L 2", "0.0..1.0");
  bad("10 release", "release takes");

  // Evaluation: a pure function of the frame.
  CHECK(ParsePadScript("10 press A\n"
                       "20 hold DOWN 30\n"
                       "30 stick L -1 1 5\n"
                       "40 hold B\n"
                       "45 trigger L 1\n"
                       "60 release B\n"
                       "70 hold X\n"
                       "80 release all\n"
                       "90 disconnect\n",
                       &ev, &err));
  Hp2PadState s = PadStateAt(ev, 0);
  CHECK(s.connected);
  CHECK_EQ(s.analog[kPadA], 0);
  CHECK_EQ(PadStateAt(ev, 10).analog[kPadA], 255);
  CHECK_EQ(PadStateAt(ev, 15).analog[kPadA], 255);
  CHECK_EQ(PadStateAt(ev, 16).analog[kPadA], 0);  // press = 6 frames
  CHECK_EQ(PadStateAt(ev, 20).buttons, uint32_t(kPadDpadDown));
  CHECK_EQ(PadStateAt(ev, 49).buttons, uint32_t(kPadDpadDown));
  CHECK_EQ(PadStateAt(ev, 50).buttons, 0u);
  s = PadStateAt(ev, 32);
  CHECK_EQ(s.lx, -32768);
  CHECK_EQ(s.ly, 32767);
  CHECK_EQ(PadStateAt(ev, 35).lx, 0);
  CHECK_EQ(PadStateAt(ev, 59).analog[kPadB], 255);
  CHECK_EQ(PadStateAt(ev, 59).analog[kPadLT], 255);
  CHECK_EQ(PadStateAt(ev, 60).analog[kPadB], 0);
  s = PadStateAt(ev, 75);
  CHECK_EQ(s.analog[kPadX], 255);
  CHECK_EQ(s.analog[kPadLT], 255);
  s = PadStateAt(ev, 80);
  CHECK_EQ(s.analog[kPadX], 0);
  CHECK_EQ(s.analog[kPadLT], 0);
  CHECK(s.connected);
  CHECK(!PadStateAt(ev, 90).connected);

  // Compact form (Render's tests/scripts/phase3_new_game.pad).
  CHECK(ParsePadScript("# menu\n450:A 456:-\n520:DOWN 524:-\n"
                       "6000:LY=32000 6090:-\n6150:LX=-32000+A+START 6180:-\n",
                       &ev, &err));
  CHECK_EQ(ev.size(), size_t(10));  // + implicit disconnect / connect at 1
  CHECK(!PadStateAt(ev, 0).connected);
  CHECK(PadStateAt(ev, 1).connected);
  CHECK_EQ(PadStateAt(ev, 449).analog[kPadA], 0);
  CHECK_EQ(PadStateAt(ev, 450).analog[kPadA], 255);
  CHECK_EQ(PadStateAt(ev, 455).analog[kPadA], 255);
  CHECK_EQ(PadStateAt(ev, 456).analog[kPadA], 0);
  CHECK_EQ(PadStateAt(ev, 520).buttons, uint32_t(kPadDpadDown));
  CHECK_EQ(PadStateAt(ev, 524).buttons, 0u);
  CHECK_EQ(PadStateAt(ev, 6000).ly, 32000);
  CHECK_EQ(PadStateAt(ev, 6089).ly, 32000);
  CHECK_EQ(PadStateAt(ev, 6090).ly, 0);
  s = PadStateAt(ev, 6150);
  CHECK_EQ(s.lx, -32000);
  CHECK_EQ(s.ly, 0);  // a set replaces the whole pad
  CHECK_EQ(s.buttons, uint32_t(kPadStart));
  CHECK_EQ(s.analog[kPadA], 255);
  bad("10:A 5:-", "frames must not decrease");
  bad("10:Z", "bad state");
  bad("10:LX=40000", "bad state");
  bad("x:A", "bad event");

  // Player: the overrides read the script at the frame clock; set_pad is ignored.
  size_t n;
  const Hp2Override* o = hp2_xinput_overrides(&n);
  std::map<std::string, HostFn> fn;
  for (size_t i = 0; i < n; i++) fn[o[i].name] = o[i].fn;
  auto call = [&](const char* name, std::initializer_list<uint32_t> args) {
    Ctx& c = ThreadCtx();
    uint32_t esp0 = c.esp, sp = esp0 - 4 * uint32_t(args.size()) - 4, i = 0;
    for (uint32_t a : args) W32(sp + 4 + 4 * i++, a);
    W32(sp, 0xDEADBEEF);
    c.esp = sp;
    fn[name](c);
    CHECK_EQ(c.esp - sp, 4 + 4 * uint32_t(args.size()));
    c.esp = esp0;
    return c.eax;
  };
  std::string path = g_tmp + "/pad.txt";
  WriteFile(path, "0 release all\n10 press A\n+5 hold DPAD_DOWN 2\n30 disconnect\n");
  CHECK(!hp2_xinput_load_script((g_tmp + "/missing.txt").c_str()));
  WriteFile(g_tmp + "/bad.txt", "10 frob\n");
  CHECK(!hp2_xinput_load_script((g_tmp + "/bad.txt").c_str()));
  CHECK(!hp2_xinput_script_active());
  hp2_xinput_set_frame_source(&FakeFrame);
  g_fake_frame = 0;
  CHECK(hp2_xinput_load_script(path.c_str()));
  CHECK(hp2_xinput_script_active());
  Hp2PadState real;
  real.connected = true;
  real.buttons = kPadStart;
  hp2_xinput_set_pad(0, real);  // e.g. the SDL backend: ignored
  const uint32_t pad = 0x00176294;
  call("XInitDevices", {2, 0});
  uint32_t ins = Scratch(4), rem = Scratch(4);
  CHECK_EQ(call("XGetDevices", {pad}), 0u);  // enumerating
  CHECK_EQ(call("XGetDeviceChanges", {pad, ins, rem}), 1u);
  CHECK_EQ(R32(ins) & 1, 1u);  // port 0 connected by the script
  uint32_t h = call("XInputOpen", {pad, 0, 0, 0});
  CHECK(h != 0);
  uint32_t st = Scratch(0x16);
  // frame -> (wButtons, A)
  const struct { uint64_t frame; uint32_t buttons; int a; } want[] = {
      {0, 0, 0}, {9, 0, 0}, {10, 0, 255}, {15, kPadDpadDown, 255},
      {16, kPadDpadDown, 0}, {17, 0, 0}, {29, 0, 0}};
  uint32_t pkt = 0;
  for (auto w : want) {
    g_fake_frame = w.frame;
    CHECK_EQ(call("XInputGetState", {h, st}), 0u);
    CHECK_EQ(R32(st + 4) & 0xFFFF, w.buttons);
    CHECK_EQ(g_base[st + 6], w.a);
    if (w.frame == 9) pkt = R32(st);
  }
  CHECK(R32(st) != pkt);  // packet number moves with the state
  g_fake_frame = 30;
  CHECK_EQ(call("XInputGetState", {h, st}), 1167u);
  // HP2_PAD_SCRIPT forms: "@file" and inline compact events.
  WriteFile(g_tmp + "/compact.pad", "5:B 7:-\n");
  CHECK(hp2_xinput_load_script(("@" + g_tmp + "/compact.pad").c_str()));
  g_fake_frame = 6;
  CHECK_EQ(call("XInputGetState", {h, st}), 0u);
  CHECK_EQ(g_base[st + 7], 255);  // B
  CHECK(hp2_xinput_load_script("3:START 4:-"));
  g_fake_frame = 3;
  CHECK_EQ(call("XInputGetState", {h, st}), 0u);
  CHECK_EQ(R32(st + 4) & 0xFFFF, uint32_t(kPadStart));
  CHECK(!hp2_xinput_load_script("@/nonexistent/x.pad"));
  CHECK(hp2_xinput_load_script(nullptr));
  CHECK(!hp2_xinput_script_active());
  hp2_xinput_set_frame_source(nullptr);
  call("XInputClose", {h});
}

uint64_t R64Time() { return Call(ThreadCtx(), 125, {}); }  // KeQueryInterruptTime

// HP2_TEST_CLOCK=frames only (the binary then runs just this group).
void TestTestClock() {
  // Boot (no Swap yet): with every counted thread blocked, vblanks are added
  // until the wait ends: a 50 ms delay = 3 vblanks, at once (no 2 s guard).
  uint64_t b0 = hp2_vblank_count();
  auto h0 = std::chrono::steady_clock::now();
  uint32_t bd = Scratch(8);
  W64(bd, uint64_t(-500000));
  Call32(99, {0, 0, bd});  // KeDelayExecutionThread on the counted main thread
  CHECK(std::chrono::steady_clock::now() - h0 < std::chrono::milliseconds(1000));
  CHECK_EQ(hp2_vblank_count(), b0 + 3);
  usleep(50000);  // running again: no more boot vblanks
  CHECK_EQ(hp2_vblank_count(), b0 + 3);
  hp2_vblank_wait(hp2_vblank_count());  // first Swap: from now on only Swaps advance
  uint64_t v0 = hp2_vblank_count();
  uint64_t t0 = R64Time();
  usleep(40000);  // 40 ms of host time, no Swap: at most the rest of one frame
  uint64_t t1 = R64Time();
  CHECK_EQ(t1, t0);  // kernel time moves only at vblanks
  CHECK_EQ(t0, v0 * 166667);
  uint64_t tsc1 = hp2_kernel_rdtsc(), tsc2 = hp2_kernel_rdtsc();
  CHECK(tsc1 > v0 * 166667 * 73 && tsc2 > tsc1);  // the TSC moves within the frame,
  CHECK(tsc2 - tsc1 >= 733 && tsc2 - tsc1 <= 734);   // 1 us per read (733.33 MHz)
  CHECK_EQ(hp2_vblank_count(), v0);
  CHECK_EQ(hp2_vblank_wait(v0), v0 + 1);      // a "Swap": one vblank, at once
  CHECK_EQ(hp2_vblank_wait(v0), v0 + 1);      // already past: no advance
  uint64_t t2 = R64Time();
  CHECK_EQ(t2, (v0 + 1) * 166667);
  CHECK_EQ(R32(hp2_kernel_data_va(156)), uint32_t((v0 + 1) * 166667 / 10000));  // KeTickCount, at once
  // A 50 ms kernel wait completes after 3 more frames, whoever presents them.
  std::thread swapper([&] {
    for (int i = 0; i < 4; i++) {
      usleep(5000);
      hp2_vblank_wait(hp2_vblank_count());
    }
  });
  uint32_t delay = Scratch(8);
  W64(delay, uint64_t(-500000));  // relative 50 ms
  Call32(99, {0, 0, delay});        // KeDelayExecutionThread
  uint64_t t3 = R64Time();
  CHECK(t3 >= t2 + 500000);
  swapper.join();
  CHECK_EQ(hp2_vblank_count(), v0 + 5);
}

}  // namespace

int main() {
  g_hpage = size_t(getpagesize());
  void* p = mmap(nullptr, 1ull << 32, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
  if (p == MAP_FAILED) {
    std::perror("mmap");
    return 2;
  }
  g_base = static_cast<uint8_t*>(p);
  Commit(0x10000, 0x5F0000, true);  // the "image"

  const char* tmpdir = std::getenv("TMPDIR");
  std::string tmpl = std::string(tmpdir ? tmpdir : "/tmp") + "/hp2k_test_XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back(0);
  if (!mkdtemp(buf.data())) return 2;
  g_tmp = buf.data();
  WriteFile(g_tmp + "/disc/_USA/Text.bin", "original");
  WriteFile(g_tmp + "/disc/_USA/Other.bin", "disc-only");
  WriteFile(g_tmp + "/disc/_USA/extra.dat", "x");
  WriteFile(g_tmp + "/mods/_usa/text.BIN", "modded");
  WriteFile(g_tmp + "/mods/_UKT/text.bin", "hybrid");  // the uk_text slot
  std::string disc = g_tmp + "/disc", mods = g_tmp + "/mods", data = g_tmp + "/data";

  Hp2KernelHost host;
  host.guest_base = g_base;
  host.commit = Commit;
  host.decommit = Decommit;
  host.call_guest = CallGuestFake;
  host.start_thread = StartThread;
  host.current_ctx = CurrentCtx;
  Hp2KernelConfig cfg;
  cfg.disc_dir = disc.c_str();
  cfg.mods_dirs = mods.c_str();
  cfg.data_dir = data.c_str();
  unsetenv("HP2_LANG");
  unsetenv("HP2_MODS");
  unsetenv("HP2_PAD_SCRIPT");
  if (!hp2_kernel_init(host, cfg)) return 2;
  hp2_kernel_thread_setup(g_main, 0x40000, 0x14);
  t_ctx = &g_main;

  if (const char* tc = std::getenv("HP2_TEST_CLOCK"); tc && !std::strcmp(tc, "frames")) {
    TestTestClock();
    std::printf("kernel tests (test clock): %d checks, %d failed\n", g_checks.load(), g_failed.load());
    std::fflush(stdout);
    std::_Exit(g_failed ? 1 : 0);
  }
  TestCoverage();
  TestMemory();
  TestStrings();
  TestSettings();
  TestTime();
  TestEvents();
  TestTimersAndDpcs();
  TestCriticalSection();
  TestThreads();
  TestFiles();
  TestVblank();
  TestSeh();
  TestXInput();
  TestPadScript();
  TestInputFilter();
#if HP2_TEST_SDL
  TestXInputSdl();
#endif

  // Crash report (last: one per process).
  setenv("HP2_CRASH_DIR", (g_tmp + "/crash").c_str(), 1);
  hp2_crash_note("runtime: a line from the runtime log");
  std::string rep = hp2_crash_report("test: deliberate report");
  CHECK(rep.find(g_tmp + "/crash/hp2-crash-") == 0);
  {
    std::ifstream in(rep);
    std::stringstream ss;
    ss << in.rdbuf();
    std::string txt = ss.str();
    CHECK(txt.find("Reason: test: deliberate report") != std::string::npos);
    CHECK(txt.find("language en") != std::string::npos);
    CHECK(txt.find("guest registers:") != std::string::npos);
    CHECK(txt.find("host backtrace:") != std::string::npos);
    CHECK(txt.find("runtime: a line from the runtime log") != std::string::npos);
    CHECK(txt.find("[krnl] first run: created profile") != std::string::npos);  // kernel log tail
  }
  CHECK(std::string(hp2_crash_report("second")) == rep);  // first fatal only

  std::string cleanup = "rm -rf '" + g_tmp + "'";
  if (system(cleanup.c_str()) != 0) std::fprintf(stderr, "cleanup failed\n");
  std::printf("kernel tests: %d checks, %d failed\n", g_checks.load(), g_failed.load());
  std::fflush(stdout);
  std::_Exit(g_failed ? 1 : 0);
}
