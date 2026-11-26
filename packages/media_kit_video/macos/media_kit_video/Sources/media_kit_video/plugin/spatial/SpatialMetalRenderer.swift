// Immuch360 (IMMUCH360-NOTE.md, patch 18): the Spatial 2.5D passes on macOS, in Metal.
//
// The renderer of the app's iOS Spatial player (mobile/ios/Runner/Spatial/SpatialRenderer.swift of
// github.com/freeKC/Immuch360), with the same shaders (SpatialMetalShaders.swift), drawing into a texture instead of
// an MTKView: the frame mpv drew is read from its pixel buffer, the passes draw the view at the viewpoint into the
// pixel buffer Flutter shows. Every frame: the eyes are cut from the frame (for an equirectangular video, the view of
// each eye is drawn first, at the output's shape), the disparity map is refreshed every [SpatialMetalQuality.interval]
// frames, then the synthesis draws the view, letterboxed. When a pass cannot be made, the left eye is drawn as it is.
// No FlutterMacOS here: the test of the passes (macos/test/spatial) builds this file with swiftc alone.

import CoreGraphics
import Foundation
import Metal
import QuartzCore
import simd

/// Where the two eyes are in the frame, in the order of SpatialLayout in lib/src/spatial_output.dart
enum SpatialMetalLayout: Int {
  case sideBySide = 0
  case topBottom = 1
  case sideBySideSwapped = 2
  case topBottomSwapped = 3
  case none = 4
}

/// The tier of the disparity map, as on the phones (SpatialQuality of lib/src/spatial_output.dart)
enum SpatialMetalQuality: Int {
  case low = 0
  case medium = 1
  case high = 2

  /// Width of the disparity map; its height follows the shape of the eye
  var mapWidth: Int {
    switch self {
    case .low: return 320
    case .medium: return 480
    case .high: return 640
    }
  }

  /// The map is computed once every this many new frames
  var interval: Int {
    switch self {
    case .low: return 4
    case .medium: return 2
    case .high: return 1
    }
  }

  /// The left right consistency check, which also gives the map in right eye coordinates
  var consistencyCheck: Bool { self != .low }
}

/// What the passes did since the previous [SpatialMetalRenderer.takeCounters]
struct SpatialMetalCounters {
  var draws = 0
  var disparityRuns = 0
  var settleRuns = 0
  var qualityChanges = 0
}

// Swift copies of the uniform structures of the shaders: 16 byte vectors and 4x4 matrices only, which Swift and
// Metal lay out the same way
private struct SpatialViewportUniforms {
  var rotation: simd_float4x4
  var rect: SIMD4<Float>
  var lens: SIMD4<Float>
  var sphere: SIMD4<Float>
}

private struct SpatialSynthesisUniforms {
  var leftRect: SIMD4<Float>
  var rightRect: SIMD4<Float>
  var view: SIMD4<Float>
  var extra: SIMD4<Float>
}

/// The textures of the disparity pipeline, all at the size of the map
@available(macOS 10.15, *)
private final class SpatialMetalMaps {
  let width: Int
  let height: Int
  let grayLeft: MTLTexture
  let grayRight: MTLTexture
  let rawLeft: MTLTexture
  let rawRight: MTLTexture
  let checkedLeft: MTLTexture
  let checkedRight: MTLTexture
  let blurPassLeft: MTLTexture
  let blurPassRight: MTLTexture
  let blurredLeft: MTLTexture
  let blurredRight: MTLTexture
  // Ping pong pairs of the temporal blend: [current] holds the newest map
  let historyLeft: [MTLTexture]
  let historyRight: [MTLTexture]
  var current = 0
  var hasHistory = false
  var hasRightMap = false

