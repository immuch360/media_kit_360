// The calls of the headset probe (lib/src/xr_probe.dart, IMMUCH360-NOTE.md patch 15) on the plugin's channel, with
// a fake plugin: the arguments the native side reads, the answers parsed, and a build without the probe (or without
// the plugin) answering plainly rather than throwing.

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

  setUp(calls.clear);
  tearDown(
    () => TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger.setMockMethodCallHandler(channel, null),
  );

  test('start sends the mode, the stereo layout, the rate, the yaw and the log file by name', () async {
    answer((_) => {'ok': true});
    final result = await XrProbe.start(
      handle: 42,
      mode: XrProbeMode.screen,
      stereo: XrProbeStereo.leftRight,
      fps: 23.976,
      yaw: -30,
      logPath: r'C:\Users\me\AppData\Roaming\xr-probe.log',
      label: 'flat film, 1920 x 1080',
    );
    expect(result.ok, isTrue);
    expect(calls.single.method, 'VideoOutputManager.XrStart');
    expect(calls.single.arguments, {
      'handle': 42,
      'mode': 'screen',
      'stereo': 'leftRight',
      'fps': 23.976,
      'yaw': -30.0,
      'logPath': r'C:\Users\me\AppData\Roaming\xr-probe.log',
      'label': 'flat film, 1920 x 1080',
    });
  });

  test('the pattern needs no player: handle 0', () async {
    answer((_) => {'ok': true});
    await XrProbe.start(mode: XrProbeMode.pattern, fps: 24, logPath: 'x.log');
    expect((calls.single.arguments as Map)['handle'], 0);
    expect((calls.single.arguments as Map)['mode'], 'pattern');
    expect((calls.single.arguments as Map)['stereo'], 'mono');
  });

  test('a refusal keeps its reason', () async {
    answer((_) => {'ok': false, 'reason': 'a headset view already runs'});
    final result = await XrProbe.start(mode: XrProbeMode.sphere, handle: 1, logPath: 'x.log');
    expect(result.ok, isFalse);
    expect(result.reason, 'a headset view already runs');
  });

  test('the state of a running probe', () async {
    answer(
      (_) => {
        'compiled': true,
        'running': true,
        'phase': 'running',
        'sessionState': 'focused',
        'reason': '',
        'runtime': 'SteamVR/OpenXR 2.19.0',
        'system': 'SteamVR/OpenXR : oculus',
        'swapchainPath': 'direct (image R8G8B8A8_TYPELESS)',
        'frames': 900,
        'framesShown': 898,
        'instanceLost': false,
        'logPath': 'x.log',
      },
    );
    final state = await XrProbe.state();
    expect(state.compiled, isTrue);
    expect(state.running, isTrue);
    expect(state.sessionState, 'focused');
    expect(state.runtime, 'SteamVR/OpenXR 2.19.0');
    expect(state.frames, 900);
    expect(state.framesShown, 898);
    expect(state.swapchainPath, startsWith('direct'));
  });

  test('a build without the probe says so', () async {
    answer((call) => call.method == 'VideoOutputManager.XrState'
        ? {'compiled': false, 'running': false, 'phase': 'not in this build'}
        : {'ok': false, 'reason': 'this build has no headset view (MEDIA_KIT_360_XR off)'});
    expect((await XrProbe.state()).compiled, isFalse);
    expect((await XrProbe.start(mode: XrProbeMode.pattern, logPath: 'x.log')).ok, isFalse);
  });

  test('no plugin (a test, another platform): plain answers, no exception', () async {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger.setMockMethodCallHandler(channel, null);
    expect((await XrProbe.state()).compiled, isFalse);
    expect((await XrProbe.start(mode: XrProbeMode.pattern, logPath: 'x.log')).reason, 'no plugin');
    expect(await XrProbe.stop(), isTrue);
    await XrProbe.recenter();
  });

  test('stop and recenter', () async {
    answer((call) => call.method == 'VideoOutputManager.XrStop' ? {'ok': true} : null);
    expect(await XrProbe.stop(), isTrue);
    await XrProbe.recenter();
    expect(calls.map((call) => call.method), ['VideoOutputManager.XrStop', 'VideoOutputManager.XrRecenter']);
  });
}
