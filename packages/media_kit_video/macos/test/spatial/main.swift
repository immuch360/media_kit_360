// Immuch360 (IMMUCH360-NOTE.md, patch 18): the Spatial passes of macOS (plugin/spatial) on the Mac's Metal device,
// without Flutter or mpv. Built by swiftc with the three files of plugin/spatial:
//
//   cd packages/media_kit_video/macos
//   xcrun swiftc -O -target "$(uname -m)-apple-macos12.0" -o /tmp/spatial_metal_test \
//     media_kit_video/Sources/media_kit_video/plugin/spatial/*.swift test/spatial/main.swift && /tmp/spatial_metal_test
//
// What it checks, on a random dot stereo pair whose square stands 8 pixels nearer than its background (the right eye
// shows it 8 pixels further left), side by side in an IOSurface pixel buffer as mpv's TextureHW and TextureSW make
// them:
// - the shaders compile and every pass is made;
// - the view at p = 0 is the left eye, at p = 1 the right eye, and half way the square moved by 4 pixels while the
//   background stayed; the same from a frame that is no IOSurface (the copy path);
// - the top bottom layout and the swapped one give the same view;
// - the output takes the shape of one eye inside the box, and the five pixels and the middle row of the probe;
// - an equirectangular VR180 pair: ahead the picture, behind black;
// - the time of a draw at the HIGH tier on this device.
// Exits with 1 on the first failure.

import CoreVideo
import Foundation
import Metal

var failures = 0

func check(_ condition: Bool, _ message: String) {
  if condition {
    print("ok: \(message)")
  } else {
    print("FAILED: \(message)")
    failures += 1
  }
}

let eyeWidth = 256
let eyeHeight = 128
let disparity = 8
let squareLeft = 96
let squareRight = 160
let squareTop = 32
let squareBottom = 96

/// Grey dots of 2 x 2 pixels, the same pattern for both eyes, the square shifted in the right eye
func eyes() -> (left: [UInt8], right: [UInt8]) {
  // A fixed sequence (a linear congruential generator), so that every run draws the same pair
  var state: UInt32 = 12345
  func next() -> UInt8 {
    state = state &* 1_103_515_245 &+ 12345
    return UInt8((state >> 16) & 0xff)
  }
  let dotsWidth = eyeWidth / 2 + disparity
  var background = [UInt8](repeating: 0, count: dotsWidth * (eyeHeight / 2))
  var square = [UInt8](repeating: 0, count: dotsWidth * (eyeHeight / 2))
  for i in 0..<background.count {
    background[i] = next() > 127 ? 230 : 25
    square[i] = next() > 127 ? 210 : 45
  }
  func dot(_ table: [UInt8], _ x: Int, _ y: Int) -> UInt8 {
    table[(y / 2) * dotsWidth + min(max(x, 0), 2 * dotsWidth - 1) / 2]
  }
  var left = [UInt8](repeating: 0, count: eyeWidth * eyeHeight)
  var right = [UInt8](repeating: 0, count: eyeWidth * eyeHeight)
  for y in 0..<eyeHeight {
    for x in 0..<eyeWidth {
      let inSquareRows = y >= squareTop && y < squareBottom
      // Left eye: the square at [96, 160)
      if inSquareRows && x >= squareLeft && x < squareRight {
        left[y * eyeWidth + x] = dot(square, x - squareLeft, y)
      } else {
        left[y * eyeWidth + x] = dot(background, x, y)
      }
      // Right eye: the square 8 pixels to the left, the background as it is
      let shifted = x + disparity
      if inSquareRows && shifted >= squareLeft && shifted < squareRight {
        right[y * eyeWidth + x] = dot(square, shifted - squareLeft, y)
      } else {
        right[y * eyeWidth + x] = dot(background, x, y)
      }
    }
  }
  return (left, right)
}

enum Arrangement { case sideBySide, topBottom, sideBySideSwapped }