  init?(device: MTLDevice, width: Int, height: Int) {
    self.width = width
    self.height = height
    func make(_ format: MTLPixelFormat) -> MTLTexture? {
      let descriptor = MTLTextureDescriptor.texture2DDescriptor(
        pixelFormat: format,
        width: width,
        height: height,
        mipmapped: false
      )
      descriptor.usage = [.shaderRead, .shaderWrite]
      descriptor.storageMode = .private
      return device.makeTexture(descriptor: descriptor)
    }
    guard let grayLeft = make(.r16Float), let grayRight = make(.r16Float),
      let rawLeft = make(.rg16Float), let rawRight = make(.rg16Float),
      let checkedLeft = make(.rg16Float), let checkedRight = make(.rg16Float),
      let blurPassLeft = make(.rg16Float), let blurPassRight = make(.rg16Float),
      let blurredLeft = make(.rg16Float), let blurredRight = make(.rg16Float),
      let historyLeft0 = make(.rg16Float), let historyLeft1 = make(.rg16Float),
      let historyRight0 = make(.rg16Float), let historyRight1 = make(.rg16Float)
    else {
      return nil
    }
    self.grayLeft = grayLeft
    self.grayRight = grayRight
    self.rawLeft = rawLeft
    self.rawRight = rawRight
    self.checkedLeft = checkedLeft
    self.checkedRight = checkedRight
    self.blurPassLeft = blurPassLeft
    self.blurPassRight = blurPassRight
    self.blurredLeft = blurredLeft
    self.blurredRight = blurredRight
    historyLeft = [historyLeft0, historyLeft1]
    historyRight = [historyRight0, historyRight1]
  }
}

@available(macOS 10.15, *)
final class SpatialMetalRenderer {
  /// The pixel format of the frames read and of the outputs drawn: mpv's and Flutter's BGRA
  static let pixelFormat: MTLPixelFormat = .bgra8Unorm

  var layout: SpatialMetalLayout = .sideBySide {
    didSet {
      if layout != oldValue { resetHistory() }
    }
  }
  /// An equirectangular video, seen through a view ([yawDegrees], [pitchDegrees], [fovDegrees])
  var equirect = false {
    didSet {
      if equirect != oldValue { resetHistory() }
    }
  }
  /// Each eye of an equirectangular video covers the front half of the sphere (VR180), black behind
  var halfSphere = false {
    didSet {
      if halfSphere != oldValue { resetHistory() }
    }
  }
  /// 0 is the left camera, 0.5 the middle, 1 the right camera
  var viewpoint: Float = 0.5
  /// The view of an equirectangular video as renderer C takes it (ProjectionOutput.setView): the yaw in degrees,
  /// positive to the right of the frame's centre, the pitch in degrees, positive up, the vertical field of view
  var yawDegrees: Float = 0
  var pitchDegrees: Float = 0
  var fovDegrees: Float = 90
  var quality: SpatialMetalQuality = .medium
  var adaptiveQuality = true
  /// The disparity map of the left eye instead of the view
  var showDisparity = false

  /// Whether every pass was made: false shows the left eye only
  let stereoAvailable: Bool
  /// What could not be made, empty when every pass was
  let error: String
  /// The device's name, for the stats ("Apple M2", "Apple Paravirtual device")
  var deviceName: String { device.name }

  private static let searchRange = 24
  // The longer side of the views of an equirectangular eye, at most
  private static let maxViewportSize = 1920

  private let device: MTLDevice
  private let commandQueue: MTLCommandQueue
  private let passthroughPipeline: MTLRenderPipelineState
  private let viewportPipeline: MTLRenderPipelineState?
  private let synthesisPipeline: MTLRenderPipelineState?
  private let disparityViewPipeline: MTLRenderPipelineState?
  private let grayPipeline: MTLComputePipelineState?
  private let disparityPipeline: MTLComputePipelineState?
  private let consistencyPipeline: MTLComputePipelineState?
  private let blurPipeline: MTLComputePipelineState?
  private let temporalPipeline: MTLComputePipelineState?

  private var maps: SpatialMetalMaps?
  private var eyeViewports: (left: MTLTexture, right: MTLTexture)?
  private var frameCount = 0

