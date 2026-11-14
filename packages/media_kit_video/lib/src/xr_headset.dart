/// Immuch360: the headset view (IMMUCH360-NOTE.md, patch 15), the calls of the plugin's OpenXR session on Windows.
///
/// The plugin shows a player's frame in a PC VR headset through whatever OpenXR runtime is active (SteamVR, Meta
/// Horizon Link, Virtual Desktop, Pico Connect): a 360 or 180 video around the head, each eye drawn by renderer C from
/// the eye's pose and field of view, a flat film on a screen, a test pattern, or a dark room while no player is
/// shown, with a status strip the app draws. The controllers give events ([XrEvent]) the app polls; recentering and
/// lying down act in the headset at once, play or pause, seek and leaving are for the app to apply to the window's
/// player. Only a Windows build with the MEDIA_KIT_360_XR option of CMake has it ([XrHeadsetState.compiled]): every
/// call answers [XrStopReason.notInBuild] without it, and [XrStopReason.notWindows] on Linux and macOS (no call
/// made). Experimental: not yet tried in a headset by the project.
library;

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';

/// What the headset shows
enum XrHeadsetMode {
  /// The player's 360 or 180 frame around the head, in renderer C's projection of the player
  sphere,

  /// The player's frame as a flat screen 2.5 m ahead, 2D or 3D
  screen,

  /// A test pattern, no player: a grey ramp ahead (the gamma), a red patch for the left eye only and a blue one for
  /// the right eye only, a bar moving at the frame rate given (the judder), green up, orange down
  pattern,

  /// No player: a dark grey room, and the strip
  idle,
}

/// How a frame holds its two eyes
enum XrStereo { mono, leftRight, topBottom }

/// A flat 3D film's eyes ([XrHeadsetMode.screen]): [full] when each eye has the picture's shape (32:9 side by side
/// for a 16:9 film), [half] when each is squeezed to half the frame and seen at the whole frame's shape. Mono
/// ignores it.
enum XrEyeSqueeze { full, half }

/// Why a view ended, or why it did not start
enum XrStopReason {
  /// Not stopped (the view runs, or none ran)
  none,

  /// [XrHeadset.stop], or the plugin going away
  stoppedByApp,

  /// The runtime ended the session without the app asking (its dashboard, the headset's menu)
  endedFromHeadset,

  /// No OpenXR runtime installed or active
  noRuntime,

  /// openxr_loader.dll is missing from the app's folder
  loaderMissing,

  /// No headset connected to the runtime within a minute
  noHeadset,

  /// The headset is on another graphics card than the one the app draws with
  otherGpu,

  /// The graphics card is under the runtime's minimum, or Windows has no graphics driver (the Basic Render Driver)
  gpuTooWeak,

  /// The runtime stopped or lost the session
  runtimeLost,

  /// A stop did not end the view in time: the runtime hangs; a restart of the runtime or of the app may be needed
  runtimeNotAnswering,

  /// A view already runs
  alreadyRunning,

  /// This build of the plugin has no headset view
  notInBuild,

  /// Not Windows
  notWindows,

  /// Anything else; the detail and the log tell
  failed;

  /// The reason of a plugin's code; [failed] for one this version does not know
  static XrStopReason fromCode(Object? code) {
    for (final reason in values) {
      if (reason.name == code) {
        return reason;
      }
    }
    return code == null || code == '' ? none : failed;
  }
}

/// What a controller did
enum XrEventKind {
  /// Play or pause the window's player
  playPause,

  /// Seek by [XrEvent.seconds] (signed)
  seek,

  /// Recentered in the headset (done already)
  recenter,

  /// Lying down turned on or off ([XrEvent.on]; done already)
  lyingDown,

  /// Leave the headset view (the app stops it)
  exit,

  /// The session became hidden for more than 2 s (the headset taken off, a dashboard) or visible again
  /// ([XrEvent.on])
  visibility,

  /// The interaction profile of a hand changed ([XrEvent.profile])
  profile,
}

/// The hand of a controller
enum XrHand { left, right }

/// What the headset shows, as the window gives it
@immutable
class XrContent {
  const XrContent({
    this.handle,
    required this.mode,
    this.stereo = XrStereo.mono,
    this.squeeze = XrEyeSqueeze.full,
    this.fps = 0,
    this.yaw = 0,
    this.label = '',
  });

