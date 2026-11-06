// Modern orbit camera (owner: Kernel HLE): wraps the game's gameplay camera
// update FUN_00022320(camera, reset) (object 0x50C0E8, called through a
// pointer once per game frame).
//
// What the original does with the right stick (decomp, NTSC-US):
//  - RX (pad struct 0x51E09C, after the game's own per-axis dead zone 0.35,
//    FUN_00019050) sets a yaw offset 0x50C460 = RX * 0.04 rad (sign by Options
//    "Camera Control" 0x527B8A) that is added to the yaw once per frame: at
//    most 0.04 rad per frame (~140 deg/s at 60 fps) and nothing below 35% of
//    the stick's travel;
//  - the visible camera (+0x00..+0x08) then moves only 1/13 of the way to
//    the wanted point (+0x200..+0x208) per frame: ~0.2 s of lag on every turn;
//  - the yaw itself (+0x20C) is re-derived each frame from the wanted point
//    around the target (+0x0C..+0x14), a tether that Harry drags along;
//  - when the left stick has moved (flag 0x50C45C) and is then released, the
//    yaw swings 1% per frame towards Harry's heading until within 10 deg (the
//    "follow" swing); +0x25C snaps it behind Harry (lerp 1/4).
// Together: a slow, heavy turn that the follow swing pulls back.
//
// Modern (settings.toml [camera] modern = true, the default): RX is read raw
// from the XInput state (0x4386AA: our own dead zone and curve only, see
// input_filter.h), hidden from the original (0x51E09C = 0 during the call, so
// its turn and its 1/13 lag on the turn are gone), and the camera is turned
// by RX * yaw_speed * dt around the target before the original runs: the
// wanted point and the visible camera together, so the turn is immediate
// while following Harry keeps the original smoothing, and the original's wall
// handling works around the new yaw (a wall pulls the camera in). The follow swing and the snap
// are held off while the stick is used and for recenter_delay seconds (4)
// after it (< 0: never once the stick was used). Zoom (RY) and pitch are unchanged.
// tests/scripts/camera_modern.sh measures the turn, the recenter delay and
// that the left stick walks Harry relative to the camera.

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "hle/kernel/kernel_internal.h"
#include "hle/xinput/input_filter.h"

HostFn hp2_original(uint32_t va);  // xinput.cpp (weak) / the runtime