  // The figures of the last second, and the adaptive rule
  private var windowStart = CACurrentMediaTime()
  private var framesInWindow = 0
  private var updatesInWindow = 0
  private var gpuTimeSum: Double = 0
  private var gpuTimeCount = 0
  private(set) var renderFps: Double = 0
  private(set) var disparityFps: Double = 0
  private(set) var gpuMs: Double = 0
  private var fastSeconds = 0
  private var lastStepDown: CFTimeInterval = -1000
  private var warmupWindows = 1
  private var counters = SpatialMetalCounters()

  /// Fails when the device cannot draw even the left eye (no command queue, the shaders do not compile); [reason]
  /// then says why
  static func make(device: MTLDevice) -> (SpatialMetalRenderer?, String) {
    guard let commandQueue = device.makeCommandQueue() else {
      return (nil, "Metal: no command queue")
    }
    let library: MTLLibrary
    do {
      library = try device.makeLibrary(source: SpatialMetalShaders.source, options: nil)
    } catch {
      return (nil, "Metal: the Spatial shaders do not compile: \(error)")
    }
    var problems: [String] = []
    func render(_ fragment: String) -> MTLRenderPipelineState? {
      guard let vertexFunction = library.makeFunction(name: "spatialFullscreenVertex"),
        let fragmentFunction = library.makeFunction(name: fragment)
      else {
        problems.append("no function \(fragment)")
        return nil
      }
      let descriptor = MTLRenderPipelineDescriptor()
      descriptor.vertexFunction = vertexFunction
      descriptor.fragmentFunction = fragmentFunction
      descriptor.colorAttachments[0].pixelFormat = SpatialMetalRenderer.pixelFormat
      do {
        return try device.makeRenderPipelineState(descriptor: descriptor)
      } catch {
        problems.append("\(fragment): \(error)")
        return nil
      }
    }
    func compute(_ name: String) -> MTLComputePipelineState? {
      guard let function = library.makeFunction(name: name) else {
        problems.append("no kernel \(name)")
        return nil
      }
      do {
        let pipeline = try device.makeComputePipelineState(function: function)
        // The kernels run in 8 x 8 threadgroups
        guard pipeline.maxTotalThreadsPerThreadgroup >= 64 else {
          problems.append("\(name) cannot run 64 threads per group")
          return nil
        }
        return pipeline
      } catch {
        problems.append("\(name): \(error)")
        return nil
      }
    }
    guard let passthrough = render("spatialPassthroughFragment") else {
      return (nil, "Metal: " + problems.joined(separator: "; "))
    }
    let renderer = SpatialMetalRenderer(
      device: device,
      commandQueue: commandQueue,
      passthrough: passthrough,
      viewport: render("spatialViewportFragment"),
      synthesis: render("spatialSynthesisFragment"),
      disparityView: render("spatialDisparityFragment"),
      gray: compute("spatialGrayDownsample"),
      disparity: compute("spatialDisparity"),
      consistency: compute("spatialConsistency"),
      blur: compute("spatialGuidedBlur"),
      temporal: compute("spatialTemporalBlend"),
      error: problems.joined(separator: "; ")
    )
    return (renderer, problems.joined(separator: "; "))
  }

  private init(
    device: MTLDevice,
    commandQueue: MTLCommandQueue,
    passthrough: MTLRenderPipelineState,
    viewport: MTLRenderPipelineState?,
    synthesis: MTLRenderPipelineState?,
    disparityView: MTLRenderPipelineState?,
    gray: MTLComputePipelineState?,
    disparity: MTLComputePipelineState?,
    consistency: MTLComputePipelineState?,
    blur: MTLComputePipelineState?,
    temporal: MTLComputePipelineState?,
    error: String
  ) {
    self.device = device
    self.commandQueue = commandQueue
    passthroughPipeline = passthrough
    viewportPipeline = viewport
    synthesisPipeline = synthesis
    disparityViewPipeline = disparityView
    grayPipeline = gray
    disparityPipeline = disparity
    consistencyPipeline = consistency
    blurPipeline = blur
    temporalPipeline = temporal
    stereoAvailable =
      synthesis != nil && disparityView != nil && gray != nil && disparity != nil && consistency != nil
      && blur != nil && temporal != nil
    self.error = error
  }

