// «TV 2002» look (Polish): one post-processing chain over the finished frame,
// drawn by the D3D8 HLE's window present instead of its plain scaled copy
// (hp2_d3d8_set_postfx; without Polish the present is unchanged).
//
//   HP2_LOOK=authentic_hd (default) | crt | authentic | modern
//     authentic_hd  grade 1.0, bloom 0.4, grain 0.15, vignette 0.2
//     crt           authentic_hd + scanlines / aperture grille 1.0
//     authentic     the chain with every effect 0 (= the plain present)
//     modern        off (the plain present)
//   HP2_LOOK_GRADE / _BLOOM / _GRAIN / _VIGNETTE / _CRT = 0..1 override one
//   strength on top of every preset except modern.
//   F2 (window) cycles authentic_hd -> crt -> modern -> authentic, no restart.
//
// Cost: GPU timestamps around the chain (stage-boundary counters), logged
// every 600 frames as "polish: look ... GPU x ms". Textures are allocated
// once per output size.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "polish/postfx/postfx.h"
#include "polish/postfx/postfx_msl.h"
#include "runtime/hp2_runtime.h"

using PostFxFn = bool (*)(void* cmd, void* src, const float uv[4], void* dst, const float vp[4]);
// D3D8 HLE (Render, gpu_metal.mm). Weak: a build without it has no present.
extern "C" void hp2_d3d8_set_postfx(PostFxFn f) __attribute__((weak));

namespace {

struct Look {
  const char* name;
  bool on;  // false: the plain present runs
  float grade, bloom, grain, vignette, crt;
};

constexpr Look kLooks[] = {
    {"authentic_hd", true, 1.0f, 0.4f, 0.15f, 0.2f, 0.0f},
    {"crt", true, 1.0f, 0.4f, 0.15f, 0.2f, 1.0f},
    {"modern", false, 0, 0, 0, 0, 0},
    {"authentic", true, 0, 0, 0, 0, 0},
};
constexpr int kNumLooks = int(sizeof kLooks / sizeof kLooks[0]);

std::atomic<int> g_look{0};
std::atomic<bool> g_look_changed{true};

struct Params {  // = Params in postfx_msl.h
  float src_uv[4];
  float src_size[2];
  float vp_origin[2];
  float vp_size[2];
  float bloom_size[2];
  float lines;
  float grade, bloom, grain, vignette, crt;
  float grain_off[2];
  float bloom_norm, dither, frame, pad;
};
static_assert(sizeof(Params) == 96, "Params layout = MSL");

struct Chain {  // = Chain in postfx_msl.h
  float in_texel[2], pad[2];
  float mul_big[4], mul_small[4];
};
static_assert(sizeof(Chain) == 48, "Chain layout = MSL");

// Bloom mip chain: level 0 = 1/4 of the fit rect, each next one half of it.
// Level weights grow with the radius (a soft, deep glow rather than a rim);
// wider levels are warmer: the red halation of light scattered in the glass.
constexpr int kLevels = 5;
constexpr float kLevelWeight[kLevels] = {1.0f, 1.0f, 1.1f, 1.25f, 1.4f};
constexpr float kHalation[3] = {1.0f, 0.5f, 0.3f};  // tint of the widest level

// Effective strengths: the preset, then the HP2_LOOK_* overrides.
Look Effective(int i) {
  Look l = kLooks[i];
  if (!l.on) return l;
  auto ov = [](const char* n, float* v) {
    const char* e = getenv(n);
    if (e && *e) *v = std::clamp(float(atof(e)), 0.0f, 1.0f);
  };
  ov("HP2_LOOK_GRADE", &l.grade);
  ov("HP2_LOOK_BLOOM", &l.bloom);
  ov("HP2_LOOK_GRAIN", &l.grain);
  ov("HP2_LOOK_VIGNETTE", &l.vignette);
  ov("HP2_LOOK_CRT", &l.crt);
  return l;
}

// SDL event watch (main thread): F2 cycles the presets.
bool OnEvent(void*, SDL_Event* e) {
  if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat && e->key.key == SDLK_F2 &&
      !(e->key.mod & (SDL_KMOD_GUI | SDL_KMOD_CTRL | SDL_KMOD_ALT))) {
    g_look = (g_look.load() + 1) % kNumLooks;
    g_look_changed = true;
  }
  return true;
}

