# Changelog

## 1.0.1, 10 November 2026

- **libmpv from libmpv-builds.** media_kit_libs_windows_video now downloads libmpv from the release
  `libmpv-windows-20261110-1` of [immuch360/libmpv-builds](https://github.com/immuch360/libmpv-builds), where the
  recipe moved, pinned by the SHA-256 of its `SHA256SUMS.txt` (x64 and arm64). It is the same mpv (`0b7ed670`) and
  FFmpeg (`5b9a3ad7`) as the release `libmpv-windows-20261009-3` of freeKC/Immuch360 that 1.0.0 pinned, rebuilt from
  the same pinned sources. The README and the Windows notices name the new source.

## 1.0.0, 10 November 2026

The 360° video renderer as Immuch360 Desktop build 24 shipped it on 9 November 2026, out of the Immuch360 repository
with its history.

- **Renderer C on Windows.** media_kit_video's plugin draws the view of a 360°, VR180 or 3D video in one OpenGL ES 3.0
  pass, from the frame mpv draws into an intermediate texture, into the texture the player's `VideoController` already
  gives Flutter. The view is a uniform sent at most once per Flutter frame, bilinear while it moves and Catmull-Rom once
  it rests; a paused video is redrawn without mpv; the swap with Flutter's copy holds the lock for 0.01 ms.
- **Projections.** Equirectangular frames, mono or one eye of a stereo frame over under or side by side, over the
  whole sphere or its front half (VR180); raw dual fisheye videos stitched as they play (Mei, equidistant,
  Kannala-Brandt) and GoPro EAC pairs; the two streams of a raw file in one frame or in two players drawn together.
- **The Dart API** in `package:media_kit_video/sphere.dart`: `PluginRenderer` (attach before the file opens,
  `setView`, `setOutputSize`, `setProjection`, `setTier`, `setPartner`, `stats`, `detach`), `PluginTier` with the start
  tier of the GPU, `RendererProbe` and its verdicts. The app's own types gave way to `PluginStereoLayout`,
  `PluginCoverage` and `PluginGpu`.
- **Fixes carried by the packages**, each described in the package's `IMMUCH360-NOTE.md`: an OpenGL ES 3.0 context
  on Windows, a lost Direct3D device reported to Dart, a texture no longer replaced while Flutter reads it, the first
  texture of a video at the capped size; on Linux, GPU rendering on Flutter 3.38 and later under Wayland and X11 and
  libmpv opened at run time; media_kit's 2026 fixes and the Windows COM fix of media-kit/media-kit#1449.
- **The libmpv build** `libmpv-windows-20261009-3`, pinned by its SHA-256, and downloads checked by SHA-256 and tried
  again after a failure.
- **An example app** for Windows and Linux, and a CI that analyses and tests the packages, builds the example on both
  systems, compiles the passes of renderer C with ANGLE and checks the Linux plugin's EGL context.

On Linux, 360° videos play flat for now (no renderer C there yet), and macOS is not covered by this repository yet.