  /// Forgets the previous disparity maps (a seek, a new layout), so that the old scene does not blend in
  func resetHistory() {
    maps?.hasHistory = false
  }

  /// The disparity maps' size, 0 x 0 before the first draw
  var mapSize: (width: Int, height: Int) { (maps?.width ?? 0, maps?.height ?? 0) }

  /// Whether the frames are matched as two eyes
  var stereo: Bool { stereoAvailable && Self.eyeRects(for: layout).stereo }

  func takeCounters() -> SpatialMetalCounters {
    let taken = counters
    counters = SpatialMetalCounters()
    return taken
  }

  /// Draws [source], the frame mpv drew (BGRA), into [target] (BGRA, the output), and waits for the GPU. [newFrame]
  /// is false for a redraw of the frame kept (a new viewpoint on a paused video): the disparity then runs again
  /// [settle] times on the same eyes, so that the blend over time settles on the still picture. Returns whether
  /// something was drawn.
  @discardableResult
  func draw(source: MTLTexture, target: MTLTexture, newFrame: Bool, settle: Int = 0) -> Bool {
    let now = CACurrentMediaTime()
    updateStatistics(now: now)
    if newFrame {
      frameCount += 1
    }
    framesInWindow += 1
    guard target.width > 0, target.height > 0, let commandBuffer = commandQueue.makeCommandBuffer() else {
      return false
    }

    let eyes = Self.eyeRects(for: layout)
    let stereo = eyes.stereo && stereoAvailable
    let targetAspect = Float(target.width) / Float(target.height)

    var leftTexture = source
    var rightTexture = source
    var leftRect = eyes.left
    var rightRect = eyes.right
    var contentAspect = Self.displayAspect(
      frameWidth: Float(source.width),
      frameHeight: Float(source.height),
      rect: eyes.left,
      layout: layout
    )
    var monoViewport = false
    if equirect, viewportPipeline != nil {
      contentAspect = targetAspect
      if stereo, let viewports = eyeViewportTextures(width: target.width, height: target.height) {
        encodeViewport(commandBuffer, source: source, rect: eyes.left, target: viewports.left)
        encodeViewport(commandBuffer, source: source, rect: eyes.right, target: viewports.right)
        leftTexture = viewports.left
        rightTexture = viewports.right
        leftRect = SIMD4<Float>(0, 0, 1, 1)
        rightRect = SIMD4<Float>(0, 0, 1, 1)
      } else {
        monoViewport = true
      }
    }

    var mapsReady = false
    if stereo && !monoViewport, let maps = disparityMaps(eyeAspect: contentAspect) {
      var runs = 0
      if newFrame {
        if !maps.hasHistory || frameCount % quality.interval == 0 {
          runs = 1
        }
      } else {
        runs = maps.hasHistory ? max(0, settle) : max(1, settle)
        counters.settleRuns += runs
      }
      for _ in 0..<runs {
        encodeDisparity(
          commandBuffer,
          maps: maps,
          left: leftTexture,
          leftRect: leftRect,
          right: rightTexture,
          rightRect: rightRect
        )
      }
      if runs > 0 {
        updatesInWindow += 1
        counters.disparityRuns += runs
      }
      mapsReady = maps.hasHistory
    }

    let pass = MTLRenderPassDescriptor()
    pass.colorAttachments[0].texture = target
    pass.colorAttachments[0].loadAction = .clear
    pass.colorAttachments[0].storeAction = .store
    pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
    guard let encoder = commandBuffer.makeRenderCommandEncoder(descriptor: pass) else {
      commandBuffer.commit()
      commandBuffer.waitUntilCompleted()
      return false
    }
    encoder.setViewport(Self.letterbox(contentAspect: contentAspect, width: target.width, height: target.height))

    if monoViewport, let viewportPipeline {
      var uniforms = viewportUniforms(rect: eyes.left, aspect: targetAspect)
      encoder.setRenderPipelineState(viewportPipeline)
      encoder.setFragmentTexture(source, index: 0)
      encoder.setFragmentBytes(&uniforms, length: MemoryLayout<SpatialViewportUniforms>.stride, index: 0)
    } else if mapsReady, let maps, let synthesisPipeline, let disparityViewPipeline {
      let leftMap = maps.historyLeft[maps.current]
      let rightMap = maps.hasRightMap ? maps.historyRight[maps.current] : leftMap
      let texel = 1 / Float(maps.width)
      var uniforms = SpatialSynthesisUniforms(
        leftRect: leftRect,
        rightRect: rightRect,
        view: SIMD4<Float>(min(max(viewpoint, 0), 1), texel, maps.hasRightMap ? 1 : 0, 1.5 * texel),
        extra: SIMD4<Float>(Float(Self.searchRange) * texel, 0, 0, 0)
      )
      if showDisparity {
        encoder.setRenderPipelineState(disparityViewPipeline)
        encoder.setFragmentTexture(leftMap, index: 0)
      } else {
        encoder.setRenderPipelineState(synthesisPipeline)
        encoder.setFragmentTexture(leftTexture, index: 0)
        encoder.setFragmentTexture(rightTexture, index: 1)
        encoder.setFragmentTexture(leftMap, index: 2)
        encoder.setFragmentTexture(rightMap, index: 3)
      }
      encoder.setFragmentBytes(&uniforms, length: MemoryLayout<SpatialSynthesisUniforms>.stride, index: 0)
    } else {
      var rect = leftRect
      encoder.setRenderPipelineState(passthroughPipeline)
      encoder.setFragmentTexture(leftTexture, index: 0)
      encoder.setFragmentBytes(&rect, length: MemoryLayout<SIMD4<Float>>.stride, index: 0)
    }
    encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
    encoder.endEncoding()
    commandBuffer.commit()
    // Flutter reads the output as soon as the frame is said available: the GPU must be done with it
    commandBuffer.waitUntilCompleted()
    if commandBuffer.status == .error {
      return false
    }
    let milliseconds = (commandBuffer.gpuEndTime - commandBuffer.gpuStartTime) * 1000
    if milliseconds.isFinite, milliseconds > 0 {
      gpuTimeSum += milliseconds
      gpuTimeCount += 1
    }
    counters.draws += 1
    return true
  }

