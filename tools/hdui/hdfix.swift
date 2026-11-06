// Post-processing of an upscaled UI texture for the HD UI pack (tools/hdui/build_pack.py).
//
//   hdfix split <original.png> <rgb.png> <alpha.png>
//     model inputs: the colour, opaque, bled under transparent texels (no dark
//     rims for the model to sharpen), and the alpha as grey;
//   hdfix join <original.png> <upscaled_rgb.png> <upscaled_alpha.png> <out.png> [--ink]
//
// original: the game's texture (w x h, straight alpha); upscaled_rgb: the
// model's k-times result of the colour (alpha ignored); upscaled_alpha: the
// model's k-times result of the alpha rendered as grey (red channel used).
// Output (k*w x k*h, straight alpha):
//   - alpha from upscaled_alpha, pixels where the original is fully opaque /
//     transparent over a 3x3 neighbourhood kept exactly opaque / transparent
//     (no model fuzz on solid areas or empty space);
//   - colour corrected per k x k cell so that its alpha-weighted mean equals
//     the original pixel (the result shrunk back = the original: tones and
//     palette exactly the game's, only the detail is new);
//   - colour bled from covered pixels into transparent ones (no dark rims
//     under bilinear filtering).
// --ink: no per-cell correction (line art / text where the model's sharper
//   edges matter more than matching each texel).
//   hdfix pad <in.png> <out.png> <px> [wrap|clamp|auto]
//     a border of px texels on every side: wrapped around (a tiling texture:
//     the model then sees across the seam, so the result tiles too) or edge
//     texels repeated; auto = wrap when the texture tiles (its opposite edges
//     differ no more than neighbouring texel rows do). Prints the mode used.
//   hdfix seam <in.png> [k]
//     prints the wrap seam's mean difference over the mean difference across
//     the other texel boundaries (k: the image is a k-times upscale; about 1
//     or less: the texture tiles without a visible seam).
//   hdfix crop <in.png> <out.png> <px> [--half]
//     removes a px border; --half then shrinks 2x (2x2 box, alpha-weighted).
import CoreGraphics
import Foundation
import ImageIO

struct Img { var w: Int; var h: Int; var px: [Float] }  // RGBA 0..1, straight alpha

func load(_ path: String) -> Img {
  let url = URL(fileURLWithPath: path) as CFURL
  guard let src = CGImageSourceCreateWithURL(url, nil), let img = CGImageSourceCreateImageAtIndex(src, 0, nil) else {
    FileHandle.standardError.write("cannot read \(path)\n".data(using: .utf8)!); exit(1)
  }
  let w = img.width, h = img.height
  var b = [UInt8](repeating: 0, count: w * h * 4)
  let ctx = CGContext(data: &b, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                      space: CGColorSpace(name: CGColorSpace.sRGB)!,
                      bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
  ctx.setBlendMode(.copy)
  ctx.draw(img, in: CGRect(x: 0, y: 0, width: w, height: h))
  var px = [Float](repeating: 0, count: w * h * 4)
  for i in 0..<(w * h) {
    let a = Float(b[4 * i + 3]) / 255
    for c in 0..<3 { px[4 * i + c] = a > 0 ? min(1, Float(b[4 * i + c]) / 255 / a) : 0 }
    px[4 * i + 3] = a
  }
  return Img(w: w, h: h, px: px)
}

func save(_ m: Img, _ path: String) {
  var b = [UInt8](repeating: 0, count: m.w * m.h * 4)
  for i in 0..<(m.w * m.h * 4) { b[i] = UInt8(max(0, min(255, (m.px[i] * 255).rounded()))) }
  let dp = CGDataProvider(data: Data(b) as CFData)!
  let img = CGImage(width: m.w, height: m.h, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: m.w * 4,
                    space: CGColorSpace(name: CGColorSpace.sRGB)!,
                    bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.last.rawValue),
                    provider: dp, decode: nil, shouldInterpolate: false, intent: .defaultIntent)!
  let dst = CGImageDestinationCreateWithURL(URL(fileURLWithPath: path) as CFURL, "public.png" as CFString, 1, nil)!
  CGImageDestinationAddImage(dst, img, nil)
  CGImageDestinationFinalize(dst)
}

// Fills colour of texels with alpha <= 0.02 from covered neighbours, `rings` deep
// (then the mean colour, so nothing is left black).
func bleed(_ m: inout Img, rings: Int) {
  var filled = (0..<(m.w * m.h)).map { m.px[4 * $0 + 3] > 0.02 }
  for _ in 0..<rings {
    var next = m.px, nfill = filled
    var changed = false
    for y in 0..<m.h { for x in 0..<m.w {
      let i = y * m.w + x
      if filled[i] { continue }
      var s = [Float](repeating: 0, count: 3); var n: Float = 0
      for (dx, dy) in [(1, 0), (-1, 0), (0, 1), (0, -1)] {
        let xx = x + dx, yy = y + dy
        if xx < 0 || yy < 0 || xx >= m.w || yy >= m.h { continue }
        let j = yy * m.w + xx
        if filled[j] { for c in 0..<3 { s[c] += m.px[4 * j + c] }; n += 1 }
      }
      if n > 0 { for c in 0..<3 { next[4 * i + c] = s[c] / n }; nfill[i] = true; changed = true }
    } }
    m.px = next; filled = nfill
    if !changed { break }
  }
}

