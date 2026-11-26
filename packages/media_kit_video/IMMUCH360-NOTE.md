> This note was written in the Immuch360 app repository (github.com/freeKC/Immuch360), where this package lived
> under `mobile/packages/` until it moved to github.com/immuch360/media_kit_360 on 10 November 2026: the paths
> under `mobile/` and `.github/` below are that repository's, and "the app" is Immuch360.

Copy of `media_kit_video` 2.0.1 (MIT, https://github.com/media-kit/media-kit, folder `media_kit_video`) at commit
`c533e446755f51cf53c7e57aea873f2aa5355f81` of `main` (2026-08-30, "fix: memory leaks (#1446)"), the newest commit of
`main` on 2026-10-08, which carries the 2026 fixes the pub.dev release 2.0.1 of 2025-12-02 lacks (#1440 for Linux,
#1446). Used by Immuch360 Desktop through `dependency_overrides` in `mobile/pubspec.yaml`, together with the copies of
`media_kit` and `media_kit_libs_windows_video` of the same commit. The MIT notice is in `LICENSE`, as upstream has it.

What differs from upstream, to apply again when a newer commit is copied here:

Layout (no change to the code):

- `pubspec.yaml`: `windows`, `linux` and `macos` are the only plugin platforms (upstream also declares android, ios and
  web), so that the Android and iOS builds of Immuch360 register nothing of it and bundle no libmpv through it;
  `publish_to: none`, version `2.0.1+immuch360`. Since the move to github.com/immuch360/media_kit_360 the package is
  again a member of a pub workspace (`resolution: workspace`, the root `pubspec.yaml` of that repository), with the
  dev dependencies of its tests and `logging` for the 360 renderer's Dart side (patch 13).
- The `android/`, `ios/` and `example/` folders are left out.
- The macOS sources that upstream shares with iOS through symbolic links into `common/darwin/Classes/` are copied in
  place under `macos/media_kit_video/Sources/media_kit_video/{plugin,stub}/common/`, and `common/darwin/Classes/` is
  left out: the Windows mirror of the work tree and Windows clones do not carry symbolic links. `common/darwin/`
  keeps the Makefile (libmpv headers) and the Podspec helper the macOS podspec uses.
- `common/renderer/`: the portable files of renderer C (patches 6, 14 and 15), shared by the platforms since
  22 November 2026 (patch 16). They were written under `windows/`, and the patches below name them where they are now.

Patches:

1. Windows, an OpenGL ES 3.0 context (`windows/angle_surface_manager.h`, `windows/angle_surface_manager.cc`).
   Upstream asks ANGLE for an ES 2.0 context (`kEGLContextAttributes`, `EGL_CONTEXT_CLIENT_VERSION, 2`). In ES 2.0 mpv
   runs GLSL ES 1.00 and may render in an 8 bit FBO (`rgba8` is the last format it tries), which bands; the 360
   projection hook and the Spatial passes of Immuch360 want GLSL ES 3.00, and ES 3.0 gives an `rgba16f` FBO. What the
   context does not change: whether mpv imports the frames of the D3D11 decoder without a copy. Its `d3d11egl` interop
   (`video/out/opengl/hwdec_d3d11egl.c`, `init`) waives `GL_OES_EGL_image_external_essl3` in ES 2.0 (`gl->es == 200`)
   anyway, and in both versions also wants `EGL_EXT_device_query` among the extensions of the display, which the
   ANGLE of the libs package lists only among its client extensions: so with the 2024 libmpv (mpv 0.39) every
   hardware decoded frame is copied back (`hwdec-current` is `d3d11va-copy` in every spike). The mpv commit that
   `.github/workflows/immuch360-libmpv.yml` builds (`0b7ed670`) also accepts the extension from the client list
   (`hwdec_d3d11egl.c`, `init`): zero copy needs no patch of mpv, only that build switched on
   (`IMMUCH360_LIBMPV_FROM_CI` in `media_kit_libs_windows_video`), then `hwdec-current` and the video support of the
   ANGLE device checked by the harness. `ANGLESurfaceManager` now chooses a config with
   `EGL_RENDERABLE_TYPE` = `EGL_OPENGL_ES3_BIT` and creates an ES 3.0 context (`CreateContext(3)`), and falls back to
   upstream's ES 2.0 config and context when the display refuses (the Direct3D 11 feature level 9_3 and Direct3D 9
   displays of its fallback chain), so a machine without ES 3.0 keeps the hardware path it had. It prints which one it
   got ("OpenGL ES 3.0 context") and exposes it as `client_version()`. Its premise is checked without a window by
   `mobile/test/desktop/video/libmpv_probe_test.dart` on Windows, which makes the same config and context through
   FFI: the ANGLE of the libs package gives OpenGL ES 3.0 with `GL_OES_EGL_image_external_essl3`, and mpv renders in
   it with GLSL ES 3.00 into an `rgba16f` FBO. The patched plugin itself runs in the reference run of the measurement
   harness (`mobile/integration_test/desktop_video_measure_test.dart`), on the owner's screen.

2. Linux, media_kit #1404, "[Linux] H/W rendering fails on Flutter 3.38+" (`linux/video_output.cc`). Since Flutter
   3.38 the engine's EGL context is current on the raster thread only, so `eglGetCurrentDisplay()` on the platform
   thread gives `EGL_NO_DISPLAY` and every output fell back to S/W rendering, in which mpv runs no user shader. The fix
   proposed in the issue: when no context is current, the EGL display is taken from GDK's native display (Wayland or
   X11) through `eglGetPlatformDisplayEXT`, looked up with `eglGetProcAddress` because epoxy's own dispatch of
   `eglGetPlatformDisplay` asks for EGL 1.5 on a current display that does not exist yet (reported in the issue); the
   config is chosen with `eglChooseConfig` (ES 2.0, RGBA 8) since Flutter's cannot be queried; the isolated context is
   released after its setup and in `dispose` instead of "restoring" a context that was never current. The path where
   Flutter's context is current is unchanged. Compiled since the copy of `media_kit_libs_linux` joined the app
   (2026-10-20, patch 8); before, the Linux build compiled the stub of upstream (`MEDIA_KIT_LIBS_NOT_FOUND`). To test
   on Wayland and on X11 with a real GPU: the issue reports a GLX `BadAccess` on X11 with the GDK display on two
   machines.