  // MARK: - Eyes

  /// Where the eyes are in the frame, in texture coordinates (origin xy, size zw)
  static func eyeRects(for layout: SpatialMetalLayout) -> (left: SIMD4<Float>, right: SIMD4<Float>, stereo: Bool) {
    let leftHalf = SIMD4<Float>(0, 0, 0.5, 1)
    let rightHalf = SIMD4<Float>(0.5, 0, 0.5, 1)
    let topHalf = SIMD4<Float>(0, 0, 1, 0.5)
    let bottomHalf = SIMD4<Float>(0, 0.5, 1, 0.5)
    switch layout {
    case .sideBySide:
      return (leftHalf, rightHalf, true)
    case .sideBySideSwapped:
      return (rightHalf, leftHalf, true)
    case .topBottom:
      return (topHalf, bottomHalf, true)
    case .topBottomSwapped:
      return (bottomHalf, topHalf, true)
    case .none:
      let whole = SIMD4<Float>(0, 0, 1, 1)
      return (whole, whole, false)
    }
  }

  /// Width over height of an eye as seen. Half side by side videos squeeze each eye to half its width, half top and
  /// bottom ones to half its height: an eye much narrower (or wider) than any usual picture is one of those, and
  /// shows at the shape of the whole frame.
  static func displayAspect(frameWidth: Float, frameHeight: Float, rect: SIMD4<Float>, layout: SpatialMetalLayout)
    -> Float
  {
    let width = frameWidth * rect.z
    let height = frameHeight * rect.w
    guard width > 0, height > 0 else { return 16 / 9 }
    var aspect = width / height
    switch layout {
    case .sideBySide, .sideBySideSwapped:
      if aspect < 1.2 { aspect *= 2 }
    case .topBottom, .topBottomSwapped:
      if aspect > 2.5 { aspect /= 2 }
    case .none:
      break
    }
    return aspect
  }