func at(_ m: Img, _ x: Int, _ y: Int) -> Int { 4 * (y * m.w + x) }

// Mean colour difference across the wrap seam over that across the other
// texel boundaries (every k-th pixel column/row of a k-times upscale: inside
// a texel neighbouring pixels are smooth, comparing with them would make
// any upscale look seamed). Flat textures (mean boundary difference under
// 0.02) are measured against 0.02.
func seamRatio(_ m: Img, _ k: Int = 1) -> Float {
  // premultiplied: colour under alpha 0 is invisible (posters with
  // transparent borders must not count as seamed)
  func d(_ i: Int, _ j: Int) -> Float {
    var s = abs(m.px[i + 3] - m.px[j + 3])
    for c in 0..<3 { s += abs(m.px[i + c] * m.px[i + 3] - m.px[j + c] * m.px[j + 3]) }
    return s
  }
  var seam: Float = 0, inner: Float = 0, ns = 0, ni = 0
  for y in 0..<m.h {
    seam += d(at(m, 0, y), at(m, m.w - 1, y)); ns += 1
    for x in stride(from: k, to: m.w, by: k) { inner += d(at(m, x - 1, y), at(m, x, y)); ni += 1 }
  }
  for x in 0..<m.w {
    seam += d(at(m, x, 0), at(m, x, m.h - 1)); ns += 1
    for y in stride(from: k, to: m.h, by: k) { inner += d(at(m, x, y - 1), at(m, x, y)); ni += 1 }
  }
  return (seam / Float(max(ns, 1))) / max(inner / Float(max(ni, 1)), 0.02)
}
func tiles(_ m: Img) -> Bool { seamRatio(m) <= 1.5 }

let a = CommandLine.arguments
if a.count >= 3 && a[1] == "seam" {
  print(String(format: "%.3f", seamRatio(load(a[2]), a.count > 3 ? Int(a[3])! : 1)))
  exit(0)
}
if a.count >= 5 && a[1] == "pad" {
  let m = load(a[2]), p = Int(a[4])!
  var mode = a.count > 5 ? a[5] : "auto"
  if mode == "auto" { mode = tiles(m) ? "wrap" : "clamp" }
  var o = Img(w: m.w + 2 * p, h: m.h + 2 * p, px: [Float](repeating: 0, count: (m.w + 2 * p) * (m.h + 2 * p) * 4))
  for y in 0..<o.h { for x in 0..<o.w {
    var sx = x - p, sy = y - p
    if mode == "wrap" { sx = ((sx % m.w) + m.w) % m.w; sy = ((sy % m.h) + m.h) % m.h }
    else { sx = min(max(sx, 0), m.w - 1); sy = min(max(sy, 0), m.h - 1) }
    for c in 0..<4 { o.px[at(o, x, y) + c] = m.px[at(m, sx, sy) + c] }
  } }
  save(o, a[3])
  print(mode)
  exit(0)
}
if a.count >= 5 && a[1] == "crop" {
  let m = load(a[2]), p = Int(a[4])!
  var o = Img(w: m.w - 2 * p, h: m.h - 2 * p, px: [Float](repeating: 0, count: (m.w - 2 * p) * (m.h - 2 * p) * 4))
  for y in 0..<o.h { for x in 0..<o.w { for c in 0..<4 { o.px[at(o, x, y) + c] = m.px[at(m, x + p, y + p) + c] } } }
  if a.contains("--half") {
    var hlf = Img(w: o.w / 2, h: o.h / 2, px: [Float](repeating: 0, count: (o.w / 2) * (o.h / 2) * 4))
    for y in 0..<hlf.h { for x in 0..<hlf.w {
      var s = [Float](repeating: 0, count: 4)
      for (dx, dy) in [(0, 0), (1, 0), (0, 1), (1, 1)] {
        let i = at(o, 2 * x + dx, 2 * y + dy), al = o.px[i + 3]
        for c in 0..<3 { s[c] += o.px[i + c] * al }
        s[3] += al
      }
      let j = at(hlf, x, y)
      for c in 0..<3 { hlf.px[j + c] = s[3] > 0 ? s[c] / s[3] : o.px[at(o, 2 * x, 2 * y) + c] }
      hlf.px[j + 3] = s[3] / 4
    } }
    o = hlf
  }
  save(o, a[3])
  exit(0)
}
if a.count == 5 && a[1] == "split" {
  var m = load(a[2])
  var alpha = Img(w: m.w, h: m.h, px: [Float](repeating: 1, count: m.w * m.h * 4))
  for i in 0..<(m.w * m.h) { for c in 0..<3 { alpha.px[4 * i + c] = m.px[4 * i + 3] } }
  bleed(&m, rings: max(m.w, m.h))
  for i in 0..<(m.w * m.h) { m.px[4 * i + 3] = 1 }
  save(m, a[3]); save(alpha, a[4])
  exit(0)
}
guard a.count >= 6, a[1] == "join" else { FileHandle.standardError.write("usage: hdfix split orig rgb alpha | hdfix join orig rgb alpha out [--ink]\n".data(using: .utf8)!); exit(2) }
let ink = a.contains("--ink")
let o = load(a[2]), rgb = load(a[3]), al = load(a[4])
let k = rgb.w / o.w
guard k >= 1, rgb.w == k * o.w, rgb.h == k * o.h, al.w == rgb.w, al.h == rgb.h else {
  FileHandle.standardError.write("size mismatch: \(o.w)x\(o.h) vs \(rgb.w)x\(rgb.h) / \(al.w)x\(al.h)\n".data(using: .utf8)!); exit(1)
}
var out = Img(w: rgb.w, h: rgb.h, px: [Float](repeating: 0, count: rgb.w * rgb.h * 4))

