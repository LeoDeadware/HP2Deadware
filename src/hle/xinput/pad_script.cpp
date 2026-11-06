// Pad script parser and evaluator (see pad_script.h).

#include "hle/xinput/pad_script.h"

#include <cmath>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <strings.h>

namespace hp2k::xinput {

namespace {

constexpr int kAnalog = 0x100;  // PadEvent::target for analog button i: kAnalog + i
constexpr uint64_t kForever = std::numeric_limits<uint64_t>::max();
constexpr uint64_t kPressFrames = 6;

struct Name {
  const char* name;
  int target;
};
const Name kButtons[] = {
    {"A", kAnalog + kPadA},         {"B", kAnalog + kPadB},
    {"X", kAnalog + kPadX},         {"Y", kAnalog + kPadY},
    {"BLACK", kAnalog + kPadBlack}, {"WHITE", kAnalog + kPadWhite},
    {"LT", kAnalog + kPadLT},       {"RT", kAnalog + kPadRT},
    {"START", kPadStart},           {"BACK", kPadBack},
    {"DPAD_UP", kPadDpadUp},        {"DPAD_DOWN", kPadDpadDown},
    {"DPAD_LEFT", kPadDpadLeft},    {"DPAD_RIGHT", kPadDpadRight},
    {"UP", kPadDpadUp},             {"DOWN", kPadDpadDown},
    {"LEFT", kPadDpadLeft},         {"RIGHT", kPadDpadRight},
    {"LTHUMB", kPadLeftThumb},      {"RTHUMB", kPadRightThumb},
};

bool ParseButton(const std::string& s, int* target) {
  for (const Name& n : kButtons)
    if (!strcasecmp(s.c_str(), n.name)) {
      *target = n.target;
      return true;
    }
  return false;
}

bool ParseSide(const std::string& s, int* side) {
  if (!strcasecmp(s.c_str(), "L")) *side = 0;
  else if (!strcasecmp(s.c_str(), "R")) *side = 1;
  else return false;
  return true;
}

bool ParseU64(const std::string& s, uint64_t* v) {
  if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos) return false;
  *v = std::strtoull(s.c_str(), nullptr, 10);
  return true;
}

bool ParseFloat(const std::string& s, float lo, float hi, float* v) {
  char* end = nullptr;
  double d = std::strtod(s.c_str(), &end);
  if (s.empty() || *end || !(d >= lo && d <= hi)) return false;
  *v = float(d);
  return true;
}

int16_t Axis(float v) { return int16_t(std::lround(v * (v < 0 ? 32768.0f : 32767.0f))); }
float AxisF(int16_t v) { return v < 0 ? v / 32768.0f : v / 32767.0f; }

// Compact "<state>": "-" or BTN / LX=n ... joined by '+'.
bool ParseCompactState(const std::string& s, Hp2PadState* p) {
  *p = Hp2PadState{};
  p->connected = true;
  if (s == "-") return true;
  std::istringstream ss(s);
  for (std::string t; std::getline(ss, t, '+');) {
    int target;
    size_t eq = t.find('=');
    if (eq == std::string::npos) {
      if (!ParseButton(t, &target)) return false;
      if (target >= kAnalog) p->analog[target - kAnalog] = 255;
      else p->buttons |= uint16_t(target);
      continue;
    }
    std::string k = t.substr(0, eq), v = t.substr(eq + 1);
    char* end = nullptr;
    long n = std::strtol(v.c_str(), &end, 10);
    if (v.empty() || *end || n < -32768 || n > 32767) return false;
    int16_t* axis = !strcasecmp(k.c_str(), "LX") ? &p->lx : !strcasecmp(k.c_str(), "LY") ? &p->ly
                  : !strcasecmp(k.c_str(), "RX") ? &p->rx : !strcasecmp(k.c_str(), "RY") ? &p->ry
                  : nullptr;
    if (!axis) return false;
    *axis = int16_t(n);
  }
  return true;
}

}  // namespace

