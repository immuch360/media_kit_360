// The example of media_kit_360: opens a 360°, VR180 or 3D video (a file or a URL, also given as the first argument of
// the command line), draws the view through renderer C (package:media_kit_video/sphere.dart) and lets the user drag
// it, zoom it with the mouse wheel and change the projection. The renderer probe watches the first seconds of each
// video and takes the tier down when the GPU does not keep up.
//
// Renderer C exists on Windows. On Linux, or when the GPU refuses it, the example plays the frame flat and says why.
//
// Copyright (c) 2026 FreeKC. Use of this source code is governed by the MIT license of the LICENSE file.

import 'dart:async';
import 'dart:io';

import 'package:file_selector/file_selector.dart';
import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:media_kit/media_kit.dart';
import 'package:media_kit_video/media_kit_video.dart';
import 'package:media_kit_video/sphere.dart';

void main(List<String> arguments) {
  WidgetsFlutterBinding.ensureInitialized();
  MediaKit.ensureInitialized();
  runApp(ExampleApp(initialSource: arguments.isEmpty ? null : arguments.first));
}

class ExampleApp extends StatelessWidget {
  const ExampleApp({super.key, this.initialSource});

  /// A path or a URL to open at the start
  final String? initialSource;

  @override
  Widget build(BuildContext context) => MaterialApp(
    title: 'media_kit_360 example',
    theme: ThemeData(colorSchemeSeed: Colors.teal, brightness: Brightness.dark),
    home: SpherePlayerPage(initialSource: initialSource),
  );
}

/// The projections the example offers: what the frame holds
enum ExampleProjection {
  mono360('360°', PluginStereoLayout.mono, PluginCoverage.full),
  overUnder360('360° 3D, over under', PluginStereoLayout.topBottom, PluginCoverage.full),
  sideBySide360('360° 3D, side by side', PluginStereoLayout.leftRight, PluginCoverage.full),
  vr180('VR180, side by side', PluginStereoLayout.leftRight, PluginCoverage.half),
  mono180('180°', PluginStereoLayout.mono, PluginCoverage.half);

  const ExampleProjection(this.label, this.layout, this.coverage);

  final String label;
  final PluginStereoLayout layout;
  final PluginCoverage coverage;

  PluginProjection toPlugin() => PluginProjection.equirect(layout: layout, coverage: coverage);
}

class SpherePlayerPage extends StatefulWidget {
  const SpherePlayerPage({super.key, this.initialSource});

  final String? initialSource;

  @override
  State<SpherePlayerPage> createState() => _SpherePlayerPageState();
}

class _SpherePlayerPageState extends State<SpherePlayerPage> {
  static const _minFov = 15.0;
  static const _maxFov = 115.0;

  late final Player _player = Player();
  late final VideoController _controller = VideoController(
    _player,
    configuration: const VideoControllerConfiguration(hwdec: 'auto-safe'),
  );
  late final PluginRenderer _renderer = PluginRenderer.forController(_controller);
  late final TextEditingController _source = TextEditingController(text: widget.initialSource ?? '');
  final _subscriptions = <StreamSubscription<Object?>>[];

  RendererProbe? _probe;
  ExampleProjection _projection = ExampleProjection.mono360;
  PluginView _view = const PluginView();
  Size? _outputSize;
  bool _dragging = false;
  bool _flat = true;
  String _status = 'Open a 360° video: a file of this computer or a URL.';

  @override
  void initState() {
    super.initState();
    _subscriptions.add(_player.stream.error.listen((error) => setState(() => _status = 'mpv: $error')));
    _subscriptions.add(_player.stream.playing.listen((_) => setState(() {})));
    final initial = widget.initialSource;
    if (initial != null && initial.isNotEmpty) {
      WidgetsBinding.instance.addPostFrameCallback((_) => unawaited(_open(initial)));
    }
  }

