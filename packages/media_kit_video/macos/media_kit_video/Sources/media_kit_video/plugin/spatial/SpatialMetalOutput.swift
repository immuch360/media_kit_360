// Immuch360 (IMMUCH360-NOTE.md, patch 18): the Spatial 2.5D passes of one player on macOS.
//
// mpv draws each frame at the frame size chosen here (the video's shape, capped as the setup asks) into the pixel
// buffers of TextureHW (OpenGL) or TextureSW (software); this object reads the newest one as a Metal texture
// (CVMetalTextureCache: the buffers are IOSurfaces, so no copy) and draws the view at the viewpoint into one of its
// own three pixel buffers, which Flutter then shows. A redraw (a new viewpoint, the view of a 360 video, the
// disparity map shown) draws the frame kept again, without mpv. The viewpoint, the view and the debug switch are set
// from the platform thread and read at the next draw on the worker thread of the video output; everything else runs
// on that worker. No FlutterMacOS here, so that the test of the passes builds this file with swiftc alone.

import CoreGraphics
import CoreVideo
import Foundation
import Metal
import QuartzCore

@available(macOS 10.15, *)
final class SpatialMetalOutput {
  /// SpatialSetup of lib/src/spatial_output.dart
  struct Setup: Equatable {
    var layout: SpatialMetalLayout = .sideBySide
    var equirect = false
    var halfSphere = false
    var quality: SpatialMetalQuality = .medium
    var adaptive = true
    var maxFrameWidth = 3840
    var maxFramePixels = 3840 * 2160
    var outputWidth = 1
    var outputHeight = 1
    var maxOutputHeight = 1440

    /// The arguments of VideoOutputManager.SetSpatial
    static func from(_ arguments: [String: Any]) -> Setup {
      func int(_ key: String, _ fallback: Int) -> Int {
        if let number = arguments[key] as? NSNumber { return number.intValue }
        if let text = arguments[key] as? String, let value = Int(text) { return value }
        return fallback
      }
      func bool(_ key: String, _ fallback: Bool) -> Bool {
        (arguments[key] as? NSNumber)?.boolValue ?? fallback
      }
      var setup = Setup()
      setup.layout = SpatialMetalLayout(rawValue: min(max(int("layout", 0), 0), 4)) ?? .sideBySide
      setup.equirect = int("projection", 0) == 1
      setup.halfSphere = bool("halfSphere", false)
      setup.quality = SpatialMetalQuality(rawValue: min(max(int("quality", 1), 0), 2)) ?? .medium
      setup.adaptive = bool("adaptive", true)
      setup.maxFrameWidth = max(16, int("maxFrameWidth", 3840))
      setup.maxFramePixels = max(256, int("maxFramePixels", 3840 * 2160))
      setup.outputWidth = max(1, int("outputWidth", 1))
      setup.outputHeight = max(1, int("outputHeight", 1))
      setup.maxOutputHeight = max(2, int("maxOutputHeight", 1440))
      return setup
    }
  }

  private(set) var setup: Setup
  let renderer: SpatialMetalRenderer
  private let device: MTLDevice
  private let textureCache: CVMetalTextureCache?

  // The three output buffers, drawn in turn; [current] is the one Flutter shows
  private var outputs: [CVPixelBuffer] = []
  private var outputSize = CGSize.zero
  private var nextOutput = 0
  private(set) var current: CVPixelBuffer?
  // A frame that is no IOSurface is copied into this texture
  private var uploadTexture: MTLTexture?
  // Whether the disparity settled on the frame shown since mpv last drew one
  private var settled = false

  // Set from the platform thread
  private let lock = NSLock()
  private var requestedViewpoint: Float = 0.5
  private var view = (yaw: Float(0), pitch: Float(0), fov: Float(90))
  private var debug = false
  private var probeRequested = false

  // The figures since the previous stats, read on the platform thread
  private var frames = 0
  private var redraws = 0
  private var failed = 0
  private var frameMs: [Double] = []
  private var redrawMs: [Double] = []
  private var counters = SpatialMetalCounters()
  private var lastDrawnViewpoint: Float = 0.5
  private var drawnSize = (width: 0, height: 0)
  private var frameSize = (width: 0, height: 0)
  private var probe: [UInt32]?
  private var probeRow: [UInt32]?

  /// The passes for [setup], or why they cannot run on this Mac
  static func make(setup: Setup, viewpoint: Float?) -> (SpatialMetalOutput?, String) {
    guard let device = MTLCreateSystemDefaultDevice() else {
      return (nil, "Metal: no device")
    }
    let (made, reason) = SpatialMetalRenderer.make(device: device)
    guard let renderer = made else {
      return (nil, reason)
    }
    let output = SpatialMetalOutput(device: device, renderer: renderer, setup: setup)
    if let viewpoint {
      output.setViewpoint(viewpoint)
    }
    return (output, reason)
  }

