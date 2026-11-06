// Pixel side -> MSL: NV2A register combiners (a bound pixel shader, i.e. the
// PS words of the render-state array) or Direct3D's fixed-function texture
// stage cascade (no pixel shader). One fragment function per PsKey; the
// constants (combiner c0/c1, texture factor, fog, alpha ref, bump matrices,
// texture coordinate scales) are uniforms, so the key holds structure only.
//
// PsKey::words layout:
//   combiners: [0..56] RS 0..56 (PS words), [57] PSTEXTUREMODES (RS 136),
//              [58] fog enable, [59] two-sided colours
//   fixed function: [s*8 + 0..7] stage s: COLOROP, COLORARG0..2, ALPHAOP,
//              ALPHAARG0..2 (s = 0..3), [32] RESULTARG mask (bit s: TEMP),
//              [33] fog enable, [34] specular enable, [35] two-sided

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "hle/d3d8/shaders.h"
#include "hle/d3d8/xbox_d3d.h"
#include "runtime/hp2_runtime.h"

namespace hp2d3d {

uint64_t PsKey::Hash() const {
  uint64_t h = 1469598103934665603ull ^ (combiners ? 0x9E37 : 0) ^ (wdepth ? 0x5A17 : 0);
  for (int i = 0; i < 4; i++) h = (h ^ tex_kind[i]) * 1099511628211ull;
  for (uint32_t i = 0; i < nwords; i++) h = (h ^ words[i]) * 1099511628211ull;
  return h;
}
bool PsKey::operator==(const PsKey& o) const {
  return combiners == o.combiners && wdepth == o.wdepth && nwords == o.nwords &&
         std::memcmp(tex_kind, o.tex_kind, 4) == 0 &&
         std::memcmp(words, o.words, nwords * 4) == 0;
}

namespace {

const char* kPsPrelude = R"(
// NV2A fog: the fog coordinate is used signed (D3D's LINEAR maps to the
// non-ABS hardware mode), so negative coordinates mean no fog.
static float fog_factor(constant FsUniforms& u, float f) {
  float d = f;
  uint m = uint(u.fogp.x);
  if (m == 3) return saturate((u.fogp.z - d) / max(u.fogp.z - u.fogp.y, 1e-6));
  if (m == 1) return saturate(exp(-abs(d) * u.fogp.w));
  if (m == 2) { float x = d * u.fogp.w; return saturate(exp(-x * x)); }
  return saturate(f);
}
static bool alpha_pass(constant FsUniforms& u, float a) {
  if (u.alpha.x == 0) return true;
  float x = round(saturate(a) * 255.0), r = round(u.alpha.z * 255.0);
  switch (uint(u.alpha.y)) {
    case 0: return false;
    case 1: return x < r;
    case 2: return x == r;
    case 3: return x <= r;
    case 4: return x > r;
    case 5: return x != r;
    case 6: return x >= r;
  }
  return true;
}
)";

std::string Sig(const PsKey& k) {
  std::string s = k.wdepth ? "\nfragment PsOutW" : "\nfragment float4";
  s += R"( fmain(VOut in [[stage_in]], bool front [[front_facing]],
                      constant FsUniforms& u [[buffer(0)]])";
  char b[160];
  for (int i = 0; i < 4; i++) {
    if (k.tex_kind[i] == 2)
      std::snprintf(b, sizeof b, ",\n    texturecube<float> tx%d [[texture(%d)]], sampler sm%d [[sampler(%d)]]", i, i, i, i);
    else
      std::snprintf(b, sizeof b, ",\n    texture2d<float> tx%d [[texture(%d)]], sampler sm%d [[sampler(%d)]]", i, i, i, i);
    s += b;
  }
  s += ") {\n";
  s += "  float4 T[4] = {in.t0, in.t1, in.t2, in.t3};\n";
  return s;
}

// The program's return: the colour, plus the W-buffer depth for USEW draws.
std::string End(const PsKey& k) {
  if (!k.wdepth) return "  return outc;\n}\n";
  return "  PsOutW o;\n  o.c = outc;\n  o.d = saturate(in.wz * (1.0 / 16777216.0));\n  return o;\n}\n";
}

