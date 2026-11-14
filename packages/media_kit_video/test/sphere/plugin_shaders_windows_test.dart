// The passes of renderer C (windows/projection_shaders.h of this package, IMMUCH360-NOTE.md patch 6) in
// the ANGLE of the Windows build, without a window: the sources are read from the header and put together as
// ProjectionRenderer does, all three programs must compile and link in OpenGL ES 3.0, and the equirectangular pass
// draws a synthetic frame (upper half red, lower half blue, the quarter ahead green) where the view looks: up is up,
// the first row of the output is the top of the view, the yaw turns to the right, a VR180 crop is black behind. The
// stabilisation of the stitches (IMMUCH360-NOTE.md patch 14): the output direction v samples the lens at K^T v, K the
// correction of the frame, and nothing changes while uStabilize is 0.
// Windows only, after a Windows build of the example app (it holds libEGL.dll and libGLESv2.dll), from this package's
// folder:
//   (cd ../../example && flutter build windows --debug)
//   flutter test test/sphere/plugin_shaders_windows_test.dart
// MEDIA_KIT_360_BUILD_DIR may name another folder holding the two DLLs (the build folder of any app that depends on
// media_kit_libs_windows_video).
//
// Moved here from the Immuch360 app (mobile/test/desktop/video/plugin_shaders_windows_test.dart of
// github.com/freeKC/Immuch360).

import 'dart:ffi';
import 'dart:io';
import 'dart:math' as math;

import 'package:ffi/ffi.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:path/path.dart' as p;

/// The folder of the Windows build that holds ANGLE, null when there is none
String? _buildDir() {
  final given = Platform.environment['MEDIA_KIT_360_BUILD_DIR'];
  final candidates = [
    if (given != null && given.isNotEmpty) given,
    for (final mode in ['Debug', 'Profile', 'Release'])
      p.join('..', '..', 'example', 'build', 'windows', 'x64', 'runner', mode),
  ];
  for (final dir in candidates) {
    if (File(p.join(dir, 'libEGL.dll')).existsSync() && File(p.join(dir, 'libGLESv2.dll')).existsSync()) {
      return p.absolute(dir);
    }
  }
  return null;
}

/// The GLSL constants of projection_shaders.h by name
Map<String, String> _shaderSources() {
  final header = File(p.join('windows', 'projection_shaders.h')).readAsStringSync();
  return {
    for (final match in RegExp(
      r'constexpr const char\* (k\w+) = R"glsl\((.*?)\)glsl";',
      dotAll: true,
    ).allMatches(header))
      match.group(1)!: match.group(2)!,
  };
}

/// The fragment program of [kind] (0 equirectangular, 1 fisheye pair, 2 EAC pair, 3 the test pattern of the headset
/// probe), in ProjectionRenderer::Program's order
String _fragment(Map<String, String> sources, int kind) => [
  sources['kFragmentHeader']!,
  '#define PROJECTION_KIND $kind\n',
  sources['kFragmentCommon']!,
  if (kind == 0)
    sources['kFragmentEquirect']!
  else if (kind == 3)
    sources['kFragmentPattern']!
  else ...[
    sources['kFragmentStreams']!,
    kind == 1 ? sources['kFragmentFisheye']! : sources['kFragmentEac']!,
  ],
  sources['kFragmentMain']!,
].join();

// EGL and GLES constants (Khronos headers, ANGLE's eglext_angle.h)
const _eglPlatformAngle = 0x3202;
const _eglPlatformAngleType = 0x3203;
const _eglPlatformAngleTypeD3d11 = 0x3208;
const _eglNone = 0x3038;
const _eglRedSize = 0x3024;
const _eglGreenSize = 0x3023;
const _eglBlueSize = 0x3022;
const _eglAlphaSize = 0x3021;
const _eglRenderableType = 0x3040;
const _eglSurfaceType = 0x3033;
const _eglPbufferBit = 0x0001;
const _eglOpenglEs3Bit = 0x0040;
const _eglContextClientVersion = 0x3098;
const _eglWidth = 0x3057;
const _eglHeight = 0x3056;
const _glRenderer = 0x1F01;
const _glVertexShader = 0x8B31;
const _glFragmentShader = 0x8B30;
const _glCompileStatus = 0x8B81;
const _glLinkStatus = 0x8B82;
const _glTexture2d = 0x0DE1;
const _glTexture0 = 0x84C0;
const _glTexture1 = 0x84C1;
const _glRgba = 0x1908;
const _glRgba8 = 0x8058;
const _glUnsignedByte = 0x1401;
const _glTextureMinFilter = 0x2801;
const _glTextureMagFilter = 0x2800;
const _glTextureWrapS = 0x2802;
const _glTextureWrapT = 0x2803;
const _glLinear = 0x2601;
const _glRepeat = 0x2901;
const _glClampToEdge = 0x812F;
const _glTriangles = 0x0004;

typedef _V = Void Function();

