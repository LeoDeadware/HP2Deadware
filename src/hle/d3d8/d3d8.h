#pragma once
// D3D8 HLE (owner: Render). Replaces the D3D section functions the game
// calls (FUNCTIONS.md) with host functions drawing through Metal. Linked
// whole-archive: the overrides register from static initialisers.
//
// Environment:
//   HP2_RENDER_SCALE=N      render targets at N x their Xbox size (default: 1 for silent
//                           HP2_BACKGROUND runs; windowed: window pixel height / 480, <= 4)
//   HP2_ANISO=N             anisotropic filtering of mipmapped textures (windowed default 8)
//   HP2_FILTER=bicubic      window present upscale filter (default bilinear); a frame larger
//                           than the window is always area-averaged (HP2_FILTER=bilinear: one
//                           bilinear tap, for A/B checks)
//   HP2_FULLSCREEN=0|1      window start mode for this run (default: settings.toml [video]
//                           fullscreen, true if absent; Cmd+F / Alt+Enter switch and save it)
//   HP2_WBUFFER=0           D3DZB_USEW draws use z/w depth instead of the emulated W-buffer
//   HP2_SHOT_DIR=dir        where screenshots go (default $HP2_LOG_DIR)
//   HP2_SHOT_EVERY=N        PNG of every Nth presented frame
//   HP2_SHOT_FRAMES=a,b,..  PNG of these frame numbers
//   HP2_EXIT_AFTER=N        clean exit after N presented frames (tests)
//   HP2_NO_PACING=1         Swap does not wait for the 60 Hz vblank
//   HP2_WIDESCREEN=1        16:10 (Hor+ 3D, centred 2D) unless hp2_d3d8_set_widescreen was called
//   HP2_WIDE_HUD=0          widescreen: keep the HUD in the centred 4:3 area (default: corner
//                           HUD sprites + their action labels anchored to the wide edges)
//   HP2_TEX_DUMP=dir        dump every new texture (level 0) as <hash>.png
//   HP2_TEX_RECHECK=N       re-hash unlocked textures every N frames (default 30; 1 = always)
//   HP2_D3D_TRACE=1         log D3D calls (targets, clears, textures, declarations)
//   HP2_SHADER_CHECK=1      translate + compile every vertex/pixel shader at creation
//   HP2_DUMP_MSL=dir        write every generated MSL program
// Debugging (one frame or draw range; see d3d8.cpp DrawCommon):
//   HP2_DEV_LOG_DRAWS=f     log every draw of frame f (state, textures)
//   HP2_DEV_HUD_LOG=f       log frame f's 2D draws: box, text/panel, widescreen anchor shift
//   HP2_DEV_PROBE=f,x,y     log the draws of frame f that change pixel (x, y)
//   HP2_DEV_SKIP_DRAWS=a-b  drop draws a..b; HP2_DEV_SKIP_PS=<hash> drop a fragment program
//   HP2_DEV_MAGENTA=a-b     draw a..b flat (HP2_DEV_SHOW=fog|d0|d0a|t0uv|t0 shows that input)
//   HP2_DEV_DUMP_TARGETS=f  PNG of every colour render target at frame f
//   HP2_DEV_NOFOG=1         fog off

#include <cstdint>

// Texture replacement hook (Polish). Called once per new texture content:
// `hash` is the content hash of the Xbox data (all levels), w x h its size;
// `data` (data_size bytes, valid during the call) is that Xbox data, level 0
// first (DXTn: plain blocks; other formats swizzled).
// Return true and fill *rgba (w2 x h2 RGBA8) to replace the texture.
struct Hp2TextureInfo {
  uint64_t hash;
  uint32_t width, height, xbox_format, levels;
  const uint8_t* data;
  uint32_t data_size;
};
using Hp2TextureHook = bool (*)(const Hp2TextureInfo& info, uint32_t* w2, uint32_t* h2,
                                const uint8_t** rgba);
void hp2_d3d8_set_texture_hook(Hp2TextureHook hook);

// Widescreen (Polish): screen-sized targets aspect_scale x wider, 3D Hor+,
// 2D centred, full-screen quads stretched, movies pillarboxed. Before CreateDevice.
extern "C" void hp2_d3d8_set_widescreen(float aspect_scale);

// Frames presented so far (Swap calls; movie overlay frames do not count).
// Stable contract: the XInput HLE's pad script (HP2_PAD_SCRIPT) uses it as
// its frame clock.
uint64_t hp2_d3d8_frame_count();
