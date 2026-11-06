// Xbox texture layouts and conversion (see textures.h).
//
// Swizzled formats store each mip level in NV2A "Morton" order: the bits of
// x and y interleave (x first) while both still have bits, then the longer
// side's remaining bits follow. DXTn levels are plain BCn blocks. Linear
// (LIN_*) formats have one level with the pitch from the Size dword.

#include "hle/d3d8/textures.h"

#include <algorithm>
#include <cstring>

#include "hle/d3d8/xbox_d3d.h"
#include "runtime/hp2_runtime.h"

namespace hp2d3d {

namespace {

XFmtInfo kFmts[256];
bool kFmtsInit = [] {
  auto set = [](uint32_t f, uint8_t bpp, bool lin, bool comp, bool depth, const char* n) {
    kFmts[f] = XFmtInfo{bpp, lin, comp, depth, n};
  };
  set(XF_L8, 8, false, false, false, "L8");
  set(XF_AL8, 8, false, false, false, "AL8");
  set(XF_A1R5G5B5, 16, false, false, false, "A1R5G5B5");
  set(XF_X1R5G5B5, 16, false, false, false, "X1R5G5B5");
  set(XF_A4R4G4B4, 16, false, false, false, "A4R4G4B4");
  set(XF_R5G6B5, 16, false, false, false, "R5G6B5");
  set(XF_A8R8G8B8, 32, false, false, false, "A8R8G8B8");
  set(XF_X8R8G8B8, 32, false, false, false, "X8R8G8B8");
  set(XF_P8, 8, false, false, false, "P8");
  set(XF_DXT1, 4, false, true, false, "DXT1");
  set(XF_DXT3, 8, false, true, false, "DXT3");
  set(XF_DXT5, 8, false, true, false, "DXT5");
  set(XF_LIN_A1R5G5B5, 16, true, false, false, "LIN_A1R5G5B5");
  set(XF_LIN_R5G6B5, 16, true, false, false, "LIN_R5G6B5");
  set(XF_LIN_A8R8G8B8, 32, true, false, false, "LIN_A8R8G8B8");
  set(XF_LIN_L8, 8, true, false, false, "LIN_L8");
  set(XF_LIN_R8B8, 16, true, false, false, "LIN_R8B8");
  set(XF_LIN_G8B8, 16, true, false, false, "LIN_G8B8");
  set(XF_A8, 8, false, false, false, "A8");
  set(XF_A8L8, 16, false, false, false, "A8L8");
  set(XF_LIN_AL8, 8, true, false, false, "LIN_AL8");
  set(XF_LIN_X1R5G5B5, 16, true, false, false, "LIN_X1R5G5B5");
  set(XF_LIN_A4R4G4B4, 16, true, false, false, "LIN_A4R4G4B4");
  set(XF_LIN_X8R8G8B8, 32, true, false, false, "LIN_X8R8G8B8");
  set(XF_LIN_A8, 8, true, false, false, "LIN_A8");
  set(XF_LIN_A8L8, 16, true, false, false, "LIN_A8L8");
  set(XF_YUY2, 16, true, false, false, "YUY2");
  set(XF_UYVY, 16, true, false, false, "UYVY");
  set(XF_L6V5U5, 16, false, false, false, "L6V5U5");
  set(XF_V8U8, 16, false, false, false, "V8U8");
  set(XF_R8B8, 16, false, false, false, "R8B8");
  set(XF_D24S8, 32, false, false, true, "D24S8");
  set(XF_F24S8, 32, false, false, true, "F24S8");
  set(XF_D16, 16, false, false, true, "D16");
  set(XF_F16, 16, false, false, true, "F16");
  set(XF_LIN_D24S8, 32, true, false, true, "LIN_D24S8");
  set(XF_LIN_F24S8, 32, true, false, true, "LIN_F24S8");
  set(XF_LIN_D16, 16, true, false, true, "LIN_D16");
  set(XF_LIN_F16, 16, true, false, true, "LIN_F16");
  set(XF_L16, 16, false, false, false, "L16");
  set(XF_V16U16, 32, false, false, false, "V16U16");
  set(XF_LIN_L16, 16, true, false, false, "LIN_L16");
  set(XF_LIN_V16U16, 32, true, false, false, "LIN_V16U16");
  set(XF_LIN_L6V5U5, 16, true, false, false, "LIN_L6V5U5");
  set(XF_R5G5B5A1, 16, false, false, false, "R5G5B5A1");
  set(XF_R4G4B4A4, 16, false, false, false, "R4G4B4A4");
  set(XF_A8B8G8R8, 32, false, false, false, "A8B8G8R8");
  set(XF_B8G8R8A8, 32, false, false, false, "B8G8R8A8");
  set(XF_R8G8B8A8, 32, false, false, false, "R8G8B8A8");
  set(XF_LIN_R5G5B5A1, 16, true, false, false, "LIN_R5G5B5A1");
  set(XF_LIN_R4G4B4A4, 16, true, false, false, "LIN_R4G4B4A4");
  set(XF_LIN_A8B8G8R8, 32, true, false, false, "LIN_A8B8G8R8");
  set(XF_LIN_B8G8R8A8, 32, true, false, false, "LIN_B8G8R8A8");
  set(XF_LIN_R8G8B8A8, 32, true, false, false, "LIN_R8G8B8A8");
  return true;
}();

uint32_t Max1(uint32_t v) { return v ? v : 1; }

uint32_t LevelBytes(const TexLayout& L, uint32_t lv) {
  uint32_t w = Max1(L.w >> lv), h = Max1(L.h >> lv), d = Max1(L.depth >> lv);
  if (L.compressed) {
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    return bw * bh * (L.xfmt == XF_DXT1 ? 8 : 16) * d;
  }
  if (L.linear) return L.pitch * h;
  return w * h * d * L.bpp / 8;
}

// Bit masks of the swizzle pattern for a w x h level.
void SwizzleMasks(uint32_t w, uint32_t h, uint32_t* mx, uint32_t* my) {
  uint32_t x = 0, y = 0, bit = 1;
  for (uint32_t i = 1; i < w || i < h; i <<= 1) {
    if (i < w) x |= bit, bit <<= 1;
    if (i < h) y |= bit, bit <<= 1;
  }
  *mx = x, *my = y;
}

// Spreads the low bits of v over the set bits of mask (pdep).
uint32_t Deposit(uint32_t v, uint32_t mask) {
  uint32_t r = 0;
  for (uint32_t b = 1; mask; b <<= 1) {
    uint32_t low = mask & (~mask + 1);
    if (v & b) r |= low;
    mask &= mask - 1;
  }
  return r;
}

inline uint32_t Ex5(uint32_t v) { return (v << 3) | (v >> 2); }
inline uint32_t Ex6(uint32_t v) { return (v << 2) | (v >> 4); }
inline uint32_t Ex4(uint32_t v) { return v * 17; }
inline uint32_t Bgra(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
  return b | g << 8 | r << 16 | a << 24;  // memory order B G R A
}
inline uint8_t Clamp8(int v) { return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v); }

// One texel of an uncompressed format -> BGRA8 (little-endian dword).
uint32_t Texel(uint32_t f, const uint8_t* p) {
  uint32_t v16 = p[0] | uint32_t(p[1]) << 8;
  uint32_t v32 = v16 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
  switch (f) {
    case XF_A8R8G8B8: case XF_LIN_A8R8G8B8: return v32;
    case XF_X8R8G8B8: case XF_LIN_X8R8G8B8: return v32 | 0xFF000000u;
    case XF_A8B8G8R8: case XF_LIN_A8B8G8R8:
      return Bgra(p[0], p[1], p[2], p[3]);
    case XF_B8G8R8A8: case XF_LIN_B8G8R8A8:
      return Bgra(p[1], p[2], p[3], p[0]);
    case XF_R8G8B8A8: case XF_LIN_R8G8B8A8:
      return Bgra(p[3], p[2], p[1], p[0]);
    case XF_R5G6B5: case XF_LIN_R5G6B5:
      return Bgra(Ex5(v16 >> 11), Ex6((v16 >> 5) & 63), Ex5(v16 & 31), 255);
    case XF_X1R5G5B5: case XF_LIN_X1R5G5B5:
      return Bgra(Ex5((v16 >> 10) & 31), Ex5((v16 >> 5) & 31), Ex5(v16 & 31), 255);
    case XF_A1R5G5B5: case XF_LIN_A1R5G5B5:
      return Bgra(Ex5((v16 >> 10) & 31), Ex5((v16 >> 5) & 31), Ex5(v16 & 31),
                  (v16 & 0x8000) ? 255 : 0);
    case XF_R5G5B5A1: case XF_LIN_R5G5B5A1:
      return Bgra(Ex5(v16 >> 11), Ex5((v16 >> 6) & 31), Ex5((v16 >> 1) & 31),
                  (v16 & 1) ? 255 : 0);
    case XF_A4R4G4B4: case XF_LIN_A4R4G4B4:
      return Bgra(Ex4((v16 >> 8) & 15), Ex4((v16 >> 4) & 15), Ex4(v16 & 15), Ex4(v16 >> 12));
    case XF_R4G4B4A4: case XF_LIN_R4G4B4A4:
      return Bgra(Ex4(v16 >> 12), Ex4((v16 >> 8) & 15), Ex4((v16 >> 4) & 15), Ex4(v16 & 15));
    case XF_L8: case XF_LIN_L8: return Bgra(p[0], p[0], p[0], 255);
    case XF_AL8: case XF_LIN_AL8: return Bgra(p[0], p[0], p[0], p[0]);
    case XF_A8: case XF_LIN_A8: return Bgra(255, 255, 255, p[0]);
    case XF_P8: return Bgra(p[0], p[0], p[0], 255);  // no palette support yet
    case XF_A8L8: case XF_LIN_A8L8: return Bgra(p[0], p[0], p[0], p[1]);
    case XF_L16: case XF_LIN_L16: return Bgra(p[1], p[1], p[1], 255);
    // Signed formats are stored biased (+128) so the shader's *2-1 expands them.
    case XF_V8U8: case XF_LIN_G8B8:
      return Bgra(uint8_t(p[0] + 128), uint8_t(p[1] + 128), 0, 255);
    case XF_R8B8: case XF_LIN_R8B8: return Bgra(p[1], 0, p[0], 255);
    case XF_L6V5U5: case XF_LIN_L6V5U5:
      return Bgra(uint8_t(((v16 & 31) << 3) + 128), uint8_t((((v16 >> 5) & 31) << 3) + 128),
                  Ex6(v16 >> 10), 255);
    case XF_V16U16: case XF_LIN_V16U16:
      return Bgra(uint8_t(p[1] + 128), uint8_t(p[3] + 128), 0, 255);
  }
  return 0xFFFF00FFu;  // magenta: unknown format
}

}  // namespace