  /// No player: the dark room with the strip
  static const idle = XrContent(mode: XrHeadsetMode.idle);

  /// The player (`Player.handle`, renderer C on for it) for [XrHeadsetMode.sphere] and [XrHeadsetMode.screen]
  final int? handle;
  final XrHeadsetMode mode;
  final XrStereo stereo;
  final XrEyeSqueeze squeeze;

  /// The content's frame rate, 0 when unknown: the refresh rate asked is a multiple of it where the runtime offers
  /// one; the pattern's bar moves at it
  final double fps;

  /// The window's yaw in degrees: what was ahead in the window is ahead in the headset
  final double yaw;

  /// The kind of video for the log ("360 video, leftRight", "flat film, 3D half side by side"), never a URL, a path
  /// or a title
  final String label;

  Map<String, Object?> toMap() => {
        'handle': handle ?? 0,
        'mode': mode.name,
        'stereo': stereo.name,
        'squeeze': squeeze.name,
        'fps': fps,
        'yaw': yaw,
        'label': label,
      };

  @override
  bool operator ==(Object other) =>
      other is XrContent &&
      other.handle == handle &&
      other.mode == mode &&
      other.stereo == stereo &&
      other.squeeze == squeeze &&
      other.fps == fps &&
      other.yaw == yaw &&
      other.label == label;

  @override
  int get hashCode => Object.hash(handle, mode, stereo, squeeze, fps, yaw, label);

  @override
  String toString() => 'XrContent(${mode.name}, ${stereo.name}, ${squeeze.name}, $fps fps, handle ${handle ?? 0})';
}

/// What [XrHeadset.start] needs
@immutable
class XrStartRequest {
  const XrStartRequest({
    required this.content,
    required this.logPath,
    this.matchRefreshRate = true,
    this.actionNames = const {},
    this.appVersion = '',
  });

  final XrContent content;

  /// The file the view writes its log to (one per view)
  final String logPath;

  /// Whether to ask the runtime for a refresh rate that is a multiple of the content's
  final bool matchRefreshRate;

  /// The names of the controllers' actions in the app's language, shown by the runtime (SteamVR's binding screen):
  /// by the action's name, `player` (the set), `play_pause`, `seek`, `seek_press`, `recenter`, `exit`, `haptic`;
  /// English for one not given
  final Map<String, String> actionNames;

  /// The app's version, for the log's first lines
  final String appVersion;

  Map<String, Object?> toMap() => {
        'content': content.toMap(),
        'logPath': logPath,
        'matchRefreshRate': matchRefreshRate,
        'actionNames': actionNames,
        'appVersion': appVersion,
      };
}

/// The answer of [XrHeadset.start]
@immutable
class XrStartResult {
  const XrStartResult({required this.ok, this.reason = XrStopReason.none, this.detail = ''});

  factory XrStartResult.fromMap(Object? value) {
    final map = value is Map ? value : const {};
    final ok = map['ok'] == true;
    return XrStartResult(
      ok: ok,
      reason: ok ? XrStopReason.none : _refusal(map['reason']),
      detail: _text(map['detail']),
    );
  }

  final bool ok;

  /// Why not: [XrStopReason.alreadyRunning], [XrStopReason.runtimeNotAnswering], [XrStopReason.notInBuild],
  /// [XrStopReason.notWindows], [XrStopReason.failed]
  final XrStopReason reason;

  /// English, for the app's log
  final String detail;

  @override
  String toString() => 'XrStartResult(${ok ? 'ok' : 'refused: ${reason.name}'})';
}

/// What a controller did, from [XrHeadsetState.events]
@immutable
class XrEvent {
  const XrEvent({
    required this.sequence,
    required this.kind,
    this.seconds = 0,
    this.on = false,
    this.hand = XrHand.left,
    this.profile = '',
    this.time = 0,
  });

  /// Null for an event this version does not know
  static XrEvent? fromMap(Object? value) {
    if (value is! Map) {
      return null;
    }
    XrEventKind? kind;
    for (final known in XrEventKind.values) {
      if (known.name == value['kind']) {
        kind = known;
      }
    }
    final sequence = _integer(value['sequence']);
    if (kind == null || sequence <= 0) {
      return null;
    }
    return XrEvent(
      sequence: sequence,
      kind: kind,
      seconds: _number(value['seconds']),
      on: value['on'] == true,
      hand: value['hand'] == 'right' ? XrHand.right : XrHand.left,
      profile: _text(value['profile']),
      time: _number(value['time']),
    );
  }

