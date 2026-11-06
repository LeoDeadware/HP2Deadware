// squircle.swift <in.png> <out.png>: a macOS app icon (1024 px, Apple grid) from a game's art.
// Xbox title images are a circle on black: the circle's bounds are found (non-black pixels), the
// inscribed square is cropped, and drawn into the 824 px rounded square of the Big Sur+ grid with a
// transparent margin, so the icon sits in the Finder/Dock container like native apps.
// Square art without a black surround is used whole. CoreGraphics only (CLT swift).
import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

let a = CommandLine.arguments
guard a.count == 3,
      let src = CGImageSourceCreateWithURL(URL(fileURLWithPath: a[1]) as CFURL, nil),
      let img = CGImageSourceCreateImageAtIndex(src, 0, nil) else {
  FileHandle.standardError.write("usage: squircle.swift in.png out.png\n".data(using: .utf8)!); exit(2)
}
let w = img.width, h = img.height
// Read pixels as RGBA8.
let cs = CGColorSpace(name: CGColorSpace.sRGB)!
var buf = [UInt8](repeating: 0, count: w * h * 4)
let rd = CGContext(data: &buf, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4, space: cs,
                   bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
rd.draw(img, in: CGRect(x: 0, y: 0, width: w, height: h))
func lit(_ x: Int, _ y: Int) -> Bool {
  let i = (y * w + x) * 4
  return buf[i + 3] > 40 && Int(buf[i]) + Int(buf[i + 1]) + Int(buf[i + 2]) > 60
}
// Bounds of the art (rows/columns with enough lit pixels, so stray noise does not count).
var x0 = w, x1 = -1, y0 = h, y1 = -1
for y in 0..<h { var n = 0; for x in 0..<w where lit(x, y) { n += 1 }; if n > w / 50 { y0 = min(y0, y); y1 = max(y1, y) } }
for x in 0..<w { var n = 0; for y in 0..<h where lit(x, y) { n += 1 }; if n > h / 50 { x0 = min(x0, x); x1 = max(x1, x) } }
if x1 < 0 { x0 = 0; y0 = 0; x1 = w - 1; y1 = h - 1 }
let bw = Double(x1 - x0 + 1), bh = Double(y1 - y0 + 1)
let cx = Double(x0) + bw / 2, cy = Double(y0) + bh / 2
// Round art: the corners of its bounding box are black. Then crop the inscribed square (+2% so
// no edge shows after the rounded mask trims the corners).
let round = !lit(x0 + Int(bw * 0.06), y0 + Int(bh * 0.06)) && !lit(x1 - Int(bw * 0.06), y1 - Int(bh * 0.06))
let side = round ? min(bw, bh) / 2.0.squareRoot() * 1.02 : min(bw, bh)
// CGImage cropping is top-left based; our scan is top-left too (buffer row 0 = top after draw? no:
// CGContext rows start at the top of memory = top of the image), so use the coordinates as is.
let crop = CGRect(x: cx - side / 2, y: cy - side / 2, width: side, height: side).integral
guard let art = img.cropping(to: crop) else { exit(1) }

let N = 1024, body = 824.0, inset = (Double(N) - body) / 2, radius = body * 0.225
let out = CGContext(data: nil, width: N, height: N, bitsPerComponent: 8, bytesPerRow: 0, space: cs,
                    bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
out.interpolationQuality = .high
let r = CGRect(x: inset, y: inset, width: body, height: body)
out.addPath(CGPath(roundedRect: r, cornerWidth: radius, cornerHeight: radius, transform: nil))
out.clip()
out.draw(art, in: r)
let dst = CGImageDestinationCreateWithURL(URL(fileURLWithPath: a[2]) as CFURL, UTType.png.identifier as CFString, 1, nil)!
CGImageDestinationAddImage(dst, out.makeImage()!, nil)
exit(CGImageDestinationFinalize(dst) ? 0 : 1)
