// Immuch360: the headset view (IMMUCH360-NOTE.md, patch 15), the small
// interface the plugin calls, its Windows parts only: the loader's path next
// to the executable, the log file, the active runtime read from the registry,
// the real runtime (xr_openxr_runtime.h) and GPU side (xr_angle_gpu.h). What
// holds a view (one at a time, its thread, the bounded stop, the commands and
// the polled state) is the portable xr_session_core.h. Compiled only with the
// MEDIA_KIT_360_XR option of CMake; nothing of it runs, and no OpenXR file is
// opened, until |Start| (|ActiveRuntime| reads the registry only).

#ifndef XR_SESSION_H_
#define XR_SESSION_H_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "projection_renderer.h"
#include "xr_runtime_info.h"
#include "xr_session_core.h"

namespace xr_session {

// The frame of the player |handle|, false once it is gone; called from the
// view's thread, never after |Stop| began
using FrameSource =
    std::function<bool(int64_t handle, ProjectionFrameSnapshot*)>;

xr_probe::StartAnswer Start(const xr_probe::StartRequest& request,
                            FrameSource source);

// Ends the view and waits up to |wait_ms| for its thread; true when it ended.
bool Stop(int wait_ms = 4000);

void SetContent(const xr_probe::ContentRequest& content);
void SetContentRate(double fps);
void SetStrip(std::vector<uint8_t> rgba, uint32_t width, uint32_t height,
              bool visible);
void Recenter(bool lying_down);
xr_probe::PollAnswer Poll(uint64_t after);

// The plugin going away: a bounded stop, then a thread that did not end is
// detached
void Teardown(int wait_ms);

}  // namespace xr_session

#endif  // XR_SESSION_H_