  /// The largest rectangle of [contentAspect] centred in the target
  static func letterbox(contentAspect: Float, width: Int, height: Int) -> MTLViewport {
    let boxWidth = Double(width)
    let boxHeight = Double(height)
    let aspect = Double(max(contentAspect, 0.01))
    var viewportWidth = boxWidth
    var viewportHeight = boxWidth / aspect
    if viewportHeight > boxHeight {
      viewportHeight = boxHeight
      viewportWidth = boxHeight * aspect
    }
    return MTLViewport(
      originX: (boxWidth - viewportWidth) / 2,
      originY: (boxHeight - viewportHeight) / 2,
      width: viewportWidth,
      height: viewportHeight,
      znear: 0,
      zfar: 1
    )
  }

  // MARK: - Equirectangular eyes

  /// The two eye views of an equirectangular video, at the shape of the output, made again when its size changes
  private func eyeViewportTextures(width outputWidth: Int, height outputHeight: Int) -> (
    left: MTLTexture, right: MTLTexture
  )? {
    let longer = max(outputWidth, outputHeight)
    let scale = min(1, Double(Self.maxViewportSize) / Double(max(longer, 1)))
    let width = max(Int((Double(outputWidth) * scale).rounded()), 1)
    let height = max(Int((Double(outputHeight) * scale).rounded()), 1)
    if let existing = eyeViewports, existing.left.width == width, existing.left.height == height {
      return existing
    }
    let descriptor = MTLTextureDescriptor.texture2DDescriptor(
      pixelFormat: Self.pixelFormat,
      width: width,
      height: height,
      mipmapped: false
    )
    descriptor.usage = [.renderTarget, .shaderRead]
    descriptor.storageMode = .private
    guard let left = device.makeTexture(descriptor: descriptor),
      let right = device.makeTexture(descriptor: descriptor)
    else {
      return nil
    }
    eyeViewports = (left, right)
    // The maps follow the shape of the views
    maps = nil
    return (left, right)
  }

  private func encodeViewport(_ commandBuffer: MTLCommandBuffer, source: MTLTexture, rect: SIMD4<Float>, target: MTLTexture) {
    guard let viewportPipeline else { return }
    let pass = MTLRenderPassDescriptor()
    pass.colorAttachments[0].texture = target
    pass.colorAttachments[0].loadAction = .dontCare
    pass.colorAttachments[0].storeAction = .store
    guard let encoder = commandBuffer.makeRenderCommandEncoder(descriptor: pass) else { return }
    var uniforms = viewportUniforms(rect: rect, aspect: Float(target.width) / Float(max(target.height, 1)))
    encoder.setRenderPipelineState(viewportPipeline)
    encoder.setFragmentTexture(source, index: 0)
    encoder.setFragmentBytes(&uniforms, length: MemoryLayout<SpatialViewportUniforms>.stride, index: 0)
    encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
    encoder.endEncoding()
  }

  /// The vertical field of view [fovDegrees] spans the height of the output, as in renderer C
  private func viewportUniforms(rect: SIMD4<Float>, aspect: Float) -> SpatialViewportUniforms {
    let fov = min(max(fovDegrees, 1), 170)
    let vertical = tan(fov * Float.pi / 360)
    let horizontal = vertical * max(aspect, 0.01)
    // The eye of a half sphere spans half the longitudes of a full one
    let longitudeSpan: Float = halfSphere ? Float.pi : 2 * Float.pi
    return SpatialViewportUniforms(
      rotation: simd_float4x4(cameraOrientation()),
      rect: rect,
      lens: SIMD4<Float>(horizontal, vertical, 0, 0),
      sphere: SIMD4<Float>(longitudeSpan, 0, 0, 0)
    )
  }

