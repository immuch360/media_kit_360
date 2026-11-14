/// The headset probe of Immuch360 Desktop: a player's 360 frame, a flat screen or a test pattern in a PC VR headset
/// through OpenXR (SteamVR, Meta Horizon Link, Virtual Desktop, Pico Connect), Windows only, in a build with the
/// MEDIA_KIT_360_XR option of CMake (IMMUCH360-NOTE.md, patch 15).
///
/// ```dart
/// final state = await XrProbe.state();
/// if (state.compiled) {
///   await XrProbe.start(mode: XrProbeMode.pattern, fps: 24, logPath: r'C:\Users\me\xr-probe.log');
/// }
/// ```
///
/// Copyright (c) 2026 FreeKC. Use of this source code is governed by the MIT license of the LICENSE file.
library;

export 'package:media_kit_video/src/xr_probe.dart';
