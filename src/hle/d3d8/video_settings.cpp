// [video] section of settings.toml: see video_settings.h.

#include "hle/d3d8/video_settings.h"

#include <sys/stat.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace hp2d3d {
namespace {

std::string Trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(uint8_t(s[a]))) a++;
  while (b > a && std::isspace(uint8_t(s[b - 1]))) b--;
  return s.substr(a, b - a);
}

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(uint8_t(c)));
  return s;
}

std::vector<std::string> Lines(const std::string& text) {
  std::vector<std::string> out;
  size_t p = 0;
  while (p < text.size()) {
    size_t q = text.find('\n', p);
    if (q == std::string::npos) q = text.size();
    out.push_back(text.substr(p, q - p));
    p = q + 1;
  }
  return out;
}

// "[name]" -> "name" (lower case); "" if the line is not a section header.
std::string Section(const std::string& line) {
  std::string t = Trim(line);
  if (t.size() < 2 || t.front() != '[' || t.back() != ']') return "";
  return Lower(Trim(t.substr(1, t.size() - 2)));
}

// The key of a "key = value" line (lower case), its value (unquoted, without
// a trailing # comment); false for other lines.
bool KeyValue(const std::string& line, std::string* key, std::string* value) {
  std::string t = Trim(line);
  if (t.empty() || t[0] == '#' || t[0] == '[') return false;
  size_t eq = t.find('=');
  if (eq == std::string::npos) return false;
  *key = Lower(Trim(t.substr(0, eq)));
  std::string v = Trim(t.substr(eq + 1));
  if (!v.empty() && v[0] == '"') {
    size_t e = v.find('"', 1);
    v = v.substr(1, e == std::string::npos ? std::string::npos : e - 1);
  } else if (size_t h = v.find('#'); h != std::string::npos) {
    v = Trim(v.substr(0, h));
  }
  *value = v;
  return true;
}

}  // namespace

std::string VideoSettingsPath() {
  const char* home = getenv("HOME");
  return std::string(home && *home ? home : ".") + "/Library/Application Support/HP2/settings.toml";
}

bool VideoSettingBool(const std::string& text, const char* key, bool def) {
  std::string section, k, v, want = Lower(key);
  for (const std::string& line : Lines(text)) {
    if (std::string s = Section(line); !s.empty()) {
      section = s;
      continue;
    }
    if (section != "video" || !KeyValue(line, &k, &v) || k != want) continue;
    v = Lower(v);
    if (v == "true" || v == "1" || v == "yes" || v == "on") return true;
    if (v == "false" || v == "0" || v == "no" || v == "off") return false;
    return def;
  }
  return def;
}

std::string WithVideoSetting(const std::string& text, const char* key, const std::string& value) {
  std::vector<std::string> lines = Lines(text);
  std::string want = Lower(key), section, k, v;
  std::string entry = std::string(key) + " = " + value;
  int video_end = -1;  // index after the last line of [video]
  for (size_t i = 0; i < lines.size(); i++) {
    if (std::string s = Section(lines[i]); !s.empty()) {
      section = s;
      if (s == "video") video_end = int(i) + 1;
      continue;
    }
    if (section != "video") continue;
    if (KeyValue(lines[i], &k, &v) && k == want) {
      lines[i] = entry;
      video_end = -2;
      break;
    }
    if (!Trim(lines[i]).empty()) video_end = int(i) + 1;
  }
  if (video_end >= 0) {
    lines.insert(lines.begin() + video_end, entry);
  } else if (video_end == -1) {
    if (!lines.empty() && !Trim(lines.back()).empty()) lines.push_back("");
    lines.push_back("[video]");
    lines.push_back(entry);
  }
  std::string out;
  for (const std::string& l : lines) out += l + "\n";
  return out;
}

std::string ReadTextFile(const std::string& path) {
  std::string text;
  if (FILE* f = fopen(path.c_str(), "rb")) {
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    fclose(f);
  }
  return text;
}

bool WriteTextFile(const std::string& path, const std::string& text) {
  for (size_t p = path.find('/', 1); p != std::string::npos; p = path.find('/', p + 1))
    mkdir(path.substr(0, p).c_str(), 0755);
  std::string tmp = path + ".tmp";
  FILE* f = fopen(tmp.c_str(), "wb");
  if (!f) return false;
  bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
  ok = fclose(f) == 0 && ok;
  return ok && rename(tmp.c_str(), path.c_str()) == 0;
}

}  // namespace hp2d3d
