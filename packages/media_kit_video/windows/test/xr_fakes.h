// Immuch360: the fakes of the headset view's tests (IMMUCH360-NOTE.md, patch
// 15): a runtime that plays a script of events and records every call, and a
// GPU side that records what it was asked to draw. No Windows header; shared
// by xr_probe_test.cc (the probe's checks) and xr_v1_test.cc (version 1).

#ifndef XR_FAKES_H_
#define XR_FAKES_H_

#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "../xr_controller.h"
#include "../xr_runtime.h"

namespace fakes {

inline int failures = 0;
inline int checks = 0;

inline void Check(bool ok, const std::string& what) {
  checks++;
  if (!ok) {
    failures++;
    std::printf("FAILED: %s\n", what.c_str());
  }
}

inline void Near(double expected, double actual, double tolerance,
                 const std::string& what) {
  Check(std::abs(expected - actual) <= tolerance,
        what + ": expected " + std::to_string(expected) + ", got " +
            std::to_string(actual));
}

inline xr_probe::Event State(XrSessionState state) {
  xr_probe::Event event;
  event.type = xr_probe::Event::Type::kSessionState;
  event.state = state;
  return event;
}

inline xr_probe::Event Simple(xr_probe::Event::Type type) {
  xr_probe::Event event;
  event.type = type;
  return event;
}

// Calls written from the view's thread, read by the test's
class Journal {
 public:
  void Add(const std::string& line) {
    std::lock_guard<std::mutex> lock(mutex_);
    lines_.push_back(line);
  }
  std::vector<std::string> lines() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lines_;
  }
  int Count(const std::string& line) const {
    int n = 0;
    for (const auto& l : lines()) n += l == line ? 1 : 0;
    return n;
  }

 private:
  mutable std::mutex mutex_;
  std::vector<std::string> lines_;
};

// Holds a call until released
class Blocker {
 public:
  void Block() {
    std::unique_lock<std::mutex> lock(mutex_);
    blocked_ = true;
    changed_.notify_all();
    changed_.wait(lock, [&] { return released_; });
  }
  void Release() {
    std::lock_guard<std::mutex> lock(mutex_);
    released_ = true;
    changed_.notify_all();
  }
  bool WaitBlocked(int ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, std::chrono::milliseconds(ms),
                             [&] { return blocked_; });
  }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  bool blocked_ = false;
  bool released_ = false;
};

// What one hand of the fake gives
struct FakeHand {
  float play = 0.0f;
  float seek_x = 0.0f;
  bool seek_press = false;
  bool recenter = false;
  bool exit = false;
};

// A runtime that plays a script: one list of events per round of PollEvent
// calls, the frames' results, and every call written down
class FakeRuntime : public xr_probe::Runtime {
 public:
  XrResult create_result = XR_SUCCESS;
  bool loader_missing = false;
  XrResult system_result = XR_SUCCESS;
  int unavailable_systems = 0;
  xr_probe::AdapterId adapter{42, 0};
  int32_t min_feature_level = 0xb000;
  uint32_t max_layers = 16;
  XrResult session_result = XR_SUCCESS;
  std::vector<int64_t> formats = {xr_probe::kDxgiR8G8B8A8UnormSrgb};
  std::vector<std::string> offered = {"XR_KHR_D3D11_enable",
                                      "XR_FB_display_refresh_rate"};
  // Per round of polls: the events
  std::deque<std::vector<xr_probe::Event>> script;
  // Once the script is over, EXITING after this many empty rounds (-1:
  // never)
  int exit_after_empty = 300;
  // Once the script is over, what every xrPollEvent returns in place of
  // XR_EVENT_UNAVAILABLE (XR_ERROR_INSTANCE_LOST: the runtime quit while no
  // frame call was made); XR_SUCCESS: none
  XrResult poll_error = XR_SUCCESS;
  // Per frame index: what xrWaitFrame returns (XR_SUCCESS by default)
  std::vector<std::pair<int, XrResult>> wait_results;
  // xrWaitFrame of this index blocks until |blocker| is released
  int block_at_frame = -1;
  std::shared_ptr<Blocker> blocker;
  // GetSystem blocks (stop during prepare)
  std::shared_ptr<Blocker> system_blocker;
  // Called before each xrWaitFrame with its index (inputs over time)
  std::function<void(int)> on_frame;
  // The test's clock, advanced 1/90 s per frame when set
  std::shared_ptr<double> clock;
  bool render = true;
  std::vector<std::string> calls;
  std::shared_ptr<Journal> journal = std::make_shared<Journal>();
  int frames = 0;
  int polls_without_events = 0;
  std::vector<float> rates = {72, 90, 120};
  float rate = 90;
  bool has_rates = true;
  uint64_t next_swapchain = 1;
  bool exit_on_request = true;
  bool exit_requested = false;
  int layers_with_projection = 0;
  int quad_layers = 0;
  // Head orientation and position of both eyes
  XrQuaternionf head{0, 0, 0, 1};
  XrVector3f head_position{0, 1.6f, 0};
  // The action system
  std::set<std::string> rejected_profiles;
  std::map<std::string, std::vector<std::pair<std::string, std::string>>>
      suggested;
  std::string current_profile[2];
  FakeHand hands[2];
  std::vector<std::pair<int, float>> haptics;
  int syncs = 0;
  bool attached = false;
  std::shared_ptr<std::atomic<bool>> destroyed;

