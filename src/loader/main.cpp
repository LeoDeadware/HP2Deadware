// hp2: runs the lifted default.xbe.
//
//   hp2 [path/to/default.xbe]
//
// Environment: HP2_DATA (main checkout, for the default XBE path),
// HP2_LOG_DIR (runtime.log there), HP2_KTRACE=0 (no kernel call trace),
// HP2_STOP_AT=<section> (stop at the first call into it, e.g. D3D).

#include <cstdio>
#include <string>

#include "runtime/hp2_internal.h"

#include <climits>
#include <cstdlib>
#include <sys/stat.h>

// No HP2_DATA: the nearest ancestor of the executable with game/extracted/default.xbe (a repo
// build: out/src/loader/hp2 -> the checkout), else the current directory. No baked-in home
// path: the DMG's build kit ships main.o (tests/scripts/dmg_no_game_code.sh).
static std::string DefaultData(const char* argv0) {
  char real[PATH_MAX];
  if (!realpath(argv0, real)) return ".";
  std::string d = real;
  struct stat st;
  for (size_t at = d.rfind('/'); at != std::string::npos && at > 0; at = d.rfind('/')) {
    d.resize(at);
    if (stat((d + "/game/extracted/default.xbe").c_str(), &st) == 0) return d;
  }
  return ".";
}
#if HP2_HAVE_XINPUT
#include "hle/xinput/xinput.h"
#endif

// Kernel HLE: `hp2 --help` text (kernel_exports.h); weak, as the kernel may be absent.
const char* hp2_help_text() __attribute__((weak));
// Polish (src/polish): reads its switches once, before the D3D8 HLE starts.
void hp2_polish_init() __attribute__((weak));

int main(int argc, char** argv) {
  using namespace hp2rt;
  if (argc > 1 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
    std::fputs(hp2_help_text ? hp2_help_text() : "usage: hp2 [path/to/default.xbe]\n", stdout);
    return 0;
  }
  OpenLog();
  if (hp2_polish_init) hp2_polish_init();
  std::string xbe = argc > 1 ? argv[1] : Env("HP2_XBE");
  if (xbe.empty())
    xbe = Env("HP2_DATA", DefaultData(argv[0]).c_str()) + "/game/extracted/default.xbe";
#if HP2_HAVE_XINPUT
  // Queued now, applied when Boot builds the dispatch table.
  size_t n = 0;
  const Hp2Override* o = hp2_xinput_overrides(&n);
  for (size_t i = 0; i < n; i++) hp2_register_override_at_start(o[i].va, o[i].fn, o[i].name);
#endif
  Boot(xbe);
  RunMain();
}