const XFmtInfo& FormatInfo(uint32_t xfmt) { return kFmts[xfmt & 0xFF]; }

uint32_t VsdtSize(uint32_t t) {
  switch (t) {
    case VSDT_FLOAT1: return 4;
    case VSDT_FLOAT2: return 8;
    case VSDT_FLOAT3: return 12;
    case VSDT_FLOAT4: return 16;
    case VSDT_D3DCOLOR: return 4;
    case VSDT_SHORT2: return 4;
    case VSDT_SHORT4: return 8;
    case VSDT_NORMSHORT1: return 2;
    case VSDT_NORMSHORT2: return 4;
    case VSDT_NORMSHORT3: return 6;
    case VSDT_NORMSHORT4: return 8;
    case VSDT_NORMPACKED3: return 4;
    case VSDT_SHORT1: return 2;
    case VSDT_SHORT3: return 6;
    case VSDT_PBYTE1: return 1;
    case VSDT_PBYTE2: return 2;
    case VSDT_PBYTE3: return 3;
    case VSDT_PBYTE4: return 4;
    case VSDT_FLOAT2H: return 12;
    case VSDT_NONE: return 0;
  }
  return 0;
}

bool DescribeTexture(uint32_t format, uint32_t size, TexLayout* L) {
  *L = TexLayout{};
  L->xfmt = FmtFormat(format);
  const XFmtInfo& fi = FormatInfo(L->xfmt);
  if (!fi.bpp) return false;
  L->bpp = fi.bpp;
  L->linear = fi.linear;
  L->compressed = fi.compressed;
  L->cube = format & FMT_CUBEMAP;
  if (size) {  // linear layout from the Size dword
    L->w = SizeWidth(size);
    L->h = SizeHeight(size);
    L->pitch = SizePitch(size);
    L->levels = 1;
    L->linear = true;
  } else {
    L->w = 1u << FmtUSizeLog(format);
    L->h = 1u << FmtVSizeLog(format);
    L->depth = FmtDims(format) == 3 ? 1u << FmtPSizeLog(format) : 1;
    L->levels = Max1(FmtLevels(format));
    if (L->linear) L->pitch = L->w * L->bpp / 8;
  }
  uint32_t off = 0;
  for (uint32_t lv = 0; lv < L->levels && lv < 16; lv++) {
    L->level_offset[lv] = off;
    off += LevelBytes(*L, lv);
  }
  if (L->cube) {
    L->face_size = (off + 127) & ~127u;
    L->total = L->face_size * 6;
  } else {
    L->face_size = off;
    L->total = off;
  }
  return true;
}

