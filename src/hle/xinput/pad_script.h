#pragma once
// Scripted controller input for silent background tests (owner: Kernel HLE).
//
// HP2_PAD_SCRIPT=<file> replaces every other input source: port 0 holds a
// connected gamepad whose state is a pure function of the game's frame
// number (D3D Swap count, hp2_d3d8_frame_count), so a run under the 60 Hz
// pacer is deterministic. The SDL backend reads nothing while a script runs.
//
// One command per line, '#' starts a comment:
//   <frame> press <BTN> [frames]        held for `frames` (default 6), then released
//   <frame> hold <BTN> [frames]         held for `frames`, or until `release` if omitted
//   <frame> release <BTN>|all           ends holds (all: buttons, sticks, triggers)
//   <frame> stick L|R <x> <y> [frames]  -1.0..1.0, up = +y; until `release` if no frames
//   <frame> trigger L|R <0..1> [frames] analog trigger (also BTN LT / RT = fully pulled)
//   <frame> disconnect | connect        port 0 cable
// <frame> is absolute or "+N" (N frames after the previous line's frame).
// BTN: A B X Y BLACK WHITE LT RT START BACK DPAD_UP DPAD_DOWN DPAD_LEFT
//      DPAD_RIGHT LTHUMB RTHUMB (case-insensitive; UP/DOWN/LEFT/RIGHT alias the D-pad).
//
// Compact form (Render's phase-3 scripts; a line whose first word has a ':'):
//   450:A 456:- 6000:LY=32000 6090:- 960:A+START
// "<frame>:<state>" sets the whole pad from that frame on; "-" releases all;
// state = BTN or LX/LY/RX/RY=<-32768..32767> joined by '+'. A script in this
// form reads as disconnected until frame 1 (like Render's player).
// HP2_PAD_SCRIPT may be a file path, "@path", or compact events inline.

#include <cstdint>
#include <string>
#include <vector>

#include "hle/xinput/xinput.h"

namespace hp2k::xinput {

struct PadEvent {
  enum Op : uint8_t { kSet, kHold, kRelease, kReleaseAll, kStick, kTrigger, kConnect, kDisconnect };
  uint64_t frame = 0;
  uint64_t frames = 0;     // 0 = until released
  Op op = kHold;
  int target = 0;          // button bit (kHold/kRelease: < 0x100 digital, 0x100+i analog i);
                           // stick/trigger: 0 = L, 1 = R
  float x = 0, y = 0;      // stick x/y, trigger value in x
  Hp2PadState pad;         // kSet: the whole pad
  int line = 0;
  std::string text;        // the source line, for logs
};

// Parses a script. On error returns false and sets *error to "line N: ...".
bool ParsePadScript(const std::string& text, std::vector<PadEvent>* out, std::string* error);

// Port-0 state at `frame`: every event with event.frame <= frame applied in
// order (a pad that is connected unless a `disconnect` is in effect).
Hp2PadState PadStateAt(const std::vector<PadEvent>& events, uint64_t frame);

}  // namespace hp2k::xinput
