// Immuch360: what holds a headset view (IMMUCH360-NOTE.md, patch 15); see the
// header.

#include "xr_session_core.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <system_error>
#include <utility>

#include "xr_controller.h"

namespace xr_probe {

const char* EventKindName(EventKind kind) {
  switch (kind) {
    case EventKind::kPlayPause:
      return "playPause";
    case EventKind::kSeek:
      return "seek";
    case EventKind::kRecenter:
      return "recenter";
    case EventKind::kLyingDown:
      return "lyingDown";
    case EventKind::kExit:
      return "exit";
    case EventKind::kVisibility:
      return "visibility";
    case EventKind::kProfile:
      return "profile";
  }
  return "?";
}

bool EventQueue::Push(HeadsetEvent event, uint64_t* sequence) {
  event.sequence = next_++;
  if (sequence != nullptr) *sequence = event.sequence;
  events_.push_back(std::move(event));
  bool dropped = false;
  while (events_.size() > capacity_) {
    events_.pop_front();
    dropped_++;
    dropped = true;
  }
  return dropped;
}

std::vector<HeadsetEvent> EventQueue::Since(uint64_t after) const {
  std::vector<HeadsetEvent> out;
  for (const auto& event : events_) {
    if (event.sequence > after) out.push_back(event);
  }
  return out;
}

Status Board::status() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return status_;
}

void Board::Update(const std::function<void(Status*)>& change) {
  std::lock_guard<std::mutex> lock(mutex_);
  change(&status_);
}

bool Board::SetReason(StopReason reason, const std::string& detail,
                      bool force) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (status_.reason != StopReason::kNone && !force) {
    return false;
  }
  status_.reason = reason;
  status_.detail = detail;
  return true;
}

bool Board::Push(HeadsetEvent event) {
  std::lock_guard<std::mutex> lock(mutex_);
  return events_.Push(std::move(event));
}

std::vector<HeadsetEvent> Board::Since(uint64_t after, uint64_t* last,
                                       uint64_t* dropped) const {
  std::lock_guard<std::mutex> lock(mutex_);
  *last = events_.last();
  *dropped = events_.dropped();
  return events_.Since(after);
}

void Inbox::SetContent(const ContentRequest& content) {
  std::lock_guard<std::mutex> lock(mutex_);
  content_ = content;
  content_new_ = true;
  // A rate given before this content is the old content's
  rate_new_ = false;
}

bool Inbox::TakeContent(ContentRequest* content) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!content_new_) return false;
  *content = content_;
  content_new_ = false;
  return true;
}

void Inbox::SetRate(double fps) {
  std::lock_guard<std::mutex> lock(mutex_);
  rate_ = fps;
  rate_new_ = true;
}

bool Inbox::TakeRate(double* fps) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!rate_new_) return false;
  *fps = rate_;
  rate_new_ = false;
  return true;
}

void Inbox::SetStrip(std::vector<uint8_t> rgba, uint32_t width,
                     uint32_t height, bool visible) {
  std::lock_guard<std::mutex> lock(mutex_);
  strip_.visible = visible;
  // A visibility change alone keeps the image (no upload)
  if (!rgba.empty()) {
    strip_.rgba = std::move(rgba);
    strip_.width = width;
    strip_.height = height;
    strip_.generation++;
  }
}

bool Inbox::TakeStrip(uint64_t known, StripImage* image, bool* visible) {
  std::lock_guard<std::mutex> lock(mutex_);
  *visible = strip_.visible;
  if (strip_.generation == known) return false;
  *image = strip_;
  return true;
}

void Inbox::Recenter(bool lying_down) {
  std::lock_guard<std::mutex> lock(mutex_);
  recenter_ = true;
  lying_down_ = lying_down;
}

bool Inbox::TakeRecenter(bool* lying_down) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!recenter_) return false;
  *lying_down = lying_down_;
  recenter_ = false;
  return true;
}

void Inbox::Stop() {
  std::lock_guard<std::mutex> lock(mutex_);
  stop_ = true;
}

bool Inbox::stop() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return stop_;
}

struct SessionCore::View {
  explicit View(uint64_t first_sequence) : board(first_sequence) {}
  std::thread thread;
  std::mutex mutex;
  std::condition_variable ended;
  bool done = false;
  bool stop_timed_out = false;
  Inbox inbox;
  Board board;
  std::shared_ptr<SourceGate> gate = std::make_shared<SourceGate>();
  std::string log_path;
};

SessionCore::SessionCore(PartsFactory factory, CoreOptions options)
    : factory_(std::move(factory)), options_(std::move(options)) {}

SessionCore::~SessionCore() { Teardown(0); }

std::shared_ptr<SessionCore::View> SessionCore::Current() {
  std::lock_guard<std::mutex> lock(mutex_);
  return view_;
}

