// Immuch360: the headset probe's session and frame loop (IMMUCH360-NOTE.md,
// patch 15); see the header.

#include "xr_controller.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>
#include <thread>

#include "projection_view.h"

namespace xr_probe {

namespace {

constexpr double kNanoseconds = 1e9;
// The screen of a flat film: 2.4 m wide, 2.5 m ahead (about 51 degrees)
constexpr float kScreenWidth = 2.4f;
constexpr float kScreenDistance = 2.5f;
// The screen's swapchain at most this wide (memory: 4096 x 2160 x 4 B x 3
// images is 106 MB)
constexpr uint32_t kScreenMaxWidth = 4096;

std::string Hex(uint64_t value) {
  std::ostringstream out;
  out << "0x" << std::hex << value;
  return out.str();
}

std::string Join(const std::vector<std::string>& items) {
  std::string out;
  for (const auto& item : items) {
    if (!out.empty()) out += ", ";
    out += item;
  }
  return out.empty() ? "none" : out;
}

std::string Version(uint64_t version) {
  return std::to_string(XR_VERSION_MAJOR(version)) + "." +
         std::to_string(XR_VERSION_MINOR(version)) + "." +
         std::to_string(XR_VERSION_PATCH(version));
}

std::string Fixed(double value, int digits = 2) {
  std::ostringstream out;
  out.setf(std::ios::fixed);
  out.precision(digits);
  out << value;
  return out.str();
}

projection_view::Quaternion QuaternionOf(const XrQuaternionf& q) {
  return {q.x, q.y, q.z, q.w};
}

}  // namespace

Controller::Controller(Runtime* runtime, Gpu* gpu, Options options)
    : runtime_(runtime), gpu_(gpu), options_(std::move(options)) {
  if (!options_.now) {
    options_.now = [] {
      return std::chrono::duration<double>(
                 std::chrono::steady_clock::now().time_since_epoch())
          .count();
    };
  }
  if (!options_.sleep_ms) {
    options_.sleep_ms = [](int ms) {
      std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    };
  }
}

Status Controller::StatusNow() {
  std::lock_guard<std::mutex> lock(status_mutex_);
  return status_;
}

void Controller::Log(const std::string& line) {
  if (options_.log) {
    options_.log(line);
  }
}

void Controller::SetPhase(const std::string& phase) {
  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.phase = phase;
  }
  Log("phase: " + phase);
}

void Controller::Fail(const std::string& reason) {
  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    if (status_.reason.empty()) {
      status_.reason = reason;
    }
  }
  Log("stopped: " + reason);
}

bool Controller::Check(XrResult result, const char* what) {
  if (XR_SUCCEEDED(result)) {
    return true;
  }
  const auto error = std::string(what) + ": " + runtime_->ResultName(result);
  // The same error at every frame is written once, then counted
  if (error == last_error_) {
    repeated_errors_++;
  } else {
    if (repeated_errors_ > 0) {
      Log("(the previous error " + std::to_string(repeated_errors_) +
          " more times)");
    }
    repeated_errors_ = 0;
    last_error_ = error;
    Log("error: " + error);
  }
  if (result == XR_ERROR_INSTANCE_LOST || result == XR_ERROR_SESSION_LOST) {
    lost_ = true;
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.instance_lost = result == XR_ERROR_INSTANCE_LOST;
  }
  return false;
}

