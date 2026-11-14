// Immuch360: the headset probe (IMMUCH360-NOTE.md, patch 15), the session and
// its frame loop, on a thread of their own, over the two interfaces of
// xr_runtime.h. What it does, in order: opens the GPU side (the plugin's
// ANGLE device), creates the OpenXR instance and waits for a headset, checks
// that the runtime renders on the plugin's graphics card, creates the session
// and the swapchains, then runs the session's states (begin once ready, end
// once stopping, leave once exiting or lost) and, while the session runs, the
// frames: wait, begin, locate the eyes, draw each eye through renderer C (or
// the frame as a flat screen on quad layers), end. Every step is written to
// the probe's log, which FreeKC sends back after a session with a headset.
// Nothing here knows Windows: the tests drive it with a fake runtime and a
// fake GPU (test/xr_probe_test.cc).

#ifndef XR_CONTROLLER_H_
#define XR_CONTROLLER_H_

#include <atomic>
#include <functional>
#include <mutex>
#include <string>

#include "xr_runtime.h"

namespace xr_probe {

enum class Mode {
  // A 360 or 180 video (any layout of renderer C) around the head
  kSphere,
  // The frame as a flat screen ahead, 2D or 3D (quad layers, one per eye)
  kScreen,
  // The test pattern of renderer C (kind 3): grey ramp, one patch per eye,
  // a bar moving at |Options.fps|
  kPattern,
};

enum class ScreenStereo { kMono, kLeftRight, kTopBottom };

struct Options {
  Mode mode = Mode::kPattern;
  ScreenStereo stereo = ScreenStereo::kMono;
  // The content's frame rate (the pattern's steps), 0 when unknown: the
  // refresh rate asked is a multiple of it where the runtime has one
  double fps = 0.0;
  // The window's yaw when the headset view started: what was ahead in the
  // window is ahead in the headset
  double yaw = 0.0;
  // How long to wait for a headset (a Quest not linked yet)
  int system_wait_seconds = 60;
  // Seconds between two lines of statistics
  double stats_every = 5.0;
  std::function<double()> now;
  std::function<void(int)> sleep_ms;
  std::function<void(const std::string&)> log;
};

struct Status {
  // "starting", "waiting for the headset", "running", "stopped"
  std::string phase = "starting";
  std::string session_state = "unknown";
  std::string reason;
  std::string runtime;
  std::string system;
  std::string swapchain_path;
  int64_t frames = 0;
  int64_t frames_shown = 0;
  bool instance_lost = false;
};

class Controller {
 public:
  Controller(Runtime* runtime, Gpu* gpu, Options options);

  // The whole probe, until the session ends, is lost or |RequestStop|; on the
  // caller's thread.
  void Run();

  // From any thread: asks the runtime to end the session (xrRequestExitSession),
  // and leaves at once if it does not run.
  void RequestStop() { stop_ = true; }
  // From any thread: what is ahead now becomes the centre (the yaw only).
  void RequestRecenter() { recenter_ = true; }

  Status StatusNow();

 private:
  void Log(const std::string& line);
  void SetPhase(const std::string& phase);
  void Fail(const std::string& reason);
  bool Check(XrResult result, const char* what);
  bool Prepare();
  bool PrepareEyes();
  void HandleEvent(const Event& event, bool* done);
  void Frame();
  void DrawEyes(const FrameState& frame, const XrView views[2], Layers* layers);
  void DrawScreen(Layers* layers);
  void Recenter(const XrView views[2]);
  void ChooseRefreshRate();
  void Stats(double now);
  void CleanUp();

  Runtime* runtime_;
  Gpu* gpu_;
  Options options_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> recenter_{true};
  std::mutex status_mutex_;
  Status status_;

  GpuInfo gpu_info_;
  RuntimeInfo runtime_info_;
  SystemInfo system_;
  SessionStateMachine machine_;
  bool instance_ = false;
  bool session_ = false;
  bool lost_ = false;
  bool exit_requested_ = false;
  double exit_requested_at_ = 0.0;
  bool refresh_known_ = false;
  Swapchain eyes_[2];
  bool eyes_wrapped_[2] = {false, false};
  Swapchain screen_;
  bool screen_wrapped_ = false;
  uint64_t screen_frame_count_ = 0;
  bool screen_drawn_ = false;
  double yaw_offset_ = 0.0;
  XrPosef screen_pose_{};
  int64_t format_ = 0;
  XrTime first_time_ = 0;
  XrTime last_time_ = 0;

  // Statistics since the last line
  double stats_at_ = 0.0;
  int64_t window_frames_ = 0;
  int64_t window_shown_ = 0;
  int64_t window_late_ = 0;
  double window_draw_ms_ = 0.0;
  double window_draw_max_ms_ = 0.0;
  bool finish_due_ = true;
  std::string last_error_;
  int64_t repeated_errors_ = 0;
};

}  // namespace xr_probe

#endif  // XR_CONTROLLER_H_
