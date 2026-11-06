// Gamepad stick shaping and input settings (input_filter.h).

#include "hle/xinput/input_filter.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

#include "hle/kernel/kernel_internal.h"

namespace hp2k::input {

InputSettings::InputSettings() {
  camera.inner = 0.08f;
  camera.outer = 0.95f;
  camera.curve = Curve::kExpo;
  camera.exponent = 1.6f;
  camera.sens_x = 1.0f;
  camera.sens_y = 0.8f;
  camera.smoothing_ms = 8.0f;  // 20 felt sluggish on a real pad (user, 2026-10-31)
  move.inner = 0.10f;
  move.outer = 0.95f;
}

float AxisToFloat(int16_t v) { return v < 0 ? v / 32768.0f : v / 32767.0f; }

int16_t FloatToAxis(float v) {
  v = std::clamp(v, -1.0f, 1.0f);
  return int16_t(std::lround(v * (v < 0 ? 32768.0f : 32767.0f)));
}

namespace {

float CurveOf(const StickParams& p, float m) {
  switch (p.curve) {
    case Curve::kLinear:
      return m;
    case Curve::kExpo:
      return std::pow(m, p.exponent);
    case Curve::kDual:
      // Fine control over the first 75% of travel (expo up to 0.5), then a
      // linear fast-turn zone; continuous at the join.
      if (m <= 0.75f) return 0.5f * std::pow(m / 0.75f, p.exponent);
      return 0.5f + (m - 0.75f) * 2.0f;
  }
  return m;
}

}  // namespace

void ShapeStick(const StickParams& p, float& x, float& y) {
  float m = std::sqrt(x * x + y * y);
  if (m <= p.inner || m == 0.0f) {
    x = y = 0.0f;
    return;
  }
  float span = std::max(p.outer - p.inner, 1e-3f);
  float r = std::min((m - p.inner) / span, 1.0f);
  r = std::clamp(CurveOf(p, r), 0.0f, 1.0f);
  x = x / m * r;
  y = y / m * r;
  if (p.invert_x) x = -x;
  if (p.invert_y) y = -y;
  x = std::clamp(x * p.sens_x, -1.0f, 1.0f);
  y = std::clamp(y * p.sens_y, -1.0f, 1.0f);
  m = std::sqrt(x * x + y * y);
  if (m > 1.0f) x /= m, y /= m;
}

void Smoother::Apply(float smoothing_ms, double dt_s, float& ox, float& oy) {
  if (smoothing_ms <= 0.0f || !primed) {
    x = ox, y = oy;
    primed = true;
    return;
  }
  float a = float(1.0 - std::exp(-std::max(dt_s, 0.0) * 1000.0 / smoothing_ms));
  x += (ox - x) * a;
  y += (oy - y) * a;
  // Settle exactly (a released stick reports 0, not a tiny drift).
  if (std::fabs(ox - x) < 1e-4f) x = ox;
  if (std::fabs(oy - y) < 1e-4f) y = oy;
  ox = x, oy = y;
}

// ---- settings ---------------------------------------------------------------

namespace {

std::string Trim(std::string s) {
  size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower((unsigned char)c));
  return s;
}

void AddWarning(std::string* w, const std::string& msg) {
  if (w) *w += msg + "\n";
}

bool ParseBool(const std::string& v, bool* out) {
  std::string l = Lower(v);
  if (l == "true" || l == "1" || l == "yes" || l == "on") return *out = true, true;
  if (l == "false" || l == "0" || l == "no" || l == "off") return *out = false, true;
  return false;
}

bool ParseFloat(const std::string& v, float lo, float hi, float* out) {
  char* end;
  float f = std::strtof(v.c_str(), &end);
  if (v.empty() || *end || !(f >= lo && f <= hi)) return false;
  *out = f;
  return true;
}

// Sets section.key = value (value already unquoted). False: unknown key or bad value.
bool Set(InputSettings& s, const std::string& section, const std::string& key,
         const std::string& value) {
  if (section == "camera" || section == "move") {
    StickParams& p = section == "camera" ? s.camera : s.move;
    if (key == "invert_x") return ParseBool(value, &p.invert_x);
    if (key == "invert_y") return ParseBool(value, &p.invert_y);
    if (key == "sensitivity_x") return ParseFloat(value, 0.1f, 3.0f, &p.sens_x);
    if (key == "sensitivity_y") return ParseFloat(value, 0.1f, 3.0f, &p.sens_y);
    if (key == "deadzone_inner") return ParseFloat(value, 0.0f, 0.9f, &p.inner);
    if (key == "deadzone_outer") return ParseFloat(value, 0.1f, 1.0f, &p.outer);
    if (key == "curve_exponent") return ParseFloat(value, 0.2f, 5.0f, &p.exponent);
    if (key == "smoothing_ms") return ParseFloat(value, 0.0f, 500.0f, &p.smoothing_ms);
    if (section == "camera" && key == "modern") return ParseBool(value, &s.camera_modern);
    if (section == "camera" && key == "yaw_speed")
      return ParseFloat(value, 30.0f, 1080.0f, &s.camera_yaw_speed);
    if (section == "camera" && key == "recenter_delay") {
      if (Lower(value) == "never") return s.camera_recenter_delay = -1.0f, true;
      return ParseFloat(value, 0.0f, 60.0f, &s.camera_recenter_delay);
    }
    if (section == "camera" && key == "default_option") {
      std::string l = Lower(value);
      if (l == "normal") return s.camera_default_normal = true, true;
      if (l == "reverse") return s.camera_default_normal = false, true;
      return false;
    }
    if (key == "curve") {
      std::string l = Lower(value);
      if (l == "linear") return p.curve = Curve::kLinear, true;
      if (l == "expo") return p.curve = Curve::kExpo, true;
      if (l == "dual") return p.curve = Curve::kDual, true;
      return false;
    }
    return false;
  }
  if (section == "gamepad") {
    if (key == "nintendo_layout") {
      std::string l = Lower(value);
      if (l == "auto") return s.nintendo_layout = NintendoLayout::kAuto, true;
      if (l == "position") return s.nintendo_layout = NintendoLayout::kPosition, true;
      if (l == "label") return s.nintendo_layout = NintendoLayout::kLabel, true;
      return false;
    }
    if (key == "rumble_strength") {
      float f;
      if (!ParseFloat(value, 0.0f, 100.0f, &f)) return false;
      s.rumble_strength = int(std::lround(f));
      return true;
    }
  }
  return false;
}

std::string Unquote(std::string v) {
  if (v.size() >= 2 && (v[0] == '"' || v[0] == '\'') && v.back() == v[0])
    return v.substr(1, v.size() - 2);
  return v;
}

}  // namespace

