// Skippable movies and cutscenes (Polish), on by default; HP2_SKIP=0 turns
// both off (the game as shipped). Scripted and silent test runs
// (HP2_PAD_SCRIPT or HP2_BACKGROUND=1) keep the game as shipped unless
// HP2_SKIP=1: their pad routes are timed by frame and hold A through
// dialogue, so a fast-forward would shift the game ahead of the route.
//
// The rules (user, 2026-11-01):
// - Movies (the EA / Warner logos and the intro, all before the main menu;
//   the game has no other FMV): a single press of A or START (keyboard Space
//   / Enter) skips — no game action to collide with there.
// - In-game cutscenes and voice lines: A or START held for 4 seconds (any
//   pad). A short press never skips — A also talks, hides, jumps. The 4 s
//   count from the later of "the scene started" and "the button went down",
//   so a press that started a game action before the scene does not count;
//   until then the game runs as shipped (no developer skip bit, no faked
//   buttons). Wall time.
//
// Movies:
// The game's movie player PlayMovie(path, skippable) (0x18D40, cdecl) ends
// a movie on a fresh A or START press (bits 0x40400 of any port) only when
// `skippable` is set: the intro hp*x.xmv passes 1, the EA logos pass 0.
// The player keeps `skippable` in edi, loaded from its own argument right
// after its first call, OpenMovie (0x18CD0) — so OpenMovie sets the
// argument to 1 in the caller's frame. (0x18D40 itself belongs to the D3D8
// HLE's movies.cpp, which wraps it; one override per slot.) Each loop
// iteration polls the pads itself (FUN_000193E0 handles pad insertion too,
// FUN_00039C40) and then tests (current ^ previous) & current & 0x40400.
//
// Frame of 0x18D40 at that call: sub esp,0x24 + 4 pushes + return address
// = 0x38 below its entry esp, so its 2nd argument (entry esp + 8) is at
// OpenMovie's esp + 0x40. Checked by the return address 0x18D50.
//
// Cutscenes are scripts of the game's script interpreter (FUN_0006F010),
// stepped once per frame by ScriptUpdate (0x70D10; 0x54F90 is a jmp to it)
// while 0x5330DE (script active) is set. The retail code keeps a developer
// fast-forward gated by bit 0x40 of the word 0x527F26, which nothing in the
// game sets: while Y (raw pad bit 0x20000 at 0x51E0AC) is held, an animation
// wait (0x16) finishes the animation and a timed wait (0x3C) ends; a fresh
// press of A/B/X/Y/START/BACK (0xF0C00) ends the current voice line (wait
// 0x38, only if the bit was set when the script started: ScriptReset
// 0x6EDA0 copies it into 0x5330EC; opcode 0x35 can clear it per script),
// and the game stops the voice (FUN_0003F8E0). The script still runs to its
// end opcode, so its flags, actor placement and end callback happen as in a
// full play. Once the 4 s hold fires, every remaining step of that script
// runs with the bit set (0x527F26 is in the state block the game saves; the
// byte is put back after each call), Y faked as held and, on a voice line,
// a fresh press faked — whether or not the button is still held.
// Camera moves, morph and fade waits are not shortened (no skip code).

#include <chrono>
#include <cstdlib>
#include <cstring>

#include "runtime/hp2_runtime.h"

// D3D8 HLE: frames presented (for log lines; tests key on them). Weak.
uint64_t hp2_d3d8_frame_count() __attribute__((weak));

namespace {

uint64_t Frame() { return hp2_d3d8_frame_count ? hp2_d3d8_frame_count() : 0; }

bool EnvOn(const char* name) {
  const char* v = std::getenv(name);
  return v && *v && std::strcmp(v, "0") != 0;
}

bool Enabled() {
  static const bool on = [] {
    const char* v = std::getenv("HP2_SKIP");
    const bool on = v && *v ? std::strcmp(v, "0") != 0
                            : !EnvOn("HP2_PAD_SCRIPT") && !EnvOn("HP2_BACKGROUND");
    hp2_log("polish: movie / cutscene skip %s (movies: A / START; cutscenes: hold 4 s)", on ? "on" : "off");
    return on;
  }();
  return on;
}

// Raw pad words (FUN_00019050): port n current at 0x51E0AC + n*0x38,
// previous frame's at +4.
constexpr uint32_t kPad0 = 0x51E0AC, kPadStride = 0x38, kPadPrev = 4;
constexpr uint32_t kPadA = 0x40000, kPadY = 0x20000, kPadStart = 0x400;
constexpr uint32_t kPadHold = kPadA | kPadStart;  // the skip buttons
constexpr uint32_t kPadSkipMask = 0xF0C00;        // A/B/X/Y/START/BACK
constexpr double kHoldSeconds = 4.0;

bool AnyPadDown(uint32_t mask) {
  for (uint32_t p = 0; p < 4; ++p)
    if (hp2_rd32(kPad0 + p * kPadStride) & mask) return true;
  return false;
}

// A / START held for kHoldSeconds within one scene.
class HoldGate {
 public:
  void Start() {
    since_ = {};
    fired_ = false;
    logged_ = 0;
  }
  // Called once per scene step; true from the step the hold reaches 4 s on.
  bool Update(const char* what) {
    if (fired_) return true;
    if (!AnyPadDown(kPadHold)) {
      since_ = {};
      logged_ = 0;
      return false;
    }
    if (since_ == Clock::time_point{}) since_ = Clock::now();
    const double held = std::chrono::duration<double>(Clock::now() - since_).count();
    if (held >= logged_ + 1 && logged_ < 3) {
      logged_ = static_cast<int>(held);
      hp2_log("polish: frame %llu: %s skip: held %d s of 4", (unsigned long long)Frame(), what,
              logged_);
    }
    if (held < kHoldSeconds) return false;
    fired_ = true;
    return true;
  }
  bool fired() const { return fired_; }