  private init(device: MTLDevice, renderer: SpatialMetalRenderer, setup: Setup) {
    self.device = device
    self.renderer = renderer
    self.setup = setup
    var cache: CVMetalTextureCache?
    if CVMetalTextureCacheCreate(kCFAllocatorDefault, nil, device, nil, &cache) != kCVReturnSuccess {
      cache = nil
    }
    textureCache = cache
    apply(setup)
    renderer.quality = setup.quality
  }

  /// A new setup of the same passes (the layout, the projection, the tier, the box): nothing of mpv changes
  func apply(_ newSetup: Setup) {
    if newSetup.layout != setup.layout || newSetup.equirect != setup.equirect
      || newSetup.halfSphere != setup.halfSphere
    {
      settled = false
    }
    if newSetup.quality != setup.quality || newSetup.adaptive != setup.adaptive {
      renderer.quality = newSetup.quality
    }
    setup = newSetup
    renderer.layout = newSetup.layout
    renderer.equirect = newSetup.equirect
    renderer.halfSphere = newSetup.halfSphere
    renderer.adaptiveQuality = newSetup.adaptive
  }

  // MARK: - From the platform thread

  func setViewpoint(_ viewpoint: Float) {
    locked { requestedViewpoint = min(max(viewpoint, 0), 1) }
  }

  var viewpoint: Float { locked { requestedViewpoint } }

  func setView(yaw: Float, pitch: Float, fov: Float) {
    locked { view = (yaw, pitch, fov) }
  }

  func setDebug(_ disparityMap: Bool) {
    locked { debug = disparityMap }
  }

  // MARK: - Sizes

  /// The size mpv draws the frame at: the video's shape, at most [Setup.maxFrameWidth] wide and
  /// [Setup.maxFramePixels] pixels, even
  func frameSize(video: CGSize) -> CGSize {
    let width = Double(video.width)
    let height = Double(video.height)
    guard width >= 1, height >= 1 else { return .zero }
    let scale = min(
      1,
      Double(setup.maxFrameWidth) / width,
      (Double(setup.maxFramePixels) / (width * height)).squareRoot()
    )
    return CGSize(width: Self.even(width * scale), height: Self.even(height * scale))
  }

  /// The size of the output for a frame of [frame]: a flat video takes the shape of one eye as it is seen, inside the
  /// box of the setup; an equirectangular one the box. At most [Setup.maxOutputHeight] lines, the shape kept.
  func outputSize(frame: CGSize) -> CGSize {
    var boxWidth = Double(setup.outputWidth)
    var boxHeight = Double(setup.outputHeight)
    if boxHeight > Double(setup.maxOutputHeight) {
      boxWidth *= Double(setup.maxOutputHeight) / boxHeight
      boxHeight = Double(setup.maxOutputHeight)
    }
    if setup.equirect || frame.width < 1 || frame.height < 1 {
      return CGSize(width: Self.even(boxWidth), height: Self.even(boxHeight))
    }
    let eyes = SpatialMetalRenderer.eyeRects(for: setup.layout)
    let aspect = Double(
      SpatialMetalRenderer.displayAspect(
        frameWidth: Float(frame.width),
        frameHeight: Float(frame.height),
        rect: eyes.left,
        layout: setup.layout
      ))
    var width = boxWidth
    var height = boxWidth / aspect
    if height > boxHeight {
      height = boxHeight
      width = boxHeight * aspect
    }
    return CGSize(width: Self.even(width), height: Self.even(height))
  }

  private static func even(_ value: Double) -> Double {
    let rounded = Int(value.rounded())
    return Double(max(2, rounded + rounded % 2))
  }

  // MARK: - Drawing, on the worker

