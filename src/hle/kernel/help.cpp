// `hp2 --help`: usage, controls and every HP2_* option of all components
// (kernel_exports.h: hp2_help_text). Keep in sync with docs/CONTROLS.md and
// the components' headers (d3d8.h, dsound mixer.cpp, runtime).

#include "hle/kernel/kernel_exports.h"

const char* hp2_help_text() {
  return R"(HP2Port - Harry Potter and the Chamber of Secrets (Xbox, NTSC-US), native on macOS.

Usage: hp2 [path/to/default.xbe]      (default: $HP2_DATA/game/extracted/default.xbe)
       hp2 --help

Controls (keyboard = controller 1; a gamepad also works, see docs/CONTROLS.md)
  Move (left stick)   W A S D        Right stick     I J K L
  A (menu: select)    Space          B (menu: back)      E
  X                   Q              Y                   R
  Black               1              White               2
  Left trigger        U              Right trigger       O
  Start (pause)       Enter          Back                Esc or Backspace
  D-pad               arrow keys     Stick clicks        F (left), H (right)
  In game the on-screen prompts show what each button does.
  Fullscreen          Cmd+F or Alt+Enter                 Quit  Cmd+Q

Profile and saves
  ~/Library/Application Support/HP2Port/<profile>/   created on first run
    hp2.ini                 settings: language=en|uk|uk_text
    hdd/partition1/UDATA/   save games (back this up)
  Crash reports: ~/Library/Logs/hp2/hp2-crash-<time>.txt
  ~/Library/Application Support/HP2/settings.toml   gamepad: modern orbit
                          camera (yaw speed, recenter delay), camera stick
                          invert/sensitivity/curve/smoothing, dead zones,
                          Nintendo layout, rumble strength (commented)

Settings (environment; HP2_* variables override hp2.ini)
  HP2_DATA=dir          folder with game/extracted/default.xbe (default: found
                        next to the app)
  HP2_PROFILE=name      profile folder (default "default")
  HP2_LANG=key          en (English, US), uk (Ukrainian: _UK slot + mods/uk) or
                        uk_text (Ukrainian text, English speech: mods/uk/_UKT);
                        in game: Options -> Language Selection (flags)
  HP2_MODS=dir:dir      override folders searched before the disc (default
                        $HP2_DATA/mods/uk when it exists)
  HP2_RENDER_SCALE=N    render at N x the Xbox resolution (default 1)
  HP2_WIDESCREEN=1      16:10 view
  HP2_AUDIO=0           no sound device
  HP2_SKIP_MOVIES=1     skip the intro movies
  HP2_VIDEO_FLAGS=n, HP2_AV_PACK=n   Xbox video settings (default: 480i
                        standard AV pack)
  HP2_ZCACHE=1          let the game use its Z:\ file cache

Tests and development (silent runs: HP2_BACKGROUND=1 SDL_AUDIO_DRIVER=dummy
SDL_VIDEO_DRIVER=dummy; see tests/scripts/)
  HP2_BACKGROUND=1      no window, never take focus, no real keyboard/gamepad
  HP2_LOG_DIR=dir       runtime.log, screenshots and dumps there
  HP2_PAD_SCRIPT=file   scripted controller (src/hle/xinput/pad_script.h)
  HP2_SHOT_FRAMES=a,b   PNG of these frames; HP2_SHOT_EVERY=N every Nth;
                        HP2_SHOT_DIR=dir
  HP2_EXIT_AFTER=N      exit after N frames
  HP2_NO_PACING=1       do not wait for the 60 Hz vblank
  HP2_TEST_CLOCK=frames experimental: kernel time and vblanks follow frames
  HP2_AUDIO_WAV=file    also write the sound mix to a WAV
  HP2_INPUT_<SECTION>_<KEY>=v   gamepad setting for this run (settings.toml keys)
  HP2_CALL_AT=..., HP2_DUMP_AT=...   call guest code / dump memory at a frame
  HP2_CAMERA_LOG=1      one log line per gameplay camera frame (modern camera)
  HP2_LOG_CALLS=<va>[/<nargs>],...   log distinct calls (args, caller, eax) of guest functions
  HP2_KTRACE=1 (default 0), HP2_KTRACE_KERNEL=1, HP2_XINPUT_TRACE=1, HP2_D3D_TRACE=1,
  HP2_XMV_TRACE=1, HP2_DSLOG=1       traces
  HP2_STUB, HP2_STOP_AT, HP2_CALL, HP2_XBE   runtime bring-up switches
  HP2_CRASH_DIR=dir     crash reports there instead of ~/Library/Logs/hp2
  Render and audio debugging: HP2_DEV_*, HP2_TEX_DUMP, HP2_DUMP_MSL,
  HP2_SHADER_CHECK, HP2_AUDIO_SOLO, HP2_DS_* (src/hle/d3d8/d3d8.h,
  src/hle/dsound/)
)";
}
