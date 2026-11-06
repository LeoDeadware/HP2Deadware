#pragma once
// MSL generation for the D3D8 HLE: Xbox vertex shader microcode (vsh.cpp)
// and register combiners / fixed-function texture stages (psh.cpp).
// Both sides share the interpolant struct and the uniform layouts below.

#include <cstdint>
#include <string>

namespace hp2d3d {

// Vertex uniforms (buffer 0 of the vertex stage). Layout must match kMslCommon.
struct VsUniforms {
  float c[192][4];      // c[-96..95] at index +96
  float scale[4];       // screen-space transform the program applies (c[-38])
  float offset[4];      // (c[-37]); inverted to get clip space back
  float ndc_shift[4];   // xy: pixel-centre convention shift in NDC units
  uint32_t attr[16][4]; // per v register: stream (0xFF = unused), offset, D3DVSDT, stride
  float snap[4];        // x != 0: 2D tile, edges snapped to the 1x pixel grid
};

// Fragment uniforms (buffer 0 of the fragment stage).
struct FsUniforms {
  float c0[8][4];
  float c1[8][4];
  float fc0[4], fc1[4];
  float tfactor[4];
  float fogcolor[4];
  float fogp[4];        // mode (0 none,1 exp,2 exp2,3 linear), start, end, density
  float alpha[4];       // test enable, func (0..7), ref (0..1)
  float texscale[4][4]; // xy multiply texture coordinates (1/size for LIN_ formats)
  float bump[4][4];     // BUMPENVMAT00, 01, 10, 11
  float bumpl[4][4];    // BUMPENVLSCALE, BUMPENVLOFFSET
  uint32_t flags[4];    // x fog enable, y specular enable (FF), z two-sided colours
  uint32_t akill[4];    // TSS_ALPHAKILL per stage
};

extern const char* kMslCommon;

// ---- vertex shaders ---------------------------------------------------------
// Translates a function (16-byte instructions, the FINAL bit ends it) to MSL.
// `words` points at the first instruction. Returns false (and logs) on a
// malformed program.
bool TranslateVsh(const uint32_t* words, uint32_t max_instr, std::string* msl,
                  uint32_t* instr_count);

// ---- pixel side ---------------------------------------------------------------
// Texture kind per stage: 0 none, 1 2D, 2 cube.
struct PsKey {
  // Combiner program (from the render-state array) when `combiners`;
  // otherwise the fixed-function texture stage cascade.
  bool combiners = false;
  bool wdepth = false;  // D3DZB_USEW: the program writes W-buffer depth (see VOut.wz)
  uint8_t tex_kind[4] = {};
  uint32_t words[64] = {};  // RS 0..56 + texture modes etc. (see psh.cpp)
  uint32_t nwords = 0;
  uint64_t Hash() const;
  bool operator==(const PsKey& o) const;
};
std::string GeneratePsh(const PsKey& k);

}  // namespace hp2d3d