// Sample stage i at 2D coordinates `uv` (already scaled) into `dst`.
std::string Sample2D(const PsKey& k, int i, const std::string& uv) {
  char b[256];
  if (k.tex_kind[i] == 1)
    std::snprintf(b, sizeof b, "tx%d.sample(sm%d, %s)", i, i, uv.c_str());
  else if (k.tex_kind[i] == 2)
    std::snprintf(b, sizeof b, "tx%d.sample(sm%d, float3(%s, 1))", i, i, uv.c_str());
  else
    std::snprintf(b, sizeof b, "float4(0, 0, 0, 1)");
  return b;
}

std::string SampleCube(const PsKey& k, int i, const std::string& dir) {
  char b[256];
  if (k.tex_kind[i] == 2)
    std::snprintf(b, sizeof b, "tx%d.sample(sm%d, %s)", i, i, dir.c_str());
  else if (k.tex_kind[i] == 1)
    std::snprintf(b, sizeof b, "tx%d.sample(sm%d, (%s).xy)", i, i, dir.c_str());
  else
    std::snprintf(b, sizeof b, "float4(0, 0, 0, 1)");
  return b;
}

std::string AlphaKill(int i) {
  char b[128];
  std::snprintf(b, sizeof b, "  if (u.akill[%d] != 0 && t%d.a == 0) discard_fragment();\n", i, i);
  return b;
}

// ---- register combiners -------------------------------------------------------

// Value of an input byte (register | channel | mapping) for the rgb (want_alpha
// false) or alpha portion.
std::string CombIn(uint32_t byte, bool alpha_portion, bool final_comb) {
  uint32_t reg = byte & 0xF, alpha = byte & 0x10, map = byte & 0xE0;
  const char* r;
  switch (reg) {
    case 0: r = "float4(0)"; break;
    case 1: r = "C0"; break;
    case 2: r = "C1"; break;
    case 3: r = "FOG"; break;
    case 4: r = "v0"; break;
    case 5: r = "v1"; break;
    case 8: r = "t0"; break;
    case 9: r = "t1"; break;
    case 10: r = "t2"; break;
    case 11: r = "t3"; break;
    case 12: r = "r0"; break;
    case 13: r = "r1"; break;
    case 14: r = final_comb ? "SUM" : "float4(0)"; break;
    case 15: r = final_comb ? "PROD" : "float4(0)"; break;
    default: r = "float4(0)"; break;
  }
  std::string v;
  if (alpha_portion)
    v = std::string(r) + (alpha ? ".aaaa" : ".bbbb");
  else
    v = std::string(r) + (alpha ? ".aaaa" : "");
  std::string x = "(" + v + ")";
  switch (map) {
    case 0x00: return "max(" + x + ", 0.0)";
    case 0x20: return "(1.0 - saturate(" + x + "))";
    case 0x40: return "(2.0 * max(" + x + ", 0.0) - 1.0)";
    case 0x60: return "(1.0 - 2.0 * max(" + x + ", 0.0))";
    case 0x80: return "(max(" + x + ", 0.0) - 0.5)";
    case 0xA0: return "(0.5 - max(" + x + ", 0.0))";
    case 0xC0: return x;
    case 0xE0: return "(-" + x + ")";
  }
  return x;
}

const char* RegName(uint32_t r) {
  switch (r) {
    case 4: return "v0";
    case 5: return "v1";
    case 8: return "t0";
    case 9: return "t1";
    case 10: return "t2";
    case 11: return "t3";
    case 12: return "r0";
    case 13: return "r1";
  }
  return nullptr;  // discard
}

std::string OutMap(uint32_t m, const std::string& x) {
  switch (m) {
    case 0x0: return x;
    case 0x1: return "(" + x + " - 0.5)";
    case 0x2: return "(" + x + " * 2.0)";
    case 0x3: return "((" + x + " - 0.5) * 2.0)";
    case 0x4: return "(" + x + " * 4.0)";
    case 0x5: return "((" + x + " - 0.5) * 4.0)";
    case 0x6: return "(" + x + " * 0.5)";
    case 0x7: return "((" + x + " - 0.5) * 0.5)";
  }
  return x;
}

