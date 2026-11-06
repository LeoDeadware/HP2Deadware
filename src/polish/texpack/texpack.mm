// HD UI texture pack (Polish): replaces game textures by content hash with
// larger PNGs of the same aspect (the D3D8 HLE's texture hook; UVs are
// normalised, so a k-times larger image maps 1:1 and nothing shifts).
//
//   HP2_HDUI=dir    the UI pack (default $HP2_DATA/mods/hd_ui when it exists,
//                   except in HP2_BACKGROUND=1 test runs; 0 = off). Files
//                   <hash16>.png; <hash16> is the hook's key.
//   HP2_HDWORLD=dir the world pack (walls, floors...; tools/hdworld), the
//                   same rules with $HP2_DATA/mods/hd_world. A hash is looked
//                   up in the UI pack first.
//   HP2_HDUI_DUMP=dir   writes level 0 of every DXT and swizzled A8R8G8B8 /
//                   X8R8G8B8 texture the game uploads
//                   as <hash16>.png plus a line "hash w h format" in
//                   dir/index.tsv (once per hash): the input of
//                   tools/hdui/build_pack.py.
//
// A pack image must be an integer multiple of the original size (same
// aspect); anything else is refused with a log line and the original kept.

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>

#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "hle/d3d8/d3d8.h"
#include "polish/texpack/texpack.h"
#include "runtime/hp2_runtime.h"

// D3D8 HLE (Render). Weak: without it there is nothing to replace.
void hp2_d3d8_set_texture_hook(Hp2TextureHook hook) __attribute__((weak));

