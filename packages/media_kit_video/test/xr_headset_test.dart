// The calls of the headset view (lib/src/xr_headset.dart, IMMUCH360-NOTE.md patch 15) on the plugin's channel, with
// a fake plugin: the maps both ways, a build without the headset view (or without the plugin) answering notInBuild,
// Linux and macOS answering notWindows without a call, and answers this version does not fully know parsed
// tolerantly.

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:media_kit_video/xr.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const channel = MethodChannel('com.alexmercerind/media_kit_video');
  final calls = <MethodCall>[];

  void answer(Object? Function(MethodCall call) reply) {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger.setMockMethodCallHandler(channel, (call) async {
      calls.add(call);
      return reply(call);
    });
  }

  setUp(() {
    calls.clear();
    XrHeadset.isWindows = () => true;
  });
  tearDown(
    () => TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger.setMockMethodCallHandler(channel, null),
  );

  const content = XrContent(
    handle: 42,
    mode: XrHeadsetMode.screen,
    stereo: XrStereo.leftRight,
    squeeze: XrEyeSqueeze.half,
    fps: 23.976,
    yaw: -30,
    label: 'flat film, 3D half side by side',
  );
  const contentMap = {
    'handle': 42,
    'mode': 'screen',
    'stereo': 'leftRight',
    'squeeze': 'half',
    'fps': 23.976,
    'yaw': -30.0,
    'label': 'flat film, 3D half side by side',
  };

  test('start sends the content, the log file, the refresh choice, the action names and the version', () async {
    answer((_) => {'ok': true});
    final result = await XrHeadset.start(
      const XrStartRequest(
        content: content,
        logPath: r'C:\Users\me\AppData\Roaming\headset\headset-20261115-101500.log',
        matchRefreshRate: false,
        actionNames: {'player': 'Lecteur vidéo', 'play_pause': 'Lecture ou pause'},
        appVersion: '3.4.3-48',
      ),
    );
    expect(result.ok, isTrue);
    expect(result.reason, XrStopReason.none);
    expect(calls.single.method, 'VideoOutputManager.XrStart');
    expect(calls.single.arguments, {
      'content': contentMap,
      'logPath': r'C:\Users\me\AppData\Roaming\headset\headset-20261115-101500.log',
      'matchRefreshRate': false,
      'actionNames': {'player': 'Lecteur vidéo', 'play_pause': 'Lecture ou pause'},
      'appVersion': '3.4.3-48',
    });
  });

  test('the defaults of a content: no player is handle 0, mono, full, idle', () {
    expect(XrContent.idle.toMap(), {
      'handle': 0,
      'mode': 'idle',
      'stereo': 'mono',
      'squeeze': 'full',
      'fps': 0.0,
      'yaw': 0.0,
      'label': '',
    });
    expect(
        const XrContent(mode: XrHeadsetMode.pattern, fps: 24), const XrContent(mode: XrHeadsetMode.pattern, fps: 24));
  });

  test('refusals keep their code and detail', () async {
    answer((_) => {'ok': false, 'reason': 'alreadyRunning', 'detail': 'a headset view already runs'});
    var result = await XrHeadset.start(const XrStartRequest(content: XrContent.idle, logPath: 'x.log'));
    expect(result.ok, isFalse);
    expect(result.reason, XrStopReason.alreadyRunning);
    expect(result.detail, 'a headset view already runs');
    answer((_) => {'ok': false, 'reason': 'runtimeNotAnswering'});
    result = await XrHeadset.start(const XrStartRequest(content: XrContent.idle, logPath: 'x.log'));
    expect(result.reason, XrStopReason.runtimeNotAnswering);
    answer((_) => {'ok': false, 'reason': 'someday'});
    result = await XrHeadset.start(const XrStartRequest(content: XrContent.idle, logPath: 'x.log'));
    expect(result.reason, XrStopReason.failed, reason: 'an unknown code is failed');
    answer((_) => {'ok': false});
    result = await XrHeadset.start(const XrStartRequest(content: XrContent.idle, logPath: 'x.log'));
    expect(result.reason, XrStopReason.failed, reason: 'a refusal without a code is failed, never none');
  });

  test('the commands send their maps', () async {
    answer((_) => {'ok': true});
    await XrHeadset.setContent(content);
    await XrHeadset.setContentRate(29.97);
    final rgba = Uint8List.fromList(List.filled(1024 * 160 * 4, 7));
    await XrHeadset.setStrip(rgba, 1024, 160, visible: true);
    await XrHeadset.setStrip(Uint8List(0), 0, 0, visible: false);
    await XrHeadset.recenter();
    await XrHeadset.recenter(lyingDown: true);
    expect(calls.map((call) => call.method), [
      'VideoOutputManager.XrSetContent',
      'VideoOutputManager.XrSetContentRate',
      'VideoOutputManager.XrSetStrip',
      'VideoOutputManager.XrSetStrip',
      'VideoOutputManager.XrRecenter',
      'VideoOutputManager.XrRecenter',
    ]);
    expect(calls[0].arguments, {'content': contentMap});
    expect(calls[1].arguments, {'fps': 29.97});
    final strip = calls[2].arguments as Map;
    expect((strip['rgba'] as Uint8List).length, 1024 * 160 * 4);
    expect([strip['width'], strip['height'], strip['visible']], [1024, 160, true]);
    expect((calls[3].arguments as Map)['visible'], isFalse);
    expect(calls[4].arguments, {'lyingDown': false});
    expect(calls[5].arguments, {'lyingDown': true});
  });

  test('poll sends the last sequence and parses the state and its events', () async {
    answer(
      (_) => {
        'compiled': true,
        'running': true,
        'phase': 'running',
        'sessionState': 'focused',
        'visible': true,
        'focused': true,
        'reason': 'none',
        'detail': '',
        'runtime': 'SteamVR/OpenXR 2.19.0',
        'system': 'SteamVR/OpenXR : oculus',
        'swapchainPath': 'direct (image R8G8B8A8_TYPELESS)',
        'mode': 'sphere',
        'leftProfile': '',
        'rightProfile': '/interaction_profiles/oculus/touch_controller',
        'controllers': true,
        'lyingDown': false,
        'refreshRate': 72.0,
        'refreshAsked': 72.0,
        'frames': 900,
        'framesShown': 898,
        'framesLate': 2,
        'memoryTight': true,
        'memoryUsedMb': 7500,
        'memoryBudgetMb': 8000,
        'statsAt': 10.0,
        'snapshotMs': 0.02,
        'stripUploads': 4,
        'stripShown': true,
        'instanceLost': false,
        'logPath': 'x.log',
        'lastSequence': 12,
        'dropped': 0,
        'events': [
          {'sequence': 11, 'kind': 'seek', 'seconds': -10.0, 'on': false, 'hand': 'left', 'profile': '', 'time': 3.5},
          {'sequence': 12, 'kind': 'lyingDown', 'seconds': 0.0, 'on': true, 'hand': 'right', 'profile': '', 'time': 4},
        ],
      },
    );
    final state = await XrHeadset.poll(afterSequence: 10);
    expect(calls.single.method, 'VideoOutputManager.XrPoll');
    expect(calls.single.arguments, {'after': 10});
    expect(state.compiled, isTrue);
    expect(state.running, isTrue);
    expect(state.focused, isTrue);
    expect(state.reason, XrStopReason.none);
    expect(state.runtime, 'SteamVR/OpenXR 2.19.0');
    expect(state.profile, '/interaction_profiles/oculus/touch_controller');
    expect(state.controllers, isTrue);
    expect(state.refreshRate, 72);
    expect((state.frames, state.framesShown, state.framesLate), (900, 898, 2));
    expect((state.memoryTight, state.memoryUsedMb, state.memoryBudgetMb), (true, 7500, 8000));
    expect(state.lastSequence, 12);
    expect(state.events, hasLength(2));
    expect(state.events[0].kind, XrEventKind.seek);
    expect(state.events[0].seconds, -10);
    expect(state.events[0].hand, XrHand.left);
    expect(state.events[1].kind, XrEventKind.lyingDown);
    expect(state.events[1].on, isTrue);
    expect(state.events[1].hand, XrHand.right);
    expect(state.events[1].time, 4);
  });

  test('every stop code maps to its reason', () async {
    for (final reason in XrStopReason.values) {
      answer((_) => {'compiled': true, 'running': false, 'phase': 'stopped', 'reason': reason.name});
      expect((await XrHeadset.poll()).reason, reason);
    }
  });

  test('tolerant parsing: unknown fields, unknown events, wrong types', () async {
    answer(
      (_) => {
        'compiled': true,
        'running': 'yes',
        'phase': 7,
        'frames': 12.0,
        'refreshRate': 90,
        'reason': 'aNewCode',
        'somethingNew': [1, 2, 3],
        'events': [
          {'sequence': 1, 'kind': 'teleport'},
          {'sequence': 2, 'kind': 'playPause', 'hand': 'right'},
          'not a map',
          {'kind': 'exit'},
          {'sequence': 3, 'kind': 'visibility', 'on': true},
        ],
      },
    );
    final state = await XrHeadset.poll();
    expect(state.running, isFalse);
    expect(state.phase, '');
    expect(state.frames, 12);
    expect(state.refreshRate, 90);
    expect(state.reason, XrStopReason.failed);
    expect(state.events.map((event) => event.kind), [XrEventKind.playPause, XrEventKind.visibility]);
    answer((_) => null);
    final empty = await XrHeadset.poll();
    expect(empty.compiled, isFalse);
    expect(empty.events, isEmpty);
    expect(empty.reason, XrStopReason.none);
  });

  test('runtime info: registered, the file name and the guess', () async {
    answer(
      (_) => {
        'compiled': true,
        'registered': true,
        'source': 'registry',
        'manifestName': 'steamxr_win64.json',
        'exists': true,
        'runtime': 'SteamVR',
      },
    );
    final info = await XrHeadset.runtimeInfo();
    expect(calls.single.method, 'VideoOutputManager.XrRuntimeInfo');
    expect(info.compiled, isTrue);
    expect(info.registered, isTrue);
    expect(info.manifestName, 'steamxr_win64.json');
    expect(info.runtime, 'SteamVR');
    expect(info.reason, XrStopReason.none);
    answer((_) => {'compiled': true, 'registered': false, 'source': 'none'});
    expect((await XrHeadset.runtimeInfo()).registered, isFalse);
  });

  test('a build without the headset view answers notInBuild', () async {
    answer(
      (call) => switch (call.method) {
        'VideoOutputManager.XrPoll' => {
            'compiled': false,
            'running': false,
            'phase': 'not in this build',
            'reason': 'notInBuild',
          },
        'VideoOutputManager.XrRuntimeInfo' => {'compiled': false, 'registered': false},
        _ => {'ok': false, 'reason': 'notInBuild', 'detail': 'this build has no headset view (MEDIA_KIT_360_XR off)'},
      },
    );
    expect((await XrHeadset.poll()).reason, XrStopReason.notInBuild);
    expect((await XrHeadset.runtimeInfo()).reason, XrStopReason.notInBuild);
    expect((await XrHeadset.runtimeInfo()).registered, isFalse);
    expect((await XrHeadset.start(const XrStartRequest(content: XrContent.idle, logPath: 'x.log'))).reason,
        XrStopReason.notInBuild);
  });

  test('no plugin: notInBuild, no exception', () async {
    expect((await XrHeadset.poll()).reason, XrStopReason.notInBuild);
    expect((await XrHeadset.runtimeInfo()).reason, XrStopReason.notInBuild);
    final result = await XrHeadset.start(const XrStartRequest(content: XrContent.idle, logPath: 'x.log'));
    expect(result.reason, XrStopReason.notInBuild);
    expect(await XrHeadset.stop(), isTrue);
    await XrHeadset.setContent(XrContent.idle);
    await XrHeadset.recenter();
  });

  test('a platform error: failed with its message', () async {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger.setMockMethodCallHandler(channel, (call) async {
      throw PlatformException(code: 'boom', message: 'the plugin broke');
    });
    final result = await XrHeadset.start(const XrStartRequest(content: XrContent.idle, logPath: 'x.log'));
    expect(result.reason, XrStopReason.failed);
    expect(result.detail, 'boom: the plugin broke');
    expect(await XrHeadset.stop(), isFalse);
  });

  test('Linux and macOS: notWindows, and no call made', () async {
    XrHeadset.isWindows = () => false;
    answer((_) => throw StateError('no call expected'));
    expect((await XrHeadset.poll()).reason, XrStopReason.notWindows);
    expect((await XrHeadset.runtimeInfo()).reason, XrStopReason.notWindows);
    expect((await XrHeadset.runtimeInfo()).registered, isFalse);
    final result = await XrHeadset.start(const XrStartRequest(content: XrContent.idle, logPath: 'x.log'));
    expect(result.ok, isFalse);
    expect(result.reason, XrStopReason.notWindows);
    expect(await XrHeadset.stop(), isTrue);
    await XrHeadset.setContent(XrContent.idle);
    await XrHeadset.setContentRate(24);
    await XrHeadset.setStrip(Uint8List(4), 1, 1, visible: true);
    await XrHeadset.recenter(lyingDown: true);
    expect(calls, isEmpty);
  });

  test('stop answers whether the view ended', () async {
    answer((call) => call.method == 'VideoOutputManager.XrStop' ? {'ok': false} : null);
    expect(await XrHeadset.stop(), isFalse);
    answer((call) => call.method == 'VideoOutputManager.XrStop' ? {'ok': true} : null);
    expect(await XrHeadset.stop(), isTrue);
  });

  test('codes: null is none, a known name its reason', () {
    expect(XrStopReason.fromCode('notWindows'), XrStopReason.notWindows);
    expect(XrStopReason.fromCode(null), XrStopReason.none);
  });
}