  ~FakeRuntime() override {
    if (destroyed) destroyed->store(true);
  }

  void Record(const std::string& call) {
    calls.push_back(call);
    journal->Add(call);
  }

  XrResult CreateInstance(const std::vector<std::string>& optional,
                          xr_probe::RuntimeInfo* info) override {
    Record("CreateInstance");
    info->name = "Fake runtime";
    info->version = XR_MAKE_VERSION(1, 0, 1);
    info->offered = offered;
    info->enabled = {"XR_KHR_D3D11_enable"};
    info->api_layers = {"XR_APILAYER_FAKE_overlay"};
    for (const auto& name : optional) {
      for (const auto& known : offered) {
        if (known == name) info->enabled.push_back(name);
      }
    }
    return create_result;
  }
  bool LoaderMissing() const override { return loader_missing; }
  XrResult GetSystem(xr_probe::SystemInfo* info) override {
    Record("GetSystem");
    if (system_blocker) system_blocker->Block();
    if (system_result != XR_SUCCESS) return system_result;
    if (unavailable_systems > 0) {
      unavailable_systems--;
      return XR_ERROR_FORM_FACTOR_UNAVAILABLE;
    }
    info->name = "Fake headset";
    info->eye_width[0] = info->eye_width[1] = 64;
    info->eye_height[0] = info->eye_height[1] = 64;
    info->max_swapchain_width = info->max_swapchain_height = 4096;
    info->max_layers = max_layers;
    info->adapter = adapter;
    info->min_feature_level = min_feature_level;
    return XR_SUCCESS;
  }
  XrResult CreateSession(void*) override {
    Record("CreateSession");
    return session_result;
  }
  XrResult SwapchainFormats(std::vector<int64_t>* out) override {
    *out = formats;
    return XR_SUCCESS;
  }
  XrResult CreateSwapchain(int64_t format, uint32_t width, uint32_t height,
                           xr_probe::Swapchain* out) override {
    Record("CreateSwapchain");
    out->id = next_swapchain++;
    journal->Add("+swapchain " + std::to_string(out->id) + " " +
                 std::to_string(width) + "x" + std::to_string(height));
    out->format = format;
    out->width = width;
    out->height = height;
    out->images = {nullptr, nullptr, nullptr};
    return XR_SUCCESS;
  }
  void DestroySwapchain(uint64_t id) override {
    Record("DestroySwapchain");
    journal->Add("-swapchain " + std::to_string(id));
  }
  // One round of polls (until it returns XR_EVENT_UNAVAILABLE) takes the next
  // entry of the script; once the script is over, EXITING after a while, as a
  // runtime that closes, or |poll_error|
  XrResult PollEvent(xr_probe::Event* event) override {
    if (!round_open) {
      round_open = true;
      if (!script.empty()) {
        pending.assign(script.front().begin(), script.front().end());
        script.pop_front();
      } else if (poll_error != XR_SUCCESS) {
        round_open = false;
        Record("PollEvent error");
        return poll_error;
      } else if (exit_after_empty >= 0 &&
                 ++polls_without_events > exit_after_empty) {
        polls_without_events = 0;
        pending.push_back(State(XR_SESSION_STATE_EXITING));
      }
    }
    if (pending.empty()) {
      round_open = false;
      return XR_EVENT_UNAVAILABLE;
    }
    *event = pending.front();
    pending.pop_front();
    return XR_SUCCESS;
  }
  XrResult BeginSession() override {
    Record("BeginSession");
    return XR_SUCCESS;
  }
  XrResult EndSession() override {
    Record("EndSession");
    return XR_SUCCESS;
  }
  XrResult RequestExitSession() override {
    Record("RequestExitSession");
    exit_requested = true;
    if (exit_on_request) {
      script.push_front(
          {State(XR_SESSION_STATE_STOPPING), State(XR_SESSION_STATE_EXITING)});
    }
    return XR_SUCCESS;
  }
  XrResult WaitFrame(xr_probe::FrameState* state) override {
    const int index = frames++;
    Record("WaitFrame");
    if (on_frame) on_frame(index);
    if (clock) *clock += 1.0 / 90.0;
    if (index == block_at_frame && blocker) blocker->Block();
    for (const auto& [at, result] : wait_results) {
      if (at == index) return result;
    }
    state->predicted_display_time = 1000000000LL + index * 11111111LL;
    state->predicted_display_period = 11111111LL;
    state->should_render = render;
    return XR_SUCCESS;
  }
  XrResult BeginFrame() override { return XR_SUCCESS; }
  XrResult LocateViews(XrTime, XrView views[2], bool* valid) override {
    for (int i = 0; i < 2; i++) {
      views[i].pose.orientation = head;
      views[i].pose.position = {head_position.x + (i == 0 ? -0.03f : 0.03f),
                                head_position.y, head_position.z};
      views[i].fov = {-0.8f, 0.7f, 0.8f, -0.85f};
    }
    *valid = true;
    return XR_SUCCESS;
  }
  XrResult AcquireImage(uint64_t, uint32_t* index) override {
    *index = 0;
    return XR_SUCCESS;
  }
  XrResult WaitImage(uint64_t) override { return XR_SUCCESS; }
  XrResult ReleaseImage(uint64_t) override { return XR_SUCCESS; }
  XrResult EndFrame(XrTime, const xr_probe::Layers& layers) override {
    if (layers.projection) layers_with_projection++;
    quad_layers += static_cast<int>(layers.quads.size());
    last_layers = layers;
    all_layers.push_back(layers);
    return XR_SUCCESS;
  }
  XrResult RefreshRates(std::vector<float>* out, float* current) override {
    if (!has_rates) return XR_ERROR_FUNCTION_UNSUPPORTED;
    *out = rates;
    *current = rate;
    return XR_SUCCESS;
  }
  XrResult RequestRefreshRate(float wanted) override {
    Record("RequestRefreshRate " + std::to_string(static_cast<int>(wanted)));
    rate = wanted;
    return XR_SUCCESS;
  }

