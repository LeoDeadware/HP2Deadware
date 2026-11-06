// Xbox (NV2A) vertex shader microcode -> MSL.
//
// An instruction is 4 dwords (dword 0 unused); fields as in Cxbx-Reloaded's
// decoder (XbVertexShader.cpp). Each instruction can issue a MAC op (MOV MUL
// ADD MAD DP3 DPH DP4 DST MIN MAX SLT SGE ARL) and an ILU op (MOV RCP RCC
// RSQ EXP LOG LIT) in parallel: both read their inputs before either writes.
// Paired, the ILU writes r1 and the MAC's r1 write is dropped. One of the two
// results may also go to an output register (or a constant). r12 is oPos.
//
// Inputs are fetched in the shader ("vertex pulling") from up to four raw
// streams, per register described by VsUniforms::attr, so one compiled
// function serves every vertex declaration and stride.
//
// oPos leaves the program in screen space (the XDK assembler appends
// "mul oPos.xyz, r12, c-38 / rcc r1.x, r12.w / mad oPos.xyz, r12, r1.x, c-37");
// the epilogue undoes that with the same scale/offset the HLE put in
// c-38/c-37, giving D3D clip space ([0,1] z, like Metal).

#include <cstdio>

#include "hle/d3d8/shaders.h"
#include "runtime/hp2_runtime.h"

namespace hp2d3d {

const char* kMslCommon = R"(#include <metal_stdlib>
using namespace metal;
struct VsUniforms {
  float4 c[192];
  float4 scale;
  float4 offset;
  float4 ndc_shift;
  uint4 attr[16];
  float4 snap;
};
struct FsUniforms {
  float4 c0[8];
  float4 c1[8];
  float4 fc0, fc1;
  float4 tfactor;
  float4 fogcolor;
  float4 fogp;
  float4 alpha;
  float4 texscale[4];
  float4 bump[4];
  float4 bumpl[4];
  uint4 flags;
  uint4 akill;
};
struct VOut {
  float4 pos [[position]];
  float4 d0;
  float4 d1;
  float4 b0;
  float4 b1;
  float fog;
  float4 t0;
  float4 t1;
  float4 t2;
  float4 t3;
  float wz;  // eye-space w (perspective-interpolated): W-buffer depth
  float pts [[point_size]];
};
// D3DZB_USEW (RS_ZENABLE 2): the NV2A W-buffer stores interpolated eye w,
// linear in distance, instead of z/w. Fragment programs of such draws write
// depth = w * 2^-24 (Depth32Float keeps 24 bits relative to any w).
struct PsOutW {
  float4 c [[color(0)]];
  float d [[depth(any)]];
};
)";

namespace {

const char* kVsPrelude = R"(
static float4 fetch(device const uchar* s, uint off, uint type) {
  device const uchar* p = s + off;
  switch (type) {
    case 0x12: return float4(*(device const float*)p, 0, 0, 1);
    case 0x22: return float4(*(device const packed_float2*)p, 0, 1);
    case 0x32: return float4(*(device const packed_float3*)p, 1);
    case 0x42: return float4(*(device const packed_float4*)p);
    case 0x40: return float4(p[2], p[1], p[0], p[3]) / 255.0;
    case 0x16: {
      uint v = *(device const uint*)p;
      int x = int(v << 21) >> 21, y = int(v << 10) >> 21, z = int(v) >> 22;
      return float4(float(x) / 1023.0, float(y) / 1023.0, float(z) / 511.0, 1);
    }
    case 0x11: return float4(max(float(*(device const short*)p) / 32767.0, -1.0), 0, 0, 1);
    case 0x21: { device const short* q = (device const short*)p;
      return float4(max(float2(q[0], q[1]) / 32767.0, -1.0), 0, 1); }
    case 0x31: { device const short* q = (device const short*)p;
      return float4(max(float3(q[0], q[1], q[2]) / 32767.0, -1.0), 1); }
    case 0x41: { device const short* q = (device const short*)p;
      return max(float4(q[0], q[1], q[2], q[3]) / 32767.0, -1.0); }
    case 0x15: return float4(float(*(device const short*)p), 0, 0, 1);
    case 0x25: { device const short* q = (device const short*)p; return float4(q[0], q[1], 0, 1); }
    case 0x35: { device const short* q = (device const short*)p; return float4(q[0], q[1], q[2], 1); }
    case 0x45: { device const short* q = (device const short*)p; return float4(q[0], q[1], q[2], q[3]); }
    case 0x14: return float4(p[0] / 255.0, 0, 0, 1);
    case 0x24: return float4(p[0] / 255.0, p[1] / 255.0, 0, 1);
    case 0x34: return float4(p[0] / 255.0, p[1] / 255.0, p[2] / 255.0, 1);
    case 0x44: return float4(p[0], p[1], p[2], p[3]) / 255.0;
    case 0x72: { float3 f = *(device const packed_float3*)p; return float4(f.x, f.y, 0, f.z); }
  }
  return float4(0, 0, 0, 1);
}
static float x_rcc(float s) {
  float r = 1.0 / s;
  return r >= 0 ? clamp(r, 5.42101e-20f, 1.84467e+19f) : clamp(r, -1.84467e+19f, -5.42101e-20f);
}
static float4 x_exp(float s) {
  float f = floor(s + 0.001);
  return float4(exp2(f), s - f, exp2(s), 1);
}
static float4 x_log(float s) {
  float a = abs(s);
  if (a == 0) return float4(-INFINITY, 1, -INFINITY, 1);
  int e;
  float m = frexp(a, e);
  return float4(float(e - 1), m * 2.0, log2(a), 1);
}
static float4 x_lit(float4 s) {
  float p = clamp(s.w, -127.9961, 127.9961);
  return float4(1, max(0.0, s.x), (s.x > 0 && s.y > 0) ? pow(s.y, p) : 0.0, 1);
}
)";