3. Dart, the size of the texture (`lib/src/video_controller/native_video_controller/real.dart`,
   `lib/src/video_controller/video_controller.dart`). Upstream sets the texture to the size of each new video
   whenever mpv reports its parameters, even when the app fixed a size (`VideoControllerConfiguration.width` and
   `height`, or `setSize`): a fixed size lasted until the first frame. A fixed size now stays. And a new static
   `VideoController.maxOutputHeight` caps the texture of a video whose size is not fixed at that many lines, keeping
   its shape (the width rounded to an even number): the render height cap of the design (2.11), 1440 lines set by
   the app (`DesktopPlayerOptions.maxRenderHeight`), Flutter scaling the texture up the rest of the way, so that an
   8K video is drawn into 2880 x 1440 rather than 7680 x 3840. The measurement harness's "window" render size relied
   on the fixed size, which upstream replaced: its runs before this patch measured the video's own size.

4. Windows, a lost graphics device (`windows/angle_surface_manager.{h,cc}`, `windows/video_output.{h,cc}`). After a
   driver update or reset, a GPU switched or gone, or some sleeps, the Direct3D 11 device of a player is removed:
   upstream then draws nothing, and a new texture size threw inside the thread pool's task, where the exception was
   lost with the texture already unregistered, so the video stayed black or frozen, with its sound, in every later
   use of that player. `ANGLESurfaceManager::IsDeviceLost` asks the device (`GetDeviceRemovedReason`) before each
   frame is drawn; `VideoOutput` stops drawing and making textures on a lost device, and tells Dart once through the
   texture update with the id 0, which is never given otherwise. The app's player pool then disposes that player and
   the idle ones, and the page opens its video again where it was in a new player, on a new device
   (`mobile/lib/desktop/video/player_pool.dart`, `PlayerLease.discard`). The detection is in the code paths that ran
   on the owner's PC; a real device loss (sleep and resume, a driver restart with `pnputil /restart-device`) is a
   check of the owner's session (plan 2.4, DEVICE).

5. Windows, a texture replaced while Flutter reads it (`windows/angle_surface_manager.cc`, `windows/video_output.cc`).
   Each new texture size (another video in a pooled player, or the render height cap of patch 3 arriving after mpv's
   first size) makes the two Direct3D textures again on the player's worker thread, while Flutter's raster thread may
   be copying between them (`Read`) for the texture being replaced. Upstream released and made them without the
   mutex `Read` holds, and gave `texture_id_` the new id before the texture was in the map its callbacks look it up
   in (`textures_.at`, which throws inside the engine). On 2026-10-09 the measurement harness ended three times out
   of eleven runs that way, each time just after a new texture of a 5.7K or 8K video on the Intel UHD, mostly when
   it shrank from 5760 x 2880 to 2880 x 1440: an access violation in the Intel driver (`igd10um64xe.dll`, c0000005)
   twice and a fail fast in `flutter_windows.dll` (c0000409) once. `SetSize` now holds the mutex while it makes the
   textures again, and `Resize` zeroes `texture_id_` before the old texture is unregistered and gives it the new id
   only once the texture is in the map, both under the lock the callbacks take.

