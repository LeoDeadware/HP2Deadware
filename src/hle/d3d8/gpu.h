#pragma once
// Host GPU layer of the D3D8 HLE (Metal, gpu_metal.mm). The HLE (d3d8.cpp)
// owns all Xbox semantics; this layer only knows textures, render targets,
// generated MSL programs and draws. Sizes passed here are physical (already
// multiplied by the render scale).

#include <cstdint>
#include <string>
#include <vector>

namespace hp2d3d::gpu {

using TexId = uint32_t;   // 0 = none
using ProgId = uint32_t;  // 0 = none / failed

enum class TexKind : uint8_t {
  BGRA8,       // all uncompressed colour formats are converted to this
  BC1, BC2, BC3,
  Depth,       // depth/stencil target (Depth32Float_Stencil8)
};

// Creates a 2D (or cube) texture. render_target: may be a colour/depth attachment.
TexId CreateTexture(TexKind kind, uint32_t w, uint32_t h, uint32_t levels, bool cube,
                    bool render_target);
void DestroyTexture(TexId t);
// A view of colour texture t whose alpha always reads 1 (for render targets in
// Xbox formats without alpha, e.g. R5G6B5: their storage here is BGRA8).
// Destroyed with DestroyTexture like any texture.
TexId OpaqueView(TexId t);
// Uploads one level (face 0..5 for cubes). BGRA8: 4 bytes/pixel rows of
// `pitch` bytes; BCn: rows of 4x4 blocks.
void UploadTexture(TexId t, uint32_t face, uint32_t level, uint32_t w, uint32_t h,
                   const void* data, uint32_t pitch);
// Fills levels 1.. of colour texture t from its level 0 on the GPU (box filter;
// a command buffer of its own, committed now: it runs before the frame's).
void GenerateMips(TexId t);

// Programs: MSL source with the entry points named "vmain"/"fmain".
// Compiled once per source (cached by the caller's key); 0 on error (logged).
ProgId CompileVertex(const std::string& msl, const char* tag);
ProgId CompileFragment(const std::string& msl, const char* tag);

struct BlendState {
  bool enable = false;
  uint8_t src = 1, dst = 0, op = 0;            // MTLBlendFactor / MTLBlendOperation
  uint8_t src_a = 1, dst_a = 0, op_a = 0;
  uint8_t write_mask = 0xF;                     // MTLColorWriteMask (R=8,G=4,B=2,A=1)
};
struct DepthState {
  bool test = false, write = false;
  uint8_t func = 7;                              // MTLCompareFunction
  bool stencil = false;
  uint8_t s_func = 7, s_fail = 0, s_zfail = 0, s_pass = 0;  // MTLStencilOperation
  uint8_t s_read = 0xFF, s_write = 0xFF;
};
struct SamplerState {
  uint8_t addr_u = 0, addr_v = 0, addr_w = 0;   // MTLSamplerAddressMode
  uint8_t mag = 1, min = 1, mip = 0;            // MTLSamplerMinMagFilter / MipFilter
  uint8_t max_aniso = 1;
  uint32_t border = 0;                           // ARGB
};

struct Stream {
  const void* data = nullptr;  // host pointer to the first byte the draw may read
  uint32_t size = 0;
};

struct Draw {
  ProgId vs = 0, fs = 0;
  uint32_t mtl_prim = 3;           // MTLPrimitiveType (0 point,1 line,2 linestrip,3 tri,4 tristrip)
  uint32_t vertex_count = 0;       // non-indexed
  int32_t base_vertex = 0;
  const uint16_t* indices = nullptr;  // indexed when non-null (vertex ids, before base_vertex)
  uint32_t index_count = 0;
  Stream streams[4];
  const void* vs_uniforms = nullptr;
  uint32_t vs_uniforms_size = 0;
  const void* fs_uniforms = nullptr;
  uint32_t fs_uniforms_size = 0;
  TexId tex[4] = {};
  SamplerState samp[4];
  BlendState blend;
  float blend_color[4] = {};
  DepthState depth;
  uint32_t stencil_ref = 0;
  uint8_t cull = 0;                // MTLCullMode
  bool front_ccw = false;          // MTLWinding
  bool wireframe = false;
  float depth_bias = 0, slope_bias = 0;
  // Viewport / scissor in physical pixels of the current target.
  float vp_x = 0, vp_y = 0, vp_w = 0, vp_h = 0, vp_minz = 0, vp_maxz = 1;
  int32_t sc_x = 0, sc_y = 0, sc_w = 0, sc_h = 0;
};

// Current targets (physical size w x h). color or depth may be 0.
void SetTargets(TexId color, TexId depth, uint32_t w, uint32_t h);
// Clears rectangles of the current targets (physical pixels; none = all).
void Clear(bool color, bool depth, bool stencil, uint32_t argb, float z, uint32_t s,
           const int32_t* rects, uint32_t nrects);
void DoDraw(const Draw& d);
// Copies a region between colour textures (scaled, linear filter).
void CopyRegion(TexId src, int sx, int sy, int sw, int sh, TexId dst, int dx, int dy, int dw,
                int dh);
// Ends the frame: submits the GPU work. Non-blocking (bounded frames in flight).
void EndFrame();
// Reads a colour texture back as RGBA8 rows (waits for the GPU).
bool ReadPixels(TexId t, uint32_t w, uint32_t h, std::vector<uint8_t>* rgba);
// Writes RGBA8 pixels as a PNG.
bool WritePng(const char* path, uint32_t w, uint32_t h, const uint8_t* rgba);

// ---- window presentation
// The CAMetalLayer frames go to (null: none). Set from the main thread.
void SetPresentLayer(void* ca_metal_layer);
// Draws colour texture `src` (its top-left src_w x src_h physical pixels) into
// the layer's next drawable, scaled to fit with the 4:3 aspect kept and black
// bars (pillar/letterbox), and schedules it with this frame. No-op without a layer.
void PresentFrame(TexId src, uint32_t src_w, uint32_t src_h);
// Dev/test: keep a copy of every presented image (HP2 present checks).
void KeepPresentCopies(bool on);
TexId LastPresentCopy(uint32_t* w, uint32_t* h);

// Milliseconds the CPU waited for a free in-flight frame since the last call.
double TakeGpuWaitMs();

bool Init();
const char* DeviceName();

}  // namespace hp2d3d::gpu