void MakeTextureHeader(uint32_t w, uint32_t h, uint32_t depth, uint32_t levels, uint32_t xfmt,
                       bool cube, bool volume, uint32_t* format, uint32_t* size) {
  const XFmtInfo& fi = FormatInfo(xfmt);
  auto log2 = [](uint32_t v) {
    uint32_t l = 0;
    while ((1u << l) < v) l++;
    return l;
  };
  uint32_t f = 1 /* DMA A */ | (cube ? FMT_CUBEMAP : 0) | (volume ? 3u : 2u) << 4 | xfmt << 8;
  if (fi.linear) {
    uint32_t pitch = ((w * fi.bpp / 8) + 63) & ~63u;
    *format = f | 1u << 16;
    *size = (w - 1) | (h - 1) << 12 | (pitch / 64 - 1) << 24;
    return;
  }
  uint32_t lw = log2(w), lh = log2(h), ld = volume ? log2(depth) : 0;
  uint32_t max_levels = std::max(std::max(lw, lh), ld) + 1;
  if (levels == 0 || levels > max_levels) levels = max_levels;
  *format = f | levels << 16 | lw << 20 | lh << 24 | ld << 28;
  *size = 0;
}

gpu::TexKind HostKind(uint32_t xfmt) {
  switch (xfmt) {
    case XF_DXT1: return gpu::TexKind::BC1;
    case XF_DXT3: return gpu::TexKind::BC2;
    case XF_DXT5: return gpu::TexKind::BC3;
  }
  return gpu::TexKind::BGRA8;
}

