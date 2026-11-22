// Immuch360: the tests of the headset view's version 1 that need no headset
// (IMMUCH360-NOTE.md, patch 15), any C++17 compiler, no Windows header: run
// by the analyze job of the CI with ASan and UBSan, and by MSVC in the xr job.
//
//   g++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -I..
//     -I../../common/renderer -I../third_party/openxr/include xr_v1_test.cc
//     ../xr_controller.cc ../xr_input.cc ../xr_session_core.cc -lpthread
//     -o test && ./test
//
// What they pin: the session core's lifecycle (start and stop cycles, a stop
// while waiting, during the preparation, while the runtime hangs, the
// teardown), the content changing with no new session, the flat screen's six
// stereo cases, the controllers' bindings and gestures, the recentering
// upright and lying down, the strip, every stop code, the visibility events,
// the event queue and the guess of a runtime from its manifest.

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "projection_view.h"
#include "../xr_controller.h"
#include "../xr_input.h"
#include "../xr_probe_core.h"
#include "../xr_session_core.h"
#include "xr_fakes.h"

namespace {

using fakes::Check;
using fakes::FakeGpu;
using fakes::FakeRuntime;
using fakes::Journal;
using fakes::Logged;
using fakes::Near;
using fakes::State;
using projection_view::kDegree;
using xr_probe::Mode;
using xr_probe::StopReason;

std::string Code(StopReason reason) { return xr_probe::StopReasonCode(reason); }

// A fake clock: every call 1 ms later, sleeps added, 1/90 s per frame when
// the runtime has it
xr_probe::Options TestOptions(std::vector<std::string>* log,
                              std::shared_ptr<double> clock = nullptr) {
  xr_probe::Options options;
  options.content.mode = Mode::kSphere;
  options.content.handle = 7;
  options.content.fps = 24;
  if (!clock) clock = std::make_shared<double>(0.0);
  options.now = [clock] { return *clock += 0.001; };
  options.sleep_ms = [clock](int ms) { *clock += ms / 1000.0; };
  options.log = [log](const std::string& line) { log->push_back(line); };
  options.system_wait_seconds = 5;
  return options;
}

// Ready and focused, then |frames| rounds of nothing, then EXITING
void Script(FakeRuntime* runtime, int frames) {
  runtime->script = {{State(XR_SESSION_STATE_IDLE), State(XR_SESSION_STATE_READY)},
                     {State(XR_SESSION_STATE_SYNCHRONIZED), State(XR_SESSION_STATE_VISIBLE),
                      State(XR_SESSION_STATE_FOCUSED)}};
  runtime->exit_after_empty = frames;
}

bool WaitFor(const std::function<bool()>& condition, int ms) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < until) {
    if (condition()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return condition();
}

// --- the session core -------------------------------------------------------

struct CoreRig {
  std::shared_ptr<Journal> journal = std::make_shared<Journal>();
  std::function<void(FakeRuntime*, FakeGpu*)> configure;
  std::shared_ptr<std::atomic<bool>> destroyed = std::make_shared<std::atomic<bool>>(false);
  std::atomic<int> made{0};

  xr_probe::PartsFactory Factory() {
    return [this](const xr_probe::StartRequest&, std::shared_ptr<xr_probe::SourceGate>) {
      xr_probe::ViewParts parts;
      auto runtime = std::make_unique<FakeRuntime>();
      auto gpu = std::make_unique<FakeGpu>();
      runtime->journal = journal;
      gpu->journal = journal;
      runtime->destroyed = destroyed;
      Script(runtime.get(), -1);
      if (configure) configure(runtime.get(), gpu.get());
      parts.runtime = std::move(runtime);
      parts.gpu = std::move(gpu);
      parts.log = [](const std::string&) {};
      made++;
      return parts;
    };
  }
};

xr_probe::CoreOptions FastCore() {
  xr_probe::CoreOptions options;
  options.stats_every = 0.5;
  options.build = "test";
  return options;
}

xr_probe::StartRequest Request(Mode mode = Mode::kSphere) {
  xr_probe::StartRequest request;
  request.content.mode = mode;
  request.content.handle = mode == Mode::kSphere ? 7 : 0;
  request.content.fps = 30;
  request.log_path = "headset.log";
  request.app_version = "3.4.3-48";
  return request;
}

// Each view's calls: every swapchain made destroyed once, all before the
// session, the session before the instance, the GPU side closed last
bool DestroyedInOrder(const std::vector<std::string>& lines, int* views) {
  *views = 0;
  bool ok = true;
  std::vector<std::string> view;
  const auto check = [&](const std::vector<std::string>& part) {
    if (part.empty()) return;
    (*views)++;
    std::map<std::string, int> made, gone;
    int session_at = -1, instance_at = -1, closed_at = -1, last_swapchain_gone = -1;
    for (int i = 0; i < static_cast<int>(part.size()); i++) {
      const auto& line = part[i];
      if (line.rfind("+swapchain ", 0) == 0) {
        made[line.substr(11, line.find(' ', 11) - 11)]++;
      } else if (line.rfind("-swapchain ", 0) == 0) {
        gone[line.substr(11)]++;
        last_swapchain_gone = i;
      } else if (line == "-session") {
        ok = ok && session_at < 0;
        session_at = i;
      } else if (line == "-instance") {
        ok = ok && instance_at < 0;
        instance_at = i;
      } else if (line == "gpu closed") {
        closed_at = i;
      }
    }
    ok = ok && made == gone;
    for (const auto& [id, n] : gone) ok = ok && n == 1;
    ok = ok && session_at > last_swapchain_gone && instance_at > session_at && closed_at > instance_at;
  };
  for (const auto& line : lines) {
    if (line == "CreateInstance") {
      check(view);
      view.clear();
    }
    view.push_back(line);
  }
  check(view);
  return ok;
}

void TestStartStopCycles() {
  CoreRig rig;
  xr_probe::SessionCore core(rig.Factory(), FastCore());
  int stopped = 0;
  uint64_t last = 0;
  for (int i = 0; i < 50; i++) {
    const auto answer = core.Start(Request());
    if (!answer.ok) {
      Check(false, "cycle " + std::to_string(i) + ": " + answer.detail);
      break;
    }
    WaitFor([&] { return core.Poll(0).status.frames > 2; }, 2000);
    stopped += core.Stop(3000) ? 1 : 0;
    const auto poll = core.Poll(0);
    if (poll.status.reason != StopReason::kStoppedByApp) {
      Check(false, "cycle " + std::to_string(i) + " reason " + Code(poll.status.reason));
    }
    last = poll.last_sequence;
  }
  Check(stopped == 50, "50 views started and stopped: " + std::to_string(stopped));
  int views = 0;
  Check(DestroyedInOrder(rig.journal->lines(), &views), "every handle destroyed once, in order");
  Check(views == 50, "50 views in the journal: " + std::to_string(views));
  Check(rig.journal->Count("-session") == 50 && rig.journal->Count("-instance") == 50,
        "50 sessions and instances destroyed");
  Check(!core.Poll(0).running, "none runs at the end");
  (void)last;
}

void TestStopWhileWaitingForTheHeadset() {
  CoreRig rig;
  rig.configure = [](FakeRuntime* runtime, FakeGpu*) { runtime->unavailable_systems = 1000000; };
  xr_probe::SessionCore core(rig.Factory(), FastCore());
  Check(core.Start(Request()).ok, "waiting: started");
  WaitFor([&] { return core.Poll(0).status.phase == "waiting for the headset"; }, 2000);
  Check(core.Poll(0).status.phase == "waiting for the headset", "waiting: the phase says so");
  const auto from = std::chrono::steady_clock::now();
  Check(core.Stop(3000), "waiting: stopped");
  Check(std::chrono::steady_clock::now() - from < std::chrono::milliseconds(1500), "waiting: stopped at once");
  const auto poll = core.Poll(0);
  Check(poll.status.reason == StopReason::kStoppedByApp, "waiting: stoppedByApp, " + Code(poll.status.reason));
  Check(rig.journal->Count("CreateSession") == 0, "waiting: no session");
  Check(rig.journal->Count("-instance") == 1, "waiting: the instance destroyed");
}

void TestStopDuringPrepare() {
  CoreRig rig;
  auto blocker = std::make_shared<fakes::Blocker>();
  rig.configure = [blocker](FakeRuntime* runtime, FakeGpu*) { runtime->system_blocker = blocker; };
  xr_probe::SessionCore core(rig.Factory(), FastCore());
  Check(core.Start(Request()).ok, "prepare: started");
  Check(blocker->WaitBlocked(2000), "prepare: inside xrGetSystem");
  bool stopped = false;
  std::thread stopper([&] { stopped = core.Stop(3000); });
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  blocker->Release();
  stopper.join();
  Check(stopped, "prepare: the stop ended the view");
  Check(core.Poll(0).status.reason == StopReason::kStoppedByApp, "prepare: stoppedByApp");
  Check(rig.journal->Count("CreateSession") == 0, "prepare: no session made after the stop");
}

void TestStopWhileTheRuntimeHangs() {
  CoreRig rig;
  auto blocker = std::make_shared<fakes::Blocker>();
  std::atomic<int> views{0};
  rig.configure = [&](FakeRuntime* runtime, FakeGpu*) {
    // The first view hangs in its fourth xrWaitFrame
    if (views++ == 0) {
      runtime->block_at_frame = 3;
      runtime->blocker = blocker;
    }
  };
  xr_probe::SessionCore core(rig.Factory(), FastCore());
  Check(core.Start(Request()).ok, "hang: started");
  Check(blocker->WaitBlocked(3000), "hang: the runtime blocks in xrWaitFrame");
  const auto from = std::chrono::steady_clock::now();
  Check(!core.Stop(200), "hang: the stop does not end it");
  const auto waited = std::chrono::steady_clock::now() - from;
  Check(waited < std::chrono::milliseconds(1000), "hang: the stop kept to its bound");
  auto poll = core.Poll(0);
  Check(poll.running, "hang: still running");
  Check(poll.status.reason == StopReason::kRuntimeNotAnswering, "hang: runtimeNotAnswering in the state");
  Check(poll.status.phase == "not answering", "hang: phase " + poll.status.phase);
  const auto refused = core.Start(Request());
  Check(!refused.ok && refused.reason == StopReason::kRuntimeNotAnswering,
        "hang: the next start answers runtimeNotAnswering, " + Code(refused.reason));
  blocker->Release();
  Check(WaitFor([&] { return !core.Poll(0).running; }, 3000), "hang: released, the view ends");
  Check(core.Poll(0).status.reason == StopReason::kRuntimeNotAnswering, "hang: the code stays");
  const auto again = core.Start(Request());
  Check(again.ok, "hang: the next start works");
  WaitFor([&] { return core.Poll(0).status.frames > 2; }, 2000);
  Check(core.Stop(3000), "hang: the new view stops");
  int count = 0;
  Check(DestroyedInOrder(rig.journal->lines(), &count) && count == 2, "hang: both views cleaned up");
}

void TestDetachedTeardown() {
  CoreRig rig;
  auto blocker = std::make_shared<fakes::Blocker>();
  rig.configure = [blocker](FakeRuntime* runtime, FakeGpu*) {
    runtime->block_at_frame = 2;
    runtime->blocker = blocker;
  };
  {
    auto core = std::make_unique<xr_probe::SessionCore>(rig.Factory(), FastCore());
    Check(core->Start(Request()).ok, "teardown: started");
    Check(blocker->WaitBlocked(3000), "teardown: the runtime hangs");
    core->Teardown(100);
    Check(core->detached() == 1, "teardown: the thread detached");
    const auto refused = core->Start(Request());
    Check(!refused.ok, "teardown: nothing starts afterwards");
    // The core goes with the thread still blocked: no std::terminate
  }
  blocker->Release();
  Check(WaitFor([&] { return rig.destroyed->load(); }, 3000), "teardown: the detached view ended on its own");
}

void TestAlreadyRunningAndSequences() {
  CoreRig rig;
  rig.configure = [](FakeRuntime* runtime, FakeGpu*) {
    // A profile event in each view: one event to count
    runtime->current_profile[1] = "/interaction_profiles/oculus/touch_controller";
    runtime->script.push_back({fakes::Simple(xr_probe::Event::Type::kInteractionProfileChanged)});
  };
  xr_probe::SessionCore core(rig.Factory(), FastCore());
  Check(core.Poll(0).status.phase == "never started", "poll before any view");
  Check(core.Start(Request()).ok, "sequences: started");
  const auto second = core.Start(Request());
  Check(!second.ok && second.reason == StopReason::kAlreadyRunning, "a second start: alreadyRunning");
  WaitFor([&] { return core.Poll(0).last_sequence >= 1; }, 2000);
  auto poll = core.Poll(0);
  Check(poll.events.size() == 1 && poll.events[0].kind == xr_probe::EventKind::kProfile &&
            poll.events[0].hand == 1 && poll.events[0].profile == "/interaction_profiles/oculus/touch_controller",
        "the profile event of the right hand");
  const auto first_last = poll.last_sequence;
  Check(core.Poll(first_last).events.empty(), "nothing after the last sequence");
  core.Stop(3000);
  Check(core.Start(Request()).ok, "sequences: started again");
  WaitFor([&] { return core.Poll(0).last_sequence > first_last; }, 2000);
  poll = core.Poll(first_last);
  Check(poll.events.size() == 1 && poll.events[0].sequence == first_last + 1,
        "the next view's events numbered on from the last one");
  core.Stop(3000);
}

void TestEventQueue() {
  xr_probe::EventQueue queue;
  int dropped = 0;
  for (int i = 0; i < 100; i++) {
    xr_probe::HeadsetEvent event;
    event.kind = xr_probe::EventKind::kSeek;
    event.seconds = i;
    dropped += queue.Push(event) ? 1 : 0;
  }
  const auto all = queue.Since(0);
  Check(all.size() == 64, "64 events kept");
  Check(!all.empty() && all.front().sequence == 37 && all.back().sequence == 100, "the oldest dropped");
  Check(queue.dropped() == 36 && dropped == 36, "36 dropped, counted");
  Check(queue.last() == 100, "last sequence 100");
  Check(queue.Since(90).size() == 10, "the 10 after sequence 90");
  bool rising = true;
  for (size_t i = 1; i < all.size(); i++) rising = rising && all[i].sequence == all[i - 1].sequence + 1;
  Check(rising, "sequences rising by one");
  xr_probe::EventQueue later(64, 501);
  later.Push({});
  Check(later.Since(500).size() == 1 && later.last() == 501, "a queue numbered from 501");
}

// --- content -----------------------------------------------------------------

void TestContentChanges() {
  FakeRuntime runtime;
  FakeGpu gpu;
  gpu.content = {true, 3840, 1080, 1};
  Script(&runtime, 70);
  std::vector<std::string> log;
  auto options = TestOptions(&log);
  xr_probe::Controller controller(&runtime, &gpu, options);
  auto* inbox = controller.inbox();
  runtime.on_frame = [&](int frame) {
    xr_probe::ContentRequest content;
    content.fps = 24;
    if (frame == 10) {
      content.mode = Mode::kScreen;
      content.handle = 8;
      content.stereo = xr_probe::Stereo::kLeftRight;
      inbox->SetContent(content);
    } else if (frame == 20) {
      content.mode = Mode::kIdle;
      inbox->SetContent(content);
    } else if (frame == 30) {
      content.mode = Mode::kSphere;
      content.handle = 9;
      inbox->SetContent(content);
    } else if (frame == 40) {
      inbox->SetRate(30);
    } else if (frame == 45) {
      content.mode = Mode::kPattern;
      content.fps = 30;
      inbox->SetContent(content);
    }
  };
  controller.Run();
  Check(runtime.Count("CreateSession") == 1 && runtime.Count("BeginSession") == 1, "content: one session");
  const auto lines = runtime.journal->lines();
  int eyes = 0, screens = 0;
  for (const auto& line : lines) {
    eyes += line.find("64x64") != std::string::npos ? 1 : 0;
    screens += line.find("3840x1080") != std::string::npos ? 1 : 0;
  }
  Check(eyes == 2, "content: the eye swapchains made once and kept: " + std::to_string(eyes));
  Check(screens == 1, "content: one screen swapchain");
  // The screen's swapchain (id 3) gone when the screen was left, before the end
  int screen_gone = -1, session_gone = -1;
  for (int i = 0; i < static_cast<int>(lines.size()); i++) {
    if (lines[i] == "-swapchain 3") screen_gone = i;
    if (lines[i] == "-session") session_gone = i;
  }
  Check(screen_gone >= 0 && screen_gone < session_gone, "content: the screen swapchain destroyed on leaving it");
  Check(Logged(log, "screen: swapchain destroyed"), "content: logged");
  Check(gpu.draws_by_mode[Mode::kSphere] > 0 && gpu.draws_by_mode[Mode::kIdle] > 0 &&
            gpu.draws_by_mode[Mode::kPattern] > 0,
        "content: sphere, idle and pattern drawn");
  Check(gpu.screens_drawn == 1, "content: the screen drawn once (one frame)");
  bool handles = false;
  for (const auto& content : gpu.contents) handles = handles || content.handle == 9;
  Check(handles, "content: the new player given to the GPU side");
  Check(runtime.Count("RequestRefreshRate 72") >= 1, "content: 24 fps asks 72 Hz");
  Check(runtime.Count("RequestRefreshRate 90") == 1, "content: 30 fps asks 90 Hz");
  int recentered = 0;
  for (const auto& line : log) recentered += line.rfind("recentered", 0) == 0 ? 1 : 0;
  Check(recentered >= 4, "content: recentered on each new source: " + std::to_string(recentered));
  Check(controller.StatusNow().mode == "pattern", "content: the mode in the state");
}

void TestRefreshMatchingOff() {
  FakeRuntime runtime;
  FakeGpu gpu;
  Script(&runtime, 20);
  std::vector<std::string> log;
  auto options = TestOptions(&log);
  options.match_refresh_rate = false;
  xr_probe::Controller controller(&runtime, &gpu, options);
  controller.Run();
  Check(runtime.Count("RequestRefreshRate 72") == 0, "matching off: no rate asked");
  Check(Logged(log, "matching to the content is off"), "matching off: logged");
}

void TestScreenLayouts() {
  using xr_probe::Squeeze;
  using xr_probe::Stereo;
  struct Case {
    Stereo stereo;
    Squeeze squeeze;
    uint32_t width, height;
    int quads;
    double eye_aspect;
    int32_t second_x, second_y, eye_width, eye_height;
  };
  const Case cases[] = {
      {Stereo::kMono, Squeeze::kFull, 1920, 1080, 1, 16.0 / 9.0, 0, 0, 1920, 1080},
      {Stereo::kMono, Squeeze::kHalf, 1920, 1080, 1, 16.0 / 9.0, 0, 0, 1920, 1080},
      {Stereo::kLeftRight, Squeeze::kFull, 3840, 1080, 2, 16.0 / 9.0, 1920, 0, 1920, 1080},
      {Stereo::kLeftRight, Squeeze::kHalf, 1920, 1080, 2, 16.0 / 9.0, 960, 0, 960, 1080},
      {Stereo::kTopBottom, Squeeze::kFull, 1920, 2160, 2, 16.0 / 9.0, 0, 1080, 1920, 1080},
      {Stereo::kTopBottom, Squeeze::kHalf, 1920, 1080, 2, 16.0 / 9.0, 0, 540, 1920, 540},
  };
  for (const auto& c : cases) {
    const std::string name = std::string(xr_probe::StereoName(c.stereo)) + " " + xr_probe::SqueezeName(c.squeeze);
    const auto layout = xr_probe::LayoutScreen(c.width, c.height, c.stereo, c.squeeze);
    Check(layout.quads == c.quads, name + ": quads");
    Near(c.eye_aspect, layout.eye_aspect, 1e-9, name + ": the eye's shape");
    Check(layout.rects[1].offset.x == c.second_x && layout.rects[1].offset.y == c.second_y &&
              layout.rects[0].extent.width == c.eye_width && layout.rects[0].extent.height == c.eye_height,
          name + ": the eyes' parts");
    // Through the controller: the quads, their eyes and their size
    FakeRuntime runtime;
    FakeGpu gpu;
    gpu.content = {true, static_cast<int32_t>(c.width), static_cast<int32_t>(c.height), 1};
    Script(&runtime, 8);
    std::vector<std::string> log;
    auto options = TestOptions(&log);
    options.content.mode = Mode::kScreen;
    options.content.stereo = c.stereo;
    options.content.squeeze = c.squeeze;
    xr_probe::Controller controller(&runtime, &gpu, options);
    controller.Run();
    const auto& quads = runtime.last_layers.quads;
    Check(static_cast<int>(quads.size()) == c.quads, name + ": the quads of a frame");
    if (static_cast<int>(quads.size()) == c.quads) {
      if (c.quads == 1) {
        Check(quads[0].eye == XR_EYE_VISIBILITY_BOTH, name + ": both eyes");
      } else {
        Check(quads[0].eye == XR_EYE_VISIBILITY_LEFT && quads[1].eye == XR_EYE_VISIBILITY_RIGHT &&
                  quads[1].rect.offset.x == c.second_x && quads[1].rect.offset.y == c.second_y,
              name + ": one quad per eye with its part");
      }
      Near(2.4, quads[0].size.width, 1e-6, name + ": 2.4 m wide");
      Near(2.4 * 9.0 / 16.0, quads[0].size.height, 1e-4, name + ": 16:9 per eye");
    }
  }
}

// --- controllers ---------------------------------------------------------------

using Pairs = std::vector<std::pair<std::string, std::string>>;

Pairs PairsOf(const std::vector<xr_probe::Binding>& bindings) {
  Pairs out;
  for (const auto& b : bindings) out.push_back({b.action, b.path});
  return out;
}

void TestBindingTable() {
  const std::string L = "/user/hand/left", R = "/user/hand/right";
  const auto haptic = Pairs{{"haptic", L + "/output/haptic"}, {"haptic", R + "/output/haptic"}};
  const auto with = [&](Pairs pairs) {
    pairs.insert(pairs.end(), haptic.begin(), haptic.end());
    return pairs;
  };
  Check(PairsOf(xr_probe::BindingsFor("/interaction_profiles/khr/simple_controller")) ==
            with({{"play_pause", L + "/input/select/click"},
                  {"play_pause", R + "/input/select/click"},
                  {"exit", L + "/input/menu/click"},
                  {"exit", R + "/input/menu/click"}}),
        "simple controller bindings");
  Check(PairsOf(xr_probe::BindingsFor("/interaction_profiles/oculus/touch_controller")) ==
            with({{"play_pause", L + "/input/trigger/value"},
                  {"play_pause", R + "/input/trigger/value"},
                  {"play_pause", R + "/input/a/click"},
                  {"play_pause", L + "/input/x/click"},
                  {"seek", L + "/input/thumbstick"},
                  {"seek", R + "/input/thumbstick"},
                  {"recenter", L + "/input/thumbstick/click"},
                  {"recenter", R + "/input/thumbstick/click"},
                  {"exit", L + "/input/menu/click"},
                  {"exit", R + "/input/b/click"}}),
        "Touch bindings");
  Check(PairsOf(xr_probe::BindingsFor("/interaction_profiles/valve/index_controller")) ==
            with({{"play_pause", L + "/input/trigger/click"},
                  {"play_pause", R + "/input/trigger/click"},
                  {"play_pause", L + "/input/a/click"},
                  {"play_pause", R + "/input/a/click"},
                  {"seek", L + "/input/thumbstick"},
                  {"seek", R + "/input/thumbstick"},
                  {"recenter", L + "/input/thumbstick/click"},
                  {"recenter", R + "/input/thumbstick/click"},
                  {"exit", L + "/input/b/click"},
                  {"exit", R + "/input/b/click"}}),
        "Index bindings");
  Check(PairsOf(xr_probe::BindingsFor("/interaction_profiles/htc/vive_controller")) ==
            with({{"play_pause", L + "/input/trigger/click"},
                  {"play_pause", R + "/input/trigger/click"},
                  {"seek", L + "/input/trackpad"},
                  {"seek", R + "/input/trackpad"},
                  {"seek_press", L + "/input/trackpad/click"},
                  {"seek_press", R + "/input/trackpad/click"},
                  {"recenter", L + "/input/squeeze/click"},
                  {"recenter", R + "/input/squeeze/click"},
                  {"exit", L + "/input/menu/click"},
                  {"exit", R + "/input/menu/click"}}),
        "Vive bindings");
  Check(PairsOf(xr_probe::BindingsFor("/interaction_profiles/microsoft/motion_controller")) ==
            with({{"play_pause", L + "/input/trigger/value"},
                  {"play_pause", R + "/input/trigger/value"},
                  {"seek", L + "/input/thumbstick"},
                  {"seek", R + "/input/thumbstick"},
                  {"recenter", L + "/input/thumbstick/click"},
                  {"recenter", R + "/input/thumbstick/click"},
                  {"exit", L + "/input/menu/click"},
                  {"exit", R + "/input/menu/click"}}),
        "Microsoft motion controller bindings");
  const auto hp = PairsOf(xr_probe::BindingsFor("/interaction_profiles/hp/mixed_reality_controller"));
  const auto pico4 = PairsOf(xr_probe::BindingsFor("/interaction_profiles/bytedance/pico4_controller"));
  const auto neo3 = PairsOf(xr_probe::BindingsFor("/interaction_profiles/bytedance/pico_neo3_controller"));
  const auto has = [](const Pairs& pairs, const std::string& action, const std::string& path) {
    for (const auto& p : pairs) {
      if (p.first == action && p.second == path) return true;
    }
    return false;
  };
  Check(has(hp, "play_pause", R + "/input/a/click") && has(hp, "exit", L + "/input/menu/click") &&
            has(hp, "haptic", R + "/output/haptic"),
        "HP bindings");
  Check(has(pico4, "exit", L + "/input/menu/click") && has(pico4, "exit", R + "/input/b/click") &&
            !has(pico4, "exit", R + "/input/menu/click"),
        "Pico 4: Menu on the left hand only");
  Check(has(neo3, "exit", R + "/input/menu/click") && has(neo3, "seek", L + "/input/thumbstick"), "Neo3 bindings");
  Check(xr_probe::BindingsFor("/interaction_profiles/unknown").empty(), "no bindings for an unknown profile");
  // Haptics on both hands for every profile
  bool haptics = true;
  for (const auto& entry : xr_probe::BindingTable()) {
    const auto pairs = PairsOf(entry.bindings);
    haptics = haptics && has(pairs, "haptic", L + "/output/haptic") && has(pairs, "haptic", R + "/output/haptic");
  }
  Check(haptics, "haptics on both hands for every profile");
  Check(xr_probe::BindingTable().size() == 8, "eight profiles");
  const auto extensions = xr_probe::ControllerExtensions();
  Check(extensions.size() == 2 && extensions[0] == "XR_EXT_hp_mixed_reality_controller" &&
            extensions[1] == "XR_BD_controller_interaction",
        "two extensions asked as optional");
  Check(xr_probe::StyleOf("/interaction_profiles/htc/vive_controller") == xr_probe::ProfileStyle::kTrackpad &&
            xr_probe::StyleOf("/interaction_profiles/khr/simple_controller") == xr_probe::ProfileStyle::kSimple &&
            xr_probe::StyleOf("/interaction_profiles/valve/index_controller") == xr_probe::ProfileStyle::kStick,
        "profile styles");
}

void TestInputSetup() {
  {
    FakeRuntime runtime;
    std::vector<std::string> log;
    xr_probe::Input input(&runtime, [&](const std::string& line) { log.push_back(line); });
    const bool ok = input.Setup({"XR_KHR_D3D11_enable"}, {{"player", "Lecteur vidéo"}, {"play_pause", "Lecture ou pause"}});
    Check(ok && input.ready(), "setup: controllers ready");
    Check(input.accepted().size() == 5, "setup: the five core profiles accepted");
    Check(runtime.suggested.count("/interaction_profiles/hp/mixed_reality_controller") == 0 &&
              runtime.suggested.count("/interaction_profiles/bytedance/pico4_controller") == 0,
          "setup: no extension profile without its extension");
    Check(Logged(log, "not suggested (the runtime has no XR_EXT_hp_mixed_reality_controller)"), "setup: logged");
    Check(runtime.Count("CreateActionSet player (Lecteur vidéo)") == 1, "setup: the set's localized name");
    Check(runtime.Count("CreateAction play_pause (Lecture ou pause) 2 hands") == 1, "setup: a localized action");
    Check(runtime.Count("CreateAction seek (Seek) 2 hands") == 1, "setup: English when not given");
    Check(runtime.attached, "setup: attached");
    const auto& touch = runtime.suggested["/interaction_profiles/oculus/touch_controller"];
    Check(touch.size() == 12, "setup: Touch's 12 bindings suggested");
  }
  {
    FakeRuntime runtime;
    std::vector<std::string> log;
    xr_probe::Input input(&runtime, [&](const std::string& line) { log.push_back(line); });
    input.Setup({"XR_EXT_hp_mixed_reality_controller", "XR_BD_controller_interaction"}, {});
    Check(input.accepted().size() == 8, "setup: the extension profiles with their extensions");
  }
  {
    FakeRuntime runtime;
    runtime.rejected_profiles = {"/interaction_profiles/oculus/touch_controller"};
    std::vector<std::string> log;
    xr_probe::Input input(&runtime, [&](const std::string& line) { log.push_back(line); });
    Check(input.Setup({}, {}), "rejected: the others still bind");
    Check(input.accepted().size() == 4, "rejected: four accepted");
    Check(Logged(log, "touch_controller: rejected"), "rejected: logged and skipped");
  }
  {
    FakeRuntime runtime;
    for (const auto& entry : xr_probe::BindingTable()) runtime.rejected_profiles.insert(entry.profile);
    std::vector<std::string> log;
    xr_probe::Input input(&runtime, [&](const std::string& line) { log.push_back(line); });
    Check(!input.Setup({}, {}), "none accepted: no controllers");
    Check(input.ready(), "none accepted: still attached (the view runs)");
    Check(Logged(log, "no profile accepted"), "none accepted: logged");
  }
}

void TestNoControllerView() {
  FakeRuntime runtime;
  for (const auto& entry : xr_probe::BindingTable()) runtime.rejected_profiles.insert(entry.profile);
  FakeGpu gpu;
  Script(&runtime, 10);
  std::vector<std::string> log;
  xr_probe::Controller controller(&runtime, &gpu, TestOptions(&log));
  controller.Run();
  Check(!controller.StatusNow().controllers, "no profile: the state says no controller");
  Check(runtime.layers_with_projection > 0, "no profile: the view ran");
}

// Runs a view whose hands do what |hands| says at each frame; the events
std::vector<xr_probe::HeadsetEvent> RunWithHands(FakeRuntime* runtime, const std::function<void(int, FakeRuntime*)>& hands,
                                                 int frames, xr_probe::Controller** out_controller = nullptr,
                                                 std::vector<std::string>* out_log = nullptr) {
  FakeGpu gpu;
  Script(runtime, frames);
  auto clock = std::make_shared<double>(0.0);
  runtime->clock = clock;
  runtime->on_frame = [&](int frame) { hands(frame, runtime); };
  std::vector<std::string> log;
  xr_probe::Controller controller(runtime, &gpu, TestOptions(&log, clock));
  controller.Run();
  uint64_t last = 0, dropped = 0;
  if (out_log) *out_log = log;
  (void)out_controller;
  return controller.board()->Since(0, &last, &dropped);
}

void TestControllerEvents() {
  FakeRuntime runtime;
  runtime.current_profile[0] = runtime.current_profile[1] = "/interaction_profiles/oculus/touch_controller";
  runtime.script.clear();
  std::vector<std::string> log;
  // Frames run at 1/90 s: the trigger pressed at frame 10, released at 20; the
  // stick held right from 30 to 60; the stick pressed 40 to 45 (recenter)
  const auto events = RunWithHands(
      &runtime,
      [](int frame, FakeRuntime* r) {
        r->hands[1].play = frame >= 10 && frame < 20 ? 1.0f : 0.0f;
        r->hands[0].seek_x = frame >= 30 && frame < 60 ? 0.9f : 0.0f;
        r->hands[0].recenter = frame >= 40 && frame < 45;
      },
      80, nullptr, &log);
  int play = 0, seek = 0, recenter = 0;
  for (const auto& event : events) {
    play += event.kind == xr_probe::EventKind::kPlayPause && event.hand == 1 ? 1 : 0;
    seek += event.kind == xr_probe::EventKind::kSeek && event.seconds == 10.0 ? 1 : 0;
    recenter += event.kind == xr_probe::EventKind::kRecenter ? 1 : 0;
  }
  Check(play == 1, "events: one play or pause from the right trigger");
  Check(seek == 1, "events: one seek of 10 s (0.33 s held)");
  Check(recenter == 1, "events: one recenter");
  Check(Logged(log, "action: playPause (right hand)") && Logged(log, "action: seek (left hand, 10 s)"),
        "events: the actions logged");
  Check(!runtime.haptics.empty(), "events: haptic pulses");
  int recentered = 0;
  for (const auto& line : log) recentered += line.rfind("recentered", 0) == 0 ? 1 : 0;
  Check(recentered == 2, "events: the recenter acted natively");
}

void TestNoInputWhileUnfocused() {
  FakeRuntime runtime;
  runtime.script = {{State(XR_SESSION_STATE_READY)}, {State(XR_SESSION_STATE_VISIBLE)}};
  runtime.exit_after_empty = 40;
  FakeGpu gpu;
  runtime.on_frame = [&](int) { runtime.hands[0].play = 1.0f; };
  std::vector<std::string> log;
  xr_probe::Controller controller(&runtime, &gpu, TestOptions(&log));
  controller.Run();
  uint64_t last = 0, dropped = 0;
  Check(runtime.syncs == 0, "unfocused: no xrSyncActions");
  Check(controller.board()->Since(0, &last, &dropped).empty(), "unfocused: no event");
  Check(runtime.layers_with_projection > 0, "unfocused: frames still shown");
}

void TestFocusLossResets() {
  FakeRuntime runtime;
  // Focused, the exit held, focus lost at round 30 for 20 rounds, focused
  // again with exit still held: no exit fired from a hold across the loss
  runtime.script = {{State(XR_SESSION_STATE_READY)}, {State(XR_SESSION_STATE_FOCUSED)}};
  for (int i = 0; i < 27; i++) runtime.script.push_back({});
  runtime.script.push_back({State(XR_SESSION_STATE_VISIBLE)});
  for (int i = 0; i < 20; i++) runtime.script.push_back({});
  runtime.script.push_back({State(XR_SESSION_STATE_FOCUSED)});
  runtime.exit_after_empty = 60;
  FakeGpu gpu;
  auto clock = std::make_shared<double>(0.0);
  runtime.clock = clock;
  runtime.on_frame = [&](int frame) { runtime.hands[0].exit = frame >= 5; };
  std::vector<std::string> log;
  xr_probe::Controller controller(&runtime, &gpu, TestOptions(&log, clock));
  controller.Run();
  uint64_t last = 0, dropped = 0;
  int exits = 0;
  for (const auto& event : controller.board()->Since(0, &last, &dropped)) {
    exits += event.kind == xr_probe::EventKind::kExit ? 1 : 0;
  }
  Check(exits == 0, "focus loss: a hold across it fires nothing");
  Check(Logged(log, "focus lost, the gestures reset"), "focus loss: logged");
}

void TestProfileChange() {
  FakeRuntime runtime;
  FakeGpu gpu;
  Script(&runtime, 30);
  runtime.script.push_back({});
  runtime.script.push_back({fakes::Simple(xr_probe::Event::Type::kInteractionProfileChanged)});
  runtime.current_profile[0] = "/interaction_profiles/valve/index_controller";
  std::vector<std::string> log;
  xr_probe::Controller controller(&runtime, &gpu, TestOptions(&log));
  controller.Run();
  Check(controller.StatusNow().profiles[0] == "/interaction_profiles/valve/index_controller" &&
            controller.StatusNow().profiles[1].empty(),
        "profile change: in the state per hand");
  Check(Logged(log, "controller of the left hand: /interaction_profiles/valve/index_controller"),
        "profile change: logged");
  uint64_t last = 0, dropped = 0;
  const auto events = controller.board()->Since(0, &last, &dropped);
  Check(events.size() == 1 && events[0].kind == xr_probe::EventKind::kProfile, "profile change: one event");
}

// --- gestures ------------------------------------------------------------------

struct Run {
  xr_probe::Gestures gestures;
  std::vector<xr_probe::Fired> fired;
  std::vector<xr_probe::Pulse> pulses;
  explicit Run(xr_probe::ProfileStyle style = xr_probe::ProfileStyle::kStick) : gestures(style) {}
  void At(double t, xr_probe::HandInput input) { gestures.Update(t, input, 0, &fired, &pulses); }
  int Count(xr_probe::Gesture gesture) const {
    int n = 0;
    for (const auto& f : fired) n += f.gesture == gesture ? 1 : 0;
    return n;
  }
};

xr_probe::HandInput Play(float value) {
  xr_probe::HandInput input;
  input.play = value;
  return input;
}

xr_probe::HandInput Stick(float x, bool press = false) {
  xr_probe::HandInput input;
  input.seek_x = x;
  input.seek_press = press;
  return input;
}

void TestGestures() {
  using G = xr_probe::Gesture;
  {
    Run run;
    run.At(0.0, Play(0.7f));
    Check(run.Count(G::kPlayPause) == 0, "trigger: 0.7 is not a press");
    run.At(0.1, Play(0.76f));
    Check(run.Count(G::kPlayPause) == 1, "trigger: above 0.75 presses");
    run.At(0.2, Play(0.4f));
    run.At(0.3, Play(0.8f));
    Check(run.Count(G::kPlayPause) == 1, "trigger: 0.4 is still held");
    run.At(0.4, Play(0.34f));
    run.At(0.5, Play(0.8f));
    Check(run.Count(G::kPlayPause) == 2, "trigger: released under 0.35, pressed again");
    run.At(2.0, Play(0.8f));
    Check(run.Count(G::kRecenter) == 0, "trigger: a long press is no recenter on a stick controller");
    Check(!run.pulses.empty(), "trigger: a pulse");
  }
  {
    Run run;
    std::vector<double> seeks;
    for (int i = 0; i <= 30; i++) {
      const double t = i * 0.1;
      run.At(t, Stick(i < 30 ? 0.71f : 0.0f));
    }
    for (const auto& f : run.fired) seeks.push_back(f.seconds);
    // At 0: 10; 0.5, 1.0, 1.5: 10; 2.0, 2.5: 30; released at 3.0
    const std::vector<double> expected = {10, 10, 10, 10, 30, 30};
    Check(seeks == expected, "seek: once, every 0.5 s, 30 s steps after 2 s (" + std::to_string(seeks.size()) + ")");
  }
  {
    Run run;
    run.At(0.0, Stick(0.69f));
    Check(run.fired.empty(), "seek: 0.69 does nothing");
    run.At(0.1, Stick(-0.75f));
    Check(run.fired.size() == 1 && run.fired[0].seconds == -10.0, "seek: left is backwards");
    run.At(0.2, Stick(-0.35f));
    run.At(0.7, Stick(-0.35f));
    Check(run.fired.size() == 2, "seek: still held above 0.3 repeats");
    run.At(0.8, Stick(-0.29f));
    run.At(0.9, Stick(-0.6f));
    run.At(1.5, Stick(-0.6f));
    Check(run.fired.size() == 2, "seek: under 0.3 ends it, 0.6 does not start it again");
    run.At(1.6, Stick(0.8f));
    Check(run.fired.size() == 3 && run.fired[2].seconds == 10.0, "seek: the other way at once");
  }
  {
    Run run(xr_probe::ProfileStyle::kTrackpad);
    run.At(0.0, Stick(0.9f, false));
    Check(run.fired.empty(), "trackpad: touched, not clicked: nothing");
    run.At(0.1, Stick(0.9f, true));
    Check(run.fired.size() == 1, "trackpad: clicked right: seek");
    run.At(0.2, Stick(0.9f, false));
    run.At(0.3, Stick(0.9f, true));
    Check(run.fired.size() == 2, "trackpad: clicked again: seek again");
  }
  {
    Run run;
    xr_probe::HandInput down;
    down.recenter = true;
    run.At(0.0, down);
    run.At(0.5, down);
    Check(run.fired.empty(), "recenter: nothing while pressed");
    run.At(0.9, xr_probe::HandInput{});
    Check(run.Count(G::kRecenter) == 1 && run.Count(G::kLyingDown) == 0, "recenter: on release before 1 s");
    run.At(2.0, down);
    run.At(2.99, down);
    Check(run.Count(G::kLyingDown) == 0, "lying down: not before 1 s");
    const auto pulses = run.pulses.size();
    run.At(3.0, down);
    Check(run.Count(G::kLyingDown) == 1 && run.pulses.size() == pulses + 1, "lying down: at 1 s held, a pulse");
    run.At(3.5, xr_probe::HandInput{});
    Check(run.Count(G::kRecenter) == 1, "lying down: no recenter on its release");
  }
  {
    Run run;
    xr_probe::HandInput down;
    down.exit = true;
    run.At(0.0, down);
    Check(run.fired.empty() && run.pulses.size() == 1, "exit: a short pulse at the press");
    run.At(0.5, xr_probe::HandInput{});
    Check(run.fired.empty(), "exit: a short press does nothing");
    run.At(1.0, down);
    run.At(2.0, down);
    Check(run.Count(G::kExit) == 1 && run.pulses.size() == 3 && run.pulses.back().seconds > 0.05,
          "exit: at 1 s held, a longer pulse");
    run.At(2.5, down);
    Check(run.Count(G::kExit) == 1, "exit: once per hold");
  }
  {
    Run run(xr_probe::ProfileStyle::kSimple);
    run.At(0.0, Play(1.0f));
    Check(run.fired.empty(), "simple: nothing at the press");
    run.At(0.4, Play(0.0f));
    Check(run.Count(G::kPlayPause) == 1, "simple: select short = play or pause");
    run.At(1.0, Play(1.0f));
    run.At(2.0, Play(1.0f));
    Check(run.Count(G::kRecenter) == 1, "simple: select held 1 s = recenter");
    run.At(2.2, Play(0.0f));
    Check(run.Count(G::kPlayPause) == 1, "simple: no play or pause after the hold");
  }
  {
    Run run;
    xr_probe::HandInput held;
    held.exit = true;
    held.play = 1.0f;
    held.seek_x = 0.9f;
    run.At(0.0, held);
    run.fired.clear();
    run.gestures.Reset();
    run.At(0.1, held);
    run.At(1.5, held);
    Check(run.fired.empty(), "reset: inputs held across it fire nothing");
    run.At(1.6, xr_probe::HandInput{});
    run.At(1.7, held);
    Check(run.Count(G::kPlayPause) == 1 && run.Count(G::kSeek) == 1, "reset: released, then they count again");
  }
}

// --- recentering ---------------------------------------------------------------

void Ray(const float rows[9], double x, double y, double out[3]) {
  const double v[3] = {x, y, 1.0};
  for (int i = 0; i < 3; i++) out[i] = rows[i * 3] * v[0] + rows[i * 3 + 1] * v[1] + rows[i * 3 + 2] * v[2];
  const double n = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
  for (int i = 0; i < 3; i++) out[i] /= n;
}

double Longitude(const double d[3]) { return std::atan2(d[0], d[2]) / kDegree; }
double Latitude(const double d[3]) { return std::asin(d[1]) / kDegree; }

projection_view::Quaternion Head(double yaw, double pitch) {
  using projection_view::AxisAngle;
  return projection_view::Multiply(AxisAngle(0, 1, 0, -yaw * kDegree), AxisAngle(1, 0, 0, pitch * kDegree));
}

void TestRecentering() {
  const double a = std::atan(1.0);
  // Upright: the probe's results exactly, for heads all round
  bool same = true;
  for (double yaw : {-170.0, -40.0, 0.0, 70.0, 135.0}) {
    for (double pitch : {-60.0, 0.0, 35.0}) {
      const auto head = Head(yaw, pitch);
      const auto anchor = projection_view::AnchorOf(head, false);
      same = same && std::abs(anchor.yaw - yaw) < 1e-6 && anchor.pitch == 0.0;
      const auto eye = Head(yaw + 15, pitch - 10);
      ProjectionView anchored, probe;
      projection_view::AnchoredEyeView(eye, -a, a, a, -a, anchor, 25.0, &anchored);
      projection_view::EyeView(eye, -a, a, a, -a, 25.0 - projection_view::HeadYawDegrees(head), &probe);
      for (int i = 0; i < 9; i++) same = same && std::abs(anchored.rows[i] - probe.rows[i]) < 1e-5;
    }
  }
  Check(same, "upright: the anchor gives the probe's views");
  // Lying on the back, turned 30 degrees right, looking up: the video's
  // centre (the window's yaw 10) is where the head looks
  const auto up = Head(30, 90);
  const auto lying = projection_view::AnchorOf(up, true);
  Near(30.0, lying.yaw, 1e-4, "lying: the yaw of a head looking straight up");
  Near(90.0, lying.pitch, 1e-4, "lying: its pitch");
  ProjectionView eye;
  projection_view::AnchoredEyeView(up, -a, a, a, -a, lying, 10.0, &eye);
  double d[3];
  Ray(eye.rows, 0, 0, d);
  Near(10.0, Longitude(d), 1e-3, "lying: the centre's longitude where the head looks");
  Near(0.0, Latitude(d), 1e-3, "lying: on the horizon of the video");
  // The top of the view (the top of the head) is the video's up
  Ray(eye.rows, 0, 0.5, d);
  Check(Latitude(d) > 20.0, "lying: up in the view is up in the video");
  // Lying, the head then turned 20 degrees to its right: 20 degrees right in
  // the video
  const auto turned = projection_view::Multiply(up, projection_view::AxisAngle(0, 1, 0, -20 * kDegree));
  projection_view::AnchoredEyeView(turned, -a, a, a, -a, lying, 10.0, &eye);
  Ray(eye.rows, 0, 0, d);
  Near(30.0, Longitude(d), 1e-3, "lying: the head turns the view");
  // Upright again: the head looking up sees the video's zenith
  const auto upright = projection_view::AnchorOf(up, false);
  projection_view::AnchoredEyeView(up, -a, a, a, -a, upright, 10.0, &eye);
  Ray(eye.rows, 0, 0, d);
  Near(90.0, Latitude(d), 1e-3, "upright again: looking up is up");
  // The screen 2.5 m along the gaze, facing the eyes
  const double centre[3] = {0.0, 1.0, 0.0};
  double position[3];
  projection_view::Quaternion facing;
  projection_view::PoseAhead(lying, centre, 2.5, 0.0, position, &facing);
  double gaze[3];
  const double forward[3] = {0, 0, -1};
  projection_view::Rotate(up, forward, gaze);
  Near(centre[1] + 2.5 * gaze[1], position[1], 1e-6, "lying: the screen along the gaze (y)");
  Near(centre[0] + 2.5 * gaze[0], position[0], 1e-6, "lying: the screen along the gaze (x)");
  double normal[3];
  const double back[3] = {0, 0, 1};
  projection_view::Rotate(facing, back, normal);
  Near(-gaze[1], normal[1], 1e-6, "lying: the screen faces the eyes");
  // The strip 20 degrees under the anchor's forward, upright
  const auto ahead = projection_view::AnchorAt(0, 0);
  const double eyes[3] = {0, 1.6, 0};
  projection_view::PoseAhead(ahead, eyes, 1.4, 20.0, position, &facing);
  Near(1.6 - 1.4 * std::sin(20 * kDegree), position[1], 1e-6, "strip pose: 20 degrees under");
  Near(-1.4 * std::cos(20 * kDegree), position[2], 1e-6, "strip pose: 1.4 m away");
}

void TestRecenterInTheController() {
  FakeRuntime runtime;
  FakeGpu gpu;
  Script(&runtime, 40);
  runtime.head = {static_cast<float>(std::sin(-35 * kDegree / 2)), 0, 0,
                  static_cast<float>(std::cos(-35 * kDegree / 2))};
  // A reference space change at round 15; lying down asked at round 25
  for (int i = 0; i < 12; i++) runtime.script.push_back({});
  runtime.script.push_back({fakes::Simple(xr_probe::Event::Type::kReferenceSpaceChange)});
  std::vector<std::string> log;
  xr_probe::Controller controller(&runtime, &gpu, TestOptions(&log));
  runtime.on_frame = [&](int frame) {
    if (frame == 25) controller.inbox()->Recenter(true);
  };
  controller.Run();
  int recentered = 0;
  for (const auto& line : log) recentered += line.rfind("recentered", 0) == 0 ? 1 : 0;
  Check(recentered == 3, "recentered at the start, on the space change, on the app's call: " +
                             std::to_string(recentered));
  Check(Logged(log, "reference space change pending: recentering"), "space change logged");
  Check(Logged(log, "recentered lying down: head yaw 0.0, pitch -35.0 degrees"), "lying down from the app");
  Check(controller.StatusNow().lying_down, "lying down in the state");
  // Lying with the head 35 degrees down: the video's centre where it looks
  double d[3];
  Ray(gpu.last_view[0].rows, 0, 0, d);
  Near(0.0, Latitude(d), 1e-3, "lying down: the centre ahead of the gaze");
}

// --- the strip -----------------------------------------------------------------

std::vector<uint8_t> StripBytes(uint8_t value) { return std::vector<uint8_t>(1024 * 160 * 4, value); }

void TestStrip() {
  FakeRuntime runtime;
  FakeGpu gpu;
  Script(&runtime, 40);
  std::vector<std::string> log;
  xr_probe::Controller controller(&runtime, &gpu, TestOptions(&log));
  auto* inbox = controller.inbox();
  std::vector<size_t> quads;
  runtime.on_frame = [&](int frame) {
    quads.push_back(runtime.last_layers.quads.size());
    if (frame == 5) inbox->SetStrip(StripBytes(7), 1024, 160, true);
    if (frame == 15) inbox->SetStrip(StripBytes(9), 1024, 160, true);
    if (frame == 25) inbox->SetStrip({}, 0, 0, false);
  };
  controller.Run();
  Check(quads.size() > 30 && quads[4] == 0, "strip: no layer before the first image");
  Check(quads[10] == 1 && quads[20] == 1, "strip: one quad once given");
  Check(quads[30] == 0, "strip: hidden when not visible");
  Check(gpu.strip_uploads == 2, "strip: one upload per new image: " + std::to_string(gpu.strip_uploads));
  Check(!gpu.last_strip.empty() && gpu.last_strip[0] == 9, "strip: the last image's bytes");
  Check(controller.StatusNow().strip_uploads == 2, "strip: uploads in the state");
  const auto& strip_layers = runtime.all_layers[12];
  if (strip_layers.quads.size() == 1) {
    const auto& q = strip_layers.quads[0];
    Near(0.9, q.size.width, 1e-6, "strip: 0.9 m wide");
    Near(0.14, q.size.height, 1e-6, "strip: 0.14 m high");
    Check(q.rect.extent.width == 1024 && q.rect.extent.height == 160, "strip: the whole image");
    Near(1.6 - 1.4 * std::sin(20 * kDegree), q.pose.position.y, 1e-4, "strip: 20 degrees under the eyes");
    Near(-1.4 * std::cos(20 * kDegree), q.pose.position.z, 1e-4, "strip: 1.4 m away");
    Check(strip_layers.projection, "strip: over the projection layer");
  } else {
    Check(false, "strip: a quad at frame 12");
  }
  int strip_swapchains = 0;
  for (const auto& line : runtime.journal->lines()) strip_swapchains += line.find("1024x160") != std::string::npos;
  Check(strip_swapchains == 1, "strip: one swapchain of 1024 x 160");
}

void TestStripRoomAndScreen() {
  {
    FakeRuntime runtime;
    runtime.max_layers = 1;
    FakeGpu gpu;
    Script(&runtime, 20);
    std::vector<std::string> log;
    xr_probe::Controller controller(&runtime, &gpu, TestOptions(&log));
    controller.inbox()->SetStrip(StripBytes(1), 1024, 160, true);
    controller.Run();
    Check(runtime.last_layers.quads.empty() && runtime.layers_with_projection > 0,
          "max layers 1: the strip dropped, the projection kept");
    Check(Logged(log, "the runtime allows 1: the strip is dropped"), "max layers: logged");
    Check(gpu.strip_uploads == 0, "max layers: nothing uploaded");
  }
  {
    FakeRuntime runtime;
    FakeGpu gpu;
    gpu.content = {true, 1920, 1080, 1};
    Script(&runtime, 20);
    std::vector<std::string> log;
    auto options = TestOptions(&log);
    options.content.mode = Mode::kScreen;
    xr_probe::Controller controller(&runtime, &gpu, options);
    controller.inbox()->SetStrip(StripBytes(1), 1024, 160, true);
    controller.Run();
    const auto& quads = runtime.last_layers.quads;
    Check(quads.size() == 2, "screen and strip: two quads");
    if (quads.size() == 2) {
      const double screen_h = 2.4 * 9.0 / 16.0;
      Near(1.6 - (screen_h / 2 + 0.15 + 0.07), quads[1].pose.position.y, 1e-4, "strip: 0.15 m under the screen");
      Near(-2.5, quads[1].pose.position.z, 1e-4, "strip: in the screen's plane");
    }
  }
  {
    FakeRuntime runtime;
    FakeGpu gpu;
    Script(&runtime, 20);
    std::vector<std::string> log;
    xr_probe::Controller controller(&runtime, &gpu, TestOptions(&log));
    controller.inbox()->SetStrip(std::vector<uint8_t>(2048 * 160 * 4, 1), 2048, 160, true);
    controller.Run();
    Check(gpu.strip_uploads == 0 && Logged(log, "strip: an image of 2048 x 160 refused"), "strip: too wide refused");
  }
}

void TestIdleMode() {
  FakeRuntime runtime;
  FakeGpu gpu;
  Script(&runtime, 15);
  std::vector<std::string> log;
  auto options = TestOptions(&log);
  options.content = {};
  xr_probe::Controller controller(&runtime, &gpu, options);
  controller.inbox()->SetStrip(StripBytes(3), 1024, 160, true);
  controller.Run();
  Check(gpu.draws_by_mode[Mode::kIdle] > 0, "idle: the eyes drawn (dark grey)");
  Check(runtime.last_layers.projection && runtime.last_layers.quads.size() == 1, "idle: a projection and the strip");
}

// --- stop codes ----------------------------------------------------------------

StopReason RunWith(const std::function<void(FakeRuntime*, FakeGpu*)>& setup, std::string* detail = nullptr) {
  FakeRuntime runtime;
  FakeGpu gpu;
  Script(&runtime, 10);
  setup(&runtime, &gpu);
  std::vector<std::string> log;
  xr_probe::Controller controller(&runtime, &gpu, TestOptions(&log));
  controller.Run();
  if (detail) *detail = controller.StatusNow().detail;
  Check(controller.StatusNow().phase == "stopped", "stopped at the end");
  Check(Logged(log, std::string("stop: ") + Code(controller.StatusNow().reason)), "the stop code logged");
  return controller.StatusNow().reason;
}

void TestStopReasons() {
  std::string detail;
  Check(RunWith([](FakeRuntime*, FakeGpu*) {}) == StopReason::kEndedFromHeadset,
        "endedFromHeadset: EXITING the app did not ask");
  Check(RunWith([](FakeRuntime* r, FakeGpu*) { r->create_result = XR_ERROR_RUNTIME_UNAVAILABLE; }) ==
            StopReason::kNoRuntime,
        "noRuntime: XR_ERROR_RUNTIME_UNAVAILABLE");
  Check(RunWith([](FakeRuntime* r, FakeGpu*) { r->create_result = XR_ERROR_RUNTIME_FAILURE; }) ==
            StopReason::kNoRuntime,
        "noRuntime: XR_ERROR_RUNTIME_FAILURE");
  Check(RunWith([](FakeRuntime* r, FakeGpu*) {
          r->create_result = XR_ERROR_INITIALIZATION_FAILED;
          r->loader_missing = true;
        }) == StopReason::kLoaderMissing,
        "loaderMissing");
  Check(RunWith([](FakeRuntime* r, FakeGpu*) { r->unavailable_systems = 1000000; }, &detail) ==
            StopReason::kNoHeadset,
        "noHeadset after the wait: " + detail);
  Check(RunWith([](FakeRuntime* r, FakeGpu*) { r->adapter = {43, 0}; }) == StopReason::kOtherGpu, "otherGpu");
  Check(RunWith([](FakeRuntime* r, FakeGpu*) { r->min_feature_level = 0xc000; }) == StopReason::kGpuTooWeak,
        "gpuTooWeak: feature level");
  Check(RunWith([](FakeRuntime*, FakeGpu* g) {
          g->vendor = 0x1414;
          g->device = 0x8c;
        }, &detail) == StopReason::kGpuTooWeak,
        "gpuTooWeak: the Basic Render Driver: " + detail);
  Check(RunWith([](FakeRuntime* r, FakeGpu*) { r->wait_results = {{4, XR_ERROR_INSTANCE_LOST}}; }) ==
            StopReason::kRuntimeLost,
        "runtimeLost: instance lost");
  Check(RunWith([](FakeRuntime* r, FakeGpu*) { r->wait_results = {{4, XR_ERROR_SESSION_LOST}}; }) ==
            StopReason::kRuntimeLost,
        "runtimeLost: session lost");
  Check(RunWith([](FakeRuntime* r, FakeGpu*) {
          r->script.push_back({State(XR_SESSION_STATE_LOSS_PENDING)});
        }) == StopReason::kRuntimeLost,
        "runtimeLost: loss pending");
  Check(RunWith([](FakeRuntime* r, FakeGpu*) {
          r->script.push_back({fakes::Simple(xr_probe::Event::Type::kInstanceLossPending)});
        }) == StopReason::kRuntimeLost,
        "runtimeLost: instance loss pending");
  Check(RunWith([](FakeRuntime* r, FakeGpu*) { r->create_result = XR_ERROR_INSTANCE_LOST; }) ==
            StopReason::kRuntimeLost,
        "runtimeLost: at xrCreateInstance");
  // The runtime quits while the session is not running (no xrWaitFrame then):
  // only xrPollEvent sees XR_ERROR_INSTANCE_LOST, and the view still ends
  {
    FakeRuntime runtime;
    FakeGpu gpu;
    runtime.script = {{State(XR_SESSION_STATE_IDLE)}};
    runtime.exit_after_empty = -1;
    runtime.poll_error = XR_ERROR_INSTANCE_LOST;
    std::vector<std::string> log;
    xr_probe::Controller controller(&runtime, &gpu, TestOptions(&log));
    controller.Run();
    Check(controller.StatusNow().reason == StopReason::kRuntimeLost, "runtimeLost: xrPollEvent while IDLE before READY");
    Check(controller.StatusNow().instance_lost, "runtimeLost: the instance marked lost");
    Check(runtime.Count("WaitFrame") == 0 && runtime.Count("BeginSession") == 0, "runtimeLost: no frame was asked");
    Check(runtime.Count("DestroyInstance") == 1, "runtimeLost: the instance destroyed");
    Check(controller.StatusNow().phase == "stopped", "runtimeLost: stopped");
  }
  {
    FakeRuntime runtime;
    FakeGpu gpu;
    Script(&runtime, -1);
    runtime.script.push_back({State(XR_SESSION_STATE_STOPPING)});
    runtime.script.push_back({State(XR_SESSION_STATE_IDLE)});
    runtime.poll_error = XR_ERROR_INSTANCE_LOST;
    std::vector<std::string> log;
    xr_probe::Controller controller(&runtime, &gpu, TestOptions(&log));
    controller.Run();
    Check(controller.StatusNow().reason == StopReason::kRuntimeLost, "runtimeLost: xrPollEvent while IDLE after STOPPING");
    Check(runtime.Count("EndSession") == 1, "runtimeLost: the session ended once, at STOPPING");
    Check(Logged(log, "xrPollEvent"), "runtimeLost: the poll's error logged");
  }
  Check(RunWith([](FakeRuntime* r, FakeGpu*) { r->create_result = XR_ERROR_EXTENSION_NOT_PRESENT; }, &detail) ==
            StopReason::kFailed,
        "failed: another result, its detail: " + detail);
  Check(detail.find("xrCreateInstance") != std::string::npos, "failed: the call in the detail");
  Check(RunWith([](FakeRuntime* r, FakeGpu*) { r->session_result = XR_ERROR_GRAPHICS_DEVICE_INVALID; }) ==
            StopReason::kFailed,
        "failed: xrCreateSession");
  Check(RunWith([](FakeRuntime* r, FakeGpu*) { r->system_result = XR_ERROR_SYSTEM_INVALID; }) == StopReason::kFailed,
        "failed: xrGetSystem");
  Check(RunWith([](FakeRuntime*, FakeGpu* g) { g->open_ok = false; }) == StopReason::kFailed, "failed: the GPU side");
  Check(RunWith([](FakeRuntime* r, FakeGpu*) { r->formats = {10}; }) == StopReason::kFailed, "failed: no format");
  // Stopped by the app (the probe's test too), and the codes of the Dart side
  {
    FakeRuntime runtime;
    FakeGpu gpu;
    Script(&runtime, -1);
    std::vector<std::string> log;
    xr_probe::Controller controller(&runtime, &gpu, TestOptions(&log));
    runtime.on_frame = [&](int frame) {
      if (frame == 5) controller.RequestStop();
    };
    controller.Run();
    Check(controller.StatusNow().reason == StopReason::kStoppedByApp, "stoppedByApp");
  }
  const std::vector<std::pair<StopReason, std::string>> codes = {
      {StopReason::kNone, "none"},
      {StopReason::kStoppedByApp, "stoppedByApp"},
      {StopReason::kEndedFromHeadset, "endedFromHeadset"},
      {StopReason::kNoRuntime, "noRuntime"},
      {StopReason::kLoaderMissing, "loaderMissing"},
      {StopReason::kNoHeadset, "noHeadset"},
      {StopReason::kOtherGpu, "otherGpu"},
      {StopReason::kGpuTooWeak, "gpuTooWeak"},
      {StopReason::kRuntimeLost, "runtimeLost"},
      {StopReason::kRuntimeNotAnswering, "runtimeNotAnswering"},
      {StopReason::kAlreadyRunning, "alreadyRunning"},
      {StopReason::kNotInBuild, "notInBuild"},
      {StopReason::kNotWindows, "notWindows"},
      {StopReason::kFailed, "failed"},
  };
  bool all = true;
  for (const auto& [reason, code] : codes) all = all && Code(reason) == code;
  Check(all, "the codes are XrStopReason's names");
}

// --- visibility ----------------------------------------------------------------

void TestVisibility() {
  xr_probe::VisibilityWatch watch;
  Check(watch.Update(0.0, false) == 0 && watch.Update(5.0, false) == 0, "visibility: nothing before it was seen");
  Check(watch.Update(6.0, true) == 0, "visibility: first seen, nothing to tell");
  Check(watch.Update(7.0, false) == 0 && watch.Update(8.9, false) == 0, "visibility: under 2 s, nothing");
  Check(watch.Update(9.1, false) == -1, "visibility: off after 2 s");
  Check(watch.Update(12.0, false) == 0, "visibility: off once");
  Check(watch.Update(13.0, true) == 1, "visibility: on again");
  Check(watch.Update(14.0, false) == 0 && watch.Update(14.5, true) == 0, "visibility: a short gap, nothing");

  FakeRuntime runtime;
  FakeGpu gpu;
  auto clock = std::make_shared<double>(0.0);
  runtime.clock = clock;
  // Focused 20 frames, synchronized (not visible) 300 frames (3.3 s), focused
  runtime.script = {{State(XR_SESSION_STATE_READY)}, {State(XR_SESSION_STATE_FOCUSED)}};
  for (int i = 0; i < 20; i++) runtime.script.push_back({});
  runtime.script.push_back({State(XR_SESSION_STATE_SYNCHRONIZED)});
  for (int i = 0; i < 300; i++) runtime.script.push_back({});
  runtime.script.push_back({State(XR_SESSION_STATE_FOCUSED)});
  runtime.exit_after_empty = 20;
  std::vector<std::string> log;
  xr_probe::Controller controller(&runtime, &gpu, TestOptions(&log, clock));
  controller.Run();
  uint64_t last = 0, dropped = 0;
  std::vector<bool> seen;
  for (const auto& event : controller.board()->Since(0, &last, &dropped)) {
    if (event.kind == xr_probe::EventKind::kVisibility) seen.push_back(event.on);
  }
  Check(seen == std::vector<bool>({false, true}), "visibility: one off, one on (" + std::to_string(seen.size()) + ")");
  Check(controller.StatusNow().reason == StopReason::kEndedFromHeadset, "visibility: then ended from the headset");
}

// --- the runtime's manifest -------------------------------------------------------

void TestRuntimeGuess() {
  const std::vector<std::pair<std::string, std::string>> cases = {
      {R"(C:\Program Files (x86)\Steam\steamapps\common\SteamVR\steamxr_win64.json)", "SteamVR"},
      {R"(C:\Program Files\Oculus\Support\oculus-runtime\oculus_openxr_64.json)", "Meta Horizon Link"},
      {R"(C:\Program Files\Virtual Desktop Streamer\OpenXR\virtualdesktop-openxr.json)", "Virtual Desktop"},
      {R"(C:\Program Files\PICO Connect\openxr\pico_openxr_runtime.json)", "Pico Connect"},
      {R"(C:\WINDOWS\system32\MixedRealityRuntime.json)", "Windows Mixed Reality"},
      {R"(C:\Program Files\Varjo\varjo-openxr\VarjoOpenXR.json)", "Varjo"},
      {R"(C:\Program Files\VIVE Hub\Runtime\ViveOpenXR.json)", "VIVE"},
      {R"(D:\tools\some_runtime.json)", ""},
      {"", ""},
  };
  bool ok = true;
  for (const auto& [path, name] : cases) {
    const auto guess = xr_probe::GuessRuntime(path);
    if (guess != name) {
      ok = false;
      std::printf("  guess of %s: %s\n", path.c_str(), guess.c_str());
    }
  }
  Check(ok, "runtime guesses from the manifest path");
  Check(xr_probe::FileNameOf(R"(C:\a\b\steamxr_win64.json)") == "steamxr_win64.json" &&
            xr_probe::FileNameOf("/x/y.json") == "y.json" && xr_probe::FileNameOf("z.json") == "z.json",
        "the file name only");
}

void TestNames() {
  Check(xr_probe::ModeOf("sphere") == Mode::kSphere && xr_probe::ModeOf("screen") == Mode::kScreen &&
            xr_probe::ModeOf("pattern") == Mode::kPattern && xr_probe::ModeOf("idle") == Mode::kIdle &&
            xr_probe::ModeOf("whatever") == Mode::kIdle,
        "modes by name, unknown is idle");
  Check(xr_probe::StereoOf("leftRight") == xr_probe::Stereo::kLeftRight &&
            xr_probe::StereoOf("topBottom") == xr_probe::Stereo::kTopBottom &&
            xr_probe::StereoOf("x") == xr_probe::Stereo::kMono,
        "stereo by name");
  Check(xr_probe::SqueezeOf("half") == xr_probe::Squeeze::kHalf && xr_probe::SqueezeOf("") == xr_probe::Squeeze::kFull,
        "squeeze by name");
  Check(xr_probe::IsBasicRenderDriver(0x1414, 0x8c) && !xr_probe::IsBasicRenderDriver(0x1414, 0x8d) &&
            !xr_probe::IsBasicRenderDriver(0x10de, 0x8c),
        "the Basic Render Driver by its ids");
}

void TestMemoryFlag() {
  FakeRuntime runtime;
  FakeGpu gpu;
  gpu.memory = {true, 7600ull << 20, 8000ull << 20};
  Script(&runtime, 30);
  std::vector<std::string> log;
  auto options = TestOptions(&log);
  options.stats_every = 0.005;
  xr_probe::Controller controller(&runtime, &gpu, options);
  controller.Run();
  Check(controller.StatusNow().memory_tight, "memory: above 90 % flagged");
  Check(controller.StatusNow().memory_budget_mb == 8000, "memory: the budget in the state");
  Check(Logged(log, "memory: GPU memory above 90 % of its budget"), "memory: logged");
  Check(Logged(log, "snapshot ") && Logged(log, "strip uploads"), "stats: snapshot time and strip uploads");
  Check(controller.StatusNow().stats_at > 0.0, "stats: their time in the state");
}

}  // namespace

int main() {
  TestEventQueue();
  TestNames();
  TestRuntimeGuess();
  TestScreenLayouts();
  TestBindingTable();
  TestGestures();
  TestRecentering();
  TestVisibility();
  TestInputSetup();
  TestNoControllerView();
  TestControllerEvents();
  TestNoInputWhileUnfocused();
  TestFocusLossResets();
  TestProfileChange();
  TestContentChanges();
  TestRefreshMatchingOff();
  TestRecenterInTheController();
  TestStrip();
  TestStripRoomAndScreen();
  TestIdleMode();
  TestStopReasons();
  TestMemoryFlag();
  TestStartStopCycles();
  TestStopWhileWaitingForTheHeadset();
  TestStopDuringPrepare();
  TestStopWhileTheRuntimeHangs();
  TestDetachedTeardown();
  TestAlreadyRunningAndSequences();
  std::printf("%d checks, %d failed\n", fakes::checks, fakes::failures);
  return fakes::failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
