import CoreGraphics
import CoreVideo
import Foundation

#if canImport(Flutter)
  import Flutter
#elseif canImport(FlutterMacOS)
  import FlutterMacOS
#endif

// This class creates and manipulates the different types of FlutterTexture,
// handles resizing, rendering calls, and notify Flutter when a new frame is
// available to render.
//
// To improve the user experience, a worker is used to execute heavy tasks on a
// dedicated thread.
public class VideoOutput: NSObject {
  // Will be called on the main thread
  public typealias TextureUpdateCallback = (Int64, CGSize) -> Void

  private static let isSimulator: Bool = {
    let isSim: Bool
    #if targetEnvironment(simulator)
      isSim = true
    #else
      isSim = false
    #endif
    return isSim
  }()

  private let handle: OpaquePointer
  private let enableHardwareAcceleration: Bool
  private let registry: FlutterTextureRegistry
  private let textureUpdateCallback: TextureUpdateCallback
  private let worker: Worker = .init()
  private var width: Int64?
  private var height: Int64?
  private var texture: ResizableTextureProtocol!
  private var textureId: Int64 = -1
  private var currentSize: CGSize = CGSize.zero
  private var disposed: Bool = false
  // Immuch360 (IMMUCH360-NOTE.md, patch 18): the texture Flutter reads, the output of the Spatial passes while they
  // run, else mpv's frame; the OpenGL texture when there is one (it finishes its frame before Metal reads it); the
  // passes (a SpatialMetalOutput, kept as AnyObject: a stored property cannot have a type of macOS 10.15 here), read
  // and written under spatialLock; the size Flutter was told; whether a redraw of the frame kept is queued
  private let flutterTexture = SpatialSwitchTexture()
  private var hardware: TextureHW?
  private let spatialLock = NSLock()
  private var spatialObject: AnyObject?
  private var reportedSize: CGSize = CGSize.zero
  private var redrawQueued = false

  init(
    handle: Int64,
    configuration: VideoOutputConfiguration,
    registry: FlutterTextureRegistry,
    textureUpdateCallback: @escaping TextureUpdateCallback
  ) {
    let handle = OpaquePointer(bitPattern: Int(handle))
    assert(handle != nil, "handle casting")

    self.handle = handle!
    width = configuration.width
    height = configuration.height
    enableHardwareAcceleration = configuration.enableHardwareAcceleration
    self.registry = registry
    self.textureUpdateCallback = textureUpdateCallback

    super.init()

    worker.enqueue {
      self._init()
    }
  }

  deinit {
    worker.cancel()

    disposed = true
    disposeTextureId()
  }

  public func setSize(width: Int64?, height: Int64?) {
    worker.enqueue {
      self.width = width
      self.height = height
    }
  }

  private func _init() {
    let enableHardwareAcceleration =
      VideoOutput.isSimulator ? false : enableHardwareAcceleration

    NSLog(
      "VideoOutput: enableHardwareAcceleration: \(enableHardwareAcceleration)"
    )

    if VideoOutput.isSimulator {
      NSLog(
        "VideoOutput: warning: hardware rendering is disabled in the iOS simulator, due to an incompatibility with OpenGL ES"
      )
    }

    // Immuch360: TextureHW is nil when OpenGL gives no context (a Mac virtual machine, or a macOS without OpenGL):
    // the software rendering of TextureSW then, as with hardware acceleration off, instead of a crash
    var hardwareTexture: TextureHW? = nil
    if enableHardwareAcceleration {
      hardwareTexture = TextureHW(
        handle: handle,
        // Use `weak self` to prevent memory leaks
        updateCallback: { [weak self]() in
          guard let that = self else {
            return
          }
          that.updateCallback()
        }
      )
      if hardwareTexture == nil {
        NSLog(
          "VideoOutput: no OpenGL context for hardware rendering, software rendering instead"
        )
      }
    }

    if let hardwareTexture = hardwareTexture {
      hardware = hardwareTexture
      texture = SafeResizableTexture(hardwareTexture)
    } else {
      texture = SafeResizableTexture(
        TextureSW(
          handle: handle,
          // Use `weak self` to prevent memory leaks
          updateCallback: { [weak self]() in
            guard let that = self else {
              return
            }
            that.updateCallback()
          }
        )
      )
    }

    DispatchQueue.main.sync { [weak self]() in
      guard let that = self else {
        return
      }
      that.registerTextureId()
    }
  }

  // Must be run on the main thread
  private func registerTextureId() {
    // Textures must be registered on the platform thread.
    flutterTexture.setInner(texture)
    textureId = registry.register(flutterTexture)
    // textureUpdateCallback must run on the main thread
    textureUpdateCallback(textureId, CGSize(width: 0, height: 0))
  }