  /// Rising from 1, and on from one view to the next
  final int sequence;
  final XrEventKind kind;

  /// [XrEventKind.seek]: signed seconds (10 at first, 30 once the stick is held 2 s)
  final double seconds;

  /// [XrEventKind.lyingDown] and [XrEventKind.visibility]
  final bool on;
  final XrHand hand;

  /// [XrEventKind.profile]: the hand's interaction profile ("/interaction_profiles/oculus/touch_controller"), ""
  /// when none
  final String profile;

  /// Seconds from the view's start
  final double time;

  @override
  String toString() => 'XrEvent($sequence, ${kind.name}${kind == XrEventKind.seek ? ' $seconds s' : ''})';
}

/// Where the view is, from [XrHeadset.poll]
@immutable
class XrHeadsetState {
  const XrHeadsetState({
    required this.compiled,
    required this.running,
    required this.phase,
    this.sessionState = '',
    this.visible = false,
    this.focused = false,
    this.reason = XrStopReason.none,
    this.detail = '',
    this.runtime = '',
    this.system = '',
    this.swapchainPath = '',
    this.mode = '',
    this.leftProfile = '',
    this.rightProfile = '',
    this.controllers = false,
    this.lyingDown = false,
    this.refreshRate = 0,
    this.refreshAsked = 0,
    this.frames = 0,
    this.framesShown = 0,
    this.framesLate = 0,
    this.memoryTight = false,
    this.memoryUsedMb = 0,
    this.memoryBudgetMb = 0,
    this.statsAt = 0,
    this.snapshotMs = 0,
    this.stripUploads = 0,
    this.stripShown = false,
    this.instanceLost = false,
    this.logPath = '',
    this.lastSequence = 0,
    this.dropped = 0,
    this.events = const [],
  });

  /// What the Dart side answers on Linux and macOS
  static const notWindows = XrHeadsetState(
    compiled: false,
    running: false,
    phase: 'not on this system',
    reason: XrStopReason.notWindows,
  );

  /// What a build without the headset view (or without the plugin) answers
  static const notInBuild = XrHeadsetState(
    compiled: false,
    running: false,
    phase: 'not in this build',
    reason: XrStopReason.notInBuild,
  );

  factory XrHeadsetState.fromMap(Object? value) {
    final map = value is Map ? value : const {};
    final events = map['events'];
    return XrHeadsetState(
      compiled: map['compiled'] == true,
      running: map['running'] == true,
      phase: _text(map['phase']),
      sessionState: _text(map['sessionState']),
      visible: map['visible'] == true,
      focused: map['focused'] == true,
      reason: XrStopReason.fromCode(map['reason']),
      detail: _text(map['detail']),
      runtime: _text(map['runtime']),
      system: _text(map['system']),
      swapchainPath: _text(map['swapchainPath']),
      mode: _text(map['mode']),
      leftProfile: _text(map['leftProfile']),
      rightProfile: _text(map['rightProfile']),
      controllers: map['controllers'] == true,
      lyingDown: map['lyingDown'] == true,
      refreshRate: _number(map['refreshRate']),
      refreshAsked: _number(map['refreshAsked']),
      frames: _integer(map['frames']),
      framesShown: _integer(map['framesShown']),
      framesLate: _integer(map['framesLate']),
      memoryTight: map['memoryTight'] == true,
      memoryUsedMb: _integer(map['memoryUsedMb']),
      memoryBudgetMb: _integer(map['memoryBudgetMb']),
      statsAt: _number(map['statsAt']),
      snapshotMs: _number(map['snapshotMs']),
      stripUploads: _integer(map['stripUploads']),
      stripShown: map['stripShown'] == true,
      instanceLost: map['instanceLost'] == true,
      logPath: _text(map['logPath']),
      lastSequence: _integer(map['lastSequence']),
      dropped: _integer(map['dropped']),
      events: [if (events is List) ...events.map(XrEvent.fromMap).whereType<XrEvent>()],
    );
  }

  /// Whether this build of the plugin has the headset view
  final bool compiled;
  final bool running;