void CombinerStage(std::string& s, int i, uint32_t rgb_in, uint32_t a_in, uint32_t rgb_out,
                   uint32_t a_out, bool unique_c0, bool unique_c1, bool mux_msb) {
  char b[512];
  std::snprintf(b, sizeof b, "  { // combiner %d\n    float4 C0 = u.c0[%d], C1 = u.c1[%d];\n", i,
                unique_c0 ? i : 0, unique_c1 ? i : 0);
  s += b;
  // RGB portion.
  s += "    float4 A = " + CombIn(rgb_in >> 24, false, false) + ";\n";
  s += "    float4 B = " + CombIn((rgb_in >> 16) & 0xFF, false, false) + ";\n";
  s += "    float4 C = " + CombIn((rgb_in >> 8) & 0xFF, false, false) + ";\n";
  s += "    float4 D = " + CombIn(rgb_in & 0xFF, false, false) + ";\n";
  bool ab_dot = rgb_out & 0x2000, cd_dot = rgb_out & 0x1000, mux = rgb_out & 0x4000;
  s += ab_dot ? "    float3 ab = float3(dot(A.rgb, B.rgb));\n" : "    float3 ab = A.rgb * B.rgb;\n";
  s += cd_dot ? "    float3 cd = float3(dot(C.rgb, D.rgb));\n" : "    float3 cd = C.rgb * D.rgb;\n";
  std::string selr = mux_msb ? "r0.a >= 0.5" : "(uint(round(saturate(r0.a) * 255.0)) & 1) != 0";
  if (mux)
    s += "    float3 sm = (" + selr + ") ? cd : ab;\n";
  else
    s += "    float3 sm = ab + cd;\n";
  uint32_t rm = (rgb_out >> 15) & 7;
  s += "    float3 o_ab = clamp(" + OutMap(rm, "ab") + ", -1.0, 1.0);\n";
  s += "    float3 o_cd = clamp(" + OutMap(rm, "cd") + ", -1.0, 1.0);\n";
  s += "    float3 o_sm = clamp(" + OutMap(rm, "sm") + ", -1.0, 1.0);\n";
  // Alpha portion.
  s += "    float a_A = " + CombIn(a_in >> 24, true, false) + ".x;\n";
  s += "    float a_B = " + CombIn((a_in >> 16) & 0xFF, true, false) + ".x;\n";
  s += "    float a_C = " + CombIn((a_in >> 8) & 0xFF, true, false) + ".x;\n";
  s += "    float a_D = " + CombIn(a_in & 0xFF, true, false) + ".x;\n";
  s += "    float a_ab = a_A * a_B, a_cd = a_C * a_D;\n";
  if (a_out & 0x4000)
    s += "    float a_sm = (" + selr + ") ? a_cd : a_ab;\n";
  else
    s += "    float a_sm = a_ab + a_cd;\n";
  uint32_t am = (a_out >> 15) & 7;
  s += "    float oa_ab = clamp(" + OutMap(am, "a_ab") + ", -1.0, 1.0);\n";
  s += "    float oa_cd = clamp(" + OutMap(am, "a_cd") + ", -1.0, 1.0);\n";
  s += "    float oa_sm = clamp(" + OutMap(am, "a_sm") + ", -1.0, 1.0);\n";
  // Writes (after all reads).
  uint32_t cd_dst = rgb_out & 0xF, ab_dst = (rgb_out >> 4) & 0xF, sm_dst = (rgb_out >> 8) & 0xF;
  if (const char* r = RegName(ab_dst)) { std::snprintf(b, sizeof b, "    %s.rgb = o_ab;\n", r); s += b; }
  if (const char* r = RegName(cd_dst)) { std::snprintf(b, sizeof b, "    %s.rgb = o_cd;\n", r); s += b; }
  if (const char* r = RegName(sm_dst)) { std::snprintf(b, sizeof b, "    %s.rgb = o_sm;\n", r); s += b; }
  uint32_t acd = a_out & 0xF, aab = (a_out >> 4) & 0xF, asm_ = (a_out >> 8) & 0xF;
  if (const char* r = RegName(aab)) { std::snprintf(b, sizeof b, "    %s.a = oa_ab;\n", r); s += b; }
  if (const char* r = RegName(acd)) { std::snprintf(b, sizeof b, "    %s.a = oa_cd;\n", r); s += b; }
  if (const char* r = RegName(asm_)) { std::snprintf(b, sizeof b, "    %s.a = oa_sm;\n", r); s += b; }
  // Blue to alpha (RGB outputs only).
  if ((rgb_out & 0x80000) && RegName(ab_dst)) {
    std::snprintf(b, sizeof b, "    %s.a = o_ab.b;\n", RegName(ab_dst));
    s += b;
  }
  if ((rgb_out & 0x40000) && RegName(cd_dst)) {
    std::snprintf(b, sizeof b, "    %s.a = o_cd.b;\n", RegName(cd_dst));
    s += b;
  }
  s += "  }\n";
}