  private func disposeTextureId() {
    let registry_ = self.registry
    let textureId_ = self.textureId
    textureId = -1
    DispatchQueue.main.async {
      // Textures must be unregistered on the platform thread
      registry_.unregisterTexture(textureId_)
    }
  }

  public func updateCallback() {
    worker.enqueue {
      self._updateCallback()
    }
  }

  private func _updateCallback() {
    if #available(macOS 10.15, *), let spatial = spatialOutput {
      _updateSpatial(spatial)
      return
    }

    let size = videoSize

    if size.width == 0 || size.height == 0 {
      return
    }

    if currentSize != size {
      currentSize = size

      texture.resize(size)
    }
    if reportedSize != size {
      reportedSize = size
      DispatchQueue.main.sync { [weak self] in
        guard let that = self else { return }
        // textureUpdateCallback must run on the main thread
        that.textureUpdateCallback(that.textureId, size)
      }
    }

    if disposed {
      return
    }

    texture.render(size)
    DispatchQueue.main.sync { [weak self] in
      guard let that = self else { return }
      // Textures must be marked as available from the main thread
      that.registry.textureFrameAvailable(that.textureId)
    }
  }

  // MARK: - Immuch360, the Spatial passes (IMMUCH360-NOTE.md, patch 18)

  @available(macOS 10.15, *)
  private var spatialOutput: SpatialMetalOutput? {
    spatialLock.lock()
    defer { spatialLock.unlock() }
    return spatialObject as? SpatialMetalOutput
  }

  /// The video's own size, turned as mpv shows it, whatever size Dart fixed
  private var naturalVideoSize: CGSize {
    let params = MPVHelpers.getVideoOutParams(handle)
    let turned = params.rotate == 90 || params.rotate == 270
    return CGSize(
      width: Double(turned ? params.dh : params.dw),
      height: Double(turned ? params.dw : params.dh)
    )
  }

  /// Turns the passes on with the arguments of VideoOutputManager.SetSpatial, or changes their setup, or turns them
  /// off; [completion] gets the answer of ProjectionResult (on the worker)
  public func setSpatial(_ arguments: [String: Any], completion: @escaping ([String: Any]) -> Void) {
    worker.enqueue { [weak self] in
      guard let that = self else {
        completion(["ok": false, "reason": "no video output"])
        return
      }
      completion(that._setSpatial(arguments))
    }
  }

  private func _setSpatial(_ arguments: [String: Any]) -> [String: Any] {
    let enabled = (arguments["enabled"] as? NSNumber)?.boolValue ?? false
    let viewpoint = (arguments["viewpoint"] as? NSNumber).map { Float(truncating: $0) }
    guard #available(macOS 10.15, *) else {
      return ["ok": false, "reason": "macOS 10.15 or later is needed for Metal"]
    }
    if !enabled {
      let wasOn = spatialOutput != nil
      setSpatialObject(nil)
      hardware?.finishBeforeRead = false
      flutterTexture.setSpatial(nil)
      if wasOn {
        // The frame at the video's size again, and Flutter told its size
        currentSize = .zero
        reportedSize = .zero
        _updateCallback()
      }
      return ["ok": true]
    }
    let setup = SpatialMetalOutput.Setup.from(arguments)
    var reason = ""
    if let existing = spatialOutput {
      existing.apply(setup)
      if let viewpoint {
        existing.setViewpoint(viewpoint)
      }
    } else {
      let (made, why) = SpatialMetalOutput.make(setup: setup, viewpoint: viewpoint)
      guard let made else {
        return ["ok": false, "reason": why]
      }
      reason = why
      setSpatialObject(made)
      hardware?.finishBeforeRead = true
      // The frame at the size the passes ask for
      currentSize = .zero
    }
    let spatial = spatialOutput!
    let frame = spatial.frameSize(video: naturalVideoSize)
    let output = spatial.outputSize(frame: frame)
    // The frame shown at once, a paused one included
    _updateSpatial(spatial)
    var answer: [String: Any] = [
      "ok": true,
      "glRenderer": "Metal (\(spatial.renderer.deviceName))",
      "outputWidth": Int(output.width),
      "outputHeight": Int(output.height),
    ]
    if !reason.isEmpty {
      answer["reason"] = reason
    }
    return answer
  }

  private func setSpatialObject(_ object: AnyObject?) {
    spatialLock.lock()
    spatialObject = object
    spatialLock.unlock()
  }

  /// The viewpoint, from 0 (the left camera) to 1 (the right one); the frame kept is drawn again. False when the
  /// passes are off.
  public func setViewpoint(_ viewpoint: Float) -> Bool {
    guard #available(macOS 10.15, *), let spatial = spatialOutput else { return false }
    spatial.setViewpoint(viewpoint)
    queueRedraw()
    return true
  }

  /// The view of an equirectangular video, as ProjectionOutput.setView gives it
  public func setView(yaw: Float, pitch: Float, fov: Float) -> Bool {
    guard #available(macOS 10.15, *), let spatial = spatialOutput else { return false }
    spatial.setView(yaw: yaw, pitch: pitch, fov: fov)
    queueRedraw()
    return true
  }

  public func setSpatialDebug(_ disparityMap: Bool) -> Bool {
    guard #available(macOS 10.15, *), let spatial = spatialOutput else { return false }
    spatial.setDebug(disparityMap)
    queueRedraw()
    return true
  }

  /// The map of SpatialStats, null when the passes are off
  public func spatialStats(probe: Bool) -> [String: Any]? {
    guard #available(macOS 10.15, *), let spatial = spatialOutput else { return nil }
    if probe {
      // The pixels come with the next draw
      defer { queueRedraw() }
      return spatial.stats(probe: true)
    }
    return spatial.stats(probe: false)
  }

  /// One redraw of the frame kept at a time: a viewpoint that moves faster than the passes draw is merged
  private func queueRedraw() {
    spatialLock.lock()
    let queued = redrawQueued
    redrawQueued = true
    spatialLock.unlock()
    if queued {
      return
    }
    worker.enqueue { [weak self] in
      guard let that = self else { return }
      that.spatialLock.lock()
      that.redrawQueued = false
      that.spatialLock.unlock()
      guard #available(macOS 10.15, *), let spatial = that.spatialOutput, !that.disposed else { return }
      guard that.currentSize.width > 0, that.currentSize.height > 0,
        let kept = that.texture.copyPixelBuffer()?.takeRetainedValue()
      else {
        return
      }
      let output = spatial.outputSize(frame: that.currentSize)
      that.present(spatial, source: kept, output: output, newFrame: false)
    }
  }

  /// A new frame of mpv through the passes: mpv draws at the frame size, the passes into the output
  @available(macOS 10.15, *)
  private func _updateSpatial(_ spatial: SpatialMetalOutput) {
    let frame = spatial.frameSize(video: naturalVideoSize)
    if frame.width == 0 || frame.height == 0 {
      return
    }
    if currentSize != frame {
      currentSize = frame
      texture.resize(frame)
    }
    if disposed {
      return
    }
    texture.render(frame)
    guard let source = texture.copyPixelBuffer()?.takeRetainedValue() else {
      return
    }
    present(spatial, source: source, output: spatial.outputSize(frame: frame), newFrame: true)
  }

  @available(macOS 10.15, *)
  private func present(_ spatial: SpatialMetalOutput, source: CVPixelBuffer, output: CGSize, newFrame: Bool) {
    if reportedSize != output {
      reportedSize = output
      DispatchQueue.main.sync { [weak self] in
        guard let that = self else { return }
        that.textureUpdateCallback(that.textureId, output)
      }
    }
    guard spatial.draw(source: source, size: output, newFrame: newFrame), let drawn = spatial.current else {
      return
    }
    flutterTexture.setSpatial(drawn)
    DispatchQueue.main.sync { [weak self] in
      guard let that = self else { return }
      that.registry.textureFrameAvailable(that.textureId)
    }
  }

    private var videoSize: CGSize {
        // fixed size
        if width != nil && height != nil {
            return CGSize(
                width: Double(width!),
                height: Double(height!)
            )
        }
        
        let params = MPVHelpers.getVideoOutParams(handle)
        return CGSize(
            width: Double(width ?? (params.rotate == 0 || params.rotate == 180
                                    ? params.dw
                                    : params.dh)),
            height: Double(height ?? (params.rotate == 0 || params.rotate == 180
                                      ? params.dh
                                      : params.dw))
        )
  }
}

/// Immuch360 (IMMUCH360-NOTE.md, patch 18): the texture registered with Flutter. It shows the output of the Spatial
/// passes while they run, else the frame of mpv's texture, so that turning the passes on or off keeps the texture id
/// the VideoController holds.
final class SpatialSwitchTexture: NSObject, FlutterTexture {
  private let lock = NSLock()
  private var inner: FlutterTexture?
  private var spatial: CVPixelBuffer?

  func setInner(_ texture: FlutterTexture) {
    lock.lock()
    inner = texture
    lock.unlock()
  }

  func setSpatial(_ buffer: CVPixelBuffer?) {
    lock.lock()
    spatial = buffer
    lock.unlock()
  }

  func copyPixelBuffer() -> Unmanaged<CVPixelBuffer>? {
    lock.lock()
    let buffer = spatial
    let texture = inner
    lock.unlock()
    if let buffer {
      return Unmanaged.passRetained(buffer)
    }
    return texture?.copyPixelBuffer()
  }
}
