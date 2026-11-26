#if canImport(Flutter)
  import Flutter
#elseif canImport(FlutterMacOS)
  import FlutterMacOS
#endif

public class MediaKitVideoPlugin: NSObject, FlutterPlugin {
  private static let CHANNEL_NAME = "com.alexmercerind/media_kit_video"

  public static func register(with registrar: FlutterPluginRegistrar) {
    #if canImport(Flutter)
      let binaryMessenger = registrar.messenger()
      let registry = registrar.textures()
      let utils: UtilsProtocol? = nil
    #elseif canImport(FlutterMacOS)
      let binaryMessenger = registrar.messenger
      let registry = registrar.textures
      let utils: UtilsProtocol? = Utils(registrar)
    #endif

    let channel = FlutterMethodChannel(
      name: CHANNEL_NAME,
      binaryMessenger: binaryMessenger
    )
    let instance = MediaKitVideoPlugin(
      registry: registry,
      channel: channel,
      utils: utils
    )
    registrar.addMethodCallDelegate(instance, channel: channel)
  }

  private let channel: FlutterMethodChannel
  private let videoOutputManager: VideoOutputManager
  private let utils: UtilsProtocol?

  init(
    registry: FlutterTextureRegistry,
    channel: FlutterMethodChannel,
    utils: UtilsProtocol?
  ) {
    self.channel = channel
    videoOutputManager = VideoOutputManager(
      registry: registry
    )
    self.utils = utils
  }

  public func handle(
    _ call: FlutterMethodCall,
    result: @escaping FlutterResult
  ) {
    switch call.method {
    case "VideoOutputManager.Create":
      handleCreateMethodCall(call.arguments, result)
    case "VideoOutputManager.SetSize":
      handleSetSizeMethodCall(call.arguments, result)
    case "VideoOutputManager.Dispose":
      handleDisposeMethodCall(call.arguments, result)
    // Immuch360 (IMMUCH360-NOTE.md, patch 18): the Spatial passes, in Metal
    case "VideoOutputManager.SetSpatial":
      handleSetSpatialMethodCall(call.arguments, result)
    case "VideoOutputManager.SetViewpoint":
      let args = call.arguments as? [String: Any] ?? [:]
      result(
        videoOutputManager.setViewpoint(
          handle: MediaKitVideoPlugin.handleOf(args),
          viewpoint: MediaKitVideoPlugin.floatOf(args["viewpoint"], 0.5)
        ))
    case "VideoOutputManager.SetView":
      let args = call.arguments as? [String: Any] ?? [:]
      result(
        videoOutputManager.setView(
          handle: MediaKitVideoPlugin.handleOf(args),
          yaw: MediaKitVideoPlugin.floatOf(args["yaw"], 0),
          pitch: MediaKitVideoPlugin.floatOf(args["pitch"], 0),
          fov: MediaKitVideoPlugin.floatOf(args["fov"], 90)
        ))
    case "VideoOutputManager.SetSpatialDebug":
      let args = call.arguments as? [String: Any] ?? [:]
      result(
        videoOutputManager.setSpatialDebug(
          handle: MediaKitVideoPlugin.handleOf(args),
          disparityMap: (args["disparityMap"] as? NSNumber)?.boolValue ?? false
        ))
    case "VideoOutputManager.SpatialStats":
      let args = call.arguments as? [String: Any] ?? [:]
      result(
        videoOutputManager.spatialStats(
          handle: MediaKitVideoPlugin.handleOf(args),
          probe: (args["probe"] as? NSNumber)?.boolValue ?? false
        ))
    case "Utils.EnterNativeFullscreen":
      handleEnterNativeFullscreenMethodCall(call.arguments, result)
    case "Utils.ExitNativeFullscreen":
      handleExitNativeFullscreenMethodCall(call.arguments, result)
    default:
      result(FlutterMethodNotImplemented)
    }
  }

  private func handleCreateMethodCall(
    _ arguments: Any?,
    _ result: FlutterResult
  ) {
    let args = arguments as? [String: Any]
    let handleStr = args?["handle"] as! String
    let handle: Int64? = Int64(handleStr)
    let configDict = args?["configuration"] as! [String: Any]
    let configuration = VideoOutputConfiguration.fromDict(configDict)

    assert(handle != nil, "handle must be an Int64")

    videoOutputManager.create(
      handle: handle!,
      configuration: configuration,
      textureUpdateCallback: { (_ textureId: Int64, _ size: CGSize) in
        self.channel.invokeMethod(
          "VideoOutput.Resize",
          arguments: [
            "handle": handle!,
            "id": textureId,
            "rect": [
              "top": 0,
              "left": 0,
              "width": size.width,
              "height": size.height,
            ],
          ] as [String: Any]
        )
      }
    )

    result(nil)
  }

  // Immuch360: the Spatial calls send the handle as a number, upstream's calls as a string
  private static func handleOf(_ args: [String: Any]) -> Int64 {
    if let number = args["handle"] as? NSNumber {
      return number.int64Value
    }
    if let text = args["handle"] as? String, let value = Int64(text) {
      return value
    }
    return 0
  }

  private static func floatOf(_ value: Any?, _ fallback: Float) -> Float {
    if let number = value as? NSNumber {
      return number.floatValue
    }
    return fallback
  }

  private func handleSetSpatialMethodCall(
    _ arguments: Any?,
    _ result: @escaping FlutterResult
  ) {
    let args = arguments as? [String: Any] ?? [:]
    videoOutputManager.setSpatial(
      handle: MediaKitVideoPlugin.handleOf(args),
      arguments: args,
      completion: { answer in
        DispatchQueue.main.async {
          result(answer)
        }
      }
    )
  }

  private func handleSetSizeMethodCall(
    _ arguments: Any?,
    _ result: FlutterResult
  ) {
    let args = arguments as? [String: Any]
    let handleStr = args?["handle"] as! String
    let widthStr = args?["width"] as! String
    let heightStr = args?["height"] as! String

    let handle: Int64? = Int64(handleStr)
    let width: Int64? = Int64(widthStr)
    let height: Int64? = Int64(heightStr)

    assert(handle != nil, "handle must be an Int64")

    self.videoOutputManager.setSize(
      handle: handle!,
      width: width,
      height: height
    )

    result(nil)
  }

  private func handleDisposeMethodCall(
    _ arguments: Any?,
    _ result: FlutterResult
  ) {
    let args = arguments as? [String: Any]
    let handleStr = args?["handle"] as! String
    let handle: Int64? = Int64(handleStr)

    assert(handle != nil, "handle must be an Int64")

    videoOutputManager.destroy(
      handle: handle!
    )

    result(nil)
  }

  private func handleEnterNativeFullscreenMethodCall(
    _: Any?,
    _ result: FlutterResult
  ) {
    if utils == nil {
      return result(FlutterMethodNotImplemented)
    }

    utils?.enterNativeFullscreen()
    result(nil)
  }

  private func handleExitNativeFullscreenMethodCall(
    _: Any?,
    _ result: FlutterResult
  ) {
    if utils == nil {
      return result(FlutterMethodNotImplemented)
    }

    utils?.exitNativeFullscreen()
    result(nil)
  }
}
