// Immuch360 (IMMUCH360-NOTE.md, patch 18): the part of FlutterMacOS's interface the plugin's Swift uses, so that CI
// type checks the macOS plugin with swiftc alone, without a Flutter app (the example app has no macOS folder). The
// declarations follow FlutterMacOS's Objective-C headers as Swift imports them; like FlutterMacOS, the stub
// exports Cocoa's modules. Never shipped.

@_exported import AppKit
@_exported import CoreVideo
@_exported import Foundation

@objc public protocol FlutterTexture: NSObjectProtocol {
  func copyPixelBuffer() -> Unmanaged<CVPixelBuffer>?
  @objc optional func onTextureUnregistered(_ texture: FlutterTexture)
}

@objc public protocol FlutterTextureRegistry: NSObjectProtocol {
  func register(_ texture: FlutterTexture) -> Int64
  func textureFrameAvailable(_ textureId: Int64)
  func unregisterTexture(_ textureId: Int64)
}

@objc public protocol FlutterBinaryMessenger: NSObjectProtocol {}

public typealias FlutterResult = (Any?) -> Void

public let FlutterMethodNotImplemented: NSObject = NSObject()

open class FlutterMethodCall: NSObject {
  open var method: String = ""
  open var arguments: Any?
}

open class FlutterMethodChannel: NSObject {
  public init(name: String, binaryMessenger messenger: FlutterBinaryMessenger) {}
  open func invokeMethod(_ method: String, arguments: Any?) {}
}

@objc public protocol FlutterPluginRegistrar: NSObjectProtocol {
  var messenger: FlutterBinaryMessenger { get }
  var textures: FlutterTextureRegistry { get }
  var view: NSView? { get }
  func addMethodCallDelegate(_ delegate: FlutterPlugin, channel: FlutterMethodChannel)
}

@objc public protocol FlutterPlugin: NSObjectProtocol {
  static func register(with registrar: FlutterPluginRegistrar)
  @objc optional func handle(_ call: FlutterMethodCall, result: @escaping FlutterResult)
}
