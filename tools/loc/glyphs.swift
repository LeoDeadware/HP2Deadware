// Renders glyphs with CoreText for tools/loc/geo_font.py.
//
//   swift glyphs.swift <font name or .ttf path> <cap height px> <cell height px> <baseline row> <out.bin> <code>...
//
// Font size is chosen so the font's cap height is <cap height> pixels; the
// baseline sits on row <baseline row> of a <cell height> tall cell. Each
// glyph cell is round(advance) + 1 pixels wide. Output (little-endian):
//   u32 count; per glyph: u32 code, u32 width, u32 height, width*height bytes coverage (0..255, rows top-down)
import CoreGraphics
import CoreText
import Foundation

let a = CommandLine.arguments
guard a.count >= 7 else {
  FileHandle.standardError.write("usage: glyphs.swift font cap cell baseline out.bin code...\n".data(using: .utf8)!)
  exit(2)
}
let fontArg = a[1]
let cap = Double(a[2])!, cell = Int(a[3])!, baseline = Double(a[4])!
let outPath = a[5]
let codes = a[6...].map { UInt32($0)! }

func makeFont(_ size: CGFloat) -> CTFont {
  if fontArg.hasSuffix(".ttf") || fontArg.hasSuffix(".otf") || fontArg.hasSuffix(".ttc") {
    let url = URL(fileURLWithPath: fontArg) as CFURL
    if let descs = CTFontManagerCreateFontDescriptorsFromURL(url) as? [CTFontDescriptor], let d = descs.first {
      return CTFontCreateWithFontDescriptor(d, size, nil)
    }
  }
  return CTFontCreateWithName(fontArg as CFString, size, nil)
}
let probe = makeFont(100)
let font = makeFont(CGFloat(100 * cap / Double(CTFontGetCapHeight(probe))))

var out = Data()
func u32(_ v: UInt32) { var x = v.littleEndian; out.append(Data(bytes: &x, count: 4)) }
u32(UInt32(codes.count))
for code in codes {
  var ch = [UniChar(code)]
  var g: CGGlyph = 0
  var f = font
  if !CTFontGetGlyphsForCharacters(f, &ch, &g, 1) {
    // Not in this font (e.g. U+02BC): the system's fallback at the same size.
    f = CTFontCreateForString(font, String(utf16CodeUnits: ch, count: 1) as CFString, CFRange(location: 0, length: 1))
    guard CTFontGetGlyphsForCharacters(f, &ch, &g, 1) else {
      FileHandle.standardError.write("no glyph for U+\(String(code, radix: 16))\n".data(using: .utf8)!)
      exit(1)
    }
    FileHandle.standardError.write("U+\(String(code, radix: 16)) from \(CTFontCopyPostScriptName(f))\n".data(using: .utf8)!)
  }
  var adv = CGSize.zero
  CTFontGetAdvancesForGlyphs(f, .horizontal, &g, &adv, 1)
  let w = Int(adv.width.rounded()) + 1, h = cell
  var px = [UInt8](repeating: 0, count: w * h)
  let ctx = CGContext(data: &px, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w,
                      space: CGColorSpaceCreateDeviceGray(), bitmapInfo: CGImageAlphaInfo.none.rawValue)!
  ctx.setFillColor(gray: 1, alpha: 1)
  ctx.setShouldAntialias(true)
  ctx.setShouldSmoothFonts(false)
  var pos = CGPoint(x: 0.5, y: Double(h) - baseline)  // CG origin is bottom-left
  CTFontDrawGlyphs(f, &g, &pos, 1, ctx)
  u32(code); u32(UInt32(w)); u32(UInt32(h))
  out.append(contentsOf: px)  // bitmap memory is top row first
}
try! out.write(to: URL(fileURLWithPath: outPath))