func frame(_ pair: (left: [UInt8], right: [UInt8]), _ arrangement: Arrangement, ioSurface: Bool) -> CVPixelBuffer {
  let width = arrangement == .topBottom ? eyeWidth : 2 * eyeWidth
  let height = arrangement == .topBottom ? 2 * eyeHeight : eyeHeight
  // Without the IOSurface and Metal keys the buffer is plain memory, which the passes copy
  var attributes: [CFString: Any] = [:]
  if ioSurface {
    attributes[kCVPixelBufferMetalCompatibilityKey] = true
    attributes[kCVPixelBufferIOSurfacePropertiesKey] = [String: Any]()
  }
  var made: CVPixelBuffer?
  let status = CVPixelBufferCreate(
    kCFAllocatorDefault, width, height, kCVPixelFormatType_32BGRA, attributes as CFDictionary, &made)
  precondition(status == kCVReturnSuccess, "CVPixelBufferCreate \(status)")
  let buffer = made!
  CVPixelBufferLockBaseAddress(buffer, [])
  let stride = CVPixelBufferGetBytesPerRow(buffer)
  let bytes = CVPixelBufferGetBaseAddress(buffer)!.assumingMemoryBound(to: UInt8.self)
  for y in 0..<eyeHeight {
    for x in 0..<eyeWidth {
      let l = pair.left[y * eyeWidth + x]
      let r = pair.right[y * eyeWidth + x]
      let (first, second) = arrangement == .sideBySideSwapped ? (r, l) : (l, r)
      let a: (Int, Int)
      let b: (Int, Int)
      switch arrangement {
      case .topBottom:
        a = (x, y)
        b = (x, y + eyeHeight)
      default:
        a = (x, y)
        b = (x + eyeWidth, y)
      }
      for (point, value) in [(a, first), (b, second)] {
        let at = bytes + point.1 * stride + point.0 * 4
        at[0] = value
        at[1] = value
        at[2] = value
        at[3] = 255
      }
    }
  }
  CVPixelBufferUnlockBaseAddress(buffer, [])
  return buffer
}

/// The green channel of a BGRA pixel buffer, row by row
func gray(_ buffer: CVPixelBuffer) -> (width: Int, height: Int, pixels: [UInt8]) {
  CVPixelBufferLockBaseAddress(buffer, .readOnly)
  defer { CVPixelBufferUnlockBaseAddress(buffer, .readOnly) }
  let width = CVPixelBufferGetWidth(buffer)
  let height = CVPixelBufferGetHeight(buffer)
  let stride = CVPixelBufferGetBytesPerRow(buffer)
  let bytes = CVPixelBufferGetBaseAddress(buffer)!.assumingMemoryBound(to: UInt8.self)
  var pixels = [UInt8](repeating: 0, count: width * height)
  for y in 0..<height {
    for x in 0..<width {
      pixels[y * width + x] = bytes[y * stride + x * 4 + 1]
    }
  }
  return (width, height, pixels)
}

/// The mean absolute difference of [view] and [eye] (both eyeWidth x eyeHeight) over the inner part, away from the
/// borders the passes call unreliable
func difference(_ view: [UInt8], _ eye: [UInt8], rows: Range<Int> = 8..<(eyeHeight - 8)) -> Double {
  var sum = 0
  var count = 0
  for y in rows {
    for x in 24..<(eyeWidth - 24) {
      sum += abs(Int(view[y * eyeWidth + x]) - Int(eye[y * eyeWidth + x]))
      count += 1
    }
  }
  return Double(sum) / Double(max(count, 1))
}

/// The shift s, from -12 to 12, that best matches view(x) = eye(x + s) over the columns and rows given
func bestShift(_ view: [UInt8], _ eye: [UInt8], columns: Range<Int>, rows: Range<Int>) -> Int {
  var best = 0
  var bestCost = Int.max
  for shift in -12...12 {
    var cost = 0
    for y in rows {
      for x in columns {
        let source = min(max(x + shift, 0), eyeWidth - 1)
        cost += abs(Int(view[y * eyeWidth + x]) - Int(eye[y * eyeWidth + source]))
      }
    }
    if cost < bestCost {
      bestCost = cost
      best = shift
    }
  }
  return best
}

guard #available(macOS 10.15, *) else {
  print("FAILED: macOS 10.15 or later is needed")
  exit(1)
}

