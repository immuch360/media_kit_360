// Immuch360: the tests of the headset probe that need no headset
// (IMMUCH360-NOTE.md, patch 15), any C++17 compiler, no Windows header: run
// by the analyze job of the CI, and by MSVC in the xr job.
//
//   g++ -std=c++17 -Wall -Wextra -I.. -I../third_party/openxr/include
//     xr_probe_test.cc ../xr_controller.cc -o test && ./test
//
// What they pin:
// - the view of an eye (projection_view.h): an eye looking ahead with a
//   symmetric field of view is the window's view at yaw 0; a head turned
//   right or up looks where the window's yaw and pitch would; the roll turns
//   the picture; the four tangents of an asymmetric eye reach the pass as
//   they are; the recentering puts what the window showed ahead;
// - the uniforms of the window's view are unchanged by the four tangents;
// - the adapter check, the swapchain format and the refresh rate chosen;
// - the session's states with a fake runtime: begun once ready, ended once
//   stopping (a headset taken off), begun again once ready again, left once
//   exiting, a lost instance ends the loop without another call into the
//   runtime but the destroys, no runtime gives a clean "no OpenXR runtime".

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "../projection_view.h"
#include "../xr_controller.h"
#include "../xr_probe_core.h"

namespace {

int failures = 0;
int checks = 0;

void Check(bool ok, const std::string& what) {
  checks++;
  if (!ok) {
    failures++;
    std::printf("FAILED: %s\n", what.c_str());
  }
}

void Near(double expected, double actual, double tolerance,
          const std::string& what) {
  Check(std::abs(expected - actual) <= tolerance,
        what + ": expected " + std::to_string(expected) + ", got " +
            std::to_string(actual));
}

using projection_view::kDegree;
using projection_view::Quaternion;

// The rotation of |angle| radians about the unit axis (x, y, z), OpenXR's
// right handed convention
Quaternion About(double x, double y, double z, double angle) {
  const double s = std::sin(angle / 2);
  return {x * s, y * s, z * s, std::cos(angle / 2)};
}

Quaternion Times(Quaternion a, Quaternion b) {
  return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
          a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
          a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
          a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

// The direction of the sphere the ray (x, y, 1) of the pass's view goes to
void Ray(const float rows[9], double x, double y, double out[3]) {
  const double v[3] = {x, y, 1.0};
  for (int i = 0; i < 3; i++) {
    out[i] = rows[i * 3] * v[0] + rows[i * 3 + 1] * v[1] + rows[i * 3 + 2] * v[2];
  }
  const double n = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
  for (int i = 0; i < 3; i++) out[i] /= n;
}

double Longitude(const double d[3]) { return std::atan2(d[0], d[2]) / kDegree; }
double Latitude(const double d[3]) { return std::asin(d[1]) / kDegree; }

void TestViews() {
  // The window's view at yaw 30, pitch 10, 90 degrees high on a 2:1 output
  ProjectionView window;
  window.yaw = 30;
  window.pitch = 10;
  window.fov = 90;
  const auto w = projection_view::UniformsOf(window, 2000, 1000);
  Near(-2.0, w.tangents[0], 1e-6, "window: left tangent, tan 45 times the aspect");
  Near(2.0, w.tangents[1], 1e-6, "window: right tangent");
  Near(1.0, w.tangents[2], 1e-6, "window: up tangent");
  Near(-1.0, w.tangents[3], 1e-6, "window: down tangent");
  // pixels per radian as before the four tangents: (height / 2) / tan(fov / 2)
  Near(500.0, w.pixels_per_radian, 1e-6, "window: pixels per radian");
  // Column major: column 2 (indices 6 to 8) is where the centre looks
  double rows[9];
  projection_view::YawPitchRows(30, 10, rows);
  for (int i = 0; i < 9; i++) {
    Near(rows[(i % 3) * 3 + i / 3], w.columns[i], 1e-6, "window: column major");
  }

  // An eye looking ahead, symmetric: the window's view at yaw 0, pitch 0
  ProjectionView eye;
  const double a = std::atan(1.0);
  projection_view::EyeView({}, -a, a, a, -a, 0.0, &eye);
  for (int i = 0; i < 9; i++) {
    Near(i % 4 == 0 ? 1.0 : 0.0, eye.rows[i], 1e-6, "ahead: identity");
  }
  Near(-1.0, eye.tangents[0], 1e-6, "ahead: tan of the left angle");
  Near(1.0, eye.tangents[2], 1e-6, "ahead: tan of the up angle");

  // The head turned 40 degrees to the right: in OpenXR a rotation about +y by
  // -40 degrees. Its centre looks at longitude 40, like the window at yaw 40.
  projection_view::EyeView(About(0, 1, 0, -40 * kDegree), -a, a, a, -a, 0.0,
                           &eye);
  double d[3];
  Ray(eye.rows, 0, 0, d);
  Near(40.0, Longitude(d), 1e-4, "turned right: longitude");
  Near(0.0, Latitude(d), 1e-4, "turned right: latitude");
  projection_view::YawPitchRows(40, 0, rows);
  for (int i = 0; i < 9; i++) {
    Near(rows[i], eye.rows[i], 1e-5, "turned right: the window's rows at yaw 40");
  }
  Near(40.0, projection_view::HeadYawDegrees(About(0, 1, 0, -40 * kDegree)),
       1e-4, "head yaw of a head turned right");

  // The head up 25 degrees (about +x by +25 in OpenXR): latitude 25
  projection_view::EyeView(About(1, 0, 0, 25 * kDegree), -a, a, a, -a, 0.0,
                           &eye);
  Ray(eye.rows, 0, 0, d);
  Near(25.0, Latitude(d), 1e-4, "looking up: latitude");
  projection_view::YawPitchRows(0, 25, rows);
  for (int i = 0; i < 9; i++) {
    Near(rows[i], eye.rows[i], 1e-5, "looking up: the window's rows at pitch 25");
  }

  // Turned right 40 then up 25 (yaw first, as a head does): the window's view
  const auto turned_up =
      Times(About(0, 1, 0, -40 * kDegree), About(1, 0, 0, 25 * kDegree));
  projection_view::EyeView(turned_up, -a, a, a, -a, 0.0, &eye);
  projection_view::YawPitchRows(40, 25, rows);
  for (int i = 0; i < 9; i++) {
    Near(rows[i], eye.rows[i], 1e-5, "yaw then pitch: the window's rows");
  }
  Near(40.0, projection_view::HeadYawDegrees(turned_up), 1e-4,
       "head yaw ignores the pitch");

  // The head rolled 30 degrees to the left (about +z, counterclockwise as
  // seen from behind): the right of the view rises, so the ray to the right of
  // the centre ends above the horizon
  projection_view::EyeView(About(0, 0, 1, 30 * kDegree), -a, a, a, -a, 0.0,
                           &eye);
  Ray(eye.rows, 1, 0, d);
  Check(Latitude(d) > 15.0, "rolled left: the right of the view looks up");
  Ray(eye.rows, 0, 1, d);
  Check(Longitude(d) < -10.0, "rolled left: the top of the view looks left");

  // An asymmetric eye (a Quest 3 left eye, roughly): the tangents as they are
  const double left = -52 * kDegree, right = 41 * kDegree;
  const double up = 47 * kDegree, down = -53 * kDegree;
  projection_view::EyeView({}, left, right, up, down, 0.0, &eye);
  Near(std::tan(left), eye.tangents[0], 1e-6, "asymmetric: left");
  Near(std::tan(right), eye.tangents[1], 1e-6, "asymmetric: right");
  Near(std::tan(up), eye.tangents[2], 1e-6, "asymmetric: up");
  Near(std::tan(down), eye.tangents[3], 1e-6, "asymmetric: down");
  const auto u = projection_view::UniformsOf(eye, 2064, 2208);
  Near(std::tan(left), u.tangents[0], 1e-6, "asymmetric: uTanFov.x");
  Near(2208.0 / (std::tan(up) - std::tan(down)), u.pixels_per_radian, 1e-3,
       "asymmetric: pixels per radian");
  // The pass's ray at the left edge (t.x = 0) and at the top (t.y = 0): the
  // eye's angles
  Ray(eye.rows, eye.tangents[0], 0, d);
  Near(-52.0, Longitude(d), 1e-3, "asymmetric: the left edge");
  Ray(eye.rows, 0, eye.tangents[2], d);
  Near(47.0, Latitude(d), 1e-3, "asymmetric: the top edge");

  // Recentering: the head was turned 70 degrees right and the window looked
  // at yaw 10; the offset puts longitude 10 where the head looks
  const auto head = About(0, 1, 0, -70 * kDegree);
  const double offset = 10.0 - projection_view::HeadYawDegrees(head);
  projection_view::EyeView(head, -a, a, a, -a, offset, &eye);
  Ray(eye.rows, 0, 0, d);
  Near(10.0, Longitude(d), 1e-4, "recentered: the window's yaw ahead");
  // ...and a head that then turns 20 degrees more sees longitude 30
  projection_view::EyeView(About(0, 1, 0, -90 * kDegree), -a, a, a, -a, offset,
                           &eye);
  Ray(eye.rows, 0, 0, d);
  Near(30.0, Longitude(d), 1e-4, "recentered: the head turns the view");
}

void TestChoices() {
  using namespace xr_probe;
  Check(CheckAdapter({7, 0}, 0xb000, {7, 0}, 0xb000) == AdapterVerdict::kSame,
        "same LUID");
  Check(CheckAdapter({7, 0}, 0xb000, {8, 0}, 0xb000) ==
            AdapterVerdict::kOtherAdapter,
        "another LUID low part");
  Check(CheckAdapter({7, 1}, 0xb000, {7, 0}, 0xb000) ==
            AdapterVerdict::kOtherAdapter,
        "another LUID high part");
  Check(CheckAdapter({7, 0}, 0xa100, {7, 0}, 0xb000) ==
            AdapterVerdict::kFeatureLevelTooLow,
        "feature level 10_1 under 11_0");

  // SteamVR's D3D11 list has no plain UNORM RGBA (field reports)
  Check(ChooseSwapchainFormat({kDxgiB8G8R8A8UnormSrgb, kDxgiR8G8B8A8UnormSrgb,
                               10, 2}) == kDxgiR8G8B8A8UnormSrgb,
        "RGBA sRGB first");
  Check(ChooseSwapchainFormat({kDxgiB8G8R8A8UnormSrgb, kDxgiB8G8R8A8Unorm}) ==
            kDxgiB8G8R8A8UnormSrgb,
        "BGRA sRGB next");
  Check(ChooseSwapchainFormat({kDxgiR8G8B8A8Unorm}) == kDxgiR8G8B8A8Unorm,
        "UNORM only when no sRGB");
  Check(ChooseSwapchainFormat({10, 24}) == 0, "none of the four");

  const std::vector<float> quest3 = {72, 80, 90, 120};
  Near(72, ChooseRefreshRate(quest3, 90, 24), 1e-6, "24 fps on a Quest 3 at 90: 72");
  Near(72, ChooseRefreshRate(quest3, 90, 23.976), 1e-6, "23.976 fps: 72");
  Near(120, ChooseRefreshRate(quest3, 120, 24), 1e-6, "24 fps at 120 stays");
  Near(90, ChooseRefreshRate(quest3, 90, 30), 1e-6, "30 fps at 90 stays");
  Near(90, ChooseRefreshRate(quest3, 72, 29.97), 1e-6, "29.97 fps from 72: 90");
  Near(120, ChooseRefreshRate(quest3, 72, 60), 1e-6, "60 fps: 120");
  Near(90, ChooseRefreshRate(quest3, 90, 25), 1e-6, "25 fps: no multiple");
  Near(90, ChooseRefreshRate(quest3, 90, 0), 1e-6, "unknown rate");
  Near(90, ChooseRefreshRate({}, 90, 24), 1e-6, "no list");
  const std::vector<float> index = {80, 90, 120, 144};
  Near(120, ChooseRefreshRate(index, 90, 24), 1e-6, "24 fps on an Index: 120");

  SessionStateMachine machine;
  Check(machine.On(XR_SESSION_STATE_IDLE) == SessionAction::kNone, "idle");
  Check(machine.On(XR_SESSION_STATE_READY) == SessionAction::kBegin, "ready");
  machine.Began();
  Check(machine.On(XR_SESSION_STATE_FOCUSED) == SessionAction::kNone, "focused");
  Check(machine.visible(), "focused is visible");
  Check(machine.On(XR_SESSION_STATE_STOPPING) == SessionAction::kEnd, "stopping");
  machine.Ended();
  Check(machine.On(XR_SESSION_STATE_READY) == SessionAction::kBegin,
        "ready again");
  Check(machine.On(XR_SESSION_STATE_LOSS_PENDING) == SessionAction::kExit,
        "loss pending");
  Check(machine.On(XR_SESSION_STATE_EXITING) == SessionAction::kExit, "exiting");
}

xr_probe::Event State(XrSessionState state) {
  xr_probe::Event event;
  event.type = xr_probe::Event::Type::kSessionState;
  event.state = state;
  return event;
}

// A runtime that plays a script: one list of events per call of PollEvent
// rounds, the frames' results, and every call written down
class FakeRuntime : public xr_probe::Runtime {
 public:
  XrResult create_result = XR_SUCCESS;
  int unavailable_systems = 0;
  xr_probe::AdapterId adapter{42, 0};
  std::vector<int64_t> formats = {xr_probe::kDxgiR8G8B8A8UnormSrgb};
  // Per frame: the events polled before it
  std::deque<std::vector<xr_probe::Event>> script;
  // Per frame index: what xrWaitFrame returns (XR_SUCCESS by default)
  std::vector<std::pair<int, XrResult>> wait_results;
  bool render = true;
  std::vector<std::string> calls;
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

  XrResult CreateInstance(const std::vector<std::string>& optional,
                          xr_probe::RuntimeInfo* info) override {
    calls.push_back("CreateInstance");
    info->name = "Fake runtime";
    info->version = XR_MAKE_VERSION(1, 0, 1);
    info->offered = {"XR_KHR_D3D11_enable", "XR_FB_display_refresh_rate"};
    info->enabled = {"XR_KHR_D3D11_enable"};
    for (const auto& name : optional) info->enabled.push_back(name);
    return create_result;
  }
  XrResult GetSystem(xr_probe::SystemInfo* info) override {
    calls.push_back("GetSystem");
    if (unavailable_systems > 0) {
      unavailable_systems--;
      return XR_ERROR_FORM_FACTOR_UNAVAILABLE;
    }
    info->name = "Fake headset";
    info->eye_width[0] = info->eye_width[1] = 64;
    info->eye_height[0] = info->eye_height[1] = 64;
    info->max_swapchain_width = info->max_swapchain_height = 4096;
    info->adapter = adapter;
    info->min_feature_level = 0xb000;
    return XR_SUCCESS;
  }
  XrResult CreateSession(void*) override {
    calls.push_back("CreateSession");
    return XR_SUCCESS;
  }
  XrResult SwapchainFormats(std::vector<int64_t>* out) override {
    *out = formats;
    return XR_SUCCESS;
  }
  XrResult CreateSwapchain(int64_t format, uint32_t width, uint32_t height,
                           xr_probe::Swapchain* out) override {
    calls.push_back("CreateSwapchain");
    out->id = next_swapchain++;
    out->format = format;
    out->width = width;
    out->height = height;
    out->images = {nullptr, nullptr, nullptr};
    return XR_SUCCESS;
  }
  void DestroySwapchain(uint64_t) override { calls.push_back("DestroySwapchain"); }
  // One round of polls (until it returns false) takes the next entry of the
  // script; once the script is over, EXITING after a while, as a runtime that
  // closes
  bool PollEvent(xr_probe::Event* event) override {
    if (!round_open) {
      round_open = true;
      if (!script.empty()) {
        pending.assign(script.front().begin(), script.front().end());
        script.pop_front();
      } else if (++polls_without_events > 300) {
        polls_without_events = 0;
        pending.push_back(State(XR_SESSION_STATE_EXITING));
      }
    }
    if (pending.empty()) {
      round_open = false;
      return false;
    }
    *event = pending.front();
    pending.pop_front();
    return true;
  }
  XrResult BeginSession() override {
    calls.push_back("BeginSession");
    return XR_SUCCESS;
  }
  XrResult EndSession() override {
    calls.push_back("EndSession");
    return XR_SUCCESS;
  }
  XrResult RequestExitSession() override {
    calls.push_back("RequestExitSession");
    exit_requested = true;
    if (exit_on_request) {
      xr_probe::Event stopping;
      stopping.type = xr_probe::Event::Type::kSessionState;
      stopping.state = XR_SESSION_STATE_STOPPING;
      xr_probe::Event exiting = stopping;
      exiting.state = XR_SESSION_STATE_EXITING;
      script.push_front({stopping, exiting});
    }
    return XR_SUCCESS;
  }
  XrResult WaitFrame(xr_probe::FrameState* state) override {
    const int index = frames++;
    calls.push_back("WaitFrame");
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
      views[i].pose.orientation = {0, 0, 0, 1};
      views[i].pose.position = {i == 0 ? -0.03f : 0.03f, 1.6f, 0};
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
    return XR_SUCCESS;
  }
  XrResult RefreshRates(std::vector<float>* out, float* current) override {
    if (!has_rates) return XR_ERROR_FUNCTION_UNSUPPORTED;
    *out = rates;
    *current = rate;
    return XR_SUCCESS;
  }
  XrResult RequestRefreshRate(float wanted) override {
    calls.push_back("RequestRefreshRate " + std::to_string(static_cast<int>(wanted)));
    rate = wanted;
    return XR_SUCCESS;
  }
  void DestroySession() override { calls.push_back("DestroySession"); }
  void DestroyInstance() override { calls.push_back("DestroyInstance"); }
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

 private:
  std::deque<xr_probe::Event> pending;
  bool round_open = false;
};

class FakeGpu : public xr_probe::Gpu {
 public:
  xr_probe::AdapterId adapter{42, 0};
  bool open = false;
  bool closed = false;
  int eyes_drawn[2] = {0, 0};
  int screens_drawn = 0;
  int wrapped = 0;
  int unwrapped = 0;
  xr_probe::Content content;
  std::vector<double> times;

  bool Open(xr_probe::GpuInfo* info, std::string*) override {
    open = true;
    info->adapter_name = "Fake GPU";
    info->adapter = adapter;
    info->feature_level = 0xb000;
    return true;
  }
  bool WrapImages(const xr_probe::Swapchain&, std::string* path,
                  std::string*) override {
    wrapped++;
    *path = "direct";
    return true;
  }
  void UnwrapImages(uint64_t) override { unwrapped++; }
  bool DrawEye(const xr_probe::Swapchain&, uint32_t, int eye, const XrView&,
               double, double time, std::string*) override {
    eyes_drawn[eye]++;
    times.push_back(time);
    return true;
  }
  bool DrawScreen(const xr_probe::Swapchain&, uint32_t, std::string*) override {
    screens_drawn++;
    return true;
  }
  void Finish() override {}
  xr_probe::Content ContentNow() override { return content; }
  xr_probe::GpuMemory Memory() override { return {}; }
  void Close() override { closed = true; }
};


xr_probe::Options TestOptions(std::vector<std::string>* log) {
  xr_probe::Options options;
  options.mode = xr_probe::Mode::kPattern;
  options.fps = 24;
  // A fake clock: every call is 1 ms later
  auto clock = std::make_shared<double>(0.0);
  options.now = [clock] { return *clock += 0.001; };
  options.sleep_ms = [clock](int ms) { *clock += ms / 1000.0; };
  options.log = [log](const std::string& line) { log->push_back(line); };
  options.system_wait_seconds = 5;
  return options;
}

bool Logged(const std::vector<std::string>& log, const std::string& part) {
  for (const auto& line : log) {
    if (line.find(part) != std::string::npos) return true;
  }
  return false;
}

void TestSessionWithHeadsetTakenOff() {
  FakeRuntime runtime;
  FakeGpu gpu;
  // Ready, then focused for some frames; the headset taken off: visible, then
  // stopping and idle (Meta Link stops the session); put back on: ready and
  // focused again; then the runtime closes
  runtime.script = {
      {State(XR_SESSION_STATE_IDLE), State(XR_SESSION_STATE_READY)},
      {State(XR_SESSION_STATE_SYNCHRONIZED), State(XR_SESSION_STATE_VISIBLE),
       State(XR_SESSION_STATE_FOCUSED)},
      {}, {}, {}, {},
      {State(XR_SESSION_STATE_VISIBLE)},
      {}, {},
      {State(XR_SESSION_STATE_STOPPING), State(XR_SESSION_STATE_IDLE)},
      {}, {},
      {State(XR_SESSION_STATE_READY)},
      {State(XR_SESSION_STATE_FOCUSED)},
      {}, {},
      {State(XR_SESSION_STATE_STOPPING)},
      {State(XR_SESSION_STATE_IDLE), State(XR_SESSION_STATE_EXITING)},
  };
  std::vector<std::string> log;
  xr_probe::Controller controller(&runtime, &gpu, TestOptions(&log));
  controller.Run();
  Check(runtime.Count("BeginSession") == 2, "begun twice: before and after the headset was off");
  Check(runtime.Count("EndSession") == 2, "ended twice");
  const int first_end = runtime.Find("EndSession");
  const int second_begin = runtime.Find("BeginSession", runtime.Find("BeginSession") + 1);
  Check(first_end >= 0 && second_begin > first_end, "ended before it began again");
  Check(runtime.Find("WaitFrame", first_end) > second_begin || runtime.Find("WaitFrame", first_end) == -1,
        "no frame between the end and the next begin");
  Check(runtime.Count("WaitFrame") > 0, "frames ran");
  Check(gpu.eyes_drawn[0] > 0 && gpu.eyes_drawn[0] == gpu.eyes_drawn[1], "both eyes drawn, as often");
  Check(runtime.layers_with_projection > 0, "a projection layer ended the frames");
  Check(runtime.Count("DestroySession") == 1 && runtime.Count("DestroyInstance") == 1, "destroyed once");
  Check(runtime.Find("DestroySession") < runtime.Find("DestroyInstance"), "the session before the instance");
  Check(gpu.unwrapped == 2 && gpu.closed, "the eye images unwrapped and the GPU side closed");
  Check(runtime.Count("RequestRefreshRate 72") >= 1, "24 fps on 72, 90 or 120 Hz: 72 asked");
  const auto status = controller.StatusNow();
  Check(status.phase == "stopped", "stopped at the end");
  Check(status.reason == "the session ended", "reason: " + status.reason);
  Check(Logged(log, "recentered"), "recentered at the first frame");
  Check(Logged(log, "swapchain formats offered: R8G8B8A8_UNORM_SRGB; chosen: R8G8B8A8_UNORM_SRGB"),
        "formats logged");
  // The pattern's bar moves at 24 steps a second: its times are multiples of 1/24
  bool stepped = !gpu.times.empty();
  for (const auto time : gpu.times) {
    const double steps = time * 24.0;
    stepped = stepped && std::abs(steps - std::round(steps)) < 1e-6;
  }
  Check(stepped, "the pattern's time stepped at 24 fps");
}

void TestInstanceLost() {
  FakeRuntime runtime;
  FakeGpu gpu;
  runtime.script = {
      {State(XR_SESSION_STATE_READY)},
      {State(XR_SESSION_STATE_FOCUSED)},
  };
  // SteamVR killed: the frame after a few returns XR_ERROR_INSTANCE_LOST
  runtime.wait_results = {{5, XR_ERROR_INSTANCE_LOST}};
  std::vector<std::string> log;
  xr_probe::Controller controller(&runtime, &gpu, TestOptions(&log));
  controller.Run();
  Check(runtime.Count("WaitFrame") == 6, "no frame after the loss");
  Check(runtime.Count("EndSession") == 0, "no xrEndSession on a lost instance");
  Check(runtime.Count("DestroyInstance") == 1, "the instance destroyed anyway");
  const auto status = controller.StatusNow();
  Check(status.instance_lost, "instance lost in the status");
  Check(status.phase == "stopped", "stopped");
  Check(status.reason.find("lost") != std::string::npos ||
            status.reason.find("frame loop") != std::string::npos,
        "reason: " + status.reason);
}

void TestInstanceLossPendingEvent() {
  FakeRuntime runtime;
  FakeGpu gpu;
  xr_probe::Event loss;
  loss.type = xr_probe::Event::Type::kInstanceLossPending;
  runtime.script = {{State(XR_SESSION_STATE_READY)}, {}, {}, {loss}};
  std::vector<std::string> log;
  xr_probe::Controller controller(&runtime, &gpu, TestOptions(&log));
  controller.Run();
  Check(controller.StatusNow().instance_lost, "loss pending: lost");
  Check(runtime.Count("DestroyInstance") == 1, "loss pending: destroyed");
}

void TestNoRuntime() {
  FakeRuntime runtime;
  runtime.create_result = XR_ERROR_RUNTIME_UNAVAILABLE;
  FakeGpu gpu;
  std::vector<std::string> log;
  xr_probe::Controller controller(&runtime, &gpu, TestOptions(&log));
  controller.Run();
  const auto status = controller.StatusNow();
  Check(status.reason.rfind("no OpenXR runtime is active on this computer", 0) == 0,
        "no runtime: " + status.reason);
  Check(runtime.Count("DestroyInstance") == 0, "no instance to destroy");
  Check(gpu.closed, "the GPU side closed");
}

void TestOtherAdapter() {
  FakeRuntime runtime;
  runtime.adapter = {43, 0};
  FakeGpu gpu;
  std::vector<std::string> log;
  xr_probe::Controller controller(&runtime, &gpu, TestOptions(&log));
  controller.Run();
  Check(controller.StatusNow().reason == "the headset is on another graphics card",
        "other adapter: " + controller.StatusNow().reason);
  Check(runtime.Count("CreateSession") == 0, "no session on the wrong card");
  Check(runtime.Count("DestroyInstance") == 1, "the instance destroyed");
}

void TestWaitingForTheHeadset() {
  FakeRuntime runtime;
  runtime.unavailable_systems = 3;
  runtime.script = {{State(XR_SESSION_STATE_READY)}, {}, {State(XR_SESSION_STATE_EXITING)}};
  FakeGpu gpu;
  std::vector<std::string> log;
  xr_probe::Controller controller(&runtime, &gpu, TestOptions(&log));
  controller.Run();
  Check(runtime.Count("GetSystem") == 4, "asked again until the headset came");
  Check(Logged(log, "phase: waiting for the headset"), "waiting logged");
  Check(runtime.Count("BeginSession") == 1, "then begun");

  FakeRuntime never;
  never.unavailable_systems = 1000000;
  FakeGpu gpu2;
  std::vector<std::string> log2;
  xr_probe::Controller controller2(&never, &gpu2, TestOptions(&log2));
  controller2.Run();
  Check(controller2.StatusNow().reason == "no headset connected to the runtime",
        "no headset: " + controller2.StatusNow().reason);
}

void TestStopFromTheApp() {
  FakeRuntime runtime;
  runtime.script = {{State(XR_SESSION_STATE_READY)}, {State(XR_SESSION_STATE_FOCUSED)}};
  for (int i = 0; i < 50; i++) runtime.script.push_back({});
  FakeGpu gpu;
  std::vector<std::string> log;
  auto options = TestOptions(&log);
  xr_probe::Controller* self = nullptr;
  int frames = 0;
  // The app asks to stop at the 10th line of the log written after the start
  options.log = [&](const std::string& line) {
    log.push_back(line);
    if (line.rfind("stats", 0) == 0 || ++frames == 30) {
      if (self) self->RequestStop();
    }
  };
  options.stats_every = 0.01;
  xr_probe::Controller controller(&runtime, &gpu, options);
  self = &controller;
  controller.Run();
  Check(runtime.Count("RequestExitSession") == 1, "the exit asked of the runtime");
  Check(runtime.Find("EndSession") > runtime.Find("RequestExitSession"), "ended after the request");
  Check(controller.StatusNow().reason == "stopped by the app", "reason: " + controller.StatusNow().reason);
}

void TestScreenMode() {
  FakeRuntime runtime;
  runtime.script = {{State(XR_SESSION_STATE_READY)}, {State(XR_SESSION_STATE_FOCUSED)}, {}, {}, {}, {},
                    {State(XR_SESSION_STATE_STOPPING)}, {State(XR_SESSION_STATE_EXITING)}};
  FakeGpu gpu;
  gpu.content.has_frame = true;
  gpu.content.frame_width = 3840;
  gpu.content.frame_height = 1080;
  gpu.content.frame_count = 1;
  std::vector<std::string> log;
  auto options = TestOptions(&log);
  options.mode = xr_probe::Mode::kScreen;
  options.stereo = xr_probe::ScreenStereo::kLeftRight;
  xr_probe::Controller controller(&runtime, &gpu, options);
  controller.Run();
  Check(gpu.eyes_drawn[0] == 0, "no eye pass for a screen");
  Check(gpu.screens_drawn == 1, "the same frame drawn once: " + std::to_string(gpu.screens_drawn));
  Check(runtime.quad_layers > 0 && runtime.quad_layers % 2 == 0, "two quads a frame for side by side");
  const auto& quads = runtime.last_layers.quads;
  Check(quads.size() == 2 && quads[0].eye == XR_EYE_VISIBILITY_LEFT && quads[1].eye == XR_EYE_VISIBILITY_RIGHT,
        "one quad per eye");
  if (quads.size() == 2) {
    Check(quads[0].rect.extent.width == 1920 && quads[1].rect.offset.x == 1920, "the halves of the frame");
    // 32:9 is full side by side: each eye is 16:9, the screen 2.4 m x 1.35 m
    Near(1.35, quads[0].size.height, 1e-3, "full side by side: 16:9 per eye");
    Near(-2.5, quads[0].pose.position.z, 1e-3, "2.5 m ahead");
    Near(1.6, quads[0].pose.position.y, 1e-3, "at eye height");
  }
}

}  // namespace

int main() {
  TestViews();
  TestChoices();
  TestSessionWithHeadsetTakenOff();
  TestInstanceLost();
  TestInstanceLossPendingEvent();
  TestNoRuntime();
  TestOtherAdapter();
  TestWaitingForTheHeadset();
  TestStopFromTheApp();
  TestScreenMode();
  std::printf("%d checks, %d failed\n", checks, failures);
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
