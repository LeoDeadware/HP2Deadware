// Dev switch HP2_SKIP_MOVIES=1: the game's movie player returns at once.
//
// 0x18D40 is the game's PlayMovie(const char* path, int skippable) (cdecl;
// 12 callers in 0xFFC20..0xFFFE0 pass D:\ea_logo_*.xmv, D:\hp*x.xmv ...).
// Movie A/V sync (measured 2026-10-31): the player busy-polls 0x14ED3D until
// a frame is due by the vblank count plus an offset that the audio stream's
// completion callback sets (ms since start - completed packet's timestamp),
// i.e. video follows the DSound stream's completion clock. Every movie log
// line compares wall time, rdtsc and vblanks.

#include <algorithm>
#include <chrono>
#include <cstdlib>

#include "hle/kernel/kernel_exports.h"
#include "runtime/hp2_runtime.h"

void hp2_d3d8_movie_event(bool start);  // d3d8.cpp

namespace {

constexpr uint32_t kPlayMovie = 0x18D40;

void PlayMovie(Hp2Ctx& c) {
  static const bool skip = [] {
    const char* v = getenv("HP2_SKIP_MOVIES");
    return v && atoi(v) != 0;
  }();
  uint32_t path = hp2_arg(c, 0);
  const char* name = path ? static_cast<const char*>(hp2_ptr(path)) : "?";
  if (skip) {
    hp2_log("movies: skipping %s (HP2_SKIP_MOVIES)", name);
    hp2_ret_cdecl(c, 0);
    return;
  }
  hp2_log("movies: playing %s", name);
  hp2_d3d8_movie_event(true);
  uint64_t tsc0 = hp2_kernel_rdtsc(), vb0 = hp2_vblank_count();
  auto w0 = std::chrono::steady_clock::now();
  hp2_original(kPlayMovie)(c);
  hp2_d3d8_movie_event(false);
  double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count();
  hp2_log("movies: %.3f s wall, %.3f s by rdtsc (733.33 MHz), %.3f s by vblanks (60 Hz)", wall,
          double(hp2_kernel_rdtsc() - tsc0) / 733333333.0,
          double(hp2_vblank_count() - vb0) / 60.0);
}

HP2_OVERRIDE(kPlayMovie, PlayMovie);

// Timing of the XMV player's per-iteration call (0x14ED3D: decode / next
// frame, called by the movie loop at 0x18DB4): logged every 300 calls that
// produced a frame (HP2_XMV_TRACE=1), to tell decoder cost from presentation cost.
constexpr uint32_t kXmvGetNextFrame = 0x14ED3D;
void XmvGetNextFrame(Hp2Ctx& c) {
  static double sum_ms = 0, max_ms = 0, busy_ms = 0;
  static uint32_t frames = 0, calls = 0;
  uint32_t status_ptr = hp2_arg(c, 2);
  auto t0 = std::chrono::steady_clock::now();
  hp2_original(kXmvGetNextFrame)(c);
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  calls++;
  busy_ms += ms;
  static const bool trace = getenv("HP2_XMV_TRACE") != nullptr;
  if (trace && status_ptr && hp2_rd32(status_ptr) == 1) {  // a new frame was decoded
    sum_ms += ms;
    max_ms = std::max(max_ms, ms);
    if (++frames % 300 == 0) {
      hp2_log("movies: XMV decode %.2f ms avg, %.1f ms max per frame; %u calls per frame, "
              "%.2f ms per frame in the player in total",
              sum_ms / 300, max_ms, calls / 300, busy_ms / 300);
      sum_ms = max_ms = busy_ms = 0;
      calls = 0;
    }
  }
}
HP2_OVERRIDE(kXmvGetNextFrame, XmvGetNextFrame);

// The XMV audio stream's packet-completion callback (0x14EB6F, called by the
// DSound HLE): sets the video clock offset [ctx+0xC8] = ms since start -
// the completed packet's timestamp. Logged (HP2_XMV_TRACE=1) to see how far
// audio completion lags real time.
constexpr uint32_t kXmvAudioCallback = 0x14EB6F;
void XmvAudioCallback(Hp2Ctx& c) {
  static const bool trace = getenv("HP2_XMV_TRACE") != nullptr;
  uint32_t ctx = hp2_arg(c, 0), ts = hp2_arg(c, 1), status = hp2_arg(c, 2);
  hp2_original(kXmvAudioCallback)(c);
  static uint32_t n = 0;
  if (trace && (++n % 25 == 0 || ts < 200))
    hp2_log("movies: audio packet done: ts %u ms, status %u -> video offset %d ms", ts, status,
            int32_t(hp2_rd32(ctx + 0xC8)));
}
HP2_OVERRIDE(kXmvAudioCallback, XmvAudioCallback);

}  // namespace
