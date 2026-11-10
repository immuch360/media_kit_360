# media_kit_360 example

A Flutter desktop app that plays a 360°, VR180 or 3D video through renderer C.

- **Open** a file with the folder button, or type a path or a URL and press Enter. The first argument of the command
  line opens at the start.
- **Drag** the picture to turn the view, turn the mouse wheel to zoom, and press the target button to look ahead again.
- **Pick the projection** in the list: 360°, 360° 3D over under or side by side, VR180 side by side, or 180°.

The line at the bottom says which tier draws the view and on which GPU, then what the renderer probe measured over
the first seconds. On Linux the video plays flat, with the reason.

From the root of the repository:

```sh
flutter pub get
cd example
flutter run -d windows   # or: flutter run -d linux
```

On Linux, install libmpv and its headers first (`sudo apt install libmpv-dev` on Debian and Ubuntu).
