// Fixes for limits of the original game that the port can hit, plus the
// diagnostics that show them. Overrides only: no lift change.
//
// Component heap ("NASHEAP_TABLE"): every level object's components (AI
// state, collision, ...) come from one free-list heap of 250 KB (engine
// config 0x1EF6F4[2] = 250 KB, reset per level by 0x461C0). A guard takes
// ~22 KB (0x5224 AI block, 0xC20C0) + ~1 KB; Pre Library spawns 16 of them,
// which exhausts the heap: later components come back null and the guard
// line-of-sight tick (0xB9DF0) writes through the null component list
// (C0000005 at 0x88). The heap is created once at startup (0x46170); it is
// placed in HLE memory (outside the game's 40 MB arena, so the arena keeps
// its full budget) at kComponentHeapKb, 4x the original.
#include <cstdint>
#include <cstdlib>

#include "runtime/hp2_runtime.h"

namespace {

constexpr uint32_t kComponentHeapInit = 0x46170;   // (size): allocate + init
constexpr uint32_t kComponentHeapReset = 0x461C0;  // per level: re-init
constexpr uint32_t kComponentHeapFormat = 0x45E40; // (base, size)
constexpr uint32_t kComponentAlloc = 0x45ED0;      // (obj, size, type, template) -> 0 on failure
constexpr uint32_t kObjectSlot = 0x46380;         // (index or -1) -> 0xDC-byte object, 0 when none free
constexpr uint32_t kArenaAlloc = 0x35C40;          // (size, name, ...) -> 0 on failure
constexpr uint32_t kHeapSize = 0x5248C0, kHeapBase = 0x5248E4, kHeapFree = 0x5248D0;
constexpr uint32_t kComponentHeapKb = 1024;

uint32_t g_failures = 0;  // component allocations that failed in this level
uint32_t g_peak = 0;      // most bytes in use at once in this level

uint32_t FreeBytes() {
  uint32_t total = 0, n = 0;
  for (uint32_t b = hp2_rd32(kHeapFree); b && n < 1u << 20; b = hp2_rd32(b + 0xC), ++n)
    total += hp2_rd32(b + 0x10) & ~3u;
  return total;
}

void ComponentHeapInit(Hp2Ctx& c) {
  uint32_t size = (hp2_arg(c, 0) + 3) & ~3u;
  uint32_t want = kComponentHeapKb;
  if (const char* e = std::getenv("HP2_COMPONENT_HEAP_KB")) want = uint32_t(std::strtoul(e, nullptr, 0));
  if (want * 1024 > size) size = want * 1024;
  const uint32_t base = hp2_hle_alloc(size, 0x80);
  hp2_wr32(kHeapSize, size);
  hp2_wr32(kHeapBase, base);
  const uint32_t args[2] = {base, size};
  hp2_call_guest(c, kComponentHeapFormat, args, 2);
  hp2_log("hp2: component heap %u KB at %08X (game asked %u KB)", size / 1024, base,
          hp2_arg(c, 0) / 1024);
  hp2_ret_cdecl(c, base);
}

void ComponentHeapReset(Hp2Ctx& c) {
  static const HostFn original = hp2_original(kComponentHeapReset);
  const uint32_t size = hp2_rd32(kHeapSize);
  if (g_peak || g_failures)
    hp2_log("hp2: component heap peak in the level: %u of %u KB, %u failed allocations",
            g_peak / 1024, size / 1024, g_failures);
  g_failures = g_peak = 0;
  original(c);
}

void ComponentAlloc(Hp2Ctx& c) {
  static const HostFn original = hp2_original(kComponentAlloc);
  const uint32_t obj = hp2_arg(c, 0), size = hp2_arg(c, 1), type = hp2_arg(c, 2),
                 from = hp2_rd32(c.esp);
  original(c);
  const uint32_t used = hp2_rd32(kHeapSize) - FreeBytes();
  // Past the original 250 KB, every 64 KB step is logged (the last level has no reset).
  if (used >= 200 * 1024 && used / 0x10000 > g_peak / 0x10000)
    hp2_log("hp2: component heap in use: %u KB", used / 1024);
  if (used > g_peak) g_peak = used;
  if (c.eax == 0 && g_failures++ < 8)
    hp2_log("hp2: component heap full: object %08X wants %X bytes (type %08X, from %08X), %X free",
            obj, size, type, from, FreeBytes());
}

// Level objects come from a fixed array (engine config 0x1EF6F4[1] = 250 slots).
void ObjectSlot(Hp2Ctx& c) {
  static const HostFn original = hp2_original(kObjectSlot);
  const uint32_t index = hp2_arg(c, 0), from = hp2_rd32(c.esp);
  original(c);
  static uint32_t n = 0;
  if (c.eax == 0 && n++ < 16)
    hp2_log("hp2: object slots full: no free object for index %d (from %08X)", int32_t(index), from);
}

void ArenaAlloc(Hp2Ctx& c) {
  static const HostFn original = hp2_original(kArenaAlloc);
  const uint32_t size = hp2_arg(c, 0), name = hp2_arg(c, 1), from = hp2_rd32(c.esp);
  original(c);
  static uint32_t n = 0;
  if (c.eax == 0 && n++ < 16) {
    char s[33] = {};
    for (int i = 0; i < 32 && name > 0x10000 && name < 0x80000000u; ++i)
      if (!(s[i] = char(hp2_rd8(name + i)))) break;
    hp2_log("hp2: game arena allocation failed: %X bytes for '%s' (from %08X)", size, s, from);
  }
}

}  // namespace

HP2_OVERRIDE(kComponentHeapInit, ComponentHeapInit);
HP2_OVERRIDE(kComponentHeapReset, ComponentHeapReset);
HP2_OVERRIDE(kComponentAlloc, ComponentAlloc);
HP2_OVERRIDE(kObjectSlot, ObjectSlot);
HP2_OVERRIDE(kArenaAlloc, ArenaAlloc);

// Referenced by the dispatch setup so the linker keeps this file.
void hp2_game_fixes_link() {}
