#pragma once
// Gamepad stick shaping and input settings (owner: Kernel HLE). Applied by the
// SDL backend to real pads only, before the game sees the axes; pad scripts
// and the keyboard are not shaped. The game keeps its own processing on top
// (FUN_00019050: per-axis dead zone 0.35, then rescale).
//
// Per stick, in this order (all in [-1, 1], up = positive):
//   1. radial dead zone: magnitude below `inner` -> 0, above `outer` -> 1,
//      rescaled linearly in between (no jump at either edge); the direction is
//      kept, so diagonals never exceed magnitude 1 (no square-gate boost);
//   2. response curve on the magnitude: linear | expo (m^exponent) | dual
//      (expo over the first 75% of travel up to 0.5, then linear to 1);
//   3. invert X / Y; 4. sensitivity X / Y (scale, clamp to [-1, 1]), then the
//      magnitude is clamped to 1 again;
//   5. smoothing: one-pole low-pass with time constant `smoothing_ms`,
//      frame-rate independent (alpha = 1 - exp(-dt / tau)).
// Defaults: right stick (camera) 8% / 95%, expo 1.6, sensitivity 1.0 / 0.8,
// smoothing 8 ms; left stick (movement) 10% / 95%, linear, no smoothing.
//
// Settings: ~/Library/Application Support/HP2/settings.toml (created with the
// defaults and comments when missing), each key overridable by the
// environment: HP2_INPUT_<SECTION>_<KEY>, e.g. HP2_INPUT_CAMERA_INVERT_Y=1.

#include <cstdint>
#include <string>

namespace hp2k::input {

enum class Curve { kLinear, kExpo, kDual };

struct StickParams {
  float inner = 0.0f, outer = 1.0f;  // radial dead zone, fractions of full deflection
  Curve curve = Curve::kLinear;
  float exponent = 1.0f;             // expo / dual
  bool invert_x = false, invert_y = false;
  float sens_x = 1.0f, sens_y = 1.0f;  // 0.1 .. 3.0
  float smoothing_ms = 0.0f;           // 0 = off
};

enum class NintendoLayout { kAuto, kPosition, kLabel };

struct InputSettings {
  StickParams camera, move;
  // Default of the game's own Options "Camera Control" for a new game / the
  // main menu (FUN_0004A8E0): the original is "Reverse"; saves keep theirs.
  bool camera_default_normal = true;
  // Modern orbit camera (camera.cpp): the right stick turns the gameplay
  // camera at yaw_speed (degrees per second at full deflection, proportional
  // below), at once; the game's own follow swing waits recenter_delay seconds
  // after the last right-stick input (< 0 = never). modern = false: the
  // original camera (0.04 rad per frame, its own dead zone and lag).
  bool camera_modern = true;
  float camera_yaw_speed = 330.0f;
  float camera_recenter_delay = 4.0f;
  NintendoLayout nintendo_layout = NintendoLayout::kAuto;
  int rumble_strength = 100;  // percent
  InputSettings();            // the defaults above
};

// int16 axis (XInput convention) <-> [-1, 1]; -32768 and 32767 are both full.
float AxisToFloat(int16_t v);
int16_t FloatToAxis(float v);

// Steps 1-4 on one sample.
void ShapeStick(const StickParams& p, float& x, float& y);

// Step 5; state per stick.
struct Smoother {
  float x = 0, y = 0;
  bool primed = false;
  void Apply(float smoothing_ms, double dt_s, float& ox, float& oy);
};

// Parses settings text into `s` (unknown keys and bad values are reported in
// `warnings`, one per line, and keep the default). Then applies HP2_INPUT_*
// environment overrides (`env` = getenv, injectable for tests).
void ParseSettings(const std::string& text, InputSettings& s, std::string* warnings);
void ApplyEnvOverrides(InputSettings& s, const char* (*env)(const char*), std::string* warnings);
// The commented default file.
const char* DefaultSettingsText();
// Default path: $HOME/Library/Application Support/HP2/settings.toml.
std::string SettingsPath();
// Reads (creating it with the defaults when missing) and applies env overrides.
InputSettings LoadSettings();

}  // namespace hp2k::input