void ConvertLevel(const TexLayout& L, uint32_t lv, const uint8_t* src, std::vector<uint8_t>* out,
                  uint32_t* out_pitch) {
  uint32_t w = Max1(L.w >> lv), h = Max1(L.h >> lv);
  if (L.compressed) {
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, bs = L.xfmt == XF_DXT1 ? 8 : 16;
    out->assign(src, src + size_t(bw) * bh * bs);
    *out_pitch = bw * bs;
    return;
  }
  out->resize(size_t(w) * h * 4);
  *out_pitch = w * 4;
  uint32_t* o = reinterpret_cast<uint32_t*>(out->data());
  uint32_t bpp = L.bpp / 8;
  if (L.xfmt == XF_YUY2 || L.xfmt == XF_UYVY) {
    bool uyvy = L.xfmt == XF_UYVY;
    for (uint32_t y = 0; y < h; y++) {
      const uint8_t* row = src + size_t(y) * L.pitch;
      for (uint32_t x = 0; x < w; x += 2) {
        const uint8_t* q = row + x * 2;
        int y0 = uyvy ? q[1] : q[0], u = uyvy ? q[0] : q[1];
        int y1 = uyvy ? q[3] : q[2], v = uyvy ? q[2] : q[3];
        for (int k = 0; k < 2 && x + k < w; k++) {
          int c = (k ? y1 : y0) - 16, d = u - 128, e = v - 128;
          int r = (298 * c + 409 * e + 128) >> 8, g = (298 * c - 100 * d - 208 * e + 128) >> 8,
              b = (298 * c + 516 * d + 128) >> 8;
          o[size_t(y) * w + x + k] = Bgra(Clamp8(r), Clamp8(g), Clamp8(b), 255);
        }
      }
    }
    return;
  }
  if (L.linear) {
    for (uint32_t y = 0; y < h; y++) {
      const uint8_t* row = src + size_t(y) * L.pitch;
      for (uint32_t x = 0; x < w; x++) o[size_t(y) * w + x] = Texel(L.xfmt, row + x * bpp);
    }
    return;
  }
  uint32_t mx, my;
  SwizzleMasks(w, h, &mx, &my);
  std::vector<uint32_t> xo(w);
  for (uint32_t x = 0; x < w; x++) xo[x] = Deposit(x, mx);
  for (uint32_t y = 0; y < h; y++) {
    uint32_t yo = Deposit(y, my);
    for (uint32_t x = 0; x < w; x++)
      o[size_t(y) * w + x] = Texel(L.xfmt, src + size_t(xo[x] | yo) * bpp);
  }
}

uint64_t HashBytes(const void* p, size_t n) {
  // 64-bit multiply-xorshift over 8-byte words (fast, not cryptographic).
  const uint8_t* b = static_cast<const uint8_t*>(p);
  uint64_t h = 0x9E3779B97F4A7C15ull ^ (n * 0xBF58476D1CE4E5B9ull);
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    uint64_t v;
    std::memcpy(&v, b + i, 8);
    h = (h ^ v) * 0x94D049BB133111EBull;
    h ^= h >> 29;
  }
  for (; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ull;
  h ^= h >> 32;
  return h;
}

}  // namespace hp2d3d
