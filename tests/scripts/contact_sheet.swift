// contact_sheet <out.png> <cols> <cell_width> <png...>: a grid of the shots (labels: file names),
// to review many frames of a run at once (tests/scripts/site_shots.sh).
import AppKit
let a = CommandLine.arguments
guard a.count >= 5, let cols = Int(a[2]), let cw = Double(a[3]) else { print("usage: contact_sheet out.png cols cell_w png..."); exit(2) }
let files = Array(a[4...])
let imgs = files.compactMap { NSImage(contentsOfFile: $0) }
guard let first = imgs.first else { exit(1) }
let ch = cw * first.size.height / first.size.width
let rows = (imgs.count + cols - 1) / cols
let W = Int(cw) * cols, H = Int(ch + 18) * rows
let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: W, pixelsHigh: H, bitsPerSample: 8, samplesPerPixel: 4,
                           hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
NSColor.black.setFill(); NSRect(x: 0, y: 0, width: W, height: H).fill()
for (i, im) in imgs.enumerated() {
  let x = Double(i % cols) * cw, y = Double(H) - Double(i / cols + 1) * (ch + 18)
  im.draw(in: NSRect(x: x, y: y, width: cw, height: ch))
  let name = (files[i] as NSString).lastPathComponent as NSString
  name.draw(at: NSPoint(x: x + 4, y: y + ch + 2), withAttributes: [.foregroundColor: NSColor.white, .font: NSFont.systemFont(ofSize: 12)])
}
NSGraphicsContext.restoreGraphicsState()
try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: a[1]))