std::string GenCombiners(const PsKey& k) {
  const uint32_t* w = k.words;
  std::string s = kMslCommon;
  s += kPsPrelude;
  s += Sig(k);
  bool fog = w[58] != 0, two_sided = w[59] != 0;
  s += two_sided ? "  float4 v0 = front ? in.d0 : in.b0, v1 = front ? in.d1 : in.b1;\n"
                 : "  float4 v0 = in.d0, v1 = in.d1;\n";
  s += "  float4 FOG = float4(u.fogcolor.rgb, ";
  s += fog ? "fog_factor(u, in.fog));\n" : "1.0);\n";
  uint32_t modes = w[57];
  char b[512];
  for (int i = 0; i < 4; i++) {
    uint32_t m = (modes >> (5 * i)) & 0x1F;
    std::snprintf(b, sizeof b, "  float4 t%d = float4(0);\n", i);
    s += b;
    std::snprintf(b, sizeof b, "  float2 uv%d = T[%d].xy * u.texscale[%d].xy;\n", i, i, i);
    s += b;
    switch (m) {
      case 0: break;
      case 1:  // PROJECT2D
      case 2:  // PROJECT3D (no volume textures: as 2D)
        std::snprintf(b, sizeof b, "  { float q = T[%d].w != 0 ? T[%d].w : 1.0; t%d = %s; }\n", i,
                      i, i, Sample2D(k, i, "uv" + std::to_string(i) + " / q").c_str());
        s += b;
        s += AlphaKill(i);
        break;
      case 3:  // CUBEMAP
        std::snprintf(b, sizeof b, "  t%d = %s;\n", i,
                      SampleCube(k, i, "T[" + std::to_string(i) + "].xyz").c_str());
        s += b;
        s += AlphaKill(i);
        break;
      case 4:  // PASSTHRU
        std::snprintf(b, sizeof b, "  t%d = saturate(T[%d]);\n", i, i);
        s += b;
        break;
      case 5:  // CLIPPLANE: reject where a coordinate is negative (compare mode bits)
        std::snprintf(b, sizeof b,
                      "  { uint cm = (%uu >> %d) & 0xF; float4 c = T[%d];\n"
                      "    if (((cm & 1) ? c.x >= 0 : c.x < 0) || ((cm & 2) ? c.y >= 0 : c.y < 0) ||\n"
                      "        ((cm & 4) ? c.z >= 0 : c.z < 0) || ((cm & 8) ? c.w >= 0 : c.w < 0)) discard_fragment(); }\n",
                      w[RS_PSCOMPAREMODE], 4 * i, i);
        s += b;
        break;
      case 6:  // BUMPENVMAP
      case 7:  // BUMPENVMAP_LUM
        if (i == 0) break;
        std::snprintf(b, sizeof b,
                      "  { float2 d = t%d.rg * 2.0 - 1.0; float4 bm = u.bump[%d];\n"
                      "    float2 uv = uv%d + float2(bm.x * d.x + bm.z * d.y, bm.y * d.x + bm.w * d.y) * u.texscale[%d].zw;\n"
                      "    t%d = %s;\n",
                      i - 1, i, i, i, i, Sample2D(k, i, "uv").c_str());
        s += b;
        if (m == 7) {
          std::snprintf(b, sizeof b,
                        "    t%d.rgb *= saturate(t%d.b * u.bumpl[%d].x + u.bumpl[%d].y);\n", i,
                        i - 1, i, i);
          s += b;
        }
        s += "  }\n";
        s += AlphaKill(i);
        break;
      case 0x11:  // DOTPRODUCT: t = dot(texcoord, prev) (used by the next stage)
        if (i == 0) break;
        std::snprintf(b, sizeof b, "  t%d = float4(dot(T[%d].xyz, t%d.rgb * 2.0 - 1.0));\n", i, i,
                      i - 1);
        s += b;
        break;
      case 9:  // DOT_ST: s from the previous dot product, t from this one
        if (i < 2) break;
        std::snprintf(b, sizeof b,
                      "  t%d = %s;\n", i,
                      Sample2D(k, i, "float2(t" + std::to_string(i - 1) + ".x, dot(T[" +
                                         std::to_string(i) + "].xyz, t" + std::to_string(i - 2) +
                                         ".rgb * 2.0 - 1.0))").c_str());
        s += b;
        break;
      default:
        std::snprintf(b, sizeof b, "  // texture mode %u of stage %d not supported\n", m, i);
        s += b;
        break;
    }
  }
  // r0.a starts as t0.a, r0.rgb / r1 as zero.
  s += "  float4 r0 = float4(0, 0, 0, t0.a), r1 = float4(0);\n";
  uint32_t cc = w[RS_PSCOMBINERCOUNT];
  uint32_t count = cc & 0xFF;
  if (count > 8) count = 8;
  bool mux_msb = (cc >> 8) & 1, unique_c0 = (cc >> 12) & 1, unique_c1 = (cc >> 16) & 1;
  for (uint32_t i = 0; i < count; i++)
    CombinerStage(s, int(i), w[RS_PSRGBINPUTS0 + i], w[RS_PSALPHAINPUTS0 + i],
                  w[RS_PSRGBOUTPUTS0 + i], w[RS_PSALPHAOUTPUTS0 + i], unique_c0, unique_c1,
                  mux_msb);
  uint32_t abcd = w[RS_PSFINALCOMBINERINPUTSABCD], efg = w[RS_PSFINALCOMBINERINPUTSEFG];
  if (abcd == 0 && efg == 0) {
    s += "  float4 outc = r0;\n";
    if (fog) s += "  outc.rgb = mix(FOG.rgb, outc.rgb, FOG.a);\n";
  } else {
    s += "  float4 outc;\n  {\n    float4 C0 = u.fc0, C1 = u.fc1;\n";
    // V1R0_SUM / EF_PROD.
    uint32_t set = efg & 0xFF;
    s += std::string("    float4 SUM = float4(") + ((set & 0x40) ? "1.0 - saturate(v1.rgb)" : "v1.rgb") +
         " + " + ((set & 0x20) ? "1.0 - saturate(r0.rgb)" : "r0.rgb") + ", 0);\n";
    if (set & 0x80) s += "    SUM = saturate(SUM);\n";
    s += "    float4 E = " + CombIn(efg >> 24, false, true) + ";\n";
    s += "    float4 F = " + CombIn((efg >> 16) & 0xFF, false, true) + ";\n";
    s += "    float4 PROD = float4(E.rgb * F.rgb, 0);\n";
    s += "    float4 A = saturate(" + CombIn(abcd >> 24, false, true) + ");\n";
    s += "    float4 B = saturate(" + CombIn((abcd >> 16) & 0xFF, false, true) + ");\n";
    s += "    float4 C = saturate(" + CombIn((abcd >> 8) & 0xFF, false, true) + ");\n";
    s += "    float4 D = saturate(" + CombIn(abcd & 0xFF, false, true) + ");\n";
    s += "    float G = saturate(" + CombIn((efg >> 8) & 0xFF, true, true) + ".x);\n";
    s += "    outc = float4(A.rgb * B.rgb + (1.0 - A.rgb) * C.rgb + D.rgb, G);\n  }\n";
  }
  s += "  outc = saturate(outc);\n";
  s += "  if (!alpha_pass(u, outc.a)) discard_fragment();\n";
  s += End(k);
  return s;
}