bool ParsePadScript(const std::string& text, std::vector<PadEvent>* out, std::string* error) {
  out->clear();
  std::istringstream in(text);
  std::string raw;
  uint64_t prev = 0;
  for (int line = 1; std::getline(in, raw); line++) {
    std::string src = raw.substr(0, raw.find('#'));
    std::istringstream ls(src);
    std::vector<std::string> w;
    for (std::string t; ls >> t;) w.push_back(t);
    if (w.empty()) continue;
    auto fail = [&](const std::string& why) {
      *error = "line " + std::to_string(line) + ": " + why + " (\"" + raw + "\")";
      return false;
    };
    if (w[0].find(':') != std::string::npos) {  // compact form: several events per line
      if (out->empty()) {
        PadEvent d;
        d.op = PadEvent::kDisconnect;
        d.text = "(compact script: disconnected until frame 1)";
        out->push_back(d);
        PadEvent c = d;
        c.frame = 1;
        c.op = PadEvent::kConnect;
        c.text = "(compact script: connect)";
        out->push_back(c);
      }
      for (const std::string& t : w) {
        PadEvent e;
        e.op = PadEvent::kSet;
        e.line = line;
        e.text = t;
        size_t colon = t.find(':');
        if (colon == std::string::npos || !ParseU64(t.substr(0, colon), &e.frame))
          return fail("bad event '" + t + "'");
        if (!ParseCompactState(t.substr(colon + 1), &e.pad)) return fail("bad state in '" + t + "'");
        if (e.frame < prev) return fail("frames must not decrease");
        prev = e.frame;
        out->push_back(std::move(e));
      }
      continue;
    }
    PadEvent e;
    e.line = line;
    e.text = src.substr(src.find_first_not_of(" \t"));
    while (!e.text.empty() && (e.text.back() == ' ' || e.text.back() == '\t' || e.text.back() == '\r'))
      e.text.pop_back();
    bool rel = w[0][0] == '+';
    if (!ParseU64(rel ? w[0].substr(1) : w[0], &e.frame)) return fail("bad frame");
    if (rel) e.frame += prev;
    if (e.frame < prev) return fail("frames must not decrease");
    prev = e.frame;
    if (w.size() < 2) return fail("missing command");
    const std::string& cmd = w[1];
    size_t nargs = w.size() - 2;
    // Optional trailing duration at index `at`.
    auto duration = [&](size_t at, uint64_t def) {
      if (w.size() <= at) {
        e.frames = def;
        return true;
      }
      return w.size() == at + 1 && ParseU64(w[at], &e.frames) && e.frames > 0;
    };
    if (!strcasecmp(cmd.c_str(), "press") || !strcasecmp(cmd.c_str(), "hold")) {
      e.op = PadEvent::kHold;
      if (nargs < 1 || !ParseButton(w[2], &e.target)) return fail("unknown button");
      bool press = !strcasecmp(cmd.c_str(), "press");
      if (!duration(3, press ? kPressFrames : 0)) return fail("bad duration");
    } else if (!strcasecmp(cmd.c_str(), "release")) {
      if (nargs != 1) return fail("release takes one button or 'all'");
      if (!strcasecmp(w[2].c_str(), "all")) e.op = PadEvent::kReleaseAll;
      else if (ParseButton(w[2], &e.target)) e.op = PadEvent::kRelease;
      else if (w[2].size() == 6 && !strncasecmp(w[2].c_str(), "stick", 5) &&
               ParseSide(w[2].substr(5), &e.target)) {
        e.op = PadEvent::kStick;  // stickL / stickR: back to centre
        e.frames = 0;
      } else return fail("unknown button");
    } else if (!strcasecmp(cmd.c_str(), "stick")) {
      e.op = PadEvent::kStick;
      if (nargs < 3 || !ParseSide(w[2], &e.target)) return fail("stick L|R x y [frames]");
      if (!ParseFloat(w[3], -1, 1, &e.x) || !ParseFloat(w[4], -1, 1, &e.y))
        return fail("stick values must be -1.0..1.0");
      if (!duration(5, 0)) return fail("bad duration");
    } else if (!strcasecmp(cmd.c_str(), "trigger")) {
      e.op = PadEvent::kTrigger;
      if (nargs < 2 || !ParseSide(w[2], &e.target)) return fail("trigger L|R value [frames]");
      if (!ParseFloat(w[3], 0, 1, &e.x)) return fail("trigger value must be 0.0..1.0");
      if (!duration(4, 0)) return fail("bad duration");
    } else if (!strcasecmp(cmd.c_str(), "connect") || !strcasecmp(cmd.c_str(), "disconnect")) {
      if (nargs) return fail("connect/disconnect take no arguments");
      e.op = !strcasecmp(cmd.c_str(), "connect") ? PadEvent::kConnect : PadEvent::kDisconnect;
    } else {
      return fail("unknown command '" + cmd + "'");
    }
    out->push_back(std::move(e));
  }
  return true;
}

