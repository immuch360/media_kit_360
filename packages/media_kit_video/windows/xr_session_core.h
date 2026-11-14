// Immuch360: the headset view (IMMUCH360-NOTE.md, patch 15), what holds a
// view between the plugin's platform thread and the view's own thread, with
// std::thread only (no Windows header), so the tests run all of it with a
// fake runtime and a fake GPU (test/xr_v1_test.cc):
// - one view at a time, |Start| returning at once, the view on a thread of
//   its own;
// - |Stop| bounded: it cuts the content source first (so the player may go),
//   asks the controller to end, waits up to its bound; a view that did not end
//   is "not answering" (a runtime hanging inside a call) and the next |Start|
//   says so until its thread ends at last;
// - the commands of the app (content, rate, strip, recenter, stop) through an
//   |Inbox| the controller reads at each loop, never a call into it;
// - what the app polls (|Board|): the state and a queue of at most 64 events,
//   the oldest dropped; nothing calls into Dart;
// - |Teardown| (the plugin going away, the process ending): a view that does
//   not end in time has its thread detached, never left joinable in a static
//   (std::terminate at exit).

#ifndef XR_SESSION_CORE_H_
#define XR_SESSION_CORE_H_

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "xr_probe_core.h"
#include "xr_runtime.h"

namespace xr_probe {

// What the app polls: the view's state
struct Status {
  // "never started", "starting", "waiting for the headset", "running",
  // "not answering", "stopped"
  std::string phase = "starting";
  std::string session_state = "unknown";
  bool visible = false;
  bool focused = false;
  StopReason reason = StopReason::kNone;
  // English, for the log and the app's log (never shown as it is)
  std::string detail;
  std::string runtime;
  std::string system;
  std::string swapchain_path;
  std::string mode = "idle";
  // The interaction profile of each hand, "" while none
  std::string profiles[2];
  // Whether a controller profile was accepted (else the window drives alone)
  bool controllers = false;
  bool lying_down = false;
  float refresh_rate = 0.0f;
  float refresh_asked = 0.0f;
  int64_t frames = 0;
  int64_t frames_shown = 0;
  int64_t frames_late = 0;
  // GPU memory used above 90 % of the budget at the last statistics
  bool memory_tight = false;
  uint64_t memory_used_mb = 0;
  uint64_t memory_budget_mb = 0;
  // Seconds from the view's start to its last statistics; the snapshot of
  // the player's frame then (average and longest, ms)
  double stats_at = 0.0;
  double snapshot_ms = 0.0;
  double snapshot_max_ms = 0.0;
  int64_t strip_uploads = 0;
  bool strip_shown = false;
  bool instance_lost = false;
};

enum class EventKind {
  kPlayPause,
  kSeek,
  kRecenter,
  kLyingDown,
  kExit,
  kVisibility,
  kProfile,
};

const char* EventKindName(EventKind kind);

struct HeadsetEvent {
  // From 1, rising
  uint64_t sequence = 0;
  EventKind kind = EventKind::kPlayPause;
  // kSeek: seconds, signed
  double seconds = 0.0;
  // kLyingDown, kVisibility
  bool on = false;
  // kPlayPause to kExit: the hand (0 left, 1 right); kProfile too
  int hand = 0;
  // kProfile: the hand's new interaction profile ("" none)
  std::string profile;
  // Seconds from the view's start
  double time = 0.0;
};

// At most |capacity| events, the oldest dropped, numbered from 1
class EventQueue {
 public:
  // |first| the first sequence: a view's numbers go on from the view before
  // it, so the app's last sequence stays meaningful
  explicit EventQueue(size_t capacity = 64, uint64_t first = 1)
      : capacity_(capacity), next_(first) {}
  // The sequence given; true when an older event had to go
  bool Push(HeadsetEvent event, uint64_t* sequence = nullptr);
  std::vector<HeadsetEvent> Since(uint64_t after) const;
  uint64_t last() const { return next_ - 1; }
  uint64_t dropped() const { return dropped_; }

 private:
  size_t capacity_;
  std::deque<HeadsetEvent> events_;
  uint64_t next_;
  uint64_t dropped_ = 0;
};

// The state and the events, written by the view's thread, read by the poll
class Board {
 public:
  explicit Board(uint64_t first_sequence = 1) : events_(64, first_sequence) {}
  Status status() const;
  void Update(const std::function<void(Status*)>& change);
  // The first reason only: what ended the view first is what it says; true
  // when this one was taken
  // |force|: this one even over an earlier one (a stop that timed out)
  bool SetReason(StopReason reason, const std::string& detail,
                 bool force = false);
  bool Push(HeadsetEvent event);
  std::vector<HeadsetEvent> Since(uint64_t after, uint64_t* last,
                                  uint64_t* dropped) const;

