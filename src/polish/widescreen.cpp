// 16:10 widescreen (Polish), opt-in: HP2_WIDESCREEN=1.
//
// The game is 4:3 only and keeps its 640x480 logical screen. The D3D8 HLE
// (hp2_d3d8_set_widescreen) renders that screen kWide = (16/10)/(4/3)
// times wider and draws the 4:3 view kWide narrower inside it: 3D shows
// more at the sides (Hor+: the clip volume is wider), 2D keeps its
// proportions, centred; full-screen quads still cover the whole width.
// Here: the camera's horizontal half angle (Camera::SetFov, 0x348C0, from
// the aspect camera[0x39] / camera[0x3A]) is widened to match, so the
// game's view culling keeps the objects in the extra side strips.
//
// HP2_WIDESCREEN unset or 0: no change at all (the override only passes
// the call through), so goldens are unaffected.

#include <cstdlib>
#include <cstring>

#include "polish/postfx/postfx.h"
#include "polish/texpack/texpack.h"
#include "runtime/hp2_runtime.h"

// D3D8 HLE (Render): widen screen-sized render targets by `aspect_scale`
// and keep 2D at 4:3. Weak: without it the 3D view would be squeezed into
// the 4:3 target, so widescreen stays off.
extern "C" void hp2_d3d8_set_widescreen(float aspect_scale) __attribute__((weak));

namespace {

constexpr float kWide = (16.0f / 10.0f) / (4.0f / 3.0f);  // 1.2
constexpr uint32_t kCameraSetFov = 0x348C0;

bool Enabled() {
  static const bool on = [] {
    const char* v = std::getenv("HP2_WIDESCREEN");
    bool want = v && *v && std::strcmp(v, "0") != 0;
    if (want && !hp2_d3d8_set_widescreen) {
      hp2_log("polish: HP2_WIDESCREEN ignored (the D3D8 HLE has no widescreen support)");
      return false;
    }
    if (want) {
      hp2_d3d8_set_widescreen(kWide);
      hp2_log("polish: widescreen 16:10 (view x%.3f wider)", kWide);
    }
    return want;
  }();
  return on;
}

bool Trace() {
  static const bool on = std::getenv("HP2_POLISH_TRACE") != nullptr;
  return on;
}

// Camera::SetFov(this = camera, degrees) (thiscall): derives the half
// angles (camera[0..3]) from the aspect camera[0x39] / camera[0x3A].
void CameraSetFov(Hp2Ctx& c) {
  static const HostFn original = hp2_original(kCameraSetFov);
  static uint32_t n = 0;
  if (Trace() && (n++ < 8 || n % 600 == 0))
    hp2_log("polish: SetFov #%u this %08X from %08X deg %08X aspect %08X/%08X", n, c.ecx,
            hp2_rd32(c.esp), hp2_rd32(c.esp + 4), hp2_rd32(c.ecx + 0x39 * 4),
            hp2_rd32(c.ecx + 0x3A * 4));
  if (!Enabled()) return original(c);
  // Widen the aspect for this call only: the stored ratio stays the game's.
  const uint32_t at = c.ecx + 0x39 * 4;
  const uint32_t saved = hp2_rd32(at);
  float aspect;
  std::memcpy(&aspect, &saved, 4);
  aspect *= kWide;
  uint32_t a;
  std::memcpy(&a, &aspect, 4);
  hp2_wr32(at, a);
  original(c);
  hp2_wr32(at, saved);
}

}  // namespace

HP2_OVERRIDE(kCameraSetFov, CameraSetFov);

// Called once at startup (main.cpp) so the D3D8 HLE knows the target width
// before CreateDevice; also sets up the post-processing look and the HD UI pack.
void hp2_polish_init() {
  (void)Enabled();
  hp2_polish_postfx_init();
  hp2_polish_texpack_init();
}
