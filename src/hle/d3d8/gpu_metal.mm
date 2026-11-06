// Metal implementation of gpu.h (Objective-C++, ARC).
//
// One command buffer per frame. Draws go into a render encoder for the
// current targets; changing targets, copying or reading back ends it. Vertex
// streams and index data are copied into a per-frame ring buffer (the guest
// may rewrite its buffers right after the draw call returns), uniforms go
// in with set*Bytes. Up to kFramesInFlight frames are queued.

#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <dispatch/dispatch.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "hle/d3d8/gpu.h"
#include "runtime/hp2_runtime.h"

namespace hp2d3d::gpu {
namespace {

constexpr int kFramesInFlight = 3;
constexpr size_t kRingSize = size_t(96) << 20;  // per frame

struct Tex {
  id<MTLTexture> t;
  TexKind kind;
  uint32_t w, h;
};

struct PipeKey {
  ProgId vs, fs;
  uint32_t blend;  // packed BlendState
  uint8_t has_color, has_depth;
  bool operator<(const PipeKey& o) const {
    return std::tie(vs, fs, blend, has_color, has_depth) <
           std::tie(o.vs, o.fs, o.blend, o.has_color, o.has_depth);
  }
};

struct State {
  id<MTLDevice> dev;
  id<MTLCommandQueue> queue;
  dispatch_semaphore_t inflight;
  id<MTLCommandBuffer> cmd;
  id<MTLRenderCommandEncoder> enc;
  id<MTLBuffer> ring[kFramesInFlight];
  int ring_index = 0;
  size_t ring_off = 0;
  std::vector<Tex> texs{Tex{}};  // id 0 unused
  std::vector<TexId> free_ids;
  // Programs may be compiled on a background queue (vertex shader
  // precompile): a deque keeps references stable, prog_mu guards it.
  std::deque<id<MTLFunction>> progs{nil};
  std::mutex prog_mu;
  std::map<PipeKey, id<MTLRenderPipelineState>> pipes;
  std::unordered_map<uint64_t, id<MTLDepthStencilState>> dss;
  std::map<std::tuple<uint64_t, uint32_t>, id<MTLSamplerState>> samplers;
  TexId cur_color = 0, cur_depth = 0;
  uint32_t cur_w = 0, cur_h = 0;
  // clear pipelines: [has_color][has_depth]
  id<MTLRenderPipelineState> clear_pipe[2][2][2];  // [color][depth][colour write]
  id<MTLFunction> clear_vs, clear_fs;
  id<MTLRenderPipelineState> copy_pipe;
  id<MTLRenderPipelineState> bicubic_pipe;  // window present filter (HP2_FILTER=bicubic)
  id<MTLRenderPipelineState> down_pipe;
  bool plain_present = false;     // window present, frame larger than the window
  bool bicubic = false;
  id<MTLFunction> copy_vs, copy_fs;
  id<MTLSamplerState> copy_samp;
  std::mutex mu;
  CAMetalLayer* layer = nil;  // the present layer as configured for this device
  TexId present_copy = 0;
  uint32_t present_w = 0, present_h = 0;
};
State* g = nullptr;

MTLPixelFormat PixFmt(TexKind k) {
  switch (k) {
    case TexKind::BGRA8: return MTLPixelFormatBGRA8Unorm;
    case TexKind::BC1: return MTLPixelFormatBC1_RGBA;
    case TexKind::BC2: return MTLPixelFormatBC2_RGBA;
    case TexKind::BC3: return MTLPixelFormatBC3_RGBA;
    case TexKind::Depth: return MTLPixelFormatDepth32Float_Stencil8;
  }
  return MTLPixelFormatBGRA8Unorm;
}

double g_gpu_wait_ms = 0;

void EnsureCmd() {
  if (g->cmd) return;
  auto t0 = std::chrono::steady_clock::now();
  dispatch_semaphore_wait(g->inflight, DISPATCH_TIME_FOREVER);
  g_gpu_wait_ms +=
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  g->cmd = [g->queue commandBuffer];
  dispatch_semaphore_t sem = g->inflight;
  [g->cmd addCompletedHandler:^(id<MTLCommandBuffer> cb) {
    if (cb.error) hp2_log("d3d8: GPU error: %s", cb.error.localizedDescription.UTF8String);
    dispatch_semaphore_signal(sem);
  }];
  g->ring_index = (g->ring_index + 1) % kFramesInFlight;
  g->ring_off = 0;
}

void EndEncoder() {
  if (g->enc) {
    [g->enc endEncoding];
    g->enc = nil;
  }
}

// Copies bytes into this frame's ring; returns the offset.
size_t RingPut(const void* p, size_t n) {
  size_t off = (g->ring_off + 255) & ~size_t(255);
  if (off + n > kRingSize) {
    // Frame too big for the ring: submit what we have and start a new buffer.
    hp2_log("d3d8: ring buffer full (%zu bytes), flushing mid-frame", off + n);
    TexId c = g->cur_color, d = g->cur_depth;
    uint32_t w = g->cur_w, h = g->cur_h;
    EndEncoder();
    [g->cmd commit];
    g->cmd = nil;
    EnsureCmd();
    g->cur_color = c, g->cur_depth = d, g->cur_w = w, g->cur_h = h;
    off = 0;
    if (n > kRingSize) hp2_fatal("d3d8: draw needs %zu bytes of vertex data", n);
  }
  std::memcpy(static_cast<uint8_t*>(g->ring[g->ring_index].contents) + off, p, n);
  g->ring_off = off + n;
  return off;
}

void EnsureEncoder() {
  EnsureCmd();
  if (g->enc) return;
  MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
  if (g->cur_color) {
    rp.colorAttachments[0].texture = g->texs[g->cur_color].t;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
  }
  if (g->cur_depth) {
    rp.depthAttachment.texture = g->texs[g->cur_depth].t;
    rp.depthAttachment.loadAction = MTLLoadActionLoad;
    rp.depthAttachment.storeAction = MTLStoreActionStore;
    rp.stencilAttachment.texture = g->texs[g->cur_depth].t;
    rp.stencilAttachment.loadAction = MTLLoadActionLoad;
    rp.stencilAttachment.storeAction = MTLStoreActionStore;
  }
  if (!g->cur_color && !g->cur_depth) {
    rp.renderTargetWidth = g->cur_w;
    rp.renderTargetHeight = g->cur_h;
    rp.defaultRasterSampleCount = 1;
  }
  g->enc = [g->cmd renderCommandEncoderWithDescriptor:rp];
}

id<MTLFunction> CompileFn(const std::string& msl, const char* entry, const char* tag) {
  NSError* err = nil;
  MTLCompileOptions* o = [MTLCompileOptions new];
  o.mathMode = MTLMathModeSafe;
  id<MTLLibrary> lib = [g->dev newLibraryWithSource:[NSString stringWithUTF8String:msl.c_str()]
                                            options:o
                                              error:&err];
  if (!lib) {
    hp2_log("d3d8: MSL compile of %s failed: %s", tag, err.localizedDescription.UTF8String);
    const char* dump = getenv("HP2_LOG_DIR");
    if (dump) {
      std::string p = std::string(dump) + "/msl_fail_" + tag + ".metal";
      if (FILE* f = fopen(p.c_str(), "w")) {
        fwrite(msl.data(), 1, msl.size(), f);
        fclose(f);
      }
    }
    return nil;
  }
  return [lib newFunctionWithName:[NSString stringWithUTF8String:entry]];
}

ProgId AddProg(id<MTLFunction> f) {
  if (!f) return 0;
  std::lock_guard<std::mutex> lk(g->prog_mu);
  g->progs.push_back(f);
  return ProgId(g->progs.size() - 1);
}

uint32_t PackBlend(const BlendState& b) {
  return uint32_t(b.enable) | uint32_t(b.src) << 1 | uint32_t(b.dst) << 6 | uint32_t(b.op) << 11 |
         uint32_t(b.src_a) << 14 | uint32_t(b.dst_a) << 19 | uint32_t(b.op_a) << 24 |
         uint32_t(b.write_mask) << 27;
}

id<MTLRenderPipelineState> Pipe(ProgId vs, ProgId fs, const BlendState& b, bool has_color,
                                bool has_depth) {
  PipeKey k{vs, fs, PackBlend(b), uint8_t(has_color), uint8_t(has_depth)};
  auto it = g->pipes.find(k);
  if (it != g->pipes.end()) return it->second;
  MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
  {
    std::lock_guard<std::mutex> lk(g->prog_mu);
    d.vertexFunction = g->progs[vs];
    d.fragmentFunction = g->progs[fs];
  }
  if (has_color) {
    auto* ca = d.colorAttachments[0];
    ca.pixelFormat = MTLPixelFormatBGRA8Unorm;
    ca.blendingEnabled = b.enable;
    ca.sourceRGBBlendFactor = MTLBlendFactor(b.src);
    ca.destinationRGBBlendFactor = MTLBlendFactor(b.dst);
    ca.rgbBlendOperation = MTLBlendOperation(b.op);
    ca.sourceAlphaBlendFactor = MTLBlendFactor(b.src_a);
    ca.destinationAlphaBlendFactor = MTLBlendFactor(b.dst_a);
    ca.alphaBlendOperation = MTLBlendOperation(b.op_a);
    ca.writeMask = MTLColorWriteMask(b.write_mask);
  }
  if (has_depth) {
    d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    d.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
  }
  d.inputPrimitiveTopology = MTLPrimitiveTopologyClassUnspecified;
  NSError* err = nil;
  id<MTLRenderPipelineState> p = [g->dev newRenderPipelineStateWithDescriptor:d error:&err];
  if (!p) hp2_log("d3d8: pipeline failed: %s", err.localizedDescription.UTF8String);
  g->pipes[k] = p;
  return p;
}

id<MTLDepthStencilState> Dss(const DepthState& s) {
  uint64_t key = uint64_t(s.test) | uint64_t(s.write) << 1 | uint64_t(s.func) << 2 |
                 uint64_t(s.stencil) << 5 | uint64_t(s.s_func) << 6 | uint64_t(s.s_fail) << 9 |
                 uint64_t(s.s_zfail) << 12 | uint64_t(s.s_pass) << 15 |
                 uint64_t(s.s_read) << 18 | uint64_t(s.s_write) << 26;
  auto it = g->dss.find(key);
  if (it != g->dss.end()) return it->second;
  MTLDepthStencilDescriptor* d = [MTLDepthStencilDescriptor new];
  d.depthCompareFunction = s.test ? MTLCompareFunction(s.func) : MTLCompareFunctionAlways;
  d.depthWriteEnabled = s.test && s.write;
  if (s.stencil) {
    MTLStencilDescriptor* st = [MTLStencilDescriptor new];
    st.stencilCompareFunction = MTLCompareFunction(s.s_func);
    st.stencilFailureOperation = MTLStencilOperation(s.s_fail);
    st.depthFailureOperation = MTLStencilOperation(s.s_zfail);
    st.depthStencilPassOperation = MTLStencilOperation(s.s_pass);
    st.readMask = s.s_read;
    st.writeMask = s.s_write;
    d.frontFaceStencil = st;
    d.backFaceStencil = st;
  }
  id<MTLDepthStencilState> r = [g->dev newDepthStencilStateWithDescriptor:d];
  g->dss[key] = r;
  return r;
}

id<MTLSamplerState> Sampler(const SamplerState& s) {
  uint64_t key = uint64_t(s.addr_u) | uint64_t(s.addr_v) << 4 | uint64_t(s.addr_w) << 8 |
                 uint64_t(s.mag) << 12 | uint64_t(s.min) << 14 | uint64_t(s.mip) << 16 |
                 uint64_t(s.max_aniso) << 20;
  auto k = std::make_tuple(key, s.border);
  auto it = g->samplers.find(k);
  if (it != g->samplers.end()) return it->second;
  MTLSamplerDescriptor* d = [MTLSamplerDescriptor new];
  d.sAddressMode = MTLSamplerAddressMode(s.addr_u);
  d.tAddressMode = MTLSamplerAddressMode(s.addr_v);
  d.rAddressMode = MTLSamplerAddressMode(s.addr_w);
  d.magFilter = MTLSamplerMinMagFilter(s.mag);
  d.minFilter = MTLSamplerMinMagFilter(s.min);
  d.mipFilter = MTLSamplerMipFilter(s.mip);
  d.maxAnisotropy = s.max_aniso ? s.max_aniso : 1;
  // Metal has only three border colours: pick the closest.
  uint32_t a = s.border >> 24, rgb = s.border & 0xFFFFFF;
  d.borderColor = a < 128 ? MTLSamplerBorderColorTransparentBlack
                  : rgb >= 0x808080 ? MTLSamplerBorderColorOpaqueWhite
                                    : MTLSamplerBorderColorOpaqueBlack;
  id<MTLSamplerState> r = [g->dev newSamplerStateWithDescriptor:d];
  g->samplers[k] = r;
  return r;
}

const char* kInternalMsl = R"(
#include <metal_stdlib>
using namespace metal;
struct ClearIn { float4 rect; float z; float4 color; };
struct ClearOut { float4 pos [[position]]; float4 color; };
vertex ClearOut clear_vs(uint vid [[vertex_id]], constant ClearIn& c [[buffer(0)]]) {
  float2 p = float2((vid & 1) ? c.rect.z : c.rect.x, (vid & 2) ? c.rect.w : c.rect.y);
  ClearOut o; o.pos = float4(p, c.z, 1); o.color = c.color; return o;
}
fragment float4 clear_fs(ClearOut i [[stage_in]]) { return i.color; }
struct CopyOut { float4 pos [[position]]; float2 uv; };
struct CopyIn { float4 dst; float4 src; };
vertex CopyOut copy_vs(uint vid [[vertex_id]], constant CopyIn& c [[buffer(0)]]) {
  float2 t = float2(vid & 1, (vid >> 1) & 1);
  CopyOut o;
  o.pos = float4(mix(c.dst.xy, c.dst.zw, t), 0, 1);
  o.uv = mix(c.src.xy, c.src.zw, t);
  return o;
}
// Catmull-Rom bicubic (9 bilinear taps), for the window present.
fragment float4 present_bicubic(CopyOut i [[stage_in]], texture2d<float> t [[texture(0)]],
                                sampler s [[sampler(0)]]) {
  float2 size = float2(t.get_width(), t.get_height());
  float2 p = i.uv * size - 0.5, f = fract(p), c = floor(p) + 0.5;
  float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
  float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);
  float2 w12 = w1 + w2, o12 = w2 / w12;
  float2 t0 = (c - 1.0) / size, t3 = (c + 2.0) / size, t12 = (c + o12) / size;
  float4 r = 0;
  r += t.sample(s, float2(t0.x, t0.y)) * w0.x * w0.y;
  r += t.sample(s, float2(t12.x, t0.y)) * w12.x * w0.y;
  r += t.sample(s, float2(t3.x, t0.y)) * w3.x * w0.y;
  r += t.sample(s, float2(t0.x, t12.y)) * w0.x * w12.y;
  r += t.sample(s, float2(t12.x, t12.y)) * w12.x * w12.y;
  r += t.sample(s, float2(t3.x, t12.y)) * w3.x * w12.y;
  r += t.sample(s, float2(t0.x, t3.y)) * w0.x * w3.y;
  r += t.sample(s, float2(t12.x, t3.y)) * w12.x * w3.y;
  r += t.sample(s, float2(t3.x, t3.y)) * w3.x * w3.y;
  return float4(saturate(r.rgb), 1);
}
fragment float4 copy_fs(CopyOut i [[stage_in]], texture2d<float> t [[texture(0)]],
                        sampler s [[sampler(0)]]) { return t.sample(s, i.uv); }
// Downscaling present (frame larger than the window, e.g. 3x = 1440 rows into
// 1250): the average over the window pixel's footprint in the frame, 4x4
// bilinear taps. One bilinear tap per pixel skips frame pixels and makes fine
// detail (lattice windows, grass edges) crawl while the camera moves.
fragment float4 present_down(CopyOut i [[stage_in]], texture2d<float> t [[texture(0)]],
                             sampler s [[sampler(0)]]) {
  float2 fp = float2(abs(dfdx(i.uv.x)), abs(dfdy(i.uv.y)));  // footprint in uv
  float4 r = 0;
  for (int y = 0; y < 4; y++)
    for (int x = 0; x < 4; x++)
      r += t.sample(s, i.uv + fp * (float2(x, y) - 1.5) * 0.25);
  return float4(r.rgb / 16.0, 1);
}
)";

}  // namespace