 private:
  mutable std::mutex mutex_;
  Status status_;
  EventQueue events_;
};

// The strip's image, as the app last gave it
struct StripImage {
  std::vector<uint8_t> rgba;
  uint32_t width = 0;
  uint32_t height = 0;
  bool visible = false;
  // Rises with each new image (not with a visibility change alone)
  uint64_t generation = 0;
};

// The app's commands, taken by the controller at its next loop
class Inbox {
 public:
  void SetContent(const ContentRequest& content);
  // The new content once, false when none came since the last take
  bool TakeContent(ContentRequest* content);
  void SetRate(double fps);
  bool TakeRate(double* fps);
  void SetStrip(std::vector<uint8_t> rgba, uint32_t width, uint32_t height,
                bool visible);
  // The image when its generation is not |known| (copied), and the
  // visibility always
  bool TakeStrip(uint64_t known, StripImage* image, bool* visible);
  void Recenter(bool lying_down);
  bool TakeRecenter(bool* lying_down);
  void Stop();
  bool stop() const;

 private:
  mutable std::mutex mutex_;
  ContentRequest content_;
  bool content_new_ = false;
  double rate_ = 0.0;
  bool rate_new_ = false;
  StripImage strip_;
  bool recenter_ = false;
  bool lying_down_ = false;
  bool stop_ = false;
};

// The content source's gate: closed by |Stop| before it waits, so that no
// frame of a player is read once the app may dispose it. The GPU side's
// source holds the lock while it reads.
struct SourceGate {
  std::mutex mutex;
  bool open = true;
  void Close() {
    std::lock_guard<std::mutex> lock(mutex);
    open = false;
  }
};

// What the app gives to start a view (XrStartRequest in Dart)
struct StartRequest {
  ContentRequest content;
  // UTF-8; empty: no log file
  std::string log_path;
  // Whether to ask the runtime for a refresh rate that is a multiple of the
  // content's
  bool match_refresh_rate = true;
  // Localized names of the action set ("player") and of the actions, by
  // their OpenXR names
  std::map<std::string, std::string> action_names;
  // For the log's first lines
  std::string app_version;
};

struct StartAnswer {
  bool ok = false;
  StopReason reason = StopReason::kNone;
  std::string detail;
};

struct PollAnswer {
  bool running = false;
  Status status;
  std::vector<HeadsetEvent> events;
  uint64_t last_sequence = 0;
  uint64_t dropped = 0;
  std::string log_path;
};

// The runtime and the GPU side of one view, made on its thread (the loader
// is opened there, never on the platform thread), and its log
struct ViewParts {
  std::unique_ptr<Runtime> runtime;
  std::unique_ptr<Gpu> gpu;
  std::function<void(const std::string&)> log;
};

using PartsFactory = std::function<ViewParts(
    const StartRequest& request, std::shared_ptr<SourceGate> gate)>;

// Settings of the controller the core does not take from the request (the
// tests' clock, shorter waits)
struct CoreOptions {
  int system_wait_seconds = 60;
  double stats_every = 5.0;
  double stop_bound_seconds = 3.0;
  std::function<double()> now;
  std::function<void(int)> sleep_ms;
  // The plugin's build, for the log (the commit when CMake knew it)
  std::string build;
};

class SessionCore {
 public:
  SessionCore(PartsFactory factory, CoreOptions options);
  // |Teardown| with no wait
  ~SessionCore();
  SessionCore(const SessionCore&) = delete;
  SessionCore& operator=(const SessionCore&) = delete;

  StartAnswer Start(const StartRequest& request);
  // True when the view ended within |wait_ms| (or none ran)
  bool Stop(int wait_ms);
  void SetContent(const ContentRequest& content);
  void SetContentRate(double fps);
  void SetStrip(std::vector<uint8_t> rgba, uint32_t width, uint32_t height,
                bool visible);
  void Recenter(bool lying_down);
  PollAnswer Poll(uint64_t after);
  // Stops with |wait_ms|, then detaches a thread that did not end. The core
  // starts nothing afterwards.
  void Teardown(int wait_ms);

  // Whether a thread was left to end on its own (the tests)
  int detached() const { return detached_; }

 private:
  struct View;
  static void Body(std::shared_ptr<View> view, StartRequest request,
                   PartsFactory factory, CoreOptions options);
  std::shared_ptr<View> Current();

  PartsFactory factory_;
  CoreOptions options_;
  std::mutex mutex_;
  std::shared_ptr<View> view_;
  uint64_t next_sequence_ = 1;
  bool torn_down_ = false;
  int detached_ = 0;
};

}  // namespace xr_probe

#endif  // XR_SESSION_CORE_H_