uint32_t Field(const uint32_t* t, int sub, int start, int len) {
  return (t[sub] >> start) & ((1u << len) - 1);
}

enum { MAC_NOP, MAC_MOV, MAC_MUL, MAC_ADD, MAC_MAD, MAC_DP3, MAC_DPH, MAC_DP4, MAC_DST, MAC_MIN,
       MAC_MAX, MAC_SLT, MAC_SGE, MAC_ARL };
enum { ILU_NOP, ILU_MOV, ILU_RCP, ILU_RCC, ILU_RSQ, ILU_EXP, ILU_LOG, ILU_LIT };

const char kSw[] = "xyzw";

std::string MaskStr(uint32_t m) {  // bit 3 = x
  std::string s;
  for (int i = 0; i < 4; i++)
    if (m & (8 >> i)) s += kSw[i];
  return s;
}

struct Src {
  uint32_t mux;  // 1 R, 2 V, 3 C
  uint32_t neg;
  uint32_t sw[4];
};

// Expression for an operand.
std::string Operand(const Src& s, uint32_t r, uint32_t v, int c, bool a0x) {
  char base[64];
  switch (s.mux) {
    case 1: std::snprintf(base, sizeof base, "R[%u]", r); break;
    case 2: std::snprintf(base, sizeof base, "V[%u]", v); break;
    case 3:
      if (a0x)
        std::snprintf(base, sizeof base, "CR(a0 + %d)", c);
      else
        std::snprintf(base, sizeof base, "CR(%d)", c);
      break;
    default: std::snprintf(base, sizeof base, "float4(0)"); break;
  }
  std::string e = std::string("(") + (s.neg ? "-" : "") + base;
  if (!(s.sw[0] == 0 && s.sw[1] == 1 && s.sw[2] == 2 && s.sw[3] == 3)) {
    e += ".";
    for (int i = 0; i < 4; i++) e += kSw[s.sw[i]];
  }
  return e + ")";
}

}  // namespace