bool Controller::Prepare() {
  std::string error;
  if (!gpu_->Open(&gpu_info_, &error)) {
    Fail("graphics: " + error);
    return false;
  }
  Log("gpu: " + gpu_info_.adapter_name + ", LUID " +
      Hex(static_cast<uint32_t>(gpu_info_.adapter.high)) + ":" +
      Hex(gpu_info_.adapter.low) + ", feature level " +
      Hex(static_cast<uint32_t>(gpu_info_.feature_level)) + ", " +
      std::to_string(gpu_info_.dedicated_video_memory >> 20) +
      " MB dedicated, " + gpu_info_.gl_renderer +
      ", multithread protection before the probe: " +
      (gpu_info_.was_multithread_protected ? "on" : "off") +
      ", shares the players' textures: " +
      (gpu_info_.shares_players ? "yes" : "no"));
  const auto created = runtime_->CreateInstance(
      {"XR_FB_display_refresh_rate"}, &runtime_info_);
  if (!XR_SUCCEEDED(created)) {
    const bool none = created == XR_ERROR_RUNTIME_UNAVAILABLE ||
                      created == XR_ERROR_RUNTIME_FAILURE ||
                      created == XR_ERROR_INSTANCE_LOST;
    Fail(std::string(none ? "no OpenXR runtime is active on this computer"
                          : "the OpenXR instance") +
         " (" + runtime_->ResultName(created) + ")");
    return false;
  }
  instance_ = true;
  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.runtime = runtime_info_.name + " " + Version(runtime_info_.version);
  }
  Log("runtime: " + runtime_info_.name + " " + Version(runtime_info_.version));
  Log("runtime extensions: " + Join(runtime_info_.offered));
  Log("enabled: " + Join(runtime_info_.enabled));
  const double waited_from = options_.now();
  for (;;) {
    const auto result = runtime_->GetSystem(&system_);
    if (XR_SUCCEEDED(result)) {
      break;
    }
    if (result != XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
      Check(result, "xrGetSystem");
      Fail("no headset: " + last_error_);
      return false;
    }
    if (stop_ || options_.now() - waited_from > options_.system_wait_seconds) {
      Fail("no headset connected to the runtime");
      return false;
    }
    if (StatusNow().phase != "waiting for the headset") {
      SetPhase("waiting for the headset");
    }
    options_.sleep_ms(1000);
  }
  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.system = system_.name;
  }
  std::string blends;
  for (const auto mode : system_.blend_modes) {
    blends += (blends.empty() ? "" : ", ") + std::to_string(mode);
  }
  Log("system: " + system_.name + ", vendor " + Hex(system_.vendor_id) +
      ", eyes " + std::to_string(system_.eye_width[0]) + " x " +
      std::to_string(system_.eye_height[0]) + " recommended (max " +
      std::to_string(system_.eye_max_width[0]) + " x " +
      std::to_string(system_.eye_max_height[0]) + "), swapchains up to " +
      std::to_string(system_.max_swapchain_width) + " x " +
      std::to_string(system_.max_swapchain_height) + ", " +
      std::to_string(system_.max_layers) + " layers, tracking " +
      (system_.orientation_tracking ? "orientation" : "no orientation") +
      (system_.position_tracking ? " and position" : "") + ", blend modes " +
      blends);
  const auto verdict =
      CheckAdapter(gpu_info_.adapter, gpu_info_.feature_level, system_.adapter,
                   system_.min_feature_level);
  Log("adapter: runtime LUID " +
      Hex(static_cast<uint32_t>(system_.adapter.high)) + ":" +
      Hex(system_.adapter.low) + ", minimum feature level " +
      Hex(static_cast<uint32_t>(system_.min_feature_level)) + ": " +
      AdapterVerdictName(verdict));
  if (verdict != AdapterVerdict::kSame) {
    Fail(AdapterVerdictName(verdict));
    return false;
  }
  if (!Check(runtime_->CreateSession(gpu_info_.device), "xrCreateSession")) {
    Fail("the session: " + last_error_);
    return false;
  }
  session_ = true;
  std::vector<int64_t> formats;
  if (!Check(runtime_->SwapchainFormats(&formats),
             "xrEnumerateSwapchainFormats")) {
    Fail("the swapchain formats: " + last_error_);
    return false;
  }
  std::vector<std::string> names;
  for (const auto format : formats) names.push_back(DxgiFormatName(format));
  format_ = ChooseSwapchainFormat(formats);
  Log("swapchain formats offered: " + Join(names) + "; chosen: " +
      (format_ == 0 ? "none" : DxgiFormatName(format_)));
  if (format_ == 0) {
    Fail("no 8 bit RGBA swapchain format");
    return false;
  }
  if (!IsSrgb(format_)) {
    Log("warning: no sRGB swapchain: the runtime takes the values as linear, "
        "the picture will look too dark");
  }
  return options_.mode == Mode::kScreen || PrepareEyes();
}