  @override
  void dispose() {
    _probe?.stop();
    for (final subscription in _subscriptions) {
      unawaited(subscription.cancel());
    }
    _source.dispose();
    unawaited(_renderer.dispose().whenComplete(_player.dispose));
    super.dispose();
  }

  /// Renderer C first, then the file: the first frame is already the view
  Future<void> _open(String source) async {
    if (source.isEmpty) {
      return;
    }
    _probe?.stop();
    var flat = true;
    String status;
    if (!PluginRenderer.supported) {
      status = 'The 360° view is drawn on Windows only for now: the video plays flat.';
    } else {
      final attached = await _renderer.attach(
        projection: _projection.toPlugin(),
        outputSize: _outputSize ?? const Size(1280, 720),
        view: _view,
      );
      flat = !attached.ok;
      status =
          attached.ok
              ? 'Renderer C, tier ${attached.tier?.name}, on ${attached.glRenderer}'
              : 'Renderer C refused (${attached.reason}): the video plays flat.';
    }
    if (!mounted) {
      return;
    }
    setState(() {
      _flat = flat;
      _status = status;
    });
    await _player.open(Media(source));
    if (!flat) {
      _startProbe();
    }
  }

  /// The probe measures the first seconds of the playback and answers whether the tier keeps up
  void _startProbe() {
    final tier = _renderer.tier;
    if (tier == null) {
      return;
    }
    final probe =
        _probe = RendererProbe(
          stats: _renderer.stats,
          targetFramesPerSecond: _framesPerSecond,
          memoryMB: () => ProcessInfo.currentRss ~/ (1 << 20),
          measuring: () => _player.state.playing && !_player.state.buffering,
          moving: () => _dragging,
        );
    probe.start(lowestTier: tier.lower == null, onResult: (verdict, sample) => unawaited(_onProbe(verdict, sample)));
  }

  Future<double?> _framesPerSecond() async {
    final native = _player.platform;
    if (native is! NativePlayer) {
      return null;
    }
    return double.tryParse(await native.getProperty('container-fps'));
  }

  Future<void> _onProbe(ProbeVerdict verdict, ProbeSample sample) async {
    if (!mounted) {
      return;
    }
    final tier = _renderer.tier;
    final measured =
        '${sample.framesPerSecond.toStringAsFixed(1)} of '
        '${sample.targetFramesPerSecond?.toStringAsFixed(0) ?? '?'} frames a second';
    switch (verdict) {
      case ProbeVerdict.keep:
        setState(() => _status = 'Renderer C keeps tier ${tier?.name}: $measured, on ${_renderer.glRenderer}');
      case ProbeVerdict.stepDown:
        final lower = tier?.lower;
        if (lower != null && await _renderer.setTier(lower) && mounted) {
          setState(() => _status = 'Tier ${tier?.name} did not keep up ($measured): tier ${lower.name} now');
          _startProbe();
        }
      case ProbeVerdict.fail:
      case ProbeVerdict.refused:
        await _renderer.detach();
        if (mounted) {
          setState(() {
            _flat = true;
            _status = 'This GPU cannot draw the 360° view of this video ($measured): it plays flat.';
          });
        }
    }
  }

  Future<void> _browse() async {
    final file = await openFile(
      acceptedTypeGroups: const [
        XTypeGroup(label: 'Videos', extensions: ['mp4', 'mov', 'mkv', 'webm', 'avi', 'm4v', 'insv']),
      ],
    );
    if (file != null) {
      _source.text = file.path;
      await _open(file.path);
    }
  }

  void _setView(PluginView view, {required bool moving}) {
    _view = PluginView(
      yaw: ((view.yaw + 180) % 360 + 360) % 360 - 180,
      pitch: view.pitch.clamp(-90.0, 90.0),
      fov: view.fov.clamp(_minFov, _maxFov),
    );
    _renderer.setView(_view, moving: moving);
  }

