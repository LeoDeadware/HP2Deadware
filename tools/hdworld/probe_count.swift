// probe_count <shot.png>...: per screenshot of an HP2_HDUI_PROBE=1 run, the
// number of pixels per texture code (see src/polish/texpack/texpack.mm):
// lines "<png>\t<code>\t<pixels>\t<total pixels>". Pixels too dark (B < 48)
// or tinted off a code's bin centre (more than PROBE_TOL, default 2.5) are not
// counted (code 0 = rejected).
import CoreGraphics
import Foundation
import ImageIO

let tol = Float(ProcessInfo.processInfo.environment["PROBE_TOL"] ?? "") ?? 2.5
for path in CommandLine.arguments.dropFirst() {
  let url = URL(fileURLWithPath: path) as CFURL
  guard let src = CGImageSourceCreateWithURL(url, nil), let img = CGImageSourceCreateImageAtIndex(src, 0, nil) else {
    FileHandle.standardError.write("cannot read \(path)\n".data(using: .utf8)!); continue
  }
  let w = img.width, h = img.height
  var b = [UInt8](repeating: 0, count: w * h * 4)
  let ctx = CGContext(data: &b, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                      space: CGColorSpace(name: CGColorSpace.sRGB)!,
                      bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)!
  ctx.draw(img, in: CGRect(x: 0, y: 0, width: w, height: h))
  var count = [Int](repeating: 0, count: 1024)
  for i in 0..<(w * h) {
    let bl = Float(b[4 * i + 2])
    if bl < 48 { count[0] += 1; continue }
    let r = Float(b[4 * i]) * 255 / bl, g = Float(b[4 * i + 1]) * 255 / bl
    let rq = Int(r / 8), gq = Int(g / 8)
    // within tol of the bin centre 8k+4 in both channels
    if rq > 31 || gq > 31 || abs(r - Float(8 * rq + 4)) > tol || abs(g - Float(8 * gq + 4)) > tol { count[0] += 1; continue }
    count[rq + 32 * gq] += 1
  }
  var out = ""
  for (c, n) in count.enumerated() where n > 0 { out += "\(path)\t\(c)\t\(n)\t\(w * h)\n" }
  FileHandle.standardOutput.write(out.data(using: .utf8)!)
}
