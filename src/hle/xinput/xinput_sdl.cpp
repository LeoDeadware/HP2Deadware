// SDL3 backend for the XInput HLE: gamepads and the keyboard.
//
// Keyboard (port 0): WASD left stick, IJKL right stick, arrows D-pad,
// Space A, E B, Q X, R Y, 1 Black, 2 White, U left trigger, O right trigger,
// F left-stick click, H right-stick click, Enter Start, Esc/Backspace Back.
// Gamepads (any SDL3 knows: Xbox, DualSense, DualShock 4, Switch Pro, 8BitDo,
// ...; on macOS through Apple GameController / HIDAPI): SDL south/east/west/
// north = A/B/X/Y by position (Nintendo pads: settings nintendo_layout),
// right/left shoulder = Black/White, triggers 1:1, sticks shaped by
// input_filter.h (Y flipped to "up = positive"). All pads hot-plug; port 0 is
// driven by the pad that gave the last input, merged with the keyboard.

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

#include "hle/kernel/kernel_internal.h"
#include "hle/xinput/input_filter.h"
#include "hle/xinput/xinput.h"

namespace {

using hp2k::input::AxisToFloat;
using hp2k::input::FloatToAxis;

struct Pad {
  SDL_Gamepad* p = nullptr;
  SDL_JoystickID id = 0;
  bool nintendo = false;
  bool busy = false;       // non-neutral at the last update
  Hp2PadState last;        // last raw state (activity detection)
  uint64_t active_ns = 0;  // last input (activity) time
  uint16_t rumble[2] = {};
};

std::vector<Pad> g_pads;  // every connected gamepad, in connection order
hp2k::input::InputSettings g_set;
hp2k::input::Smoother g_cam;
uint64_t g_last_update_ns = 0;
SDL_JoystickID g_active = 0, g_logged_active = 0;

// Event watch (called where SDL queues events): host time of the first
// gamepad input event per pad since the last update, for the latency stamp.
std::mutex g_ev_mu;
std::vector<std::pair<SDL_JoystickID, uint64_t>> g_ev_pending;

bool SDLCALL Watch(void*, SDL_Event* e) {
  SDL_JoystickID which;
  switch (e->type) {
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
      which = e->gaxis.which;
      break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
      which = e->gbutton.which;
      break;
    default:
      return true;
  }
  // SDL timestamps are SDL_GetTicksNS(); convert to the HLE's clock.
  uint64_t age = SDL_GetTicksNS() - std::min(e->common.timestamp, SDL_GetTicksNS());
  uint64_t t = hp2_xinput_now_ns() - age;
  std::lock_guard<std::mutex> l(g_ev_mu);
  for (auto& [id, ns] : g_ev_pending)
    if (id == which) return true;  // keep the first (oldest) one
  g_ev_pending.push_back({which, t});
  return true;
}

uint64_t TakeEventNs(SDL_JoystickID id) {
  std::lock_guard<std::mutex> l(g_ev_mu);
  for (size_t i = 0; i < g_ev_pending.size(); i++)
    if (g_ev_pending[i].first == id) {
      uint64_t t = g_ev_pending[i].second;
      g_ev_pending.erase(g_ev_pending.begin() + long(i));
      return t;
    }
  return 0;
}

const char* TypeName(SDL_GamepadType t) {
  switch (t) {
    case SDL_GAMEPAD_TYPE_XBOX360: return "Xbox 360";
    case SDL_GAMEPAD_TYPE_XBOXONE: return "Xbox One/Series";
    case SDL_GAMEPAD_TYPE_PS3: return "PS3";
    case SDL_GAMEPAD_TYPE_PS4: return "DualShock 4";
    case SDL_GAMEPAD_TYPE_PS5: return "DualSense";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: return "Switch Pro";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return "Joy-Con";
    default: return "generic";
  }
}

void Refresh() {
  // Drop disconnected pads, open new ones (hot-plug, no restart).
  g_pads.erase(std::remove_if(g_pads.begin(), g_pads.end(),
                              [](Pad& d) {
                                if (SDL_GamepadConnected(d.p)) return false;
                                hp2k::Log("xinput: gamepad %u disconnected", unsigned(d.id));
                                SDL_CloseGamepad(d.p);
                                return true;
                              }),
               g_pads.end());
  int n = 0;
  SDL_JoystickID* ids = SDL_GetGamepads(&n);
  for (int i = 0; ids && i < n; i++) {
    bool have = false;
    for (auto& d : g_pads) have = have || d.id == ids[i];
    if (have || g_pads.size() >= 8) continue;
    if (SDL_Gamepad* p = SDL_OpenGamepad(ids[i])) {
      Pad d;
      d.p = p;
      d.id = ids[i];
      SDL_GamepadType t = SDL_GetGamepadType(p);
      d.nintendo = t == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO ||
                   t == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT ||
                   t == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT ||
                   t == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR;
      hp2k::Log("xinput: gamepad %u connected: %s (%s)%s", unsigned(d.id),
                SDL_GetGamepadName(p) ? SDL_GetGamepadName(p) : "?", TypeName(t),
                d.nintendo && g_set.nintendo_layout == hp2k::input::NintendoLayout::kLabel
                    ? ", Nintendo buttons by label"
                    : "");
      g_pads.push_back(d);
    }
  }
  SDL_free(ids);
}

// SDL Y is down-positive, XInput up-positive. Negate in int: centre stays 0,
// -32768 (full up) clamps to 32767.
int16_t Axis(SDL_Gamepad* p, SDL_GamepadAxis a, bool flip) {
  int v = SDL_GetGamepadAxis(p, a);
  if (flip) v = -v;
  return int16_t(std::clamp(v, -32768, 32767));
}

uint8_t Trigger(SDL_Gamepad* p, SDL_GamepadAxis a) {
  return uint8_t(std::clamp(int(SDL_GetGamepadAxis(p, a)) * 255 / 32767, 0, 255));
}

// Raw state of one pad (no stick shaping).
Hp2PadState FromGamepad(const Pad& d) {
  SDL_Gamepad* p = d.p;
  Hp2PadState s;
  s.connected = true;
  auto b = [&](SDL_GamepadButton btn) { return SDL_GetGamepadButton(p, btn); };
  if (b(SDL_GAMEPAD_BUTTON_DPAD_UP)) s.buttons |= kPadDpadUp;
  if (b(SDL_GAMEPAD_BUTTON_DPAD_DOWN)) s.buttons |= kPadDpadDown;
  if (b(SDL_GAMEPAD_BUTTON_DPAD_LEFT)) s.buttons |= kPadDpadLeft;
  if (b(SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) s.buttons |= kPadDpadRight;
  if (b(SDL_GAMEPAD_BUTTON_START)) s.buttons |= kPadStart;
  if (b(SDL_GAMEPAD_BUTTON_BACK)) s.buttons |= kPadBack;
  if (b(SDL_GAMEPAD_BUTTON_LEFT_STICK)) s.buttons |= kPadLeftThumb;
  if (b(SDL_GAMEPAD_BUTTON_RIGHT_STICK)) s.buttons |= kPadRightThumb;
  // Positional by default (bottom face button = A); "label" on Nintendo pads
  // maps their letters instead (A is east, X north).
  bool label = d.nintendo && g_set.nintendo_layout == hp2k::input::NintendoLayout::kLabel;
  s.analog[kPadA] = b(label ? SDL_GAMEPAD_BUTTON_EAST : SDL_GAMEPAD_BUTTON_SOUTH) ? 255 : 0;
  s.analog[kPadB] = b(label ? SDL_GAMEPAD_BUTTON_SOUTH : SDL_GAMEPAD_BUTTON_EAST) ? 255 : 0;
  s.analog[kPadX] = b(label ? SDL_GAMEPAD_BUTTON_NORTH : SDL_GAMEPAD_BUTTON_WEST) ? 255 : 0;
  s.analog[kPadY] = b(label ? SDL_GAMEPAD_BUTTON_WEST : SDL_GAMEPAD_BUTTON_NORTH) ? 255 : 0;
  s.analog[kPadBlack] = b(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) ? 255 : 0;
  s.analog[kPadWhite] = b(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) ? 255 : 0;
  s.analog[kPadLT] = Trigger(p, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
  s.analog[kPadRT] = Trigger(p, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
  s.lx = Axis(p, SDL_GAMEPAD_AXIS_LEFTX, false);
  s.ly = Axis(p, SDL_GAMEPAD_AXIS_LEFTY, true);
  s.rx = Axis(p, SDL_GAMEPAD_AXIS_RIGHTX, false);
  s.ry = Axis(p, SDL_GAMEPAD_AXIS_RIGHTY, true);
  return s;
}

// Non-neutral: a button or trigger pressed, or a stick outside its dead zone.
bool Busy(const Hp2PadState& s) {
  if (s.buttons) return true;
  for (uint8_t a : s.analog)
    if (a > 30) return true;
  auto out = [](int16_t x, int16_t y, float inner) {
    float fx = AxisToFloat(x), fy = AxisToFloat(y);
    return fx * fx + fy * fy > inner * inner;
  };
  return out(s.lx, s.ly, std::max(g_set.move.inner, 0.1f)) ||
         out(s.rx, s.ry, std::max(g_set.camera.inner, 0.1f));
}

// Sticks of the active pad through input_filter.h.
void Shape(Hp2PadState& s, double dt_s) {
  float lx = AxisToFloat(s.lx), ly = AxisToFloat(s.ly);
  hp2k::input::ShapeStick(g_set.move, lx, ly);
  s.lx = FloatToAxis(lx), s.ly = FloatToAxis(ly);
  float rx = AxisToFloat(s.rx), ry = AxisToFloat(s.ry);
  hp2k::input::ShapeStick(g_set.camera, rx, ry);
  g_cam.Apply(g_set.camera.smoothing_ms, dt_s, rx, ry);
  s.rx = FloatToAxis(rx), s.ry = FloatToAxis(ry);
}

void MergeKeyboard(Hp2PadState& s) {
  const bool* k = SDL_GetKeyboardState(nullptr);
  if (!k) return;
  auto stick = [&](SDL_Scancode neg, SDL_Scancode pos, int16_t& v) {
    if (k[neg] && !k[pos]) v = -32768;
    else if (k[pos] && !k[neg]) v = 32767;
  };
  stick(SDL_SCANCODE_A, SDL_SCANCODE_D, s.lx);
  stick(SDL_SCANCODE_S, SDL_SCANCODE_W, s.ly);
  stick(SDL_SCANCODE_J, SDL_SCANCODE_L, s.rx);
  stick(SDL_SCANCODE_K, SDL_SCANCODE_I, s.ry);
  struct Btn {
    SDL_Scancode key;
    uint16_t bit;
  };
  for (Btn b : {Btn{SDL_SCANCODE_UP, kPadDpadUp}, Btn{SDL_SCANCODE_DOWN, kPadDpadDown},
                Btn{SDL_SCANCODE_LEFT, kPadDpadLeft}, Btn{SDL_SCANCODE_RIGHT, kPadDpadRight},
                Btn{SDL_SCANCODE_RETURN, kPadStart}, Btn{SDL_SCANCODE_ESCAPE, kPadBack},
                Btn{SDL_SCANCODE_BACKSPACE, kPadBack}, Btn{SDL_SCANCODE_F, kPadLeftThumb},
                Btn{SDL_SCANCODE_H, kPadRightThumb}})
    if (k[b.key]) s.buttons |= b.bit;
  struct Analog {
    SDL_Scancode key;
    int idx;
  };
  for (Analog a : {Analog{SDL_SCANCODE_SPACE, kPadA}, Analog{SDL_SCANCODE_E, kPadB},
                   Analog{SDL_SCANCODE_Q, kPadX}, Analog{SDL_SCANCODE_R, kPadY},
                   Analog{SDL_SCANCODE_1, kPadBlack}, Analog{SDL_SCANCODE_2, kPadWhite},
                   Analog{SDL_SCANCODE_U, kPadLT}, Analog{SDL_SCANCODE_O, kPadRT}})
    if (k[a.key]) s.analog[a.idx] = 255;
}

// The game's motors (port 0) -> the active pad, scaled by rumble_strength;
// other pads are stopped. Re-sent every 500 ms while on (SDL effects time out).
void Rumble(uint64_t now) {
  static uint64_t last_send = 0;
  uint16_t l, r;
  hp2_xinput_get_rumble(0, &l, &r);
  l = uint16_t(uint32_t(l) * uint32_t(g_set.rumble_strength) / 100);
  r = uint16_t(uint32_t(r) * uint32_t(g_set.rumble_strength) / 100);
  for (Pad& d : g_pads) {
    uint16_t want_l = d.id == g_active ? l : 0, want_r = d.id == g_active ? r : 0;
    bool changed = want_l != d.rumble[0] || want_r != d.rumble[1];
    bool refresh = (want_l || want_r) && now - last_send > 500'000'000ull;
    if (!changed && !refresh) continue;
    SDL_RumbleGamepad(d.p, want_l, want_r, 1000);
    d.rumble[0] = want_l, d.rumble[1] = want_r;
    last_send = now;
  }
}

// Silent background runs and scripted runs never read a real device.
bool Off() {
  static const bool background = [] {
    const char* b = SDL_getenv("HP2_BACKGROUND");
    return b && *b && *b != '0';
  }();
  return background || hp2_xinput_script_active();
}

}  // namespace

bool hp2_xinput_sdl_init() {
  if (Off()) return true;
  g_set = hp2k::input::LoadSettings();
  if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) return false;
  SDL_AddEventWatch(Watch, nullptr);
  return true;
}

void hp2_xinput_sdl_update() {
  if (Off()) return;
  SDL_UpdateGamepads();
  Refresh();
  uint64_t now = hp2_xinput_now_ns();
  double dt = g_last_update_ns ? (now - g_last_update_ns) / 1e9 : 0.0;
  g_last_update_ns = now;
  // Activity: a pad that changes while non-neutral, or becomes so, is the
  // one the player is using.
  for (Pad& d : g_pads) {
    Hp2PadState s = FromGamepad(d);
    bool busy = Busy(s);
    if (busy && (!d.busy || std::memcmp(&s, &d.last, sizeof s) != 0)) d.active_ns = now;
    d.busy = busy;
    d.last = s;
  }
  const Pad* act = nullptr;
  for (const Pad& d : g_pads)
    if (!act || d.active_ns > act->active_ns) act = &d;
  g_active = act ? act->id : 0;
  if (g_active != g_logged_active) {
    if (act)
      hp2k::Log("xinput: active gamepad %u (%s)", unsigned(g_active),
                SDL_GetGamepadName(act->p) ? SDL_GetGamepadName(act->p) : "?");
    g_logged_active = g_active;
    g_cam = {};
  }
  Hp2PadState s;
  uint64_t input_ns = 0;
  if (act) {
    s = act->last;
    Shape(s, dt);
    input_ns = TakeEventNs(act->id);
  }
  {
    std::lock_guard<std::mutex> l(g_ev_mu);
    g_ev_pending.clear();  // events of other pads
  }
  s.connected = true;  // the keyboard is always "controller 1"
  MergeKeyboard(s);
  hp2_xinput_set_pad_at(0, s, input_ns);
  for (int port = 1; port < 4; port++) hp2_xinput_set_pad(port, Hp2PadState{});
  Rumble(now);
}