bool TranslateVsh(const uint32_t* words, uint32_t max_instr, std::string* msl,
                  uint32_t* instr_count) {
  std::string body;
  bool writes_c = false;
  uint32_t n = 0;
  char buf[512];
  for (;; n++) {
    if (n >= max_instr) {
      hp2_log("d3d8: vertex shader without FINAL in %u instructions", max_instr);
      return false;
    }
    const uint32_t* t = words + 4 * n;
    uint32_t ilu = Field(t, 1, 25, 3), mac = Field(t, 1, 21, 4);
    int cfield = int(Field(t, 1, 13, 8));
    int creg = (((cfield >> 5) & 7) - 3) * 32 + (cfield & 31);
    uint32_t vreg = Field(t, 1, 9, 4);
    Src a{Field(t, 2, 26, 2), Field(t, 1, 8, 1),
          {Field(t, 1, 6, 2), Field(t, 1, 4, 2), Field(t, 1, 2, 2), Field(t, 1, 0, 2)}};
    uint32_t ar = Field(t, 2, 28, 4);
    Src b{Field(t, 2, 11, 2), Field(t, 2, 25, 1),
          {Field(t, 2, 23, 2), Field(t, 2, 21, 2), Field(t, 2, 19, 2), Field(t, 2, 17, 2)}};
    uint32_t br = Field(t, 2, 13, 4);
    Src c{Field(t, 3, 28, 2), Field(t, 2, 10, 1),
          {Field(t, 2, 8, 2), Field(t, 2, 6, 2), Field(t, 2, 4, 2), Field(t, 2, 2, 2)}};
    uint32_t cr = Field(t, 2, 0, 2) << 2 | Field(t, 3, 30, 2);
    uint32_t mac_mask = Field(t, 3, 24, 4), out_r = Field(t, 3, 20, 4);
    uint32_t ilu_mask = Field(t, 3, 16, 4), o_mask = Field(t, 3, 12, 4);
    uint32_t orb = Field(t, 3, 11, 1), out_addr = Field(t, 3, 3, 8);
    uint32_t out_mux = Field(t, 3, 2, 1);
    bool a0x = Field(t, 3, 1, 1) != 0;
    bool final = Field(t, 3, 0, 1) != 0;
    if (mac > MAC_ARL) {
      hp2_log("d3d8: vertex shader: unknown MAC op %u", mac);
      return false;
    }
    std::snprintf(buf, sizeof buf, "  { // %u: mac %u ilu %u\n", n, mac, ilu);
    body += buf;
    std::string A = Operand(a, ar, vreg, creg, a0x);
    std::string B = Operand(b, br, vreg, creg, a0x);
    std::string C = Operand(c, cr, vreg, creg, a0x);
    bool paired = mac != MAC_NOP && ilu != ILU_NOP;
    if (mac != MAC_NOP) {
      std::string e;
      switch (mac) {
        case MAC_MOV: e = A; break;
        case MAC_MUL: e = A + " * " + B; break;
        case MAC_ADD: e = A + " + " + C; break;
        case MAC_MAD: e = A + " * " + B + " + " + C; break;
        case MAC_DP3: e = "float4(dot(" + A + ".xyz, " + B + ".xyz))"; break;
        case MAC_DPH: e = "float4(dot(" + A + ".xyz, " + B + ".xyz) + " + B + ".w)"; break;
        case MAC_DP4: e = "float4(dot(" + A + ", " + B + "))"; break;
        case MAC_DST:
          e = "float4(1, " + A + ".y * " + B + ".y, " + A + ".z, " + B + ".w)";
          break;
        case MAC_MIN: e = "min(" + A + ", " + B + ")"; break;
        case MAC_MAX: e = "max(" + A + ", " + B + ")"; break;
        case MAC_SLT: e = "select(float4(0), float4(1), " + A + " < " + B + ")"; break;
        case MAC_SGE: e = "select(float4(0), float4(1), " + A + " >= " + B + ")"; break;
        case MAC_ARL: e = A; break;
      }
      body += "    float4 mac = " + e + ";\n";
    }
    if (ilu != ILU_NOP) {
      std::string e, s = C + ".x";
      switch (ilu) {
        case ILU_MOV: e = C; break;
        case ILU_RCP: e = "float4(x_rcc(" + s + "))"; break;
        case ILU_RCC: e = "float4(x_rcc(" + s + "))"; break;
        case ILU_RSQ: e = "float4(rsqrt(abs(" + s + ")))"; break;
        case ILU_EXP: e = "x_exp(" + s + ")"; break;
        case ILU_LOG: e = "x_log(" + s + ")"; break;
        case ILU_LIT: e = "x_lit(" + C + ")"; break;
      }
      body += "    float4 ilu = " + e + ";\n";
    }
    // Writes.
    if (mac == MAC_ARL) {
      body += "    a0 = int(floor(mac.x + 0.001));\n";
    } else if (mac != MAC_NOP && mac_mask && !(paired && out_r == 1)) {
      std::string m = MaskStr(mac_mask);
      std::snprintf(buf, sizeof buf, "    R[%u].%s = mac.%s;\n", out_r, m.c_str(), m.c_str());
      body += buf;
    }
    if (ilu != ILU_NOP && ilu_mask) {
      std::string m = MaskStr(ilu_mask);
      std::snprintf(buf, sizeof buf, "    R[%u].%s = ilu.%s;\n", paired ? 1 : out_r, m.c_str(),
                    m.c_str());
      body += buf;
    }
    if (o_mask && ((out_mux == 0 && mac != MAC_NOP && mac != MAC_ARL) ||
                   (out_mux == 1 && ilu != ILU_NOP))) {
      std::string m = MaskStr(o_mask);
      const char* src = out_mux == 0 ? "mac" : "ilu";
      if (orb) {  // output register
        uint32_t o = out_addr & 0xF;
        if (o == 0)
          std::snprintf(buf, sizeof buf, "    R[12].%s = %s.%s;\n", m.c_str(), src, m.c_str());
        else
          std::snprintf(buf, sizeof buf, "    O[%u].%s = %s.%s;\n", o, m.c_str(), src, m.c_str());
      } else {  // constant register
        int ci = (((int(out_addr) >> 5) & 7) - 3) * 32 + (int(out_addr) & 31);
        writes_c = true;
        std::snprintf(buf, sizeof buf, "    { int i = %d + 96; if (i >= 0 && i < 192) CW[i].%s = %s.%s; }\n",
                      ci, m.c_str(), src, m.c_str());
      }
      body += buf;
    }
    body += "  }\n";
    if (final) break;
  }
  *instr_count = n + 1;

  std::string s = kMslCommon;
  s += kVsPrelude;
  s += R"(
vertex VOut vmain(uint vid [[vertex_id]], constant VsUniforms& u [[buffer(0)]],
                  device const uchar* s0 [[buffer(1)]], device const uchar* s1 [[buffer(2)]],
                  device const uchar* s2 [[buffer(3)]], device const uchar* s3 [[buffer(4)]]) {
  float4 V[16];
  for (int i = 0; i < 16; i++) {
    uint4 at = u.attr[i];
    device const uchar* sp = at.x == 0 ? s0 : at.x == 1 ? s1 : at.x == 2 ? s2 : s3;
    V[i] = at.x < 4 ? fetch(sp, vid * at.w + at.y, at.z) : float4(0, 0, 0, 1);
  }
  float4 R[13];
  for (int i = 0; i < 12; i++) R[i] = float4(0);
  R[12] = float4(0, 0, 0, 1);
  float4 O[13];
  for (int i = 0; i < 13; i++) O[i] = float4(0, 0, 0, 1);
  O[5] = float4(1);
  O[6] = float4(1);
  int a0 = 0;
)";
  if (writes_c) {
    s += "  float4 CW[192];\n  for (int i = 0; i < 192; i++) CW[i] = u.c[i];\n";
    s += "  #define CR(i) (((i) + 96) >= 0 && ((i) + 96) < 192 ? CW[(i) + 96] : float4(0))\n";
  } else {
    s += "  #define CR(i) (((i) + 96) >= 0 && ((i) + 96) < 192 ? u.c[(i) + 96] : float4(0))\n";
  }
  s += body;
  s += R"(
  VOut o;
  float4 p = R[12];
  // A 2D tile above 1x: its edges go where 1x coverage puts them (pixel
  // centres x0 <= c < x1), so tiles that overlap by half a pixel do not
  // blend twice at the seam.
  if (u.snap.x != 0) p.xy = ceil(p.xy) - 0.5;
  p.xyz = (p.xyz - u.offset.xyz) / u.scale.xyz;
  if (p.w == 0) p.w = 1;
  // Widescreen: the 4:3 view is drawn kWide narrower (3D shows more at the
  // sides, 2D stays 4:3 and centred); 2D vertices on the screen's left or
  // right edge (within ~3 of 640 pixels) stay there: full-screen quads stretch.
  // HUD anchoring (ndc_shift.w): corner elements follow their wide edge.
  if (u.ndc_shift.z != 0 && !(p.w == 1 && abs(p.x) >= 0.99)) p.x = p.x * u.ndc_shift.z + u.ndc_shift.w;
  p.xy += u.ndc_shift.xy;
  p.xyz *= p.w;
  o.pos = p;
  o.wz = p.w;
  o.d0 = saturate(O[3]);
  o.d1 = saturate(O[4]);
  o.b0 = saturate(O[7]);
  o.b1 = saturate(O[8]);
  o.fog = O[5].x;
  o.pts = max(O[6].x, 1.0);
  o.t0 = O[9];
  o.t1 = O[10];
  o.t2 = O[11];
  o.t3 = O[12];
  return o;
}
)";
  *msl = std::move(s);
  return true;
}

}  // namespace hp2d3d