constexpr int kSlots = 4;  // timestamp slots (> frames in flight)
constexpr int kPasses = 2 * kLevels;  // passes at most: prefilter, down x4, up x4, composite

struct Fx {
  id<MTLDevice> dev;
  id<MTLRenderPipelineState> prefilter, down, up, composite;
  id<MTLSamplerState> samp, wrap;
  id<MTLTexture> noise;  // 256x256 white noise (grain)
  id<MTLTexture> lv[kLevels], ul[kLevels - 1];  // chain levels: down (lv), up sums (ul)
  uint32_t bloom_w = 0, bloom_h = 0;
  uint64_t frame = 0;
  bool watch = false;
  // GPU timing
  id<MTLCounterSampleBuffer> ts;
  int slot = 0;
  std::mutex mu;
  double sum_ms = 0, max_ms = 0;
  int n = 0;
  uint64_t cal_cpu = 0, cal_gpu = 0;
  std::chrono::steady_clock::time_point cal_t;
  double ns_per_tick = 0;
  uint32_t out_w = 0, out_h = 0;
};
Fx* f = nullptr;

id<MTLRenderPipelineState> Pipe(id<MTLLibrary> lib, NSString* fs, MTLPixelFormat fmt) {
  MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
  pd.vertexFunction = [lib newFunctionWithName:@"pf_vs"];
  pd.fragmentFunction = [lib newFunctionWithName:fs];
  pd.colorAttachments[0].pixelFormat = fmt;
  NSError* err = nil;
  id<MTLRenderPipelineState> p = [f->dev newRenderPipelineStateWithDescriptor:pd error:&err];
  if (!p) hp2_log("polish: postfx pipeline %s: %s", fs.UTF8String, err.localizedDescription.UTF8String);
  return p;
}

bool Setup(id<MTLDevice> dev) {
  f = new Fx;
  f->dev = dev;
  NSError* err = nil;
  id<MTLLibrary> lib = [dev newLibraryWithSource:[NSString stringWithUTF8String:hp2polish::kPostFxMsl]
                                         options:nil
                                           error:&err];
  if (!lib) {
    hp2_log("polish: postfx shaders: %s (look off)", err.localizedDescription.UTF8String);
    return false;
  }
  f->prefilter = Pipe(lib, @"pf_prefilter", MTLPixelFormatRG11B10Float);
  f->down = Pipe(lib, @"pf_down", MTLPixelFormatRG11B10Float);
  f->up = Pipe(lib, @"pf_up", MTLPixelFormatRG11B10Float);
  f->composite = Pipe(lib, @"pf_composite", MTLPixelFormatBGRA8Unorm);
  if (!f->prefilter || !f->down || !f->up || !f->composite) return false;
  MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
  sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
  sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
  f->samp = [dev newSamplerStateWithDescriptor:sd];
  sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeRepeat;
  f->wrap = [dev newSamplerStateWithDescriptor:sd];
  MTLTextureDescriptor* nd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                                                                width:256
                                                                               height:256
                                                                            mipmapped:NO];
  f->noise = [dev newTextureWithDescriptor:nd];
  std::vector<uint8_t> px(256 * 256);
  uint32_t x = 0x9E3779B9u;
  for (auto& v : px) {  // xorshift32
    x ^= x << 13, x ^= x >> 17, x ^= x << 5;
    v = uint8_t(x >> 24);
  }
  [f->noise replaceRegion:MTLRegionMake2D(0, 0, 256, 256) mipmapLevel:0 withBytes:px.data() bytesPerRow:256];
  // Timestamps at render-stage boundaries (Apple GPUs).
  if ([dev supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]) {
    for (id<MTLCounterSet> cs in dev.counterSets) {
      if (![cs.name isEqualToString:MTLCommonCounterSetTimestamp]) continue;
      MTLCounterSampleBufferDescriptor* d = [MTLCounterSampleBufferDescriptor new];
      d.counterSet = cs;
      d.storageMode = MTLStorageModeShared;
      d.sampleCount = 2 * kPasses * kSlots;
      f->ts = [dev newCounterSampleBufferWithDescriptor:d error:&err];
    }
  }
  if (f->ts) {
    [dev sampleTimestamps:&f->cal_cpu gpuTimestamp:&f->cal_gpu];
    f->cal_t = std::chrono::steady_clock::now();
  } else {
    hp2_log("polish: no GPU timestamp counters: the look's cost is not measured");
  }
  return true;
}