class _Gles {
  _Gles(DynamicLibrary gl)
    : createShader = gl.lookupFunction<Uint32 Function(Uint32), int Function(int)>('glCreateShader'),
      shaderSource = gl
          .lookupFunction<
            Void Function(Uint32, Int32, Pointer<Pointer<Utf8>>, Pointer<Int32>),
            void Function(int, int, Pointer<Pointer<Utf8>>, Pointer<Int32>)
          >('glShaderSource'),
      compileShader = gl.lookupFunction<Void Function(Uint32), void Function(int)>('glCompileShader'),
      getShaderiv = gl
          .lookupFunction<Void Function(Uint32, Uint32, Pointer<Int32>), void Function(int, int, Pointer<Int32>)>(
            'glGetShaderiv',
          ),
      getShaderInfoLog = gl
          .lookupFunction<
            Void Function(Uint32, Int32, Pointer<Int32>, Pointer<Utf8>),
            void Function(int, int, Pointer<Int32>, Pointer<Utf8>)
          >('glGetShaderInfoLog'),
      createProgram = gl.lookupFunction<Uint32 Function(), int Function()>('glCreateProgram'),
      attachShader = gl.lookupFunction<Void Function(Uint32, Uint32), void Function(int, int)>('glAttachShader'),
      linkProgram = gl.lookupFunction<Void Function(Uint32), void Function(int)>('glLinkProgram'),
      getProgramiv = gl
          .lookupFunction<Void Function(Uint32, Uint32, Pointer<Int32>), void Function(int, int, Pointer<Int32>)>(
            'glGetProgramiv',
          ),
      getProgramInfoLog = gl
          .lookupFunction<
            Void Function(Uint32, Int32, Pointer<Int32>, Pointer<Utf8>),
            void Function(int, int, Pointer<Int32>, Pointer<Utf8>)
          >('glGetProgramInfoLog'),
      useProgram = gl.lookupFunction<Void Function(Uint32), void Function(int)>('glUseProgram'),
      getUniformLocation = gl.lookupFunction<Int32 Function(Uint32, Pointer<Utf8>), int Function(int, Pointer<Utf8>)>(
        'glGetUniformLocation',
      ),
      uniform1i = gl.lookupFunction<Void Function(Int32, Int32), void Function(int, int)>('glUniform1i'),
      uniform1f = gl.lookupFunction<Void Function(Int32, Float), void Function(int, double)>('glUniform1f'),
      uniform2f = gl.lookupFunction<Void Function(Int32, Float, Float), void Function(int, double, double)>(
        'glUniform2f',
      ),
      uniform3f = gl
          .lookupFunction<Void Function(Int32, Float, Float, Float), void Function(int, double, double, double)>(
            'glUniform3f',
          ),
      uniform4f = gl
          .lookupFunction<
            Void Function(Int32, Float, Float, Float, Float),
            void Function(int, double, double, double, double)
          >('glUniform4f'),
      uniformMatrix3fv = gl
          .lookupFunction<
            Void Function(Int32, Int32, Uint8, Pointer<Float>),
            void Function(int, int, int, Pointer<Float>)
          >('glUniformMatrix3fv'),
      genTextures = gl.lookupFunction<Void Function(Int32, Pointer<Uint32>), void Function(int, Pointer<Uint32>)>(
        'glGenTextures',
      ),
      bindTexture = gl.lookupFunction<Void Function(Uint32, Uint32), void Function(int, int)>('glBindTexture'),
      activeTexture = gl.lookupFunction<Void Function(Uint32), void Function(int)>('glActiveTexture'),
      texImage2D = gl
          .lookupFunction<
            Void Function(Uint32, Int32, Int32, Int32, Int32, Int32, Uint32, Uint32, Pointer<Uint8>),
            void Function(int, int, int, int, int, int, int, int, Pointer<Uint8>)
          >('glTexImage2D'),
      texParameteri = gl.lookupFunction<Void Function(Uint32, Uint32, Int32), void Function(int, int, int)>(
        'glTexParameteri',
      ),
      genVertexArrays = gl.lookupFunction<Void Function(Int32, Pointer<Uint32>), void Function(int, Pointer<Uint32>)>(
        'glGenVertexArrays',
      ),
      bindVertexArray = gl.lookupFunction<Void Function(Uint32), void Function(int)>('glBindVertexArray'),
      viewport = gl.lookupFunction<Void Function(Int32, Int32, Int32, Int32), void Function(int, int, int, int)>(
        'glViewport',
      ),
      drawArrays = gl.lookupFunction<Void Function(Uint32, Int32, Int32), void Function(int, int, int)>('glDrawArrays'),
      readPixels = gl
          .lookupFunction<
            Void Function(Int32, Int32, Int32, Int32, Uint32, Uint32, Pointer<Uint8>),
            void Function(int, int, int, int, int, int, Pointer<Uint8>)
          >('glReadPixels'),
      finish = gl.lookupFunction<_V, void Function()>('glFinish'),
      getError = gl.lookupFunction<Uint32 Function(), int Function()>('glGetError'),
      getString = gl.lookupFunction<Pointer<Utf8> Function(Uint32), Pointer<Utf8> Function(int)>('glGetString');

  final int Function(int) createShader;
  final void Function(int, int, Pointer<Pointer<Utf8>>, Pointer<Int32>) shaderSource;
  final void Function(int) compileShader;
  final void Function(int, int, Pointer<Int32>) getShaderiv;
  final void Function(int, int, Pointer<Int32>, Pointer<Utf8>) getShaderInfoLog;
  final int Function() createProgram;
  final void Function(int, int) attachShader;
  final void Function(int) linkProgram;
  final void Function(int, int, Pointer<Int32>) getProgramiv;
  final void Function(int, int, Pointer<Int32>, Pointer<Utf8>) getProgramInfoLog;
  final void Function(int) useProgram;
  final int Function(int, Pointer<Utf8>) getUniformLocation;
  final void Function(int, int) uniform1i;
  final void Function(int, double) uniform1f;
  final void Function(int, double, double) uniform2f;
  final void Function(int, double, double, double) uniform3f;
  final void Function(int, double, double, double, double) uniform4f;
  final void Function(int, int, int, Pointer<Float>) uniformMatrix3fv;
  final void Function(int, Pointer<Uint32>) genTextures;
  final void Function(int, int) bindTexture;
  final void Function(int) activeTexture;
  final void Function(int, int, int, int, int, int, int, int, Pointer<Uint8>) texImage2D;
  final void Function(int, int, int) texParameteri;
  final void Function(int, Pointer<Uint32>) genVertexArrays;
  final void Function(int) bindVertexArray;
  final void Function(int, int, int, int) viewport;
  final void Function(int, int, int) drawArrays;
  final void Function(int, int, int, int, int, int, Pointer<Uint8>) readPixels;
  final void Function() finish;
  final int Function() getError;
  final Pointer<Utf8> Function(int) getString;