  /// Draws [source], the newest frame of mpv ([newFrame]) or the frame kept again, into the next output buffer of
  /// [size], which becomes [current]. False when nothing could be drawn.
  @discardableResult
  func draw(source: CVPixelBuffer, size: CGSize, newFrame: Bool) -> Bool {
    let start = CACurrentMediaTime()
    let (viewpoint, view, debug, probeWanted) = locked {
      (requestedViewpoint, self.view, self.debug, probeRequested)
    }
    renderer.viewpoint = viewpoint
    renderer.yawDegrees = view.yaw
    renderer.pitchDegrees = view.pitch
    renderer.fovDegrees = view.fov
    renderer.showDisparity = debug
    if newFrame {
      settled = false
    }
    let settle = newFrame || settled ? 0 : 3
    var drawn = false
    var output: CVPixelBuffer?
    if let input = texture(of: source), let target = nextOutputBuffer(size: size),
      let targetTexture = texture(of: target)
    {
      drawn = renderer.draw(source: input.texture, target: targetTexture.texture, newFrame: newFrame, settle: settle)
      withExtendedLifetime(input.keep) {}
      withExtendedLifetime(targetTexture.keep) {}
      output = target
    }
    if !newFrame && drawn {
      settled = true
    }
    var probePixels: [UInt32]?
    var row: [UInt32]?
    if drawn, probeWanted, let output {
      let read = Self.readProbe(output)
      probePixels = read.0
      row = read.1
    }
    if drawn {
      current = output
    }
    let milliseconds = (CACurrentMediaTime() - start) * 1000
    let taken = renderer.takeCounters()
    locked {
      if drawn {
        if newFrame {
          frames += 1
          Self.keep(&frameMs, milliseconds)
        } else {
          redraws += 1
          Self.keep(&redrawMs, milliseconds)
        }
        lastDrawnViewpoint = viewpoint
        drawnSize = (Int(size.width), Int(size.height))
      } else {
        failed += 1
      }
      frameSize = (CVPixelBufferGetWidth(source), CVPixelBufferGetHeight(source))
      counters.draws += taken.draws
      counters.disparityRuns += taken.disparityRuns
      counters.settleRuns += taken.settleRuns
      counters.qualityChanges += taken.qualityChanges
      if let probePixels {
        probe = probePixels
        probeRow = row
        probeRequested = false
      }
    }
    return drawn
  }

  /// A Metal view of [buffer], with what must stay alive while the GPU reads it
  private func texture(of buffer: CVPixelBuffer) -> (texture: MTLTexture, keep: AnyObject?)? {
    let width = CVPixelBufferGetWidth(buffer)
    let height = CVPixelBufferGetHeight(buffer)
    guard width > 0, height > 0 else { return nil }
    if let textureCache {
      var made: CVMetalTexture?
      let status = CVMetalTextureCacheCreateTextureFromImage(
        kCFAllocatorDefault,
        textureCache,
        buffer,
        nil,
        SpatialMetalRenderer.pixelFormat,
        width,
        height,
        0,
        &made
      )
      if status == kCVReturnSuccess, let made, let texture = CVMetalTextureGetTexture(made) {
        return (texture, made)
      }
    }
    // Not an IOSurface: a copy of its pixels (an output buffer is always one)
    if uploadTexture?.width != width || uploadTexture?.height != height {
      let descriptor = MTLTextureDescriptor.texture2DDescriptor(
        pixelFormat: SpatialMetalRenderer.pixelFormat,
        width: width,
        height: height,
        mipmapped: false
      )
      descriptor.usage = [.shaderRead]
      descriptor.storageMode = .managed
      uploadTexture = device.makeTexture(descriptor: descriptor)
    }
    guard let uploadTexture else { return nil }
    CVPixelBufferLockBaseAddress(buffer, .readOnly)
    defer { CVPixelBufferUnlockBaseAddress(buffer, .readOnly) }
    guard let base = CVPixelBufferGetBaseAddress(buffer) else { return nil }
    uploadTexture.replace(
      region: MTLRegionMake2D(0, 0, width, height),
      mipmapLevel: 0,
      withBytes: base,
      bytesPerRow: CVPixelBufferGetBytesPerRow(buffer)
    )
    return (uploadTexture, nil)
  }

  /// The next of the three output buffers, made again at a new size
  private func nextOutputBuffer(size: CGSize) -> CVPixelBuffer? {
    if size != outputSize || outputs.count != 3 {
      outputs = []
      current = nil
      outputSize = size
      for _ in 0..<3 {
        guard let buffer = Self.makePixelBuffer(width: Int(size.width), height: Int(size.height)) else {
          outputs = []
          return nil
        }
        outputs.append(buffer)
      }
      if let textureCache {
        CVMetalTextureCacheFlush(textureCache, 0)
      }
    }
    nextOutput = (nextOutput + 1) % outputs.count
    return outputs[nextOutput]
  }

  static func makePixelBuffer(width: Int, height: Int) -> CVPixelBuffer? {
    guard width > 0, height > 0 else { return nil }
    let attributes: [CFString: Any] = [
      kCVPixelBufferMetalCompatibilityKey: true,
      kCVPixelBufferIOSurfacePropertiesKey: [String: Any](),
    ]
    var buffer: CVPixelBuffer?
    let status = CVPixelBufferCreate(
      kCFAllocatorDefault,
      width,
      height,
      kCVPixelFormatType_32BGRA,
      attributes as CFDictionary,
      &buffer
    )
    return status == kCVReturnSuccess ? buffer : nil
  }

