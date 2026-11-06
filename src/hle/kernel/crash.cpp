// Crash reports for players: the last log lines plus guest and host state,
// written to ~/Library/Logs/hp2/ on a fatal error (kernel_exports.h).

#include <cxxabi.h>
#include <execinfo.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <string>

#include "hle/kernel/kernel_internal.h"

// Render's Swap counter (d3d8.h); 0 when no D3D HLE is linked in.
__attribute__((weak)) uint64_t hp2_d3d8_frame_count();

namespace hp2k {
namespace {

constexpr size_t kTailLines = 300;
std::mutex g_tail_mu;
std::deque<std::string> g_tail;
std::string g_report_path;

std::string Demangle(const char* sym) {
  // "<idx> <image> <addr> <mangled> + <off>"
  std::string s = sym;
  size_t a = s.find(" _Z");
  if (a == std::string::npos) return s;
  size_t b = s.find(' ', a + 1);
  std::string mangled = s.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
  int st = 0;
  char* d = abi::__cxa_demangle(mangled.c_str(), nullptr, nullptr, &st);
  if (st != 0 || !d) return s;
  std::string out = s.substr(0, a + 1) + d + (b == std::string::npos ? "" : s.substr(b));
  std::free(d);
  return out;
}

bool Readable(uint32_t va) {
  // Guest memory the kernel knows to be committed: the stack of a guest
  // thread lies inside its KPCR's NtTib bounds, checked by the caller.
  return va >= 0x10000;
}

void GuestState(FILE* f) {
  recomp::Ctx* c = g_host.current_ctx ? g_host.current_ctx() : nullptr;
  if (!c || !g_host.guest_base) {
    std::fputs("guest thread: none on this host thread\n", f);
    return;
  }
  std::fprintf(f, "guest registers:\n  eax=%08X ecx=%08X edx=%08X ebx=%08X\n"
                  "  esp=%08X ebp=%08X esi=%08X edi=%08X fs=%08X\n",
               c->eax, c->ecx, c->edx, c->ebx, c->esp, c->ebp, c->esi, c->edi, c->fs_base);
  uint32_t lo = c->esp, hi = 0;
  if (c->fs_base && Readable(c->fs_base)) hi = Rd32(c->fs_base + 4);  // NtTib.StackBase
  if (!hi || hi <= lo || hi - lo > 0x100000) hi = lo + 0x4000;
  std::fprintf(f, "guest call chain (return addresses into game code, innermost first; "
                  "stack %08X..%08X):\n ", lo, hi);
  int shown = 0;
  for (uint32_t a = lo & ~3u; a + 4 <= hi && shown < 48; a += 4) {
    uint32_t v = Rd32(a);
    bool code = g_host.code_hi ? (v >= g_host.code_lo && v < g_host.code_hi)
                               : (v >= 0x11000 && v < 0x17DA68);
    if (!code) continue;
    std::fprintf(f, " %08X", v);
    if (++shown % 8 == 0) std::fputs("\n ", f);
  }
  std::fputs(shown ? "\n" : " (none found)\n", f);
}

}  // namespace

void CrashNote(const char* line) {
  std::unique_lock<std::mutex> l(g_tail_mu, std::try_to_lock);
  if (!l.owns_lock()) return;  // never block a logging thread
  g_tail.emplace_back(line);
  if (g_tail.size() > kTailLines) g_tail.pop_front();
}

}  // namespace hp2k

using namespace hp2k;

void hp2_crash_note(const char* line) { CrashNote(line); }

const char* hp2_crash_report(const char* reason) {
  static std::mutex once_mu;
  std::lock_guard<std::mutex> once(once_mu);
  if (!g_report_path.empty()) return g_report_path.c_str();  // first fatal only
  std::string dir;
  if (const char* d = std::getenv("HP2_CRASH_DIR"); d && *d) dir = d;
  else dir = std::string(std::getenv("HOME") ? std::getenv("HOME") : "/tmp") + "/Library/Logs/hp2";
  std::string mk = "/bin/mkdir -p '" + dir + "'";
  if (std::system(mk.c_str()) != 0) return "";
  std::time_t now = std::time(nullptr);
  char stamp[32];
  std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", std::localtime(&now));
  std::string path = dir + "/hp2-crash-" + stamp + ".txt";
  FILE* f = std::fopen(path.c_str(), "w");
  if (!f) return "";
  char when[64];
  std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", std::localtime(&now));
  std::fprintf(f, "HP2Port crash report\n====================\n\n"
                  "When:   %s\nReason: %s\n\n"
                  "What to do: send this file with a note on what you were doing.\n"
                  "Your saves are not touched by a crash (profile below, hdd/partition1/UDATA).\n\n",
               when, reason ? reason : "?");
  std::fprintf(f, "settings:\n  profile  %s\n  language %s\n  frame    %llu (vblank %llu)\n",
               g_data_dir.c_str(), LanguageKey().c_str(),
               (unsigned long long)(&hp2_d3d8_frame_count ? hp2_d3d8_frame_count() : 0), (unsigned long long)hp2_vblank_count());
  extern char** environ;
  for (char** e = environ; e && *e; e++)
    if (!std::strncmp(*e, "HP2_", 4) || !std::strncmp(*e, "SDL_", 4)) std::fprintf(f, "  %s\n", *e);
  std::fputs("\n", f);
  GuestState(f);
  std::fputs("\nhost backtrace:\n", f);
  void* frames[64];
  int n = backtrace(frames, 64);
  if (char** syms = backtrace_symbols(frames, n)) {
    for (int i = 0; i < n; i++) std::fprintf(f, "  %s\n", Demangle(syms[i]).c_str());
    std::free(syms);
  }
  std::fputs("\nlast log lines:\n", f);
  {
    std::unique_lock<std::mutex> l(g_tail_mu, std::try_to_lock);
    if (l.owns_lock())
      for (const std::string& s : g_tail) std::fprintf(f, "  %s\n", s.c_str());
    else
      std::fputs("  (log busy)\n", f);
  }
  std::fclose(f);
  g_report_path = path;
  std::fprintf(stderr, "hp2: crash report written to %s\n", path.c_str());
  return g_report_path.c_str();
}
