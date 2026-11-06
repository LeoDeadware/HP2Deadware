#pragma once
// «TV 2002» post-processing shaders (MSL, compiled at run time by postfx.mm).
//
// Passes: prefilter (soft-knee bright pass in linear light, source -> 1/4 of
// the output's fit rect), a mip chain (4 downsamples to 1/64, 4 tent
// upsamples summing the levels; wider levels warmer = red halation),
// composite (CRT scanlines + aperture grille, grade, bloom added in linear
// light with a soft highlight shoulder, vignette, grain, dither) into the
// drawable's fit rect. Strength 0 of an effect = its code is skipped.

namespace hp2polish {

inline const char* kPostFxMsl = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct Params {
  float4 src_uv;      // source rect in uv (x0, y0, x1, y1)
  float2 src_size;    // source texture size in texels (whole texture)
  float2 vp_origin;   // fit rect in drawable pixels
  float2 vp_size;
  float2 bloom_size;  // size of the chain's first level (1/4 of the fit rect)
  float lines;        // source scanlines (480)
  float grade, bloom, grain, vignette, crt;
  float2 grain_off;   // per-frame offset into the noise texture (texels)
  float bloom_norm;   // 1 / sum of the chain's level weights
  float dither;       // 1: dither the 8-bit output
  float frame;        // frame counter (dither animation)
  float pad;
};

// One step of the bloom mip chain.
struct Chain {
  float2 in_texel;     // 1 / size of the input (down: the larger level; up: the smaller one)
  float2 pad;
  float4 mul_big;      // up: weight x tint of the level being added (rgb)
  float4 mul_small;    // up: multiplier of the upsampled smaller level (rgb)
};

struct VOut { float4 pos [[position]]; float2 t; };  // t: 0..1 over the drawn rect

vertex VOut pf_vs(uint vid [[vertex_id]]) {
  float2 t = float2((vid << 1) & 2, vid & 2);  // full-screen triangle
  VOut o;
  o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
  o.t = t;
  return o;
}

float luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

float2 src_uv(constant Params& p, float2 t) { return mix(p.src_uv.xy, p.src_uv.zw, t); }

// ---- bloom: soft-knee bright pass at 1/4 of the fit rect (4 bilinear
// taps = 4x4 box), in linear light. A small share of every pixel (haze)
// gives the air a faint glow; highlights carry the rest.
fragment float4 pf_prefilter(VOut i [[stage_in]], constant Params& p [[buffer(0)]],
                             texture2d<float> src [[texture(0)]], sampler s [[sampler(0)]]) {
  float2 uv = src_uv(p, i.t);
  float2 d = 0.25 * (p.src_uv.zw - p.src_uv.xy) / p.bloom_size;  // a quarter bloom texel
  float3 c = 0;
  for (int k = 0; k < 4; k++) {
    float3 x = src.sample(s, uv + d * float2((k & 1) ? 1 : -1, (k & 2) ? 1 : -1)).rgb;
    c += x * x * sqrt(x);  // ~ pow 2.5 per tap, so a lone bright texel is not averaged away first
  }
  c *= 0.25;
  float m = max(c.r, max(c.g, c.b));
  float k = 0.04 + 0.96 * smoothstep(0.25, 0.9, m);
  return float4(c * k, 1);
}

// Downsample (input 2x the output): centre x4 + 4 diagonal taps one input
// texel out (bilinear: a 4x4 footprint), / 8.
fragment float4 pf_down(VOut i [[stage_in]], constant Chain& q [[buffer(0)]],
                        texture2d<float> b [[texture(0)]], sampler s [[sampler(0)]]) {
  float2 o = q.in_texel;
  float3 c = 4 * b.sample(s, i.t).rgb;
  c += b.sample(s, i.t + float2(-o.x, -o.y)).rgb + b.sample(s, i.t + float2(o.x, -o.y)).rgb;
  c += b.sample(s, i.t + float2(-o.x, o.y)).rgb + b.sample(s, i.t + float2(o.x, o.y)).rgb;
  return float4(c / 8, 1);
}

// Upsample: 3x3 tent of the smaller level (radius one of its texels), plus
// this level weighted and tinted.
fragment float4 pf_up(VOut i [[stage_in]], constant Chain& q [[buffer(0)]],
                      texture2d<float> big [[texture(0)]], texture2d<float> small [[texture(1)]],
                      sampler s [[sampler(0)]]) {
  float2 o = q.in_texel;
  float3 t = 4 * small.sample(s, i.t).rgb;
  t += 2 * (small.sample(s, i.t + float2(o.x, 0)).rgb + small.sample(s, i.t - float2(o.x, 0)).rgb +
            small.sample(s, i.t + float2(0, o.y)).rgb + small.sample(s, i.t - float2(0, o.y)).rgb);
  t += small.sample(s, i.t + o).rgb + small.sample(s, i.t - o).rgb +
       small.sample(s, i.t + float2(o.x, -o.y)).rgb + small.sample(s, i.t + float2(-o.x, o.y)).rgb;
  return float4(t / 16 * q.mul_small.rgb + big.sample(s, i.t).rgb * q.mul_big.rgb, 1);
}

// ---- composite
// "Xbox on a CRT": the set's gamma ~2.5 shown on a 2.2 display (slightly
// deeper mids), more saturation and contrast in the shadows, a warm white.
float3 grade(float3 c, float k) {
  float3 g = pow(max(c, 0.0), 2.5 / 2.2);
  float l = luma(g);
  float shadow = 1 - smoothstep(0.05, 0.5, l);
  g = mix(float3(l), g, 1 + 0.18 * shadow);           // saturation in the shadows
  g = mix(g, g * g * (3 - 2 * g), 0.25 * shadow);     // contrast in the shadows
  g *= float3(1.03, 1.0, 0.93);                       // warm white point (~5800K)
  return mix(c, saturate(g), k);
}

// Weight of one scanline (centre distance d, in lines) integrated over the
// output pixel's footprint [d - h/2, d + h/2] (h = lines per pixel): an
// area-sampled beam, so no moire at any output size. Beam width grows with
// brightness; the weight is normalised to a mean of 1 over a line period.
float erf_approx(float x) { return fast::tanh(x * (1.1283792 + 0.1009 * x * x)); }  // |err| < 4e-3
float beam(float d, float h, float bright) {
  float sigma = mix(0.20, 0.36, bright);
  float q = 1 / (sigma * 1.41421356);
  return 0.5 * (erf_approx((d + 0.5 * h) * q) - erf_approx((d - 0.5 * h) * q)) / h;
}

// Slight horizontal softness of the CRT's beam / analog bandwidth: two
// bilinear taps half a step apart.
float3 sample_soft(texture2d<float> src, sampler s, float2 uv, float dx) {
  return 0.5 * (src.sample(s, uv - float2(dx, 0)).rgb + src.sample(s, uv + float2(dx, 0)).rgb);
}

fragment float4 pf_composite(VOut i [[stage_in]], constant Params& p [[buffer(0)]],
                             texture2d<float> src [[texture(0)]], texture2d<float> blm [[texture(1)]],
                             texture2d<float> noise [[texture(2)]], sampler s [[sampler(0)]],
                             sampler sw [[sampler(1)]]) {
  float2 t = (i.pos.xy - p.vp_origin) / p.vp_size;  // 0..1 over the fit rect
  float2 uv = src_uv(p, t);
  float3 c;
  if (p.crt > 0) {
    // Two nearest scanlines (centres in source line units), in linear light
    // (gamma 2 approximation: square / sqrt), weighted by the area-sampled beam.
    float y = t.y * p.lines;
    float c0 = floor(y - 0.5) + 0.5;
    float h = p.lines / p.vp_size.y;
    float vline = (p.src_uv.w - p.src_uv.y) / p.lines;
    float dx = 0.3 * p.crt * (p.src_uv.z - p.src_uv.x) / (p.lines * 4.0 / 3.0);
    float3 a = sample_soft(src, s, float2(uv.x, p.src_uv.y + c0 * vline), dx);
    float3 b = sample_soft(src, s, float2(uv.x, p.src_uv.y + (c0 + 1) * vline), dx);
    float3 flat = mix(a, b, y - c0);
    float la = luma(a), lb = luma(b);  // beam width from the gamma-space brightness
    a *= a, b *= b;
    float3 lines = a * beam(y - c0, h, la) + b * beam(y - c0 - 1, h, lb);
    // Lines fade out where they would be finer than ~1.5 output pixels.
    float k = p.crt * smoothstep(1.2, 2.2, 1 / h);
    float3 lc = mix(flat * flat, lines, k);
    // Aperture grille: RGB stripes over 3 output pixels; mask depth eased
    // on bright pixels and the mean brought back to 1 (brightness kept).
    float m = p.crt * mix(0.45, 0.2, luma(flat));
    uint ph = uint(i.pos.x) % 3;
    float3 mask = select(float3(1 - m), float3(1), uint3(ph) == uint3(0, 1, 2));
    c = sqrt(lc * mask / (1 - 2 * m / 3));
  } else {
    c = src.sample(s, uv).rgb;
  }
  if (p.grade > 0) c = grade(c, p.grade);
  if (p.bloom > 0) {
    // Added in linear light (energy kept: light spreads, it is not painted
    // over); highlights then roll off on a soft shoulder instead of clipping.
    // (Linear ~ gamma 2: square / sqrt; the shoulder is rational, no exp.)
    float3 lin = c * c + p.bloom * p.bloom_norm * blm.sample(s, t).rgb;
    const float kn = 0.8;
    float3 over = max(lin - kn, 0.0);
    lin = min(lin, kn) + (1 - kn) * over / (over + (1 - kn));
    c = sqrt(lin);
  }
  if (p.vignette > 0) {
    float2 q = (t - 0.5) * float2(p.vp_size.x / p.vp_size.y, 1);
    c *= 1 - p.vignette * 0.55 * smoothstep(0.08, 0.75, dot(q, q));
  }
  if (p.grain > 0) {
    // Film grain: a tiling noise texture (bilinear = value noise), cells
    // ~1/600 of the picture height, jumping every frame; strongest in the
    // mid tones (none on black and white).
    float cell = max(1.0, p.vp_size.y / 600);
    float n = noise.sample(sw, ((i.pos.xy - p.vp_origin) / cell + p.grain_off) / 256).r - 0.5;
    float l = luma(c);
    c += p.grain * 0.09 * n * 4 * l * (1 - l);
  }
  if (p.dither > 0) {
    // Triangular dither of +-1 LSB (two interleaved-gradient noises,
    // animated): no bands in the dark gradients the deeper gamma brings.
    float2 q = i.pos.xy + 5.588238 * fmod(p.frame, 64.0);
    float n1 = fract(52.9829189 * fract(dot(q, float2(0.06711056, 0.00583715))));
    float n2 = fract(52.9829189 * fract(dot(q + float2(17.3, 41.1), float2(0.06711056, 0.00583715))));
    c += (n1 + n2 - 1) / 255;
  }
  return float4(saturate(c), 1);
}
)MSL";

}  // namespace hp2polish