Hp2PadState PadStateAt(const std::vector<PadEvent>& events, uint64_t frame) {
  // Channels: 8 digital bits, 8 analog buttons, 2 sticks; each with a value
  // and the first frame it no longer applies.
  struct Chan {
    uint64_t until = 0;
    float x = 0, y = 0;
  };
  Chan digital[8], analog[8], stick[2];
  bool connected = true;
  auto end = [](const PadEvent& e) { return e.frames ? e.frame + e.frames : kForever; };
  for (const PadEvent& e : events) {
    if (e.frame > frame) break;
    switch (e.op) {
      case PadEvent::kSet:
        for (int i = 0; i < 8; i++) {
          digital[i] = {e.pad.buttons >> i & 1 ? kForever : e.frame, 1, 0};
          analog[i] = {e.pad.analog[i] ? kForever : e.frame, e.pad.analog[i] / 255.0f, 0};
        }
        stick[0] = {kForever, AxisF(e.pad.lx), AxisF(e.pad.ly)};
        stick[1] = {kForever, AxisF(e.pad.rx), AxisF(e.pad.ry)};
        break;
      case PadEvent::kHold: {
        Chan& c = e.target >= kAnalog ? analog[e.target - kAnalog]
                                      : digital[__builtin_ctz(unsigned(e.target))];
        c.until = end(e);
        c.x = 1;
        break;
      }
      case PadEvent::kRelease:
        (e.target >= kAnalog ? analog[e.target - kAnalog]
                             : digital[__builtin_ctz(unsigned(e.target))]).until = e.frame;
        break;
      case PadEvent::kReleaseAll:
        for (Chan* set : {digital, analog})
          for (int i = 0; i < 8; i++) set[i].until = e.frame;
        stick[0].until = stick[1].until = e.frame;
        break;
      case PadEvent::kStick:
        stick[e.target] = {end(e), e.x, e.y};
        break;
      case PadEvent::kTrigger:
        analog[e.target ? kPadRT : kPadLT] = {end(e), e.x, 0};
        break;
      case PadEvent::kConnect:
      case PadEvent::kDisconnect:
        connected = e.op == PadEvent::kConnect;
        break;
    }
  }
  Hp2PadState s;
  s.connected = connected;
  if (!connected) return s;
  for (int i = 0; i < 8; i++) {
    if (digital[i].until > frame) s.buttons |= uint16_t(1u << i);
    if (analog[i].until > frame) s.analog[i] = uint8_t(std::lround(analog[i].x * 255));
  }
  if (stick[0].until > frame) s.lx = Axis(stick[0].x), s.ly = Axis(stick[0].y);
  if (stick[1].until > frame) s.rx = Axis(stick[1].x), s.ry = Axis(stick[1].y);
  return s;
}

}  // namespace hp2k::xinput