void ParseSettings(const std::string& text, InputSettings& s, std::string* warnings) {
  std::istringstream in(text);
  std::string line, section;
  int n = 0;
  while (std::getline(in, line)) {
    n++;
    // Comments: '#' outside a quoted string.
    bool q = false;
    for (size_t i = 0; i < line.size(); i++) {
      if (line[i] == '"') q = !q;
      if (line[i] == '#' && !q) {
        line.resize(i);
        break;
      }
    }
    line = Trim(line);
    if (line.empty()) continue;
    if (line.front() == '[' && line.back() == ']') {
      section = Lower(Trim(line.substr(1, line.size() - 2)));
      continue;
    }
    size_t eq = line.find('=');
    if (eq == std::string::npos) {
      AddWarning(warnings, "settings line " + std::to_string(n) + ": expected key = value");
      continue;
    }
    std::string key = Lower(Trim(line.substr(0, eq))), value = Unquote(Trim(line.substr(eq + 1)));
    if (!Set(s, section, key, value))
      AddWarning(warnings, "settings line " + std::to_string(n) + ": [" + section + "] " + key + " = " +
                         value + " ignored (unknown key or value out of range)");
  }
}

void ApplyEnvOverrides(InputSettings& s, const char* (*env)(const char*), std::string* warnings) {
  static const char* const kKeys[][2] = {
      {"camera", "invert_x"},       {"camera", "invert_y"},       {"camera", "sensitivity_x"},
      {"camera", "sensitivity_y"},  {"camera", "curve"},          {"camera", "curve_exponent"},
      {"camera", "smoothing_ms"},   {"camera", "deadzone_inner"}, {"camera", "deadzone_outer"},
      {"camera", "default_option"}, {"camera", "modern"},         {"camera", "yaw_speed"},
      {"camera", "recenter_delay"},
      {"move", "deadzone_inner"},   {"move", "deadzone_outer"},   {"gamepad", "nintendo_layout"},
      {"gamepad", "rumble_strength"},
  };
  for (auto& k : kKeys) {
    std::string name = std::string("HP2_INPUT_") + k[0] + "_" + k[1];
    for (char& c : name) c = char(std::toupper((unsigned char)c));
    const char* v = env(name.c_str());
    if (!v || !*v) continue;
    if (!Set(s, k[0], k[1], v)) AddWarning(warnings, name + "=" + v + " ignored (bad value)");
  }
}

