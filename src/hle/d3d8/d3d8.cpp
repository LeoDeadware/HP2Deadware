// D3D8 HLE: the D3D section functions the game (and the XMV player) call,
// replaced by host functions (see FUNCTIONS.md for addresses, names and
// evidence). API-level: the NV2A push buffer is never produced.
//
// State model. The game writes some Direct3D state straight into the lib's
// globals (inlined XDK header code): D3D__RenderState (0x1405F0),
// D3D__TextureState (0x1403F0) and D3D__DirtyFlags. Those arrays stay in
// guest memory as the single source of truth for render and texture stage
// states: the replaced setters write them exactly like the lib, and every
// draw reads them. Everything else (shaders, constants, streams, textures,
// targets) lives here.
//
// Resources keep their Xbox headers in guest memory (Common/Data/Lock/
// Format/Size): the game reads and aliases them. Data is a physical address
// of kernel contiguous memory (CPU address = Data | 0x80000000). Render
// targets exist on the GPU, keyed by Data: a texture whose Data is a render
// target's is sampled from the GPU copy. Other textures are converted from
// guest memory and cached by (Data, Format, Size); a content hash per frame
// of use catches CPU rewrites, and feeds the replacement-pack hook.
//
// Sizes are logical (Xbox) here; gpu:: gets physical ones (x render scale).

#include "hle/d3d8/d3d8.h"

#include <algorithm>
#include <array>
#include <memory>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <thread>

#include <dispatch/dispatch.h>
#include <unordered_map>
#include <vector>

#include "hle/d3d8/gpu.h"
#include "hle/d3d8/shaders.h"
#include "hle/d3d8/textures.h"
#include "hle/d3d8/xbox_d3d.h"
#include "hle/kernel/kernel_exports.h"
#include "runtime/hp2_runtime.h"

using namespace hp2d3d;

extern "C" int hp2_window_auto_scale();  // window.mm

