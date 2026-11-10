// The renderer probe of the 360 player (lib/src/sphere/renderer_probe.dart, DP1 of 9 October 2026): the verdicts of the
// probe on the measures of the first build, a shortfall that is the decoder's, a renderer that draws nothing, and the
// memory watched over the first seconds and the drags only.
//
// Taken from the Immuch360 app (the probe's groups of mobile/test/desktop/video/sphere_renderer_test.dart of
// github.com/freeKC/Immuch360); the tests of the app's renderer choice and of its settings file stay there.

import 'package:fake_async/fake_async.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:media_kit_video/sphere.dart';

ProjectionStats _frames(int frames, {int failed = 0, double? drawMs, String? error}) => ProjectionStats(
  enabled: true,
  frames: frames,
  redraws: 0,
  failed: failed,
  frameMs: drawMs == null ? const [] : List.filled(frames, drawMs),
  redrawMs: const [],
  lockedMs: const [],
  frameWidth: 2880,
  frameHeight: 1440,
  error: error,
);

void main() {
  group('the verdicts of the probe', () {
    ProbeSample sample(double fps, {double target = 30, int? memory}) =>
        ProbeSample(seconds: 5, frames: (fps * 5).round(), targetFramesPerSecond: target, memoryGrowthMB: memory);

    test('the skeleton measures: the RTX at 30 and the Intel at 27.3 of 30 keep their tier', () {
      expect(judgeProbe(sample(30), lowestTier: false), ProbeVerdict.keep);
      expect(judgeProbe(sample(27.4), lowestTier: true), ProbeVerdict.keep);
    });

    test('below 0.9 of the rate a tier down; at the lowest tier flat below 0.6 only', () {
      expect(judgeProbe(sample(22.2), lowestTier: false), ProbeVerdict.stepDown);
      expect(judgeProbe(sample(22.2), lowestTier: true), ProbeVerdict.keep);
      expect(judgeProbe(sample(15), lowestTier: true), ProbeVerdict.fail);
    });

    test('a 60 fps video is measured against 60, an unknown rate keeps the tier', () {
      expect(judgeProbe(sample(50, target: 60), lowestTier: false), ProbeVerdict.stepDown);
      expect(
        judgeProbe(const ProbeSample(seconds: 5, frames: 10), lowestTier: false),
        ProbeVerdict.keep,
        reason: 'no rate: nothing to compare with',
      );
    });

    test('kept at the lowest tier below the rate: the user hears that a dedicated GPU helps', () {
      // The Intel UHD at 2880 on a 5.7K H.264 video decoded in software (V-360, 2026-10-10): 20.6 of 30
      expect(judgeProbe(sample(20.6), lowestTier: true), ProbeVerdict.keep);
      expect(slowAtLowestTier(sample(20.6), lowestTier: true), isTrue);
      expect(slowAtLowestTier(sample(29.8), lowestTier: true), isFalse);
      expect(slowAtLowestTier(sample(20.6), lowestTier: false), isFalse, reason: 'a tier down first');
    });

    test('memory that grows by more than 512 MiB takes a tier down, whatever the frames', () {
      expect(judgeProbe(sample(30, memory: 600), lowestTier: false), ProbeVerdict.stepDown);
      expect(judgeProbe(sample(30, memory: 600), lowestTier: true), ProbeVerdict.fail);
      expect(judgeProbe(sample(30, memory: 400), lowestTier: false), ProbeVerdict.keep);
    });

    test('two players: the counts of the plugin added over the window, the delay and the speed the latest', () {
      ProjectionStats pair(int paired, int alone, int partner, double offset, double speed) => ProjectionStats(
        enabled: true,
        frames: paired + alone,
        redraws: 0,
        failed: 0,
        frameMs: const [],
        redrawMs: const [],
        lockedMs: const [],
        frameWidth: 3840,
        frameHeight: 3840,
        pairedFrames: paired,
        aloneFrames: alone,
        partnerFrames: partner,
        partnerOffsetMs: offset,
        partnerSpeed: speed,
      );
      expect(PairCounts.of(null), isNull);
      expect(PairCounts.of(_frames(25)), isNull, reason: 'one player: nothing to tell');
      final window = PairCounts.of(pair(24, 1, 25, 6, 1.02))! + PairCounts.of(pair(25, 0, 25, 2.5, 1.005));
      expect((window.paired, window.alone, window.partnerFrames), (49, 1, 50));
      expect(
        '$window',
        'two players: 49 paired, 1 alone, 0 caught up, 50 frames of the second, its delay 2.5 ms, its speed 1.0050',
      );
      expect(PairCounts.of(pair(0, 25, 0, 0, 1))!.partnerFrames, 0, reason: 'a second player that draws nothing');
    });

    test('draws that all fail: renderer C is refused, whatever the tier', () {
      const nothing = ProbeSample(seconds: 3, frames: 0, failed: 90, error: 'intermediate FBO incomplete (36054)');
      expect(judgeProbe(nothing, lowestTier: false), ProbeVerdict.refused);
      expect(judgeProbe(nothing, lowestTier: true), ProbeVerdict.refused);
    });

    test(
      'a shortfall is the decoder\'s when the processor decodes, or when the plugin draws in a fraction of a frame',
      () {
        ProbeSample timed(double fps, double drawMs) =>
            ProbeSample(seconds: 5, frames: (fps * 5).round(), targetFramesPerSecond: 30, drawMs: drawMs);
        // The Intel UHD on a 5.7K H.264 video the processor decodes: 20.6 of 30, draws of 40 to 52 ms (V-360)
        expect(decodeLimited(timed(20.6, 46), hwdec: 'no'), isTrue);
        // 8K HEVC copied back at 11 fps while the plugin draws each frame in 8 ms: the frames come late
        expect(decodeLimited(timed(11, 8), hwdec: 'd3d11va-copy'), isTrue);
        // The Intel at 4096 on a 5.7K video it decodes: 15.5 fps with draws of 61 ms, the drawing is the limit (V-C)
        expect(decodeLimited(timed(15.5, 61), hwdec: 'd3d11va'), isFalse);
        expect(decodeLimited(timed(29.8, 5), hwdec: 'no'), isFalse, reason: 'no shortfall');
        expect(decodeLimited(timed(20, 8)), isTrue, reason: 'the decoder not known: the draw time tells');
        expect(decodeLimited(const ProbeSample(seconds: 5, frames: 50, targetFramesPerSecond: 30)), isFalse);
      },
    );
  });

  group('the probe on a player', () {
    test('measures from the interval after the first frame, leaves paused time out, judges after 5 s', () {
      fakeAsync((async) {
        var frames = 0;
        var playing = true;
        const memory = 1000;
        final results = <(ProbeVerdict, ProbeSample)>[];
        final probe = RendererProbe(
          stats: () async => _frames(frames),
          targetFramesPerSecond: () async => 29.97,
          memoryMB: () => memory,
          measuring: () => playing,
        );
        probe.start(lowestTier: false, onResult: (verdict, sample) => results.add((verdict, sample)));

        // Opening: nothing drawn yet
        async.elapse(const Duration(seconds: 2));
        // The first frame, then 22 a second (the Intel at 4096 on a 5.7K video)
        frames = 4;
        async.elapse(const Duration(seconds: 1));
        frames = 22;
        async.elapse(const Duration(seconds: 2));
        // Paused for 3 s: not measured
        playing = false;
        async.elapse(const Duration(seconds: 3));
        expect(results, isEmpty);
        playing = true;
        async.elapse(const Duration(seconds: 3));
        expect(results, hasLength(1));
        final (verdict, sample) = results.single;
        expect(verdict, ProbeVerdict.stepDown);
        expect(sample.seconds, 5);
        expect(sample.framesPerSecond, 22);
        expect(sample.targetFramesPerSecond, closeTo(29.97, 1e-9));
        expect(probe.running, isFalse, reason: 'the page starts it again at the next tier');
      });
    });

    test('the memory the open takes before the first frame (decoder surfaces, mpv textures) is no growth', () {
      fakeAsync((async) {
        var frames = 0;
        var memory = 150;
        final results = <ProbeVerdict>[];
        final probe = RendererProbe(
          stats: () async => _frames(frames),
          targetFramesPerSecond: () async => 30,
          memoryMB: () => memory,
          measuring: () => true,
        );
        probe.start(lowestTier: false, onResult: (verdict, _) => results.add(verdict));
        async.elapse(const Duration(seconds: 1));
        // An 8K video opens on the RTX 4060: the process goes from 150 MB to 950 MB before its first frame
        memory = 950;
        frames = 30;
        async.elapse(const Duration(seconds: 7));
        expect(results, [ProbeVerdict.keep]);
      });
    });

    test('the median draw time is in the verdict', () {
      fakeAsync((async) {
        final results = <ProbeSample>[];
        final probe = RendererProbe(
          stats: () async => _frames(20, drawMs: 9),
          targetFramesPerSecond: () async => 30,
          memoryMB: () => 1000,
          measuring: () => true,
        );
        probe.start(lowestTier: true, onResult: (_, sample) => results.add(sample));
        async.elapse(const Duration(seconds: 7));
        expect(results.single.drawMs, 9);
        expect(decodeLimited(results.single), isTrue);
      });
    });

    test('a kept tier is reported once, then only memory that grows while the view moves', () {
      fakeAsync((async) {
        var memory = 1000;
        var moving = false;
        final results = <ProbeVerdict>[];
        final probe = RendererProbe(
          stats: () async => _frames(30),
          targetFramesPerSecond: () async => 30,
          memoryMB: () => memory,
          measuring: () => true,
          moving: () => moving,
        );
        probe.start(lowestTier: false, onResult: (verdict, _) => results.add(verdict));
        async.elapse(const Duration(seconds: 7));
        expect(results, [ProbeVerdict.keep]);
        // The demuxer caches of a streamed pair fill over a minute, the rest of the app syncs: not the renderer's
        for (var i = 0; i < 60; i++) {
          memory += 15;
          async.elapse(const Duration(seconds: 1));
        }
        expect(results, [ProbeVerdict.keep]);
        // A drag that takes 600 MiB more in 3 s: what renderer A did per view change
        moving = true;
        async.elapse(const Duration(seconds: 1));
        memory += 300;
        async.elapse(const Duration(seconds: 1));
        expect(results, [ProbeVerdict.keep]);
        memory += 300;
        async.elapse(const Duration(seconds: 1));
        expect(results, [ProbeVerdict.keep, ProbeVerdict.stepDown]);
        expect(probe.running, isFalse);
      });
    });

    test('a drag measures from its own start', () {
      fakeAsync((async) {
        var memory = 1000;
        var moving = false;
        final results = <ProbeVerdict>[];
        final probe = RendererProbe(
          stats: () async => _frames(30),
          targetFramesPerSecond: () async => 30,
          memoryMB: () => memory,
          measuring: () => true,
          moving: () => moving,
        );
        probe.start(lowestTier: false, onResult: (verdict, _) => results.add(verdict));
        async.elapse(const Duration(seconds: 7));
        memory += 450;
        moving = true;
        async.elapse(const Duration(seconds: 1));
        memory += 100;
        async.elapse(const Duration(seconds: 2));
        moving = false;
        async.elapse(const Duration(seconds: 1));
        memory += 450;
        moving = true;
        async.elapse(const Duration(seconds: 2));
        expect(results, [ProbeVerdict.keep], reason: 'two drags of 100 MiB and none, 1000 MiB over the whole time');
      });
    });

    test(
      'draws that fail while the video plays and none that succeeds: refused after 3 s, with the plugin\'s error',
      () {
        fakeAsync((async) {
          var playing = false;
          final results = <(ProbeVerdict, ProbeSample)>[];
          final probe = RendererProbe(
            stats: () async => _frames(0, failed: 30, error: 'shader: ERROR: 0:12: syntax error'),
            targetFramesPerSecond: () async => 30,
            memoryMB: () => 1000,
            measuring: () => playing,
          );
          probe.start(lowestTier: false, onResult: (verdict, sample) => results.add((verdict, sample)));
          // While the video opens, a failed draw is a view asked before the first frame
          async.elapse(const Duration(seconds: 10));
          expect(results, isEmpty);
          playing = true;
          async.elapse(const Duration(seconds: 2));
          expect(results, isEmpty);
          async.elapse(const Duration(seconds: 1));
          final (verdict, sample) = results.single;
          expect(verdict, ProbeVerdict.refused);
          expect((sample.frames, sample.failed, sample.error), (0, 90, 'shader: ERROR: 0:12: syntax error'));
          expect(probe.running, isFalse);
        });
      },
    );
  });
}