  /// "never started", "starting", "waiting for the headset", "running", "not answering", "stopped"; "not in this
  /// build" and "not on this system" from the Dart side
  final String phase;

  /// The OpenXR session state: "idle", "ready", "synchronized", "visible", "focused", "stopping"...
  final String sessionState;
  final bool visible;
  final bool focused;

  /// Why it stopped, once stopped ([XrStopReason.none] while it runs)
  final XrStopReason reason;

  /// English, for the app's log, never shown as it is
  final String detail;

  /// The runtime's name and version, and the headset's name, once known
  final String runtime;
  final String system;

  /// How the eyes reach the runtime's images: "direct (...)" or "copy (...)"
  final String swapchainPath;

  /// The content's mode now: "sphere", "screen", "pattern" or "idle"
  final String mode;

  /// The interaction profile of each hand, "" while none
  final String leftProfile;
  final String rightProfile;

  /// Whether the runtime accepted a controller profile (else the window drives alone)
  final bool controllers;
  final bool lyingDown;

  /// The display's refresh rate and the one asked, 0 when the runtime tells none
  final double refreshRate;
  final double refreshAsked;
  final int frames;
  final int framesShown;
  final int framesLate;

  /// GPU memory above 90 % of its budget at the last statistics
  final bool memoryTight;
  final int memoryUsedMb;
  final int memoryBudgetMb;

  /// Seconds from the view's start to its last statistics (every 5 s), and the time the view took for a frame of the
  /// player then
  final double statsAt;
  final double snapshotMs;
  final int stripUploads;
  final bool stripShown;
  final bool instanceLost;
  final String logPath;

  /// The last event's sequence: the next poll's `afterSequence`
  final int lastSequence;

  /// Events dropped because the queue (64) was full
  final int dropped;

  /// The events after the sequence given to [XrHeadset.poll], oldest first
  final List<XrEvent> events;

  /// A hand's profile, the right one first
  String get profile => rightProfile.isNotEmpty ? rightProfile : leftProfile;

  @override
  String toString() =>
      'XrHeadsetState($phase, session $sessionState, $frames frames, $framesShown shown, ${events.length} events'
      '${reason == XrStopReason.none ? '' : ', ${reason.name}'})';
}

/// The active OpenXR runtime, read from the registry without loading anything, from [XrHeadset.runtimeInfo]
@immutable
class XrRuntimeInfo {
  const XrRuntimeInfo({
    required this.compiled,
    required this.registered,
    this.source = 'none',
    this.manifestName = '',
    this.exists = false,
    this.runtime = '',
    this.reason = XrStopReason.none,
  });

  static const notWindows = XrRuntimeInfo(compiled: false, registered: false, reason: XrStopReason.notWindows);
  static const notInBuild = XrRuntimeInfo(compiled: false, registered: false, reason: XrStopReason.notInBuild);

  factory XrRuntimeInfo.fromMap(Object? value) {
    final map = value is Map ? value : const {};
    final compiled = map['compiled'] == true;
    return XrRuntimeInfo(
      compiled: compiled,
      registered: map['registered'] == true,
      source: map['source'] is String ? map['source'] as String : 'none',
      manifestName: _text(map['manifestName']),
      exists: map['exists'] == true,
      runtime: _text(map['runtime']),
      reason: compiled ? XrStopReason.none : XrStopReason.notInBuild,
    );
  }

  final bool compiled;

  /// A runtime is registered and its manifest exists
  final bool registered;

  /// "environment" (XR_RUNTIME_JSON), "registry" or "none"
  final String source;

  /// The manifest's file name only ("steamxr_win64.json", "oculus_openxr_64.json")
  final String manifestName;
  final bool exists;

  /// The runtime guessed from the manifest's path ("SteamVR", "Meta Horizon Link", "Virtual Desktop", "Pico
  /// Connect"...), "" when unknown
  final String runtime;

  /// [XrStopReason.notWindows] or [XrStopReason.notInBuild] when the answer did not come from a Windows build with
  /// the headset view
  final XrStopReason reason;

  @override
  String toString() => 'XrRuntimeInfo(${registered ? '$runtime, $manifestName' : 'none'}, $source)';
}

/// The calls of the headset view on the channel of media_kit_video. One view at a time; it belongs to the app, not
/// to a page: [setContent] moves it to the next video with no new session.
abstract final class XrHeadset {
  static const _channel = MethodChannel('com.alexmercerind/media_kit_video');

