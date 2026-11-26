#if canImport(Flutter)
  import Flutter
#elseif canImport(FlutterMacOS)
  import FlutterMacOS
#endif

public class VideoOutputManager: NSObject {
  private let registry: FlutterTextureRegistry
  private var videoOutputs = [Int64: VideoOutput]()

  init(registry: FlutterTextureRegistry) {
    self.registry = registry
  }

  public func create(
    handle: Int64,
    configuration: VideoOutputConfiguration,
    textureUpdateCallback: @escaping VideoOutput.TextureUpdateCallback
  ) {
    let videoOutput = VideoOutput(
      handle: handle,
      configuration: configuration,
      registry: self.registry,
      textureUpdateCallback: textureUpdateCallback
    )

    self.videoOutputs[handle] = videoOutput
  }

  public func setSize(
    handle: Int64,
    width: Int64?,
    height: Int64?
  ) {
    let videoOutput = self.videoOutputs[handle]
    if videoOutput == nil {
      return
    }

    videoOutput!.setSize(
      width: width,
      height: height
    )
  }

  // Immuch360 (IMMUCH360-NOTE.md, patch 18): the Spatial passes of a player

  public func setSpatial(
    handle: Int64,
    arguments: [String: Any],
    completion: @escaping ([String: Any]) -> Void
  ) {
    guard let videoOutput = self.videoOutputs[handle] else {
      completion(["ok": false, "reason": "no video output"])
      return
    }
    videoOutput.setSpatial(arguments, completion: completion)
  }

  public func setViewpoint(handle: Int64, viewpoint: Float) -> Bool {
    return self.videoOutputs[handle]?.setViewpoint(viewpoint) ?? false
  }

  public func setView(handle: Int64, yaw: Float, pitch: Float, fov: Float) -> Bool {
    return self.videoOutputs[handle]?.setView(yaw: yaw, pitch: pitch, fov: fov) ?? false
  }

  public func setSpatialDebug(handle: Int64, disparityMap: Bool) -> Bool {
    return self.videoOutputs[handle]?.setSpatialDebug(disparityMap) ?? false
  }

  public func spatialStats(handle: Int64, probe: Bool) -> [String: Any]? {
    return self.videoOutputs[handle]?.spatialStats(probe: probe)
  }

  public func destroy(
    handle: Int64
  ) {
    let videoOutput = self.videoOutputs[handle]
    if videoOutput == nil {
      return
    }

    self.videoOutputs[handle] = nil
  }
}