namespace {

constexpr uint32_t kDxt1 = 0x0C, kDxt3 = 0x0E, kDxt5 = 0x0F;  // Xbox formats

struct Image {
  uint32_t w = 0, h = 0;
  std::vector<uint8_t> rgba;
};

std::vector<std::string> g_packs;
std::string g_dump;
size_t g_loaded_bytes = 0;  // pack images uploaded (VRAM, +1/3 mips)
std::mutex g_mu;
std::map<uint64_t, Image> g_cache;  // probe images (kept: the hook returns pointers into them)
std::set<uint64_t> g_missing, g_dumped, g_used;

bool Exists(const std::string& p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0;
}

bool LoadPng(const std::string& path, Image* out) {
  @autoreleasepool {
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
    CGImageSourceRef src = CGImageSourceCreateWithURL((__bridge CFURLRef)url, nullptr);
    if (!src) return false;
    CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
    CFRelease(src);
    if (!img) return false;
    out->w = uint32_t(CGImageGetWidth(img)), out->h = uint32_t(CGImageGetHeight(img));
    out->rgba.assign(size_t(out->w) * out->h * 4, 0);
    // Straight (non-premultiplied) alpha as the game expects: draw into a
    // premultiplied context, then divide back.
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(out->rgba.data(), out->w, out->h, 8, out->w * 4, cs,
                                             kCGImageAlphaPremultipliedLast | kCGBitmapByteOrderDefault);
    CGColorSpaceRelease(cs);
    if (!ctx) {
      CGImageRelease(img);
      return false;
    }
    CGContextSetBlendMode(ctx, kCGBlendModeCopy);
    CGContextDrawImage(ctx, CGRectMake(0, 0, out->w, out->h), img);
    CGContextRelease(ctx);
    CGImageRelease(img);
    for (size_t i = 0; i < out->rgba.size(); i += 4) {
      uint32_t a = out->rgba[i + 3];
      if (a && a < 255)
        for (int c = 0; c < 3; c++) out->rgba[i + c] = uint8_t(std::min(255u, (out->rgba[i + c] * 255 + a / 2) / a));
    }
    return true;
  }
}

bool SavePng(const std::string& path, uint32_t w, uint32_t h, const uint8_t* rgba) {
  @autoreleasepool {
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGDataProviderRef dp = CGDataProviderCreateWithData(nullptr, rgba, size_t(w) * h * 4, nullptr);
    CGImageRef img = CGImageCreate(w, h, 8, 32, w * 4, cs, kCGImageAlphaLast | kCGBitmapByteOrderDefault,
                                   dp, nullptr, false, kCGRenderingIntentDefault);
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL((__bridge CFURLRef)url, CFSTR("public.png"), 1, nullptr);
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

// ---- BCn (DXT) level-0 decoding, for the dump
void Rgb565(uint16_t v, uint8_t* o) {
  o[0] = uint8_t(((v >> 11) & 31) * 255 / 31);
  o[1] = uint8_t(((v >> 5) & 63) * 255 / 63);
  o[2] = uint8_t((v & 31) * 255 / 31);
}

void ColorBlock(const uint8_t* b, bool dxt1, uint8_t pal[4][4]) {
  uint16_t c0 = uint16_t(b[0] | b[1] << 8), c1 = uint16_t(b[2] | b[3] << 8);
  Rgb565(c0, pal[0]), Rgb565(c1, pal[1]);
  pal[0][3] = pal[1][3] = 255;
  for (int c = 0; c < 3; c++) {
    if (c0 > c1 || !dxt1) {
      pal[2][c] = uint8_t((2 * pal[0][c] + pal[1][c]) / 3);
      pal[3][c] = uint8_t((pal[0][c] + 2 * pal[1][c]) / 3);
    } else {
      pal[2][c] = uint8_t((pal[0][c] + pal[1][c]) / 2);
      pal[3][c] = 0;
    }
  }
  pal[2][3] = 255;
  pal[3][3] = (c0 > c1 || !dxt1) ? 255 : 0;
}

bool DecodeDxt(uint32_t fmt, uint32_t w, uint32_t h, const uint8_t* src, size_t size, Image* out) {
  uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, bs = fmt == kDxt1 ? 8 : 16;
  if (size < size_t(bw) * bh * bs) return false;
  out->w = w, out->h = h;
  out->rgba.assign(size_t(w) * h * 4, 0);
  for (uint32_t by = 0; by < bh; by++)
    for (uint32_t bx = 0; bx < bw; bx++) {
      const uint8_t* b = src + (size_t(by) * bw + bx) * bs;
      const uint8_t* cb = fmt == kDxt1 ? b : b + 8;
      uint8_t pal[4][4];
      ColorBlock(cb, fmt == kDxt1, pal);
      uint8_t alpha[16];
      if (fmt == kDxt3) {
        for (int i = 0; i < 16; i++) alpha[i] = uint8_t(((b[i / 2] >> ((i & 1) * 4)) & 15) * 17);
      } else if (fmt == kDxt5) {
        uint8_t a[8] = {b[0], b[1]};
        for (int i = 2; i < 8; i++)
          a[i] = b[0] > b[1] ? uint8_t(((8 - i) * b[0] + (i - 1) * b[1]) / 7)
                 : i < 6     ? uint8_t(((6 - i) * b[0] + (i - 1) * b[1]) / 5)
                             : uint8_t(i == 6 ? 0 : 255);
        uint64_t bits = 0;
        for (int i = 0; i < 6; i++) bits |= uint64_t(b[2 + i]) << (8 * i);
        for (int i = 0; i < 16; i++) alpha[i] = a[(bits >> (3 * i)) & 7];
      }
      uint32_t idx = uint32_t(cb[4] | cb[5] << 8 | cb[6] << 16 | uint32_t(cb[7]) << 24);
      for (int i = 0; i < 16; i++) {
        uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
        if (x >= w || y >= h) continue;
        uint8_t* o = &out->rgba[(size_t(y) * w + x) * 4];
        const uint8_t* c = pal[(idx >> (2 * i)) & 3];
        o[0] = c[0], o[1] = c[1], o[2] = c[2];
        o[3] = fmt == kDxt1 ? c[3] : alpha[i];
      }
    }
  return true;
}

// Swizzled 32-bit ARGB (Xbox A8R8G8B8 / X8R8G8B8, power-of-two sizes): texel
// (x, y) at the interleaved bits of x and y (x first), the larger
// dimension's extra bits on top.
bool DecodeArgbSwizzled(uint32_t fmt, uint32_t w, uint32_t h, const uint8_t* src, size_t size, Image* out) {
  if (!w || !h || (w & (w - 1)) || (h & (h - 1)) || size < size_t(w) * h * 4) return false;
  out->w = w, out->h = h;
  out->rgba.assign(size_t(w) * h * 4, 0);
  for (uint32_t y = 0; y < h; y++)
    for (uint32_t x = 0; x < w; x++) {
      uint32_t off = 0, bit = 0;
      for (uint32_t m = 1; m < w || m < h; m <<= 1) {
        if (m < w) off |= (x & m ? 1u : 0u) << bit++;
        if (m < h) off |= (y & m ? 1u : 0u) << bit++;
      }
      const uint8_t* t = src + size_t(off) * 4;  // B, G, R, A
      uint8_t* o = &out->rgba[(size_t(y) * w + x) * 4];
      o[0] = t[2], o[1] = t[1], o[2] = t[0], o[3] = fmt == 0x07 ? 255 : t[3];
    }
  return true;
}

void Dump(const Hp2TextureInfo& info) {
  uint32_t f = info.xbox_format;
  if (f != kDxt1 && f != kDxt3 && f != kDxt5 && f != 0x06 && f != 0x07) return;
  if (!info.data || !g_dumped.insert(info.hash).second) return;
  Image img;
  bool ok = f == 0x06 || f == 0x07 ? DecodeArgbSwizzled(f, info.width, info.height, info.data, info.data_size, &img)
                                   : DecodeDxt(f, info.width, info.height, info.data, info.data_size, &img);
  if (!ok) return;
  char name[32];
  std::snprintf(name, sizeof name, "%016llx", (unsigned long long)info.hash);
  SavePng(g_dump + "/" + name + ".png", img.w, img.h, img.rgba.data());
  if (FILE* f = std::fopen((g_dump + "/index.tsv").c_str(), "a")) {
    std::fprintf(f, "%s\t%u\t%u\t%02X\t%u\n", name, info.width, info.height, info.xbox_format, info.levels);
    std::fclose(f);
  }
}

// HP2_HDUI_PROBE=1 (dev): every texture becomes a flat colour that encodes
// its number n as code c = 1 + (n - 1) % 1023: R = 4 + 8 * (c & 31),
// G = 4 + 8 * (c >> 5), B = 255 (a grey vertex colour scales all three, so
// c = (255 * R / B) / 8 + 32 * ((255 * G / B) / 8); bins of 8 tolerate light
// tints), alpha the original's, logged with its hash: one pixel of a
// screenshot names the texture drawn there (tools/hdworld/coverage.py; with
// HP2_DEV_MAGENTA=0-999999 HP2_DEV_SHOW=t0 every draw shows its texture
// unlit, so the codes come out exact).
bool g_probe = false;

bool Probe(const Hp2TextureInfo& info, uint32_t* w2, uint32_t* h2, const uint8_t** rgba) {
  static uint32_t n = 0;
  auto it = g_cache.find(info.hash);
  if (it == g_cache.end()) {
    n++;
    uint32_t c = 1 + (n - 1) % 1023;
    Image img;
    img.w = info.width, img.h = info.height;
    img.rgba.resize(size_t(img.w) * img.h * 4);
    // The original's alpha where it can be decoded (cut-out foliage, fences
    // keep their shape); else opaque.
    Image orig;
    uint32_t f = info.xbox_format;
    bool has_a = info.data && (f == kDxt1 || f == kDxt3 || f == kDxt5 || f == 0x06 || f == 0x07) &&
                 (f == 0x06 || f == 0x07 ? DecodeArgbSwizzled(f, info.width, info.height, info.data, info.data_size, &orig)
                                         : DecodeDxt(f, info.width, info.height, info.data, info.data_size, &orig)) &&
                 orig.rgba.size() == img.rgba.size();
    for (size_t i = 0; i < img.rgba.size(); i += 4)
      img.rgba[i] = uint8_t(4 + 8 * (c & 31)), img.rgba[i + 1] = uint8_t(4 + 8 * (c >> 5)), img.rgba[i + 2] = 255,
      img.rgba[i + 3] = has_a ? orig.rgba[i + 3] : 255;
    hp2_log("polish: hd_ui probe %u code %u = %016llx %ux%u fmt %02X levels %u", n, c,
            (unsigned long long)info.hash, info.width, info.height, info.xbox_format, info.levels);
    it = g_cache.emplace(info.hash, std::move(img)).first;
  }
  *w2 = it->second.w, *h2 = it->second.h;
  *rgba = it->second.rgba.data();
  return true;
}

bool Hook(const Hp2TextureInfo& info, uint32_t* w2, uint32_t* h2, const uint8_t** rgba) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (!g_dump.empty()) Dump(info);
  if (g_probe) return Probe(info, w2, h2, rgba);
  if (g_packs.empty() || g_missing.count(info.hash)) return false;
  // Pack images are not kept: the D3D8 HLE copies the image to the GPU during
  // the call, and a texture made again (rare) loads its PNG again. RAM stays
  // flat, the pack costs VRAM only.
  char name[32];
  std::snprintf(name, sizeof name, "%016llx.png", (unsigned long long)info.hash);
  std::string path;
  for (const std::string& d : g_packs)
    if (Exists(d + "/" + name)) {
      path = d + "/" + name;
      break;
    }
  static Image img;
  if (path.empty() || !LoadPng(path, &img)) {
    g_missing.insert(info.hash);
    return false;
  }
  uint32_t k = info.width ? img.w / info.width : 0;
  if (!k || img.w != k * info.width || img.h != k * info.height) {
    hp2_log("polish: hd_ui %s is %ux%u, not a multiple of %ux%u: ignored", name, img.w, img.h, info.width,
            info.height);
    g_missing.insert(info.hash);
    return false;
  }
  if (g_used.insert(info.hash).second) {
    static int logged = 0;
    if (logged++ < 64) hp2_log("polish: hd_ui %s %ux%u -> %ux%u", name, info.width, info.height, img.w, img.h);
    g_loaded_bytes += img.rgba.size();
    if (g_used.size() % 25 == 0)
      hp2_log("polish: hd pack: %zu textures replaced so far, %.1f MB uploaded (VRAM ~%.1f MB with mips)",
              g_used.size(), g_loaded_bytes / 1048576.0, g_loaded_bytes * 4 / 3 / 1048576.0);
  }
  *w2 = img.w, *h2 = img.h;
  *rgba = img.rgba.data();
  return true;
}

// Silent test runs (HP2_BACKGROUND=1) keep the original textures unless
// HP2_HDUI names a pack: the goldens are of the original game.
bool Background() {
  const char* b = getenv("HP2_BACKGROUND");
  return b && *b && *b != '0';
}

}  // namespace

void hp2_polish_texpack_init() {
  if (!hp2_d3d8_set_texture_hook) return;
  for (auto [var, sub] : {std::pair{"HP2_HDUI", "/mods/hd_ui"}, std::pair{"HP2_HDWORLD", "/mods/hd_world"}}) {
    const char* v = getenv(var);
    if (v && *v) {
      if (std::strcmp(v, "0")) g_packs.push_back(v);
    } else if (const char* d = getenv("HP2_DATA"); d && !Background()) {
      std::string p = std::string(d) + sub;
      if (Exists(p)) g_packs.push_back(p);
    }
  }
  if (const char* d = getenv("HP2_HDUI_DUMP"); d && *d) {
    g_dump = d;
    mkdir(d, 0755);
  }
  if (const char* p = getenv("HP2_HDUI_PROBE"); p && *p && *p != '0') {
    g_probe = true;
    hp2_log("polish: hd_ui probe: every texture a flat colour = its number (dev)");
  }
  if (g_packs.empty() && g_dump.empty() && !g_probe) return;
  for (const std::string& d : g_packs) hp2_log("polish: hd_ui pack %s", d.c_str());
  if (!g_dump.empty()) hp2_log("polish: hd_ui dump of DXT textures to %s", g_dump.c_str());
  hp2_d3d8_set_texture_hook(Hook);
}