  XrResult StringToPath(const std::string& path, XrPath* out) override {
    if (path.empty() || path[0] != '/') return XR_ERROR_PATH_FORMAT_INVALID;
    auto found = paths.find(path);
    if (found == paths.end()) {
      found = paths.emplace(path, paths.size() + 1).first;
      names[found->second] = path;
    }
    *out = found->second;
    return XR_SUCCESS;
  }
  XrResult CreateActionSet(const std::string& name, const std::string& localized,
                           uint64_t* id) override {
    Record("CreateActionSet " + name + " (" + localized + ")");
    *id = 1000;
    return XR_SUCCESS;
  }
  XrResult CreateAction(uint64_t, const xr_probe::ActionSpec& spec,
                        uint64_t* id) override {
    Record("CreateAction " + spec.name + " (" + spec.localized + ") " +
           std::to_string(spec.subactions.size()) + " hands");
    *id = 2000 + actions.size();
    actions[*id] = spec.name;
    return XR_SUCCESS;
  }
  XrResult SuggestBindings(
      XrPath profile,
      const std::vector<xr_probe::SuggestedBinding>& bindings) override {
    const auto name = names[profile];
    if (rejected_profiles.count(name) > 0) return XR_ERROR_PATH_UNSUPPORTED;
    auto& list = suggested[name];
    for (const auto& binding : bindings) {
      list.push_back({actions[binding.action], names[binding.binding]});
    }
    return XR_SUCCESS;
  }
  XrResult AttachActionSets(uint64_t) override {
    Record("AttachActionSets");
    attached = true;
    return XR_SUCCESS;
  }
  XrResult SyncActions(uint64_t) override {
    syncs++;
    return XR_SUCCESS;
  }
  int HandOf(XrPath path) {
    return names[path] == "/user/hand/left" ? 0 : 1;
  }
  XrResult GetBoolean(uint64_t action, XrPath hand,
                      xr_probe::BoolState* out) override {
    const auto& h = hands[HandOf(hand)];
    const auto& name = actions[action];
    out->active = true;
    out->value = name == "seek_press" ? h.seek_press
                 : name == "recenter" ? h.recenter
                 : name == "exit"     ? h.exit
                                      : false;
    return XR_SUCCESS;
  }
  XrResult GetFloat(uint64_t, XrPath hand, xr_probe::FloatState* out) override {
    out->active = true;
    out->value = hands[HandOf(hand)].play;
    return XR_SUCCESS;
  }
  XrResult GetVector2(uint64_t, XrPath hand,
                      xr_probe::Vector2State* out) override {
    out->active = true;
    out->x = hands[HandOf(hand)].seek_x;
    return XR_SUCCESS;
  }
  XrResult ApplyHaptic(uint64_t, XrPath hand, float amplitude,
                       XrDuration) override {
    haptics.push_back({HandOf(hand), amplitude});
    return XR_SUCCESS;
  }
  XrResult CurrentProfile(XrPath hand, std::string* profile) override {
    *profile = current_profile[HandOf(hand)];
    return XR_SUCCESS;
  }