guard let device = MTLCreateSystemDefaultDevice() else {
  print("FAILED: no Metal device on this Mac")
  exit(1)
}
print("Metal device: \(device.name)")

let pair = eyes()

func makeOutput(_ layout: SpatialMetalLayout, quality: SpatialMetalQuality = .high) -> SpatialMetalOutput {
  var setup = SpatialMetalOutput.Setup()
  setup.layout = layout
  setup.quality = quality
  setup.adaptive = false
  setup.outputWidth = eyeWidth
  setup.outputHeight = eyeHeight
  let (made, reason) = SpatialMetalOutput.make(setup: setup, viewpoint: 0.5)
  guard let output = made else {
    print("FAILED: the passes cannot be made: \(reason)")
    exit(1)
  }
  return output
}

/// The view at [viewpoint] of [source] after a few frames of the same pair, then a settled redraw
func view(_ output: SpatialMetalOutput, _ source: CVPixelBuffer, _ viewpoint: Float) -> [UInt8] {
  let size = output.outputSize(frame: CGSize(width: CVPixelBufferGetWidth(source), height: CVPixelBufferGetHeight(source)))
  output.setViewpoint(viewpoint)
  for _ in 0..<4 {
    _ = output.draw(source: source, size: size, newFrame: true)
  }
  let drawn = output.draw(source: source, size: size, newFrame: false)
  check(drawn, "a draw at p = \(viewpoint)")
  let pixels = gray(output.current!)
  precondition(pixels.width == eyeWidth && pixels.height == eyeHeight, "output \(pixels.width) x \(pixels.height)")
  return pixels.pixels
}

// 1. The passes, and the views of a side by side pair
let sideBySide = makeOutput(.sideBySide)
check(sideBySide.renderer.stereoAvailable, "every pass is made (\(sideBySide.renderer.error))")
let source = frame(pair, .sideBySide, ioSurface: true)
let outputSize = sideBySide.outputSize(frame: CGSize(width: 2 * eyeWidth, height: eyeHeight))
check(
  outputSize == CGSize(width: eyeWidth, height: eyeHeight),
  "the output takes the shape of one eye in the box: \(outputSize)")

let atLeft = view(sideBySide, source, 0)
let atRight = view(sideBySide, source, 1)
let halfWay = view(sideBySide, source, 0.5)
let leftDifference = difference(atLeft, pair.left)
let rightDifference = difference(atRight, pair.right)
check(leftDifference < 3, "p = 0 is the left eye (mean difference \(leftDifference))")
check(rightDifference < 3, "p = 1 is the right eye (mean difference \(rightDifference))")
let squareRows = (squareTop + 6)..<(squareBottom - 6)
let squareShift = bestShift(halfWay, pair.left, columns: (squareLeft + 4)..<(squareRight - 12), rows: squareRows)
check(abs(squareShift - disparity / 2) <= 1, "half way the square moved by half its disparity: \(squareShift) px")
let backgroundShift = bestShift(halfWay, pair.left, columns: 24..<80, rows: squareRows)
check(abs(backgroundShift) <= 1, "and the background stayed: \(backgroundShift) px")

// 2. The copy path: a frame that is no IOSurface gives the same view
let copied = makeOutput(.sideBySide)
let copiedView = view(copied, frame(pair, .sideBySide, ioSurface: false), 0.5)
let copyDifference = difference(copiedView, halfWay)
check(copyDifference < 1, "a frame that is no IOSurface gives the same view (difference \(copyDifference))")

// 3. Top bottom and swapped layouts
let topBottomView = view(makeOutput(.topBottom), frame(pair, .topBottom, ioSurface: true), 0.5)
let topBottomDifference = difference(topBottomView, halfWay)
check(topBottomDifference < 4, "top bottom gives the side by side view (difference \(topBottomDifference))")
let swappedView = view(makeOutput(.sideBySideSwapped), frame(pair, .sideBySideSwapped, ioSurface: true), 0.5)
let swappedDifference = difference(swappedView, halfWay)
check(swappedDifference < 1, "swapped eyes give the same view (difference \(swappedDifference))")