func oa(_ x: Int, _ y: Int) -> Float { o.px[4 * (min(max(y, 0), o.h - 1) * o.w + min(max(x, 0), o.w - 1)) + 3] }

// Alpha
for y in 0..<out.h {
  for x in 0..<out.w {
    let sx = x / k, sy = y / k
    var lo: Float = 1, hi: Float = 0
    for dy in -1...1 { for dx in -1...1 { let v = oa(sx + dx, sy + dy); lo = min(lo, v); hi = max(hi, v) } }
    var v = al.px[4 * (y * out.w + x)]
    if lo >= 0.999 { v = 1 } else if hi <= 0.001 { v = 0 }
    out.px[4 * (y * out.w + x) + 3] = max(0, min(1, v))
  }
}
// Colour: the model's, plus a smooth correction field: per original texel,
// (original - alpha-weighted mean of its k x k cell), interpolated
// bilinearly at full size (a per-cell offset would bring the blocks back).
var diff = [Float](repeating: 0, count: o.w * o.h * 3)
for sy in 0..<o.h {
  for sx in 0..<o.w {
    var mean = [Float](repeating: 0, count: 3); var wsum: Float = 0
    for y in (sy * k)..<(sy * k + k) { for x in (sx * k)..<(sx * k + k) {
      let i = y * out.w + x, wa = max(out.px[4 * i + 3], 1e-3)
      for c in 0..<3 { mean[c] += rgb.px[4 * i + c] * wa }
      wsum += wa
    } }
    for c in 0..<3 { diff[3 * (sy * o.w + sx) + c] = o.px[4 * (sy * o.w + sx) + c] - mean[c] / wsum }
  }
}
// Only broad tone shifts: the field is smoothed (3 passes of a 3x3 box, ~ a
// gaussian of 1.4 texels) so the DXT block noise of the original is not copied.
for _ in 0..<3 {
  var next = diff
  for sy in 0..<o.h { for sx in 0..<o.w { for c in 0..<3 {
    var s: Float = 0, n: Float = 0
    for dy in -1...1 { for dx in -1...1 {
      let xx = sx + dx, yy = sy + dy
      if xx < 0 || yy < 0 || xx >= o.w || yy >= o.h { continue }
      s += diff[3 * (yy * o.w + xx) + c]; n += 1
    } }
    next[3 * (sy * o.w + sx) + c] = s / n
  } } }
  diff = next
}
func d(_ x: Int, _ y: Int, _ c: Int) -> Float { diff[3 * (min(max(y, 0), o.h - 1) * o.w + min(max(x, 0), o.w - 1)) + c] }
for y in 0..<out.h {
  let fy = (Float(y) + 0.5) / Float(k) - 0.5, y0 = Int(fy.rounded(.down)), ty = fy - Float(y0)
  for x in 0..<out.w {
    let fx = (Float(x) + 0.5) / Float(k) - 0.5, x0 = Int(fx.rounded(.down)), tx = fx - Float(x0)
    let i = y * out.w + x
    for c in 0..<3 {
      let corr = (1 - ty) * ((1 - tx) * d(x0, y0, c) + tx * d(x0 + 1, y0, c)) + ty * ((1 - tx) * d(x0, y0 + 1, c) + tx * d(x0 + 1, y0 + 1, c))
      out.px[4 * i + c] = max(0, min(1, rgb.px[4 * i + c] + (ink ? 0 : corr)))
    }
  }
}
bleed(&out, rings: 2 * k)
save(out, a[5])
