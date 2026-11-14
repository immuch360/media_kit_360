// Immuch360: the headset probe (IMMUCH360-NOTE.md, patch 15), the small
// interface the plugin calls. Compiled only with the MEDIA_KIT_360_XR option
// of CMake; nothing of it runs, and no OpenXR file is opened, until |Start|.
//
// One probe at a time, on a thread of its own: |Start| returns at once, the
// thread opens the loader and the runtime, waits for a headset and runs the
// session (xr_controller.h); |Stop| asks the runtime to end it and waits a
// few seconds for the thread. Every step goes to the log file given to
// |Start|, which is what FreeKC sends back after a session with a headset.

#ifndef XR_SESSION_H_
#define XR_SESSION_H_

#include <EGL/egl.h>

#include <cstdint>
#include <functional>
#include <string>

#include "projection_renderer.h"
#include "xr_controller.h"

namespace xr_session {

struct StartOptions {
  xr_probe::Mode mode = xr_probe::Mode::kPattern;
  xr_probe::ScreenStereo stereo = xr_probe::ScreenStereo::kMono;
  double fps = 0.0;
  double yaw = 0.0;
  // UTF-8; empty: no log file, the lines go to the standard output only
  std::string log_path;
  // A description of what is shown, for the log (the app gives the file's
  // name or kind, never a URL or a token)
  std::string label;
};

// The frame of the player shown, false once it is gone (the player
// disposed); called from the probe's thread
using FrameSource = std::function<bool(ProjectionFrameSnapshot*)>;

struct State {
  bool running = false;
  xr_probe::Status status;
  std::string log_path;
};

// False with |reason| when a probe already runs.
bool Start(const StartOptions& options, FrameSource source,
           std::string* reason);

// Ends the probe and waits up to |wait_ms| for its thread; true when it ended.
bool Stop(int wait_ms = 4000);

void Recenter();

State Now();

}  // namespace xr_session

#endif  // XR_SESSION_H_