  /// Compiles [source]; the shader, or a [StateError] with the driver's log
  int compile(int type, String source) {
    return using((arena) {
      final shader = createShader(type);
      final text = arena<Pointer<Utf8>>()..value = source.toNativeUtf8(allocator: arena);
      shaderSource(shader, 1, text, nullptr);
      compileShader(shader);
      final status = arena<Int32>();
      getShaderiv(shader, _glCompileStatus, status);
      if (status.value != 1) {
        final log = arena<Uint8>(4096).cast<Utf8>();
        getShaderInfoLog(shader, 4095, nullptr, log);
        throw StateError('compile: ${log.toDartString()}');
      }
      return shader;
    });
  }

  int link(int vertex, int fragment) {
    return using((arena) {
      final program = createProgram();
      attachShader(program, vertex);
      attachShader(program, fragment);
      linkProgram(program);
      final status = arena<Int32>();
      getProgramiv(program, _glLinkStatus, status);
      if (status.value != 1) {
        final log = arena<Uint8>(4096).cast<Utf8>();
        getProgramInfoLog(program, 4095, nullptr, log);
        throw StateError('link: ${log.toDartString()}');
      }
      return program;
    });
  }

  int location(int program, String name) =>
      using((arena) => getUniformLocation(program, name.toNativeUtf8(allocator: arena)));
}

Pointer<Int32> _attributes(Arena arena, List<int> values) {
  final list = arena<Int32>(values.length);
  for (var i = 0; i < values.length; i++) {
    list[i] = values[i];
  }
  return list;
}