const char* DefaultSettingsText() {
  return R"(# HP2Port settings. Created on first run with the defaults; edit and restart.
# Delete this file to get the defaults back. Every key can also be set for one
# run from the environment: HP2_INPUT_<SECTION>_<KEY>, e.g. HP2_INPUT_CAMERA_INVERT_Y=1.
# Applies to gamepads only (not the keyboard). The game has its own camera
# option too: Options -> Camera Control (Reverse = the game's default, Normal).

[camera]                 # right stick: left/right turns, up/down zooms
modern = true            # true: orbit camera, turns at yaw_speed at once;
                         # false: the original (slow turn, lag, own dead zone)
yaw_speed = 330          # degrees per second at full deflection (30 .. 1080)
recenter_delay = 4.0     # seconds after the right stick before the camera
                         # swings back behind Harry again; "never" = never
default_option = "normal" # the game's Options -> Camera Control for a new game:
                          # normal (stick right = view right) or reverse (the
                          # original Xbox default); a save keeps its own
invert_x = false
invert_y = false
sensitivity_x = 1.0      # 0.1 .. 3.0
sensitivity_y = 0.8      # 0.1 .. 3.0
curve = "expo"           # linear | expo | dual (fine aim, then a fast-turn zone)
curve_exponent = 1.6
smoothing_ms = 8         # 0 = off; more = smoother but laggier
deadzone_inner = 0.08    # round dead zone, fraction of full deflection
deadzone_outer = 0.95    # at or beyond: full deflection

[move]                   # left stick: walking speed follows how far you push
deadzone_inner = 0.10
deadzone_outer = 0.95

[gamepad]
nintendo_layout = "auto" # auto | position | label: on Nintendo pads, "position"
                         # maps by place (bottom button = Xbox A, as the game's
                         # prompts are drawn), "label" by letter (Nintendo A =
                         # Xbox A); auto = position
rumble_strength = 100    # 0 .. 100 %
)";
}

std::string SettingsPath() {
  const char* home = std::getenv("HOME");
  return std::string(home && *home ? home : ".") + "/Library/Application Support/HP2/settings.toml";
}

InputSettings LoadSettings() {
  InputSettings s;
  std::string path = SettingsPath(), warnings, text;
  if (std::ifstream f{path}) {
    std::stringstream ss;
    ss << f.rdbuf();
    text = ss.str();
  } else {
    // mkdir -p of the directory.
    std::string dir = path.substr(0, path.rfind('/'));
    for (size_t i = 1; i <= dir.size(); i++)
      if (i == dir.size() || dir[i] == '/') mkdir(dir.substr(0, i).c_str(), 0755);
    if (std::ofstream o{path}) {
      o << DefaultSettingsText();
      Log("input: created %s with the defaults", path.c_str());
    }
    text = DefaultSettingsText();
  }
  ParseSettings(text, s, &warnings);
  ApplyEnvOverrides(s, [](const char* n) -> const char* { return std::getenv(n); }, &warnings);
  for (size_t a = 0, b; a < warnings.size(); a = b + 1) {
    b = warnings.find('\n', a);
    Warn("input: %s", warnings.substr(a, b - a).c_str());
  }
  Log("input: camera dead zone %.2f..%.2f, curve %d^%.2f, sensitivity %.2f/%.2f, invert %d/%d, "
      "smoothing %.0f ms, game default %s, %s (%.0f deg/s, recenter %.2f s); move dead zone "
      "%.2f..%.2f; nintendo layout %d; rumble %d%%",
      s.camera.inner, s.camera.outer, int(s.camera.curve), s.camera.exponent, s.camera.sens_x,
      s.camera.sens_y, s.camera.invert_x, s.camera.invert_y, s.camera.smoothing_ms,
      s.camera_default_normal ? "Normal" : "Reverse", s.camera_modern ? "modern" : "original",
      s.camera_yaw_speed, s.camera_recenter_delay, s.move.inner,
      s.move.outer, int(s.nintendo_layout), s.rumble_strength);
  return s;
}

}  // namespace hp2k::input
