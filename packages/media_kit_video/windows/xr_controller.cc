// Immuch360: the headset view's session and frame loop (IMMUCH360-NOTE.md,
// patch 15); see the header.

#include "xr_controller.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>
#include <thread>

namespace xr_probe {

namespace {

constexpr double kNanoseconds = 1e9;
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
  // No "-0.0" in the log
  if (std::abs(value) < 0.5 * std::pow(10.0, -digits)) value = 0.0;
  std::ostringstream out;
  out.setf(std::ios::fixed);
  out.precision(digits);
  out << value;
  return out.str();
}

projection_view::Quaternion QuaternionOf(const XrQuaternionf& q) {
  return {q.x, q.y, q.z, q.w};
}

XrQuaternionf XrOf(const projection_view::Quaternion& q) {
  return {static_cast<float>(q.x), static_cast<float>(q.y),
          static_cast<float>(q.z), static_cast<float>(q.w)};
}

const char* HandName(int hand) { return hand == 0 ? "left" : "right"; }

std::string Describe(const ContentRequest& content) {
  return std::string("mode ") + ModeName(content.mode) + ", stereo " +
         StereoName(content.stereo) + ", squeeze " +
         SqueezeName(content.squeeze) + ", content " + Fixed(content.fps, 3) +
         " fps, window yaw " + Fixed(content.yaw, 1) + ", " +
         (content.handle != 0 ? "a player" : "no player") + ", " +
         (content.label.empty() ? "no label" : content.label);
}

}  // namespace