  // The picture follows the pointer: dragging to the right turns the view to the left
  void _onPanUpdate(DragUpdateDetails details, double height) {
    if (height <= 0) {
      return;
    }
    final degreesPerPixel = _view.fov / height;
    _setView(
      PluginView(
        yaw: _view.yaw - details.delta.dx * degreesPerPixel,
        pitch: _view.pitch + details.delta.dy * degreesPerPixel,
        fov: _view.fov,
      ),
      moving: true,
    );
  }

  void _onWheel(PointerSignalEvent event) {
    if (event is PointerScrollEvent) {
      final factor = event.scrollDelta.dy > 0 ? 1.1 : 1 / 1.1;
      _setView(PluginView(yaw: _view.yaw, pitch: _view.pitch, fov: _view.fov * factor), moving: true);
    }
  }

  void _onLayout(Size logical, double devicePixelRatio) {
    final size = Size(
      (logical.width * devicePixelRatio).roundToDouble(),
      (logical.height * devicePixelRatio).roundToDouble(),
    );
    if (size.isEmpty || size == _outputSize) {
      return;
    }
    _outputSize = size;
    // The renderer merges the sizes of a window dragged by its border
    _renderer.setOutputSize(size);
  }

  Future<void> _setProjection(ExampleProjection projection) async {
    setState(() => _projection = projection);
    if (_renderer.attached) {
      await _renderer.setProjection(projection.toPlugin());
    }
  }

  @override
  Widget build(BuildContext context) => Scaffold(
    body: Column(
      children: [
        _toolbar(context),
        Expanded(child: _video(context)),
        Padding(
          padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 6),
          child: Text(_status, maxLines: 2, overflow: TextOverflow.ellipsis),
        ),
      ],
    ),
  );

  Widget _toolbar(BuildContext context) => Padding(
    padding: const EdgeInsets.all(8),
    child: Row(
      children: [
        Expanded(
          child: TextField(
            controller: _source,
            decoration: const InputDecoration(hintText: 'Path or URL of a 360° video', isDense: true),
            onSubmitted: (value) => unawaited(_open(value.trim())),
          ),
        ),
        const SizedBox(width: 8),
        IconButton(tooltip: 'Browse', icon: const Icon(Icons.folder_open), onPressed: () => unawaited(_browse())),
        IconButton(
          tooltip: 'Open',
          icon: const Icon(Icons.play_circle),
          onPressed: () => unawaited(_open(_source.text.trim())),
        ),
        IconButton(
          tooltip: _player.state.playing ? 'Pause' : 'Play',
          icon: Icon(_player.state.playing ? Icons.pause : Icons.play_arrow),
          onPressed: () => unawaited(_player.playOrPause()),
        ),
        IconButton(
          tooltip: 'Look ahead',
          icon: const Icon(Icons.center_focus_strong),
          onPressed: () => _setView(const PluginView(), moving: false),
        ),
        const SizedBox(width: 8),
        DropdownButton<ExampleProjection>(
          value: _projection,
          items: [
            for (final projection in ExampleProjection.values)
              DropdownMenuItem(value: projection, child: Text(projection.label)),
          ],
          onChanged: (projection) {
            if (projection != null) {
              unawaited(_setProjection(projection));
            }
          },
        ),
      ],
    ),
  );

  Widget _video(BuildContext context) => LayoutBuilder(
    builder: (context, constraints) {
      _onLayout(constraints.biggest, MediaQuery.devicePixelRatioOf(context));
      return Listener(
        onPointerSignal: _onWheel,
        child: GestureDetector(
          onPanStart: (_) => _dragging = true,
          onPanUpdate: (details) => _onPanUpdate(details, constraints.maxHeight),
          onPanEnd: (_) {
            _dragging = false;
            _setView(_view, moving: false);
          },
          child: Video(
            controller: _controller,
            controls: NoVideoControls,
            // Renderer C gives a texture of the view's own shape; a flat frame is letterboxed
            fit: _flat ? BoxFit.contain : BoxFit.fill,
          ),
        ),
      );
    },
  );
}