bool Controller::PrepareEyes() {
  for (int eye = 0; eye < 2; eye++) {
    const auto width = std::max<uint32_t>(16, system_.eye_width[eye]);
    const auto height = std::max<uint32_t>(16, system_.eye_height[eye]);
    if (!Check(runtime_->CreateSwapchain(format_, width, height, &eyes_[eye]),
               "xrCreateSwapchain")) {
      Fail("the eye swapchains: " + last_error_);
      return false;
    }
    std::string path, error;
    if (!gpu_->WrapImages(eyes_[eye], &path, &error)) {
      Fail("the eye images: " + error);
      return false;
    }
    eyes_wrapped_[eye] = true;
    {
      std::lock_guard<std::mutex> lock(status_mutex_);
      status_.swapchain_path = path;
    }
    Log("eye " + std::to_string(eye) + ": " + std::to_string(width) + " x " +
        std::to_string(height) + ", " +
        std::to_string(eyes_[eye].images.size()) + " images, " + path);
  }
  return true;
}

void Controller::Run() {
  stats_at_ = options_.now();
  if (Prepare()) {
    SetPhase("running");
    bool done = false;
    while (!done && !lost_) {
      if (stop_ && !exit_requested_) {
        exit_requested_ = true;
        exit_requested_at_ = options_.now();
        if (machine_.running()) {
          Log("stop asked: xrRequestExitSession");
          Check(runtime_->RequestExitSession(), "xrRequestExitSession");
        } else {
          Log("stop asked");
          break;
        }
      }
      // A runtime that never says EXITING after the request: leave anyway
      if (exit_requested_ && options_.now() - exit_requested_at_ > 3.0) {
        Log("the runtime did not end the session within 3 s: leaving");
        break;
      }
      Event event;
      while (!done && runtime_->PollEvent(&event)) {
        HandleEvent(event, &done);
      }
      if (done || lost_) {
        break;
      }
      if (!machine_.running()) {
        options_.sleep_ms(20);
        continue;
      }
      Frame();
    }
    if (lost_ && StatusNow().reason.empty()) {
      Fail("lost: " + last_error_);
    }
  }
  CleanUp();
  if (StatusNow().reason.empty()) {
    Fail(stop_ ? "stopped by the app" : "the session ended");
  }
  SetPhase("stopped");
}

void Controller::HandleEvent(const Event& event, bool* done) {
  switch (event.type) {
    case Event::Type::kSessionState: {
      const auto action = machine_.On(event.state);
      {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.session_state = SessionStateName(event.state);
      }
      Log("session state: " + SessionStateName(event.state));
      if (action == SessionAction::kBegin) {
        if (Check(runtime_->BeginSession(), "xrBeginSession")) {
          machine_.Began();
          recenter_ = true;
          ChooseRefreshRate();
        } else {
          *done = true;
        }
      } else if (action == SessionAction::kEnd) {
        Check(runtime_->EndSession(), "xrEndSession");
        machine_.Ended();
        if (exit_requested_) {
          *done = true;
        }
      } else if (action == SessionAction::kExit) {
        if (event.state == XR_SESSION_STATE_LOSS_PENDING) {
          Fail("the runtime lost the session (loss pending)");
        }
        *done = true;
      }
      break;
    }
    case Event::Type::kInstanceLossPending:
      Log("instance loss pending (the runtime stops or updates)");
      Fail("the runtime is going away (instance loss pending)");
      {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.instance_lost = true;
      }
      *done = true;
      break;
    case Event::Type::kReferenceSpaceChange:
      Log("reference space change pending: recentering");
      recenter_ = true;
      break;
    case Event::Type::kEventsLost:
      Log("events lost: " + std::to_string(event.lost));
      break;
    case Event::Type::kRefreshRateChanged:
      Log("refresh rate changed: " + Fixed(event.from_rate) + " to " +
          Fixed(event.to_rate) + " Hz");
      break;
    case Event::Type::kOther:
      Log("event " + std::to_string(static_cast<int>(event.other)));
      break;
  }
}

void Controller::ChooseRefreshRate() {
  std::vector<float> rates;
  float current = 0.0f;
  const auto result = runtime_->RefreshRates(&rates, &current);
  if (!XR_SUCCEEDED(result)) {
    if (!refresh_known_) {
      Log("refresh rates: not offered (" + runtime_->ResultName(result) + ")");
    }
    refresh_known_ = true;
    return;
  }
  std::string listed;
  for (const auto rate : rates) {
    listed += (listed.empty() ? "" : ", ") + Fixed(rate);
  }
  const auto wanted = xr_probe::ChooseRefreshRate(rates, current, options_.fps);
  Log("refresh rates: " + listed + " Hz, current " + Fixed(current) +
      ", content " + Fixed(options_.fps, 3) + " fps: " +
      (wanted == current ? "kept" : "asking " + Fixed(wanted)));
  if (wanted != current) {
    Check(runtime_->RequestRefreshRate(wanted), "xrRequestDisplayRefreshRateFB");
  }
  refresh_known_ = true;
}