  /// Five pixels at renderer C's places (its rows count from the bottom) and the middle row, as 0xAABBGGRR
  static func readProbe(_ buffer: CVPixelBuffer) -> ([UInt32], [UInt32]) {
    CVPixelBufferLockBaseAddress(buffer, .readOnly)
    defer { CVPixelBufferUnlockBaseAddress(buffer, .readOnly) }
    let width = CVPixelBufferGetWidth(buffer)
    let height = CVPixelBufferGetHeight(buffer)
    let stride = CVPixelBufferGetBytesPerRow(buffer)
    guard width > 0, height > 0, let base = CVPixelBufferGetBaseAddress(buffer) else { return ([], []) }
    let bytes = base.assumingMemoryBound(to: UInt8.self)
    func pixel(_ x: Int, _ glY: Int) -> UInt32 {
      let row = min(max(height - 1 - glY, 0), height - 1)
      let column = min(max(x, 0), width - 1)
      let at = bytes + row * stride + column * 4
      // BGRA in memory
      return UInt32(at[2]) | UInt32(at[1]) << 8 | UInt32(at[0]) << 16 | UInt32(at[3]) << 24
    }
    let points = [
      (width / 2, height / 2),
      (width / 2, height / 8),
      (width / 2, height - height / 8 - 1),
      (width / 8, height / 2),
      (width - width / 8 - 1, height / 2),
    ]
    let five = points.map { pixel($0.0, $0.1) }
    let middle = (0..<width).map { pixel($0, height / 2) }
    return (five, middle)
  }

  // MARK: - The figures

  /// The maps of ProjectionStats and SpatialStats of lib/src, the counters taken; [probe] asks for the pixels of the
  /// next draw
  func stats(probe wanted: Bool) -> [String: Any] {
    let (stereo, quality, renderFps, disparityFps, gpuMs, mapSize, error, deviceName) = (
      renderer.stereo, renderer.quality.rawValue, renderer.renderFps, renderer.disparityFps, renderer.gpuMs,
      renderer.mapSize, renderer.error, renderer.deviceName
    )
    return locked {
      var map: [String: Any] = [
        "enabled": true,
        "frames": frames,
        "redraws": redraws,
        "failed": failed,
        "frameMs": frameMs,
        "redrawMs": redrawMs,
        "lockedMs": [Double](),
        "frameWidth": frameSize.width,
        "frameHeight": frameSize.height,
        "outputWidth": Int(outputSize.width),
        "outputHeight": Int(outputSize.height),
        "error": error,
        "glRenderer": "Metal (\(deviceName))",
        "spatial": true,
        "stereo": stereo,
        "quality": quality,
        "renderFps": renderFps,
        "disparityFps": disparityFps,
        "renderMs": gpuMs,
        "disparityWidth": mapSize.width,
        "disparityHeight": mapSize.height,
        "eyeWidth": eyeSize().width,
        "eyeHeight": eyeSize().height,
        "viewpoint": Double(lastDrawnViewpoint),
        "requestedViewpoint": Double(requestedViewpoint),
        "gpuTimer": true,
        "halfFloatMaps": true,
        "spatialDraws": counters.draws,
        "disparityRuns": counters.disparityRuns,
        "settleRuns": counters.settleRuns,
        "qualityChanges": counters.qualityChanges,
        "drawnWidth": drawnSize.width,
        "drawnHeight": drawnSize.height,
        "showDisparity": debug,
      ]
      if let probe {
        map["probe"] = probe.map { Int($0) }
        self.probe = nil
      }
      if let probeRow {
        map["probeRow"] = probeRow.map { Int($0) }
        self.probeRow = nil
      }
      frames = 0
      redraws = 0
      failed = 0
      frameMs = []
      redrawMs = []
      counters = SpatialMetalCounters()
      if wanted {
        probeRequested = true
      }
      return map
    }
  }

  /// The eye in the frame mpv draws, in pixels (the view of each eye for an equirectangular video)
  private func eyeSize() -> (width: Int, height: Int) {
    if setup.equirect {
      return (Int(outputSize.width), Int(outputSize.height))
    }
    let rect = SpatialMetalRenderer.eyeRects(for: setup.layout).left
    return (Int(Float(frameSize.width) * rect.z), Int(Float(frameSize.height) * rect.w))
  }

  private static func keep(_ list: inout [Double], _ value: Double) {
    list.append(value)
    if list.count > 120 {
      list.removeFirst(list.count - 120)
    }
  }

  private func locked<T>(_ block: () -> T) -> T {
    lock.lock()
    defer { lock.unlock() }
    return block()
  }
}