6. Windows, renderer C: the 360 view drawn by the plugin (`common/renderer/projection_renderer.{h,cc}`,
   `common/renderer/projection_shaders.h`, `windows/video_output.{h,cc}`, `windows/video_output_manager.{h,cc}`,
   `windows/angle_surface_manager.{h,cc}`, `windows/media_kit_video_plugin.cc`, `windows/CMakeLists.txt`,
   `lib/src/projection_output.dart`, exported by `lib/media_kit_video.dart`). Decision DP1 of Immuch360 Desktop
   (2026-10-09): the 360 view as an mpv user shader changed through `glsl-shader-opts` rebuilt mpv's passes at every
   change, kept 40 to 200 MB of GPU memory per change on an RTX 4060 until the PC ran out of memory, and gave 11 frames
   a second on an Intel UHD. With this patch a player may be switched to a projection
   (`VideoOutputManager.SetProjection`): mpv then draws each frame, exactly as for a flat video, into an intermediate
   RGBA 8 texture of the video's size, capped by the tier Dart asks (at most so wide and so many pixels, the shape kept,
   rounded to even sizes), and one GLSL ES 3.00 pass draws the view from it into the player's own texture, which keeps
   its id, its `VideoController` and its `Video` widget; the texture is then the output size Dart gives (capped at
   `maxOutputHeight` lines) from its first frame, not the video's. Three passes: an equirectangular frame (one eye of a
   stereo layout, the whole sphere or its front half), a fisheye pair (Mei, equidistant, Kannala-Brandt) and a GoPro EAC
   pair, both stitches being the GLSL of the phones (`RawStitchShaders.kt` of the app) reading the decoded streams side
   by side in the one frame; bilinear while the view moves, Catmull-Rom (or four reads per pixel when the view shrinks
   the frame) once it rests. The view (`VideoOutputManager.SetView`, yaw, pitch, vertical field of view, sharp) is kept
   by the output and drawn by at most one waiting task of the thread pool, so that views coming faster than the pass are
   merged; it is answered on the platform thread without waiting for `VideoOutputManager`'s mutex (a lookup map of its
   own, filled once an output is made and emptied before it is destroyed). Each draw asks mpv
   (`mpv_render_context_update`) whether it has a frame: mpv draws into the intermediate texture only then, and a paused
   video is redrawn from the texture kept without any call into mpv. The player's `ANGLESurfaceManager` gets a second
   internal texture while a projection is on (`SetDoubleBuffered`, `MakeBackCurrent`, `SwapBack`, and `Read` copying
   from the front one): mpv's frame and the view are drawn into the texture Flutter does not copy from and finished
   (`glFinish`), then the two are swapped under the lock that `Read` takes on Flutter's raster thread, which is thus
   held for the swap only (0.01 ms at the 99th percentile on both GPUs, against up to 16 ms per draw when the drawing
   happened under it: the RTX 4060 takes 14 to 16 ms to finish each draw of a paused video). No swap is reported to mpv
   (`mpv_render_context_report_swap`): once one is, vo_libmpv waits for the next before it hands over each frame, which
   put 30 to 53 frame intervals over 50 ms in 10 s into an 8K video on the RTX 4060.
   `VideoOutputManager.ProjectionStats` gives the counts and times of the draws and, on request, five pixels of the
   output read back. A context without OpenGL ES 3.0 (patch 1's fallback) refuses the projection with the reason, and
   so does a driver that does not compile or link the pass of the projection's kind, which is built when the
   projection is set rather than at the first frame, with the shader's log as the reason (the player is then flat
   again, a projection that was on included); turning it off gives upstream's drawing back, the current frame drawn at
   once. A new projection whose frame layout differs (the kind, the streams, the lens or face uniforms: the next file
   of a raw video's fallback chain) does not draw the frame kept from the previous file: the output keeps its last view
   until the new file's first frame. The view convention is the app's photo
   sphere: yaw positive to the right of the frame's centre column, pitch positive up; the first row of the output is the
   top of the view, as mpv draws into the pbuffer (`MPV_RENDER_PARAM_FLIP_Y` left at 0). Checked without a window by
   `mobile/test/desktop/video/plugin_shaders_windows_test.dart` (the three programs compile and link in ANGLE's ES 3.0,
   the equirectangular pass looks where the view looks) and with the player by the measurement harness
   (`IMMUCH360_MEASURE_RENDERER=c`). Also fixed on the way, in `VideoOutput::~VideoOutput`: when no texture was
   registered (after a lost device of patch 4, or a failed `Resize`), the destructor waited for a promise that nothing
   would fulfil, holding `VideoOutputManager`'s mutex, so that every later player of the app hung; the cleanup is now
   posted to the thread pool in that case too.
7. Windows, two players for one raw 360 file (`windows/angle_surface_manager.{h,cc}`,
   `common/renderer/projection_renderer.{h,cc}`, `common/renderer/projection_shaders.h`, `windows/video_output.{h,cc}`,
   `windows/video_output_manager.{h,cc}`, `windows/media_kit_video_plugin.cc`, `lib/src/projection_output.dart`).
   The raw files whose lenses are in two streams (Insta360 X4 and later, X3 pairs, DJI Osmo 360) were stacked side by
   side in one mpv core with FFmpeg's `hstack`, which needs the frames in memory: two 3840x3840 HEVC streams copied
   back from the GPU are too much for a laptop. With this patch each stream gets its own player, decoded by the GPU
   without a copy (`d3d11` into ANGLE through `d3d11egl`, 10 bit `p010` frames included), and renderer C draws the view
   from both. The ANGLE contexts of the players join one share group (`share_group()`: each new context shares with a
   live one of the same display and version, and leaves the group when it is destroyed or when its constructor
   throws), so that the primary player's context reads the secondary player's intermediate texture. A projection
   setup can be `secondary` (no pass of its own, a 2x2 output, its frame drawn into its intermediate texture only) or
   `partnerStream` (the stitch pass reads lens 1 from a second sampler, `uSource1`, with `uSecondTexture` set).
   `VideoOutputManager.SetPartner` (handle, partner or nothing) pairs the two outputs on the thread pool. From then on
   the primary player draws for both (`DrawPaired`): each draw asks both mpv cores whether they have a frame and of
   which time (`time-pos`, with `video-timing-offset` 0), draws two frames whose times are within 0.45 of a frame
   period together and then the view, draws a frame alone once it waited for its partner longer than the measured
   delay (at least 12 ms, at most 0.75 of a period, `PairTimer` waking the primary then), draws the earlier of two
   frames apart by one or two periods without a view to catch up, and seeks the secondary player to the primary's time
   when they are more than half a second apart (at most every 1.5 s). The delay of the secondary player's frames behind
   the primary's is measured at each frame drawn, from the latest frame of each player whatever their times (when the
   secondary player's frame came, less when the primary's clock reached that frame's time), and smoothed; every 500 ms
   the secondary player's `speed` is set to the primary's, corrected by at most 20% in proportion to that delay, so
   that its frames come with the primary's. A secondary player opens a little after the primary and starts some frames
   behind it: the first PC run measured the delay only between frames of the same time, which never came, so the two
   lenses stayed some frames apart; it is now closed within about two seconds. A secondary player whose primary
   is gone draws its frames alone, so that its mpv never waits. `ProjectionStats` adds `pairedFrames`, `aloneFrames`,
   `catchUpFrames`, `partnerFrames` (counts since the last read), `partnerOffsetMs` and `partnerSpeed`. Measured
   before the patch with two mpv players on the test laptop and a real Osmo 360 file (two 3840x3840 HEVC Main 10
   tracks at 25 frames a second): no frame dropped on the RTX 4060, 1% on the Intel UHD; with the patch, see
   `docs/immuch360/desktop-360-video-renderer.md`. Checked without a window by
   `mobile/test/desktop/video/plugin_shaders_windows_test.dart` (the fisheye pass reads lens 1 from the second
   texture).

8. Linux, libmpv opened at run time, not linked (`linux/mpv_symbols.cc`, `linux/include/media_kit_video/mpv_symbols.h`,
   `linux/video_output.cc`, `linux/texture_gl.cc`, `linux/CMakeLists.txt`). Upstream links the plugin to the libmpv it
   is built against (`PkgConfig::mpv`). The Linux build runs on Ubuntu 22.04 for its glibc, whose libmpv is
   `libmpv.so.1` (mpv 0.34), while Ubuntu 24.04, Debian 12 and Fedora have `libmpv.so.2`: a plugin linked to one does
   not load where only the other is, and a plugin that does not load stops the whole app at start. The Linux packages
   depend on either (`libmpv2 | libmpv1`, `mpv-libs`) and media_kit's Dart side already opens whichever is there
   (`NativeLibrary`: `LIBMPV_LIBRARY_PATH`, then `libmpv.so`, `libmpv.so.2`, `libmpv.so.1`). The plugin now finds the
   seven functions it calls (`mpv_set_option_string`, `mpv_get_property`, `mpv_free_node_contents` and four of the
   render API, which both versions have) with `dlsym` in the same library: the one already in the process
   (`RTLD_NOLOAD`, in the same order), else the first that opens. Without them an output gets a texture that draws
   nothing, and says so. The CMake takes the headers of libmpv only (libmpv-dev on the build machine, a configure
   error naming the package when they are missing) and links `libdl`. The ELF files of the bundle then need no libmpv,
   which `.github/desktop/packaging/linux/check_package.sh` already expected. The switch that compiles the real plugin
   instead of the stub is `MEDIA_KIT_LIBS_AVAILABLE`, set by `mobile/packages/media_kit_libs_linux`.
9. macOS, software rendering when OpenGL has no context (`macos/.../plugin/gl/OpenGLHelpers.swift`,
   `plugin/TextureHW.swift`, `plugin/common/VideoOutput.swift`). Upstream force unwraps the OpenGL 3.2 pixel format of
   `createPixelFormat` and calls `exit(1)` when `createContext` fails, so the first player of a Mac without such a
   pixel format ends the app: the macOS runner of GitHub (a virtual Mac with a paravirtual GPU) crashed so at the first
   player of the self check, "Unexpectedly found nil" in `OpenGLHelpers.createPixelFormat()`. Both now return nil
   with the CGL error in the log, `TextureHW.init` is failable, and `VideoOutput` renders that player with `TextureSW`
   (mpv's software render API, as with hardware acceleration off) and logs "no OpenGL context for hardware rendering".
   A Mac with a GPU keeps the OpenGL path of upstream.
10. Linux, the isolated EGL context next to GDK's GL context (`linux/isolated_egl.cc`,
   `linux/include/media_kit_video/isolated_egl.h`, `linux/video_output.cc`, `linux/texture_gl.cc`,
   `linux/CMakeLists.txt`, `linux/test/isolated_egl_test.cc`). On X11 every output fell back to S/W rendering with
   "Failed to make isolated EGL context current. Error: 0x3002" (EGL_BAD_ACCESS), while Wayland rendered with the GPU.
   GTK 3 paints a window holding GL content through a GL context of its own, GLX on X11 and EGL on Wayland, and leaves
   it current on the platform thread after each frame (`gdk_window_end_paint_internal`); libglvnd refuses an EGL
   context on a thread where a GLX one is current ("Another window API already has a current context",
   `src/EGL/libegl.c`). Under Wayland the switch went through, but GDK then believed its context still current, and
   the code of patch 2 took GDK's EGL context for Flutter's (its display and config). Now every place that makes the
   isolated context current on any thread (`video_output_new`, `video_output_dispose`, `texture_gl_dispose`) puts
   GDK's context aside through `gdk_gl_context_clear_current` and makes it current again after, restores another
   EGL context (Flutter's, before Flutter 3.38) as before, and binds the EGL API it found; a context is taken for
   Flutter's only when GDK has none current, else the config is chosen as Flutter chooses its own (ES 2.0, RGBA 8).
   Before the H/W path is taken, a 1x1 texture goes through an EGLImage into another texture, as `TextureGL` hands
   each frame to Flutter; when the display or the context cannot, the output is drawn in S/W instead of black. The
   log line says the path: "H/W rendering with isolated EGL context (X11, renderer)" or "S/W rendering (reason)"; a
   failed H/W setup unregisters its texture and destroys its context instead of leaving them behind. Flutter's
   texture names are deleted in Flutter's context (at once on the raster thread, else at the next populate there),
   not in the context current where the output is disposed of, GDK's on the platform thread, and the 1x1 texture
   made before the first frame is deleted when the first frame comes. The test program, built and run by the Linux job
   of the desktop workflow on its virtual screen, paints a GTK window as Flutter's view does and checks all of it;
   checked on this PC too, under Xvfb (llvmpipe) and under WSLg on X11 and Wayland (D3D12 on an Intel GPU), where the
   former code failed with 0x3002 on X11.
11. Windows, the first texture of a video at the capped size (`windows/video_output.{h,cc}`,
   `windows/media_kit_video_plugin.cc`, `lib/src/video_controller/native_video_controller/real.dart`). The cap of
   patch 3 is applied by Dart once mpv reports the video's parameters; until that size reaches the plugin,
   `CheckAndResize` sized the texture to `video-out-params`. A video of 7680 x 4320 then got two textures of 133 MB
   for its first frame, made again at 2560 x 1440 a frame later with a new texture id: in 3 of 6 opens of an 8K HEVC
   file from a Samsung S24+ on the owner's PC (2026-11-02), and 4 of 23 opens of 5.7K and 8K files on 2026-10-09.
   `VideoOutputManager.Create` now also gives `maxOutputHeight` (`VideoController.maxOutputHeight`, "null" when not
   set), kept in `VideoOutputConfiguration::max_output_height`, and `CheckAndResize` caps the video's own size with it
   while no size was given (no projection, H/W rendering), with the arithmetic of the Dart side
   (`(width * cap / height).round() & ~1`), so that the size Dart gives next is the same and makes no new texture.
   A size given by the configuration or by `setSize` is not capped, as before.
12. Windows, a flat frame drawn outside the lock, when asked (`windows/video_output.{h,cc}`,
   `windows/angle_surface_manager.h`). Upstream draws each flat frame (`ANGLESurfaceManager::Draw`: mpv's draw, then
   `glFinish`) under the lock that `Read` takes on Flutter's raster thread to copy the texture. A frame of the graphics
   card's decoder is drawn in a millisecond or two, but a frame decoded by the processor is uploaded by that draw: the
   y4m of an AV2 video (Immuch360's AV2 companion, `lib/desktop/video/av2_playback.dart`), 12 MB a picture in 4K. With
   the variable `IMMUCH360_FLAT_BACK_BUFFER` of the app's environment, a flat frame is drawn as renderer C draws (patch
   6): into the internal texture `Read` does not copy from, finished there, then swapped under the lock; `1` for every
   flat frame, `2` for the frames of the processor only (mpv's `video-out-params` has a `pixelformat` and no
   `hw-pixelformat`, which a d3d11 frame names), read once per frame. Without it, upstream's single texture. Not on by
   default: measured on the PC on 3 November 2026 (the fix round of lot AD3 of the AV2 videos, PresentMon and Flutter's
   frame times, H.264 and AV2 runs one after the other), it changed nothing steady: at 1080p the AV2 video already
   presented as often as H.264, and at 4K, with a decoder slower than the video, one pair of runs was better with it,
   one the same and one worse; the Intel UHD, busy 8.4 to 13.3 ms a present with the 4K AV2 video (12.5 or more in 4
   of those 6 runs) against 5.4 to 8.6 ms with H.264, looked like the limit rather than the lock. Kept for the next
   measures.
13. Dart, the 360 renderer's API in the package (`lib/sphere.dart`, `lib/src/sphere/plugin_renderer.dart`,
   `lib/src/sphere/renderer_probe.dart`, `test/sphere/`, 10 November 2026). `PluginRenderer` (attach before the file
   opens, `setView`, `setOutputSize`, `setProjection`, `setTier`, `setPartner`, `stats`, `detach`), `PluginTier` and
   `RendererProbe` were the app's (`mobile/lib/desktop/video/render/`), on top of patch 6's `ProjectionOutput`. They
   moved here with their history and their tests, the app's own types replaced by the package's: `PluginStereoLayout`
   for the app's `StereoLayout`, `PluginCoverage` for its `SphereCoverage`, `PluginGpu` for the `GpuAdapter` of its
   DXGI plugin, given through `PluginRenderer.forController(adapter: ...)`. The app keeps what is its own: the renderer
   choice of its settings and what the probe measured per kind of video (`sphere_renderer.dart`, a JSON file of its
   support folder), the projection of its assets (`projection_params.dart`), its two lens raw file logic and its page.
   The plugin renderer test runs on every system; the shader test needs ANGLE, so the Windows build of the example
   app (`MEDIA_KIT_360_BUILD_DIR` names another build folder).
14. Windows, renderer C stabilises a raw 360 video with its camera's motion (`common/renderer/stabilization.h`,
   `common/renderer/test/stabilization_test.cc`, `common/renderer/projection_shaders.h`, `common/renderer/projection_renderer.{h,cc}`,
   `windows/video_output.{h,cc}`, `windows/video_output_manager.{h,cc}`, `windows/media_kit_video_plugin.cc`,
   `lib/src/projection_output.dart`, `lib/src/sphere/plugin_renderer.dart`, 12 November 2026, release 1.1.0). The app
   reads the motion track of an Insta360, GoPro or DJI file and builds one correction per frame (issue 14 of the app),
   keyed on the frame's presentation time: a unit quaternion K that shows the point of the stitched sphere at
   direction d (the stitch's view frame, x right, y down, z forward) at K d, the convention of its Android player.
   `VideoOutputManager.SetStabilization` (handle, enabled, and a table of times in microseconds and four values a frame
   when "table" is true; none removes it) keeps the table in the output, checked as the Android player checks it
   (rising times, unit quaternions). The stitch passes read `stitch(uCorrection * v)` when `uStabilize` is 1, with
   uCorrection = K^T of the frame drawn (column major, which is K row major); 0, the value of an unset uniform, draws
   as before. The time of the frame: libmpv's render API gives none with the frame it draws. mpv sets `time-pos` to a
   frame's time when it hands it over to vo_libmpv (with `video-timing-offset` 0, at its display time), and the next
   frame only at the next display time, so `time-pos` read when mpv says it has a frame is that frame's, unless the draw
   comes after the next frame's display time: then mpv has handed the next one over and `time-pos` is one frame ahead.
   `FrameClock` reads the frame's target time (`mpv_render_context_get_info`, `MPV_RENDER_PARAM_NEXT_FRAME_INFO`) with
   `mpv_get_time_us`: in time, `time-pos`; late, whichever of `time-pos` and the frame before it the last frame drawn in
   time puts nearer (two frames at most, else `time-pos`). The target time is in nanoseconds in mpv `0b7ed670` (the
   pinned build) and in microseconds in mpv 0.35: a target far beyond the clock is taken for nanoseconds. Measured on
   11 November 2026 with clips whose frames carry their index (25, 29.97, 30, 50 and 60 frames a second, H.264 and HEVC,
   audio or none, an audio track starting early), a draw loop copying `DrawProjection` with delays of zero to two and a
   half frames, seeks, a pause, speed 1.5: on Windows with the libmpv and ANGLE of the app (Intel UHD, software and
   d3d11va decoding), 604 of 18052 draws (3.35%) one frame off with `time-pos` alone, 27 (0.15%) with the rule, all
   among the first five draws of a file; on Linux with mpv 0.35, 324 of 14336 and 3. The first draw after a seek has no
   target time and keeps `time-pos`, at most one frame off (31 ms at 25 frames a second). The frame time is read only
   while a table is on, so other videos make no more calls into mpv; a paused frame drawn before the table came takes
   `time-pos` as its time and is drawn again corrected at once. Two players (patch 7): the primary's frame time is read
   with its pending frame (`NotePending`). `ProjectionStats` adds `stabilizedFrames`, `frameTime` and `stabilizing`.
   `PluginRenderer.setStabilization(table, enabled:)` sends the table once, the switch alone afterwards, and `detach`
   takes the table out of the player before it may go to another video. The equirectangular pass is unchanged: a
   stitched file has no table.

15. Windows, the headset view: OpenXR in the plugin, off unless built with the `MEDIA_KIT_360_XR` option of CMake
   (`windows/xr_*.h` and `.cc`, `common/renderer/projection_view.h`, `lib/xr.dart`, `lib/src/xr_headset.dart`; Immuch360
   store/m2/release-48/pc-vr-v1-plugin.md). Experimental: not yet tried in a headset by the project. It began as the
   probe of 14 November 2026 (branch `xr-probe`) and is version 1 since branch `xr-v1`; the C++ namespace keeps the
   probe's name.
   - Renderer C's view takes four tangents (`uTanFov`, left, right, up and down, signed as OpenXR's `XrFovf`) in
     place of the symmetric `uTanHalfFov`, and a rotation built from an eye's orientation (`ProjectionView.eye`,
     `rows`, `tangents`; `projection_view::EyeView`: OpenXR's view space has -z ahead, renderer C's +z, so the eye
     sees the sphere through S R S, S = diag(1, 1, -1)); the window's view gives the same uniforms as before.
     Recentering keeps an anchor (`projection_view::Anchor`): the head's yaw only upright, its yaw and pitch lying
     down (never the roll), the yaw taken from the head's forward and up together so that a head looking straight up
     has one; the sphere (`AnchoredEyeView`), the flat screen's pose and the strip's pose all derive from it. Upright
     it gives the probe's views exactly.
   - `ProjectionRenderer::DrawFrame` draws from another renderer's intermediate texture of the share group. Kind 3 is
     a test pattern drawn with no frame (`kFragmentPattern`: a grey ramp ahead, one coloured patch per eye, a bar
     moving at a stepped time, green up, orange down). Kind 4 (`kFragmentFlat`, in every build) draws a flat film as
     it is, letterboxed with black bars, a stereo film's eye (`uEye`, the left one) at the shape it is seen with
     (`uAspect`, `ProjectionSetup.aspect`, 0 for its shape in the frame); Dart: `ProjectionKind.flat` (its `code`
     is 4: `ProjectionKind` now carries the plugin's numbers), `PluginProjection.flat(layout:, aspect:)`. The flat
     player uses it while its film is in the headset, so the window keeps its normal picture.
   - With the option on, the Khronos loader (`openxr_loader.dll` 1.1.65 of the `OpenXR.Loader` package, Apache 2.0
     OR MIT, SHA-256 checked at build time, nothing downloaded at run time) is bundled next to the executable and
     opened from there by full path at the first view only; the vendored headers (`windows/third_party/openxr`,
     Apache 2.0 OR MIT, unchanged) serve every build. CMake gives the plugin's commit to the log when its folder is
     a git checkout.
   - The method calls: `VideoOutputManager.XrRuntimeInfo` (the active runtime from `XR_RUNTIME_JSON` or
     `HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime` in the 64 bit view, whether its manifest exists, the file name
     only and the runtime guessed from the path; nothing loaded), `XrStart` (content, logPath, matchRefreshRate,
     actionNames, appVersion; returns at once), `XrSetContent` and `XrSetContentRate` (the next video or another mode
     with no new session), `XrSetStrip` (RGBA, at most 1024 x 160, visible), `XrRecenter` (lyingDown), `XrPoll`
     (after: the state and the events after that sequence) and `XrStop` (answered off the platform thread; the thread
     holds a token the plugin's destructor clears first, never the plugin itself, and the destructor ends the view
     before any member goes, so a window closed while the runtime takes its time drops the answer). Without
     the option every call answers `notInBuild`; the Dart side answers `notWindows` on Linux and macOS without a call
     and `notInBuild` without the plugin. Nothing calls into Dart: the app polls (10 times a second while a view
     runs), the queue keeps 64 events, the oldest dropped, numbered on from one view to the next.
   - Portable, with std::thread only (`xr_session_core.h/.cc`, no Windows, Direct3D or OpenGL header, a grep in CI):
     one view at a time on a thread of its own; `Stop` cuts the content source first (no frame of a player is read
     once it began, so the player may go), asks the controller to end and waits within its bound; a view that did not
     end is `runtimeNotAnswering` and the next start answers that until its thread ends; the plugin's teardown
     detaches a thread that does not end (the core is never destroyed, so no joinable thread is left in a static).
     `xr_session.cc` and `xr_runtime_info.cc` keep the Windows parts: the loader's path, the log file, the registry,
     the real runtime and GPU side. The view holds the players' EGL display while it runs
     (`ANGLESurfaceManager::RetainDisplay` and `ReleaseDisplay`, given to `AngleGpu` like the share context): ANGLE
     does not count eglInitialize, so the last player going during a view (an AI job, an idle limit, a test pattern
     started with no player) no longer terminates the display under it; the view's release terminates it when no
     player is left.
   - The controller (`xr_controller.cc`, portable): the GPU side opened, the Basic Render Driver (vendor 0x1414,
     device 0x8c) refused as `gpuTooWeak`, the instance (XR_KHR_D3D11_enable, XR_FB_display_refresh_rate,
     XR_EXT_hp_mixed_reality_controller and XR_BD_controller_interaction when offered), the API layers the loader
     lists logged, up to 60 s waiting for a headset, the adapter's LUID and feature level checked, the session, the
     controllers, the formats; then the session's states (begun once ready, ended once stopping, left once exiting or
     lost) and the frames. xrPollEvent's result goes through the same check as the frame calls, so an instance lost
     while the session is not running (IDLE, before READY or after STOPPING: no frame call then) still ends the view
     with `runtimeLost`. What is shown changes at the next frame (`Inbox`): the eye swapchains are made when first
     needed and kept, the flat screen's at the frame's size and destroyed when the screen is left, the refresh rate
     chosen again when the frame rate changes (a whole multiple where the runtime offers one; matching can be off),
     a new source recentered. Idle mode draws a dark grey projection layer (0.08) with the strip. The flat screen's
     eyes come from the stereo layout and the squeeze the app gives (full or half side by side or over under; no
     guess from the frame's shape): one quad, or one per eye with `eyeVisibility` and its part of the frame, 2.4 m
     wide, 2.5 m ahead of the anchor.
   - The strip: a 1024 x 160 swapchain made at the first image, the app's RGBA bytes uploaded with `glTexSubImage2D`
     only when new and copied into the image with the nearest texel, an opaque quad 0.9 x 0.14 m, 1.4 m away 20
     degrees under the anchor's forward, or 0.15 m under the flat screen; shown only while visible, dropped and
     logged when the runtime's `maxLayerCount` leaves no room.
   - The controllers (`xr_input.h/.cc`, portable): an action set "player" with `play_pause` (float), `seek`
     (vector2f), `seek_press` (trackpads), `recenter`, `exit` and `haptic`, both hands as subaction paths, the
     localized names from the app (English when refused). Bindings suggested per profile from one table
     (`BindingsFor`): simple controller, Oculus Touch, Valve Index, HTC Vive, Microsoft motion controller, and HP
     Mixed Reality, Pico 4 and Neo3 only with their extension; haptics on both hands for every profile; a profile
     the runtime rejects is logged and skipped, with none accepted the view runs and its state says it has no
     controller. `Gestures`: a trigger is pressed above 0.75 and released under 0.35; seek 10 s once the stick passes
     0.7, again every 0.5 s while held, 30 s steps after 2 s, over under 0.3, a trackpad only while clicked;
     recenter on a release before 1 s, lying down toggled at 1 s held; leaving at 1 s held, with haptic pulses; the
     simple controller's select is play or pause when short and recenter when held. Inputs are read only while the
     session is focused, and losing focus resets the gestures (a button held across it fires nothing). Recenter and
     lying down act in the headset at once and are reported; play or pause, seek and leaving are only reported, for
     the app to apply to the window's player. A profile change is logged and reported per hand.
   - Stop codes (`StopReason`, `XrStopReason` in Dart) with an English detail: `stoppedByApp`, `endedFromHeadset`
     (EXITING the app did not ask), `noRuntime` (RUNTIME_UNAVAILABLE or RUNTIME_FAILURE), `loaderMissing`,
     `noHeadset` (none within 60 s), `otherGpu` (LUID), `gpuTooWeak` (feature level, Basic Render Driver),
     `runtimeLost` (instance or session lost, loss pending), `runtimeNotAnswering` (a stop that timed out),
     `alreadyRunning`, `notInBuild`, `notWindows`, `failed`. Leaving VISIBLE for more than 2 s gives a visibility
     event, coming back another. The statistics every 5 s: frames, late frames, draw time, refresh rate, GPU memory
     (a flag above 90 % of the budget), the time taken for the player's frame, strip uploads.
   - Proved without a headset (CI): the portable tests with g++ and ASan and UBSan in the analyze job and with MSVC
     `/W4 /WX` in the `windows-xr` job (the probe's 140 checks, `test/xr_probe_test.cc`, and version 1's,
     `test/xr_v1_test.cc`: 50 start and stop cycles with every handle destroyed once in order, a stop while waiting
     for the headset, during the preparation and while the runtime hangs in xrWaitFrame, a detached teardown, the
     content changes, an instance lost seen by xrPollEvent alone, the six screen cases, the binding table, the gestures with time steps, the recentering, the
     strip, every stop code, the visibility events, the event queue, the runtime guess); on the hosted runner
     (`test/xr/xr_windows_test.cc`) the registry with no runtime and with `XR_RUNTIME_JSON` naming a missing file,
     the bundled loader with no runtime and every action call failing cleanly, the WARP device refused as
     `gpuTooWeak`, the swapchain images taken in place with the bytes written, the strip's 1024 x 160 bytes read back
     unchanged, the display held by the view while a real player is made and disposed and terminated by the view's
     release; the flat pass letterboxing in ANGLE (`test/sphere/plugin_shaders_windows_test.dart`); the Dart calls
     both ways (`test/xr_headset_test.dart`).
   - Waits for a headset (the session of Immuch360 store/m2/release-48/pc-vr-v1-plan.md, section 5): that a real
     runtime takes the plugin's device, orientation and 3D depth, gamma on a real driver, comfort and judder, the
     refresh rates honoured, the bindings on real controllers (SteamVR's binding screen with the localized names),
     the strip's size and place, the app surviving a headset taken off and a runtime killed, GPU memory with an 8K
     video. Not in version 1: the raw files of two players, the stabilisation in the headset, photos, a frame ring or
     fence (the frame is published after the player's glFinish), audio routing.

16. All platforms, renderer C's portable files in one folder (`common/renderer/projection_renderer.{h,cc}`,
   `projection_shaders.h`, `projection_view.h`, `stabilization.h` and `test/`, `windows/CMakeLists.txt`,
   `windows/test/xr/CMakeLists.txt`, 22 November 2026; Immuch360 desktop parity plan, lot L12). The files moved from
   `windows/` unchanged but for their comments: they include only EGL, OpenGL ES 3.0 and the C++17 library. Windows
   builds `projection_renderer.cc` from there, with the folder on its include path, so the plugin's Windows build and
   behaviour do not change. The Linux plugin does not build them yet (its context is OpenGL ES 2.0, patch 10); the CI
   compiles `projection_renderer.cc` against Mesa's EGL and GLES 3 headers on Ubuntu with every g++ warning an error,
   and runs `test/stabilization_test.cc` and `test/projection_view_test.cc` (the window's view, the eye's view, the
   uniforms as OpenGL reads them) there. The SwiftPM package of macOS reads only its own folder: a macOS target will
   need these files reached from `macos/media_kit_video/` (not done here).

18. macOS, the Spatial 2.5D passes in Metal (`macos/media_kit_video/Sources/media_kit_video/plugin/spatial/`:
   `SpatialMetalRenderer.swift`, `SpatialMetalOutput.swift`, `SpatialMetalShaders.swift`; `plugin/common/VideoOutput.swift`,
   `plugin/common/VideoOutputManager.swift`, `plugin/common/MediaKitVideoPlugin.swift`, `plugin/TextureHW.swift`;
   `macos/spatial/SpatialShaders.metal` and `embed_shaders.py`; `macos/test/`). The calls of patch 17
   (`VideoOutputManager.SetSpatial`, `SetViewpoint`, `SetSpatialDebug`, `SpatialStats`, and `SetView` for an
   equirectangular video) answered on macOS, where renderer C does not exist: the renderer of the app's iOS Spatial
   player (`mobile/ios/Runner/Spatial/SpatialRenderer.swift` of github.com/freeKC/Immuch360, which imports Metal and
   no UIKit) with its shaders word for word (`SpatialShaders.metal`, the SHA-256 in `SpatialMetalShaders.swift`),
   drawing into a texture instead of an MTKView. mpv draws each frame at the frame size of the setup (the video's
   shape, at most `maxFrameWidth` wide and `maxFramePixels` pixels) into the pixel buffers of `TextureHW` (OpenGL;
   it then finishes the frame with `glFinish` before returning, since Metal reads the same IOSurface) or of
   `TextureSW` (a Mac without OpenGL, such as a virtual machine); `SpatialMetalOutput` reads the newest one as a Metal
   texture through `CVMetalTextureCache` (no copy; a buffer that is no IOSurface is copied), runs the passes (eyes,
   grey, 7 x 7 block matching both ways, the left right check, the guided blur, the blend over time, the synthesis;
   for an equirectangular video the view of each eye first, VR180 black behind), and draws the view into one of
   three IOSurface pixel buffers of its own, which the texture registered with Flutter (`SpatialSwitchTexture`, so
   the texture id stays the VideoController's) then shows. The output takes the shape of one eye inside the box
   Dart gives, an equirectangular one the box, at most `maxOutputHeight` lines, as on Windows; Flutter is told the
   output's size. The tiers and the adaptive rule are the phones' (Metal's GPU time of each command buffer); a new
   viewpoint, view or debug switch draws the frame kept again on the output's worker thread, without mpv, merged
   when they come faster than the passes draw, and the first redraw of a still frame runs the estimation three more
   times on the same eyes (the settling). `SpatialStats` gives the map of Windows (the device's name in
   `glRenderer`, the probe's five pixels at renderer C's places and the middle row). The shaders are compiled from
   source when the passes are first turned on (`makeLibrary(source:)`), so that neither Swift Package Manager nor
   CocoaPods builds a `.metal` file; a pass that does not compile leaves the left eye shown, a device without Metal
   refuses the setup with the reason. Checked in CI on a Mac runner (`macos` job of `.github/workflows/ci.yml`): the
   plugin's Swift type checked against the part of FlutterMacOS it uses (`macos/test/typecheck/FlutterMacOS.swift`)
   and mpv's headers, the embedded shaders equal to `SpatialShaders.metal`, and the passes run on the runner's Metal
   device (`macos/test/spatial/main.swift`: the views at p = 0, 0.5 and 1 of a random dot pair, the copy path, the
   top bottom and swapped layouts, the output size and the probe, a VR180 pair ahead and behind, the time of a
   1920 x 1080 pair). Not tried on a Mac with a real GPU, nor in a built app with mpv's frames.

Not done yet (plan 20-plan-desktop.md, 2.2): the D3D11 device on the adapter Flutter uses. It is not needed on a
laptop with two GPUs: Flutter's ANGLE, the plugin's device and display and mpv's decoder all take the adapter DXGI
lists first, which Windows' graphics preference for the app and the runner's NvOptimusEnablement and
AmdPowerXpressRequestHighPerformance exports (`mobile/windows/runner/main.cpp`, 2026-11-02) choose for the whole
process, so the three never differ (measured with both cards of the owner's laptop).

17. Windows, the Spatial 2.5D passes in renderer C (`common/renderer/spatial_renderer.{h,cc}`,
   `common/renderer/spatial_shaders.h`, `common/renderer/projection_renderer.h`, `windows/video_output.{h,cc}`, `windows/video_output_manager.{h,cc}`,
   `windows/media_kit_video_plugin.cc`, `windows/CMakeLists.txt`, `lib/src/spatial_output.dart`, exported by
   `lib/media_kit_video.dart`). The Spatial 2.5D player of the phones (a stereo video seen from a viewpoint between its
   two cameras, which the head of the person in front of the screen moves) on Windows, where decision DP1 of
   2026-10-09 put it: a projection may carry a Spatial setup (`VideoOutputManager.SetSpatial`: where the eyes are, in
   the order of the phones' EyeLayout, a flat or an equirectangular eye, VR180, the tier and whether the adaptive rule
   may change it). mpv then draws each frame into renderer C's intermediate texture, as for a 360 video, and the passes
   of the phones' `SpatialRenderer.kt` draw the output from it: each eye into its own texture (the flat eye pass of
   `SpatialShaders.kt` reading the intermediate texture as a `sampler2D`; an equirectangular eye through renderer C's
   own pass, with a main that writes in the alpha whether a VR180 eye covers the direction), both eyes in grey at the
   disparity size, the 7 x 7 block matching both ways, the left right check and the temporal blend with the previous
   map of a ping pong pair, the blur, and the synthesis at the viewpoint. The GLSL bodies are the text of
   `SpatialShaders.kt` but for the eye's sampler: the app's `mobile/test/desktop/video/spatial_shaders_parity_test.dart`
   fails when one differs, and when a constant or a tier of `SpatialRenderer.kt` differs in the C++ (it lives in the
   app, which holds the Kotlin side). Every texture keeps its
   first row at the top, as renderer C's do (the phones keep it at the bottom); the passes after the eyes work along
   the rows and do not notice. The tiers (LOW 320 x 180 every 4 new frames without the check, MEDIUM 480 x 270 every
   2, HIGH 640 x 360 every frame), the adaptive rule (down after a second over 12 ms, up after 5 seconds under 6 ms, a
   tier left for slowness out of reach for a minute; the GPU time from `GL_EXT_disjoint_timer_query` when ANGLE lists
   it, else one draw in 15 timed on the CPU around `glFinish`) and the settling (four more estimations once the eyes
   have not changed for 100 ms) are those of `SpatialRenderer.kt`; the plugin draws on events rather than at the
   screen's rate, so the settling runs are asked for by a timer of their own (`PairTimer`), at most every 16 ms. A flat
   output takes the shape of one eye as it shows (a squeezed stereo frame shows its eyes at the frame's shape) inside
   the box Dart gives; an equirectangular output is the view (`SetView`, as for a 360 video). The viewpoint
   (`VideoOutputManager.SetViewpoint`) and the disparity map shown instead of the view (`SetSpatialDebug`) are kept and
   merged as views are, and a paused video is redrawn from the frame kept without mpv; `SpatialStats` adds to
   `ProjectionStats` the figures of the passes' last second, the tier, the sizes of the eyes and maps, the counts since
   the last read (draws, estimations, settling runs, tier changes) and, with the probe, the middle row of the output.
   Every program is compiled when the setup is set, the disparity pass's loops included: a driver that refuses one
   refuses the setup with the shader's log, and the player is flat. Re-landed on patches 14 to 16 for
   Immuch360 release 53 (first written for the app's own copy of the plugin in October 2026): the files sit in
   `common/renderer` beside the other portable files of renderer C (patch 16, none of them Windows specific), and the
   equirectangular eye takes the view of patch 15 (`projection_view::UniformsOf`: `uViewToSphere` and the four
   tangents `uTanFov`) instead of the former `uTanHalfFov`; the stabilisation of patch 14 applies to the plain 360
   pass only, not to the stereo pairs of these passes. Checked without a window by
   `test/spatial/spatial_shaders_windows_test.dart` (the programs as the header assembles them, in ANGLE's ES 3.0 on
   the Windows runner's WARP, and on Mesa's llvmpipe with `MEDIA_KIT_360_GLES=mesa` on the Linux runner: the disparity
   of a random dot pair found to a pixel both ways, the view at p = 0, 1 and 0.5, the eyes' orientation and VR180
   alpha), and `test/spatial/spatial_output_test.dart` (the calls on the channel). The test of a built app (a side by
   side clip through the plugin, `SpatialStats` and its probe) stays in the app, once its pin moves. Not measured yet on
   a GPU, nor driven by the head tracker.

Not done yet (plan 20-plan-desktop.md, 2.2): the D3D11 device on the adapter Flutter uses, only if spike 6 shows a
mismatch on a hybrid GPU machine.