Controller::Controller(Runtime* runtime, Gpu* gpu, Options options,
                       Inbox* inbox, Board* board)
    : runtime_(runtime),
      gpu_(gpu),
      options_(std::move(options)),
      own_inbox_(inbox == nullptr ? std::make_unique<Inbox>() : nullptr),
      own_board_(board == nullptr ? std::make_unique<Board>() : nullptr),
      inbox_(inbox != nullptr ? inbox : own_inbox_.get()),
      board_(board != nullptr ? board : own_board_.get()),
      input_(runtime, [this](const std::string& line) { Log(line); }) {
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

void Controller::Log(const std::string& line) {
  if (options_.log) {
    options_.log(line);
  }
}

void Controller::SetPhase(const std::string& phase) {
  board_->Update([&](Status* status) { status->phase = phase; });
  Log("phase: " + phase);
}

void Controller::Fail(StopReason reason, const std::string& detail) {
  if (board_->SetReason(reason, detail)) {
    Log(std::string("stop: ") + StopReasonCode(reason) + ": " + detail);
  }
}

bool Controller::StopAsked() {
  if (!inbox_->stop()) {
    return false;
  }
  Fail(StopReason::kStoppedByApp, "stopped by the app");
  return true;
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
  if (IsLoss(result)) {
    lost_ = true;
    board_->Update([&](Status* status) {
      status->instance_lost = result == XR_ERROR_INSTANCE_LOST;
    });
  }
  return false;
}

bool Controller::Prepare() {
  if (StopAsked()) {
    return false;
  }
  std::string error;
  if (!gpu_->Open(&gpu_info_, &error)) {
    Fail(StopReason::kFailed, "graphics: " + error);
    return false;
  }
  gpu_->SetContent(content_);
  Log("gpu: " + gpu_info_.adapter_name + ", vendor " +
      Hex(gpu_info_.vendor_id) + ", device " + Hex(gpu_info_.device_id) +
      ", LUID " + Hex(static_cast<uint32_t>(gpu_info_.adapter.high)) + ":" +
      Hex(gpu_info_.adapter.low) + ", feature level " +
      Hex(static_cast<uint32_t>(gpu_info_.feature_level)) + ", " +
      std::to_string(gpu_info_.dedicated_video_memory >> 20) +
      " MB dedicated, " + gpu_info_.gl_renderer +
      ", multithread protection before the view: " +
      (gpu_info_.was_multithread_protected ? "on" : "off") +
      ", shares the players' textures: " +
      (gpu_info_.shares_players ? "yes" : "no"));
  if (IsBasicRenderDriver(gpu_info_.vendor_id, gpu_info_.device_id)) {
    Fail(StopReason::kGpuTooWeak,
         "the graphics adapter is Microsoft's Basic Render Driver (no "
         "graphics driver): it cannot drive a headset");
    return false;
  }
  if (StopAsked()) {
    return false;
  }
  std::vector<std::string> optional = {"XR_FB_display_refresh_rate"};
  for (const auto& name : ControllerExtensions()) optional.push_back(name);
  const auto created = runtime_->CreateInstance(optional, &runtime_info_);
  if (!XR_SUCCEEDED(created)) {
    const auto reason =
        ReasonOfInstanceFailure(created, runtime_->LoaderMissing());
    const auto name = runtime_->ResultName(created);
    Fail(reason, reason == StopReason::kNoRuntime
                     ? "no OpenXR runtime is active on this computer (" +
                           name + ")"
                 : reason == StopReason::kLoaderMissing
                     ? "the OpenXR loader: " + name
                     : "xrCreateInstance: " + name);
    return false;
  }
  instance_ = true;
  board_->Update([&](Status* status) {
    status->runtime = runtime_info_.name + " " + Version(runtime_info_.version);
  });
  Log("runtime: " + runtime_info_.name + " " + Version(runtime_info_.version));
  Log("runtime extensions: " + Join(runtime_info_.offered));
  Log("enabled: " + Join(runtime_info_.enabled));
  Log("API layers (implicit ones run without being asked): " +
      Join(runtime_info_.api_layers));
  const double waited_from = options_.now();
  for (;;) {
    const auto result = runtime_->GetSystem(&system_);
    if (XR_SUCCEEDED(result)) {
      break;
    }
    if (result != XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
      Check(result, "xrGetSystem");
      Fail(ReasonOfCallFailure(result), "no headset: " + last_error_);
      return false;
    }
    if (StopAsked()) {
      return false;
    }
    if (options_.now() - waited_from > options_.system_wait_seconds) {
      Fail(StopReason::kNoHeadset,
           "no headset connected to the runtime within " +
               std::to_string(options_.system_wait_seconds) + " s");
      return false;
    }
    if (board_->status().phase != "waiting for the headset") {
      SetPhase("waiting for the headset");
    }
    options_.sleep_ms(250);
  }
  board_->Update([&](Status* status) { status->system = system_.name; });
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
  if (verdict == AdapterVerdict::kOtherAdapter) {
    Fail(StopReason::kOtherGpu, AdapterVerdictName(verdict));
    return false;
  }
  if (verdict == AdapterVerdict::kFeatureLevelTooLow) {
    Fail(StopReason::kGpuTooWeak, AdapterVerdictName(verdict));
    return false;
  }
  if (StopAsked()) {
    return false;
  }
  const auto session = runtime_->CreateSession(gpu_info_.device);
  if (!Check(session, "xrCreateSession")) {
    Fail(ReasonOfCallFailure(session), "the session: " + last_error_);
    return false;
  }
  session_ = true;
  const bool controllers =
      input_.Setup(runtime_info_.enabled, options_.action_names);
  board_->Update([&](Status* status) { status->controllers = controllers; });
  std::vector<int64_t> formats;
  const auto listed = runtime_->SwapchainFormats(&formats);
  if (!Check(listed, "xrEnumerateSwapchainFormats")) {
    Fail(ReasonOfCallFailure(listed), "the swapchain formats: " + last_error_);
    return false;
  }
  std::vector<std::string> names;
  for (const auto format : formats) names.push_back(DxgiFormatName(format));
  format_ = ChooseSwapchainFormat(formats);
  Log("swapchain formats offered: " + Join(names) + "; chosen: " +
      (format_ == 0 ? "none" : DxgiFormatName(format_)));
  if (format_ == 0) {
    Fail(StopReason::kFailed, "no 8 bit RGBA swapchain format");
    return false;
  }
  if (!IsSrgb(format_)) {
    Log("warning: no sRGB swapchain: the runtime takes the values as linear, "
        "the picture will look too dark");
  }
  return true;
}

bool Controller::EnsureEyes() {
  if (eyes_made_) {
    return true;
  }
  for (int eye = 0; eye < 2; eye++) {
    const auto width = (std::max)(16u, system_.eye_width[eye]);
    const auto height = (std::max)(16u, system_.eye_height[eye]);
    const auto made = runtime_->CreateSwapchain(format_, width, height,
                                                &eyes_[eye]);
    if (!Check(made, "xrCreateSwapchain")) {
      eyes_[eye].id = 0;
      Fail(ReasonOfCallFailure(made), "the eye swapchains: " + last_error_);
      done_ = true;
      return false;
    }
    std::string path, error;
    if (!gpu_->WrapImages(eyes_[eye], &path, &error)) {
      Fail(StopReason::kFailed, "the eye images: " + error);
      done_ = true;
      return false;
    }
    eyes_wrapped_[eye] = true;
    board_->Update([&](Status* status) { status->swapchain_path = path; });
    Log("eye " + std::to_string(eye) + ": " + std::to_string(width) + " x " +
        std::to_string(height) + ", " +
        std::to_string(eyes_[eye].images.size()) + " images, " + path);
  }
  eyes_made_ = true;
  return true;
}

void Controller::Run() {
  start_ = options_.now();
  stats_at_ = start_;
  ContentRequest first;
  content_ = inbox_->TakeContent(&first) ? first : options_.content;
  board_->Update([&](Status* status) {
    status->mode = ModeName(content_.mode);
    status->phase = "starting";
  });
  Log("content: " + Describe(content_));
  if (Prepare()) {
    SetPhase("running");
    while (!done_ && !lost_) {
      if (inbox_->stop() && !exit_requested_) {
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
      if (exit_requested_ &&
          options_.now() - exit_requested_at_ > options_.stop_bound_seconds) {
        Log("the runtime did not end the session within " +
            Fixed(options_.stop_bound_seconds, 1) + " s: leaving");
        break;
      }
      Event event;
      while (!done_ && runtime_->PollEvent(&event)) {
        HandleEvent(event, &done_);
      }
      if (done_ || lost_) {
        break;
      }
      TakeCommands();
      const int seen = visibility_.Update(options_.now(), machine_.visible());
      if (seen != 0) {
        Log(seen > 0 ? "visible again" : "not visible for more than 2 s");
        HeadsetEvent visible;
        visible.kind = EventKind::kVisibility;
        visible.on = seen > 0;
        PushEvent(visible);
      }
      if (!machine_.running()) {
        options_.sleep_ms(20);
        continue;
      }
      Frame();
    }
    if (lost_) {
      Fail(StopReason::kRuntimeLost, "lost: " + last_error_);
    }
  }
  CleanUp();
  if (exit_requested_) {
    Fail(StopReason::kStoppedByApp, "stopped by the app");
  } else {
    Fail(StopReason::kEndedFromHeadset,
         "the session was ended from the headset or its runtime");
  }
  SetPhase("stopped");
}

void Controller::HandleEvent(const Event& event, bool* done) {
  switch (event.type) {
    case Event::Type::kSessionState: {
      const auto action = machine_.On(event.state);
      board_->Update([&](Status* status) {
        status->session_state = SessionStateName(event.state);
        status->visible = machine_.visible();
        status->focused = event.state == XR_SESSION_STATE_FOCUSED;
      });
      Log("session state: " + SessionStateName(event.state));
      if (action == SessionAction::kBegin) {
        const auto begun = runtime_->BeginSession();
        if (Check(begun, "xrBeginSession")) {
          machine_.Began();
          recenter_ = true;
          ChooseRefreshRate();
        } else {
          Fail(ReasonOfCallFailure(begun), "the session: " + last_error_);
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
          Fail(StopReason::kRuntimeLost,
               "the runtime lost the session (loss pending)");
        } else if (!exit_requested_) {
          Fail(StopReason::kEndedFromHeadset,
               "the session was ended from the headset or its runtime");
        }
        *done = true;
      }
      break;
    }
    case Event::Type::kInstanceLossPending:
      Log("instance loss pending (the runtime stops or updates)");
      Fail(StopReason::kRuntimeLost,
           "the runtime is going away (instance loss pending)");
      board_->Update([](Status* status) { status->instance_lost = true; });
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
      Log("refresh rate obtained: " + Fixed(event.from_rate) + " to " +
          Fixed(event.to_rate) + " Hz");
      board_->Update(
          [&](Status* status) { status->refresh_rate = event.to_rate; });
      break;
    case Event::Type::kInteractionProfileChanged: {
      bool changed[2] = {false, false};
      input_.ReadProfiles(changed);
      for (int hand = 0; hand < 2; hand++) {
        if (!changed[hand]) continue;
        const auto profile = input_.profile(hand);
        Log(std::string("controller of the ") + HandName(hand) +
            " hand: " + (profile.empty() ? "none" : profile));
        board_->Update(
            [&](Status* status) { status->profiles[hand] = profile; });
        HeadsetEvent told;
        told.kind = EventKind::kProfile;
        told.hand = hand;
        told.profile = profile;
        PushEvent(told);
      }
      break;
    }
    case Event::Type::kOther:
      Log("event " + std::to_string(static_cast<int>(event.other)));
      break;
  }
}

void Controller::TakeCommands() {
  ContentRequest next;
  if (inbox_->TakeContent(&next)) {
    ApplyContent(next);
  }
  double fps = 0.0;
  if (inbox_->TakeRate(&fps) && fps != content_.fps) {
    content_.fps = fps;
    gpu_->SetContent(content_);
    Log("content rate: " + Fixed(fps, 3) + " fps");
    if (machine_.running()) {
      ChooseRefreshRate();
    }
  }
  bool lying_down = false;
  if (inbox_->TakeRecenter(&lying_down)) {
    lying_down_ = lying_down;
    recenter_ = true;
    board_->Update([&](Status* status) { status->lying_down = lying_down_; });
  }
  StripImage image;
  bool visible = false;
  if (inbox_->TakeStrip(strip_image_.generation, &image, &visible)) {
    strip_image_ = std::move(image);
  }
  strip_visible_ = visible;
}

void Controller::ApplyContent(const ContentRequest& next) {
  const bool new_source =
      next.handle != content_.handle || next.mode != content_.mode;
  const bool new_rate = next.fps != content_.fps;
  if (content_.mode == Mode::kScreen && next.mode != Mode::kScreen) {
    DestroyScreen();
  }
  content_ = next;
  gpu_->SetContent(content_);
  Log("content: " + Describe(content_));
  board_->Update(
      [&](Status* status) { status->mode = ModeName(content_.mode); });
  if (new_source) {
    recenter_ = true;
    screen_drawn_ = false;
  }
  if (new_rate && machine_.running()) {
    ChooseRefreshRate();
  }
}

void Controller::ChooseRefreshRate() {
  if (!options_.match_refresh_rate) {
    if (!refresh_known_) {
      Log("refresh rate: matching to the content is off");
    }
    refresh_known_ = true;
    return;
  }
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
  const auto wanted = xr_probe::ChooseRefreshRate(rates, current, content_.fps);
  Log("refresh rates: " + listed + " Hz, current " + Fixed(current) +
      ", content " + Fixed(content_.fps, 3) + " fps: " +
      (wanted == current ? "kept" : "asking " + Fixed(wanted)));
  board_->Update([&](Status* status) {
    status->refresh_rate = current;
    status->refresh_asked = wanted;
  });
  if (wanted != current) {
    Check(runtime_->RequestRefreshRate(wanted), "xrRequestDisplayRefreshRateFB");
  }
  refresh_known_ = true;
}

void Controller::Recenter(const XrView views[2]) {
  // Both eyes' orientations together (a headset with canted displays has
  // them apart)
  auto a = QuaternionOf(views[0].pose.orientation);
  auto b = QuaternionOf(views[1].pose.orientation);
  if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0.0) {
    b = {-b.x, -b.y, -b.z, -b.w};
  }
  const auto head =
      projection_view::Normalized({a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w});
  anchor_ = projection_view::AnchorOf(head, lying_down_);
  head_centre_[0] = (views[0].pose.position.x + views[1].pose.position.x) / 2.0;
  head_centre_[1] = (views[0].pose.position.y + views[1].pose.position.y) / 2.0;
  head_centre_[2] = (views[0].pose.position.z + views[1].pose.position.z) / 2.0;
  // The screen ahead of the anchor, facing the eyes
  double position[3];
  projection_view::Quaternion orientation;
  projection_view::PoseAhead(anchor_, head_centre_, kScreenDistance, 0.0,
                             position, &orientation);
  screen_pose_.position = {static_cast<float>(position[0]),
                           static_cast<float>(position[1]),
                           static_cast<float>(position[2])};
  screen_pose_.orientation = XrOf(orientation);
  Log(std::string("recentered ") + (lying_down_ ? "lying down" : "upright") +
      ": head yaw " + Fixed(anchor_.yaw, 1) + ", pitch " +
      Fixed(anchor_.pitch, 1) + " degrees, window yaw " +
      Fixed(content_.yaw, 1));
}

void Controller::PushEvent(HeadsetEvent event) {
  event.time = options_.now() - start_;
  if (board_->Push(event)) {
    events_dropped_++;
    if (events_dropped_ == 1 || events_dropped_ % 64 == 0) {
      Log("event queue full: the oldest event dropped (" +
          std::to_string(events_dropped_) + " so far)");
    }
  }
}

void Controller::ApplyFired(const std::vector<Fired>& fired) {
  for (const auto& gesture : fired) {
    HeadsetEvent event;
    event.hand = gesture.hand;
    switch (gesture.gesture) {
      case Gesture::kPlayPause:
        event.kind = EventKind::kPlayPause;
        break;
      case Gesture::kSeek:
        event.kind = EventKind::kSeek;
        event.seconds = gesture.seconds;
        break;
      case Gesture::kRecenter:
        event.kind = EventKind::kRecenter;
        recenter_ = true;
        break;
      case Gesture::kLyingDown:
        event.kind = EventKind::kLyingDown;
        lying_down_ = !lying_down_;
        recenter_ = true;
        event.on = lying_down_;
        board_->Update(
            [&](Status* status) { status->lying_down = lying_down_; });
        break;
      case Gesture::kExit:
        event.kind = EventKind::kExit;
        break;
    }
    Log(std::string("action: ") + GestureName(gesture.gesture) + " (" +
        HandName(gesture.hand) + " hand" +
        (gesture.gesture == Gesture::kSeek
             ? ", " + Fixed(gesture.seconds, 0) + " s"
             : gesture.gesture == Gesture::kLyingDown
                 ? std::string(lying_down_ ? ", on" : ", off")
                 : std::string()) +
        ")");
    PushEvent(event);
  }
}

void Controller::Frame() {
  FrameState frame;
  const auto waited = runtime_->WaitFrame(&frame);
  if (!Check(waited, "xrWaitFrame")) {
    if (!lost_) {
      Fail(ReasonOfCallFailure(waited), "the frame loop: " + last_error_);
      done_ = true;
    }
    return;
  }
  const auto begun = runtime_->BeginFrame();
  if (!Check(begun, "xrBeginFrame")) {
    if (!lost_) {
      Fail(ReasonOfCallFailure(begun), "the frame loop: " + last_error_);
      done_ = true;
    }
    return;
  }
  if (first_time_ == 0) {
    first_time_ = frame.predicted_display_time;
  }
  bool late = false;
  if (last_time_ != 0 && frame.predicted_display_period > 0 &&
      frame.predicted_display_time - last_time_ >
          frame.predicted_display_period * 3 / 2) {
    window_late_++;
    late = true;
  }
  last_time_ = frame.predicted_display_time;
  // The controllers, read only while focused
  std::vector<Fired> fired;
  input_.Update(options_.now(),
                machine_.state() == XR_SESSION_STATE_FOCUSED, &fired);
  ApplyFired(fired);
  const double draw_start = options_.now();
  Layers layers;
  if (frame.should_render && !done_) {
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
      if (recenter_) {
        recenter_ = false;
        Recenter(views);
      }
      if (content_.mode == Mode::kScreen) {
        DrawScreen(&layers);
      } else {
        DrawEyes(frame, views, &layers);
      }
      AddStrip(&layers);
    }
  }
  const double draw_ms = (options_.now() - draw_start) * 1000.0;
  const auto ended = runtime_->EndFrame(frame.predicted_display_time, layers);
  if (!Check(ended, "xrEndFrame") && lost_) {
    Fail(StopReason::kRuntimeLost, "the frame loop: " + last_error_);
  }
  const bool shown = layers.projection || !layers.quads.empty();
  window_frames_++;
  window_shown_ += shown ? 1 : 0;
  window_draw_ms_ += draw_ms;
  window_draw_max_ms_ = (std::max)(window_draw_max_ms_, draw_ms);
  board_->Update([&](Status* status) {
    status->frames++;
    status->frames_shown += shown ? 1 : 0;
    status->frames_late += late ? 1 : 0;
  });
  Stats(options_.now());
}

