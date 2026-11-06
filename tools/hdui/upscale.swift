// Local x2 / x4 upscaler for the HD packs (tools/hdui/build_pack.py, tools/hdworld/build_world.py)
// when Real-ESRGAN is not installed: no download, no model files.
//
//   upscale <in_dir> <out_dir> <k> [metalfx|lanczos]
//
// Every <name>.png of in_dir (opaque colour or grey alpha from `hdfix split`) -> out_dir/<name>.png,
// k (2 or 4) times larger. metalfx (default): Apple's MetalFX spatial scaler (edge-adaptive
// upsampling + sharpening, the FSR1 family), run as 2x steps on 16-bit float textures;
// lanczos: Core Image Lanczos + a light unsharp mask (fallback when MetalFX is not supported).
// The calling tool then joins colour and alpha and tone-corrects to the original (hdfix join).
import CoreGraphics
import CoreImage
import Foundation
import ImageIO
import Metal
import MetalFX

func die(_ s: String) -> Never {
  FileHandle.standardError.write((s + "\n").data(using: .utf8)!)
  exit(1)
}

let args = CommandLine.arguments
if args.count < 4 { die("usage: upscale <in_dir> <out_dir> <k> [metalfx|lanczos]") }
let inDir = args[1], outDir = args[2]
guard let k = Int(args[3]), k == 2 || k == 4 else { die("k must be 2 or 4") }
var engine = args.count > 4 ? args[4] : "metalfx"
let srgb = CGColorSpace(name: CGColorSpace.sRGB)!

func loadRGBA(_ path: String) -> (Int, Int, [UInt8]) {
  let url = URL(fileURLWithPath: path) as CFURL
  guard let src = CGImageSourceCreateWithURL(url, nil), let img = CGImageSourceCreateImageAtIndex(src, 0, nil) else {
    die("cannot read \(path)")
  }
  let w = img.width, h = img.height
  var b = [UInt8](repeating: 0, count: w * h * 4)
  let ctx = CGContext(data: &b, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4, space: srgb,
                      bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)!
  ctx.setBlendMode(.copy)
  ctx.draw(img, in: CGRect(x: 0, y: 0, width: w, height: h))
  for i in 0..<(w * h) { b[4 * i + 3] = 255 }
  return (w, h, b)
}

func savePNG(_ path: String, _ w: Int, _ h: Int, _ b: [UInt8]) {
  var data = b
  let ctx = CGContext(data: &data, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4, space: srgb,
                      bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)!
  let img = ctx.makeImage()!
  let dst = CGImageDestinationCreateWithURL(URL(fileURLWithPath: path) as CFURL, "public.png" as CFString, 1, nil)!
  CGImageDestinationAddImage(dst, img, nil)
  if !CGImageDestinationFinalize(dst) { die("cannot write \(path)") }
}

// ---- MetalFX: 2x steps, rgba16Float in and out (perceptual mode: the data is sRGB-encoded)
let device = MTLCreateSystemDefaultDevice()
if engine == "metalfx", device == nil || !MTLFXSpatialScalerDescriptor.supportsDevice(device!) {
  FileHandle.standardError.write("upscale: MetalFX not supported here, using lanczos\n".data(using: .utf8)!)
  engine = "lanczos"
}
let queue = device?.makeCommandQueue()

func toHalf(_ b: [UInt8]) -> [Float16] { b.map { Float16(Float($0) / 255) } }

