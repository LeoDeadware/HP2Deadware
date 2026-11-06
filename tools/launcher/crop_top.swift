// crop_top <in.png> <out.png> <fraction>: keeps the top <fraction> of the image (the launcher art:
// the title logo without the menu below it). tools/launcher/make_art.sh.
import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

let a = CommandLine.arguments
guard a.count == 4, let frac = Double(a[3]),
      let src = CGImageSourceCreateWithURL(URL(fileURLWithPath: a[1]) as CFURL, nil),
      let img = CGImageSourceCreateImageAtIndex(src, 0, nil) else { print("usage: crop_top in out fraction"); exit(2) }
let h = Int(Double(img.height) * frac)
guard let out = img.cropping(to: CGRect(x: 0, y: 0, width: img.width, height: h)),
      let dst = CGImageDestinationCreateWithURL(URL(fileURLWithPath: a[2]) as CFURL, UTType.png.identifier as CFString, 1, nil)
else { exit(1) }
CGImageDestinationAddImage(dst, out, nil)
exit(CGImageDestinationFinalize(dst) ? 0 : 1)
