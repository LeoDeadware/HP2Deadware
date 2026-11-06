// The game window (SDL3 + CAMetalLayer) and the host event loop.
//
// macOS only lets the process main thread run windowing and events, while the
// game runs on threads of its own; so the runtime hands the main thread to
// hp2_host_main_loop() once the XBE entry has started the title. It creates
// the window, gives its CAMetalLayer to the GPU layer (the game thread draws
// every presented frame into it, fitted 4:3: gpu::PresentFrame) and pumps
// events: controller/keyboard (XInput HLE's SDL backend), resize, fullscreen
// (Cmd+F, Alt+Enter: desktop fullscreen), quit (Cmd+Q, window close).
// The window starts in borderless (desktop) fullscreen on the display under
// the mouse unless settings.toml [video] fullscreen = false; Cmd+F / Alt+Enter
// switch and save the choice there. HP2_FULLSCREEN=0|1 overrides it for a run.
//
// HP2_BACKGROUND=1 (tests): no window, no SDL video, the loop returns at once.
// HP2_DEV_PRESENT_TEST=WxH (with HP2_BACKGROUND=1): the frames go to an
// unattached W x H layer instead, and each screenshot frame also writes
// present_NNNNNN.png: the present path checked without taking the screen.

#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <QuartzCore/CAMetalLayer.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "hle/d3d8/gpu.h"
#include "hle/d3d8/video_settings.h"
#include "hle/xinput/xinput.h"
#include "runtime/hp2_runtime.h"

extern "C" float hp2_d3d8_output_aspect();  // d3d8.cpp

namespace {

bool EnvOn(const char* n) {
  const char* v = getenv(n);
  return v && *v && *v != '0';
}

void ResizeLayer(SDL_Window* w, CAMetalLayer* layer) {
  int pw = 0, ph = 0;
  SDL_GetWindowSizeInPixels(w, &pw, &ph);
  if (pw > 0 && ph > 0) layer.drawableSize = CGSizeMake(pw, ph);
}

void ToggleFullscreen(SDL_Window* w) {
  bool full = (SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN) != 0;
  SDL_SetWindowFullscreenMode(w, nullptr);  // desktop fullscreen
  SDL_SetWindowFullscreen(w, !full);
  std::string path = hp2d3d::VideoSettingsPath();
  std::string text = hp2d3d::WithVideoSetting(hp2d3d::ReadTextFile(path), "fullscreen",
                                              full ? "false" : "true");
  if (!hp2d3d::WriteTextFile(path, text)) hp2_log("window: cannot save %s", path.c_str());
}

// Fullscreen at start: HP2_FULLSCREEN, else settings.toml [video] fullscreen
// (default true).
bool StartFullscreen() {
  if (const char* v = getenv("HP2_FULLSCREEN"); v && *v) return *v != '0';
  return hp2d3d::VideoSettingBool(hp2d3d::ReadTextFile(hp2d3d::VideoSettingsPath()), "fullscreen",
                                  true);
}

// The display under the mouse (the one the player is looking at).
CGDirectDisplayID ActiveDisplay() {
  CGDirectDisplayID d = CGMainDisplayID();
  if (CGEventRef e = CGEventCreate(nullptr)) {
    CGPoint p = CGEventGetLocation(e);
    CFRelease(e);
    uint32_t n = 0;
    CGDirectDisplayID found;
    if (CGGetDisplaysWithPoint(p, 1, &found, &n) == kCGErrorSuccess && n == 1) d = found;
  }
  return d;
}

}  // namespace

// Render scale for windowed play: the pixel height the frame is shown at /
// 480, rounded down (at most 4 by the caller): fullscreen = the active
// display's height; a window = 900 points, <= 90% of the display. Times the
// display's backing scale. CoreGraphics only: callable from the game thread.
extern "C" int hp2_window_auto_scale() {
  CGDisplayModeRef m = CGDisplayCopyDisplayMode(ActiveDisplay());
  if (!m) return 2;
  double points_h = double(CGDisplayModeGetHeight(m));
  double px_per_pt = CGDisplayModeGetHeight(m) ? double(CGDisplayModeGetPixelHeight(m)) / points_h : 1;
  CGDisplayModeRelease(m);
  double shown_pt = StartFullscreen() ? points_h : std::min(900.0, 0.9 * points_h);
  int scale = int(shown_pt * px_per_pt / 480.0);
  return scale < 1 ? 1 : scale;
}