// Ticks -> ns from two (steady clock, GPU timestamp) pairs, refreshed per report.
void Calibrate() {
  MTLTimestamp cpu = 0, gpu = 0;
  [f->dev sampleTimestamps:&cpu gpuTimestamp:&gpu];
  auto now = std::chrono::steady_clock::now();
  double dns = std::chrono::duration<double, std::nano>(now - f->cal_t).count();
  if (gpu > f->cal_gpu && dns > 1e8) f->ns_per_tick = dns / double(gpu - f->cal_gpu);
}

// (Re)allocates the chain for a level-0 size of w x h (on the first use and on resize).
void EnsureBloom(uint32_t w, uint32_t h) {
  if (f->lv[0] && f->bloom_w == w && f->bloom_h == h) return;
  for (int k = 0; k < kLevels; k++) {
    MTLTextureDescriptor* td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG11B10Float
                                                           width:std::max(1u, w >> k)
                                                          height:std::max(1u, h >> k)
                                                       mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModePrivate;
    f->lv[k] = [f->dev newTextureWithDescriptor:td];
    if (k < kLevels - 1) f->ul[k] = [f->dev newTextureWithDescriptor:td];
  }
  f->bloom_w = w, f->bloom_h = h;
}

void LogLook(const Look& l) {
  if (!l.on)
    hp2_log("polish: look %s (off: the plain present)", kLooks[g_look.load()].name);
  else
    hp2_log("polish: look %s (grade %.2f bloom %.2f grain %.2f vignette %.2f crt %.2f)",
            kLooks[g_look.load()].name, l.grade, l.bloom, l.grain, l.vignette, l.crt);
}

// A pass into t; with timestamps, samples the start and end of its fragment
// stage into ts slots ts_base, ts_base + 1 (the vertex stage of a
// full-screen triangle is negligible, and the fragment stage starts only
// once the pass's inputs are written: no waiting on the game's frame counted).
id<MTLRenderCommandEncoder> Pass(id<MTLCommandBuffer> cb, id<MTLTexture> t, bool clear, int ts_base) {
  MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
  rp.colorAttachments[0].texture = t;
  rp.colorAttachments[0].loadAction = clear ? MTLLoadActionClear : MTLLoadActionDontCare;
  rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
  rp.colorAttachments[0].storeAction = MTLStoreActionStore;
  if (f->ts) {
    MTLRenderPassSampleBufferAttachmentDescriptor* a = rp.sampleBufferAttachments[0];
    a.sampleBuffer = f->ts;
    a.startOfVertexSampleIndex = MTLCounterDontSample;
    a.endOfVertexSampleIndex = MTLCounterDontSample;
    a.startOfFragmentSampleIndex = ts_base >= 0 ? NSUInteger(ts_base) : MTLCounterDontSample;
    a.endOfFragmentSampleIndex = ts_base >= 0 ? NSUInteger(ts_base + 1) : MTLCounterDontSample;
  }
  return [cb renderCommandEncoderWithDescriptor:rp];
}