  void DestroySession() override {
    Record("DestroySession");
    journal->Add("-session");
  }
  void DestroyInstance() override {
    Record("DestroyInstance");
    journal->Add("-instance");
  }
  std::string ResultName(XrResult result) override {
    return "XrResult " + std::to_string(static_cast<int>(result));
  }

  int Count(const std::string& call) const {
    int n = 0;
    for (const auto& c : calls) n += c == call ? 1 : 0;
    return n;
  }
  // The index of the first |call| after position |from|, -1 when none
  int Find(const std::string& call, int from = 0) const {
    for (int i = from; i < static_cast<int>(calls.size()); i++) {
      if (calls[i] == call) return i;
    }
    return -1;
  }

  xr_probe::Layers last_layers;
  std::vector<xr_probe::Layers> all_layers;

 private:
  std::deque<xr_probe::Event> pending;
  bool round_open = false;
  std::map<std::string, XrPath> paths;
  std::map<XrPath, std::string> names;
  std::map<uint64_t, std::string> actions;
};

class FakeGpu : public xr_probe::Gpu {
 public:
  xr_probe::AdapterId adapter{42, 0};
  uint32_t vendor = 0x10de;
  uint32_t device = 0x2820;
  bool open_ok = true;
  bool open = false;
  bool closed = false;
  int eyes_drawn[2] = {0, 0};
  int screens_drawn = 0;
  int wrapped = 0;
  int unwrapped = 0;
  int strip_uploads = 0;
  std::vector<uint8_t> last_strip;
  xr_probe::FrameInfo content;
  std::vector<double> times;
  std::vector<xr_probe::ContentRequest> contents;
  // Draws per mode of the content set
  std::map<xr_probe::Mode, int> draws_by_mode;
  ProjectionView last_view[2];
  std::shared_ptr<Journal> journal;

  bool Open(xr_probe::GpuInfo* info, std::string* error) override {
    if (!open_ok) {
      *error = "no device";
      return false;
    }
    open = true;
    info->adapter_name = "Fake GPU";
    info->adapter = adapter;
    info->vendor_id = vendor;
    info->device_id = device;
    info->feature_level = 0xb000;
    return true;
  }
  void SetContent(const xr_probe::ContentRequest& next) override {
    contents.push_back(next);
  }
  bool WrapImages(const xr_probe::Swapchain&, std::string* path,
                  std::string*) override {
    wrapped++;
    *path = "direct";
    return true;
  }
  void UnwrapImages(uint64_t) override { unwrapped++; }
  bool DrawEye(const xr_probe::Swapchain&, uint32_t, int eye,
               const ProjectionView& view, double time, std::string*) override {
    eyes_drawn[eye]++;
    last_view[eye] = view;
    times.push_back(time);
    if (!contents.empty()) draws_by_mode[contents.back().mode]++;
    return true;
  }
  bool DrawScreen(const xr_probe::Swapchain&, uint32_t, std::string*) override {
    screens_drawn++;
    return true;
  }
  bool UploadStrip(const xr_probe::Swapchain&, uint32_t, const uint8_t* rgba,
                   uint32_t width, uint32_t height, std::string*) override {
    strip_uploads++;
    last_strip.assign(rgba, rgba + static_cast<size_t>(width) * height * 4);
    return true;
  }
  void Finish() override {}
  xr_probe::FrameInfo FrameNow() override { return content; }
  xr_probe::GpuMemory Memory() override { return memory; }
  void Close() override {
    closed = true;
    if (journal) journal->Add("gpu closed");
  }

  xr_probe::GpuMemory memory;
};

inline bool Logged(const std::vector<std::string>& log,
                   const std::string& part) {
  for (const auto& line : log) {
    if (line.find(part) != std::string::npos) return true;
  }
  return false;
}

}  // namespace fakes

#endif  // XR_FAKES_H_
