// Unit test of the [video] settings.toml helper: out/src/hle/d3d8/hp2_d3d8_video_settings_test.
#include "hle/d3d8/video_settings.h"
#include <cstdio>
using namespace hp2d3d;
int fails = 0;
void eq(const std::string& a, const std::string& b, const char* what) {
  if (a != b) { printf("FAIL %s:\n--got--\n%s--want--\n%s", what, a.c_str(), b.c_str()); fails++; }
}
int main() {
  if (!VideoSettingBool("", "fullscreen", true)) fails++, puts("FAIL default");
  if (VideoSettingBool("[video]\nfullscreen = false\n", "fullscreen", true)) fails++, puts("FAIL false");
  if (VideoSettingBool("[gamepad]\nfullscreen = false\n", "fullscreen", true) != true) fails++, puts("FAIL other section");
  if (VideoSettingBool("[Video]\n FullScreen = \"off\" # x\n", "fullscreen", true)) fails++, puts("FAIL quoted");
  eq(WithVideoSetting("", "fullscreen", "false"), "[video]\nfullscreen = false\n", "empty");
  eq(WithVideoSetting("[gamepad]\nlayout = xbox\n", "fullscreen", "true"),
     "[gamepad]\nlayout = xbox\n\n[video]\nfullscreen = true\n", "append section");
  eq(WithVideoSetting("[video]\nlow_latency = true\n\n[gamepad]\nx = 1\n", "fullscreen", "false"),
     "[video]\nlow_latency = true\nfullscreen = false\n\n[gamepad]\nx = 1\n", "insert in section");
  eq(WithVideoSetting("# c\n[video]\nfullscreen = true # keep\n[camera]\ny=2\n", "fullscreen", "false"),
     "# c\n[video]\nfullscreen = false\n[camera]\ny=2\n", "replace");
  printf(fails ? "%d failures\n" : "video_settings: all passed\n", fails);
  return fails != 0;
}
