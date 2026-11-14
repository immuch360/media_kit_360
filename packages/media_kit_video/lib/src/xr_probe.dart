/// Immuch360: the headset probe (IMMUCH360-NOTE.md, patch 15), the calls of the plugin's OpenXR session on Windows.
///
/// The plugin draws a player's frame in a PC VR headset through whatever OpenXR runtime is active (SteamVR, Meta
/// Horizon Link, Virtual Desktop, Pico Connect), each eye by renderer C from the eye's pose and field of view, or the
/// frame as a flat screen, or a test pattern with no player at all. Only a build with the MEDIA_KIT_360_XR option of
/// CMake has it ([XrProbeState.compiled]); everything else answers that it is not in the build. It is a probe: every
/// step goes to the log file given to [XrProbe.start], which is what tells, after a session with a headset, how the
/// runtime, the graphics card and the frames behaved.
library;

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';

/// What the headset shows
enum XrProbeMode {
  /// The player's 360 or 180 frame around the head, in renderer C's projection of the player
  sphere,

  /// The player's frame as a flat screen 2.5 m ahead, 2D or 3D
  screen,

  /// A test pattern, no player: a grey ramp ahead (the gamma), a red patch for the left eye only and a blue one for
  /// the right eye only, a bar moving at the frame rate given (the judder), green up, orange down
  pattern,
}

/// How a stereo frame holds its two eyes
enum XrProbeStereo { mono, leftRight, topBottom }

/// The answer of [XrProbe.start]
@immutable
class XrProbeResult {
  const XrProbeResult({required this.ok, this.reason});

  factory XrProbeResult.fromMap(Object? value) {
    final map = value is Map ? value : const {};
    return XrProbeResult(ok: map['ok'] == true, reason: map['reason'] as String?);
  }

  final bool ok;

  /// Why not: "a headset view already runs", or that the build has no headset view
  final String? reason;

  @override
  String toString() => 'XrProbeResult(${ok ? 'ok' : 'refused: $reason'})';
}

/// Where the probe is, from [XrProbe.state]
@immutable
class XrProbeState {
  const XrProbeState({
    required this.compiled,
    required this.running,
    required this.phase,
    this.sessionState = '',
    this.reason = '',
    this.runtime = '',
    this.system = '',
    this.swapchainPath = '',
    this.frames = 0,
    this.framesShown = 0,
    this.instanceLost = false,
    this.logPath = '',
  });

  factory XrProbeState.fromMap(Object? value) {
    final map = value is Map ? value : const {};
    String text(String key) => map[key] is String ? map[key] as String : '';
    int count(String key) => map[key] is int ? map[key] as int : 0;
    return XrProbeState(
      compiled: map['compiled'] == true,
      running: map['running'] == true,
      phase: text('phase'),
      sessionState: text('sessionState'),
      reason: text('reason'),
      runtime: text('runtime'),
      system: text('system'),
      swapchainPath: text('swapchainPath'),
      frames: count('frames'),
      framesShown: count('framesShown'),
      instanceLost: map['instanceLost'] == true,
      logPath: text('logPath'),
    );
  }

  /// Whether this build of the plugin has the probe
  final bool compiled;
  final bool running;

  /// "starting", "waiting for the headset", "running", "stopped", "never started", or "not in this build"
  final String phase;

  /// The OpenXR session state: "idle", "ready", "synchronized", "visible", "focused", "stopping"...
  final String sessionState;

  /// Why it stopped, once stopped: "no OpenXR runtime is active on this computer", "no headset connected to the
  /// runtime", "the headset is on another graphics card", "stopped by the app"...
  final String reason;

  /// The runtime's name and version, and the headset's name, once known
  final String runtime;
  final String system;

  /// How the eyes reach the runtime's images: "direct (...)" or "copy (...)"
  final String swapchainPath;
  final int frames;
  final int framesShown;
  final bool instanceLost;
  final String logPath;

  @override
  String toString() =>
      'XrProbeState($phase, session $sessionState, $frames frames, $framesShown shown'
      '${reason.isEmpty ? '' : ', $reason'})';
}

/// The calls of the headset probe on the channel of media_kit_video
abstract final class XrProbe {
  static const _channel = MethodChannel('com.alexmercerind/media_kit_video');

  /// Starts the headset view: of the player [handle] (`Player.handle`, renderer C on for it) for [XrProbeMode.sphere]
  /// and [XrProbeMode.screen], none for [XrProbeMode.pattern]. [stereo] picks each eye's part of the frame, [fps] the
  /// content's rate (the refresh rate asked is a multiple of it where the runtime offers one; the pattern's bar moves
  /// at it), [yaw] the window's yaw, kept ahead. [logPath] the file the probe writes; [label] a description for its
  /// first lines (never a URL or a token). Returns at once: the probe goes on on a thread of the plugin, see [state].
  static Future<XrProbeResult> start({
    int? handle,
    required XrProbeMode mode,
    XrProbeStereo stereo = XrProbeStereo.mono,
    double fps = 0,
    double yaw = 0,
    required String logPath,
    String label = '',
  }) async {
    try {
      return XrProbeResult.fromMap(
        await _channel.invokeMethod<Object?>('VideoOutputManager.XrStart', {
          'handle': handle ?? 0,
          'mode': mode.name,
          'stereo': stereo.name,
          'fps': fps,
          'yaw': yaw,
          'logPath': logPath,
          'label': label,
        }),
      );
    } on MissingPluginException {
      return const XrProbeResult(ok: false, reason: 'no plugin');
    }
  }

  /// Ends the headset view; false when the runtime did not end it within a few seconds (the probe's thread then ends
  /// on its own)
  static Future<bool> stop() async {
    try {
      final answer = await _channel.invokeMethod<Object?>('VideoOutputManager.XrStop');
      return answer is Map && answer['ok'] == true;
    } on MissingPluginException {
      return true;
    }
  }

  /// What is ahead now becomes the centre
  static Future<void> recenter() async {
    try {
      await _channel.invokeMethod<Object?>('VideoOutputManager.XrRecenter');
    } on MissingPluginException {
      return;
    }
  }

  static Future<XrProbeState> state() async {
    try {
      return XrProbeState.fromMap(await _channel.invokeMethod<Object?>('VideoOutputManager.XrState'));
    } on MissingPluginException {
      return const XrProbeState(compiled: false, running: false, phase: 'no plugin');
    }
  }
}
