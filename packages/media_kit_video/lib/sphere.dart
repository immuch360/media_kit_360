/// 360°, VR180 and side by side 3D video for Flutter desktop apps: the Dart side of renderer C, the view drawn on the
/// GPU by this package's plugin (Windows for now).
///
/// ```dart
/// final controller = VideoController(player);
/// final renderer = PluginRenderer.forController(controller);
/// final attached = await renderer.attach(
///   projection: PluginProjection.equirect(),
///   outputSize: const Size(1920, 1080), // physical pixels of the view
/// );
/// await player.open(Media(path)); // after the attach: the first frame is already the view
/// renderer.setView(const PluginView(yaw: 30, pitch: 10, fov: 90)); // on every drag event
/// ```
///
/// [PluginRenderer] drives one player, [RendererProbe] measures whether its tier keeps up with the video. The
/// method calls underneath are [ProjectionOutput]'s, exported by `package:media_kit_video/media_kit_video.dart` too.
///
/// Copyright (c) 2026 FreeKC. Use of this source code is governed by the MIT license of the LICENSE file.
library;

export 'package:media_kit_video/src/projection_output.dart';
export 'package:media_kit_video/src/sphere/plugin_renderer.dart';
export 'package:media_kit_video/src/sphere/renderer_probe.dart';
