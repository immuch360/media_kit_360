# media_kit_360

360°, VR180 and side by side 3D video in a Flutter desktop app, drawn on the GPU.

media_kit_360 is a fork of [media_kit](https://github.com/media-kit/media-kit)'s video packages whose plugin draws the
view of an immersive video itself: libmpv decodes and draws each frame as for a flat video, and one OpenGL ES 3.0 pass
of the plugin turns it into the view the user drags, straight into the texture Flutter already shows. It is the 360°
video renderer of [Immuch360](https://immuch360.com) Desktop, taken out of the
[Immuch360 repository](https://github.com/freeKC/Immuch360) so that any Flutter app can use it.

## The problem

A Flutter app on a computer has no public way to play an immersive video with the GPU doing the work.

- **No desktop player draws a sphere.** media_kit plays flat video on Windows, Linux and macOS with hardware decoding,
  and nothing more. The Flutter packages that play 360° video wrap ExoPlayer or AVPlayer, phones only.
- **Dart cannot project the video texture.** A Flutter `Texture` is not an input of a `FragmentShader` or of a
  `CustomPainter`, so the view cannot be drawn in Dart from the frames media_kit shows.
- **mpv's own shader hook costs too much.** A GLSL hook moved with `glsl-shader-opts`, as
  [mpv360](https://github.com/kasper93/mpv360) does, makes mpv rebuild its passes at every view change. Measured on a
  laptop with an Intel UHD and an RTX 4060 (below), it dropped to 11 frames a second on the Intel GPU while the view
  moved, and kept 100 to 200 MB of GPU memory per change on the RTX until the laptop ran out of memory.

## What it does

The view becomes a uniform of one pass of the plugin, and nothing of mpv changes while the user drags.

1. mpv draws each frame, exactly as for a flat video, into an intermediate RGBA texture of the video's size, capped by
   a tier.
2. One GLSL ES 3.00 pass draws the view from that texture into the player's own texture, the one its `VideoController`
   already gives Flutter. The `Video` widget, the controller and the texture id stay as they are.
3. Dart sends the view (yaw, pitch, field of view) at most once per Flutter frame, however fast the mouse moves. The
   filter is bilinear while the view moves and sharper (Catmull-Rom) once it has rested for 150 ms.
4. A paused video is redrawn from the frame kept, without any call into mpv.
5. The frame and the view are drawn into a back texture, then swapped under the lock Flutter's raster thread takes, so
   that lock is held for the swap only: 0.01 ms at the 99th percentile.

It draws an equirectangular frame (mono, one eye of a stereo frame over under or side by side, the whole sphere or its
front half for VR180), and it stitches raw dual fisheye videos as they play (Mei, equidistant and Kannala-Brandt lens
models, GoPro EAC pairs), with the two streams of a raw file in one player or in two players drawn together.

### What was measured

On 9 October 2026, a Windows 11 laptop with an Intel UHD integrated GPU and an NVIDIA GeForce RTX 4060 Laptop GPU, 16 GB
of memory, a 2026 libmpv, synthetic equirectangular clips (5.7K H.264 decoded by the processor, 8K HEVC decoded by the
GPU), the view changing 15 to 60 times a second as a drag does.

| GPU | Clip | Renderer | Frames a second |
| --- | --- | --- | --- |
| Intel UHD | 5.7K H.264 | mpv shader hook, view moving | 10.7 to 12.5 |
| RTX 4060 | 5.7K H.264 | mpv shader hook, view moving | 27.8, 17.8 and 14.8, with 3.8 to 7.4 GB of GPU memory |
| Intel UHD | 4K H.264 | this plugin, tier 2880 | 30 at rest and while dragging |
| Intel UHD | 8K HEVC | this plugin, tier 2880 | 29.2 at rest, 28.7 while dragging |
| Intel UHD | 5.7K H.264 | this plugin, tier 2880 | 27 at rest, 19 while dragging |
| RTX 4060 | 8K HEVC | this plugin, tier full | 30, no frame dropped, 8 MB more memory after 30 s of dragging |
| RTX 4060 | 5.7K H.264 | this plugin, tier full | 30, no frame dropped |

The full account, with the two players of a raw file, is in
[desktop-360-video-renderer.md](https://github.com/freeKC/Immuch360/blob/immuch360/docs/immuch360/desktop-360-video-renderer.md)
of the Immuch360 repository.

## Use it in your app

The packages are not on pub.dev. Take them from this repository by tag:

```yaml
dependencies:
  media_kit: ^1.2.6
  media_kit_video:
    git:
      url: https://github.com/immuch360/media_kit_360
      ref: v1.0.0
      path: packages/media_kit_video
  media_kit_libs_windows_video:
    git:
      url: https://github.com/immuch360/media_kit_360
      ref: v1.0.0
      path: packages/media_kit_libs_windows_video
  media_kit_libs_linux:
    git:
      url: https://github.com/immuch360/media_kit_360
      ref: v1.0.0
      path: packages/media_kit_libs_linux

# Optional: media_kit with the fixes of this repository (packages/media_kit/IMMUCH360-NOTE.md), among them drag and
# drop that no longer breaks on Windows after a few players were disposed. media_kit_video also works with media_kit
# 1.2.6 of pub.dev.
dependency_overrides:
  media_kit:
    git:
      url: https://github.com/immuch360/media_kit_360
      ref: v1.0.0
      path: packages/media_kit
```

Then attach the renderer before the file opens, so that the first frame is already the view:

```dart
import 'package:media_kit/media_kit.dart';
import 'package:media_kit_video/media_kit_video.dart';
import 'package:media_kit_video/sphere.dart';

final player = Player();
final controller = VideoController(player);
final renderer = PluginRenderer.forController(controller);

final attached = await renderer.attach(
  projection: PluginProjection.equirect(layout: PluginStereoLayout.leftRight, coverage: PluginCoverage.half),
  outputSize: const Size(2560, 1440), // the view in physical pixels
);
await player.open(Media(path));

// In the widget tree: the texture has the view's own shape when attached.ok, else the frame is flat
Video(controller: controller, controls: NoVideoControls, fit: attached.ok ? BoxFit.fill : BoxFit.contain);

// On every drag, wheel or key event; moving: false once the view rests
renderer.setView(const PluginView(yaw: 30, pitch: -10, fov: 90), moving: true);
```

[example/lib/main.dart](example/lib/main.dart) is a complete app: it opens a file or a URL, lets the user drag the
view, zoom with the wheel and change the projection, and runs the renderer probe.

### The API

`PluginRenderer` drives one player, `package:media_kit_video/sphere.dart`:

| Call | What it does |
| --- | --- |
| `PluginRenderer.forController(controller, adapter: ...)` | the renderer of the player of a `VideoController`; `adapter` optionally gives the GPU in use (`PluginGpu`) for the start tier |
| `attach(projection:, outputSize:, tier:, view:)` | turns the projection on, before the file opens; answers `PluginAttach` with `ok`, the tier, the GPU's name, or the reason of a refusal |
| `setView(view, moving:)` | the view: yaw positive to the right, pitch positive up, vertical field of view, in degrees |
| `setOutputSize(size)` | the window's new size in physical pixels; sizes closer than 100 ms are merged |
| `setProjection(projection)` | another layout, eye or coverage for the same video: uniforms only, nothing of mpv |
| `setTier(tier)` | another size of the intermediate texture, at most once a second once a frame was drawn |
| `stats(probe:)` | what the plugin drew since the previous call: frames, redraws, failures, draw times |
| `setPartner(other)` | pairs a second player that decodes stream 1 of a raw file |
| `detach()`, `dispose()` | the texture shows the flat frame again; `dispose` also ends the renderer |

`PluginProjection.equirect` takes a `PluginStereoLayout` (mono, top bottom, left right), a `PluginCoverage` (full or
half) and a `PluginEye`. `PluginProjection.raw` takes the rawProjection JSON of a raw camera file, documented on
`rawStitchUniforms`.

### Tiers and the probe

The intermediate texture keeps the video's shape, capped by a tier.

| Tier | Intermediate texture | Where it starts |
| --- | --- | --- |
| `full` | the video's size, at most 8192 wide (118 MB for 8K) | a dedicated GPU |
| `w4096` | at most 4096 x 2048 (33.5 MB) | the step down from full |
| `w2880` | at most 2880 x 1440 (16.6 MB) | an integrated GPU, or one not recognised |

`PluginTier.startFor` reads ANGLE's renderer string, or better what DXGI says of the adapter when the app gives it
through `adapter`: the integrated GPUs of the Intel Core Ultra call themselves "Intel(R) Arc(TM) Graphics", as the
dedicated Arc cards do. 2880 rather than 4096 on an integrated GPU, since the Intel UHD drew a 5.7K video at 22 frames
a second at rest at 4096 against 27 at 2880.

`RendererProbe` measures the playback itself: over its first 5 seconds, paused and buffering time left out, the frames
drawn against the video's frame rate and the growth of the process's memory, then the memory again during each drag.
At 0.9 of the rate or more the tier stays, below it one tier down; at the lowest tier below 0.6 the video should play
flat. More than 512 MiB of growth takes a tier down. `decodeLimited` tells a shortfall that comes from the decoder,
which a smaller tier does not cure.

### The libmpv build

On Windows, `media_kit_libs_windows_video` downloads libmpv and ANGLE when the app's build is configured, checks each
archive by its SHA-256 and bundles them next to the executable. The libmpv it takes by default is the release
[libmpv-windows-20261110-1](https://github.com/immuch360/libmpv-builds/releases/tag/libmpv-windows-20261110-1) of
[libmpv-builds](https://github.com/immuch360/libmpv-builds) (mpv `0b7ed670`, FFmpeg `5b9a3ad7`), whose sources are
published with it. To pin another release, change `IMMUCH360_LIBMPV_TAG`, the archive names and their SHA-256 at the
top of `packages/media_kit_libs_windows_video/windows/CMakeLists.txt`, from the release's `SHA256SUMS.txt`. Its d3d11 interop imports the frames the GPU decodes into ANGLE without a copy, where media-kit's
2024 libmpv copies each of them back to memory (`d3d11va-copy`). Set `IMMUCH360_LIBMPV_FROM_CI=0` in the environment of a build
to take media-kit's 2024 archive instead. The licences of what is bundled, libmpv and FFmpeg under the LGPL among
them, are in [NOTICES.md](packages/media_kit_video/windows/NOTICES.md); ship it and its `licenses/` folder with your app.

On Linux nothing is bundled: the plugin opens the system's libmpv at run time (`libmpv.so.2` or `libmpv.so.1`), and
the build machine needs its headers (`libmpv-dev`).

## Platforms

| System | Flat video | 360°, VR180 and 3D view |
| --- | --- | --- |
| Windows x64 | yes, GPU rendering through ANGLE | yes, renderer C |
| Linux x64 (X11 and Wayland) | yes, GPU rendering through EGL | not yet: `attach` answers not ok and the video plays flat |
| macOS | not yet in this repository | not yet |
| Windows on Arm | not yet: the ANGLE build is x64 only | not yet |

`PluginRenderer.supported` tells whether the view can be drawn on the running system.

## What is in this repository

| Folder | What it is |
| --- | --- |
| `packages/media_kit_video` | the fork with renderer C (Windows native code, the Dart API under `lib/src/sphere`), and fixes for Windows and Linux; each change is described in its `IMMUCH360-NOTE.md` |
| `packages/media_kit` | media_kit with the 2026 fixes of upstream's main branch and a Windows COM fix |
| `packages/media_kit_libs_windows_video` | the Windows libraries: the pinned libmpv, ANGLE, downloads checked by SHA-256 and tried again |
| `packages/media_kit_libs_linux` | switches the Linux plugin on, without upstream's mimalloc download |
| `example` | the example app for Windows and Linux |

The history of these files in the Immuch360 repository came along. The CI analyses and tests the packages, builds
the example on Windows and Linux, compiles the passes of renderer C with the ANGLE of the Windows build and checks the
EGL context of the Linux plugin on a virtual screen.

## Credits

- [media_kit](https://github.com/media-kit/media-kit) by Hitesh Kumar Saini and its contributors, MIT: the code of
  these packages is theirs, but for the changes each `IMMUCH360-NOTE.md` describes.
- [mpv](https://mpv.io) and [FFmpeg](https://ffmpeg.org), which decode and draw every frame.
- [ANGLE](https://chromium.googlesource.com/angle/angle), OpenGL ES over Direct3D 11 on Windows.
- [mpv360](https://github.com/kasper93/mpv360) by kasper93, MIT, which showed the shader hook this renderer replaces.

## Licence

MIT, see [LICENSE](LICENSE): the upstream code keeps the copyright of Hitesh Kumar Saini, the 360 work is copyright
2026 FreeKC. The libraries the Windows build bundles keep their own licences, listed in
[NOTICES.md](packages/media_kit_video/windows/NOTICES.md). `packages/media_kit/lib/ffi` is a copy of `package:ffi`
under the BSD 3-Clause licence of the Dart project authors.

media_kit_360 is not affiliated with media_kit, the mpv project or FFmpeg. Windows is a trademark of the Microsoft
group of companies.
