// Fixes for the game's own sound engine (0x114E00..0x11C400) where it relies
// on the Xbox having one CPU (owner: Audio).
//
// The sound thread (0x1176B0, time-critical priority) runs every 10 ms tick
// inside the engine's critical section 0x54B8D4 (0x1183A0 / 0x1183B0). On the
// Xbox, while that tick runs no other game thread runs at all, so engine code
// on the main thread may tear down state the tick reads without taking the
// lock, as long as it unpublishes before freeing. Here guest threads run in
// parallel, so the tick can be in the middle of reading what the main
// thread is freeing.
//
// Wrapped functions run their original body inside the critical section
// (recursive, so the body's own lock/unlock pairs still work).
//
// 0x115F10 StreamClose(int slot) (cdecl): calls the stop 0x115E10 (locked),
// then unlocked removes the per-tick service 0x115BA0, clears
// 0x54B4E0[slot] and frees the stream (0x12FDF0), while the tick's
// 0x115BA0 -> 0x115B30 may be reading that slot on another core. Lock order is
// the tick's own (0x54B8D4, then the stream library's 0x54AD7C in 0x114F00).
// Note: the observed access violation at 0x115B30 after Continue (NOTES.md)
// has a different cause (the loader reuses a still-open stream's memory).
//
// The other public stream-slot entries (slot table 0x54B4E0, the slot's
// segment list at +0x11C = slot[0x47] {head, tail, count}, current segment
// +0x134 = slot[0x4D]) are serialised the same way. The service 0x115BA0
// locks itself and runs from the game thread's pump 0x116FE0; the consume
// callback 0x1157B0 (removes finished segments) runs in the sound tick; both
// locked. Of the entries the game calls, only the stop 0x115E10 and 0x116790
// lock; the rest read or change the slot unlocked, e.g. QueueSegment
// 0x115CF0 hands the segment's file request to the stream reader (0x12F370)
// unlocked and only then appends the segment under the lock.
// Seen once (Kernel, pick_uk.pad, frame 3875, not reproduced in 24 runs):
// the service read a segment header while the slot had no segment for it,
// and 0x115880 wrote through a null segment (access violation at 0x115880,
// write to 0x18). The exact interleaving is unproven; the whole class is
// closed by running these bodies locked:
//   0x115CF0 QueueSegment      0x115FE0 Open (publishes the slot)
//   0x1155A0 / 0x1167C0 / 0x1168A0 setters (read the slot, poke its voice)
//   0x116350 / 0x116390 / 0x116470 queries (walk the segment list)
//   0x1199E0 reader seek       0x116FE0 pump (walks the service list
//                              that Open appends to)
// and by the guards below, which release a chunk that finds no segment
// instead of writing through null.
// Lock order stays the tick's: 0x54B8D4, then the reader's own locks.

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "dsound_internal.h"
#include "hle/kernel/kernel_exports.h"
#include "runtime/hp2_runtime.h"