void Controller::DrawEyes(const FrameState& frame, const XrView views[2],
                          Layers* layers) {
  if (!EnsureEyes()) {
    return;
  }
  // The pattern's bar moves at the rate imitated: its time stepped
  double time = static_cast<double>(frame.predicted_display_time - first_time_) /
                kNanoseconds;
  if (content_.fps >= 1.0) {
    time = std::floor(time * content_.fps) / content_.fps;
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
    ProjectionView eye_view;
    const auto& fov = views[eye].fov;
    projection_view::AnchoredEyeView(
        QuaternionOf(views[eye].pose.orientation), fov.angleLeft,
        fov.angleRight, fov.angleUp, fov.angleDown, anchor_, content_.yaw,
        &eye_view);
    // The head never rests in a headset: the bilinear filter, the cheap one
    eye_view.sharp = false;
    std::string error;
    if (!gpu_->DrawEye(eyes_[eye], index, eye, eye_view, time, &error)) {
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

void Controller::DestroyScreen() {
  if (screen_wrapped_) {
    gpu_->UnwrapImages(screen_.id);
    screen_wrapped_ = false;
  }
  if (screen_.id != 0) {
    runtime_->DestroySwapchain(screen_.id);
    screen_.id = 0;
    Log("screen: swapchain destroyed");
  }
  screen_drawn_ = false;
}

void Controller::DrawScreen(Layers* layers) {
  const auto content = gpu_->FrameNow();
  if (!content.has_frame || content.frame_width < 2 ||
      content.frame_height < 2) {
    return;
  }
  // The screen's swapchain has the frame's size (capped), made again when it
  // changes
  double scale = 1.0;
  const uint32_t max_width = (std::min)(
      kScreenMaxWidth, system_.max_swapchain_width > 0
                           ? system_.max_swapchain_width
                           : kScreenMaxWidth);
  if (static_cast<uint32_t>(content.frame_width) > max_width) {
    scale = static_cast<double>(max_width) / content.frame_width;
  }
  const auto width = (std::max)(
      2u, static_cast<uint32_t>(content.frame_width * scale));
  const auto height = (std::max)(
      2u, static_cast<uint32_t>(content.frame_height * scale));
  if (screen_.id == 0 || screen_.width != width || screen_.height != height) {
    DestroyScreen();
    if (!Check(runtime_->CreateSwapchain(format_, width, height, &screen_),
               "xrCreateSwapchain")) {
      screen_.id = 0;
      return;
    }
    std::string path, error;
    if (!gpu_->WrapImages(screen_, &path, &error)) {
      Log("screen images: " + error);
      DestroyScreen();
      return;
    }
    screen_wrapped_ = true;
    screen_drawn_ = false;
    board_->Update([&](Status* status) { status->swapchain_path = path; });
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
  // One quad, or one per eye with its half of the frame, the eyes' shape as
  // the app gives it (full or half)
  const auto layout =
      LayoutScreen(width, height, content_.stereo, content_.squeeze);
  const XrExtent2Df size{kScreenWidth,
                         static_cast<float>(kScreenWidth / layout.eye_aspect)};
  screen_height_ = size.height;
  if (layout.quads == 1) {
    layers->quads.push_back({screen_.id, XR_EYE_VISIBILITY_BOTH,
                             layout.rects[0], screen_pose_, size});
  } else {
    layers->quads.push_back({screen_.id, XR_EYE_VISIBILITY_LEFT,
                             layout.rects[0], screen_pose_, size});
    layers->quads.push_back({screen_.id, XR_EYE_VISIBILITY_RIGHT,
                             layout.rects[1], screen_pose_, size});
  }
}

void Controller::AddStrip(Layers* layers) {
  const auto hide = [&] {
    board_->Update([](Status* status) { status->strip_shown = false; });
  };
  if (!strip_visible_ || strip_image_.generation == 0 || strip_failed_) {
    hide();
    return;
  }
  const auto& image = strip_image_;
  if (image.width == 0 || image.height == 0 || image.width > kStripWidth ||
      image.height > kStripHeight ||
      image.rgba.size() < static_cast<size_t>(image.width) * image.height * 4) {
    if (!strip_size_logged_) {
      Log("strip: an image of " + std::to_string(image.width) + " x " +
          std::to_string(image.height) + " refused (at most " +
          std::to_string(kStripWidth) + " x " + std::to_string(kStripHeight) +
          ", RGBA)");
      strip_size_logged_ = true;
    }
    hide();
    return;
  }
  const size_t needed = (layers->projection ? 1u : 0u) + layers->quads.size() + 1u;
  if (system_.max_layers > 0 && needed > system_.max_layers) {
    if (!strip_room_logged_) {
      Log("strip: " + std::to_string(needed) +
          " layers needed, the runtime allows " +
          std::to_string(system_.max_layers) + ": the strip is dropped");
      strip_room_logged_ = true;
    }
    hide();
    return;
  }
  if (strip_.id == 0) {
    if (!Check(runtime_->CreateSwapchain(format_, kStripWidth, kStripHeight,
                                         &strip_),
               "xrCreateSwapchain")) {
      strip_.id = 0;
      strip_failed_ = true;
      Log("strip: no swapchain, the view goes on without it");
      hide();
      return;
    }
    std::string path, error;
    if (!gpu_->WrapImages(strip_, &path, &error)) {
      Log("strip images: " + error + ", the view goes on without it");
      runtime_->DestroySwapchain(strip_.id);
      strip_.id = 0;
      strip_failed_ = true;
      hide();
      return;
    }
    strip_wrapped_ = true;
    Log("strip: " + std::to_string(kStripWidth) + " x " +
        std::to_string(kStripHeight) + ", " +
        std::to_string(strip_.images.size()) + " images, " + path);
  }
  if (image.generation != strip_uploaded_) {
    uint32_t index = 0;
    if (Check(runtime_->AcquireImage(strip_.id, &index),
              "xrAcquireSwapchainImage") &&
        Check(runtime_->WaitImage(strip_.id), "xrWaitSwapchainImage")) {
      std::string error;
      if (gpu_->UploadStrip(strip_, index, image.rgba.data(), image.width,
                            image.height, &error)) {
        strip_drawn_ = true;
        strip_rect_ = {{0, 0},
                       {static_cast<int32_t>(image.width),
                        static_cast<int32_t>(image.height)}};
        window_strip_uploads_++;
        board_->Update([](Status* status) { status->strip_uploads++; });
      } else if (error != last_error_) {
        last_error_ = error;
        Log("strip: " + error);
      }
      Check(runtime_->ReleaseImage(strip_.id), "xrReleaseSwapchainImage");
    }
    strip_uploaded_ = image.generation;
  }
  if (!strip_drawn_) {
    hide();
    return;
  }
  QuadLayer quad;
  quad.swapchain = strip_.id;
  quad.eye = XR_EYE_VISIBILITY_BOTH;
  quad.rect = strip_rect_;
  quad.size = {kStripMetresWide, kStripMetresHigh};
  if (content_.mode == Mode::kScreen && screen_drawn_) {
    // Under the screen, in its plane
    const double up_unit[3] = {0.0, 1.0, 0.0};
    double up[3];
    projection_view::Rotate(anchor_.rotation, up_unit, up);
    const double drop =
        screen_height_ / 2.0 + kStripUnderScreen + kStripMetresHigh / 2.0;
    quad.pose.orientation = screen_pose_.orientation;
    quad.pose.position = {
        static_cast<float>(screen_pose_.position.x - up[0] * drop),
        static_cast<float>(screen_pose_.position.y - up[1] * drop),
        static_cast<float>(screen_pose_.position.z - up[2] * drop)};
  } else {
    double position[3];
    projection_view::Quaternion orientation;
    projection_view::PoseAhead(anchor_, head_centre_, kStripDistance,
                               kStripDown, position, &orientation);
    quad.pose.orientation = XrOf(orientation);
    quad.pose.position = {static_cast<float>(position[0]),
                          static_cast<float>(position[1]),
                          static_cast<float>(position[2])};
  }
  layers->quads.push_back(quad);
  board_->Update([](Status* status) { status->strip_shown = true; });
}

void Controller::Stats(double now) {
  if (now - stats_at_ < options_.stats_every || window_frames_ == 0) {
    return;
  }
  const double seconds = now - stats_at_;
  std::string memory;
  const auto gpu_memory = gpu_->Memory();
  bool tight = false;
  if (gpu_memory.known) {
    memory = ", GPU memory " + std::to_string(gpu_memory.usage >> 20) +
             " of " + std::to_string(gpu_memory.budget >> 20) + " MB";
    tight = gpu_memory.budget > 0 &&
            static_cast<double>(gpu_memory.usage) >
                0.9 * static_cast<double>(gpu_memory.budget);
  }
  if (tight != memory_tight_) {
    Log(tight ? "memory: GPU memory above 90 % of its budget"
              : "memory: GPU memory back under 90 % of its budget");
    memory_tight_ = tight;
  }
  const auto snapshot = gpu_->TakeSnapshotTiming();
  const double snapshot_ms =
      snapshot.count > 0 ? snapshot.total_ms / static_cast<double>(snapshot.count)
                         : 0.0;
  std::vector<float> rates;
  float current = 0.0f;
  std::string rate;
  if (XR_SUCCEEDED(runtime_->RefreshRates(&rates, &current))) {
    rate = ", " + Fixed(current) + " Hz";
  }
  Log("stats: " + Fixed(static_cast<double>(window_frames_) / seconds, 1) +
      " frames/s, " + std::to_string(window_shown_) + " of " +
      std::to_string(window_frames_) + " shown, " +
      std::to_string(window_late_) + " late, draw " +
      Fixed(window_draw_ms_ / static_cast<double>(window_frames_)) +
      " ms average, " + Fixed(window_draw_max_ms_) + " max" + rate + memory +
      ", snapshot " + Fixed(snapshot_ms, 3) + " ms average, " +
      Fixed(snapshot.max_ms, 3) + " max, " +
      std::to_string(window_strip_uploads_) + " strip uploads, state " +
      SessionStateName(machine_.state()));
  board_->Update([&](Status* status) {
    status->memory_tight = tight;
    status->memory_used_mb = gpu_memory.usage >> 20;
    status->memory_budget_mb = gpu_memory.budget >> 20;
    status->stats_at = now - start_;
    status->snapshot_ms = snapshot_ms;
    status->snapshot_max_ms = snapshot.max_ms;
    if (!rate.empty()) status->refresh_rate = current;
  });
  stats_at_ = now;
  window_frames_ = 0;
  window_shown_ = 0;
  window_late_ = 0;
  window_strip_uploads_ = 0;
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
  DestroyScreen();
  if (strip_wrapped_) {
    gpu_->UnwrapImages(strip_.id);
    strip_wrapped_ = false;
  }
  if (strip_.id != 0) {
    runtime_->DestroySwapchain(strip_.id);
    strip_.id = 0;
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