extern "C" void hp2_host_main_loop() {
  if (const char* t = getenv("HP2_DEV_PRESENT_TEST")) {
    unsigned w = 0, h = 0;
    if (std::sscanf(t, "%ux%u", &w, &h) == 2 && w && h) {
      CAMetalLayer* layer = [CAMetalLayer layer];
      layer.drawableSize = CGSizeMake(w, h);
      hp2d3d::gpu::KeepPresentCopies(true);
      hp2d3d::gpu::SetPresentLayer((__bridge void*)layer);
      hp2_log("window: HP2_DEV_PRESENT_TEST: frames presented to an unattached %ux%u layer", w, h);
    }
    return;
  }
  if (EnvOn("HP2_BACKGROUND")) return;

  SDL_SetHint(SDL_HINT_APP_NAME, "Harry Potter and the Chamber of Secrets");
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    hp2_log("window: SDL_Init(VIDEO) failed: %s (running without a window)", SDL_GetError());
    return;
  }
  // Window shape = the output's aspect (4:3, 16:10 in widescreen): 900 points
  // tall, at most 90% of the display's usable area. Other shapes (resized,
  // fullscreen) get black bars from the present.
  float aspect = hp2_d3d8_output_aspect();
  int wh = 900, ww = int(900 * aspect + 0.5f);
  float mx = 0, my = 0;
  SDL_GetGlobalMouseState(&mx, &my);
  SDL_Point mouse{int(mx), int(my)};
  SDL_DisplayID display = SDL_GetDisplayForPoint(&mouse);
  if (!display) display = SDL_GetPrimaryDisplay();
  SDL_Rect usable;
  if (SDL_GetDisplayUsableBounds(display, &usable) && usable.w > 0 && usable.h > 0) {
    float k = std::min({1.0f, 0.9f * usable.w / ww, 0.9f * usable.h / wh});
    ww = int(ww * k + 0.5f), wh = int(wh * k + 0.5f);
  }
  bool full = StartFullscreen();
  SDL_PropertiesID props = SDL_CreateProperties();
  SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING,
                        "Harry Potter and the Chamber of Secrets");
  SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(display));
  SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(display));
  SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, ww);
  SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, wh);
  SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER,
                        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_METAL |
                            (full ? SDL_WINDOW_FULLSCREEN : 0));
  SDL_Window* win = SDL_CreateWindowWithProperties(props);  // fullscreen: desktop mode (no mode set)
  SDL_DestroyProperties(props);
  if (!win) {
    hp2_log("window: SDL_CreateWindow failed: %s (running without a window)", SDL_GetError());
    return;
  }
  SDL_SetWindowMinimumSize(win, 320, 240);
  SDL_MetalView view = SDL_Metal_CreateView(win);
  CAMetalLayer* layer = view ? (__bridge CAMetalLayer*)SDL_Metal_GetLayer(view) : nil;
  if (!layer) {
    hp2_log("window: no Metal layer: %s (running without a window)", SDL_GetError());
    return;
  }
  ResizeLayer(win, layer);
  hp2d3d::gpu::SetPresentLayer((__bridge void*)layer);
  if (!hp2_xinput_sdl_init()) hp2_log("window: gamepad subsystem failed: %s", SDL_GetError());
  int pw = 0, ph = 0;
  SDL_GetWindowSizeInPixels(win, &pw, &ph);
  hp2_log("window: %dx%d points (aspect %.3f), %dx%d pixels%s (Cmd+F / Alt+Enter fullscreen, "
          "Cmd+Q quit)", ww, wh, aspect, pw, ph, full ? ", fullscreen" : "");

  for (;;) {
    SDL_Event e;
    if (SDL_WaitEventTimeout(&e, 4)) {
      do {
        switch (e.type) {
          case SDL_EVENT_QUIT:
          case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            hp2_exit(0, "window closed");
          case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            ResizeLayer(win, layer);
            break;
          case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
            SDL_HideCursor();
            ResizeLayer(win, layer);
            break;
          case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
            SDL_ShowCursor();
            ResizeLayer(win, layer);
            break;
          case SDL_EVENT_KEY_DOWN:
            if (!e.key.repeat &&
                ((e.key.key == SDLK_F && (e.key.mod & SDL_KMOD_GUI)) ||
                 (e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT))))
              ToggleFullscreen(win);
            break;
          default:
            break;
        }
      } while (SDL_PollEvent(&e));
    }
    // Keyboard state and gamepads -> XInput port 0..3 (once per ~4 ms).
    hp2_xinput_sdl_update();
  }
}