  /// Whether this is Windows (the tests set it)
  @visibleForTesting
  static bool Function() isWindows = () => !kIsWeb && defaultTargetPlatform == TargetPlatform.windows;

  static Future<Object?> _call(String method, [Map<String, Object?>? arguments]) =>
      _channel.invokeMethod<Object?>('VideoOutputManager.$method', arguments);

  /// The active runtime, without loading any DLL: read when a player page opens, to show the button only where it
  /// can be of use
  static Future<XrRuntimeInfo> runtimeInfo() async {
    if (!isWindows()) {
      return XrRuntimeInfo.notWindows;
    }
    try {
      return XrRuntimeInfo.fromMap(await _call('XrRuntimeInfo'));
    } on MissingPluginException {
      return XrRuntimeInfo.notInBuild;
    } on PlatformException {
      return XrRuntimeInfo.notInBuild;
    }
  }

  /// Starts the headset view. Returns at once: the view opens the runtime, waits for the headset and runs on a thread
  /// of the plugin; [poll] tells how it goes.
  static Future<XrStartResult> start(XrStartRequest request) async {
    if (!isWindows()) {
      return const XrStartResult(ok: false, reason: XrStopReason.notWindows);
    }
    try {
      return XrStartResult.fromMap(await _call('XrStart', request.toMap()));
    } on MissingPluginException {
      return const XrStartResult(ok: false, reason: XrStopReason.notInBuild, detail: 'no plugin');
    } on PlatformException catch (error) {
      return XrStartResult(ok: false, reason: XrStopReason.failed, detail: '${error.code}: ${error.message}');
    }
  }

  /// The next video, or another mode, with no new session (taken at the headset's next frame)
  static Future<void> setContent(XrContent content) => _command('XrSetContent', {'content': content.toMap()});

  /// The content's frame rate once the player knows it (the refresh rate is chosen again)
  static Future<void> setContentRate(double fps) => _command('XrSetContentRate', {'fps': fps});

  /// The strip's image: RGBA bytes, [width] x [height] (at most 1024 x 160), the first row on top; uploaded once per
  /// new image. [visible] alone, with empty bytes, shows or hides the last image.
  static Future<void> setStrip(Uint8List rgba, int width, int height, {required bool visible}) =>
      _command('XrSetStrip', {'rgba': rgba, 'width': width, 'height': height, 'visible': visible});

  /// What is ahead now becomes the centre: the head's yaw, and its pitch too when [lyingDown]
  static Future<void> recenter({bool lyingDown = false}) => _command('XrRecenter', {'lyingDown': lyingDown});

  /// The view's state and the events after [afterSequence] (call it about 10 times a second while a view runs and
  /// pass the last [XrHeadsetState.lastSequence])
  static Future<XrHeadsetState> poll({int afterSequence = 0}) async {
    if (!isWindows()) {
      return XrHeadsetState.notWindows;
    }
    try {
      return XrHeadsetState.fromMap(await _call('XrPoll', {'after': afterSequence}));
    } on MissingPluginException {
      return XrHeadsetState.notInBuild;
    } on PlatformException {
      return XrHeadsetState.notInBuild;
    }
  }

  /// Ends the headset view; false when the runtime did not end it within a few seconds ([poll] then says
  /// [XrStopReason.runtimeNotAnswering]). Answered off the platform thread.
  static Future<bool> stop() async {
    if (!isWindows()) {
      return true;
    }
    try {
      final answer = await _call('XrStop');
      return answer is Map && answer['ok'] == true;
    } on MissingPluginException {
      return true;
    } on PlatformException {
      return false;
    }
  }

  static Future<void> _command(String method, Map<String, Object?> arguments) async {
    if (!isWindows()) {
      return;
    }
    try {
      await _call(method, arguments);
    } on MissingPluginException {
      return;
    } on PlatformException {
      return;
    }
  }
}

XrStopReason _refusal(Object? code) {
  final reason = XrStopReason.fromCode(code);
  return reason == XrStopReason.none ? XrStopReason.failed : reason;
}

String _text(Object? value) => value is String ? value : '';

double _number(Object? value) => value is num ? value.toDouble() : 0;

int _integer(Object? value) => value is int ? value : (value is num ? value.toInt() : 0);