// ---- fixed function -------------------------------------------------------------

std::string FfArg(uint32_t a, int stage, bool alpha) {
  std::string r;
  switch (a & TA_SELECTMASK) {
    case TA_DIFFUSE: r = "v0"; break;
    case TA_CURRENT: r = "cur"; break;
    case TA_TEXTURE: r = "t" + std::to_string(stage); break;
    case TA_TFACTOR: r = "u.tfactor"; break;
    case TA_SPECULAR: r = "v1"; break;
    case TA_TEMP: r = "tmp"; break;
    default: r = "float4(0)"; break;
  }
  if ((a & TA_ALPHAREPLICATE) && !alpha) r = "(" + r + ").aaaa";
  if (a & TA_COMPLEMENT) r = "(1.0 - " + r + ")";
  return r;
}

std::string FfOp(uint32_t op, const std::string& a0, const std::string& a1, const std::string& a2,
                 int stage, bool alpha) {
  std::string s = std::to_string(stage);
  switch (op) {
    case TOP_SELECTARG1: return a1;
    case TOP_SELECTARG2: return a2;
    case TOP_MODULATE: return a1 + " * " + a2;
    case TOP_MODULATE2X: return "2.0 * " + a1 + " * " + a2;
    case TOP_MODULATE4X: return "4.0 * " + a1 + " * " + a2;
    case TOP_ADD: return a1 + " + " + a2;
    case TOP_ADDSIGNED: return a1 + " + " + a2 + " - 0.5";
    case TOP_ADDSIGNED2X: return "2.0 * (" + a1 + " + " + a2 + " - 0.5)";
    case TOP_SUBTRACT: return a1 + " - " + a2;
    case TOP_ADDSMOOTH: return a1 + " + " + a2 + " - " + a1 + " * " + a2;
    case TOP_BLENDDIFFUSEALPHA: return "mix(" + a2 + ", " + a1 + ", v0.a)";
    case TOP_BLENDCURRENTALPHA: return "mix(" + a2 + ", " + a1 + ", cur.a)";
    case TOP_BLENDTEXTUREALPHA: return "mix(" + a2 + ", " + a1 + ", t" + s + ".a)";
    case TOP_BLENDFACTORALPHA: return "mix(" + a2 + ", " + a1 + ", u.tfactor.a)";
    case TOP_BLENDTEXTUREALPHAPM: return a1 + " + " + a2 + " * (1.0 - t" + s + ".a)";
    case TOP_PREMODULATE: return a1;
    case TOP_MODULATEALPHA_ADDCOLOR:
      return alpha ? a1 : "(" + a1 + " + " + a1 + ".a * " + a2 + ")";
    case TOP_MODULATECOLOR_ADDALPHA:
      return alpha ? a1 : "(" + a1 + " * " + a2 + " + " + a1 + ".a)";
    case TOP_MODULATEINVALPHA_ADDCOLOR:
      return alpha ? a1 : "(" + a1 + " + (1.0 - " + a1 + ".a) * " + a2 + ")";
    case TOP_MODULATEINVCOLOR_ADDALPHA:
      return alpha ? a1 : "((1.0 - " + a1 + ") * " + a2 + " + " + a1 + ".a)";
    case TOP_DOTPRODUCT3:
      return "float4(saturate(dot((" + a1 + ").rgb * 2.0 - 1.0, (" + a2 + ").rgb * 2.0 - 1.0)))";
    case TOP_MULTIPLYADD: return a0 + " + " + a1 + " * " + a2;
    case TOP_LERP: return "mix(" + a2 + ", " + a1 + ", " + a0 + ")";
    case TOP_BUMPENVMAP:
    case TOP_BUMPENVMAPLUMINANCE: return "cur";
  }
  return "cur";
}