void main() {
  final buildDir = Platform.isWindows ? _buildDir() : null;
  final skip = !Platform.isWindows
      ? 'the ANGLE of the Windows build: Windows only'
      : buildDir == null
      ? 'no Windows build with libEGL.dll (flutter build windows of the example first, or set MEDIA_KIT_360_BUILD_DIR)'
      : false;

  late _Gles gl;
  final arena = Arena();
  const size = 256;

  setUpAll(() {
    if (buildDir == null) {
      return;
    }
    // ANGLE's libEGL loads libGLESv2 by name: the build folder must be searched for it
    final setDllDirectory = DynamicLibrary.open(
      'kernel32.dll',
    ).lookupFunction<Int32 Function(Pointer<Utf16>), int Function(Pointer<Utf16>)>('SetDllDirectoryW');
    using((arena) => setDllDirectory(buildDir.toNativeUtf16(allocator: arena)));
    final egl = DynamicLibrary.open(p.join(buildDir, 'libEGL.dll'));
    gl = _Gles(DynamicLibrary.open(p.join(buildDir, 'libGLESv2.dll')));
    final getProcAddress = egl
        .lookupFunction<Pointer<Void> Function(Pointer<Utf8>), Pointer<Void> Function(Pointer<Utf8>)>(
          'eglGetProcAddress',
        );
    final initialize = egl
        .lookupFunction<
          Int32 Function(Pointer<Void>, Pointer<Int32>, Pointer<Int32>),
          int Function(Pointer<Void>, Pointer<Int32>, Pointer<Int32>)
        >('eglInitialize');
    final chooseConfig = egl
        .lookupFunction<
          Int32 Function(Pointer<Void>, Pointer<Int32>, Pointer<Pointer<Void>>, Int32, Pointer<Int32>),
          int Function(Pointer<Void>, Pointer<Int32>, Pointer<Pointer<Void>>, int, Pointer<Int32>)
        >('eglChooseConfig');
    final createContext = egl
        .lookupFunction<
          Pointer<Void> Function(Pointer<Void>, Pointer<Void>, Pointer<Void>, Pointer<Int32>),
          Pointer<Void> Function(Pointer<Void>, Pointer<Void>, Pointer<Void>, Pointer<Int32>)
        >('eglCreateContext');
    final createPbufferSurface = egl
        .lookupFunction<
          Pointer<Void> Function(Pointer<Void>, Pointer<Void>, Pointer<Int32>),
          Pointer<Void> Function(Pointer<Void>, Pointer<Void>, Pointer<Int32>)
        >('eglCreatePbufferSurface');
    final makeCurrent = egl
        .lookupFunction<
          Int32 Function(Pointer<Void>, Pointer<Void>, Pointer<Void>, Pointer<Void>),
          int Function(Pointer<Void>, Pointer<Void>, Pointer<Void>, Pointer<Void>)
        >('eglMakeCurrent');
    final getPlatformDisplay = getProcAddress('eglGetPlatformDisplayEXT'.toNativeUtf8(allocator: arena))
        .cast<NativeFunction<Pointer<Void> Function(Int32, Pointer<Void>, Pointer<Int32>)>>()
        .asFunction<Pointer<Void> Function(int, Pointer<Void>, Pointer<Int32>)>();
    final display = getPlatformDisplay(
      _eglPlatformAngle,
      nullptr,
      _attributes(arena, [_eglPlatformAngleType, _eglPlatformAngleTypeD3d11, _eglNone]),
    );
    expect(initialize(display, nullptr, nullptr), 1);
    // The attributes of the patched ANGLESurfaceManager, plus pbuffer since this test has no shared texture
    final config = arena<Pointer<Void>>();
    final count = arena<Int32>();
    expect(
      chooseConfig(
        display,
        _attributes(arena, [
          _eglRedSize, 8, _eglGreenSize, 8, _eglBlueSize, 8, _eglAlphaSize, 8, //
          _eglRenderableType, _eglOpenglEs3Bit, _eglSurfaceType, _eglPbufferBit, _eglNone,
        ]),
        config,
        1,
        count,
      ),
      1,
    );
    expect(count.value, greaterThan(0));
    final context = createContext(
      display,
      config.value,
      nullptr,
      _attributes(arena, [_eglContextClientVersion, 3, _eglNone]),
    );
    expect(context, isNot(nullptr));
    final surface = createPbufferSurface(
      display,
      config.value,
      _attributes(arena, [_eglWidth, size, _eglHeight, size, _eglNone]),
    );
    expect(surface, isNot(nullptr));
    expect(makeCurrent(display, surface, surface, context), 1);
    // ignore: avoid_print
    print('ANGLE: ${gl.getString(_glRenderer).toDartString()}');
  });

  tearDownAll(arena.releaseAll);

  test('the three passes and the test pattern compile and link in OpenGL ES 3.0', () {
    final sources = _shaderSources();
    expect(sources.keys, containsAll(['kVertexSource', 'kFragmentCommon', 'kFragmentEquirect', 'kFragmentMain']));
    final vertex = gl.compile(_glVertexShader, sources['kVertexSource']!);
    for (final kind in [0, 1, 2, 3]) {
      final program = gl.link(vertex, gl.compile(_glFragmentShader, _fragment(sources, kind)));
      expect(program, greaterThan(0), reason: 'projection $kind');
    }
  }, skip: skip);

  test('the equirectangular pass looks where the view looks', () {
    final sources = _shaderSources();
    final program = gl.link(
      gl.compile(_glVertexShader, sources['kVertexSource']!),
      gl.compile(_glFragmentShader, _fragment(sources, 0)),
    );
    // The frame as mpv draws it into the intermediate texture: its first row is the top of the picture. Upper half
    // red, lower half blue, the quarter of the longitudes ahead (u 0.375 to 0.625) green.
    const frameWidth = 1024;
    const frameHeight = 512;
    final pixels = arena<Uint8>(frameWidth * frameHeight * 4);
    for (var y = 0; y < frameHeight; y++) {
      for (var x = 0; x < frameWidth; x++) {
        final i = (y * frameWidth + x) * 4;
        final u = (x + 0.5) / frameWidth;
        pixels[i] = y < frameHeight / 2 ? 255 : 0;
        pixels[i + 1] = u > 0.375 && u < 0.625 ? 255 : 0;
        pixels[i + 2] = y < frameHeight / 2 ? 0 : 255;
        pixels[i + 3] = 255;
      }
    }
    final texture = arena<Uint32>();
    gl
      ..genTextures(1, texture)
      ..activeTexture(_glTexture0)
      ..bindTexture(_glTexture2d, texture.value)
      ..texImage2D(_glTexture2d, 0, _glRgba8, frameWidth, frameHeight, 0, _glRgba, _glUnsignedByte, pixels)
      ..texParameteri(_glTexture2d, _glTextureMinFilter, _glLinear)
      ..texParameteri(_glTexture2d, _glTextureMagFilter, _glLinear)
      ..texParameteri(_glTexture2d, _glTextureWrapS, _glRepeat)
      ..texParameteri(_glTexture2d, _glTextureWrapT, _glClampToEdge);
    final vertexArray = arena<Uint32>();
    gl
      ..genVertexArrays(1, vertexArray)
      ..bindVertexArray(vertexArray.value)
      ..useProgram(program)
      ..viewport(0, 0, size, size);

    final matrix = arena<Float>(9);
    // Draws the view and reads the pixel of the output at (x, row), row 0 being the first row of the output
    // [tangents]: the four tangents of an eye of a headset (left, right, up, down, signed as XrFovf; patch 15), in
    // place of the symmetric ones of [fov]
    (int, int, int) draw({
      required double yaw,
      required double pitch,
      double fov = 60,
      List<double>? tangents,
      List<double> crop = const [0, 0, 1, 1],
      int x = size ~/ 2,
      int row = size ~/ 2,
    }) {
      // ProjectionRenderer::SetUniforms: rows of Ry(yaw) * Rx(pitch), uploaded column major
      final cy = math.cos(yaw * math.pi / 180);
      final sy = math.sin(yaw * math.pi / 180);
      final cp = math.cos(pitch * math.pi / 180);
      final sp = math.sin(pitch * math.pi / 180);
      final rows = [cy, -sy * sp, sy * cp, 0.0, cp, sp, -sy, -cy * sp, cy * cp];
      for (var i = 0; i < 9; i++) {
        matrix[i] = rows[(i % 3) * 3 + i ~/ 3];
      }
      final tanHalf = math.tan(fov * math.pi / 360);
      final t = tangents ?? [-tanHalf, tanHalf, tanHalf, -tanHalf];
      gl
        ..uniform1i(gl.location(program, 'uSource'), 0)
        ..uniformMatrix3fv(gl.location(program, 'uViewToSphere'), 1, 0, matrix)
        ..uniform4f(gl.location(program, 'uTanFov'), t[0], t[1], t[2], t[3])
        ..uniform1f(gl.location(program, 'uSharp'), 0)
        ..uniform2f(gl.location(program, 'uOutputSize'), size.toDouble(), size.toDouble())
        ..uniform1f(gl.location(program, 'uTexelsPerPixel'), 1)
        ..uniform4f(gl.location(program, 'uEye'), 0, 0, 1, 1)
        ..uniform4f(gl.location(program, 'uCrop'), crop[0], crop[1], crop[2], crop[3])
        ..drawArrays(_glTriangles, 0, 3)
        ..finish();
      final out = arena<Uint8>(4);
      // Row 0 of the output is OpenGL's y = 0 (the pbuffer media_kit fills is read by Flutter from its first row)
      gl.readPixels(x, row, 1, 1, _glRgba, _glUnsignedByte, out);
      expect(gl.getError(), 0);
      return (out[0], out[1], out[2]);
    }

    (int, int, int) colour(int r, int g, int b) => (r, g, b);
    final yellow = colour(255, 255, 0);
    final cyan = colour(0, 255, 255);
    final red = colour(255, 0, 0);
    final blue = colour(0, 0, 255);
    final black = colour(0, 0, 0);

    expect(draw(yaw: 0, pitch: 45), yellow, reason: 'ahead and up: the upper half, the green quarter');
    expect(draw(yaw: 0, pitch: -45), cyan, reason: 'ahead and down: the lower half, the green quarter');
    expect(draw(yaw: 180, pitch: 45), red, reason: 'behind and up: outside the green quarter');
    expect(draw(yaw: 90, pitch: -45), blue);
    // The yaw turns to the right: the green quarter (45 degrees on each side of ahead) leaves the centre of a view
    // turned 60 degrees, and the left edge of that 60 degree wide view (30 degrees to the left) is back in it
    expect(draw(yaw: 60, pitch: 10), red);
    expect(draw(yaw: 60, pitch: 10, x: 2), yellow, reason: 'the left edge looks 30 degrees to the left of the yaw');
    expect(draw(yaw: 60, pitch: 10, x: size - 3), red);
    // The first row of the output is the top of the view
    expect(draw(yaw: 180, pitch: 0, fov: 90, row: 2), red, reason: 'the top row looks up');
    expect(draw(yaw: 180, pitch: 0, fov: 90, row: size - 3), blue, reason: 'the last row looks down');
    // VR180: the front half only, black behind
    expect(draw(yaw: 0, pitch: 45, crop: const [0.25, 0, 0.5, 1]), isNot(black));
    expect(draw(yaw: 180, pitch: 45, crop: const [0.25, 0, 0.5, 1]), black);
    // An eye of a headset (patch 15): its field of view is asymmetric. Left 0 and right 4: the left edge looks
    // ahead (the green quarter) and the centre at atan(2), 63 degrees to the right (out of it)
    expect(draw(yaw: 0, pitch: 10, tangents: const [0, 4, 0.5, -0.5], x: 1), yellow, reason: 'the left edge, ahead');
    expect(draw(yaw: 0, pitch: 10, tangents: const [0, 4, 0.5, -0.5]), red, reason: 'the centre, 63 degrees right');
    // Up 1 and down 0, looking behind: the whole view is above the horizon, its last row too
    expect(draw(yaw: 180, pitch: 0, tangents: const [-0.5, 0.5, 1, 0], row: size - 3), red, reason: 'above the horizon');
    expect(draw(yaw: 180, pitch: 0, tangents: const [-0.5, 0.5, 0, -1], row: 2), blue, reason: 'below the horizon');
  }, skip: skip);

  test('the test pattern of the headset probe: the grey ramp ahead, one patch per eye (patch 15)', () {
    final sources = _shaderSources();
    final program = gl.link(
      gl.compile(_glVertexShader, sources['kVertexSource']!),
      gl.compile(_glFragmentShader, _fragment(sources, 3)),
    );
    expect(program, greaterThan(0));
    final vertexArray = arena<Uint32>();
    gl
      ..genVertexArrays(1, vertexArray)
      ..bindVertexArray(vertexArray.value)
      ..useProgram(program)
      ..viewport(0, 0, size, size);
    final matrix = arena<Float>(9);
    // The centre of a view looking at (yaw, pitch), for the eye given
    (int, int, int) draw({required double yaw, required double pitch, required double eye, double time = 0}) {
      final cy = math.cos(yaw * math.pi / 180);
      final sy = math.sin(yaw * math.pi / 180);
      final cp = math.cos(pitch * math.pi / 180);
      final sp = math.sin(pitch * math.pi / 180);
      final rows = [cy, -sy * sp, sy * cp, 0.0, cp, sp, -sy, -cy * sp, cy * cp];
      for (var i = 0; i < 9; i++) {
        matrix[i] = rows[(i % 3) * 3 + i ~/ 3];
      }
      const t = 0.01;
      gl
        ..uniformMatrix3fv(gl.location(program, 'uViewToSphere'), 1, 0, matrix)
        ..uniform4f(gl.location(program, 'uTanFov'), -t, t, t, -t)
        ..uniform1f(gl.location(program, 'uSharp'), 0)
        ..uniform2f(gl.location(program, 'uOutputSize'), size.toDouble(), size.toDouble())
        ..uniform1f(gl.location(program, 'uTexelsPerPixel'), 0)
        ..uniform2f(gl.location(program, 'uPattern'), eye, time)
        ..drawArrays(_glTriangles, 0, 3)
        ..finish();
      final out = arena<Uint8>(4);
      gl.readPixels(size ~/ 2, size ~/ 2, 1, 1, _glRgba, _glUnsignedByte, out);
      expect(gl.getError(), 0);
      return (out[0], out[1], out[2]);
    }

    // The ramp: step k of 16 (6 degrees each from 48 degrees left) is k / 15 as written, no gamma applied
    for (final (lon, level) in [(-45.0, 0), (-3.0, 7), (3.0, 8), (45.0, 15)]) {
      final value = (level * 255 / 15).round();
      final (r, g, b) = draw(yaw: lon, pitch: 0, eye: 0.5);
      expect((r - value).abs() <= 1 && g == r && b == r, isTrue, reason: 'step $level at $lon: ($r, $g, $b)');
    }
    expect(draw(yaw: -24, pitch: 16, eye: 0), (255, 0, 0), reason: 'the red patch, left eye');
    expect(draw(yaw: 24, pitch: 16, eye: 0), isNot((0, 0, 255)), reason: 'no blue patch in the left eye');
    expect(draw(yaw: 24, pitch: 16, eye: 1), (0, 0, 255), reason: 'the blue patch, right eye');
    expect(draw(yaw: -24, pitch: 16, eye: 1), isNot((255, 0, 0)), reason: 'no red patch in the right eye');
    expect(draw(yaw: -45, pitch: -16, eye: 0.5, time: 0), (255, 255, 255), reason: 'the bar at its start');
    expect(draw(yaw: 0, pitch: -16, eye: 0.5, time: 2), (255, 255, 255), reason: 'the bar half way after 2 s');
    expect(draw(yaw: 0, pitch: 80, eye: 0.5).$2, greaterThan(150), reason: 'green up');
  }, skip: skip);

  test('the fisheye pass reads the second lens from the texture of the second player (IMMUCH360-NOTE.md, patch 7)', () {
    final sources = _shaderSources();
    final program = gl.link(
      gl.compile(_glVertexShader, sources['kVertexSource']!),
      gl.compile(_glFragmentShader, _fragment(sources, 1)),
    );
    // One colour per lens frame: green in this player's frame (unit 0), magenta in the second player's (unit 1)
    const side = 64;
    void solid(int unit, (int, int, int) rgb) {
      final pixels = arena<Uint8>(side * side * 4);
      for (var i = 0; i < side * side; i++) {
        pixels[i * 4] = rgb.$1;
        pixels[i * 4 + 1] = rgb.$2;
        pixels[i * 4 + 2] = rgb.$3;
        pixels[i * 4 + 3] = 255;
      }
      final texture = arena<Uint32>();
      gl
        ..genTextures(1, texture)
        ..activeTexture(unit)
        ..bindTexture(_glTexture2d, texture.value)
        ..texImage2D(_glTexture2d, 0, _glRgba8, side, side, 0, _glRgba, _glUnsignedByte, pixels)
        ..texParameteri(_glTexture2d, _glTextureMinFilter, _glLinear)
        ..texParameteri(_glTexture2d, _glTextureMagFilter, _glLinear)
        ..texParameteri(_glTexture2d, _glTextureWrapS, _glClampToEdge)
        ..texParameteri(_glTexture2d, _glTextureWrapT, _glClampToEdge);
    }

    solid(_glTexture0, (0, 255, 0));
    solid(_glTexture1, (255, 0, 255));
    gl.activeTexture(_glTexture0);
    final vertexArray = arena<Uint32>();
    gl
      ..genVertexArrays(1, vertexArray)
      ..bindVertexArray(vertexArray.value)
      ..useProgram(program)
      ..viewport(0, 0, size, size);
    Pointer<Float> matrix(List<double> columns) {
      final values = arena<Float>(9);
      for (var i = 0; i < 9; i++) {
        values[i] = columns[i];
      }
      return values;
    }

    // Lens 0 looks ahead, lens 1 behind: two equidistant lenses whose image circle reaches 95 degrees
    final ahead = matrix(const [1, 0, 0, 0, 1, 0, 0, 0, 1]);
    final behind = matrix(const [-1, 0, 0, 0, 1, 0, 0, 0, -1]);
    const degrees = math.pi / 180;
    const focal = (side / 2) / (95 * degrees);
    (int, int, int) draw({required double yaw, required bool secondTexture}) {
      final cy = math.cos(yaw * degrees);
      final sy = math.sin(yaw * degrees);
      final view = matrix([cy, 0, -sy, 0, 1, 0, sy, 0, cy]);
      final tanHalf = math.tan(30 * degrees);
      int at(String name) => gl.location(program, name);
      gl
        ..uniform1i(at('uSource'), 0)
        ..uniform1i(at('uSource1'), 1)
        ..uniformMatrix3fv(at('uViewToSphere'), 1, 0, view)
        ..uniform4f(at('uTanFov'), -tanHalf, tanHalf, tanHalf, -tanHalf)
        ..uniform1f(at('uSharp'), 0)
        ..uniform2f(at('uOutputSize'), size.toDouble(), size.toDouble())
        ..uniform1f(at('uTexelsPerPixel'), 1)
        ..uniform1f(at('uTracks'), 1)
        ..uniform2f(at('uEnabled'), 1, 1)
        ..uniform1f(at('uSecondTexture'), secondTexture ? 1 : 0)
        ..uniformMatrix3fv(at('uViewToLens0'), 1, 0, ahead)
        ..uniformMatrix3fv(at('uViewToLens1'), 1, 0, behind)
        ..uniform4f(at('uIntr0'), focal, focal, side / 2, side / 2)
        ..uniform4f(at('uIntr1'), focal, focal, side / 2, side / 2)
        ..uniform4f(at('uK0'), 0, 0, 0, 0)
        ..uniform4f(at('uK1'), 0, 0, 0, 0)
        ..uniform4f(at('uX0'), 0, 0, 0, 0)
        ..uniform4f(at('uX1'), 0, 0, 0, 0)
        ..uniform4f(at('uRegion0'), 0, 0, 1, 1)
        ..uniform4f(at('uRegion1'), 0, 0, 1, 1)
        ..uniform1f(at('uTexOf0'), 0)
        ..uniform1f(at('uTexOf1'), 1)
        ..uniform1f(at('uEquidistantFocal0'), focal)
        ..uniform1f(at('uEquidistantFocal1'), focal)
        ..uniform1f(at('uModel'), 1)
        ..uniform1f(at('uSquare'), side.toDouble())
        ..uniform3f(at('uTheta'), 100 * degrees, 85 * degrees, 95 * degrees)
        ..drawArrays(_glTriangles, 0, 3)
        ..finish();
      final out = arena<Uint8>(4);
      gl.readPixels(size ~/ 2, size ~/ 2, 1, 1, _glRgba, _glUnsignedByte, out);
      expect(gl.getError(), 0);
      return (out[0], out[1], out[2]);
    }

    expect(draw(yaw: 0, secondTexture: true), (0, 255, 0), reason: 'ahead: lens 0, in this player\'s frame');
    expect(draw(yaw: 180, secondTexture: true), (255, 0, 255), reason: 'behind: lens 1, in the second player\'s frame');
    expect(draw(yaw: 180, secondTexture: false), (
      0,
      255,
      0,
    ), reason: 'one player: stream 1 is a part of this frame, here clamped to its last column');
  }, skip: skip);
  test('the stitches sample K^T v with the correction of the frame, v without it (IMMUCH360-NOTE.md, patch 14)', () {
    final sources = _shaderSources();
    final program = gl.link(
      gl.compile(_glVertexShader, sources['kVertexSource']!),
      gl.compile(_glFragmentShader, _fragment(sources, 1)),
    );
    // Lens 0 looks ahead (the stitch frame: x right, y down, z forward), equidistant, its image circle reaching 95
    // degrees; each texel holds its own position (red its column, green its row), so the colour of an output pixel
    // tells the point of the lens it was read from. Lens 1, behind, is plain blue in the second texture.
    const side = 256;
    final ramp = arena<Uint8>(side * side * 4);
    for (var y = 0; y < side; y++) {
      for (var x = 0; x < side; x++) {
        final i = (y * side + x) * 4;
        ramp[i] = x;
        ramp[i + 1] = y;
        ramp[i + 2] = 0;
        ramp[i + 3] = 255;
      }
    }
    final blue = arena<Uint8>(4)
      ..[0] = 0
      ..[1] = 0
      ..[2] = 255
      ..[3] = 255;
    void texture(int unit, int width, Pointer<Uint8> pixels) {
      final name = arena<Uint32>();
      gl
        ..genTextures(1, name)
        ..activeTexture(unit)
        ..bindTexture(_glTexture2d, name.value)
        ..texImage2D(_glTexture2d, 0, _glRgba8, width, width, 0, _glRgba, _glUnsignedByte, pixels)
        ..texParameteri(_glTexture2d, _glTextureMinFilter, _glLinear)
        ..texParameteri(_glTexture2d, _glTextureMagFilter, _glLinear)
        ..texParameteri(_glTexture2d, _glTextureWrapS, _glClampToEdge)
        ..texParameteri(_glTexture2d, _glTextureWrapT, _glClampToEdge);
    }

    texture(_glTexture0, side, ramp);
    texture(_glTexture1, 1, blue);
    gl.activeTexture(_glTexture0);
    final vertexArray = arena<Uint32>();
    gl
      ..genVertexArrays(1, vertexArray)
      ..bindVertexArray(vertexArray.value)
      ..useProgram(program)
      ..viewport(0, 0, size, size);
    Pointer<Float> matrix(List<double> columns) {
      final values = arena<Float>(9);
      for (var i = 0; i < 9; i++) {
        values[i] = columns[i];
      }
      return values;
    }

    const degrees = math.pi / 180;
    const focal = (side / 2) / (95 * degrees);
    // Rodrigues, row major: the rotation by angle about axis
    List<double> rotation(List<double> axis, double angle) {
      final n = math.sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
      final (x, y, z) = (axis[0] / n, axis[1] / n, axis[2] / n);
      final c = math.cos(angle);
      final s = math.sin(angle);
      final t = 1 - c;
      return [
        t * x * x + c, t * x * y - s * z, t * x * z + s * y, //
        t * x * y + s * z, t * y * y + c, t * y * z - s * x, //
        t * x * z - s * y, t * y * z + s * x, t * z * z + c,
      ];
    }

    List<double> times(List<double> m, List<double> v) => [
      for (var r = 0; r < 3; r++) m[r * 3] * v[0] + m[r * 3 + 1] * v[1] + m[r * 3 + 2] * v[2],
    ];
    List<double> transposeTimes(List<double> m, List<double> v) => [
      for (var c = 0; c < 3; c++) m[c] * v[0] + m[3 + c] * v[1] + m[6 + c] * v[2],
    ];
    // The colour lens 0 gives the stitch direction s: its canvas pixel, less half a texel (the ramp's value at a
    // texel centre is its index)
    (double, double) expectedColour(List<double> s) {
      final q = math.sqrt(s[0] * s[0] + s[1] * s[1]);
      final theta = math.atan2(q, s[2]);
      if (q < 1e-9) {
        return (side / 2 - 0.5, side / 2 - 0.5);
      }
      return (side / 2 + focal * theta * s[0] / q - 0.5, side / 2 + focal * theta * s[1] / q - 0.5);
    }

    // Draws the view at yaw and pitch with the correction k (row major; null: uStabilize 0) and reads its centre
    (int, int, int) draw({required double yaw, required double pitch, List<double>? k}) {
      final cy = math.cos(yaw * degrees);
      final sy = math.sin(yaw * degrees);
      final cp = math.cos(pitch * degrees);
      final sp = math.sin(pitch * degrees);
      final rows = [cy, -sy * sp, sy * cp, 0.0, cp, sp, -sy, -cy * sp, cy * cp];
      final tanHalf = math.tan(30 * degrees);
      int at(String name) => gl.location(program, name);
      gl
        ..uniform1i(at('uSource'), 0)
        ..uniform1i(at('uSource1'), 1)
        ..uniformMatrix3fv(at('uViewToSphere'), 1, 0, matrix([for (var i = 0; i < 9; i++) rows[(i % 3) * 3 + i ~/ 3]]))
        ..uniform4f(at('uTanFov'), -tanHalf, tanHalf, tanHalf, -tanHalf)
        ..uniform1f(at('uSharp'), 0)
        ..uniform2f(at('uOutputSize'), size.toDouble(), size.toDouble())
        ..uniform1f(at('uTexelsPerPixel'), 1)
        ..uniform1f(at('uTracks'), 1)
        ..uniform2f(at('uEnabled'), 1, 1)
        ..uniform1f(at('uSecondTexture'), 1)
        ..uniformMatrix3fv(at('uViewToLens0'), 1, 0, matrix(const [1, 0, 0, 0, 1, 0, 0, 0, 1]))
        ..uniformMatrix3fv(at('uViewToLens1'), 1, 0, matrix(const [-1, 0, 0, 0, 1, 0, 0, 0, -1]))
        ..uniform4f(at('uIntr0'), focal, focal, side / 2, side / 2)
        ..uniform4f(at('uIntr1'), 1, 1, 0.5, 0.5)
        ..uniform4f(at('uK0'), 0, 0, 0, 0)
        ..uniform4f(at('uK1'), 0, 0, 0, 0)
        ..uniform4f(at('uX0'), 0, 0, 0, 0)
        ..uniform4f(at('uX1'), 0, 0, 0, 0)
        ..uniform4f(at('uRegion0'), 0, 0, 1, 1)
        ..uniform4f(at('uRegion1'), 0, 0, 1, 1)
        ..uniform1f(at('uTexOf0'), 0)
        ..uniform1f(at('uTexOf1'), 1)
        ..uniform1f(at('uEquidistantFocal0'), focal)
        ..uniform1f(at('uEquidistantFocal1'), 1)
        ..uniform1f(at('uModel'), 1)
        ..uniform1f(at('uSquare'), side.toDouble())
        ..uniform3f(at('uTheta'), 100 * degrees, 85 * degrees, 95 * degrees)
        // K^T column major is K row major (ProjectionRenderer::SetUniforms, stabilization.h)
        ..uniform1f(at('uStabilize'), k == null ? 0 : 1)
        ..uniformMatrix3fv(at('uCorrection'), 1, 0, matrix(k ?? const [1, 0, 0, 0, 1, 0, 0, 0, 1]))
        ..drawArrays(_glTriangles, 0, 3)
        ..finish();
      final out = arena<Uint8>(4);
      gl.readPixels(size ~/ 2, size ~/ 2, 1, 1, _glRgba, _glUnsignedByte, out);
      expect(gl.getError(), 0);
      return (out[0], out[1], out[2]);
    }

    // The stitch direction of the centre of a view at yaw and pitch: the sphere's (y up) with y turned down
    List<double> viewDirection(double yaw, double pitch) {
      final d = [
        math.sin(yaw * degrees) * math.cos(pitch * degrees),
        math.sin(pitch * degrees),
        math.cos(yaw * degrees) * math.cos(pitch * degrees),
      ];
      return [d[0], -d[1], d[2]];
    }

    void expectRead((int, int, int) read, List<double> s, String reason) {
      final (red, green) = expectedColour(s);
      expect(read.$3, 0, reason: '$reason: lens 0');
      expect(read.$1.toDouble(), closeTo(red, 1.5), reason: '$reason: column');
      expect(read.$2.toDouble(), closeTo(green, 1.5), reason: '$reason: row');
    }

    final views = [(0.0, 0.0), (25.0, 10.0), (-40.0, -20.0), (60.0, 30.0)];
    // Off: the direction of the view itself
    for (final (yaw, pitch) in views) {
      expectRead(draw(yaw: yaw, pitch: pitch), viewDirection(yaw, pitch), 'off, $yaw $pitch');
    }
    // A camera pitched down 10 degrees (the correction about x by -10, y down): the front samples the point 10
    // degrees above the centre of the lens, the horizon
    final pitched = rotation(const [1, 0, 0], -10 * degrees);
    expectRead(draw(yaw: 0, pitch: 0, k: pitched), [0, -math.sin(10 * degrees), math.cos(10 * degrees)], 'pitched');
    // Corrections about several axes: K^T v, never K v
    final corrections = [
      rotation(const [0, 1, 0], 20 * degrees),
      rotation(const [0, 0, 1], 15 * degrees),
      rotation(const [1, -2, 0.5], 25 * degrees),
      rotation(const [-0.3, 0.4, 1], -30 * degrees),
    ];
    var mutations = 0;
    for (final k in corrections) {
      for (final (yaw, pitch) in views) {
        final v = viewDirection(yaw, pitch);
        final read = draw(yaw: yaw, pitch: pitch, k: k);
        final right = transposeTimes(k, v);
        expectRead(read, right, 'K^T v at $yaw $pitch');
        // The mutation K v (a turn doubled instead of undone) is told apart wherever it reads elsewhere: everywhere
        // but the roll seen at its own axis
        final (red, green) = expectedColour(right);
        final (wrongRed, wrongGreen) = expectedColour(times(k, v));
        if ((red - wrongRed).abs() + (green - wrongGreen).abs() > 6) {
          mutations++;
          expect(
            (read.$1 - wrongRed).abs() + (read.$2 - wrongGreen).abs(),
            greaterThan(6),
            reason: 'K v at $yaw $pitch',
          );
        }
      }
    }
    expect(mutations, 15);
  }, skip: skip);
}