bool Init() {
  if (g) return true;
  g = new State;
  g->dev = MTLCreateSystemDefaultDevice();
  if (!g->dev) {
    hp2_log("d3d8: no Metal device");
    return false;
  }
  g->queue = [g->dev newCommandQueue];
  g->inflight = dispatch_semaphore_create(kFramesInFlight);
  for (auto& r : g->ring)
    r = [g->dev newBufferWithLength:kRingSize options:MTLResourceStorageModeShared];
  NSError* err = nil;
  id<MTLLibrary> lib =
      [g->dev newLibraryWithSource:[NSString stringWithUTF8String:kInternalMsl] options:nil error:&err];
  if (!lib) hp2_fatal("d3d8: internal shaders: %s", err.localizedDescription.UTF8String);
  g->clear_vs = [lib newFunctionWithName:@"clear_vs"];
  g->clear_fs = [lib newFunctionWithName:@"clear_fs"];
  g->copy_vs = [lib newFunctionWithName:@"copy_vs"];
  g->copy_fs = [lib newFunctionWithName:@"copy_fs"];
  for (int c = 0; c < 2; c++)
    for (int d = 0; d < 2; d++)
      for (int w = 0; w < 2; w++) {
        if (!c && !d) continue;
        MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = g->clear_vs;
        pd.fragmentFunction = g->clear_fs;
        if (c) {
          pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
          pd.colorAttachments[0].writeMask = w ? MTLColorWriteMaskAll : MTLColorWriteMaskNone;
        }
        if (d) {
          pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
          pd.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
        }
        g->clear_pipe[c][d][w] = [g->dev newRenderPipelineStateWithDescriptor:pd error:&err];
      }
  MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
  pd.vertexFunction = g->copy_vs;
  pd.fragmentFunction = g->copy_fs;
  pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
  g->copy_pipe = [g->dev newRenderPipelineStateWithDescriptor:pd error:&err];
  pd.fragmentFunction = [lib newFunctionWithName:@"present_bicubic"];
  g->bicubic_pipe = [g->dev newRenderPipelineStateWithDescriptor:pd error:&err];
  pd.fragmentFunction = [lib newFunctionWithName:@"present_down"];
  g->down_pipe = [g->dev newRenderPipelineStateWithDescriptor:pd error:&err];
  const char* f = getenv("HP2_FILTER");
  g->bicubic = f && !strcmp(f, "bicubic");
  g->plain_present = f && !strcmp(f, "bilinear");  // A/B: one bilinear tap, also downscaling
  MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
  sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
  g->copy_samp = [g->dev newSamplerStateWithDescriptor:sd];
  hp2_log("d3d8: Metal device %s", g->dev.name.UTF8String);
  return true;
}