  /// Camera space to world. The shaders' longitude grows to the right and the camera's yaw turns it to the left, so a
  /// view looking [yawDegrees] to the right turns the camera by minus that; the pitch turns it up.
  private func cameraOrientation() -> simd_quatf {
    let yaw = -yawDegrees * Float.pi / 180
    let pitch = min(max(pitchDegrees, -90), 90) * Float.pi / 180
    return simd_quatf(angle: yaw, axis: SIMD3<Float>(0, 1, 0)) * simd_quatf(angle: pitch, axis: SIMD3<Float>(1, 0, 0))
  }

  // MARK: - Disparity

  /// The maps of the current tier, with the height that keeps the pixels of the eye square
  private func disparityMaps(eyeAspect: Float) -> SpatialMetalMaps? {
    let width = quality.mapWidth
    var height = Int((Float(width) / max(eyeAspect, 0.01)).rounded())
    height = min(max(height, width / 4), width * 2)
    height += height % 2
    if let maps, maps.width == width, maps.height == height {
      return maps
    }
    maps = SpatialMetalMaps(device: device, width: width, height: height)
    // The second that makes the maps and first runs the passes is not representative
    warmupWindows = max(warmupWindows, 1)
    return maps
  }

  private func encodeDisparity(
    _ commandBuffer: MTLCommandBuffer,
    maps: SpatialMetalMaps,
    left: MTLTexture,
    leftRect: SIMD4<Float>,
    right: MTLTexture,
    rightRect: SIMD4<Float>
  ) {
    guard let grayPipeline, let disparityPipeline, let consistencyPipeline, let blurPipeline, let temporalPipeline,
      let encoder = commandBuffer.makeComputeCommandEncoder()
    else {
      return
    }
    let width = maps.width
    let height = maps.height
    let check = quality.consistencyCheck

    // Grayscale eyes at the size of the map
    encoder.setComputePipelineState(grayPipeline)
    for (eye, rect, gray) in [(left, leftRect, maps.grayLeft), (right, rightRect, maps.grayRight)] {
      var eyeRect = rect
      encoder.setTexture(eye, index: 0)
      encoder.setTexture(gray, index: 1)
      encoder.setBytes(&eyeRect, length: MemoryLayout<SIMD4<Float>>.stride, index: 0)
      Self.dispatch(encoder, width: width, height: height)
    }

    // Block matching, from the left eye, and from the right eye for the consistency check
    encoder.setComputePipelineState(disparityPipeline)
    var leftConfig = SIMD4<Int32>(1, Int32(Self.searchRange), 0, 0)
    encoder.setTexture(maps.grayLeft, index: 0)
    encoder.setTexture(maps.grayRight, index: 1)
    encoder.setTexture(maps.rawLeft, index: 2)
    encoder.setBytes(&leftConfig, length: MemoryLayout<SIMD4<Int32>>.stride, index: 0)
    Self.dispatch(encoder, width: width, height: height)
    if check {
      var rightConfig = SIMD4<Int32>(-1, Int32(Self.searchRange), 0, 0)
      encoder.setTexture(maps.grayRight, index: 0)
      encoder.setTexture(maps.grayLeft, index: 1)
      encoder.setTexture(maps.rawRight, index: 2)
      encoder.setBytes(&rightConfig, length: MemoryLayout<SIMD4<Int32>>.stride, index: 0)
      Self.dispatch(encoder, width: width, height: height)

      encoder.setComputePipelineState(consistencyPipeline)
      encoder.setTexture(maps.rawLeft, index: 0)
      encoder.setTexture(maps.rawRight, index: 1)
      encoder.setTexture(maps.checkedLeft, index: 2)
      encoder.setTexture(maps.checkedRight, index: 3)
      Self.dispatch(encoder, width: width, height: height)
    }

    // Edge aware blur, horizontal then vertical, guided by the grayscale eye
    encoder.setComputePipelineState(blurPipeline)
    var sides = [(check ? maps.checkedLeft : maps.rawLeft, maps.grayLeft, maps.blurPassLeft, maps.blurredLeft)]
    if check {
      sides.append((maps.checkedRight, maps.grayRight, maps.blurPassRight, maps.blurredRight))
    }
    for (input, guide, blurPass, output) in sides {
      var horizontal = SIMD4<Int32>(1, 0, 0, 0)
      encoder.setTexture(input, index: 0)
      encoder.setTexture(guide, index: 1)
      encoder.setTexture(blurPass, index: 2)
      encoder.setBytes(&horizontal, length: MemoryLayout<SIMD4<Int32>>.stride, index: 0)
      Self.dispatch(encoder, width: width, height: height)
      var vertical = SIMD4<Int32>(0, 1, 0, 0)
      encoder.setTexture(blurPass, index: 0)
      encoder.setTexture(guide, index: 1)
      encoder.setTexture(output, index: 2)
      encoder.setBytes(&vertical, length: MemoryLayout<SIMD4<Int32>>.stride, index: 0)
      Self.dispatch(encoder, width: width, height: height)
    }

    // Blend over time into the other texture of each ping pong pair
    let next = 1 - maps.current
    let reset = !maps.hasHistory || (check && !maps.hasRightMap)
    encoder.setComputePipelineState(temporalPipeline)
    var blends = [(maps.blurredLeft, maps.historyLeft[maps.current], maps.historyLeft[next])]
    if check {
      blends.append((maps.blurredRight, maps.historyRight[maps.current], maps.historyRight[next]))
    }
    for (current, history, output) in blends {
      var config = SIMD4<Int32>(reset ? 1 : 0, 0, 0, 0)
      encoder.setTexture(current, index: 0)
      encoder.setTexture(history, index: 1)
      encoder.setTexture(output, index: 2)
      encoder.setBytes(&config, length: MemoryLayout<SIMD4<Int32>>.stride, index: 0)
      Self.dispatch(encoder, width: width, height: height)
    }
    encoder.endEncoding()

    maps.current = next
    maps.hasHistory = true
    maps.hasRightMap = check
  }