void Controller::Recenter(const XrView views[2]) {
  const auto head = projection_view::HeadYawDegrees(
      QuaternionOf(views[0].pose.orientation));
  // What was ahead in the window (its yaw) is where the head looks now
  yaw_offset_ = options_.yaw - head;
  // The screen ahead of the head, at its height, facing it
  const double h = head * projection_view::kDegree;
  XrVector3f centre{
      (views[0].pose.position.x + views[1].pose.position.x) / 2.0f,
      (views[0].pose.position.y + views[1].pose.position.y) / 2.0f,
      (views[0].pose.position.z + views[1].pose.position.z) / 2.0f,
  };
  screen_pose_.position = {
      centre.x + kScreenDistance * static_cast<float>(std::sin(h)),
      centre.y,
      centre.z - kScreenDistance * static_cast<float>(std::cos(h)),
  };
  screen_pose_.orientation = {0.0f, static_cast<float>(std::sin(-h / 2.0)),
                              0.0f, static_cast<float>(std::cos(-h / 2.0))};
  Log("recentered: head yaw " + Fixed(head, 1) + " degrees, offset " +
      Fixed(yaw_offset_, 1));
}

void Controller::Frame() {
  FrameState frame;
  if (!Check(runtime_->WaitFrame(&frame), "xrWaitFrame")) {
    if (!lost_) {
      Fail("the frame loop: " + last_error_);
      lost_ = true;
    }
    return;
  }
  if (!Check(runtime_->BeginFrame(), "xrBeginFrame")) {
    if (!lost_) {
      Fail("the frame loop: " + last_error_);
      lost_ = true;
    }
    return;
  }
  if (first_time_ == 0) {
    first_time_ = frame.predicted_display_time;
  }
  if (last_time_ != 0 && frame.predicted_display_period > 0 &&
      frame.predicted_display_time - last_time_ >
          frame.predicted_display_period * 3 / 2) {
    window_late_++;
  }
  last_time_ = frame.predicted_display_time;
  const double draw_start = options_.now();
  Layers layers;
  if (frame.should_render) {
    XrView views[2];
    for (auto& view : views) {
      view = XrView{};
      view.type = XR_TYPE_VIEW;
      view.pose.orientation.w = 1.0f;
    }
    bool valid = false;
    if (Check(runtime_->LocateViews(frame.predicted_display_time, views,
                                    &valid),
              "xrLocateViews") &&
        valid) {
      if (recenter_.exchange(false)) {
        Recenter(views);
      }
      if (options_.mode == Mode::kScreen) {
        DrawScreen(&layers);
      } else {
        DrawEyes(frame, views, &layers);
      }
    }
  }
  const double draw_ms = (options_.now() - draw_start) * 1000.0;
  if (!Check(runtime_->EndFrame(frame.predicted_display_time, layers),
             "xrEndFrame")) {
    if (lost_) {
      Fail("the frame loop: " + last_error_);
    }
  }
  const bool shown = layers.projection || !layers.quads.empty();
  window_frames_++;
  window_shown_ += shown ? 1 : 0;
  window_draw_ms_ += draw_ms;
  window_draw_max_ms_ = (std::max)(window_draw_max_ms_, draw_ms);
  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.frames++;
    status_.frames_shown += shown ? 1 : 0;
  }
  Stats(options_.now());
}

