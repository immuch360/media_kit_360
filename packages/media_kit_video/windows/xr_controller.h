// Immuch360: the headset view (IMMUCH360-NOTE.md, patch 15), the session and
// its frame loop, on a thread of their own, over the two interfaces of
// xr_runtime.h. What it does, in order: opens the GPU side (the plugin's
// ANGLE device), creates the OpenXR instance and waits for a headset, checks
// that the runtime renders on the plugin's graphics card, creates the session,
// its controllers (xr_input.h), then runs the session's states (begin once
// ready, end once stopping, leave once exiting or lost) and, while the
// session runs, the frames: wait, begin, read the controllers, locate the
// eyes, draw each eye through renderer C (or the frame as a flat screen on
// quad layers), the strip, end. What is shown changes at any time with no new
// session (|Inbox::SetContent|): the eye swapchains are made when first needed
// and kept, the screen's per frame size and gone once the screen is left.
// Every step is written to the view's log. Nothing here knows Windows: the
// tests drive it with a fake runtime and a fake GPU.

#ifndef XR_CONTROLLER_H_
#define XR_CONTROLLER_H_

#include <functional>
#include <map>
#include <memory>
#include <string>

#include "projection_view.h"
#include "xr_input.h"
#include "xr_runtime.h"
#include "xr_session_core.h"

namespace xr_probe {

struct Options {
  // What is shown first when the inbox has no content yet
  ContentRequest content = [] {
    ContentRequest first;
    first.mode = Mode::kPattern;
    return first;
  }();
  bool match_refresh_rate = true;
  std::map<std::string, std::string> action_names;
  // How long to wait for a headset (a Quest not linked yet)
  int system_wait_seconds = 60;
  // Seconds between two lines of statistics
  double stats_every = 5.0;
  // How long the runtime has to end the session once the app asks
  double stop_bound_seconds = 3.0;
  std::function<double()> now;
  std::function<void(int)> sleep_ms;
  std::function<void(const std::string&)> log;
};

// The strip: 1024 x 160 pixels on a quad 0.9 m x 0.14 m, 1.4 m from the eyes
// 20 degrees under the anchor's forward, or 0.15 m under a flat screen
constexpr uint32_t kStripWidth = 1024;
constexpr uint32_t kStripHeight = 160;
constexpr float kStripMetresWide = 0.9f;
constexpr float kStripMetresHigh = 0.14f;
constexpr double kStripDistance = 1.4;
constexpr double kStripDown = 20.0;
constexpr double kStripUnderScreen = 0.15;
// The flat screen: 2.4 m wide, 2.5 m ahead (about 51 degrees)
constexpr float kScreenWidth = 2.4f;
constexpr double kScreenDistance = 2.5;
// The idle mode's room
constexpr float kIdleGrey = 0.08f;

class Controller {
 public:
  // |inbox| and |board| shared with the session core; the controller makes
  // its own when null (the tests of the probe)
  Controller(Runtime* runtime, Gpu* gpu, Options options,
             Inbox* inbox = nullptr, Board* board = nullptr);

  // The whole view, until the session ends, is lost or is stopped; on the
  // caller's thread.
  void Run();

  // From any thread, the probe's calls: through the inbox
  void RequestStop() { inbox_->Stop(); }
  void RequestRecenter() { inbox_->Recenter(false); }
  Status StatusNow() { return board_->status(); }
  Inbox* inbox() { return inbox_; }
  Board* board() { return board_; }

 private:
  void Log(const std::string& line);
  void SetPhase(const std::string& phase);
  void Fail(StopReason reason, const std::string& detail);
  bool StopAsked();
  bool Check(XrResult result, const char* what);
  bool Prepare();
  bool EnsureEyes();
  void HandleEvent(const Event& event, bool* done);
  void TakeCommands();
  void ApplyContent(const ContentRequest& next);
  void Frame();
  void ApplyFired(const std::vector<Fired>& fired);
  void PushEvent(HeadsetEvent event);
  void DrawEyes(const FrameState& frame, const XrView views[2], Layers* layers);
  void DrawScreen(Layers* layers);
  void AddStrip(Layers* layers);
  void DestroyScreen();
  void Recenter(const XrView views[2]);
  void ChooseRefreshRate();
  void Stats(double now);
  void CleanUp();

  Runtime* runtime_;
  Gpu* gpu_;
  Options options_;
  std::unique_ptr<Inbox> own_inbox_;
  std::unique_ptr<Board> own_board_;
  Inbox* inbox_;
  Board* board_;
  Input input_;

  GpuInfo gpu_info_;
  RuntimeInfo runtime_info_;
  SystemInfo system_;
  SessionStateMachine machine_;
  ContentRequest content_;
  bool instance_ = false;
  bool session_ = false;
  bool lost_ = false;
  bool done_ = false;
  bool exit_requested_ = false;
  double exit_requested_at_ = 0.0;
  bool refresh_known_ = false;
  double start_ = 0.0;
  VisibilityWatch visibility_;

  Swapchain eyes_[2];
  bool eyes_made_ = false;
  bool eyes_wrapped_[2] = {false, false};
  Swapchain screen_;
  bool screen_wrapped_ = false;
  uint64_t screen_frame_count_ = 0;
  bool screen_drawn_ = false;
  double screen_height_ = 0.0;
  Swapchain strip_;
  bool strip_wrapped_ = false;
  bool strip_failed_ = false;
  bool strip_drawn_ = false;
  bool strip_room_logged_ = false;
  bool strip_size_logged_ = false;
  StripImage strip_image_;
  bool strip_visible_ = false;
  uint64_t strip_uploaded_ = 0;
  XrRect2Di strip_rect_{};

  bool recenter_ = true;
  bool lying_down_ = false;
  projection_view::Anchor anchor_;
  double head_centre_[3] = {0.0, 0.0, 0.0};
  XrPosef screen_pose_{};
  int64_t format_ = 0;
  XrTime first_time_ = 0;
  XrTime last_time_ = 0;
  uint64_t events_dropped_ = 0;

  // Statistics since the last line
  double stats_at_ = 0.0;
  int64_t window_frames_ = 0;
  int64_t window_shown_ = 0;
  int64_t window_late_ = 0;
  int64_t window_strip_uploads_ = 0;
  double window_draw_ms_ = 0.0;
  double window_draw_max_ms_ = 0.0;
  bool finish_due_ = true;
  bool memory_tight_ = false;
  std::string last_error_;
  int64_t repeated_errors_ = 0;
};

}  // namespace xr_probe

#endif  // XR_CONTROLLER_H_
