/// The headset view of Immuch360 Desktop: a player's 360 video around you, a flat film on a screen, a test pattern
/// or a dark room with a status strip, in a PC VR headset through OpenXR (SteamVR, Meta Horizon Link, Virtual
/// Desktop, Pico Connect), with the controllers' events polled by the app. Windows only, in a build with the
/// MEDIA_KIT_360_XR option of CMake (IMMUCH360-NOTE.md, patch 15). Experimental: not yet tried in a headset by the
/// project.
///
/// ```dart
/// final runtime = await XrHeadset.runtimeInfo();
/// if (runtime.registered) {
///   await XrHeadset.start(XrStartRequest(
///     content: XrContent(handle: await player.handle, mode: XrHeadsetMode.sphere, fps: 30),
///     logPath: r'C:\Users\me\headset.log',
///   ));
///   final state = await XrHeadset.poll();
/// }
/// ```
///
/// Copyright (c) 2026 FreeKC. Use of this source code is governed by the MIT license of the LICENSE file.
library;

export 'package:media_kit_video/src/xr_headset.dart';