// Encodes the chain for `look` (src uv rect -> fit rect vp of dst) into cb;
// with ts0 >= 0, each pass samples its fragment stage into ts0 + 2k, +1.
// Returns the number of passes.
int Encode(id<MTLCommandBuffer> cb, const Look& look, id<MTLTexture> src, const float uv[4],
           id<MTLTexture> dst, const float vp[4], int ts0) {
  Params p{};
  std::memcpy(p.src_uv, uv, sizeof p.src_uv);
  p.src_size[0] = float(src.width), p.src_size[1] = float(src.height);
  p.vp_origin[0] = vp[0], p.vp_origin[1] = vp[1];
  p.vp_size[0] = vp[2], p.vp_size[1] = vp[3];
  p.lines = 480;  // the Xbox's output lines, whatever the render scale
  p.grade = look.grade, p.bloom = look.bloom, p.grain = look.grain;
  p.vignette = look.vignette, p.crt = look.crt;
  uint64_t fr = f->frame++;
  p.grain_off[0] = float((fr * 97) % 256), p.grain_off[1] = float((fr * 61 + 13) % 256);
  uint32_t bw = std::max(1u, uint32_t(vp[2] / 4 + 0.5f)), bh = std::max(1u, uint32_t(vp[3] / 4 + 0.5f));
  p.bloom_size[0] = float(bw), p.bloom_size[1] = float(bh);
  float wsum = 0;
  for (float w : kLevelWeight) wsum += w;
  p.bloom_norm = 1 / wsum;
  // Dither whenever the picture is changed at all (authentic stays exact).
  p.dither = (look.grade > 0 || look.bloom > 0 || look.grain > 0 || look.vignette > 0 || look.crt > 0);
  p.frame = float(fr % 4096);

  int npass = 0;
  auto ts = [&] { return ts0 < 0 ? -1 : ts0 + 2 * npass++; };
  auto draw = [&](id<MTLTexture> out, id<MTLRenderPipelineState> ps, const void* u, size_t n,
                  id<MTLTexture> t0, id<MTLTexture> t1) {
    id<MTLRenderCommandEncoder> e = Pass(cb, out, false, ts());
    [e setRenderPipelineState:ps];
    [e setFragmentBytes:u length:n atIndex:0];
    [e setFragmentTexture:t0 atIndex:0];
    if (t1) [e setFragmentTexture:t1 atIndex:1];
    [e setFragmentSamplerState:f->samp atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
  };
  if (look.bloom > 0) {
    EnsureBloom(bw, bh);
    draw(f->lv[0], f->prefilter, &p, sizeof p, src, nil);
    for (int k = 1; k < kLevels; k++) {
      Chain c{};
      c.in_texel[0] = 1.0f / f->lv[k - 1].width, c.in_texel[1] = 1.0f / f->lv[k - 1].height;
      draw(f->lv[k], f->down, &c, sizeof c, f->lv[k - 1], nil);
    }
    auto tint = [](int k, float w, float* out) {
      float h = float(k) / (kLevels - 1);
      for (int i = 0; i < 3; i++) out[i] = w * (1 + h * (kHalation[i] - 1));
    };
    for (int k = kLevels - 2; k >= 0; k--) {
      Chain c{};
      id<MTLTexture> small = k == kLevels - 2 ? f->lv[k + 1] : f->ul[k + 1];
      c.in_texel[0] = 1.0f / small.width, c.in_texel[1] = 1.0f / small.height;
      tint(k, kLevelWeight[k], c.mul_big);
      if (k == kLevels - 2)
        tint(k + 1, kLevelWeight[k + 1], c.mul_small);
      else
        c.mul_small[0] = c.mul_small[1] = c.mul_small[2] = 1;
      draw(f->ul[k], f->up, &c, sizeof c, f->lv[k], small);
    }
  }
  id<MTLRenderCommandEncoder> e = Pass(cb, dst, true, ts());
  [e setViewport:MTLViewport{vp[0], vp[1], vp[2], vp[3], 0, 1}];
  [e setRenderPipelineState:f->composite];
  [e setFragmentBytes:&p length:sizeof p atIndex:0];
  [e setFragmentTexture:src atIndex:0];
  [e setFragmentTexture:(look.bloom > 0 ? f->ul[0] : src) atIndex:1];
  [e setFragmentTexture:f->noise atIndex:2];
  [e setFragmentSamplerState:f->samp atIndex:0];
  [e setFragmentSamplerState:f->wrap atIndex:1];
  [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  [e endEncoding];
  return npass;
}

// HP2_LOOK_BENCH=1: once, on the first frame, every look's chain 200 times
// into an offscreen 2880x1800 target (fit 2400x1800) in one command buffer
// each: the GPU at a sustained clock, unlike a light game frame. Logs
// "polish: bench <look> at 2880x1800: x ms".
void Bench(id<MTLTexture> src, const float uv[4]) {
  MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                width:2880
                                                                               height:1800
                                                                            mipmapped:NO];
  td.usage = MTLTextureUsageRenderTarget;
  td.storageMode = MTLStorageModePrivate;
  id<MTLTexture> dst = [f->dev newTextureWithDescriptor:td];
  const float vp[4] = {240, 0, 2400, 1800};
  id<MTLCommandQueue> q = [f->dev newCommandQueue];
  for (int warm = 0; warm < 2; warm++)
    for (int i = 0; i <= kNumLooks; i++) {  // the presets, then the current look with its overrides
      Look l = i < kNumLooks ? kLooks[i] : Effective(g_look.load());
      if (!l.on) continue;
      if (i == kNumLooks) l.name = "current";
      constexpr int kN = 200;
      id<MTLCommandBuffer> cb = [q commandBuffer];
      for (int n = 0; n < kN; n++) Encode(cb, l, src, uv, dst, vp, -1);
      [cb commit];
      [cb waitUntilCompleted];
      if (warm)
        hp2_log("polish: bench %s at 2880x1800: %.3f ms per frame (%d frames, sustained clock)", l.name,
                (cb.GPUEndTime - cb.GPUStartTime) * 1e3 / kN, kN);
    }
}

bool PostFx(void* cmd, void* src_p, const float uv[4], void* dst_p, const float vp[4]) {
  int li = g_look.load();
  static Look look;
  if (g_look_changed.exchange(false)) {
    look = Effective(li);
    LogLook(look);
  }
  if (!f->watch && SDL_WasInit(SDL_INIT_VIDEO)) {
    f->watch = SDL_AddEventWatch(OnEvent, nullptr);
    if (f->watch) hp2_log("polish: F2 cycles the look (authentic_hd, crt, modern, authentic)");
  }
  if (!look.on) return false;

  id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)cmd;
  id<MTLTexture> src = (__bridge id<MTLTexture>)src_p;
  id<MTLTexture> dst = (__bridge id<MTLTexture>)dst_p;
  if (dst.pixelFormat != MTLPixelFormatBGRA8Unorm) return false;
  static bool bench = [] {
    const char* b = getenv("HP2_LOOK_BENCH");
    return b && *b && *b != '0';
  }();
  if (bench) {
    bench = false;
    Bench(src, uv);
  }
  if (dst.width != f->out_w || dst.height != f->out_h) {
    f->out_w = uint32_t(dst.width), f->out_h = uint32_t(dst.height);
    hp2_log("polish: look output %ux%u (fit %.0fx%.0f)", f->out_w, f->out_h, vp[2], vp[3]);
  }

  const int ts0 = f->ts ? 2 * kPasses * f->slot : -1;
  f->slot = (f->slot + 1) % kSlots;
  const int npass = Encode(cb, look, src, uv, dst, vp, ts0);

  if (f->ts) {
    id<MTLCounterSampleBuffer> tsb = f->ts;
    const char* name = kLooks[li].name;
    uint32_t ow = f->out_w, oh = f->out_h;
    [cb addCompletedHandler:^(id<MTLCommandBuffer>) {
      NSData* d = [tsb resolveCounterRange:NSMakeRange(NSUInteger(ts0), NSUInteger(2 * npass))];
      if (!d || d.length < 2 * npass * sizeof(MTLCounterResultTimestamp)) return;
      const MTLCounterResultTimestamp* r = (const MTLCounterResultTimestamp*)d.bytes;
      uint64_t ticks = 0;  // sum of the passes' fragment stages
      for (int k = 0; k < npass; k++) {
        uint64_t a = r[2 * k].timestamp, b = r[2 * k + 1].timestamp;
        if (a == MTLCounterErrorValue || b == MTLCounterErrorValue || b < a) return;
        ticks += b - a;
      }
      std::lock_guard<std::mutex> lk(f->mu);
      if (!f->ns_per_tick) Calibrate();
      if (!f->ns_per_tick) return;
      double ms = double(ticks) * f->ns_per_tick * 1e-6;
      f->sum_ms += ms, f->max_ms = std::max(f->max_ms, ms), f->n++;
      if (f->n == 600) {
        hp2_log("polish: look %s at %ux%u: GPU %.3f ms avg, %.3f ms max over %d frames (%d passes)", name,
                ow, oh, f->sum_ms / f->n, f->max_ms, f->n, npass);
        f->sum_ms = f->max_ms = 0, f->n = 0;
        Calibrate();
      }
    }];
  }
  return true;
}

}  // namespace

void hp2_polish_postfx_init() {
  if (!hp2_d3d8_set_postfx) return;
  const char* v = getenv("HP2_LOOK");
  int li = 0;
  if (v && *v) {
    li = -1;
    for (int i = 0; i < kNumLooks; i++)
      if (!std::strcmp(v, kLooks[i].name)) li = i;
    if (li < 0) {
      hp2_log("polish: HP2_LOOK=%s unknown (authentic_hd, crt, authentic, modern): authentic_hd", v);
      li = 0;
    }
  }
  g_look = li;
  id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
  if (!dev || !Setup(dev)) return;
  hp2_d3d8_set_postfx(PostFx);
}