  /// 8 x 8 threadgroups, the tile of the block matching kernel, over the whole map
  private static func dispatch(_ encoder: MTLComputeCommandEncoder, width: Int, height: Int) {
    let group = 8
    let threads = MTLSize(width: group, height: group, depth: 1)
    let groups = MTLSize(width: (width + group - 1) / group, height: (height + group - 1) / group, depth: 1)
    encoder.dispatchThreadgroups(groups, threadsPerThreadgroup: threads)
  }

  // MARK: - Figures and the adaptive rule

  /// Once a second: the rates, the mean GPU time, and the step of the tier it calls for (12 ms down, 6 ms for five
  /// seconds up, never back up to a tier left for slowness within half a minute)
  private func updateStatistics(now: CFTimeInterval) {
    let elapsed = now - windowStart
    guard elapsed >= 1 else { return }
    let sum = gpuTimeSum
    let count = gpuTimeCount
    gpuTimeSum = 0
    gpuTimeCount = 0
    renderFps = Double(framesInWindow) / elapsed
    disparityFps = Double(updatesInWindow) / elapsed
    framesInWindow = 0
    updatesInWindow = 0
    windowStart = now
    guard count > 0 else { return }
    let average = sum / Double(count)
    gpuMs = average
    guard adaptiveQuality, stereo else {
      fastSeconds = 0
      return
    }
    if warmupWindows > 0 {
      warmupWindows -= 1
      return
    }
    if average > 12 {
      fastSeconds = 0
      if let lower = SpatialMetalQuality(rawValue: quality.rawValue - 1) {
        quality = lower
        lastStepDown = now
        warmupWindows = 1
        counters.qualityChanges += 1
      }
    } else if average < 6 {
      fastSeconds += 1
      if fastSeconds >= 5, now - lastStepDown > 30, let higher = SpatialMetalQuality(rawValue: quality.rawValue + 1) {
        quality = higher
        fastSeconds = 0
        warmupWindows = 1
        counters.qualityChanges += 1
      }
    } else {
      fastSeconds = 0
    }
  }
}