// 4. The probe and the figures
_ = sideBySide.stats(probe: true)
let probeSize = sideBySide.outputSize(frame: CGSize(width: 2 * eyeWidth, height: eyeHeight))
_ = sideBySide.draw(source: source, size: probeSize, newFrame: false)
let stats = sideBySide.stats(probe: false)
let probe = stats["probe"] as? [Int] ?? []
let probeRow = stats["probeRow"] as? [Int] ?? []
check(probe.count == 5, "five pixels in the probe")
check(probeRow.count == eyeWidth, "the middle row in the probe (\(probeRow.count) pixels)")
check(probe.allSatisfy { ($0 >> 24) & 0xff == 255 }, "opaque pixels in the probe")
check((stats["stereo"] as? Bool) == true, "the stats say stereo")
check((stats["disparityWidth"] as? Int) == SpatialMetalQuality.high.mapWidth, "the HIGH map")
check(((stats["redraws"] as? Int) ?? 0) >= 1, "redraws counted")

// 5. An equirectangular VR180 pair of a flat grey: ahead the picture, behind black
var equirectSetup = SpatialMetalOutput.Setup()
equirectSetup.layout = .sideBySide
equirectSetup.equirect = true
equirectSetup.halfSphere = true
equirectSetup.outputWidth = 160
equirectSetup.outputHeight = 90
let (equirectMade, equirectReason) = SpatialMetalOutput.make(setup: equirectSetup, viewpoint: 0.5)
if let equirect = equirectMade {
  let greyPair = (
    left: [UInt8](repeating: 200, count: eyeWidth * eyeHeight),
    right: [UInt8](repeating: 200, count: eyeWidth * eyeHeight)
  )
  let greySource = frame(greyPair, .sideBySide, ioSurface: true)
  let size = equirect.outputSize(frame: CGSize(width: 2 * eyeWidth, height: eyeHeight))
  check(size == CGSize(width: 160, height: 90), "an equirectangular output fills the box: \(size)")
  func centre(yaw: Float) -> UInt8 {
    equirect.setView(yaw: yaw, pitch: 0, fov: 60)
    _ = equirect.draw(source: greySource, size: size, newFrame: true)
    let pixels = gray(equirect.current!)
    return pixels.pixels[(pixels.height / 2) * pixels.width + pixels.width / 2]
  }
  let ahead = centre(yaw: 0)
  let behind = centre(yaw: 180)
  check(abs(Int(ahead) - 200) <= 3, "ahead of a VR180 eye: the picture (\(ahead))")
  check(behind <= 3, "behind a VR180 eye: black (\(behind))")
} else {
  check(false, "the equirectangular passes: \(equirectReason)")
}

// 6. The time of a draw of a 1920 x 1080 pair at the HIGH tier
var bigSetup = SpatialMetalOutput.Setup()
bigSetup.quality = .high
bigSetup.adaptive = false
bigSetup.outputWidth = 1920
bigSetup.outputHeight = 1080
if let big = SpatialMetalOutput.make(setup: bigSetup, viewpoint: 0.3).0,
  let bigSource = SpatialMetalOutput.makePixelBuffer(width: 3840, height: 1080)
{
  let size = big.outputSize(frame: CGSize(width: 3840, height: 1080))
  for _ in 0..<5 {
    _ = big.draw(source: bigSource, size: size, newFrame: true)
  }
  var times: [Double] = []
  for _ in 0..<20 {
    let start = CFAbsoluteTimeGetCurrent()
    _ = big.draw(source: bigSource, size: size, newFrame: true)
    times.append((CFAbsoluteTimeGetCurrent() - start) * 1000)
  }
  times.sort()
  print(
    String(
      format: "a 3840 x 1080 pair to a %d x %d view at HIGH: median %.2f ms, slowest %.2f ms on %@", Int(size.width),
      Int(size.height), times[times.count / 2], times.last!, device.name))
  check(times[times.count / 2] < 1000, "a large draw completes")
} else {
  check(false, "the passes for a 1920 x 1080 view")
}

if failures > 0 {
  print("\(failures) check(s) failed")
  exit(1)
}
print("every check passed")