namespace {

// ---- configuration --------------------------------------------------------------
struct Config {
  uint32_t scale = 1;
  std::string shot_dir;
  uint32_t shot_every = 0;
  std::set<uint64_t> shot_frames;
  uint64_t exit_after = 0;
  bool pacing = true;
  std::string tex_dump;
  bool trace = false;
  bool shader_check = false;
  bool wbuffer = true;    // HP2_WBUFFER=0: D3DZB_USEW draws use plain z/w depth (A/B checks)
  bool wide_hud = true;   // HP2_WIDE_HUD: corner HUD elements anchored to the wide edges
  uint32_t aniso = 1;     // HP2_ANISO: anisotropic filtering of mipmapped textures (windowed: 8)
  uint32_t tex_recheck = 30;
  float pixel_offset = 0.5f;
  float wide = 1.0f;  // hp2_d3d8_set_widescreen: screen-sized targets this much wider
};
Config g_cfg;

void LoadConfig() {
  auto env = [](const char* n) -> const char* {
    const char* v = getenv(n);
    return v && *v ? v : nullptr;
  };
  // Windowed play renders at 2x by default (GPU cost at 2x: see handoff);
  // silent test runs (HP2_BACKGROUND=1) keep the Xbox's 1x for the goldens.
  const char* bg = env("HP2_BACKGROUND");
  bool windowed = !(bg && *bg != '0');
  // Windowed: render at the window's pixel density (e.g. 3x for a 900-point
  // window on a 2x panel: 1440 rows for 1800), at most 4x.
  if (windowed) g_cfg.scale = uint32_t(std::clamp(hp2_window_auto_scale(), 1, 4));
  if (windowed) g_cfg.aniso = 8;
  if (const char* v = env("HP2_RENDER_SCALE")) g_cfg.scale = std::clamp(atoi(v), 1, 8);
  if (const char* v = env("HP2_SHOT_DIR"))
    g_cfg.shot_dir = v;
  else if (const char* v = env("HP2_LOG_DIR"))
    g_cfg.shot_dir = v;
  if (const char* v = env("HP2_SHOT_EVERY")) g_cfg.shot_every = uint32_t(atoi(v));
  if (const char* v = env("HP2_SHOT_FRAMES")) {
    std::string s = v;
    size_t p = 0;
    while (p < s.size()) {
      size_t q = s.find(',', p);
      if (q == std::string::npos) q = s.size();
      g_cfg.shot_frames.insert(strtoull(s.substr(p, q - p).c_str(), nullptr, 10));
      p = q + 1;
    }
  }
  if (const char* v = env("HP2_EXIT_AFTER")) g_cfg.exit_after = strtoull(v, nullptr, 10);
  if (const char* v = env("HP2_NO_PACING")) g_cfg.pacing = atoi(v) == 0;
  if (const char* v = env("HP2_TEX_DUMP")) g_cfg.tex_dump = v;
  if (const char* v = env("HP2_D3D_TRACE")) g_cfg.trace = atoi(v) != 0;
  // Widescreen: Polish sets it (hp2_d3d8_set_widescreen); HP2_WIDESCREEN=1
  // alone gives 16:10 for tests of this library without Polish.
  if (const char* v = env("HP2_WIDESCREEN"); v && atoi(v) && g_cfg.wide <= 1.0f)
    g_cfg.wide = (16.0f / 10.0f) / (4.0f / 3.0f);
  if (const char* v = env("HP2_ANISO")) g_cfg.aniso = uint32_t(std::clamp(atoi(v), 1, 16));
  if (const char* v = env("HP2_WBUFFER")) g_cfg.wbuffer = atoi(v) != 0;
  if (const char* v = env("HP2_WIDE_HUD")) g_cfg.wide_hud = atoi(v) != 0;
  if (const char* v = env("HP2_TEX_RECHECK")) g_cfg.tex_recheck = std::max(1, atoi(v));
  if (const char* v = env("HP2_SHADER_CHECK")) g_cfg.shader_check = atoi(v) != 0;
  if (const char* v = env("HP2_PIXEL_OFFSET")) g_cfg.pixel_offset = float(atof(v));
}

#define TRACE(...) \
  do {             \
    if (g_cfg.trace) hp2_log(__VA_ARGS__); \
  } while (0)

// First call of each entry point is logged once.
void Seen(const char* name) {
  static std::set<const char*> seen;
  if (seen.insert(name).second) hp2_log("d3d8: first call %s", name);
}

float F(uint32_t v) {
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
uint32_t U(float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  return v;
}

// ---- guest memory helpers --------------------------------------------------------
uint32_t RS(uint32_t i) { return hp2_rd32(kRenderStateVa + 4 * i); }
void SetRS(uint32_t i, uint32_t v) { hp2_wr32(kRenderStateVa + 4 * i, v); }
uint32_t TS(uint32_t stage, uint32_t i) {
  return hp2_rd32(kTextureStateVa + stage * kTextureStageStride + 4 * i);
}
void SetTS(uint32_t stage, uint32_t i, uint32_t v) {
  hp2_wr32(kTextureStateVa + stage * kTextureStageStride + 4 * i, v);
}

uint32_t CpuAddr(uint32_t data) { return data < 0x80000000u ? (data | 0x80000000u) : data; }

constexpr uint32_t kOrdMmAllocateContiguousMemoryEx = 166;
constexpr uint32_t kOrdMmFreeContiguousMemory = 171;

uint32_t AllocContiguous(Hp2Ctx& c, uint32_t size, uint32_t align) {
  uint32_t args[5] = {size, 0, 0x03FFFFFF, align, 0x404};
  uint32_t va = hp2_call_guest(c, hp2_kernel_thunk_va(kOrdMmAllocateContiguousMemoryEx), args, 5);
  if (!va) hp2_log("d3d8: MmAllocateContiguousMemoryEx(%u) failed", size);
  return va;
}
void FreeContiguous(Hp2Ctx& c, uint32_t va) {
  uint32_t args[1] = {va};
  hp2_call_guest(c, hp2_kernel_thunk_va(kOrdMmFreeContiguousMemory), args, 1);
}

// Resource header helpers.
struct Hdr {
  uint32_t va;
  uint32_t common() const { return hp2_rd32(va); }
  uint32_t data() const { return hp2_rd32(va + 4); }
  uint32_t format() const { return hp2_rd32(va + 0xC); }
  uint32_t size() const { return hp2_rd32(va + 0x10); }
  uint32_t type() const { return common() & COMMON_TYPE_MASK; }
};

// ---- render targets ----------------------------------------------------------------
struct Target {
  uint32_t data = 0;
  uint32_t w = 0, h = 0;  // logical
  uint32_t pw = 0, ph = 0;  // physical (render scale; screen-sized ones x g_cfg.wide)
  bool depth = false;
  gpu::TexId tex = 0;     // physical size
  gpu::TexId opaque = 0;  // view of tex with alpha = 1 (made on first use)
};
std::unordered_map<uint64_t, Target> g_targets;  // key: data << 1 | depth

Target* FindTarget(uint32_t data, bool depth) {
  auto it = g_targets.find(uint64_t(data) << 1 | depth);
  return it == g_targets.end() ? nullptr : &it->second;
}

Target* GetTarget(uint32_t data, uint32_t w, uint32_t h, bool depth) {
  if (Target* t = FindTarget(data, depth)) {
    if (t->w == w && t->h == h) return t;
    gpu::DestroyTexture(t->opaque);
    gpu::DestroyTexture(t->tex);
    g_targets.erase(uint64_t(data) << 1 | depth);
  }
  Target t;
  t.data = data;
  t.w = w;
  t.h = h;
  t.depth = depth;
  // Widescreen: the 640x480 screen (back/front buffer, depth, the game's
  // full-screen copies) is rendered wider; the logical size stays 640x480.
  bool wide = g_cfg.wide > 1.0f && w == 640 && h == 480;
  t.pw = (wide ? uint32_t(w * g_cfg.wide + 0.5f) : w) * g_cfg.scale;
  t.ph = h * g_cfg.scale;
  t.tex = gpu::CreateTexture(depth ? gpu::TexKind::Depth : gpu::TexKind::BGRA8, t.pw, t.ph, 1,
                             false, true);
  hp2_log("d3d8: render target %s %ux%u data %08X -> tex %u", depth ? "depth" : "colour", w, h,
          data, t.tex);
  return &(g_targets[uint64_t(data) << 1 | depth] = t);
}

// Logical size of a surface/texture header.
bool SurfaceSize(uint32_t hdr, uint32_t* w, uint32_t* h, uint32_t* xfmt) {
  Hdr H{hdr};
  TexLayout L;
  if (!DescribeTexture(H.format(), H.size(), &L)) return false;
  *w = L.w, *h = L.h, *xfmt = L.xfmt;
  return true;
}

// ---- textures --------------------------------------------------------------------------
struct CachedTex {
  gpu::TexId tex = 0;
  uint64_t hash = 0;
  uint64_t frame = ~0ull;    // last frame it was bound
  uint64_t checked = 0;      // frame its content was last hashed
  TexLayout L;
};
std::unordered_map<uint64_t, CachedTex> g_texcache;
// Frame of the last CPU lock (LockRect) per texture Data: a locked texture
// is re-hashed at its next use. Others are re-hashed every
// g_cfg.tex_recheck frames (staggered), which catches CPU writes without a
// lock; hashing every bound texture every frame cost ~1-2 ms of CPU.
std::unordered_map<uint32_t, uint64_t> g_tex_locked;
Hp2TextureHook g_tex_hook = nullptr;

// ---- vertex shaders -----------------------------------------------------------------
// Background compile of one vertex program (started at CreateVertexShader:
// all 264 are created at boot, long before their first draw).
struct VsJob {
  std::string msl;
  char tag[32];
  gpu::ProgId prog = 0;
  dispatch_semaphore_t done = dispatch_semaphore_create(0);
};

struct VShader {
  std::vector<uint32_t> func;      // microcode (instructions only)
  uint32_t attr[16][3] = {};       // stream, offset, type per v register (stream 0xFF: unused)
  std::vector<std::pair<uint32_t, std::vector<uint32_t>>> constants;  // declaration consts
  gpu::ProgId prog = 0;
  bool failed = false;
  bool screen_space = false;       // oPos = v0 (2D: positions in screen pixels)
  int8_t tc_reg[4] = {-1, -1, -1, -1};  // 2D: oT<s> = v<n> (n), else -1
  uint32_t index;
  std::shared_ptr<VsJob> job;      // pending background compile
};
std::vector<VShader> g_vshaders;
constexpr uint32_t kVsMagic = 0x56534831;  // 'VSH1' in the guest handle object

// ---- pixel shaders (fragment programs by key) ---------------------------------------
std::unordered_map<uint64_t, std::vector<std::pair<PsKey, gpu::ProgId>>> g_fprogs;

// ---- device -----------------------------------------------------------------------------
struct Stream {
  uint32_t vb = 0;  // header VA
  uint32_t stride = 0;
};
struct Device {
  bool created = false;
  uint32_t bb_w = 640, bb_h = 480;
  uint32_t back = 0, front = 0, zbuf = 0;  // surface header VAs
  uint32_t rt = 0, z = 0;                  // current targets (header VAs)
  float vs_c[192][4] = {};
  uint32_t const_mode = 0;
  uint32_t vs_handle = 0;
  Stream streams[16];
  uint32_t idx_base = 0;  // SetIndices BaseVertexIndex
  uint32_t idx_data = 0;  // SetIndices pIndexData (unused by draws that pass indices)
  uint32_t tex[4] = {};
  uint32_t ps = 0;        // pixel shader handle (0: fixed function)
  float transforms[10][16] = {};
  uint32_t material[17] = {};
  uint64_t frame = 0;
  uint64_t overlay_frames = 0, overlay_at_swap = 0;
  uint64_t last_vblank = 0;
  std::chrono::steady_clock::time_point last_swap;
  double frame_ms_sum = 0, frame_ms_max = 0, frame_busy_sum = 0;
  uint32_t frame_ms_n = 0;
  uint32_t draws = 0, frame_draws = 0;
  std::vector<std::pair<uint32_t, uint64_t>> pending_free;  // (contiguous VA, free at frame)
};
Device g_dev;

// Movie timing statistics (logged per movie): time inside UpdateOverlay and
// in BlockUntilVerticalBlank, and the gap between overlay frames.
struct OverlayStats {
  std::chrono::steady_clock::time_point last, movie_start;
  uint32_t movie_frames = 0;
  double in_ms = 0, wait_ms = 0, gap_ms = 0, gap_max = 0;
  uint32_t n = 0, late = 0;
} g_ov;

// ---- surfaces created by the HLE -----------------------------------------------------
uint32_t NewHeader(uint32_t bytes) { return hp2_hle_alloc(bytes, 16); }

uint32_t MakeSurface(Hp2Ctx& c, uint32_t w, uint32_t h, uint32_t xfmt) {
  uint32_t fmt, size;
  MakeTextureHeader(w, h, 1, 1, xfmt, false, false, &fmt, &size);
  if (!size) {  // swizzled/depth surfaces still describe their size linearly
    uint32_t pitch = w * FormatInfo(xfmt).bpp / 8;
    size = (w - 1) | (h - 1) << 12 | ((pitch + 63) / 64 - 1) << 24;
    fmt = (fmt & ~0xFFF00000u & ~0xF0000u) | 1u << 16;
  }
  uint32_t bytes = SizePitch(size) * h;
  uint32_t data = AllocContiguous(c, bytes, 0x1000);
  uint32_t hdr = NewHeader(0x20);
  hp2_wr32(hdr + 0, COMMON_TYPE_SURFACE | COMMON_D3DCREATED | 1);
  hp2_wr32(hdr + 4, data & 0x0FFFFFFF);
  hp2_wr32(hdr + 8, 0);
  hp2_wr32(hdr + 0xC, fmt);
  hp2_wr32(hdr + 0x10, size);
  hp2_wr32(hdr + 0x14, 0);
  return hdr;
}

void AddRef(uint32_t res) {
  if (res) hp2_wr32(res, hp2_rd32(res) + 1);
}

void InitDefaultStates() {
  for (uint32_t i = RS_ZFUNC; i < RS_COUNT; i++) SetRS(i, 0);
  SetRS(RS_ZFUNC, 0x203);
  SetRS(RS_ALPHAFUNC, 0x207);
  SetRS(RS_SRCBLEND, 1);
  SetRS(RS_DESTBLEND, 0);
  SetRS(RS_ZWRITEENABLE, 1);
  SetRS(RS_SHADEMODE, 0x1D01);
  SetRS(RS_COLORWRITEENABLE, 0x01010101);
  SetRS(RS_STENCILZFAIL, STENCILOP_KEEP);
  SetRS(RS_STENCILPASS, STENCILOP_KEEP);
  SetRS(RS_STENCILFUNC, 0x207);
  SetRS(RS_STENCILMASK, 0xFF);
  SetRS(RS_STENCILWRITEMASK, 0xFF);
  SetRS(RS_BLENDOP, BLENDOP_ADD);
  SetRS(RS_FOGEND, U(1.0f));
  SetRS(RS_FOGDENSITY, U(1.0f));
  SetRS(RS_LIGHTING, 1);
  SetRS(RS_COLORVERTEX, 1);
  SetRS(RS_DIFFUSEMATERIALSOURCE, 1);
  SetRS(RS_SPECULARMATERIALSOURCE, 2);
  SetRS(RS_POINTSIZE, U(1.0f));
  SetRS(RS_POINTSIZE_MAX, U(64.0f));
  SetRS(RS_POINTSCALEENABLE, 0);
  SetRS(RS_FILLMODE, FILL_SOLID);
  SetRS(RS_BACKFILLMODE, FILL_SOLID);
  SetRS(RS_ZENABLE, 1);
  SetRS(RS_STENCILFAIL, STENCILOP_KEEP);
  SetRS(RS_FRONTFACE, CULL_CW);
  SetRS(RS_CULLMODE, CULL_CCW);
  SetRS(RS_TEXTUREFACTOR, 0xFFFFFFFF);
  SetRS(RS_MULTISAMPLEANTIALIAS, 1);
  SetRS(RS_MULTISAMPLEMASK, 0xFFFFFFFF);
  SetRS(RS_LINEWIDTH, U(1.0f));
  SetRS(RS_PRESENTATIONINTERVAL, 1);
  for (uint32_t s = 0; s < 4; s++) {
    for (uint32_t i = 0; i < TSS_COUNT; i++) SetTS(s, i, 0);
    SetTS(s, TSS_ADDRESSU, 1);
    SetTS(s, TSS_ADDRESSV, 1);
    SetTS(s, TSS_ADDRESSW, 1);
    SetTS(s, TSS_MAGFILTER, 1);
    SetTS(s, TSS_MINFILTER, 1);
    SetTS(s, TSS_MIPFILTER, 0);
    SetTS(s, TSS_MAXANISOTROPY, 1);
    SetTS(s, TSS_COLOROP, s == 0 ? TOP_MODULATE : TOP_DISABLE);
    SetTS(s, TSS_COLORARG1, TA_TEXTURE);
    SetTS(s, TSS_COLORARG2, TA_CURRENT);
    SetTS(s, TSS_ALPHAOP, s == 0 ? TOP_SELECTARG1 : TOP_DISABLE);
    SetTS(s, TSS_ALPHAARG1, TA_TEXTURE);
    SetTS(s, TSS_ALPHAARG2, TA_CURRENT);
    SetTS(s, TSS_RESULTARG, TA_CURRENT);
    SetTS(s, TSS_TEXCOORDINDEX, s);
  }
}

// ---- texture binding -------------------------------------------------------------------
struct Bound {
  gpu::TexId tex = 0;
  uint8_t kind = 0;  // 0 none, 1 2D, 2 cube
  float sx = 1, sy = 1;  // texture coordinate scale (LIN_ formats: 1/size)
  uint32_t levels = 1;
};

void DumpTexture(uint64_t hash, const TexLayout& L, const std::vector<uint8_t>& bgra) {
  std::vector<uint8_t> rgba(bgra.size());
  for (size_t i = 0; i + 3 < bgra.size(); i += 4) {
    rgba[i] = bgra[i + 2];
    rgba[i + 1] = bgra[i + 1];
    rgba[i + 2] = bgra[i];
    rgba[i + 3] = bgra[i + 3];
  }
  char path[512];
  std::snprintf(path, sizeof path, "%s/%016llx.png", g_cfg.tex_dump.c_str(),
                (unsigned long long)hash);
  gpu::WritePng(path, L.w, L.h, rgba.data());
}

Bound BindTexture(uint32_t hdr) {
  Bound b;
  if (!hdr) return b;
  Hdr H{hdr};
  uint32_t data = H.data(), fmt = H.format(), size = H.size();
  TexLayout L;
  if (!DescribeTexture(fmt, size, &L)) {
    static std::set<uint32_t> warned;
    if (warned.insert(FmtFormat(fmt)).second)
      hp2_log("d3d8: texture %08X: unknown format %02X", hdr, FmtFormat(fmt));
    return b;
  }
  if (L.linear) b.sx = 1.0f / L.w, b.sy = 1.0f / L.h;
  b.kind = L.cube ? 2 : 1;
  if (Target* t = FindTarget(data, false)) {
    // The target's storage is BGRA8; an Xbox format without alpha reads 1.
    bool has_alpha = true;
    switch (L.xfmt) {
      case XF_R5G6B5: case XF_LIN_R5G6B5: case XF_X8R8G8B8: case XF_LIN_X8R8G8B8:
      case XF_X1R5G5B5: case XF_LIN_X1R5G5B5:
        has_alpha = false;
    }
    if (!has_alpha && !t->opaque) t->opaque = gpu::OpaqueView(t->tex);
    b.tex = has_alpha ? t->tex : t->opaque;
    b.kind = 1;
    return b;
  }
  if (FormatInfo(L.xfmt).depth) {
    if (Target* t = FindTarget(data, true)) {
      b.tex = 0;  // depth textures are not sampled yet
      static bool once = false;
      if (!once) hp2_log("d3d8: depth texture %08X sampled (shadow maps not supported)", hdr), once = true;
      (void)t;
    }
    return b;
  }
  uint64_t key = (uint64_t(data) << 32) ^ (uint64_t(fmt) * 0x9E3779B1u) ^ size;
  CachedTex& ct = g_texcache[key];
  if (ct.tex && ct.frame == g_dev.frame) {
    b.tex = ct.tex;
    b.levels = ct.L.levels;
    return b;
  }
  if (ct.tex) {
    ct.frame = g_dev.frame;
    auto lk = g_tex_locked.find(data);
    bool locked = lk != g_tex_locked.end() && lk->second >= ct.checked;
    bool due = (g_dev.frame + (key >> 7)) % g_cfg.tex_recheck == 0;
    if (!locked && !due) {
      b.tex = ct.tex;
      b.levels = ct.L.levels;
      return b;
    }
  }
  uint32_t cpu = CpuAddr(data);
  if (!hp2_is_committed(cpu) || !hp2_is_committed(cpu + L.total - 1)) {
    static std::set<uint32_t> warned;
    if (warned.insert(data).second)
      hp2_log("d3d8: texture %08X data %08X (%u bytes) not in guest memory", hdr, data, L.total);
    return Bound{};
  }
  const uint8_t* src = static_cast<const uint8_t*>(hp2_ptr(cpu));
  uint64_t hash = HashBytes(src, L.total) ^ (uint64_t(fmt) << 1) ^ (uint64_t(size) << 33);
  ct.frame = g_dev.frame;
  ct.checked = g_dev.frame;
  if (ct.tex && ct.hash == hash) {
    b.tex = ct.tex;
    b.levels = ct.L.levels;
    return b;
  }
  if (ct.tex) gpu::DestroyTexture(ct.tex);
  ct.hash = hash;
  ct.L = L;
  uint32_t levels = L.levels;
  // Hook (replacement packs): whole-texture replacement by content hash.
  if (g_tex_hook) {
    Hp2TextureInfo info{hash, L.w, L.h, L.xfmt, L.levels, src, L.total};
    uint32_t w2 = 0, h2 = 0;
    const uint8_t* rgba = nullptr;
    if (g_tex_hook(info, &w2, &h2, &rgba) && rgba && w2 && h2) {
      std::vector<uint8_t> bgra(size_t(w2) * h2 * 4);
      for (size_t i = 0; i < size_t(w2) * h2; i++) {
        bgra[4 * i] = rgba[4 * i + 2];
        bgra[4 * i + 1] = rgba[4 * i + 1];
        bgra[4 * i + 2] = rgba[4 * i];
        bgra[4 * i + 3] = rgba[4 * i + 3];
      }
      // A mipmapped original gets a full mip chain of the replacement (built on
      // the GPU): world textures would shimmer at a distance without one.
      uint32_t lv2 = 1;
      if (levels > 1)
        while ((std::max(w2, h2) >> lv2) > 0) lv2++;
      ct.tex = gpu::CreateTexture(gpu::TexKind::BGRA8, w2, h2, lv2, false, false);
      gpu::UploadTexture(ct.tex, 0, 0, w2, h2, bgra.data(), w2 * 4);
      if (lv2 > 1) gpu::GenerateMips(ct.tex);
      ct.L.levels = lv2;
      b.tex = ct.tex;
      b.levels = lv2;
      return b;
    }
  }
  gpu::TexKind kind = HostKind(L.xfmt);
  ct.tex = gpu::CreateTexture(kind, L.w, L.h, levels, L.cube, false);
  std::vector<uint8_t> buf;
  uint32_t pitch;
  for (uint32_t face = 0; face < (L.cube ? 6u : 1u); face++)
    for (uint32_t lv = 0; lv < levels; lv++) {
      ConvertLevel(L, lv, src + face * L.face_size + L.level_offset[lv], &buf, &pitch);
      gpu::UploadTexture(ct.tex, face, lv, std::max(1u, L.w >> lv), std::max(1u, L.h >> lv),
                         buf.data(), pitch);
      if (lv == 0 && face == 0 && !g_cfg.tex_dump.empty() && kind == gpu::TexKind::BGRA8)
        DumpTexture(hash, L, buf);
    }
  TRACE("d3d8: texture %08X %s %ux%u lv %u -> %u (hash %016llx)", hdr, FormatInfo(L.xfmt).name,
        L.w, L.h, levels, ct.tex, (unsigned long long)hash);
  b.tex = ct.tex;
  b.levels = levels;
  return b;
}

// ---- state -> gpu ------------------------------------------------------------------------
uint8_t MtlBlend(uint32_t v) {
  switch (v) {
    case 0: return 0;
    case 1: return 1;
    case 0x300: return 2;
    case 0x301: return 3;
    case 0x302: return 4;
    case 0x303: return 5;
    case 0x304: return 8;
    case 0x305: return 9;
    case 0x306: return 6;
    case 0x307: return 7;
    case 0x308: return 10;
    case 0x8001: return 11;
    case 0x8002: return 12;
    case 0x8003: return 13;
    case 0x8004: return 14;
  }
  return 1;
}
uint8_t MtlBlendOp(uint32_t v) {
  switch (v) {
    case BLENDOP_SUBTRACT: return 1;
    case BLENDOP_REVSUBTRACT: case 0xF006: return 2;
    case BLENDOP_MIN: return 3;
    case BLENDOP_MAX: return 4;
  }
  return 0;
}
uint8_t MtlCmp(uint32_t v) { return v >= 0x200 && v <= 0x207 ? uint8_t(v - 0x200) : 7; }
uint8_t MtlStencilOp(uint32_t v) {
  switch (v) {
    case STENCILOP_KEEP: return 0;
    case STENCILOP_ZERO: return 1;
    case STENCILOP_REPLACE: return 2;
    case STENCILOP_INCRSAT: return 3;
    case STENCILOP_DECRSAT: return 4;
    case STENCILOP_INVERT: return 5;
    case STENCILOP_INCR: return 6;
    case STENCILOP_DECR: return 7;
  }
  return 0;
}
uint8_t MtlAddr(uint32_t v) {
  switch (v) {
    case 1: return 2;  // wrap -> repeat
    case 2: return 3;  // mirror
    case 3: return 0;  // clamp
    case 4: return 5;  // border
    case 5: return 0;  // clamp to edge
  }
  return 2;
}

void ColorF(uint32_t argb, float* out) {
  out[0] = ((argb >> 16) & 0xFF) / 255.f;
  out[1] = ((argb >> 8) & 0xFF) / 255.f;
  out[2] = (argb & 0xFF) / 255.f;
  out[3] = (argb >> 24) / 255.f;
}

gpu::ProgId FragmentProgram(const PsKey& k) {
  uint64_t h = k.Hash();
  auto& bucket = g_fprogs[h];
  for (auto& [key, prog] : bucket)
    if (key == k) return prog;
  std::string msl = GeneratePsh(k);
  char tag[64];
  std::snprintf(tag, sizeof tag, "ps_%016llx", (unsigned long long)h);
  if (const char* dir = getenv("HP2_DUMP_MSL")) {
    std::string path = std::string(dir) + "/" + tag + ".metal";
    if (FILE* f = fopen(path.c_str(), "w")) fwrite(msl.data(), 1, msl.size(), f), fclose(f);
  }
  gpu::ProgId p = gpu::CompileFragment(msl, tag);
  hp2_log("d3d8: fragment program %s (%s) -> %u", tag, k.combiners ? "combiners" : "fixed", p);
  bucket.push_back({k, p});
  return p;
}

// Translates vs to MSL (false: malformed program, logged).
bool TranslateVs(VShader& vs, std::string* msl, char (&tag)[32]) {
  uint32_t n = 0;
  std::snprintf(tag, sizeof tag, "vs_%u", vs.index);
  if (!TranslateVsh(vs.func.data(), uint32_t(vs.func.size() / 4), msl, &n)) return false;
  // A pass-through program (oPos = v0, nothing else writes it) draws 2D in
  // screen pixels: widescreen HUD anchoring reads its positions.
  size_t at = msl->find("float4 mac = (V[0]);\n    R[12].xyzw = mac.xyzw;");
  size_t writes = 0;
  for (size_t q = msl->find("R[12]."); q != std::string::npos; q = msl->find("R[12].", q + 1)) writes++;
  vs.screen_space = at != std::string::npos && writes == 1;
  // Its texture coordinates, when copied straight from an input (oT<s> =
  // O[9 + s]): the clamp below reads their range.
  for (int t = 0; t < 4 && vs.screen_space; t++) {
    char out[40];
    std::snprintf(out, sizeof out, ");\n    O[%d].xyzw = mac.xyzw;", 9 + t);
    size_t q = msl->find(out);
    size_t v = q == std::string::npos ? q : msl->rfind("float4 mac = (V[", q);
    if (v != std::string::npos && v + 20 >= q) vs.tc_reg[t] = int8_t(atoi(msl->c_str() + v + 16));
  }
  if (const char* dir = getenv("HP2_DUMP_MSL")) {
    std::string path = std::string(dir) + "/" + tag + ".metal";
    if (FILE* f = fopen(path.c_str(), "w")) fwrite(msl->data(), 1, msl->size(), f), fclose(f);
  }
  return true;
}

// Starts compiling vs on one low-priority background queue (one core: the
// boot screens keep their 60 Hz).
void PrecompileVertexProgram(VShader& vs) {
  auto job = std::make_shared<VsJob>();
  if (!TranslateVs(vs, &job->msl, job->tag)) {
    vs.failed = true;
    return;
  }
  vs.job = job;
  static dispatch_queue_t q = dispatch_queue_create(
      "hp2.vs-compile",
      dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
  auto* hold = new std::shared_ptr<VsJob>(job);
  dispatch_async_f(q, hold, [](void* p) {
    auto* h = static_cast<std::shared_ptr<VsJob>*>(p);
    (*h)->prog = gpu::CompileVertex((*h)->msl, (*h)->tag);
    (*h)->msl.clear();
    dispatch_semaphore_signal((*h)->done);
    delete h;
  });
}

gpu::ProgId VertexProgram(VShader& vs) {
  if (vs.prog || vs.failed) return vs.prog;
  if (vs.job) {  // precompiled in the background: wait for it if still running
    dispatch_semaphore_wait(vs.job->done, DISPATCH_TIME_FOREVER);
    vs.prog = vs.job->prog;
    vs.job.reset();
  } else {
    std::string msl;
    char tag[32];
    if (TranslateVs(vs, &msl, tag)) vs.prog = gpu::CompileVertex(msl, tag);
  }
  if (!vs.prog) vs.failed = true;
  return vs.prog;
}

// Current colour/depth targets.
struct TargetsNow {
  Target* color = nullptr;
  Target* depth = nullptr;
  uint32_t w = 0, h = 0;
  uint32_t pw = 0, ph = 0;  // physical
};

TargetsNow BindTargets() {
  TargetsNow t;
  uint32_t w = 0, h = 0, f = 0;
  if (g_dev.rt && SurfaceSize(g_dev.rt, &w, &h, &f)) {
    t.color = GetTarget(Hdr{g_dev.rt}.data(), w, h, false);
    t.w = w, t.h = h, t.pw = t.color->pw, t.ph = t.color->ph;
  }
  if (g_dev.z && SurfaceSize(g_dev.z, &w, &h, &f)) {
    t.depth = GetTarget(Hdr{g_dev.z}.data(), w, h, true);
    if (!t.color) t.w = w, t.h = h, t.pw = t.depth->pw, t.ph = t.depth->ph;
  }
  gpu::SetTargets(t.color ? t.color->tex : 0, t.depth ? t.depth->tex : 0, t.pw, t.ph);
  return t;
}

// ---- drawing ------------------------------------------------------------------------------

// Xbox primitive -> Metal primitive + index list (in vertex numbers 0..n-1)
// for types Metal lacks. Returns false if nothing to draw.
bool Topology(uint32_t prim, uint32_t n, uint32_t* mtl, std::vector<uint16_t>* idx,
              bool* need_idx) {
  *need_idx = false;
  idx->clear();
  switch (prim) {
    case PT_POINTLIST: *mtl = 0; return n > 0;
    case PT_LINELIST: *mtl = 1; return n >= 2;
    case PT_LINESTRIP: *mtl = 2; return n >= 2;
    case PT_TRIANGLELIST: *mtl = 3; return n >= 3;
    case PT_TRIANGLESTRIP: *mtl = 4; return n >= 3;
    case PT_QUADSTRIP: *mtl = 4; return n >= 4;
    case PT_LINELOOP:
      *mtl = 2;
      *need_idx = true;
      for (uint32_t i = 0; i < n; i++) idx->push_back(uint16_t(i));
      idx->push_back(0);
      return n >= 2;
    case PT_TRIANGLEFAN:
    case PT_POLYGON:
      *mtl = 3;
      *need_idx = true;
      for (uint32_t i = 1; i + 1 < n; i++) {
        idx->push_back(0);
        idx->push_back(uint16_t(i));
        idx->push_back(uint16_t(i + 1));
      }
      return n >= 3;
    case PT_QUADLIST:
      *mtl = 3;
      *need_idx = true;
      for (uint32_t q = 0; q + 3 < n; q += 4) {
        uint16_t b = uint16_t(q);
        idx->insert(idx->end(), {b, uint16_t(b + 1), uint16_t(b + 2), b, uint16_t(b + 2),
                                 uint16_t(b + 3)});
      }
      return n >= 4;
  }
  return false;
}

struct StreamSrc {
  uint32_t va = 0;      // guest VA of vertex 0
  uint32_t stride = 0;
};

// Widescreen HUD anchoring: the NDC x offset for a 2D (screen-space) draw.
// HUD elements in a top/bottom corner region of the 640x480 screen move with
// their side's edge of the wide screen; everything else stays centred (menus,
// dialogs, subtitles, mid-height elements). Full-screen quads are stretched by
// the shader instead. z = 1/wide.
//
// An element is drawn in pieces (the health bar: icon, bar, fill, end cap;
// the enemy bar: cap, bar, fill, portrait; the button cluster: ring, buttons,
// label), so the decision is per element, not per draw: the sprites of the
// previous frame that touch (12-pixel margin) form one element, and the
// element's box decides its edge. A piece inherits the decision of the element
// its centre lies in; a piece new this frame decides by its own box.
// Text (a font page: a texture used by many 2D draws; the game draws one glyph
// per draw) follows an element only when it lies inside it or, for a top
// element, under it (the action label: "Open", "Talk / To", "Go!"); text inside
// a panel (an untextured or large 2D draw: dialogs, tutorial boxes) never moves.
struct HudBox {
  float x0, y0, x1, y1;
  bool Contains(float x, float y, float mx, float up, float down) const {
    return x >= x0 - mx && x <= x1 + mx && y >= y0 - up && y <= y1 + down;
  }
};
struct HudElement {
  HudBox box;
  float shift;
  bool text;  // a string (glyphs, maybe with its icon): its glyphs follow it, not other rules
};
struct HudPiece {
  HudBox box;
  bool text;
  bool icon = false;  // a sprite that joins a line of text as its icon
};
std::unordered_map<uint32_t, uint32_t> g_hud_tex_prev, g_hud_tex_now;
std::vector<HudPiece> g_hud_pieces;  // this frame's sprites and glyphs (not panels)
std::vector<HudBox> g_hud_panels, g_hud_panels_prev;
std::vector<HudElement> g_hud_elements;  // from the previous frame's sprites
uint64_t g_hud_frame = 0;
// The frame opens with a full-screen 2D backdrop (the front end's cloudy sky:
// title, menus, inventory scroll, card album, spellbook): a 4:3 composition
// around a 3D scroll or book whose pages hold the 2D text and buttons. Nothing
// in it is anchored; edge anchoring tore page text off the book.
bool g_hud_menu_scene = false;

// The edge an element (or a lone piece) belongs to: -(1 - z) left, 1 - z
// right, 0 centred.
float HudEdge(const HudBox& b, float z) {
  // Corner band by the element's centre (the Remembrall's glow reaches up
  // to y 282 while the icon sits in the bottom-left corner).
  float cx = (b.x0 + b.x1) / 2, cy = (b.y0 + b.y1) / 2;
  if (cy > 480 * 0.38f && cy < 480 * 0.62f) return 0;
  if (cx < 240 && b.x0 < 160 && b.x1 <= 400) return -(1 - z);
  if (cx > 400 && b.x1 > 480 && b.x0 >= 240) return 1 - z;
  return 0;
}

// Two pieces belong to one element: sprites that touch (12-pixel margin), or
// - when either is text - pieces on one line (vertical overlap at least half
// the lower one's height) with a gap under 0.6 of the taller one's height: a
// string's glyphs, its decorative initial (a larger glyph, sometimes from
// another texture) and the button icon in front of it ("(A) Select"; an
// icon at most 1.6x the line's height).
bool HudJoin(const HudPiece& a, const HudPiece& b) {
  const HudBox &p = a.box, &q = b.box;
  // Stacked legend icons ("(A) Select" over "(B) Exit", 7 px apart) each
  // belong to their own line, not to one taller sprite element.
  if (!a.text && !b.text && a.icon && b.icon) return false;
  if (!a.text && !b.text)
    return p.x0 <= q.x1 + 12 && q.x0 <= p.x1 + 12 && p.y0 <= q.y1 + 12 && q.y0 <= p.y1 + 12;
  float hp = p.y1 - p.y0, hq = q.y1 - q.y0;
  // A sprite joins a line of text only as an icon of about the glyphs' size:
  // the button cluster (129 px) must not swallow its label ("Dodge").
  if (a.text != b.text && (a.text ? hq > 1.6f * hp : hp > 1.6f * hq)) return false;
  float overlap = std::min(p.y1, q.y1) - std::max(p.y0, q.y0);
  float gap = std::max(p.x0, q.x0) - std::min(p.x1, q.x1);
  return overlap >= 0.5f * std::min(hp, hq) && gap <= 0.6f * std::max(hp, hq);
}

void HudNewFrame(float z) {
  g_hud_tex_prev.swap(g_hud_tex_now);
  g_hud_tex_now.clear();
  g_hud_panels_prev.swap(g_hud_panels);
  g_hud_panels.clear();
  // Union the previous frame's pieces into elements.
  for (HudPiece& p : g_hud_pieces)
    if (!p.text)
      for (const HudPiece& t : g_hud_pieces)
        if (t.text && HudJoin(p, t)) {
          p.icon = true;
          break;
        }
  std::vector<HudPiece> el;
  for (const HudPiece& b : g_hud_pieces) {
    HudPiece m = b;
    for (bool merged = true; merged;) {
      merged = false;
      for (size_t i = 0; i < el.size(); i++) {
        const HudPiece& e = el[i];
        if (HudJoin(m, e)) {
          m.box = {std::min(m.box.x0, e.box.x0), std::min(m.box.y0, e.box.y0),
                   std::max(m.box.x1, e.box.x1), std::max(m.box.y1, e.box.y1)};
          m.text = m.text || e.text;
          m.icon = m.icon || e.icon;
          el.erase(el.begin() + long(i));
          merged = true;
          break;
        }
      }
    }
    el.push_back(m);
  }
  g_hud_pieces.clear();
  g_hud_elements.clear();
  // A string never picks an edge itself (menu titles, subtitles, the menus'
  // "(A) Select" legends, whose icon is a font glyph on some screens and a
  // sprite on others): it belongs to a centred layout. Only sprite elements
  // (bars, button cluster, icons) are anchored.
  for (const HudPiece& e : el) g_hud_elements.push_back({e.box, e.text ? 0.0f : HudEdge(e.box, z), e.text});
  // A string that lies in, under or (bottom corners) next to an anchored
  // element moves with it, as a whole: the action label "Go!" / "Close" under
  // the button cluster, the bean count by its icon.
  for (HudElement& t : g_hud_elements) {
    if (!t.text || t.shift != 0) continue;
    float cx = (t.box.x0 + t.box.x1) / 2, cy = (t.box.y0 + t.box.y1) / 2;
    for (const HudElement& e : g_hud_elements)
      if (&e != &t && e.shift != 0 &&
          (e.box.y1 <= 480 * 0.40f ? e.box.Contains(cx, cy, 24, 8, 96)
                                   : e.box.Contains(cx, cy, 24, 40, 40))) {
        t.shift = e.shift;
        break;
      }
  }
}

float HudAnchor(const VShader& vs, const StreamSrc* streams, uint32_t first, uint32_t count,
                float z) {
  if (g_hud_frame != g_dev.frame) {
    HudNewFrame(z);
    g_hud_frame = g_dev.frame;
    g_hud_menu_scene = false;
  }
  uint32_t s = vs.attr[0][0], off = vs.attr[0][1], type = vs.attr[0][2];
  if (s >= 16 || !streams[s].va || !count || (type != VSDT_FLOAT3 && type != VSDT_FLOAT4) ||
      count > 4096)
    return 0;
  HudBox b{1e9f, 1e9f, -1e9f, -1e9f};
  for (uint32_t v = first; v < first + count; v++) {
    uint32_t a = streams[s].va + v * streams[s].stride + off;
    if (!hp2_is_committed(a) || !hp2_is_committed(a + 7)) return 0;
    float x = F(hp2_rd32(a)), y = F(hp2_rd32(a + 4));
    b = {std::min(b.x0, x), std::min(b.y0, y), std::max(b.x1, x), std::max(b.y1, y)};
  }
  if (b.x0 <= 2 && b.x1 >= 638) {  // full width: the shader stretches it
    if (g_dev.frame_draws == 1 && b.y0 <= 2 && b.y1 >= 478 && !RS(RS_ZENABLE)) g_hud_menu_scene = true;
    return 0;
  }
  uint32_t tex = g_dev.tex[0] ? Hdr{g_dev.tex[0]}.data() : 0;
  bool panel = !tex || b.x1 - b.x0 > 300 || b.y1 - b.y0 > 200;
  uint32_t uses = tex ? ++g_hud_tex_now[tex] : 0;
  auto prev = tex ? g_hud_tex_prev.find(tex) : g_hud_tex_prev.end();
  bool text = !panel && (uses > 8 || (prev != g_hud_tex_prev.end() && prev->second > 8));
  float cx = (b.x0 + b.x1) / 2, cy = (b.y0 + b.y1) / 2;
  float shift = 0;
  if (panel) {
    g_hud_panels.push_back(b);
  } else if (text) {
    g_hud_pieces.push_back({b, true});
    bool in_panel = false, in_string = false;
    for (const auto* list : {&g_hud_panels, &g_hud_panels_prev})
      for (const HudBox& p : *list) in_panel = in_panel || p.Contains(cx, cy, 0, 0, 0);
    if (!in_panel)
      for (const HudElement& e : g_hud_elements)
        if (e.text && e.box.Contains(cx, cy, 2, 2, 2)) {
          shift = e.shift, in_string = true;
          break;
        }
    if (!in_panel && !in_string)
      for (const HudElement& e : g_hud_elements)
        if (e.shift != 0 && e.box.Contains(cx, cy, 24, 8, e.box.y1 <= 480 * 0.40f ? 96 : 8)) {
          shift = e.shift;
          break;
        }
  } else {
    g_hud_pieces.push_back({b, false});
    bool found = false;
    for (const HudElement& e : g_hud_elements)
      if (e.box.Contains(cx, cy, 12, 12, 12)) {
        shift = e.shift, found = true;
        break;
      }
    if (!found) shift = HudEdge(b, z);
  }
  static const uint64_t dev_hud = [] {
    const char* v = getenv("HP2_DEV_HUD_LOG");
    return v ? strtoull(v, nullptr, 10) : 0ull;
  }();
  // Menu scene: pieces are still recorded (the next frame's elements).
  if (g_hud_menu_scene) shift = 0;
  if (dev_hud && g_dev.frame == dev_hud)
    hp2_log("hud: frame %llu draw %u tex %08X%s%s box %.0f,%.0f-%.0f,%.0f shift %+.3f",
            (unsigned long long)g_dev.frame, g_dev.draws, tex, text ? " text" : "",
            panel ? " panel" : "", b.x0, b.y0, b.x1, b.y1, shift);
  return shift;
}

// A 2D draw whose texture coordinates for stage s stay within the texture
// (0..1 after the LIN_ scale) never repeats it: WRAP and CLAMP differ only in
// the half texel at its edges. Above 1x, pixel centres fall into that half
// texel and WRAP blends in the opposite edge - a dark seam between the title
// screen's side-by-side tiles (sky, logo). Such a draw samples clamped.
void UvRangeInUnit(const VShader& vs, const StreamSrc* streams, uint32_t first, uint32_t count,
                   int stage, float sx, float sy, bool* u_in, bool* v_in) {
  int r = vs.tc_reg[stage];
  if (r < 0 || r >= 16) return;
  uint32_t s = vs.attr[r][0], off = vs.attr[r][1], type = vs.attr[r][2];
  if (s >= 16 || !streams[s].va || !count || count > 4096 ||
      (type != VSDT_FLOAT2 && type != VSDT_FLOAT3 && type != VSDT_FLOAT4))
    return;
  const float lo = -1.0f / 4096, hi = 1 + 1.0f / 4096;
  bool u = true, v = true;
  for (uint32_t i = first; i < first + count && (u || v); i++) {
    uint32_t a = streams[s].va + i * streams[s].stride + off;
    if (!hp2_is_committed(a) || !hp2_is_committed(a + 7)) return;
    float tu = F(hp2_rd32(a)) * sx, tv = F(hp2_rd32(a + 4)) * sy;
    u = u && tu >= lo && tu <= hi;
    v = v && tv >= lo && tv <= hi;
  }
  *u_in = u, *v_in = v;
}

// Four 2D vertices on two x and two y values: a screen-aligned rectangle (a
// background or logo tile, whose edges the 1x pixel snap may move).
bool AxisAlignedQuad(const VShader& vs, const StreamSrc* streams, uint32_t first, uint32_t count) {
  uint32_t s = vs.attr[0][0], off = vs.attr[0][1];
  if (count != 4 || s >= 16 || !streams[s].va) return false;
  float xs[2], ys[2];
  int nx = 0, ny = 0;
  for (uint32_t i = first; i < first + 4; i++) {
    uint32_t a = streams[s].va + i * streams[s].stride + off;
    if (!hp2_is_committed(a) || !hp2_is_committed(a + 7)) return false;
    float x = F(hp2_rd32(a)), y = F(hp2_rd32(a + 4));
    if (!(nx > 0 && x == xs[0]) && !(nx > 1 && x == xs[1])) {
      if (nx == 2) return false;
      xs[nx++] = x;
    }
    if (!(ny > 0 && y == ys[0]) && !(ny > 1 && y == ys[1])) {
      if (ny == 2) return false;
      ys[ny++] = y;
    }
  }
  return nx == 2 && ny == 2;
}

// One draw. `streams` gives the vertex data per Xbox stream; vertices
// first..first+count-1 are read (indexed: the index values, already rebased
// by the caller to start at 0 = vertex `first`).
void DrawCommon(uint32_t prim, const StreamSrc* streams, uint32_t first, uint32_t count,
                const uint16_t* indices, uint32_t nidx) {
  g_dev.draws++;
  g_dev.frame_draws++;
  uint32_t h = g_dev.vs_handle;
  if (!(h & 1)) {
    static bool once = false;
    if (!once) hp2_log("d3d8: draw with a fixed-function vertex shader (FVF %08X): skipped", h), once = true;
    return;
  }
  uint32_t vsidx = hp2_rd32((h & ~1u) + 4);
  if (hp2_rd32(h & ~1u) != kVsMagic || vsidx >= g_vshaders.size()) return;
  VShader& vs = g_vshaders[vsidx];
  gpu::Draw d;
  d.vs = VertexProgram(vs);
  if (!d.vs) return;

  TargetsNow tg = BindTargets();
  if (!tg.w || !tg.h) return;

  // Vertex input.
  VsUniforms vu;
  std::memcpy(vu.c, g_dev.vs_c, sizeof vu.c);
  // Screen-space transform (c-38 scale, c-37 offset), as D3D sets it for the
  // full-target viewport; the shader epilogue inverts it.
  float zscale = 16777215.0f;
  if (g_dev.z) {
    uint32_t zw, zh, zf;
    if (SurfaceSize(g_dev.z, &zw, &zh, &zf) && (zf == XF_D16 || zf == XF_LIN_D16)) zscale = 65535.0f;
  } else {
    zscale = 1.0f;
  }
  float sc[4] = {tg.w * 0.5f, -(tg.h * 0.5f), zscale, 1.0f};
  float of[4] = {tg.w * 0.5f, tg.h * 0.5f, 0.0f, 0.0f};
  if (!(g_dev.const_mode & 0x10)) {
    std::memcpy(vu.c[96 - 38], sc, 16);
    std::memcpy(vu.c[96 - 37], of, 16);
  }
  std::memcpy(vu.scale, vu.c[96 - 38], 16);
  std::memcpy(vu.offset, vu.c[96 - 37], 16);
  for (int i = 0; i < 4; i++)
    if (vu.scale[i] == 0) vu.scale[i] = 1;
  // D3D puts pixel centres on integer coordinates, Metal on +0.5.
  vu.ndc_shift[0] = -2.0f * g_cfg.pixel_offset * g_cfg.scale / tg.pw;
  vu.ndc_shift[1] = 2.0f * g_cfg.pixel_offset * g_cfg.scale / tg.ph;
  // Widescreen: z = 1/wide on wide targets (the shader epilogue scales x).
  vu.ndc_shift[2] = tg.pw * tg.h != tg.ph * tg.w ? float(tg.ph * tg.w) / float(tg.pw * tg.h) : 0.0f;
  vu.ndc_shift[3] = 0;
  vu.snap[0] = vu.snap[1] = vu.snap[2] = vu.snap[3] = 0;
  if (vu.ndc_shift[2] != 0 && g_cfg.wide_hud && vs.screen_space)
    vu.ndc_shift[3] = HudAnchor(vs, streams, first, count, vu.ndc_shift[2]);
  // Streams used by the declaration -> gpu streams 0..3.
  int map[16];
  std::fill(map, map + 16, -1);
  int nstreams = 0;
  for (int r = 0; r < 16; r++) {
    uint32_t s = vs.attr[r][0];
    if (s == 0xFF || s >= 16) {
      vu.attr[r][0] = 0xFF;
      continue;
    }
    if (map[s] < 0) {
      if (nstreams == 4) {
        vu.attr[r][0] = 0xFF;
        continue;
      }
      map[s] = nstreams++;
      const StreamSrc& ss = streams[s];
      if (ss.va) {
        uint32_t stride = ss.stride;
        uint32_t bytes = stride ? (first + count) * stride : 64;
        uint32_t start = stride ? first * stride : 0;
        bytes -= start;
        if (hp2_is_committed(ss.va + start) && hp2_is_committed(ss.va + start + bytes - 1)) {
          d.streams[map[s]].data = hp2_ptr(ss.va + start);
          d.streams[map[s]].size = bytes;
        } else {
          static std::set<uint32_t> warned;
          if (warned.insert(ss.va).second)
            hp2_log("d3d8: stream %d data %08X+%u (%u bytes) not in guest memory: draw skipped", s,
                    ss.va, start, bytes);
          return;
        }
      }
    }
    vu.attr[r][0] = uint32_t(map[s]);
    vu.attr[r][1] = vs.attr[r][1];
    vu.attr[r][2] = vs.attr[r][2];
    vu.attr[r][3] = streams[s].stride;
    if (!streams[s].va) vu.attr[r][0] = 0xFF;
  }
  d.vs_uniforms = &vu;
  d.vs_uniforms_size = sizeof vu;

  // Topology.
  std::vector<uint16_t> tidx;
  bool need_idx;
  uint32_t mtl;
  if (indices) {
    if (!Topology(prim, nidx, &mtl, &tidx, &need_idx)) return;
    if (need_idx) {
      for (auto& v : tidx) v = indices[v];
    } else {
      tidx.assign(indices, indices + nidx);
    }
    d.indices = tidx.data();
    d.index_count = uint32_t(tidx.size());
  } else {
    if (!Topology(prim, count, &mtl, &tidx, &need_idx)) return;
    if (need_idx) {
      d.indices = tidx.data();
      d.index_count = uint32_t(tidx.size());
    } else {
      d.vertex_count = count;
    }
  }
  d.mtl_prim = mtl;

  // Textures.
  FsUniforms fu{};
  PsKey key;
  Bound bt[4];
  for (int s = 0; s < 4; s++) {
    bt[s] = BindTexture(g_dev.tex[s]);
    key.tex_kind[s] = bt[s].kind;
    d.tex[s] = bt[s].tex;
    gpu::SamplerState& sm = d.samp[s];
    sm.addr_u = MtlAddr(TS(s, TSS_ADDRESSU));
    sm.addr_v = MtlAddr(TS(s, TSS_ADDRESSV));
    sm.addr_w = MtlAddr(TS(s, TSS_ADDRESSW));
    uint32_t mag = TS(s, TSS_MAGFILTER), min = TS(s, TSS_MINFILTER), mip = TS(s, TSS_MIPFILTER);
    sm.mag = mag >= 2 ? 1 : 0;
    sm.min = min >= 2 ? 1 : 0;
    sm.mip = bt[s].levels > 1 ? uint8_t(std::min(mip, 2u)) : 0;
    sm.max_aniso = (mag == 3 || min == 3) ? uint8_t(std::clamp(TS(s, TSS_MAXANISOTROPY), 1u, 16u)) : 1;
    // Quality option: mipmapped linear textures get at least HP2_ANISO.
    if (sm.mip && sm.min == 1 && g_cfg.aniso > sm.max_aniso) sm.max_aniso = uint8_t(g_cfg.aniso);
    sm.border = TS(s, TSS_BORDERCOLOR);
    if (vs.screen_space && bt[s].tex && (sm.addr_u == 2 || sm.addr_v == 2 || (s == 0 && g_cfg.scale > 1))) {
      bool u_in = false, v_in = false;
      UvRangeInUnit(vs, streams, first, count, s, bt[s].sx, bt[s].sy, &u_in, &v_in);
      if (u_in && sm.addr_u == 2) sm.addr_u = 0;
      if (v_in && sm.addr_v == 2) sm.addr_v = 0;
      if (s == 0 && g_cfg.scale > 1 && u_in && v_in && !indices && AxisAlignedQuad(vs, streams, first, count))
        vu.snap[0] = 1;
    }
    fu.texscale[s][0] = bt[s].sx;
    fu.texscale[s][1] = bt[s].sy;
    fu.texscale[s][2] = bt[s].sx;
    fu.texscale[s][3] = bt[s].sy;
    fu.bump[s][0] = F(TS(s, TSS_BUMPENVMAT00));
    fu.bump[s][1] = F(TS(s, TSS_BUMPENVMAT01));
    fu.bump[s][2] = F(TS(s, TSS_BUMPENVMAT10));
    fu.bump[s][3] = F(TS(s, TSS_BUMPENVMAT11));
    fu.bumpl[s][0] = F(TS(s, TSS_BUMPENVLSCALE));
    fu.bumpl[s][1] = F(TS(s, TSS_BUMPENVLOFFSET));
    fu.akill[s] = TS(s, TSS_ALPHAKILL) != 0;
  }
  static const bool dev_nofog = getenv("HP2_DEV_NOFOG") != nullptr;
  bool fog = RS(RS_FOGENABLE) != 0 && !dev_nofog;
  bool two = RS(RS_TWOSIDEDLIGHTING) != 0;
  if (g_dev.ps) {
    key.combiners = true;
    for (uint32_t i = 0; i <= RS_PSINPUTTEXTURE; i++) key.words[i] = RS(i);
    key.words[57] = RS(RS_PSTEXTUREMODES);
    key.words[58] = fog;
    key.words[59] = two;
    key.nwords = 60;
    // Texture modes that do not sample leave the texture kind out of the key.
    for (int s = 0; s < 4; s++) {
      uint32_t m = (key.words[57] >> (5 * s)) & 0x1F;
      if (m == 0 || m == 4 || m == 5 || m == 0x11) key.tex_kind[s] = 0;
    }
  } else {
    for (uint32_t s = 0; s < 4; s++) {
      key.words[s * 8 + 0] = TS(s, TSS_COLOROP);
      key.words[s * 8 + 1] = TS(s, TSS_COLORARG0);
      key.words[s * 8 + 2] = TS(s, TSS_COLORARG1);
      key.words[s * 8 + 3] = TS(s, TSS_COLORARG2);
      key.words[s * 8 + 4] = TS(s, TSS_ALPHAOP);
      key.words[s * 8 + 5] = TS(s, TSS_ALPHAARG0);
      key.words[s * 8 + 6] = TS(s, TSS_ALPHAARG1);
      key.words[s * 8 + 7] = TS(s, TSS_ALPHAARG2);
      if (TS(s, TSS_RESULTARG) == TA_TEMP) key.words[32] |= 1u << s;
    }
    // Stages after the first disabled one do not matter.
    bool off = false;
    for (uint32_t s = 0; s < 4; s++) {
      if (key.words[s * 8] == TOP_DISABLE || key.words[s * 8] == 0) off = true;
      if (off) {
        for (int i = 0; i < 8; i++) key.words[s * 8 + i] = 0;
        key.tex_kind[s] = 0;
      }
    }
    key.words[33] = fog;
    key.words[34] = RS(RS_SPECULARENABLE) != 0;
    key.words[35] = two;
    key.nwords = 36;
  }
  // D3DZB_USEW: the game's 3D draws use the W-buffer (linear eye w); z/w
  // in a float buffer loses the far range and coplanar decals fight.
  key.wdepth = g_cfg.wbuffer && RS(RS_ZENABLE) == 2 && tg.depth;
  d.fs = FragmentProgram(key);
  if (!d.fs) return;
  // Dev: HP2_DEV_SKIP_PS=<hash> drops draws with that fragment program (bisecting).
  static const uint64_t dev_skip = [] {
    const char* v = getenv("HP2_DEV_SKIP_PS");
    return v ? strtoull(v, nullptr, 16) : 0ull;
  }();
  if (dev_skip && key.Hash() == dev_skip) return;
  // Dev: HP2_DEV_SKIP_DRAWS=a-b drops draws a..b of every frame;
  // HP2_DEV_LOG_DRAWS=<frame> logs every draw of that frame.
  static const std::pair<uint32_t, uint32_t> dev_range = [] {
    const char* v = getenv("HP2_DEV_SKIP_DRAWS");
    uint32_t a = 0, b = 0;
    if (v) std::sscanf(v, "%u-%u", &a, &b);
    return std::make_pair(a, v ? b : 0u);
  }();
  static const uint64_t dev_log_frame = [] {
    const char* v = getenv("HP2_DEV_LOG_DRAWS");
    return v ? strtoull(v, nullptr, 10) : ~0ull;
  }();
  uint32_t draw_no = g_dev.frame_draws - 1;
  if (g_dev.frame + 1 == dev_log_frame) {
    char tx[320] = "";
    size_t o = 0;
    for (int s2 = 0; s2 < 4; s2++) {
      uint32_t th = g_dev.tex[s2];
      if (!th) continue;
      TexLayout L;
      DescribeTexture(Hdr{th}.format(), Hdr{th}.size(), &L);
      o += std::snprintf(tx + o, sizeof tx - o, " t%d=%s %ux%u lv %u filt %u/%u/%u an %u ad %u/%u lb %X ml %u ak %u d%08X%s", s2,
                         FormatInfo(L.xfmt).name ? FormatInfo(L.xfmt).name : "?", L.w, L.h,
                         bt[s2].levels, d.samp[s2].mag, d.samp[s2].min, d.samp[s2].mip,
                         d.samp[s2].max_aniso, d.samp[s2].addr_u, d.samp[s2].addr_v, TS(s2, TSS_MIPMAPLODBIAS),
                         TS(s2, TSS_MAXMIPLEVEL), TS(s2, TSS_ALPHAKILL), Hdr{th}.data(), bt[s2].tex ? "" : "(none)");
    }
    char st[96] = "";
    if (RS(RS_STENCILENABLE))
      std::snprintf(st, sizeof st, " stencil %X ref %X mask %X/%X ops %X/%X/%X", RS(RS_STENCILFUNC),
                    RS(RS_STENCILREF), RS(RS_STENCILMASK), RS(RS_STENCILWRITEMASK),
                    RS(RS_STENCILFAIL), RS(RS_STENCILZFAIL), RS(RS_STENCILPASS));
    hp2_log("draw %u: prim %u n %u vs %u ps %016llx%s blend %u %X/%X z %u/%u %X cw %X cull %X at %u %X/%02X%s%s",
            draw_no, prim, indices ? nidx : count, vsidx, (unsigned long long)key.Hash(),
            g_dev.ps ? "(comb)" : "(ff)", RS(RS_ALPHABLENDENABLE), RS(RS_SRCBLEND),
            RS(RS_DESTBLEND), RS(RS_ZENABLE), RS(RS_ZWRITEENABLE), RS(RS_ZFUNC),
            RS(RS_COLORWRITEENABLE), RS(RS_CULLMODE), RS(RS_ALPHATESTENABLE), RS(RS_ALPHAFUNC),
            RS(RS_ALPHAREF) & 0xFF, st, tx);
  }
  if (draw_no >= dev_range.first && draw_no <= dev_range.second && dev_range.second) return;
  // Dev: HP2_DEV_MAGENTA=a-b draws a..b as opaque magenta (no blend).
  static const std::pair<uint32_t, uint32_t> dev_mag = [] {
    const char* v = getenv("HP2_DEV_MAGENTA");
    uint32_t a = 0, b = 0;
    if (v) std::sscanf(v, "%u-%u", &a, &b);
    return std::make_pair(a, v ? b : 0u);
  }();
  bool magenta = dev_mag.second && draw_no >= dev_mag.first && draw_no <= dev_mag.second;
  if (magenta) {
    // HP2_DEV_SHOW=fog|d0|t0uv|t0: show that input instead of magenta (t0: stage 0's
    // texture unlit, texels with alpha < 0.5 discarded; the HD pack's coverage probe).
    static gpu::ProgId mp = [] {
      const char* v = getenv("HP2_DEV_SHOW");
      if (v && !strcmp(v, "t0"))
        return gpu::CompileFragment(std::string(kMslCommon) +
                                        "fragment float4 fmain(VOut in [[stage_in]], texture2d<float> tx0 "
                                        "[[texture(0)]], sampler sm0 [[sampler(0)]]) {\n"
                                        "  float4 c = tx0.sample(sm0, in.t0.xy);\n"
                                        "  if (c.a < 0.5) discard_fragment();\n"
                                        "  return float4(c.rgb, 1); }\n",
                                    "devshow");
      std::string e = "float4(1, 0, 1, 1)";
      if (v && !strcmp(v, "fog")) e = "float4(saturate(in.fog), saturate(-in.fog), saturate(in.fog / 100), 1)";
      if (v && !strcmp(v, "d0")) e = "float4(in.d0.rgb, 1)";
      if (v && !strcmp(v, "d0a")) e = "float4(in.d0.aaa, 1)";
      if (v && !strcmp(v, "t0uv")) e = "float4(fract(in.t0.xy), 0, 1)";
      return gpu::CompileFragment(std::string(kMslCommon) +
                                      "fragment float4 fmain(VOut in [[stage_in]]) { return " + e +
                                      "; }\n",
                                  "devshow");
    }();
    d.fs = mp;
  }
  for (int i = 0; i < 8; i++) {
    ColorF(RS(RS_PSCONSTANT0_0 + i), fu.c0[i]);
    ColorF(RS(RS_PSCONSTANT1_0 + i), fu.c1[i]);
  }
  ColorF(RS(RS_PSFINALCOMBINERCONSTANT0), fu.fc0);
  ColorF(RS(RS_PSFINALCOMBINERCONSTANT1), fu.fc1);
  ColorF(RS(RS_TEXTUREFACTOR), fu.tfactor);
  ColorF(RS(RS_FOGCOLOR), fu.fogcolor);
  if (fog && g_cfg.trace) {
    static std::set<uint64_t> seen;
    uint64_t k = uint64_t(RS(RS_FOGTABLEMODE)) << 48 ^ uint64_t(RS(RS_FOGSTART)) << 16 ^
                 RS(RS_FOGEND) ^ uint64_t(RS(RS_FOGCOLOR)) << 24;
    if (seen.size() < 40 && seen.insert(k).second)
      hp2_log("d3d8: fog: mode %u start %g end %g density %g color %08X range %u",
              RS(RS_FOGTABLEMODE), F(RS(RS_FOGSTART)), F(RS(RS_FOGEND)), F(RS(RS_FOGDENSITY)),
              RS(RS_FOGCOLOR), RS(RS_RANGEFOGENABLE));
  }
  fu.fogp[0] = float(RS(RS_FOGTABLEMODE));
  fu.fogp[1] = F(RS(RS_FOGSTART));
  fu.fogp[2] = F(RS(RS_FOGEND));
  fu.fogp[3] = F(RS(RS_FOGDENSITY));
  fu.alpha[0] = RS(RS_ALPHATESTENABLE) ? 1.f : 0.f;
  fu.alpha[1] = float(MtlCmp(RS(RS_ALPHAFUNC)));
  fu.alpha[2] = (RS(RS_ALPHAREF) & 0xFF) / 255.f;
  fu.flags[0] = fog;
  fu.flags[1] = RS(RS_SPECULARENABLE) != 0;
  fu.flags[2] = two;
  d.fs_uniforms = &fu;
  d.fs_uniforms_size = sizeof fu;

  // Output merger.
  gpu::BlendState& bs = d.blend;
  bs.enable = RS(RS_ALPHABLENDENABLE) != 0 && !(magenta && !getenv("HP2_DEV_MAGENTA_BLEND"));
  bs.src = bs.src_a = MtlBlend(RS(RS_SRCBLEND));
  bs.dst = bs.dst_a = MtlBlend(RS(RS_DESTBLEND));
  bs.op = bs.op_a = MtlBlendOp(RS(RS_BLENDOP));
  uint32_t cw = RS(RS_COLORWRITEENABLE);
  bs.write_mask = uint8_t(((cw >> 16) & 1) << 3 | ((cw >> 8) & 1) << 2 | (cw & 1) << 1 |
                          ((cw >> 24) & 1));
  ColorF(RS(RS_BLENDCOLOR), d.blend_color);
  gpu::DepthState& ds = d.depth;
  ds.test = RS(RS_ZENABLE) != 0 && tg.depth;
  ds.write = RS(RS_ZWRITEENABLE) != 0;
  ds.func = MtlCmp(RS(RS_ZFUNC));
  ds.stencil = RS(RS_STENCILENABLE) != 0 && tg.depth;
  ds.s_func = MtlCmp(RS(RS_STENCILFUNC));
  ds.s_fail = MtlStencilOp(RS(RS_STENCILFAIL));
  ds.s_zfail = MtlStencilOp(RS(RS_STENCILZFAIL));
  ds.s_pass = MtlStencilOp(RS(RS_STENCILPASS));
  ds.s_read = uint8_t(RS(RS_STENCILMASK));
  ds.s_write = uint8_t(RS(RS_STENCILWRITEMASK));
  d.stencil_ref = RS(RS_STENCILREF) & 0xFF;
  uint32_t cull = RS(RS_CULLMODE), front = RS(RS_FRONTFACE);
  d.front_ccw = front == CULL_CCW;
  d.cull = cull == CULL_NONE ? 0 : (cull != front ? 2 /* back */ : 1 /* front */);
  d.wireframe = RS(RS_FILLMODE) == FILL_WIREFRAME;
  // Polygon offset per fill mode (D3D8 on Xbox: POINT/WIREFRAME/SOLIDOFFSETENABLE;
  // D3DRS_ZBIAS goes through these too, via SetRenderState_ZBias, which this
  // game neither links nor inlines). Units: depth LSBs, like Metal's for z
  // near 1. Not applied to W-buffer depth (written by the shader); the game
  // sets no offsets at all.
  uint32_t fill = RS(RS_FILLMODE);
  uint32_t offset_on = fill == FILL_WIREFRAME ? RS(RS_WIREFRAMEOFFSETENABLE)
                       : fill == FILL_POINT   ? RS(RS_POINTOFFSETENABLE)
                                              : RS(RS_SOLIDOFFSETENABLE);
  if (offset_on) {
    static bool once = false;
    if (!once)
      hp2_log("d3d8: first polygon offset: %f + slope %f%s", F(RS(RS_POLYGONOFFSETZOFFSET)),
              F(RS(RS_POLYGONOFFSETZSLOPESCALE)), key.wdepth ? " (W-buffer draw: ignored)" : ""),
          once = true;
    d.depth_bias = F(RS(RS_POLYGONOFFSETZOFFSET));
    d.slope_bias = F(RS(RS_POLYGONOFFSETZSLOPESCALE));
  }
  float sw = float(tg.pw), sh = float(tg.ph);
  d.vp_x = 0, d.vp_y = 0, d.vp_w = sw, d.vp_h = sh, d.vp_minz = 0, d.vp_maxz = 1;
  d.sc_x = 0, d.sc_y = 0, d.sc_w = int32_t(sw), d.sc_h = int32_t(sh);
  gpu::DoDraw(d);
  // Dev: HP2_DEV_PROBE=frame,x,y logs every draw of that frame that changes
  // back-buffer pixel (x, y) (reads the target back after each draw: slow).
  static const std::array<int64_t, 3> probe = [] {
    std::array<int64_t, 3> p{-1, 0, 0};
    if (const char* v = getenv("HP2_DEV_PROBE"))
      std::sscanf(v, "%lld,%lld,%lld", (long long*)&p[0], (long long*)&p[1], (long long*)&p[2]);
    return p;
  }();
  if (probe[0] >= 0 && int64_t(g_dev.frame + 1) == probe[0] && tg.color) {
    static uint32_t last = 0;
    std::vector<uint8_t> px;
    if (gpu::ReadPixels(tg.color->tex, tg.pw, tg.ph, &px)) {
      size_t i = (size_t(probe[2]) * tg.pw + size_t(probe[1])) * 4;
      uint32_t v = i + 3 < px.size() ? (px[i] << 16 | px[i + 1] << 8 | px[i + 2]) : 0;
      if (v != last)
        hp2_log("probe: draw %u -> %06X (target %08X %ux%u)", g_dev.frame_draws - 1, v,
                tg.color->data, tg.w, tg.h);
      last = v;
    }
  }
}

void DrawFromStreams(uint32_t prim, uint32_t first, uint32_t count, const uint16_t* idx,
                     uint32_t nidx) {
  StreamSrc ss[16];
  for (int s = 0; s < 16; s++) {
    uint32_t vb = g_dev.streams[s].vb;
    if (!vb) continue;
    ss[s].va = CpuAddr(Hdr{vb}.data());
    ss[s].stride = g_dev.streams[s].stride;
  }
  DrawCommon(prim, ss, first, count, idx, nidx);
}

// Index data the draw reads: a CPU pointer, or a physical one (Data of a
// D3D-created buffer). Null (logged once) if neither is guest memory.
const uint16_t* IndexPtr(uint32_t va, uint32_t n) {
  uint32_t bytes = n * 2;
  for (uint32_t a : {va, CpuAddr(va)})
    if (a && hp2_is_committed(a) && hp2_is_committed(a + bytes - 1))
      return static_cast<const uint16_t*>(hp2_ptr(a));
  static bool once = false;
  if (!once) hp2_log("d3d8: index data %08X (%u indices) not in guest memory", va, n), once = true;
  return nullptr;
}

// Indexed draw: rebases the indices to the smallest one used.
void DrawIndexed(uint32_t prim, uint32_t count, const uint16_t* raw, const StreamSrc* ss,
                 uint32_t base) {
  if (!count) return;
  uint32_t lo = 0xFFFF, hi = 0;
  for (uint32_t i = 0; i < count; i++) lo = std::min<uint32_t>(lo, raw[i]), hi = std::max<uint32_t>(hi, raw[i]);
  std::vector<uint16_t> idx(raw, raw + count);
  for (auto& v : idx) v = uint16_t(v - lo);
  DrawCommon(prim, ss, base + lo, hi - lo + 1, idx.data(), count);
}

// ---- frame end ----------------------------------------------------------------------------
// Shows a colour surface in the window (fitted 4:3; no-op without one).
void Present(uint32_t surface) {
  if (!surface) return;
  if (Target* t = FindTarget(Hdr{surface}.data(), false))
    gpu::PresentFrame(t->tex, t->pw, t->ph);
}

std::vector<std::thread> g_shot_writers;

void JoinShotWriters() {
  for (auto& t : g_shot_writers) t.join();
  g_shot_writers.clear();
}

void Screenshot(uint32_t surface, const char* kind, uint64_t n) {
  if (g_shot_writers.size() > 8) JoinShotWriters();
  if (g_cfg.shot_dir.empty() || !surface) return;
  Target* t = FindTarget(Hdr{surface}.data(), false);
  if (!t) return;
  std::vector<uint8_t> px;
  uint32_t w = t->pw, h = t->ph;
  if (!gpu::ReadPixels(t->tex, w, h, &px)) return;
  char path[600];
  std::snprintf(path, sizeof path, "%s/%s_%06llu.png", g_cfg.shot_dir.c_str(), kind,
                (unsigned long long)n);
  // Encode on a worker so the Swap thread does not stall (joined at exit).
  g_shot_writers.emplace_back([px = std::move(px), p = std::string(path), w, h] {
    if (gpu::WritePng(p.c_str(), w, h, px.data())) hp2_log("d3d8: screenshot %s", p.c_str());
  });
}

// HP2_DEV_PRESENT_TEST: what the window would show for this frame.
void ScreenshotPresent(uint64_t n) {
  uint32_t w, h;
  gpu::TexId t = gpu::LastPresentCopy(&w, &h);
  if (!t || g_cfg.shot_dir.empty()) return;
  std::vector<uint8_t> px;
  if (!gpu::ReadPixels(t, w, h, &px)) return;
  char path[600];
  std::snprintf(path, sizeof path, "%s/present_%06llu.png", g_cfg.shot_dir.c_str(),
                (unsigned long long)n);
  g_shot_writers.emplace_back([px = std::move(px), p = std::string(path), w, h] {
    if (gpu::WritePng(p.c_str(), w, h, px.data())) hp2_log("d3d8: screenshot %s", p.c_str());
  });
}

void FreePending(Hp2Ctx& c, bool all) {
  auto& v = g_dev.pending_free;
  for (size_t i = 0; i < v.size();) {
    if (all || v[i].second <= g_dev.frame) {
      FreeContiguous(c, v[i].first);
      v[i] = v.back();
      v.pop_back();
    } else {
      i++;
    }
  }
}

// ==== the replaced functions =============================================================
// Names and conventions: FUNCTIONS.md. stdcall unless noted.

// 0x133FA0 Direct3DCreate8(SDKVersion)
void Direct3DCreate8(Hp2Ctx& c) {
  Seen("Direct3DCreate8");
  // Dev switch until the DSound HLE exists (Audio role): back the APU MMIO
  // window with plain memory so the DSOUND lib's init does not fault.
  if (const char* v = getenv("HP2_DEV_APU_RAM"); v && atoi(v)) {
    hp2_commit(0xFE800000, 0x80000);  // APU
    hp2_commit(0xFEC00000, 0x10000);  // AC97
    hp2_wr32(0xFEC00130, 0x100);       // AC97 global status: primary codec ready
    hp2_log("d3d8: HP2_DEV_APU_RAM: APU registers 0xFE800000 backed by RAM (dev only)");
  }
  hp2_ret_stdcall(c, 1, 1);
}

// 0x130620 D3D::SetPushBufferSize(PushBufferSize, KickOffSize)
void SetPushBufferSize(Hp2Ctx& c) {
  Seen("SetPushBufferSize");
  hp2_ret_stdcall(c, 0, 2);
}

// 0x130640 Direct3D_CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags,
//                                pPresentationParameters, ppReturnedDeviceInterface)
void CreateDevice(Hp2Ctx& c) {
  Seen("CreateDevice");
  uint32_t pp = hp2_arg(c, 4), out = hp2_arg(c, 5);
  LoadConfig();
  if (!gpu::Init()) hp2_fatal("d3d8: Metal is not available");
  uint32_t w = hp2_rd32(pp + 0), h = hp2_rd32(pp + 4), fmt = hp2_rd32(pp + 8);
  uint32_t autodepth = hp2_rd32(pp + 0x20), zfmt = hp2_rd32(pp + 0x24);
  hp2_log("d3d8: CreateDevice %ux%u fmt %02X backbuffers %u ms %X autodepth %u zfmt %02X "
          "flags %X refresh %u interval %u; render scale %u, %s",
          w, h, fmt, hp2_rd32(pp + 0xC), hp2_rd32(pp + 0x10), autodepth, zfmt,
          hp2_rd32(pp + 0x28), hp2_rd32(pp + 0x2C), hp2_rd32(pp + 0x30), g_cfg.scale,
          gpu::DeviceName());
  if (!w) w = 640;
  if (!h) h = 480;
  g_dev.bb_w = w, g_dev.bb_h = h;
  // The CDevice the game gets back: the lib's own global (the game never
  // dereferences it; only g_pDevice-reading code would).
  hp2_wr32(kDevicePtrVa, kDeviceVa);
  if (!g_dev.created) {
    g_dev.back = MakeSurface(c, w, h, fmt ? fmt : XF_LIN_A8R8G8B8);
    g_dev.front = MakeSurface(c, w, h, fmt ? fmt : XF_LIN_A8R8G8B8);
    if (autodepth) g_dev.zbuf = MakeSurface(c, w, h, zfmt ? zfmt : XF_D24S8);
    InitDefaultStates();
    g_dev.created = true;
  }
  g_dev.rt = g_dev.back;
  g_dev.z = g_dev.zbuf;
  if (!autodepth) SetRS(RS_ZENABLE, 0);
  g_dev.last_vblank = hp2_vblank_count();
  g_dev.last_swap = std::chrono::steady_clock::now();
  if (out) hp2_wr32(out, kDeviceVa);
  hp2_ret_stdcall(c, 0, 6);
}

// 0x130720 SetRenderState_Simple push stub: fastcall(ecx = method | count << 18,
// edx = value), plain ret. The inlined callers store the render-state cell
// themselves right after; mirror it anyway for the methods seen.
void Push1(Hp2Ctx& c) {
  Seen("Push1");
  uint32_t m = c.ecx & 0x1FFFC, v = c.edx;
  int rs = -1;
  switch (m) {
    case 0x300: rs = RS_ALPHATESTENABLE; break;
    case 0x304: rs = RS_ALPHABLENDENABLE; break;
    case 0x310: rs = RS_DITHERENABLE; break;
    case 0x33C: rs = RS_ALPHAFUNC; break;
    case 0x340: rs = RS_ALPHAREF; break;
    case 0x344: rs = RS_SRCBLEND; break;
    case 0x348: rs = RS_DESTBLEND; break;
    case 0x350: rs = RS_BLENDOP; break;
    case 0x354: rs = RS_ZFUNC; break;
    case 0x358: rs = RS_COLORWRITEENABLE; break;
    case 0x35C: rs = RS_ZWRITEENABLE; break;
  }
  if (rs >= 0)
    SetRS(uint32_t(rs), v);
  else {
    static std::set<uint32_t> warned;
    if (warned.insert(m).second) hp2_log("d3d8: Push1 method %04X value %08X not mapped", m, v);
  }
  hp2_ret_cdecl(c, c.eax);
}

#define RS_SETTER(fnname, idx)              \
  void fnname(Hp2Ctx& c) {                  \
    Seen(#fnname);                          \
    SetRS(idx, hp2_arg(c, 0));              \
    TRACE("d3d8: " #fnname " %08X", hp2_arg(c, 0)); \
    hp2_ret_stdcall(c, 0, 1);               \
  }
RS_SETTER(SetRenderState_FogColor, RS_FOGCOLOR)          // 0x130B00
RS_SETTER(SetRenderState_CullMode, RS_CULLMODE)          // 0x130B50
RS_SETTER(SetRenderState_NormalizeNormals, RS_NORMALIZENORMALS)  // 0x130C00
RS_SETTER(SetRenderState_ZEnable, RS_ZENABLE)            // 0x131BD0
RS_SETTER(SetRenderState_StencilEnable, RS_STENCILENABLE)  // 0x131C60

// 0x1310F0 SetTextureState_BumpEnv(Stage, Type, Value)
void SetTextureState_BumpEnv(Hp2Ctx& c) {
  Seen("SetTextureState_BumpEnv");
  uint32_t s = hp2_arg(c, 0), t = hp2_arg(c, 1), v = hp2_arg(c, 2);
  if (s < 4 && t < TSS_COUNT) SetTS(s, t, v);
  hp2_ret_stdcall(c, 0, 3);
}

// 0x131160 SetTextureState_BorderColor(Stage, Value)
void SetTextureState_BorderColor(Hp2Ctx& c) {
  Seen("SetTextureState_BorderColor");
  uint32_t s = hp2_arg(c, 0), v = hp2_arg(c, 1);
  if (s < 4) SetTS(s, TSS_BORDERCOLOR, v);
  hp2_ret_stdcall(c, 0, 2);
}

// 0x132570 CreateVertexShader(pDeclaration, pFunction, pHandle, Usage)
void CreateVertexShader(Hp2Ctx& c) {
  Seen("CreateVertexShader");
  uint32_t decl = hp2_arg(c, 0), func = hp2_arg(c, 1), ph = hp2_arg(c, 2);
  VShader vs;
  vs.index = uint32_t(g_vshaders.size());
  for (auto& a : vs.attr) a[0] = 0xFF;
  if (func) {
    uint32_t hdr = hp2_rd32(func);
    uint32_t type = hdr & 0xFFFF, count = hdr >> 16;
    if (type != 0x2078)
      hp2_log("d3d8: CreateVertexShader: function type %04X (expected 0x2078 'xvs')", type);
    vs.func.resize(size_t(count) * 4);
    std::memcpy(vs.func.data(), hp2_ptr(func + 4), vs.func.size() * 4);
  } else {
    hp2_log("d3d8: CreateVertexShader without a function (fixed-function declaration)");
  }
  if (decl) {
    uint32_t stream = 0, offset = 0;
    for (uint32_t p = decl, guard = 0; guard < 256; guard++) {
      uint32_t t = hp2_rd32(p);
      p += 4;
      if (t == 0xFFFFFFFF) break;
      uint32_t kind = t >> 29;
      if (kind == VSD_TOKEN_STREAM) {
        stream = t & 0xF;
        offset = 0;
      } else if (kind == VSD_TOKEN_STREAMDATA) {
        if (t & 0x10000000) {  // skip
          uint32_t n = (t >> 16) & 0xF;
          offset += (t & 0x08000000) ? n : n * 4;
        } else {
          uint32_t reg = t & 0x1F, type = (t >> 16) & 0xFF;
          if (reg < 16) {
            vs.attr[reg][0] = type == VSDT_NONE ? 0xFF : stream;
            vs.attr[reg][1] = offset;
            vs.attr[reg][2] = type;
          }
          offset += VsdtSize(type);
        }
      } else if (kind == VSD_TOKEN_CONSTMEM) {
        uint32_t n = (t >> 25) & 0xF, addr = t & 0xFF;
        std::vector<uint32_t> vals(n * 4);
        for (auto& v : vals) v = hp2_rd32(p), p += 4;
        vs.constants.push_back({addr, std::move(vals)});
      } else if (kind == VSD_TOKEN_EXT) {
        p += 4 * ((t >> 24) & 0x1F);
      } else if (kind == VSD_TOKEN_TESSELLATOR || kind == VSD_TOKEN_NOP) {
        // not used by this game
      }
    }
  }
  if (g_cfg.trace) {
    std::string a;
    char b[48];
    for (int r = 0; r < 16; r++)
      if (vs.attr[r][0] != 0xFF) {
        std::snprintf(b, sizeof b, " v%d=s%u+%u:%02X", r, vs.attr[r][0], vs.attr[r][1], vs.attr[r][2]);
        a += b;
      }
    hp2_log("d3d8: vs %u decl %08X func %08X:%s", vs.index, decl, func, a.c_str());
  }
  uint32_t obj = NewHeader(16);
  hp2_wr32(obj, kVsMagic);
  hp2_wr32(obj + 4, vs.index);
  g_vshaders.push_back(std::move(vs));
  if (!g_cfg.shader_check && !g_vshaders.back().func.empty())
    PrecompileVertexProgram(g_vshaders.back());
  if (g_cfg.shader_check) {
    VShader& v = g_vshaders.back();
    bool ok = VertexProgram(v) != 0;
    static uint32_t n_ok = 0, n_bad = 0;
    (ok ? n_ok : n_bad)++;
    if (!ok) hp2_log("d3d8: shader check: vertex shader %u (function %08X) FAILED", v.index, func);
    hp2_log("d3d8: shader check: %u vertex shaders compiled, %u failed", n_ok, n_bad);
  }
  if (ph) hp2_wr32(ph, obj | 1);
  hp2_ret_stdcall(c, 0, 4);
}

bool DevLogFrame() {
  static const uint64_t f = [] {
    const char* v = getenv("HP2_DEV_LOG_DRAWS");
    return v ? strtoull(v, nullptr, 10) : ~0ull;
  }();
  return g_dev.frame + 1 == f;
}

// 0x1326B0 SetVertexShaderConstant1: fastcall(ecx = register 0..191, edx = data)
void SetVertexShaderConstant1(Hp2Ctx& c) {
  if (DevLogFrame()) hp2_log("  const1 reg %u (c%d) n %u", c.ecx, int(c.ecx) - 96, 1);
  Seen("SetVertexShaderConstant1");
  uint32_t r = c.ecx;
  if (r < 192) std::memcpy(g_dev.vs_c[r], hp2_ptr(c.edx), 16);
  hp2_ret_cdecl(c, c.eax);
}
// 0x132710 SetVertexShaderConstant4: fastcall, 4 registers
void SetVertexShaderConstant4(Hp2Ctx& c) {
  if (DevLogFrame()) hp2_log("  const4 reg %u (c%d) n %u", c.ecx, int(c.ecx) - 96, 4);
  Seen("SetVertexShaderConstant4");
  uint32_t r = c.ecx;
  for (uint32_t i = 0; i < 4; i++)
    if (r + i < 192) std::memcpy(g_dev.vs_c[r + i], hp2_ptr(c.edx + 16 * i), 16);
  hp2_ret_cdecl(c, c.eax);
}
// 0x1328A0 SetVertexShaderConstantNotInline: fastcall(ecx reg, edx data), [esp+4] count.
// The count is in DWORDs (the lib copies it with rep movsd): the game passes
// 8 for two registers, 12 for three.
void SetVertexShaderConstantNotInline(Hp2Ctx& c) {
  Seen("SetVertexShaderConstantNotInline");
  uint32_t r = c.ecx, n = hp2_arg(c, 0);
  if (DevLogFrame()) hp2_log("  constN reg %u (c%d) %u dwords", r, int(r) - 96, n);
  for (uint32_t i = 0; i < n; i++)
    if (r * 4 + i < 192 * 4) std::memcpy(&g_dev.vs_c[0][0] + r * 4 + i, hp2_ptr(c.edx + 4 * i), 4);
  hp2_ret_stdcall(c, c.eax, 1);
}

// 0x1328E0 SetStreamSource(StreamNumber, pStreamData, Stride)
void SetStreamSource(Hp2Ctx& c) {
  Seen("SetStreamSource");
  uint32_t n = hp2_arg(c, 0);
  if (n < 16) {
    g_dev.streams[n].vb = hp2_arg(c, 1);
    g_dev.streams[n].stride = hp2_arg(c, 2);
  }
  hp2_ret_stdcall(c, 0, 3);
}

// 0x132A50 SetShaderConstantMode(Mode)
void SetShaderConstantMode(Hp2Ctx& c) {
  Seen("SetShaderConstantMode");
  g_dev.const_mode = hp2_arg(c, 0);
  hp2_log("d3d8: SetShaderConstantMode %X", g_dev.const_mode);
  hp2_ret_stdcall(c, 0, 1);
}

// 0x132CA0 SetVertexShader(Handle)
void SetVertexShader(Hp2Ctx& c) {
  Seen("SetVertexShader");
  uint32_t h = hp2_arg(c, 0);
  g_dev.vs_handle = h;
  if ((h & 1) && hp2_rd32(h & ~1u) == kVsMagic) {
    uint32_t i = hp2_rd32((h & ~1u) + 4);
    if (i < g_vshaders.size())
      for (auto& [addr, vals] : g_vshaders[i].constants)
        for (size_t k = 0; k < vals.size() / 4; k++)
          if (addr + k < 192) std::memcpy(g_dev.vs_c[addr + k], &vals[4 * k], 16);
  }
  hp2_ret_stdcall(c, 0, 1);
}

// 0x132E00 CreateVertexBuffer2(Length) -> IDirect3DVertexBuffer8*
void CreateVertexBuffer2(Hp2Ctx& c) {
  Seen("CreateVertexBuffer2");
  uint32_t len = hp2_arg(c, 0);
  uint32_t data = AllocContiguous(c, len, 0);
  uint32_t vb = 0;
  if (data) {
    vb = NewHeader(16);
    hp2_wr32(vb, COMMON_TYPE_VERTEXBUFFER | COMMON_D3DCREATED | 1);
    hp2_wr32(vb + 4, data & 0x0FFFFFFF);
    hp2_wr32(vb + 8, 0);
  }
  hp2_ret_stdcall(c, vb, 1);
}

// 0x132E50 VertexBuffer_Lock2(pVB, Flags) -> BYTE*
void VertexBuffer_Lock2(Hp2Ctx& c) {
  Seen("VertexBuffer_Lock2");
  uint32_t vb = hp2_arg(c, 0);
  hp2_ret_stdcall(c, CpuAddr(Hdr{vb}.data()), 2);
}

// 0x132EA0 SetRenderTarget(pRenderTarget, pNewZStencil)
void SetRenderTarget(Hp2Ctx& c) {
  Seen("SetRenderTarget");
  uint32_t rt = hp2_arg(c, 0), z = hp2_arg(c, 1);
  if (rt) g_dev.rt = rt;
  g_dev.z = z;
  TRACE("d3d8: SetRenderTarget %08X %08X", rt, z);
  hp2_ret_stdcall(c, 0, 2);
}

// 0x132EC0 SetTransform(State, pMatrix)
void SetTransform(Hp2Ctx& c) {
  Seen("SetTransform");
  uint32_t s = hp2_arg(c, 0), m = hp2_arg(c, 1);
  if (s < 10) std::memcpy(g_dev.transforms[s], hp2_ptr(m), 64);
  hp2_ret_stdcall(c, 0, 2);
}

// 0x132FD0 SetMaterial(pMaterial)
void SetMaterial(Hp2Ctx& c) {
  Seen("SetMaterial");
  std::memcpy(g_dev.material, hp2_ptr(hp2_arg(c, 0)), sizeof g_dev.material);
  hp2_ret_stdcall(c, 0, 1);
}

// 0x133000 KickOffAndWaitForIdle()
void KickOffAndWaitForIdle(Hp2Ctx& c) {
  Seen("KickOffAndWaitForIdle");
  hp2_ret_cdecl(c, 0);
}

// 0x133010 BlockUntilVerticalBlank()
void BlockUntilVerticalBlank(Hp2Ctx& c) {
  Seen("BlockUntilVerticalBlank");
  auto t0 = std::chrono::steady_clock::now();
  hp2_vblank_wait(hp2_vblank_count());
  g_ov.wait_ms +=
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  hp2_ret_cdecl(c, 0);
}

// 0x133330 GetBackBuffer2(BackBuffer) -> surface (AddRef'd); -1 = front buffer
void GetBackBuffer2(Hp2Ctx& c) {
  Seen("GetBackBuffer2");
  int32_t i = int32_t(hp2_arg(c, 0));
  uint32_t s = i == -1 ? g_dev.front : g_dev.back;
  AddRef(s);
  hp2_ret_stdcall(c, s, 1);
}

// 0x133380 GetRenderTarget2() -> surface (AddRef'd)
void GetRenderTarget2(Hp2Ctx& c) {
  Seen("GetRenderTarget2");
  AddRef(g_dev.rt);
  hp2_ret_cdecl(c, g_dev.rt);
}

// 0x1333A0 GetDepthStencilSurface2() -> surface (AddRef'd) or 0
void GetDepthStencilSurface2(Hp2Ctx& c) {
  Seen("GetDepthStencilSurface2");
  AddRef(g_dev.z);
  hp2_ret_cdecl(c, g_dev.z);
}

// 0x133520 SetTexture(Stage, pTexture)
void SetTexture(Hp2Ctx& c) {
  Seen("SetTexture");
  uint32_t s = hp2_arg(c, 0);
  if (s < 4) g_dev.tex[s] = hp2_arg(c, 1);
  hp2_ret_stdcall(c, 0, 2);
}

// 0x1336D0 SetIndices(pIndexBuffer, BaseVertexIndex): like the lib, publishes the
// buffer's Data in D3D__IndexData (0x1403E4), which the game reads inline to
// build the pIndexData it passes to DrawIndexedVertices.
void SetIndices(Hp2Ctx& c) {
  Seen("SetIndices");
  uint32_t ib = hp2_arg(c, 0);
  g_dev.idx_data = ib;
  g_dev.idx_base = hp2_arg(c, 1);
  hp2_wr32(kIndexDataVa, ib ? hp2_rd32(ib + 4) : 0);
  hp2_ret_stdcall(c, 0, 2);
}

// 0x133A80 CreatePixelShader(pPSDef, pHandle): {refcount, flags, pDef, copy of def}
void CreatePixelShader(Hp2Ctx& c) {
  Seen("CreatePixelShader");
  uint32_t def = hp2_arg(c, 0), ph = hp2_arg(c, 1);
  uint32_t obj = NewHeader(0xFC);
  hp2_wr32(obj, 1);
  hp2_wr32(obj + 4, 1);
  hp2_wr32(obj + 8, obj + 0xC);
  std::memcpy(hp2_ptr(obj + 0xC), hp2_ptr(def), 0xF0);
  if (ph) hp2_wr32(ph, obj);
  if (g_cfg.shader_check) {
    PsKey k;
    k.combiners = true;
    for (uint32_t i = 0; i <= RS_PSINPUTTEXTURE; i++) k.words[i] = hp2_rd32(def + 4 * i);
    k.words[57] = hp2_rd32(def + 4 * 54);
    k.words[58] = 1;
    k.nwords = 60;
    for (int s = 0; s < 4; s++) k.tex_kind[s] = ((k.words[57] >> (5 * s)) & 0x1F) ? 1 : 0;
    hp2_log("d3d8: shader check: pixel shader %08X %s", def,
            FragmentProgram(k) ? "compiled" : "FAILED");
  }
  hp2_log("d3d8: CreatePixelShader %08X: combiners %08X texmodes %08X -> %08X", def,
          hp2_rd32(def + 4 * RS_PSCOMBINERCOUNT), hp2_rd32(def + 4 * 54), obj);
  hp2_ret_stdcall(c, 0, 2);
}

// 0x133AD0 SetPixelShader(Handle): copies the definition into the render states.
void SetPixelShader(Hp2Ctx& c) {
  Seen("SetPixelShader");
  uint32_t h = hp2_arg(c, 0);
  g_dev.ps = h;
  if (h) {
    uint32_t def = hp2_rd32(h + 8);
    for (uint32_t i = 0; i <= RS_PSINPUTTEXTURE; i++) SetRS(i, hp2_rd32(def + 4 * i));
    SetRS(RS_PSTEXTUREMODES, hp2_rd32(def + 4 * 54));
  }
  hp2_ret_stdcall(c, 0, 1);
}

// 0x133CD0 SetPixelShaderConstant(Register, pConstantData, ConstantCount): packs each
// float4 to D3DCOLOR and stores it in every combiner constant slot the
// current shader maps to that register (PSC0Mapping/PSC1Mapping/final).
void SetPixelShaderConstant(Hp2Ctx& c) {
  Seen("SetPixelShaderConstant");
  uint32_t reg = hp2_arg(c, 0), data = hp2_arg(c, 1), n = hp2_arg(c, 2);
  if (g_dev.ps) {
    uint32_t def = hp2_rd32(g_dev.ps + 8);
    uint32_t m0 = hp2_rd32(def + 0xE4), m1 = hp2_rd32(def + 0xE8), mf = hp2_rd32(def + 0xEC);
    for (uint32_t k = 0; k < n; k++, reg++) {
      float v[4];
      std::memcpy(v, hp2_ptr(data + 16 * k), 16);
      auto ch = [](float f) { return uint32_t(std::lround(std::clamp(f, 0.f, 1.f) * 255.f)); };
      uint32_t argb = ch(v[3]) << 24 | ch(v[0]) << 16 | ch(v[1]) << 8 | ch(v[2]);
      for (uint32_t i = 0; i < 8; i++) {
        if (((m0 >> (4 * i)) & 0xF) == reg) SetRS(RS_PSCONSTANT0_0 + i, argb);
        if (((m1 >> (4 * i)) & 0xF) == reg) SetRS(RS_PSCONSTANT1_0 + i, argb);
      }
      if ((mf & 0xF) == reg) SetRS(RS_PSFINALCOMBINERCONSTANT0, argb);
      if (((mf >> 4) & 0xF) == reg) SetRS(RS_PSFINALCOMBINERCONSTANT1, argb);
    }
  }
  hp2_ret_stdcall(c, 0, 3);
}

// 0x134120 / 0x134160 Resource_AddRef / Release(pThis)
void Resource_Release(Hp2Ctx& c) {
  Seen("Resource_Release");
  uint32_t r = hp2_arg(c, 0);
  uint32_t common = hp2_rd32(r);
  uint32_t ref = common & COMMON_REFCOUNT_MASK;
  if (ref <= 1) {
    hp2_wr32(r, common & ~COMMON_REFCOUNT_MASK);
    uint32_t type = common & COMMON_TYPE_MASK;
    if (type == COMMON_TYPE_SURFACE) {
      uint32_t parent = hp2_rd32(r + 0x14);
      if (parent) {
        uint32_t pc = hp2_rd32(parent);
        if ((pc & COMMON_REFCOUNT_MASK) > 1)
          hp2_wr32(parent, pc - 1);
      }
    }
    // Free D3D-created data (not surfaces of a parent texture, not the
    // device's own buffers) once the GPU is surely done with it.
    bool own = (common & COMMON_D3DCREATED) && !(type == COMMON_TYPE_SURFACE && hp2_rd32(r + 0x14));
    if (own && r != g_dev.back && r != g_dev.front && r != g_dev.zbuf) {
      uint32_t data = hp2_rd32(r + 4);
      if (data) {
        for (bool dep : {false, true})
          if (Target* t = FindTarget(data, dep)) {
            gpu::DestroyTexture(t->opaque);
            gpu::DestroyTexture(t->tex);
            g_targets.erase(uint64_t(data) << 1 | dep);
          }
        g_dev.pending_free.push_back({CpuAddr(data), g_dev.frame + 4});
        hp2_wr32(r + 4, 0);
      }
    }
    hp2_ret_stdcall(c, 0, 1);
    return;
  }
  hp2_wr32(r, common - 1);
  hp2_ret_stdcall(c, ref - 1, 1);
}

// 0x134240 Resource_Register(pThis, pBase): Data += pBase (physical, except push buffers)
void Resource_Register(Hp2Ctx& c) {
  Seen("Resource_Register");
  uint32_t r = hp2_arg(c, 0), base = hp2_arg(c, 1);
  uint32_t d = hp2_rd32(r + 4) + base;
  if ((hp2_rd32(r) & COMMON_TYPE_MASK) != COMMON_TYPE_PUSHBUFFER) d &= 0x0FFFFFFF;
  hp2_wr32(r + 4, d);
  hp2_ret_stdcall(c, 0, 2);
}

// 0x134270 Resource_BlockUntilNotBusy(pThis): draws copy their data, nothing to wait for.
void Resource_BlockUntilNotBusy(Hp2Ctx& c) { hp2_ret_stdcall(c, 0, 1); }

// 0x134D70 Swap(Flags)
void Swap(Hp2Ctx& c) {
  Seen("Swap");
  g_dev.frame++;
  bool shot = (g_cfg.shot_every && g_dev.frame % g_cfg.shot_every == 0) ||
              g_cfg.shot_frames.count(g_dev.frame);
  Present(g_dev.back);
  if (shot) Screenshot(g_dev.back, "frame", g_dev.frame);
  if (shot) ScreenshotPresent(g_dev.frame);
  // Dev: HP2_DEV_DUMP_TARGETS=<frame> writes every colour render target.
  static const uint64_t dump_rt = [] {
    const char* v = getenv("HP2_DEV_DUMP_TARGETS");
    return v ? strtoull(v, nullptr, 10) : ~0ull;
  }();
  if (g_dev.frame == dump_rt)
    for (auto& [k, t] : g_targets) {
      if (t.depth) continue;
      std::vector<uint8_t> px;
      if (gpu::ReadPixels(t.tex, t.pw, t.ph, &px)) {
        char path[600];
        std::snprintf(path, sizeof path, "%s/target_%08X_%ux%u.png", g_cfg.shot_dir.c_str(), t.data,
                      t.w, t.h);
        gpu::WritePng(path, t.pw, t.ph, px.data());
      }
    }
  // Present = the back buffer becomes the front buffer.
  if (g_dev.back && g_dev.front) {
    uint32_t b = hp2_rd32(g_dev.back + 4), f = hp2_rd32(g_dev.front + 4);
    hp2_wr32(g_dev.back + 4, f);
    hp2_wr32(g_dev.front + 4, b);
  }
  gpu::EndFrame();
  FreePending(c, false);
  // 60.000 Hz: at most one Swap per vblank (PresentationInterval 1).
  auto wait0 = std::chrono::steady_clock::now();
  if (g_cfg.pacing) g_dev.last_vblank = hp2_vblank_wait(g_dev.last_vblank);
  double wait_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wait0).count();
  auto now = std::chrono::steady_clock::now();
  double ms = std::chrono::duration<double, std::milli>(now - g_dev.last_swap).count();
  g_dev.last_swap = now;
  // A movie (XMV overlay frames, no Swap) between two Swaps is not a stall:
  // report it apart and keep it out of the Swap pacing statistics.
  uint64_t movie_frames = g_dev.overlay_frames - g_dev.overlay_at_swap;
  g_dev.overlay_at_swap = g_dev.overlay_frames;
  if (movie_frames) {
    hp2_log("d3d8: frame %llu: movie in between (%llu overlay frames, %.1f s)",
            (unsigned long long)g_dev.frame, (unsigned long long)movie_frames, ms / 1000);
  } else {
    double gpu_wait = gpu::TakeGpuWaitMs();
    if (ms > 25.0 && g_dev.frame > 3)
      hp2_log("d3d8: slow frame %llu: %.1f ms (%u draws, %.1f ms waiting for the GPU)",
              (unsigned long long)g_dev.frame, ms, g_dev.frame_draws, gpu_wait);
    g_dev.frame_ms_sum += ms;
    g_dev.frame_busy_sum += ms - wait_ms;
    g_dev.frame_ms_max = std::max(g_dev.frame_ms_max, ms);
    g_dev.frame_ms_n++;
  }
  if (g_dev.frame_ms_n == 120) {
    // busy = frame time minus the vblank wait (the host's own cost per frame).
    hp2_log("d3d8: frame %llu: %.2f ms avg, %.2f ms max over 120 frames (busy %.2f ms); %u draws "
            "last frame, %zu textures, %zu vs, %zu targets",
            (unsigned long long)g_dev.frame, g_dev.frame_ms_sum / 120, g_dev.frame_ms_max,
            g_dev.frame_busy_sum / 120,
            g_dev.frame_draws, g_texcache.size(), g_vshaders.size(), g_targets.size());
    g_dev.frame_ms_sum = g_dev.frame_ms_max = g_dev.frame_busy_sum = 0;
    g_dev.frame_ms_n = 0;
  }
  if (g_dev.frame <= 3 || g_cfg.trace) hp2_log("d3d8: Swap %llu (%u draws)", (unsigned long long)g_dev.frame, g_dev.frame_draws);
  g_dev.frame_draws = 0;
  if (g_cfg.exit_after && g_dev.frame >= g_cfg.exit_after) {
    gpu::EndFrame();
    JoinShotWriters();
    hp2_exit(0, "HP2_EXIT_AFTER frames presented");
  }
  hp2_ret_stdcall(c, 0, 1);
}

// 0x134F10 CreateTexture2(Width, Height, Depth, Levels, Usage, Format, D3DResourceType)
void CreateTexture2(Hp2Ctx& c) {
  Seen("CreateTexture2");
  uint32_t w = hp2_arg(c, 0), h = hp2_arg(c, 1), depth = hp2_arg(c, 2), levels = hp2_arg(c, 3);
  uint32_t usage = hp2_arg(c, 4), xfmt = hp2_arg(c, 5), rtype = hp2_arg(c, 6);
  bool volume = rtype == 4, cube = rtype == 5;
  uint32_t fmt, size;
  MakeTextureHeader(w, h, depth, levels, xfmt, cube, volume, &fmt, &size);
  TexLayout L;
  uint32_t tex = 0;
  if (DescribeTexture(fmt, size, &L)) {
    uint32_t data = AllocContiguous(c, L.total, 0x80);
    if (data) {
      tex = NewHeader(0x14);
      hp2_wr32(tex, COMMON_TYPE_TEXTURE | COMMON_D3DCREATED | 1);
      hp2_wr32(tex + 4, data & 0x0FFFFFFF);
      hp2_wr32(tex + 8, 0);
      hp2_wr32(tex + 0xC, fmt);
      hp2_wr32(tex + 0x10, size);
    }
  }
  hp2_log("d3d8: CreateTexture %ux%ux%u lv %u usage %X %s type %u -> %08X (%u bytes)", w, h,
          depth, levels, usage, FormatInfo(xfmt).name ? FormatInfo(xfmt).name : "?", rtype, tex,
          L.total);
  hp2_ret_stdcall(c, tex, 7);
}

// 0x134FD0 Texture_GetSurfaceLevel2(pThis, Level) -> surface (AddRef'd)
void Texture_GetSurfaceLevel2(Hp2Ctx& c) {
  Seen("Texture_GetSurfaceLevel2");
  uint32_t tex = hp2_arg(c, 0), lv = hp2_arg(c, 1);
  Hdr T{tex};
  TexLayout L;
  uint32_t s = 0;
  if (DescribeTexture(T.format(), T.size(), &L) && lv < L.levels) {
    uint32_t w = std::max(1u, L.w >> lv), h = std::max(1u, L.h >> lv);
    uint32_t pitch = L.linear ? L.pitch : w * L.bpp / 8;
    // The game asks for render-target / reflection levels every frame: one
    // header per (texture, level), reused (the HLE region is a bump
    // allocator; a new header per call filled it in ~20 min). Texture headers
    // are never reused, so the key stays valid. Still held -> AddRef'd.
    static std::unordered_map<uint64_t, uint32_t> cache;
    static uint64_t calls = 0;
    if (++calls % 20000 == 0)
      hp2_log("d3d8: GetSurfaceLevel: %llu calls, %zu surface headers", (unsigned long long)calls,
              cache.size());
    uint32_t& slot = cache[uint64_t(tex) << 8 | lv];
    if (slot && (hp2_rd32(slot) & COMMON_REFCOUNT_MASK)) {
      AddRef(slot);
      hp2_ret_stdcall(c, slot, 2);
      return;
    }
    if (!slot) slot = NewHeader(0x20);
    s = slot;
    hp2_wr32(s, COMMON_TYPE_SURFACE | COMMON_D3DCREATED | 1);
    hp2_wr32(s + 4, T.data() + L.level_offset[lv]);
    hp2_wr32(s + 8, 0);
    uint32_t fmt = (T.format() & ~0xFFFF0000u & ~FMT_CUBEMAP) | 1u << 16;
    hp2_wr32(s + 0xC, fmt);
    hp2_wr32(s + 0x10, (w - 1) | (h - 1) << 12 | ((pitch + 63) / 64 - 1) << 24);
    hp2_wr32(s + 0x14, tex);
    AddRef(tex);
  }
  hp2_ret_stdcall(c, s, 2);
}

// 0x135020 Texture_LockRect(pThis, Level, pLockedRect, pRect, Flags)
void Texture_LockRect(Hp2Ctx& c) {
  Seen("Texture_LockRect");
  uint32_t tex = hp2_arg(c, 0), lv = hp2_arg(c, 1), out = hp2_arg(c, 2), rect = hp2_arg(c, 3);
  Hdr T{tex};
  TexLayout L;
  uint32_t pitch = 0, bits = 0;
  if (DescribeTexture(T.format(), T.size(), &L) && lv < L.levels) {
    uint32_t w = std::max(1u, L.w >> lv);
    pitch = L.linear ? L.pitch : L.compressed ? ((w + 3) / 4) * (L.xfmt == XF_DXT1 ? 8 : 16)
                                              : w * L.bpp / 8;
    bits = CpuAddr(T.data() + L.level_offset[lv]);
    g_tex_locked[T.data()] = g_dev.frame;
    if (rect && L.linear) {
      bits += hp2_rd32(rect + 4) * pitch + hp2_rd32(rect) * (L.bpp / 8);
    }
  }
  hp2_wr32(out, pitch);
  hp2_wr32(out + 4, bits);
  hp2_ret_stdcall(c, 0, 5);
}

// 0x135050 Clear(Count, pRects, Flags, Color, Z, Stencil)
void Clear(Hp2Ctx& c) {
  Seen("Clear");
  uint32_t n = hp2_arg(c, 0), rects = hp2_arg(c, 1), flags = hp2_arg(c, 2), color = hp2_arg(c, 3);
  float z = hp2_argf(c, 4);
  uint32_t st = hp2_arg(c, 5);
  TargetsNow tg = BindTargets();
  std::vector<int32_t> r;
  float kx = tg.w ? float(tg.pw) / tg.w : float(g_cfg.scale);
  for (uint32_t i = 0; i < n && rects; i++)
    for (int k = 0; k < 4; k++) {
      int32_t v = int32_t(hp2_rd32(rects + 16 * i + 4 * k));
      r.push_back(k & 1 ? v * int32_t(g_cfg.scale) : int32_t(v * kx + 0.5f));
    }
  gpu::Clear((flags & 0xF0) != 0, flags & 1, flags & 2, color, z, st, r.data(),
             uint32_t(r.size() / 4));
  TRACE("d3d8: Clear %u flags %X color %08X z %f", n, flags, color, z);
  (void)tg;
  hp2_ret_stdcall(c, 0, 6);
}

// 0x135760 InsertCallback(Type, pCallback, Context): the GPU is always done here, so
// the callback runs right away (D3DCALLBACK: void __cdecl cb(DWORD Context)).
void InsertCallback(Hp2Ctx& c) {
  Seen("InsertCallback");
  uint32_t cb = hp2_arg(c, 1), ctx = hp2_arg(c, 2);
  if (cb) {
    uint32_t args[1] = {ctx};
    hp2_call_guest(c, cb, args, 1);
  }
  hp2_ret_stdcall(c, 0, 3);
}

// 0x135E60 DrawVerticesUP(PrimitiveType, VertexCount, pVertexStreamZeroData, Stride)
void DrawVerticesUP(Hp2Ctx& c) {
  Seen("DrawVerticesUP");
  uint32_t prim = hp2_arg(c, 0), n = hp2_arg(c, 1), data = hp2_arg(c, 2), stride = hp2_arg(c, 3);
  StreamSrc ss[16];
  ss[0].va = data;
  ss[0].stride = stride;
  DrawCommon(prim, ss, 0, n, nullptr, 0);
  hp2_ret_stdcall(c, 0, 4);
}

// 0x135FC0 DrawIndexedVerticesUP(PrimitiveType, VertexCount, pIndexData, pVertexStreamZeroData, Stride)
void DrawIndexedVerticesUP(Hp2Ctx& c) {
  Seen("DrawIndexedVerticesUP");
  uint32_t prim = hp2_arg(c, 0), n = hp2_arg(c, 1), idx = hp2_arg(c, 2), data = hp2_arg(c, 3),
           stride = hp2_arg(c, 4);
  StreamSrc ss[16];
  ss[0].va = data;
  ss[0].stride = stride;
  const uint16_t* ip = IndexPtr(idx, n);
  if (ip) DrawIndexed(prim, n, ip, ss, 0);
  hp2_ret_stdcall(c, 0, 5);
}

// 0x136100 DrawVertices(PrimitiveType, StartVertex, VertexCount)
void DrawVertices(Hp2Ctx& c) {
  Seen("DrawVertices");
  uint32_t prim = hp2_arg(c, 0), start = hp2_arg(c, 1), n = hp2_arg(c, 2);
  DrawFromStreams(prim, start, n, nullptr, 0);
  hp2_ret_stdcall(c, 0, 3);
}

// 0x1361A0 DrawIndexedVertices(PrimitiveType, VertexCount, pIndexData)
void DrawIndexedVertices(Hp2Ctx& c) {
  Seen("DrawIndexedVertices");
  uint32_t prim = hp2_arg(c, 0), n = hp2_arg(c, 1), idx = hp2_arg(c, 2);
  StreamSrc ss[16];
  for (int s = 0; s < 16; s++) {
    uint32_t vb = g_dev.streams[s].vb;
    if (!vb) continue;
    ss[s].va = CpuAddr(Hdr{vb}.data());
    ss[s].stride = g_dev.streams[s].stride;
  }
  const uint16_t* ip = IndexPtr(idx, n);
  if (ip) DrawIndexed(prim, n, ip, ss, g_dev.idx_base);
  hp2_ret_stdcall(c, 0, 3);
}

// 0x136520 UpdateOverlay(pSurface, SrcRect, DstRect, EnableColorKey, ColorKey): the XMV
// player's frames (a YUY2 surface in CPU memory). The NV2A scans the overlay
// out over the frame buffer; here it is drawn into the displayed buffer (the
// front buffer: the movie loop does not Swap), which then counts as a shown
// frame for screenshots. The colour key (UI over video) is not supported.

void UpdateOverlay(Hp2Ctx& c) {
  Seen("UpdateOverlay");
  auto t_in = std::chrono::steady_clock::now();
  if (g_ov.n) {
    double gap = std::chrono::duration<double, std::milli>(t_in - g_ov.last).count();
    g_ov.gap_ms += gap;
    g_ov.gap_max = std::max(g_ov.gap_max, gap);
  }
  uint32_t surf = hp2_arg(c, 0), sr = hp2_arg(c, 1), dr = hp2_arg(c, 2);
  Hdr H{surf};
  TexLayout L;
  uint32_t cpu = CpuAddr(H.data());
  if (surf && g_dev.front && DescribeTexture(H.format(), H.size(), &L) && hp2_is_committed(cpu) &&
      hp2_is_committed(cpu + L.total - 1)) {
    std::vector<uint8_t> buf;
    uint32_t pitch;
    ConvertLevel(L, 0, static_cast<const uint8_t*>(hp2_ptr(cpu)), &buf, &pitch);
    gpu::TexId t = gpu::CreateTexture(gpu::TexKind::BGRA8, L.w, L.h, 1, false, false);
    gpu::UploadTexture(t, 0, 0, L.w, L.h, buf.data(), pitch);
    auto rect = [](uint32_t r, int32_t def_w, int32_t def_h, int32_t* o) {
      if (r)
        for (int i = 0; i < 4; i++) o[i] = int32_t(hp2_rd32(r + 4 * i));
      else
        o[0] = o[1] = 0, o[2] = def_w, o[3] = def_h;
    };
    int32_t s4[4], d4[4];
    rect(sr, int32_t(L.w), int32_t(L.h), s4);
    rect(dr, int32_t(g_dev.bb_w), int32_t(g_dev.bb_h), d4);
    Target* f = GetTarget(Hdr{g_dev.front}.data(), g_dev.bb_w, g_dev.bb_h, false);
    int32_t k = int32_t(g_cfg.scale);
    // Movies keep their 4:3 shape: on a wide screen they are centred
    // (pillarboxed: clear the bars, the game does not draw there).
    int32_t pad = (int32_t(f->pw) - int32_t(f->w) * k) / 2;
    if (pad > 0) {
      gpu::SetTargets(f->tex, 0, f->pw, f->ph);
      gpu::Clear(true, false, false, 0xFF000000, 1, 0, nullptr, 0);
    }
    gpu::CopyRegion(t, s4[0], s4[1], s4[2] - s4[0], s4[3] - s4[1], f->tex, pad + d4[0] * k,
                    d4[1] * k, (d4[2] - d4[0]) * k, (d4[3] - d4[1]) * k);
    gpu::DestroyTexture(t);
    Present(g_dev.front);
    gpu::EndFrame();
    g_dev.overlay_frames++;
    if (g_cfg.shot_every && g_dev.overlay_frames % g_cfg.shot_every == 0)
      Screenshot(g_dev.front, "overlay", g_dev.overlay_frames);
    TRACE("d3d8: UpdateOverlay %08X %s %ux%u", surf, FormatInfo(L.xfmt).name, L.w, L.h);
  }
  g_ov.last = std::chrono::steady_clock::now();
  if (g_ov.movie_frames++ == 0)
    hp2_log("d3d8: movie: first overlay frame %.3f s after PlayMovie",
            std::chrono::duration<double>(g_ov.last - g_ov.movie_start).count());
  g_ov.in_ms += std::chrono::duration<double, std::milli>(g_ov.last - t_in).count();
  if (++g_ov.n % 300 == 0)
    hp2_log("d3d8: overlay %u frames: %.2f ms in UpdateOverlay, %.2f ms in vblank waits, "
            "%.2f ms between frames (max %.1f) on average",
            g_ov.n, g_ov.in_ms / 300, g_ov.wait_ms / 300, g_ov.gap_ms / 300, g_ov.gap_max),
        g_ov.in_ms = g_ov.wait_ms = g_ov.gap_ms = g_ov.gap_max = 0;
  hp2_ret_stdcall(c, 0, 5);
}

// 0x1366A0 EnableOverlay(Enable)
void EnableOverlay(Hp2Ctx& c) {
  Seen("EnableOverlay");
  hp2_ret_stdcall(c, 0, 1);
}

// ---- used by the XMV player ------------------------------------------------------------
// 0x133260 GetDisplayMode(pMode): D3DDISPLAYMODE {Width, Height, RefreshRate, Flags, Format}
void GetDisplayMode(Hp2Ctx& c) {
  Seen("GetDisplayMode");
  uint32_t p = hp2_arg(c, 0);
  hp2_wr32(p, g_dev.bb_w);
  hp2_wr32(p + 4, g_dev.bb_h);
  hp2_wr32(p + 8, 60);
  hp2_wr32(p + 12, 0);
  hp2_wr32(p + 16, XF_LIN_A8R8G8B8);
  hp2_ret_stdcall(c, 0, 1);
}

// 0x133790 GetDisplayFieldStatus(pFieldStatus): {Field, VBlankCount}
void GetDisplayFieldStatus(Hp2Ctx& c) {
  Seen("GetDisplayFieldStatus");
  uint32_t p = hp2_arg(c, 0);
  hp2_wr32(p, 3 /* D3DFIELD_PROGRESSIVE */);
  hp2_wr32(p + 4, uint32_t(hp2_vblank_count()));
  hp2_ret_stdcall(c, 0, 1);
}

// 0x13A5E0 Surface_GetDesc(pThis, pDesc): D3DSURFACE_DESC {Format, Type, Usage, Size,
// MultiSampleType, Width, Height}
void Surface_GetDesc(Hp2Ctx& c) {
  Seen("Surface_GetDesc");
  uint32_t s = hp2_arg(c, 0), p = hp2_arg(c, 1);
  Hdr H{s};
  TexLayout L;
  DescribeTexture(H.format(), H.size(), &L);
  hp2_wr32(p, L.xfmt);
  hp2_wr32(p + 4, 1 /* D3DRTYPE_SURFACE */);
  hp2_wr32(p + 8, 0);
  hp2_wr32(p + 12, L.total);
  hp2_wr32(p + 16, 0x11);
  hp2_wr32(p + 20, L.w);
  hp2_wr32(p + 24, L.h);
  hp2_ret_stdcall(c, 0, 2);
}

// 0x13A600 Surface_LockRect(pThis, pLockedRect, pRect, Flags)
void Surface_LockRect(Hp2Ctx& c) {
  Seen("Surface_LockRect");
  uint32_t s = hp2_arg(c, 0), out = hp2_arg(c, 1), rect = hp2_arg(c, 2);
  Hdr H{s};
  TexLayout L;
  DescribeTexture(H.format(), H.size(), &L);
  uint32_t pitch = L.linear ? L.pitch : L.w * L.bpp / 8;
  uint32_t bits = CpuAddr(H.data());
  g_tex_locked[H.data()] = g_dev.frame;
  if (uint32_t parent = hp2_rd32(s + 0x14)) g_tex_locked[Hdr{parent}.data()] = g_dev.frame;
  if (rect) bits += hp2_rd32(rect + 4) * pitch + hp2_rd32(rect) * (L.bpp / 8);
  hp2_wr32(out, pitch);
  hp2_wr32(out + 4, bits);
  hp2_ret_stdcall(c, 0, 4);
}

// ---- registration ------------------------------------------------------------------------
HP2_OVERRIDE(0x133FA0, Direct3DCreate8);
HP2_OVERRIDE(0x130620, SetPushBufferSize);
HP2_OVERRIDE(0x130640, CreateDevice);
HP2_OVERRIDE(0x130720, Push1);
HP2_OVERRIDE(0x130B00, SetRenderState_FogColor);
HP2_OVERRIDE(0x130B50, SetRenderState_CullMode);
HP2_OVERRIDE(0x130C00, SetRenderState_NormalizeNormals);
HP2_OVERRIDE(0x1310F0, SetTextureState_BumpEnv);
HP2_OVERRIDE(0x131160, SetTextureState_BorderColor);
HP2_OVERRIDE(0x131BD0, SetRenderState_ZEnable);
HP2_OVERRIDE(0x131C60, SetRenderState_StencilEnable);
HP2_OVERRIDE(0x132570, CreateVertexShader);
HP2_OVERRIDE(0x1326B0, SetVertexShaderConstant1);
HP2_OVERRIDE(0x132710, SetVertexShaderConstant4);
HP2_OVERRIDE(0x1328A0, SetVertexShaderConstantNotInline);
HP2_OVERRIDE(0x1328E0, SetStreamSource);
HP2_OVERRIDE(0x132A50, SetShaderConstantMode);
HP2_OVERRIDE(0x132CA0, SetVertexShader);
HP2_OVERRIDE(0x132E00, CreateVertexBuffer2);
HP2_OVERRIDE(0x132E50, VertexBuffer_Lock2);
HP2_OVERRIDE(0x132EA0, SetRenderTarget);
HP2_OVERRIDE(0x132EC0, SetTransform);
HP2_OVERRIDE(0x132FD0, SetMaterial);
HP2_OVERRIDE(0x133000, KickOffAndWaitForIdle);
HP2_OVERRIDE(0x133010, BlockUntilVerticalBlank);
HP2_OVERRIDE(0x133330, GetBackBuffer2);
HP2_OVERRIDE(0x133380, GetRenderTarget2);
HP2_OVERRIDE(0x1333A0, GetDepthStencilSurface2);
HP2_OVERRIDE(0x133520, SetTexture);
HP2_OVERRIDE(0x1336D0, SetIndices);
HP2_OVERRIDE(0x133A80, CreatePixelShader);
HP2_OVERRIDE(0x133AD0, SetPixelShader);
HP2_OVERRIDE(0x133CD0, SetPixelShaderConstant);
HP2_OVERRIDE(0x134160, Resource_Release);
HP2_OVERRIDE(0x134240, Resource_Register);
HP2_OVERRIDE(0x134270, Resource_BlockUntilNotBusy);
HP2_OVERRIDE(0x134D70, Swap);
HP2_OVERRIDE(0x134F10, CreateTexture2);
HP2_OVERRIDE(0x134FD0, Texture_GetSurfaceLevel2);
HP2_OVERRIDE(0x135020, Texture_LockRect);
HP2_OVERRIDE(0x135050, Clear);
HP2_OVERRIDE(0x135760, InsertCallback);
HP2_OVERRIDE(0x135E60, DrawVerticesUP);
HP2_OVERRIDE(0x135FC0, DrawIndexedVerticesUP);
HP2_OVERRIDE(0x136100, DrawVertices);
HP2_OVERRIDE(0x1361A0, DrawIndexedVertices);
HP2_OVERRIDE(0x136520, UpdateOverlay);
HP2_OVERRIDE(0x1366A0, EnableOverlay);
HP2_OVERRIDE(0x133260, GetDisplayMode);
HP2_OVERRIDE(0x133790, GetDisplayFieldStatus);
HP2_OVERRIDE(0x13A5E0, Surface_GetDesc);
HP2_OVERRIDE(0x13A600, Surface_LockRect);

}  // namespace

void hp2_d3d8_set_texture_hook(Hp2TextureHook hook) { g_tex_hook = hook; }

// movies.cpp: a movie starts / ends (timing log for audio/video sync).
void hp2_d3d8_movie_event(bool start) {
  auto now = std::chrono::steady_clock::now();
  if (start) {
    g_ov.movie_start = now;
    g_ov.movie_frames = 0;
  } else {
    hp2_log("d3d8: movie: %u overlay frames, last at %.3f s, PlayMovie returned at %.3f s",
            g_ov.movie_frames, std::chrono::duration<double>(g_ov.last - g_ov.movie_start).count(),
            std::chrono::duration<double>(now - g_ov.movie_start).count());
  }
}
uint64_t hp2_d3d8_frame_count() { return g_dev.frame; }

// The presented frame's aspect (4:3, or wider in widescreen); usable before
// CreateDevice (the window is created first): Polish's call or HP2_WIDESCREEN.
extern "C" float hp2_d3d8_output_aspect() {
  float wide = g_cfg.wide;
  if (wide <= 1.0f) {
    const char* v = getenv("HP2_WIDESCREEN");
    if (v && atoi(v)) wide = (16.0f / 10.0f) / (4.0f / 3.0f);
  }
  return 4.0f / 3.0f * wide;
}

// Widescreen (Polish, src/polish/widescreen.cpp): call before CreateDevice.
extern "C" void hp2_d3d8_set_widescreen(float aspect_scale) {
  g_cfg.wide = aspect_scale > 1.0f ? aspect_scale : 1.0f;
  hp2_log("d3d8: widescreen, screen targets x%.3f wide", g_cfg.wide);
}