// Set by the main thread (window loop), possibly before Init; the game thread
// configures the layer the first time it presents to it.
std::atomic<void*> g_present_layer{nullptr};

void SetPresentLayer(void* l) {
  void* old = g_present_layer.exchange(l ? (void*)CFBridgingRetain((__bridge id)l) : nullptr);
  (void)old;  // never released: the window lives as long as the process
}

std::atomic<bool> g_keep_copies{false};  // may be set before Init

void KeepPresentCopies(bool on) { g_keep_copies = on; }

TexId LastPresentCopy(uint32_t* w, uint32_t* h) {
  *w = g->present_w, *h = g->present_h;
  return g->present_copy;
}

// Polish post-processing (src/polish/postfx), registered by Polish at init.
// Draws src (uv rect) into the fit rect vp {x, y, w, h} of dst (clearing the
// bars) in cmd; false = not drawn, the plain present below runs.
using PostFxFn = bool (*)(void* cmd, void* src, const float uv[4], void* dst, const float vp[4]);
std::atomic<PostFxFn> g_postfx{nullptr};

void PresentFrame(TexId src, uint32_t src_w, uint32_t src_h) {
  if (!src || src >= g->texs.size() || !g->texs[src].t || !src_w || !src_h) return;
  CAMetalLayer* layer = (__bridge CAMetalLayer*)g_present_layer.load();
  if (!layer) return;
  if (layer != g->layer) {
    layer.device = g->dev;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = !g_keep_copies.load();
    layer.maximumDrawableCount = 3;
    g->layer = layer;
  }
  @autoreleasepool {
    id<CAMetalDrawable> dr = [layer nextDrawable];
    if (!dr) return;
    id<MTLTexture> t = dr.texture;
    double dw = double(t.width), dh = double(t.height);
    // Fit 4:3 (the Xbox output aspect, whatever the render size) into the drawable.
    const double ar = double(src_w) / double(src_h);  // 4:3, or wider in widescreen
    double fw = dw, fh = dw / ar;
    if (fh > dh) fh = dh, fw = dh * ar;
    double fx = (dw - fw) / 2, fy = (dh - fh) / 2;
    EndEncoder();
    EnsureCmd();
    const Tex& s = g->texs[src];
    const float pf_uv[4] = {0, 0, float(src_w) / s.w, float(src_h) / s.h};
    const float pf_vp[4] = {float(fx), float(fy), float(fw), float(fh)};
    PostFxFn pf = g_postfx.load();
    if (!pf || !pf((__bridge void*)g->cmd, (__bridge void*)s.t, pf_uv, (__bridge void*)t, pf_vp)) {
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = t;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [g->cmd renderCommandEncoderWithDescriptor:rp];
    // Upscaling: Catmull-Rom if HP2_FILTER=bicubic; else bilinear.
    bool up = fw > src_w + 0.5 || fh > src_h + 0.5;
    bool down = (fw < src_w - 0.5 || fh < src_h - 0.5) && !g->plain_present;
    [e setRenderPipelineState:down ? g->down_pipe : (g->bicubic && up) ? g->bicubic_pipe : g->copy_pipe];
    struct {
      float dst[4], src[4];
    } in;
    in.dst[0] = float(2 * fx / dw - 1), in.dst[1] = float(1 - 2 * fy / dh);
    in.dst[2] = float(2 * (fx + fw) / dw - 1), in.dst[3] = float(1 - 2 * (fy + fh) / dh);
    in.src[0] = 0, in.src[1] = 0;
    in.src[2] = float(src_w) / s.w, in.src[3] = float(src_h) / s.h;
    [e setVertexBytes:&in length:sizeof in atIndex:0];
    [e setFragmentTexture:s.t atIndex:0];
    [e setFragmentSamplerState:g->copy_samp atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [e endEncoding];
    }
    if (g_keep_copies.load()) {
      if (!g->present_copy || g->present_w != t.width || g->present_h != t.height) {
        DestroyTexture(g->present_copy);
        g->present_copy = CreateTexture(TexKind::BGRA8, uint32_t(t.width), uint32_t(t.height), 1,
                                        false, true);
        g->present_w = uint32_t(t.width), g->present_h = uint32_t(t.height);
      }
      id<MTLBlitCommandEncoder> b = [g->cmd blitCommandEncoder];
      [b copyFromTexture:t toTexture:g->texs[g->present_copy].t];
      [b endEncoding];
    }
    // Frame cadence: the game runs on the kernel's 60 Hz pacer, not on the
    // display's vblank. On a 120 Hz (ProMotion) or variable-rate panel a plain
    // present shows each frame until the next vblank after it lands - 8, 17 or
    // 25 ms - and moving fine detail (grass, lattice windows) shimmers. Each
    // frame stays up at least one 60 Hz period instead (HP2_PRESENT_MIN=0: off).
    static const double min_s = [] {
      const char* v = getenv("HP2_PRESENT_MIN");
      return v && atoi(v) == 0 ? 0.0 : 1.0 / 62.0;  // a little under 1/60: no skipped vblank at 60 Hz
    }();
    if (min_s > 0)
      [g->cmd presentDrawable:dr afterMinimumDuration:min_s];
    else
      [g->cmd presentDrawable:dr];
  }
}

double TakeGpuWaitMs() {
  double v = g_gpu_wait_ms;
  g_gpu_wait_ms = 0;
  return v;
}

const char* DeviceName() { return g && g->dev ? g->dev.name.UTF8String : "none"; }

TexId CreateTexture(TexKind kind, uint32_t w, uint32_t h, uint32_t levels, bool cube,
                    bool render_target) {
  MTLTextureDescriptor* d = [MTLTextureDescriptor new];
  d.textureType = cube ? MTLTextureTypeCube : MTLTextureType2D;
  d.pixelFormat = PixFmt(kind);
  d.width = w ? w : 1;
  d.height = h ? h : 1;
  d.mipmapLevelCount = levels ? levels : 1;
  d.usage = MTLTextureUsageShaderRead | (render_target ? MTLTextureUsageRenderTarget : 0);
  d.storageMode = render_target ? MTLStorageModePrivate : MTLStorageModeShared;
  if (kind == TexKind::Depth) d.storageMode = MTLStorageModePrivate;
  id<MTLTexture> t = [g->dev newTextureWithDescriptor:d];
  if (!t) {
    hp2_log("d3d8: texture %ux%u kind %d failed", w, h, int(kind));
    return 0;
  }
  TexId id;
  if (!g->free_ids.empty()) {
    id = g->free_ids.back();
    g->free_ids.pop_back();
  } else {
    id = TexId(g->texs.size());
    g->texs.push_back({});
  }
  g->texs[id] = Tex{t, kind, w, h};
  return id;
}

TexId OpaqueView(TexId src) {
  if (!src || src >= g->texs.size() || !g->texs[src].t) return 0;
  const Tex& x = g->texs[src];
  MTLTextureSwizzleChannels sw = MTLTextureSwizzleChannelsMake(
      MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleOne);
  id<MTLTexture> v = [x.t newTextureViewWithPixelFormat:x.t.pixelFormat
                                            textureType:x.t.textureType
                                                 levels:NSMakeRange(0, x.t.mipmapLevelCount)
                                                 slices:NSMakeRange(0, x.t.arrayLength)
                                                swizzle:sw];
  if (!v) return 0;
  TexId id;
  if (!g->free_ids.empty()) {
    id = g->free_ids.back();
    g->free_ids.pop_back();
  } else {
    id = TexId(g->texs.size());
    g->texs.push_back({});
  }
  g->texs[id] = Tex{v, x.kind, x.w, x.h};
  return id;
}

void DestroyTexture(TexId t) {
  if (!t || t >= g->texs.size() || !g->texs[t].t) return;
  if (t == g->cur_color || t == g->cur_depth) {
    EndEncoder();
    if (t == g->cur_color) g->cur_color = 0;
    if (t == g->cur_depth) g->cur_depth = 0;
  }
  g->texs[t] = Tex{};  // the command buffer keeps it alive while in use
  g->free_ids.push_back(t);
}

void UploadTexture(TexId t, uint32_t face, uint32_t level, uint32_t w, uint32_t h,
                   const void* data, uint32_t pitch) {
  if (!t || !g->texs[t].t) return;
  const Tex& x = g->texs[t];
  bool bc = x.kind == TexKind::BC1 || x.kind == TexKind::BC2 || x.kind == TexKind::BC3;
  MTLRegion r = MTLRegionMake2D(0, 0, w, h);
  uint32_t rows = bc ? (h + 3) / 4 : h;
  [x.t replaceRegion:r
         mipmapLevel:level
               slice:face
           withBytes:data
         bytesPerRow:pitch
       bytesPerImage:pitch * rows];
}

void GenerateMips(TexId t) {
  if (!t || t >= g->texs.size() || !g->texs[t].t || g->texs[t].t.mipmapLevelCount < 2) return;
  id<MTLCommandBuffer> cb = [g->queue commandBuffer];
  id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
  [b generateMipmapsForTexture:g->texs[t].t];
  [b endEncoding];
  [cb commit];
}

ProgId CompileVertex(const std::string& msl, const char* tag) {
  return AddProg(CompileFn(msl, "vmain", tag));
}
ProgId CompileFragment(const std::string& msl, const char* tag) {
  return AddProg(CompileFn(msl, "fmain", tag));
}

void SetTargets(TexId color, TexId depth, uint32_t w, uint32_t h) {
  if (color == g->cur_color && depth == g->cur_depth && w == g->cur_w && h == g->cur_h) return;
  EndEncoder();
  g->cur_color = color;
  g->cur_depth = depth;
  g->cur_w = w;
  g->cur_h = h;
}

void Clear(bool color, bool depth, bool stencil, uint32_t argb, float z, uint32_t s,
           const int32_t* rects, uint32_t nrects) {
  bool c = color && g->cur_color;
  bool d = (depth || stencil) && g->cur_depth;
  if (!c && !d) return;
  EnsureEncoder();
  bool hc = g->cur_color != 0, hd = g->cur_depth != 0;
  [g->enc setRenderPipelineState:g->clear_pipe[hc][hd][c]];
  DepthState ds;
  ds.test = depth && hd;
  ds.write = depth && hd;
  ds.func = MTLCompareFunctionAlways;
  ds.stencil = stencil && hd;
  ds.s_func = MTLCompareFunctionAlways;
  ds.s_fail = ds.s_zfail = ds.s_pass = MTLStencilOperationReplace;
  ds.s_write = 0xFF;
  if (hd) [g->enc setDepthStencilState:Dss(ds)];
  [g->enc setStencilReferenceValue:s & 0xFF];
  [g->enc setCullMode:MTLCullModeNone];
  [g->enc setTriangleFillMode:MTLTriangleFillModeFill];
  [g->enc setDepthBias:0 slopeScale:0 clamp:0];
  MTLViewport vp{0, 0, double(g->cur_w), double(g->cur_h), 0, 1};
  [g->enc setViewport:vp];
  struct {
    float rect[4];
    float z, pad[3];
    float color[4];
  } in{};
  in.z = z;
  in.color[0] = ((argb >> 16) & 0xFF) / 255.f;
  in.color[1] = ((argb >> 8) & 0xFF) / 255.f;
  in.color[2] = (argb & 0xFF) / 255.f;
  in.color[3] = (argb >> 24) / 255.f;
  int32_t full[4] = {0, 0, int32_t(g->cur_w), int32_t(g->cur_h)};
  if (!nrects) rects = full, nrects = 1;
  for (uint32_t i = 0; i < nrects; i++) {
    const int32_t* r = rects + 4 * i;
    int32_t x0 = std::max(0, r[0]), y0 = std::max(0, r[1]);
    int32_t x1 = std::min(int32_t(g->cur_w), r[2]), y1 = std::min(int32_t(g->cur_h), r[3]);
    if (x1 <= x0 || y1 <= y0) continue;
    MTLScissorRect sc{NSUInteger(x0), NSUInteger(y0), NSUInteger(x1 - x0), NSUInteger(y1 - y0)};
    [g->enc setScissorRect:sc];
    in.rect[0] = -1, in.rect[1] = -1, in.rect[2] = 1, in.rect[3] = 1;
    [g->enc setVertexBytes:&in length:sizeof in atIndex:0];
    [g->enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
  }
}

void DoDraw(const Draw& d) {
  if (!d.vs || !d.fs) return;
  EnsureEncoder();
  id<MTLRenderPipelineState> p = Pipe(d.vs, d.fs, d.blend, g->cur_color != 0, g->cur_depth != 0);
  if (!p) return;
  id<MTLRenderCommandEncoder> e = g->enc;
  [e setRenderPipelineState:p];
  if (g->cur_depth) [e setDepthStencilState:Dss(d.depth)];
  [e setStencilReferenceValue:d.stencil_ref];
  [e setBlendColorRed:d.blend_color[0] green:d.blend_color[1] blue:d.blend_color[2]
                alpha:d.blend_color[3]];
  [e setCullMode:MTLCullMode(d.cull)];
  [e setFrontFacingWinding:d.front_ccw ? MTLWindingCounterClockwise : MTLWindingClockwise];
  [e setTriangleFillMode:d.wireframe ? MTLTriangleFillModeLines : MTLTriangleFillModeFill];
  [e setDepthBias:d.depth_bias slopeScale:d.slope_bias clamp:0];
  MTLViewport vp{d.vp_x, d.vp_y, d.vp_w, d.vp_h, d.vp_minz, d.vp_maxz};
  [e setViewport:vp];
  int32_t sx = std::max(0, d.sc_x), sy = std::max(0, d.sc_y);
  int32_t sx1 = std::min(int32_t(g->cur_w), d.sc_x + d.sc_w);
  int32_t sy1 = std::min(int32_t(g->cur_h), d.sc_y + d.sc_h);
  if (sx1 <= sx || sy1 <= sy) return;
  MTLScissorRect sc{NSUInteger(sx), NSUInteger(sy), NSUInteger(sx1 - sx), NSUInteger(sy1 - sy)};
  [e setScissorRect:sc];
  for (int i = 0; i < 4; i++) {
    if (d.streams[i].data && d.streams[i].size) {
      size_t off = RingPut(d.streams[i].data, d.streams[i].size);
      [e setVertexBuffer:g->ring[g->ring_index] offset:off atIndex:1 + i];
    } else {
      [e setVertexBuffer:g->ring[g->ring_index] offset:0 atIndex:1 + i];
    }
  }
  if (d.vs_uniforms_size) [e setVertexBytes:d.vs_uniforms length:d.vs_uniforms_size atIndex:0];
  if (d.fs_uniforms_size) [e setFragmentBytes:d.fs_uniforms length:d.fs_uniforms_size atIndex:0];
  for (int i = 0; i < 4; i++) {
    if (d.tex[i] && d.tex[i] < g->texs.size() && g->texs[d.tex[i]].t) {
      [e setFragmentTexture:g->texs[d.tex[i]].t atIndex:i];
      [e setFragmentSamplerState:Sampler(d.samp[i]) atIndex:i];
    } else {
      [e setFragmentTexture:nil atIndex:i];
      [e setFragmentSamplerState:Sampler(SamplerState{}) atIndex:i];
    }
  }
  MTLPrimitiveType pt = MTLPrimitiveType(d.mtl_prim);
  if (d.indices) {
    if (!d.index_count) return;
    size_t off = RingPut(d.indices, d.index_count * 2);
    [e drawIndexedPrimitives:pt
                  indexCount:d.index_count
                   indexType:MTLIndexTypeUInt16
                 indexBuffer:g->ring[g->ring_index]
           indexBufferOffset:off
               instanceCount:1
                  baseVertex:d.base_vertex
                baseInstance:0];
  } else if (d.vertex_count) {
    [e drawPrimitives:pt vertexStart:d.base_vertex vertexCount:d.vertex_count];
  }
}

void CopyRegion(TexId src, int sx, int sy, int sw, int sh, TexId dst, int dx, int dy, int dw,
                int dh) {
  if (!src || !dst || src == dst) return;
  const Tex& s = g->texs[src];
  const Tex& t = g->texs[dst];
  if (!s.t || !t.t) return;
  TexId oc = g->cur_color, od = g->cur_depth;
  uint32_t ow = g->cur_w, oh = g->cur_h;
  EndEncoder();
  EnsureCmd();
  if (sw == dw && sh == dh && s.kind == t.kind) {
    id<MTLBlitCommandEncoder> b = [g->cmd blitCommandEncoder];
    [b copyFromTexture:s.t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(sx, sy, 0)
            sourceSize:MTLSizeMake(sw, sh, 1) toTexture:t.t destinationSlice:0
      destinationLevel:0 destinationOrigin:MTLOriginMake(dx, dy, 0)];
    [b endEncoding];
  } else {
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = t.t;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [g->cmd renderCommandEncoderWithDescriptor:rp];
    [e setRenderPipelineState:g->copy_pipe];
    struct {
      float dst[4], src[4];
    } in;
    in.dst[0] = 2.f * dx / t.w - 1, in.dst[1] = 1 - 2.f * dy / t.h;
    in.dst[2] = 2.f * (dx + dw) / t.w - 1, in.dst[3] = 1 - 2.f * (dy + dh) / t.h;
    in.src[0] = float(sx) / s.w, in.src[1] = float(sy) / s.h;
    in.src[2] = float(sx + sw) / s.w, in.src[3] = float(sy + sh) / s.h;
    [e setVertexBytes:&in length:sizeof in atIndex:0];
    [e setFragmentTexture:s.t atIndex:0];
    [e setFragmentSamplerState:g->copy_samp atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [e endEncoding];
  }
  g->cur_color = oc, g->cur_depth = od, g->cur_w = ow, g->cur_h = oh;
}

void EndFrame() {
  EndEncoder();
  if (g->cmd) {
    [g->cmd commit];
    g->cmd = nil;
  }
}

bool ReadPixels(TexId t, uint32_t w, uint32_t h, std::vector<uint8_t>* rgba) {
  if (!t || !g->texs[t].t) return false;
  const Tex& x = g->texs[t];
  w = std::min(w, x.w);
  h = std::min(h, x.h);
  EndEncoder();
  EnsureCmd();
  id<MTLBuffer> buf = [g->dev newBufferWithLength:size_t(w) * h * 4
                                          options:MTLResourceStorageModeShared];
  id<MTLBlitCommandEncoder> b = [g->cmd blitCommandEncoder];
  [b copyFromTexture:x.t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
          sourceSize:MTLSizeMake(w, h, 1) toBuffer:buf destinationOffset:0
  destinationBytesPerRow:w * 4 destinationBytesPerImage:size_t(w) * h * 4];
  [b endEncoding];
  id<MTLCommandBuffer> cb = g->cmd;
  [cb commit];
  g->cmd = nil;
  [cb waitUntilCompleted];
  rgba->resize(size_t(w) * h * 4);
  const uint8_t* s = static_cast<const uint8_t*>(buf.contents);
  uint8_t* o = rgba->data();
  for (size_t i = 0; i < size_t(w) * h; i++) {  // BGRA -> RGBA, opaque
    o[4 * i + 0] = s[4 * i + 2];
    o[4 * i + 1] = s[4 * i + 1];
    o[4 * i + 2] = s[4 * i + 0];
    o[4 * i + 3] = 0xFF;
  }
  return true;
}

bool WritePng(const char* path, uint32_t w, uint32_t h, const uint8_t* rgba) {
  @autoreleasepool {
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGDataProviderRef dp = CGDataProviderCreateWithData(nullptr, rgba, size_t(w) * h * 4, nullptr);
    CGImageRef img = CGImageCreate(w, h, 8, 32, w * 4, cs,
                                   kCGBitmapByteOrderDefault | kCGImageAlphaNoneSkipLast, dp,
                                   nullptr, false, kCGRenderingIntentDefault);
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
    CGImageDestinationRef dst =
        CGImageDestinationCreateWithURL((__bridge CFURLRef)url, CFSTR("public.png"), 1, nullptr);
    bool ok = false;
    if (dst) {
      CGImageDestinationAddImage(dst, img, nullptr);
      ok = CGImageDestinationFinalize(dst);
      CFRelease(dst);
    }
    CGImageRelease(img);
    CGDataProviderRelease(dp);
    CGColorSpaceRelease(cs);
    return ok;
  }
}

}  // namespace hp2d3d::gpu

// Polish registers its post-processing pass here (see PostFxFn); null = none.
extern "C" void hp2_d3d8_set_postfx(hp2d3d::gpu::PostFxFn f) { hp2d3d::gpu::g_postfx = f; }
