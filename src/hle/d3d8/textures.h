#pragma once
// Xbox texture memory -> host upload data: sizes and mip layout of a pixel
// container, unswizzling, conversion to BGRA8 (or BCn passthrough), content
// hashes for the replacement-pack hook.

#include <cstdint>
#include <vector>

#include "hle/d3d8/gpu.h"

namespace hp2d3d {

// Decoded description of a texture/surface header (Format + Size dwords).
struct TexLayout {
  uint32_t xfmt = 0;
  uint32_t w = 0, h = 0, depth = 1;
  uint32_t levels = 1;
  bool cube = false;
  bool linear = false, compressed = false;
  uint32_t bpp = 32;
  uint32_t pitch = 0;          // linear: bytes per row
  uint32_t level_offset[16] = {};
  uint32_t face_size = 0;      // bytes per cube face (whole chain, aligned)
  uint32_t total = 0;          // bytes of the whole resource
};
bool DescribeTexture(uint32_t format, uint32_t size, TexLayout* out);

// Builds the Format (and Size) dwords for a new texture, as D3D would.
void MakeTextureHeader(uint32_t w, uint32_t h, uint32_t depth, uint32_t levels, uint32_t xfmt,
                       bool cube, bool volume, uint32_t* format, uint32_t* size);

gpu::TexKind HostKind(uint32_t xfmt);

// Converts one level of one face to host upload data (BGRA8 rows or BCn
// blocks). `src` points at the level's first byte in guest memory.
void ConvertLevel(const TexLayout& L, uint32_t level, const uint8_t* src,
                  std::vector<uint8_t>* out, uint32_t* out_pitch);

uint64_t HashBytes(const void* p, size_t n);

}  // namespace hp2d3d