void Controller::DrawEyes(const FrameState& frame, const XrView views[2],
                          Layers* layers) {
  // The pattern's bar moves at the rate imitated: its time stepped
  double time = static_cast<double>(frame.predicted_display_time - first_time_) /
                kNanoseconds;
  if (options_.fps >= 1.0) {
    time = std::floor(time * options_.fps) / options_.fps;
  }
  bool all = true;
  for (int eye = 0; eye < 2; eye++) {
    uint32_t index = 0;
    if (!Check(runtime_->AcquireImage(eyes_[eye].id, &index),
               "xrAcquireSwapchainImage") ||
        !Check(runtime_->WaitImage(eyes_[eye].id), "xrWaitSwapchainImage")) {
      all = false;
      continue;
    }
    std::string error;
    if (!gpu_->DrawEye(eyes_[eye], index, eye, views[eye], yaw_offset_, time,
                       &error)) {
      all = false;
      if (error != last_error_) {
        last_error_ = error;
        Log("eye " + std::to_string(eye) + ": " + error);
      }
    }
    Check(runtime_->ReleaseImage(eyes_[eye].id), "xrReleaseSwapchainImage");
    auto& out = layers->views[eye];
    out = XrCompositionLayerProjectionView{};
    out.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
    out.pose = views[eye].pose;
    out.fov = views[eye].fov;
    out.subImage.imageRect.offset = {0, 0};
    out.subImage.imageRect.extent = {static_cast<int32_t>(eyes_[eye].width),
                                     static_cast<int32_t>(eyes_[eye].height)};
    out.subImage.imageArrayIndex = 0;
    layers->view_swapchains[eye] = eyes_[eye].id;
  }
  if (finish_due_) {
    const double start = options_.now();
    gpu_->Finish();
    Log("the two eye passes finished on the GPU " +
        Fixed((options_.now() - start) * 1000.0) + " ms after their draws");
    finish_due_ = false;
  }
  layers->projection = all;
}

void Controller::DrawScreen(Layers* layers) {
  const auto content = gpu_->ContentNow();
  if (!content.has_frame || content.frame_width < 2 ||
      content.frame_height < 2) {
    return;
  }
  // The screen's swapchain has the frame's size (capped), made again when it
  // changes
  double scale = 1.0;
  const uint32_t max_width = std::min<uint32_t>(
      kScreenMaxWidth, system_.max_swapchain_width > 0
                           ? system_.max_swapchain_width
                           : kScreenMaxWidth);
  if (static_cast<uint32_t>(content.frame_width) > max_width) {
    scale = static_cast<double>(max_width) / content.frame_width;
  }
  const auto width =
      std::max<uint32_t>(2, static_cast<uint32_t>(content.frame_width * scale));
  const auto height = std::max<uint32_t>(
      2, static_cast<uint32_t>(content.frame_height * scale));
  if (!screen_wrapped_ || screen_.width != width || screen_.height != height) {
    if (screen_wrapped_) {
      gpu_->UnwrapImages(screen_.id);
      runtime_->DestroySwapchain(screen_.id);
      screen_wrapped_ = false;
    }
    if (!Check(runtime_->CreateSwapchain(format_, width, height, &screen_),
               "xrCreateSwapchain")) {
      return;
    }
    std::string path, error;
    if (!gpu_->WrapImages(screen_, &path, &error)) {
      Log("screen images: " + error);
      runtime_->DestroySwapchain(screen_.id);
      return;
    }
    screen_wrapped_ = true;
    screen_drawn_ = false;
    {
      std::lock_guard<std::mutex> lock(status_mutex_);
      status_.swapchain_path = path;
    }
    Log("screen: " + std::to_string(width) + " x " + std::to_string(height) +
        ", " + std::to_string(screen_.images.size()) + " images, " + path);
  }
  // A new image only for a new frame of the video: the runtime shows the
  // last one released meanwhile
  if (!screen_drawn_ || content.frame_count != screen_frame_count_) {
    uint32_t index = 0;
    if (Check(runtime_->AcquireImage(screen_.id, &index),
              "xrAcquireSwapchainImage") &&
        Check(runtime_->WaitImage(screen_.id), "xrWaitSwapchainImage")) {
      std::string error;
      if (gpu_->DrawScreen(screen_, index, &error)) {
        screen_drawn_ = true;
        screen_frame_count_ = content.frame_count;
      } else if (error != last_error_) {
        last_error_ = error;
        Log("screen: " + error);
      }
      Check(runtime_->ReleaseImage(screen_.id), "xrReleaseSwapchainImage");
    }
  }
  if (!screen_drawn_) {
    return;
  }
  // One quad, or one per eye with its half of the frame. A frame of full side
  // by side (two 16:9 pictures, 32:9) or full over under has each eye's shape;
  // a half one is squeezed and each eye shows as the whole frame's shape.
  const double frame_aspect = static_cast<double>(width) / height;
  double eye_aspect = frame_aspect;
  XrRect2Di rects[2] = {{{0, 0}, {static_cast<int32_t>(width),
                                  static_cast<int32_t>(height)}},
                        {{0, 0}, {static_cast<int32_t>(width),
                                  static_cast<int32_t>(height)}}};
  const auto half_w = static_cast<int32_t>(width / 2);
  const auto half_h = static_cast<int32_t>(height / 2);
  if (options_.stereo == ScreenStereo::kLeftRight) {
    rects[0] = {{0, 0}, {half_w, static_cast<int32_t>(height)}};
    rects[1] = {{half_w, 0}, {half_w, static_cast<int32_t>(height)}};
    eye_aspect = frame_aspect >= 3.0 ? frame_aspect / 2.0 : frame_aspect;
  } else if (options_.stereo == ScreenStereo::kTopBottom) {
    rects[0] = {{0, 0}, {static_cast<int32_t>(width), half_h}};
    rects[1] = {{0, half_h}, {static_cast<int32_t>(width), half_h}};
    eye_aspect = frame_aspect <= 1.0 ? frame_aspect * 2.0 : frame_aspect;
  }
  const XrExtent2Df size{kScreenWidth,
                         static_cast<float>(kScreenWidth / eye_aspect)};
  if (options_.stereo == ScreenStereo::kMono) {
    layers->quads.push_back(
        {screen_.id, XR_EYE_VISIBILITY_BOTH, rects[0], screen_pose_, size});
  } else {
    layers->quads.push_back(
        {screen_.id, XR_EYE_VISIBILITY_LEFT, rects[0], screen_pose_, size});
    layers->quads.push_back(
        {screen_.id, XR_EYE_VISIBILITY_RIGHT, rects[1], screen_pose_, size});
  }
}