void SessionCore::Body(std::shared_ptr<View> view, StartRequest request,
                       PartsFactory factory, CoreOptions core) {
  {
    ViewParts parts;
    try {
      parts = factory(request, view->gate);
    } catch (...) {
      parts = ViewParts{};
    }
    if (!parts.runtime || !parts.gpu) {
      view->board.SetReason(StopReason::kFailed,
                            "the view's runtime or graphics could not be made");
      view->board.Update([](Status* status) { status->phase = "stopped"; });
    } else {
      auto log = parts.log ? parts.log : [](const std::string&) {};
      log("Immuch360 headset view, media_kit_360 " +
          (core.build.empty() ? std::string("(build unknown)") : core.build) +
          ", app " +
          (request.app_version.empty() ? std::string("(version unknown)")
                                       : request.app_version));
      log(std::string("refresh rate matching: ") +
          (request.match_refresh_rate ? "on" : "off"));
      Options options;
      options.content = request.content;
      options.match_refresh_rate = request.match_refresh_rate;
      options.action_names = request.action_names;
      options.system_wait_seconds = core.system_wait_seconds;
      options.stats_every = core.stats_every;
      options.stop_bound_seconds = core.stop_bound_seconds;
      options.now = core.now;
      options.sleep_ms = core.sleep_ms;
      options.log = log;
      try {
        Controller controller(parts.runtime.get(), parts.gpu.get(), options,
                              &view->inbox, &view->board);
        controller.Run();
      } catch (const std::exception& error) {
        view->board.SetReason(StopReason::kFailed,
                              std::string("an error in the view: ") +
                                  error.what());
      } catch (...) {
        view->board.SetReason(StopReason::kFailed, "an error in the view");
      }
      const auto status = view->board.status();
      log(std::string("view ended: ") + StopReasonCode(status.reason) + ", " +
          status.detail);
    }
    // The GPU side, then the runtime, gone on this thread
  }
  {
    std::lock_guard<std::mutex> lock(view->mutex);
    view->done = true;
  }
  view->ended.notify_all();
}

StartAnswer SessionCore::Start(const StartRequest& request) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (torn_down_) {
    return {false, StopReason::kFailed, "the plugin is going away"};
  }
  if (view_ != nullptr) {
    std::unique_lock<std::mutex> view_lock(view_->mutex);
    if (!view_->done) {
      if (view_->stop_timed_out) {
        return {false, StopReason::kRuntimeNotAnswering,
                "the last headset view has not ended yet: the runtime does "
                "not answer"};
      }
      return {false, StopReason::kAlreadyRunning,
              "a headset view already runs"};
    }
    view_lock.unlock();
    if (view_->thread.joinable()) view_->thread.join();
    // The next view's events numbered on from this one's
    uint64_t last = 0, dropped = 0;
    view_->board.Since(~0ull, &last, &dropped);
    next_sequence_ = last + 1;
  }
  auto view = std::make_shared<View>(next_sequence_);
  view->log_path = request.log_path;
  view->inbox.SetContent(request.content);
  try {
    view->thread = std::thread(Body, view, request, factory_, options_);
  } catch (const std::system_error&) {
    return {false, StopReason::kFailed, "no thread for the headset view"};
  }
  view_ = view;
  return {true, StopReason::kNone, ""};
}

bool SessionCore::Stop(int wait_ms) {
  const auto view = Current();
  if (view == nullptr) {
    return true;
  }
  // No frame of a player read from now on: the app may dispose it
  view->gate->Close();
  view->inbox.Stop();
  std::unique_lock<std::mutex> lock(view->mutex);
  const bool ended = view->ended.wait_for(
      lock, std::chrono::milliseconds((std::max)(0, wait_ms)),
      [&] { return view->done; });
  if (!ended) {
    view->stop_timed_out = true;
  }
  lock.unlock();
  if (ended) {
    std::lock_guard<std::mutex> core_lock(mutex_);
    if (view_ == view && view->thread.joinable()) view->thread.join();
    return true;
  }
  view->board.SetReason(StopReason::kRuntimeNotAnswering,
                        "the view did not end within " +
                            std::to_string(wait_ms) +
                            " ms of the stop: the runtime does not answer",
                        true);
  view->board.Update([](Status* status) { status->phase = "not answering"; });
  return false;
}

void SessionCore::SetContent(const ContentRequest& content) {
  if (const auto view = Current()) view->inbox.SetContent(content);
}

void SessionCore::SetContentRate(double fps) {
  if (const auto view = Current()) view->inbox.SetRate(fps);
}

void SessionCore::SetStrip(std::vector<uint8_t> rgba, uint32_t width,
                           uint32_t height, bool visible) {
  if (const auto view = Current()) {
    view->inbox.SetStrip(std::move(rgba), width, height, visible);
  }
}

void SessionCore::Recenter(bool lying_down) {
  if (const auto view = Current()) view->inbox.Recenter(lying_down);
}

PollAnswer SessionCore::Poll(uint64_t after) {
  PollAnswer answer;
  const auto view = Current();
  if (view == nullptr) {
    answer.status.phase = "never started";
    answer.last_sequence = 0;
    return answer;
  }
  {
    std::lock_guard<std::mutex> lock(view->mutex);
    answer.running = !view->done;
  }
  answer.status = view->board.status();
  answer.events =
      view->board.Since(after, &answer.last_sequence, &answer.dropped);
  answer.log_path = view->log_path;
  return answer;
}

void SessionCore::Teardown(int wait_ms) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    torn_down_ = true;
  }
  if (Stop(wait_ms)) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (view_ != nullptr && view_->thread.joinable()) {
    // It ends on its own, holding what it uses; never a joinable thread left
    // in a static at exit
    view_->thread.detach();
    detached_++;
  }
}

}  // namespace xr_probe