namespace {

constexpr uint32_t kSoundLock = 0x54B8D4;
constexpr uint32_t kOrdRtlEnterCriticalSection = 277;
constexpr uint32_t kOrdRtlLeaveCriticalSection = 294;

void lock_call(Hp2Ctx& c, uint32_t ord) {
  uint32_t a = kSoundLock;
  hp2_call_guest(c, hp2_kernel_thunk_va(ord), &a, 1);
}

// Dev/test switches:
//   HP2_DS_NO_ENGINE_FIX=1      run the wrapped functions unlocked (as shipped)
//   HP2_DS_NO_SLOT_FIX=1        only the stream-slot entries run unlocked
//   HP2_DS_NO_JUMP_FIX=1        music section jumps stop the stream at once (as shipped)
//   HP2_DS_RACE_WIDEN_MS=<ms>   the tick stalls <ms> inside 0x115B30 with the
//                               slot in hand, widening the race window
//   HP2_DS_QUEUE_WIDEN_MS=<ms> QueueSegment stalls <ms> between its reader
//                               request and appending the segment
//   HP2_DS_TRACE_CLOSE=1        log every StreamClose with its caller
const bool g_no_fix = [] {
  const char* v = getenv("HP2_DS_NO_ENGINE_FIX");
  return v && atoi(v) != 0;
}();
const bool g_no_slot_fix = [] {
  const char* v = getenv("HP2_DS_NO_SLOT_FIX");
  return g_no_fix || (v && atoi(v) != 0);
}();
const bool g_no_jump_fix = [] {
  const char* v = getenv("HP2_DS_NO_JUMP_FIX");
  return g_no_fix || (v && atoi(v) != 0);
}();
const int g_widen_ms = [] {
  const char* v = getenv("HP2_DS_RACE_WIDEN_MS");
  return v ? atoi(v) : 0;
}();
const int g_queue_widen_ms = [] {
  const char* v = getenv("HP2_DS_QUEUE_WIDEN_MS");
  return v ? atoi(v) : 0;
}();
const bool g_trace_close = getenv("HP2_DS_TRACE_CLOSE") != nullptr;

// Runs the original body of VA in place (any calling convention: stack,
// ecx and edx are untouched; the body's own `ret` pops its frame) inside the
// sound critical section.
template <uint32_t VA, const bool& kOff = g_no_fix>
void Locked(Hp2Ctx& c) {
  if (kOff) return hp2_original(VA)(c);
  const uint32_t ecx = c.ecx, edx = c.edx;
  lock_call(c, kOrdRtlEnterCriticalSection);
  c.ecx = ecx;
  c.edx = edx;
  hp2_original(VA)(c);
  const uint32_t eax = c.eax, edx_out = c.edx;
  lock_call(c, kOrdRtlLeaveCriticalSection);
  c.eax = eax;
  c.edx = edx_out;
}

void StreamClose(Hp2Ctx& c) {
  if (g_trace_close) {
    uint32_t slot = hp2_arg(c, 0);
    hp2_log("dsound: StreamClose(%u) slot %08x from %08x", slot,
            slot < 8 ? hp2_rd32(0x54B4E0 + 4 * slot) : 0, hp2_rd32(c.esp));
  }
  Locked<0x115F10>(c);
}

HP2_OVERRIDE(0x115F10, StreamClose);

// The speech/music cue subsystem (0x124000..0x127000): every public entry
// guards itself with a busy flag, `if (DF18 == 0 && enter(-1))` (0x1240A0
// sets DF18/DF0C, the entry clears them on exit), and silently does nothing
// when it finds the flag set. On one CPU a caller never finds another thread
// halfway inside; here it can. Seen: the main thread's "stop and free all
// music" 0x1C940 -> 0x126B10 found the subsystem busy, closed nothing, then
// freed the music memory; the level loader streamed Speech_Burrows.sdt over
// the still-open frontend stream and the sound tick read bank bytes as a
// pointer (access violation at 0x115B30, ~1 in 16 loads under CPU load).
// All entries that take the flag (every writer of 0x54DF18) are serialised on
// the sound critical section.
//
// Section jumps (interactive map): at a jump the update 0x125F00 -> 0x125A70 ->
// 0x1249D0 first takes the map's "end" branch and stops the music stream
// (0x1C860 called at 0x124D29), and only on a later update (50 ms later in
// the paced menu, clock 0x54AE98 2561 -> 2566) resolves the next section and
// queues it "immediate" (0x1CD40 -> QueueSegment(slot, -1, ...)). The music
// decoder runs dry in between: ~50 ms of silence at every jump (measured in
// the mix, tests/music_jump_gaps.sh). That stop is deferred: it runs when the
// engine next resolves a section (0x1249D0 entry), at any other cue entry,
// or at the first update 100 ms after it was asked for, whichever is first.
// The old section keeps sounding until the new one is queued.
constexpr uint32_t kStopAtJump = 0x124D2E;  // return address of that 0x1C860 call
constexpr uint32_t kClock = 0x54AE98;       // 100 Hz sound timer ticks
constexpr int kStopDeadlineTicks = 10;
uint32_t g_pending_stop[32];
std::atomic<bool> g_pump_on_tick{false};  // stream index -> clock + 1 of the deferred stop (0 = none)

void flush_stops(Hp2Ctx& c, bool deadline_only) {
  const uint32_t now = hp2_rd32(kClock);
  for (uint32_t i = 0; i < 32; ++i) {
    if (!g_pending_stop[i] || (deadline_only && int(now - (g_pending_stop[i] - 1)) < kStopDeadlineTicks))
      continue;
    g_pending_stop[i] = 0;
    hp2_call_guest(c, 0x1C860, &i, 1);
  }
}

void StopStream(Hp2Ctx& c) {  // 0x1C860(int stream)
  const uint32_t i = hp2_arg(c, 0);
  if (!g_no_jump_fix && i < 32 && hp2_rd32(c.esp) == kStopAtJump) {
    g_pending_stop[i] = hp2_rd32(kClock) + 1;
    return hp2_ret_cdecl(c, 0);
  }
  if (i < 32) g_pending_stop[i] = 0;
  hp2_original(0x1C860)(c);
}

void ResolveSection(Hp2Ctx& c) {  // 0x1249D0, inside the update (locked)
  if (!g_no_jump_fix) {
    const uint32_t ecx = c.ecx, edx = c.edx;
    flush_stops(c, false);
    c.ecx = ecx;
    c.edx = edx;
  }
  hp2_original(0x1249D0)(c);
}

// Locked<VA> that first runs the deferred stops (all of them, or the update's
// overdue ones), inside the lock.
template <uint32_t VA, bool kDeadlineOnly = false>
void CueEntry(Hp2Ctx& c) {
  if (g_no_fix) return hp2_original(VA)(c);
  const uint32_t ecx = c.ecx, edx = c.edx;
  lock_call(c, kOrdRtlEnterCriticalSection);
  flush_stops(c, kDeadlineOnly);
  c.ecx = ecx;
  c.edx = edx;
  hp2_original(VA)(c);
  const uint32_t eax = c.eax, edx_out = c.edx;
  lock_call(c, kOrdRtlLeaveCriticalSection);
  c.eax = eax;
  c.edx = edx_out;
}

HP2_OVERRIDE(0x1C860, StopStream);
HP2_OVERRIDE(0x1249D0, ResolveSection);
HP2_OVERRIDE(0x125F00, (CueEntry<0x125F00, true>));
HP2_OVERRIDE(0x125FE0, CueEntry<0x125FE0>);
HP2_OVERRIDE(0x126290, CueEntry<0x126290>);
HP2_OVERRIDE(0x126610, CueEntry<0x126610>);
HP2_OVERRIDE(0x126710, CueEntry<0x126710>);
HP2_OVERRIDE(0x126800, CueEntry<0x126800>);
HP2_OVERRIDE(0x126B10, CueEntry<0x126B10>);
HP2_OVERRIDE(0x126C40, CueEntry<0x126C40>);
HP2_OVERRIDE(0x126D60, CueEntry<0x126D60>);
HP2_OVERRIDE(0x126E20, CueEntry<0x126E20>);
HP2_OVERRIDE(0x126F10, CueEntry<0x126F10>);

// Stream-slot entries (see the header).
HP2_OVERRIDE(0x1155A0, (Locked<0x1155A0, g_no_slot_fix>));
HP2_OVERRIDE(0x116350, (Locked<0x116350, g_no_slot_fix>));
HP2_OVERRIDE(0x116390, (Locked<0x116390, g_no_slot_fix>));
HP2_OVERRIDE(0x116470, (Locked<0x116470, g_no_slot_fix>));
HP2_OVERRIDE(0x1167C0, (Locked<0x1167C0, g_no_slot_fix>));
HP2_OVERRIDE(0x1168A0, (Locked<0x1168A0, g_no_slot_fix>));
HP2_OVERRIDE(0x1199E0, (Locked<0x1199E0, g_no_slot_fix>));
HP2_OVERRIDE(0x116FE0, (Locked<0x116FE0, g_no_slot_fix>));

// Guards at the fault site (both called from the service 0x115BA0, under the
// lock): a stream chunk that arrives while the slot has no segment for it is
// released the way the game releases chunks it does not want (0x12F980), not
// written through a null segment.
//   0x115880 Header(int slot, chunk): next segment = cur ? cur->next : head
//   0x115A60 Data(slot*, chunk):      segment = cur (+0x134)
std::atomic<int> g_orphan_chunks{0};

void drop_chunk(Hp2Ctx& c, uint32_t s, uint32_t chunk, const char* what) {
  const int n = ++g_orphan_chunks;
  if (n <= 10 || n % 1000 == 0)
    hp2_log("dsound: stream slot %08x: %s chunk with no queued segment (cur %08x head %08x count %u), "
            "dropped (%d so far)", s, what, hp2_rd32(s + 0x134), hp2_rd32(s + 0x11C),
            hp2_rd32(s + 0x124), n);
  const uint32_t args[2] = {hp2_rd32(s), chunk};
  hp2_call_guest(c, 0x12F980, args, 2);
  hp2_ret_cdecl(c, 0);
}

void StreamHeader(Hp2Ctx& c) {
  const uint32_t s = hp2_rd32(0x54B4E0 + 4 * hp2_arg(c, 0));
  const uint32_t cur = hp2_rd32(s + 0x134);
  if (!g_no_slot_fix && !(cur ? hp2_rd32(cur) : hp2_rd32(s + 0x11C)))
    return drop_chunk(c, s, hp2_arg(c, 1), "header");
  hp2_original(0x115880)(c);
}

void StreamData(Hp2Ctx& c) {
  const uint32_t s = hp2_arg(c, 0);
  if (!g_no_slot_fix && !hp2_rd32(s + 0x134)) return drop_chunk(c, s, hp2_arg(c, 1), "data");
  hp2_original(0x115A60)(c);
}

HP2_OVERRIDE(0x115A60, StreamData);

// 0x1157B0 Consume(?, n, slot*), the decoder's "n bytes used" callback (sound
// tick, locked): takes n off the head segment's remaining (+0x1C), retires
// segments that are used up and carries the rest to the next one, without a
// null check: n larger than all queued segments hold walks off the list end
// (C0000005 reading 0x1C, Diagon Alley wander frame 43862 on bb5e726). n is
// clamped to what the list holds; nothing queued = nothing to consume.
std::atomic<int> g_overrun_consumes{0};

void StreamConsume(Hp2Ctx& c) {
  const uint32_t s = hp2_arg(c, 2), n = hp2_arg(c, 1);
  if (!g_no_slot_fix) {
    uint64_t held = 0;
    int nodes = 0;
    for (uint32_t seg = hp2_rd32(s + 0x11C); seg && nodes < 256; seg = hp2_rd32(seg), ++nodes)
      held += hp2_rd32(seg + 0x1C);
    if (n > held) {
      const int k = ++g_overrun_consumes;
      if (k <= 10 || k % 1000 == 0)
        hp2_log("dsound: stream slot %08x: consume %u bytes, segments hold %llu (%d queued), clamped (%d so far)",
                s, n, (unsigned long long)held, nodes, k);
      if (!nodes) return hp2_ret_cdecl(c, 0);
      hp2_wr32(c.esp + 8, uint32_t(held));
    }
  }
  hp2_original(0x1157B0)(c);
}

// Stream stress (HP2_DS_STREAM_STRESS=<n>): once the game has opened and
// queued its first stream, a second guest thread replays that pair n times
// on its own slot memory while the sound tick and the game's pump service
// it: open, then the game's first (up to 4) segment requests on that slot,
// each followed by a random wait of up to 300 ms and a status query, then
// close. A segment queued after the previous one is fully read but still
// playing is the case that crashed in 0x115880 (see the header).
// Logs "stream stress: done <n>" at the end; HP2_DS_STREAM_STRESS_EXIT=1
// then exits the run (code 0).
const int g_stress_n = [] {
  const char* v = getenv("HP2_DS_STREAM_STRESS");
  return v ? atoi(v) : 0;
}();

struct StressArgs {
  uint32_t open[7];   // Open 0x115FE0 args; [3] = slot memory, [4] = its size
  uint32_t queue[4][5];  // the first QueueSegment 0x115CF0 calls on that slot
  int nqueue = 0;
  uint32_t open_slot = ~0u;
  bool have_open = false, started = false;
};
StressArgs g_stress;

// A copy of `n` bytes at `va` in HLE memory when `va` looks like a guest
// pointer (the game's own may be a stack local or freed later), else `va`.
uint32_t guest_copy(uint32_t va, uint32_t n) {
  if (va < 0x10000 || va >= 0x04000000) return va;
  const uint32_t to = hp2_hle_alloc(n);
  std::memcpy(hp2_ptr(to), hp2_ptr(va), n);
  return to;
}

// Guest thread start routine (stdcall, one arg), created through the game's
// XAPI CreateThread 0x111E05 so the file library finds its per-thread data.
void stress_body(Hp2Ctx& c) {
  StressArgs a = g_stress;
  a.open[3] = hp2_hle_alloc(a.open[4], 64);
  const uint32_t status = hp2_hle_alloc(16);
  std::this_thread::sleep_for(std::chrono::seconds(15));  // let the game queue more
  a.nqueue = g_stress.nqueue;
  std::memcpy(a.queue, g_stress.queue, sizeof a.queue);
  hp2_log("dsound: stream stress: %d iterations (open size %x, %d segment requests)", g_stress_n,
          a.open[4], a.nqueue);
  uint32_t rng = 12345;
  auto rnd = [&](uint32_t n) { rng = rng * 1103515245u + 12345u; return (rng >> 16) % n; };
  int opened = 0, failed = 0;
  for (int i = 0; i < g_stress_n; ++i) {
    std::memset(hp2_ptr(a.open[3]), 0, a.open[4]);
    const int32_t slot = int32_t(hp2_call_guest(c, 0x115FE0, a.open, 7));
    if (slot < 0) {
      ++failed;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }
    ++opened;
    for (int k = 0; k < a.nqueue; ++k) {
      uint32_t q[5] = {uint32_t(slot), a.queue[k][1], a.queue[k][2], a.queue[k][3], a.queue[k][4]};
      const int32_t seg = int32_t(hp2_call_guest(c, 0x115CF0, q, 5));
      if (i == 0) hp2_log("dsound: stream stress: slot %d segment %d", slot, seg);
      std::this_thread::sleep_for(std::chrono::microseconds(rnd(300000)));
      uint32_t st[2] = {uint32_t(slot), status};
      hp2_call_guest(c, 0x116470, st, 2);
    }
    uint32_t cl = uint32_t(slot);
    hp2_call_guest(c, 0x115F10, &cl, 1);
    if ((i + 1) % 100 == 0) hp2_log("dsound: stream stress: %d / %d", i + 1, g_stress_n);
  }
  hp2_log("dsound: stream stress: done %d (opened %d, slot full %d)", g_stress_n, opened, failed);
  const char* e = getenv("HP2_DS_STREAM_STRESS_EXIT");
  if (e && atoi(e)) hp2_exit(0, "stream stress done");
  hp2_ret_stdcall(c, 0, 1);
}

void Open(Hp2Ctx& c) {
  const bool capture = g_stress_n > 0 && !g_stress.have_open;
  uint32_t args[7];
  if (capture) {
    for (int i = 0; i < 7; ++i) args[i] = hp2_arg(c, i);
    args[0] = guest_copy(args[0], 24);  // params block, often on the stack
  }
  Locked<0x115FE0, g_no_slot_fix>(c);
  if (capture && int32_t(c.eax) >= 0) {
    std::memcpy(g_stress.open, args, sizeof args);
    g_stress.open_slot = c.eax;
    g_stress.have_open = true;
  }
}

void QueueSegment(Hp2Ctx& c) {
  if (g_trace_close) {
    const uint32_t a2 = hp2_arg(c, 2);
    hp2_log("dsound: QueueSegment(%d, %d, %08x \"%.32s\", %u, %u) from %08x thread %u mix %llu clock %u",
            int32_t(hp2_arg(c, 0)),
            int32_t(hp2_arg(c, 1)), a2, a2 >= 0x10000 && a2 < 0x04000000 ? (const char*)hp2_ptr(a2) : "",
            hp2_arg(c, 3), hp2_arg(c, 4), hp2_rd32(c.esp), hp2_current_thread()->id,
            (unsigned long long)hp2ds::g_ds.mixed_frames, hp2_rd32(0x54AE98));
  }
  if (g_stress.have_open && g_stress.nqueue < 4 && hp2_arg(c, 0) == g_stress.open_slot) {
    uint32_t* q = g_stress.queue[g_stress.nqueue++];
    for (int i = 1; i < 5; ++i) q[i] = guest_copy(hp2_arg(c, i), 256);
  }
  if (g_stress.nqueue && !g_stress.started) {
    g_stress.started = true;
    const uint32_t tid = hp2_hle_alloc(4);
    const uint32_t ct[6] = {0, 0x10000, hp2_host_thunk(stress_body, "StreamStress"), 0, 0, tid};
    hp2_call_guest(c, 0x111E05, ct, 6);  // CreateThread
  }
  Locked<0x115CF0, g_no_slot_fix>(c);
}

HP2_OVERRIDE(0x115FE0, Open);
HP2_OVERRIDE(0x115CF0, QueueSegment);

void StreamSliceWiden(Hp2Ctx& c) {
  if (g_widen_ms > 0 && hp2_rd32(hp2_arg(c, 0) + 0x134))
    std::this_thread::sleep_for(std::chrono::milliseconds(g_widen_ms));
  hp2_original(0x115B30)(c);
}

// QueueSegment's reader request (0x12F370 / 0x12F430, called before it
// takes the lock): stall after it so a service can see the new segment's
// header before the segment is on the slot's list.
template <uint32_t VA>
void ReaderRequestWiden(Hp2Ctx& c) {
  const uint32_t ret = hp2_rd32(c.esp);
  hp2_original(VA)(c);
  if (ret == 0x115D4A || ret == 0x115D6A) {
    const uint32_t eax = c.eax;
    std::this_thread::sleep_for(std::chrono::milliseconds(g_queue_widen_ms));
    c.eax = eax;
  }
}

const bool g_widen_registered = [] {
  if (g_widen_ms > 0)
    hp2_register_override_at_start(0x115B30, StreamSliceWiden, "StreamSliceWiden");
  if (g_queue_widen_ms > 0) {
    hp2_register_override_at_start(0x12F370, ReaderRequestWiden<0x12F370>, "ReaderRequestWiden");
    hp2_register_override_at_start(0x12F430, ReaderRequestWiden<0x12F430>, "ReaderRequestWiden");
  }
  return true;
}();

}  // namespace