std::string GenFixed(const PsKey& k) {
  const uint32_t* w = k.words;
  std::string s = kMslCommon;
  s += kPsPrelude;
  s += Sig(k);
  bool fog = w[33] != 0, spec = w[34] != 0, two = w[35] != 0;
  s += two ? "  float4 v0 = front ? in.d0 : in.b0, v1 = front ? in.d1 : in.b1;\n"
           : "  float4 v0 = in.d0, v1 = in.d1;\n";
  s += "  float4 cur = v0, tmp = float4(0);\n";
  char b[512];
  // Textures; a stage whose previous stage is BUMPENVMAP perturbs its coordinates.
  // A stage without a texture reads opaque white (Direct3D's convention, which
  // the game relies on: untextured HUD panels use COLOROP MODULATE2X with
  // TEXTURE x DIFFUSE and would vanish with 0).
  for (int i = 0; i < 4; i++) {
    std::snprintf(b, sizeof b, "  float4 t%d = float4(%d);\n", i, k.tex_kind[i] ? 0 : 1);
    s += b;
  }
  for (int i = 0; i < 4; i++) {
    uint32_t cop = w[i * 8 + 0];
    if (cop == TOP_DISABLE || cop == 0) break;
    if (!k.tex_kind[i]) continue;
    uint32_t prev = i > 0 ? w[(i - 1) * 8 + 0] : 0;
    std::string uv = "T[" + std::to_string(i) + "].xy * u.texscale[" + std::to_string(i) + "].xy";
    if (prev == TOP_BUMPENVMAP || prev == TOP_BUMPENVMAPLUMINANCE) {
      std::snprintf(b, sizeof b,
                    "  { float2 d = t%d.rg * 2.0 - 1.0; float4 bm = u.bump[%d];\n"
                    "    float2 uv = %s + float2(bm.x * d.x + bm.z * d.y, bm.y * d.x + bm.w * d.y) * u.texscale[%d].zw;\n"
                    "    t%d = %s;\n",
                    i - 1, i - 1, uv.c_str(), i, i, Sample2D(k, i, "uv").c_str());
      s += b;
      if (prev == TOP_BUMPENVMAPLUMINANCE) {
        std::snprintf(b, sizeof b,
                      "    t%d.rgb *= saturate(t%d.b * u.bumpl[%d].x + u.bumpl[%d].y);\n", i,
                      i - 1, i - 1, i - 1);
        s += b;
      }
      s += "  }\n";
    } else if (k.tex_kind[i] == 2) {
      std::snprintf(b, sizeof b, "  t%d = %s;\n", i,
                    SampleCube(k, i, "T[" + std::to_string(i) + "].xyz").c_str());
      s += b;
    } else {
      std::snprintf(b, sizeof b, "  { float q = T[%d].w != 0 ? T[%d].w : 1.0; t%d = %s; }\n", i,
                    i, i, Sample2D(k, i, "(" + uv + ") / q").c_str());
      s += b;
    }
    s += AlphaKill(i);
  }
  for (int i = 0; i < 4; i++) {
    uint32_t cop = w[i * 8 + 0], aop = w[i * 8 + 4];
    if (cop == TOP_DISABLE || cop == 0) break;
    std::string c0 = FfArg(w[i * 8 + 1], i, false), c1 = FfArg(w[i * 8 + 2], i, false),
                c2 = FfArg(w[i * 8 + 3], i, false);
    std::string a0 = FfArg(w[i * 8 + 5], i, true), a1 = FfArg(w[i * 8 + 6], i, true),
                a2 = FfArg(w[i * 8 + 7], i, true);
    bool to_temp = (w[32] >> i) & 1;
    const char* dst = to_temp ? "tmp" : "cur";
    std::snprintf(b, sizeof b, "  { // stage %d: colorop %u alphaop %u\n", i, cop, aop);
    s += b;
    s += "    float3 rgb = saturate((" + FfOp(cop, c0, c1, c2, i, false) + ").rgb);\n";
    if (aop == TOP_DISABLE || aop == 0)
      s += "    float a = cur.a;\n";
    else
      s += "    float a = saturate((" + FfOp(aop, a0, a1, a2, i, true) + ").a);\n";
    std::snprintf(b, sizeof b, "    %s = float4(rgb, a);\n  }\n", dst);
    s += b;
  }
  s += "  float4 outc = cur;\n";
  if (spec) s += "  outc.rgb = saturate(outc.rgb + v1.rgb);\n";
  if (fog) s += "  outc.rgb = mix(u.fogcolor.rgb, outc.rgb, fog_factor(u, in.fog));\n";
  s += "  if (!alpha_pass(u, outc.a)) discard_fragment();\n";
  s += End(k);
  return s;
}

}  // namespace

std::string GeneratePsh(const PsKey& k) { return k.combiners ? GenCombiners(k) : GenFixed(k); }

}  // namespace hp2d3d