void Controller::Stats(double now) {
  if (now - stats_at_ < options_.stats_every || window_frames_ == 0) {
    return;
  }
  const double seconds = now - stats_at_;
  std::string memory;
  const auto gpu_memory = gpu_->Memory();
  if (gpu_memory.known) {
    memory = ", GPU memory " + std::to_string(gpu_memory.usage >> 20) +
             " of " + std::to_string(gpu_memory.budget >> 20) + " MB";
  }
  std::vector<float> rates;
  float current = 0.0f;
  std::string rate;
  if (XR_SUCCEEDED(runtime_->RefreshRates(&rates, &current))) {
    rate = ", " + Fixed(current) + " Hz";
  }
  Log("stats: " + Fixed(static_cast<double>(window_frames_) / seconds, 1) + " frames/s, " +
      std::to_string(window_shown_) + " of " + std::to_string(window_frames_) +
      " shown, " + std::to_string(window_late_) + " late, draw " +
      Fixed(window_draw_ms_ / static_cast<double>(window_frames_)) + " ms average, " +
      Fixed(window_draw_max_ms_) + " max" + rate + memory + ", state " +
      SessionStateName(machine_.state()));
  stats_at_ = now;
  window_frames_ = 0;
  window_shown_ = 0;
  window_late_ = 0;
  window_draw_ms_ = 0.0;
  window_draw_max_ms_ = 0.0;
  finish_due_ = true;
}

void Controller::CleanUp() {
  if (session_ && machine_.running() && !lost_) {
    Check(runtime_->EndSession(), "xrEndSession");
    machine_.Ended();
  }
  for (int eye = 0; eye < 2; eye++) {
    if (eyes_wrapped_[eye]) {
      gpu_->UnwrapImages(eyes_[eye].id);
      eyes_wrapped_[eye] = false;
    }
    if (eyes_[eye].id != 0) {
      runtime_->DestroySwapchain(eyes_[eye].id);
      eyes_[eye].id = 0;
    }
  }
  if (screen_wrapped_) {
    gpu_->UnwrapImages(screen_.id);
    runtime_->DestroySwapchain(screen_.id);
    screen_wrapped_ = false;
  }
  if (session_) {
    runtime_->DestroySession();
    session_ = false;
  }
  if (instance_) {
    runtime_->DestroyInstance();
    instance_ = false;
  }
  gpu_->Close();
  Log("cleaned up");
}

}  // namespace xr_probe