namespace {
void ProbeHeader(Hp2Ctx& c) {
  const uint32_t idx = hp2_arg(c, 0), s = hp2_rd32(0x54B4E0 + 4 * idx);
  const uint32_t cur = hp2_rd32(s + 0x134), head = hp2_rd32(s + 0x11C);
  hp2_log("probe: 115880 slot %u cur %08x next %08x head %08x count %u thread %u mix %llu", idx, cur,
          cur ? hp2_rd32(cur) : 0, head, hp2_rd32(s + 0x124), hp2_current_thread()->id,
          (unsigned long long)hp2ds::g_ds.mixed_frames);
  StreamHeader(c);
}
void ProbeConsume(Hp2Ctx& c) {  // 0x1157B0(?, n, slot)
  const uint32_t s = hp2_arg(c, 2), head = hp2_rd32(s + 0x11C);
  hp2_log("probe: consume n %u head %08x [14]=%u [18]=%u [1C]=%u count %u cur %08x thread %u mix %llu",
          hp2_arg(c, 1), head, head ? hp2_rd32(head + 0x14) : 0, head ? hp2_rd32(head + 0x18) : 0,
          head ? hp2_rd32(head + 0x1C) : 0, hp2_rd32(s + 0x124), hp2_rd32(s + 0x134),
          hp2_current_thread()->id);
  StreamConsume(c);
}
void ProbeService(Hp2Ctx& c) {  // 0x115BA0 / 0x1190C0 entry
  hp2_log("probe: %s thread %u", hp2_rd32(c.esp) == 0x115C1F ? "drain" : "x", hp2_current_thread()->id);
  hp2_original(0x1190C0)(c);
}
template <uint32_t VA>
void ProbeCall(Hp2Ctx& c) {
  hp2_log("probe: call %06x(%x, %x, %x) from %08x thread %u mix %llu clock %u", VA, hp2_arg(c, 0),
          hp2_arg(c, 1), hp2_arg(c, 2), hp2_rd32(c.esp), hp2_current_thread()->id,
          (unsigned long long)hp2ds::g_ds.mixed_frames, hp2_rd32(0x54AE98));
  const uint32_t m = hp2_rd32(0x54DF0C);
  if (m)
    hp2_log("probe:   map [12]=%08x [13]=%08x [14]=%u [15]=%u [16]=%x [3]=%u [4]=%u df1c %u", hp2_rd32(m + 0x48),
            hp2_rd32(m + 0x4C), hp2_rd32(m + 0x50), hp2_rd32(m + 0x54), hp2_rd32(m + 0x58), hp2_rd32(m + 12),
            hp2_rd32(m + 16), hp2_rd32(0x54DF1C));
  hp2_original(VA)(c);
  hp2_log("probe:   %06x -> %d", VA, int32_t(c.eax));
}
const bool g_probe = [] {
  hp2_register_override_at_start(0x115880, getenv("HP2_DS_PROBE") ? ProbeHeader : StreamHeader,
                                 "StreamHeader");
  hp2_register_override_at_start(0x1157B0, getenv("HP2_DS_PROBE") ? ProbeConsume : StreamConsume,
                                 "StreamConsume");
  if (getenv("HP2_DS_PROBE")) {
    hp2_register_override_at_start(0x1190C0, ProbeService, "ProbeService");
    if (getenv("HP2_DS_PROBE_MUSIC")) {
      hp2_register_override_at_start(0x1C700, ProbeCall<0x1C700>, "Probe1C700");
      hp2_register_override_at_start(0x1CD40, ProbeCall<0x1CD40>, "Probe1CD40");
      hp2_register_override_at_start(0x125070, ProbeCall<0x125070>, "Probe125070");
      hp2_register_override_at_start(0x124DD0, ProbeCall<0x124DD0>, "Probe124DD0");
    }
  }
  return true;
}();
}  // namespace