namespace hp2k::xinput {

namespace {

constexpr uint32_t kCameraFn = 0x00022320;
constexpr uint32_t kCamera = 0x0050C0E8;     // the gameplay camera object
constexpr uint32_t kPadRx = 0x0051E09C;      // port 0 pad struct, RX after the game's dead zone
constexpr uint32_t kRawRx = 0x004386AA;      // port 0 XINPUT_STATE sThumbRX
constexpr uint32_t kFollow = 0x0050C45C;     // follow swing armed (byte)
constexpr uint32_t kOptionNormal = 0x00527B8A;  // Options "Camera Control": 1 Normal, 0 Reverse
// Camera object fields (byte offsets).
constexpr uint32_t kPos = 0x00, kTarget = 0x0C, kDist = 0x34, kWanted = 0x200, kYaw = 0x20C,
                   kSnap = 0x25C;
constexpr uint32_t kPitch = 0x0050C2F8;     // camera pitch (rad), fixed in gameplay
// The camera's wall probes FUN_0002E840(world 0x5276B0, mask, out[0x300]):
// out+0 is the straight line target -> wanted point (FUN_0002B7E0), +0x80 /
// +0x100 the same 16 deg to either side, +0x180 from above; per probe +0x28
// hit point, +0x34 hit flag (byte). Collision mask = 1 << byte 0x527B7C.
constexpr uint32_t kProbeFn = 0x0002E840, kWorld = 0x005276B0, kMaskShift = 0x00527B7C;
constexpr float kActive = 0.02f;  // |RX| above this counts as camera input

float RdF(uint32_t va) {
  float f;
  std::memcpy(&f, GP(va), 4);
  return f;
}
void WrF(uint32_t va, float f) { std::memcpy(GP(va), &f, 4); }

// Turns the point at `at` (x, z; y kept) around the target by `delta` rad, in
// the game's convention (point = target + r * (sin yaw, cos yaw)).
void Orbit(uint32_t at, float tx, float tz, float delta) {
  float ox = RdF(at) - tx, oz = RdF(at + 8) - tz;
  float r = std::hypot(ox, oz);
  if (r < 1e-4f) return;
  float yaw = std::atan2(ox, oz) + delta;
  WrF(at, tx + r * std::sin(yaw));
  WrF(at + 8, tz + r * std::cos(yaw));
}

struct State {
  bool modern = true;
  float yaw_speed = 0, recenter_delay = 0;  // rad/s, s
  uint64_t last_vblank = 0;
  double last_input = -1e9;  // vblank time of the last right-stick input
  // Stats for the log: turned frames and total angle.
  uint64_t turned = 0;
  double total = 0;
};

State& Get() {
  static State s = [] {
    State st;
    input::InputSettings in = input::LoadSettings();
    st.modern = in.camera_modern;
    st.yaw_speed = in.camera_yaw_speed * float(M_PI / 180.0);
    st.recenter_delay = in.camera_recenter_delay;
    Log("camera: %s", st.modern ? "modern orbit" : "original");
    return st;
  }();
  return s;
}

}  // namespace

// HP2_CAMERA_LOG=1: one line per camera frame (tests/scripts/camera_modern.sh).
void Trace(uint64_t vb, float rx, bool hold) {
  static const bool trace = [] {
    const char* e = std::getenv("HP2_CAMERA_LOG");
    return e && *e == '1';
  }();
  if (trace)
    Log("camera: vb %llu rx %.3f hold %d pos %.4f %.4f target %.4f %.4f wanted %.4f %.4f "
        "follow %d",
        (unsigned long long)vb, rx, int(hold), RdF(kCamera + kPos), RdF(kCamera + kPos + 8),
        RdF(kCamera + kTarget), RdF(kCamera + kTarget + 8), RdF(kCamera + kWanted),
        RdF(kCamera + kWanted + 8), Rd8(kFollow));
}

// When the straight line from Harry to the camera is blocked, the original
// moves the camera onto a side probe, 16 deg off the yaw, so turning towards a
// wall stops there (and the camera jumps after the release). While the stick
// holds the camera, it stays on its yaw instead, pulled in to just short of
// the wall (the straight probe's hit point), as orbit cameras do.
void KeepYawAtWalls(Ctx& c, const float pos0[3]) {
  float yaw = RdF(kCamera + kYaw), d = RdF(kCamera + kDist), pitch = RdF(kPitch);
  float tx = RdF(kCamera + kTarget), ty = RdF(kCamera + kTarget + 4), tz = RdF(kCamera + kTarget + 8);
  float wx = RdF(kCamera + kWanted) - tx, wz = RdF(kCamera + kWanted + 8) - tz;
  float off = std::remainder(std::atan2(wx, wz) - yaw, float(2 * M_PI));
  if (std::fabs(off) < 0.02f || d <= 0) return;  // on its yaw: not a side probe
  float ex = tx + d * std::sin(yaw), ey = ty + d * std::sin(pitch), ez = tz + d * std::cos(yaw);
  WrF(kCamera + kWanted, ex);
  WrF(kCamera + kWanted + 4, ey);
  WrF(kCamera + kWanted + 8, ez);
  uint32_t esp = c.esp, eax = c.eax, ecx = c.ecx, edx = c.edx;
  c.esp = (c.esp - 0x400) & ~0xFu;
  uint32_t out = c.esp;
  CallGuest(c, kProbeFn, {kWorld, 1u << (Rd8(kMaskShift) & 31), out});
  c.esp = esp, c.eax = eax, c.ecx = ecx, c.edx = edx;
  if (!Rd8(out + 0x34)) return;  // clear now: the straight point stays
  float hx = RdF(out + 0x28) - tx, hy = RdF(out + 0x2C) - ty, hz = RdF(out + 0x30) - tz;
  float h = std::sqrt(hx * hx + hy * hy + hz * hz);
  if (!(h <= d + 0.01f)) return;  // not on the line: keep the straight point
  float f = std::max(h - 0.15f, 0.25f) / std::max(h, 1e-3f);
  float w[3] = {tx + hx * f, ty + hy * f, tz + hz * f};
  // The original has already moved the visible camera 1/13 of the way to the
  // side probe in this call: move it towards this point instead.
  for (int k = 0; k < 3; k++) {
    WrF(kCamera + kWanted + 4 * k, w[k]);
    WrF(kCamera + kPos + 4 * k, pos0[k] + (w[k] - pos0[k]) / 13.0f);
  }
}

void GameCamera(Ctx& c) {
  static HostFn orig = hp2_original(kCameraFn);
  if (!orig) return;
  if (Arg(c, 0) != kCamera) return orig(c);
  State& s = Get();
  uint64_t vb = hp2_vblank_count();
  float rx = input::AxisToFloat(int16_t(Rd32(kRawRx) & 0xFFFF));
  if (!s.modern) {
    orig(c);
    return Trace(vb, rx, false);
  }
  bool reset = (Arg(c, 1) & 0xFF) != 0;
  double now = vb / 60.0;
  double dt = s.last_vblank ? std::min<double>(vb - s.last_vblank, 6) / 60.0 : 0.0;
  s.last_vblank = vb;

  bool active = std::fabs(rx) > kActive;
  if (active) s.last_input = now;
  // recenter_delay < 0 ("never"): once the stick was used, no follow swing.
  bool hold = s.recenter_delay < 0 ? s.last_input > -1e8 : now - s.last_input < s.recenter_delay;

  // The turn goes in before the original runs: it derives its yaw from the
  // wanted point and resolves walls (its probes) around that yaw, so a wall
  // pulls the camera in instead of undoing the turn.
  if (active && !reset && dt > 0) {
    float sign = Rd8(kOptionNormal) ? 1.0f : -1.0f;
    float delta = float(sign * rx * s.yaw_speed * dt);
    float tx = RdF(kCamera + kTarget), tz = RdF(kCamera + kTarget + 8);
    Orbit(kCamera + kWanted, tx, tz, delta);
    Orbit(kCamera + kPos, tx, tz, delta);
    s.turned++;
    s.total += delta;
    if (s.turned % 600 == 1)
      Log("camera: turned %llu frames, %.1f deg in all; last %.2f deg (rx %.2f, dt %.1f ms)",
          (unsigned long long)s.turned, s.total * 180 / M_PI, delta * 180 / M_PI, rx, dt * 1000);
  }
  // The follow swing runs only while the flag is set and the left stick is
  // idle; the game sets the flag in the call when the left stick moves. While
  // held off, the flag is hidden from the call and kept (or newly set) for
  // after the delay, so Harry's last walk still swings the camera then.
  float pad_rx = RdF(kPadRx);
  uint8_t follow = Rd8(kFollow);
  WrF(kPadRx, 0.0f);
  if (hold) Wr8(kFollow, 0);
  if (active) Wr8(kCamera + kSnap, 0);
  float pos0[3] = {RdF(kCamera + kPos), RdF(kCamera + kPos + 4), RdF(kCamera + kPos + 8)};
  orig(c);
  WrF(kPadRx, pad_rx);
  if (hold) Wr8(kFollow, uint8_t(follow | Rd8(kFollow)));
  if (hold && !reset) KeepYawAtWalls(c, pos0);
  Trace(vb, rx, hold);
}

}  // namespace hp2k::xinput