func metalfx2x(_ w: Int, _ h: Int, _ px: [Float16]) -> [Float16]? {
  let dev = device!
  let d = MTLFXSpatialScalerDescriptor()
  d.inputWidth = w; d.inputHeight = h; d.outputWidth = 2 * w; d.outputHeight = 2 * h
  d.colorTextureFormat = .rgba16Float; d.outputTextureFormat = .rgba16Float
  d.colorProcessingMode = .perceptual
  guard let sc = d.makeSpatialScaler(device: dev) else { return nil }
  let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba16Float, width: w, height: h, mipmapped: false)
  td.usage = sc.colorTextureUsage; td.storageMode = .shared
  let od = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba16Float, width: 2 * w, height: 2 * h,
                                                    mipmapped: false)
  od.usage = sc.outputTextureUsage; od.storageMode = .private
  guard let tin = dev.makeTexture(descriptor: td), let tout = dev.makeTexture(descriptor: od) else { return nil }
  px.withUnsafeBytes { tin.replace(region: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0, withBytes: $0.baseAddress!,
                                   bytesPerRow: w * 8) }
  let rd = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba16Float, width: 2 * w, height: 2 * h,
                                                    mipmapped: false)
  rd.storageMode = .shared
  guard let back = dev.makeTexture(descriptor: rd), let cb = queue!.makeCommandBuffer() else { return nil }
  sc.colorTexture = tin; sc.outputTexture = tout
  sc.inputContentWidth = w; sc.inputContentHeight = h
  sc.encode(commandBuffer: cb)
  let bl = cb.makeBlitCommandEncoder()!
  bl.copy(from: tout, to: back)
  bl.endEncoding()
  cb.commit(); cb.waitUntilCompleted()
  if cb.status != .completed { return nil }
  var out = [Float16](repeating: 0, count: 4 * w * h * 4)
  out.withUnsafeMutableBytes { back.getBytes($0.baseAddress!, bytesPerRow: 2 * w * 8,
                                             from: MTLRegionMake2D(0, 0, 2 * w, 2 * h), mipmapLevel: 0) }
  return out
}

// ---- Core Image Lanczos (+ unsharp mask)
let ci = CIContext(options: [.workingColorSpace: srgb, .outputColorSpace: srgb])
func lanczos(_ w: Int, _ h: Int, _ b: [UInt8], _ k: Int) -> [UInt8] {
  let ctx = CGContext(data: UnsafeMutableRawPointer(mutating: b), width: w, height: h, bitsPerComponent: 8,
                      bytesPerRow: w * 4, space: srgb, bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)!
  let img = CIImage(cgImage: ctx.makeImage()!).clampedToExtent()
  let f = CIFilter(name: "CILanczosScaleTransform")!
  f.setValue(img, forKey: kCIInputImageKey); f.setValue(Double(k), forKey: kCIInputScaleKey)
  f.setValue(1.0, forKey: kCIInputAspectRatioKey)
  let u = CIFilter(name: "CIUnsharpMask")!
  u.setValue(f.outputImage!, forKey: kCIInputImageKey)
  u.setValue(Double(k) * 0.6, forKey: kCIInputRadiusKey); u.setValue(0.5, forKey: kCIInputIntensityKey)
  let W = k * w, H = k * h
  var out = [UInt8](repeating: 0, count: W * H * 4)
  ci.render(u.outputImage!.cropped(to: CGRect(x: 0, y: 0, width: W, height: H)), toBitmap: &out, rowBytes: W * 4,
            bounds: CGRect(x: 0, y: 0, width: W, height: H), format: .RGBA8, colorSpace: srgb)
  return out
}

try? FileManager.default.createDirectory(atPath: outDir, withIntermediateDirectories: true)
let files = ((try? FileManager.default.contentsOfDirectory(atPath: inDir)) ?? []).filter { $0.hasSuffix(".png") }.sorted()
var n = 0, fallback = 0
for f in files {
  let (w, h, b) = loadRGBA(inDir + "/" + f)
  var out: [UInt8]? = nil
  if engine == "metalfx" {
    var px = toHalf(b), cw = w, ch = h, ok = true
    for _ in 0..<(k == 4 ? 2 : 1) {
      guard let r = metalfx2x(cw, ch, px) else { ok = false; break }
      px = r; cw *= 2; ch *= 2
    }
    if ok { out = px.map { UInt8(max(0, min(255, (Float($0) * 255).rounded()))) } }
  }
  if out == nil {
    if engine == "metalfx" { fallback += 1 }
    out = lanczos(w, h, b, k)
  }
  savePNG(outDir + "/" + f, k * w, k * h, out!)
  n += 1
}
print("upscale: \(n) images x\(k) (\(engine)\(fallback > 0 ? ", \(fallback) by lanczos" : ""))")