 private:
  using Clock = std::chrono::steady_clock;
  Clock::time_point since_{};
  bool fired_ = false;
  int logged_ = 0;
};

constexpr uint32_t kOpenMovie = 0x18CD0;
constexpr uint32_t kOpenMovieRet = 0x18D50;  // in PlayMovie, after `call 0x18CD0`
constexpr uint32_t kSkippableAt = 0x40;      // PlayMovie's 2nd argument, from OpenMovie's esp
void OpenMovie(Hp2Ctx& c) {
  static const HostFn original = hp2_original(kOpenMovie);
  if (Enabled() && hp2_rd32(c.esp) == kOpenMovieRet) {
    const uint32_t at = c.esp + kSkippableAt;
    if (hp2_rd32(at) == 0) {
      hp2_wr32(at, 1);
      hp2_log("polish: movie made skippable (A / START)");
    }
  }
  original(c);
}

constexpr uint32_t kScriptUpdate = 0x70D10;
constexpr uint32_t kScriptReset = 0x6EDA0;
constexpr uint32_t kScriptActive = 0x5330DE;  // byte
constexpr uint32_t kWait = 0x5330DC;          // u16: what the script waits for
constexpr uint16_t kWaitVoice = 0x38;
constexpr uint32_t kDevFlags = 0x527F26;      // byte; 0x40 = cutscene fast-forward
constexpr uint8_t kDevSkip = 0x40;

// Runs `fn` with the fast-forward bit set, then puts the byte back.
void WithDevSkip(Hp2Ctx& c, HostFn fn) {
  const uint8_t saved = hp2_rd8(kDevFlags);
  hp2_wr8(kDevFlags, saved | kDevSkip);
  fn(c);
  hp2_wr8(kDevFlags, (hp2_rd8(kDevFlags) & ~kDevSkip) | (saved & kDevSkip));
}

// Sets 0x5330EC (voice lines skippable) as a developer build would; it only
// has an effect while the bit is set, i.e. after the hold fired.
void ScriptReset(Hp2Ctx& c) {
  static const HostFn original = hp2_original(kScriptReset);
  if (!Enabled()) return original(c);
  WithDevSkip(c, original);
}

void ScriptUpdate(Hp2Ctx& c) {
  static const HostFn original = hp2_original(kScriptUpdate);
  // Start / end lines in both modes: tests compare a skipped run with a full one.
  static bool active = false;
  static HoldGate gate;
  if (!hp2_rd8(kScriptActive)) {
    active = false;
    return original(c);
  }
  if (!active) {
    active = true;
    gate.Start();
    hp2_log("polish: frame %llu: script started", (unsigned long long)Frame());
  }
  const bool was_fired = gate.fired();
  if (!Enabled() || !gate.Update("cutscene")) {
    original(c);
  } else {
    if (!was_fired)
      hp2_log("polish: frame %llu: cutscene fast-forward (A / START held 4 s)",
              (unsigned long long)Frame());
    // Y held for animation / timed waits; on a voice line a fresh Y press.
    const uint32_t cur = hp2_rd32(kPad0), prev = hp2_rd32(kPad0 + kPadPrev);
    hp2_wr32(kPad0, cur | kPadY);
    if (hp2_rd16(kWait) == kWaitVoice) hp2_wr32(kPad0 + kPadPrev, prev & ~kPadSkipMask);
    WithDevSkip(c, original);
    hp2_wr32(kPad0, cur);
    hp2_wr32(kPad0 + kPadPrev, prev);
  }
  if (!hp2_rd8(kScriptActive)) {
    active = false;
    hp2_log("polish: frame %llu: script ended%s", (unsigned long long)Frame(),
            gate.fired() ? " (fast-forwarded)" : "");
  }
}

}  // namespace

HP2_OVERRIDE(kOpenMovie, OpenMovie);
HP2_OVERRIDE(kScriptReset, ScriptReset);
HP2_OVERRIDE(kScriptUpdate, ScriptUpdate);
