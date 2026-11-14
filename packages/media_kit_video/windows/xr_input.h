// Immuch360: the headset view's controllers (IMMUCH360-NOTE.md, patch 15),
// through OpenXR's action system: one action set "player" with six actions,
// bindings suggested for every controller profile a PC runtime may bind (the
// table of |BindingsFor|), and the gestures read from them (|Gestures|).
// Portable: the runtime is xr_runtime.h's interface, the time a number of
// seconds, so the tests drive all of it (test/xr_v1_test.cc).
//
// What the controllers do: play or pause, seek, recenter, lying down and
// leave. Recenter and lying down act in the headset at once (the controller
// applies them) and are reported; play or pause, seek and leave are only
// reported, for the app to apply to the window's player, which stays the one
// place the video is driven from.

#ifndef XR_INPUT_H_
#define XR_INPUT_H_

#include <openxr/openxr.h>

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "xr_runtime.h"

namespace xr_probe {

// The actions, by their OpenXR names
constexpr const char* kActionPlayPause = "play_pause";
constexpr const char* kActionSeek = "seek";
constexpr const char* kActionSeekPress = "seek_press";
constexpr const char* kActionRecenter = "recenter";
constexpr const char* kActionExit = "exit";
constexpr const char* kActionHaptic = "haptic";
constexpr const char* kActionSet = "player";

struct Binding {
  std::string action;
  std::string path;
};

struct ProfileBindings {
  std::string profile;
  // The instance extension the profile needs, "" for a core one
  std::string extension;
  std::vector<Binding> bindings;
};

// How a profile's gestures go: a stick (or a pad) to seek and a button to
// recenter; a trackpad that seeks only while clicked; the simple controller,
// whose select button also recenters when held.
enum class ProfileStyle { kStick, kTrackpad, kSimple };

// Every profile the view suggests bindings for, in this order
std::vector<ProfileBindings> BindingTable();
// The bindings of |profile|, none for a profile not in the table
std::vector<Binding> BindingsFor(const std::string& profile);
ProfileStyle StyleOf(const std::string& profile);
// The extensions whose profiles the table holds (asked as optional)
std::vector<std::string> ControllerExtensions();

// What one hand gives at one instant
struct HandInput {
  // play_pause, 0 to 1 (a trigger's value; a button is 0 or 1)
  float play = 0.0f;
  // seek's x, -1 to 1
  float seek_x = 0.0f;
  bool seek_press = false;
  bool recenter = false;
  bool exit = false;
};

enum class Gesture { kPlayPause, kSeek, kRecenter, kLyingDown, kExit };

const char* GestureName(Gesture gesture);

struct Fired {
  Gesture gesture = Gesture::kPlayPause;
  // 0 left, 1 right
  int hand = 0;
  // kSeek: seconds, signed
  double seconds = 0.0;
};

struct Pulse {
  int hand = 0;
  float amplitude = 0.5f;
  double seconds = 0.02;
};

// The gestures of one hand, fed at every frame with the time and the inputs:
// - play or pause: the trigger pressed above 0.75 (released under 0.35);
//   with the simple controller on release before 1 s, recenter once held 1 s;
// - seek: once the stick goes past 0.7 left or right, 10 s, again every
//   0.5 s while held, 30 s steps once held 2 s, over once back under 0.3; a
//   trackpad only while clicked;
// - recenter on release before 1 s, lying down toggled once held 1 s (a
//   pulse then);
// - leave once held 1 s (a short pulse at the press, a longer one at 1 s).
// After |Reset| (focus lost) every input must be seen released before it
// counts again: no gesture from a button held across the loss.
class Gestures {
 public:
  static constexpr double kHold = 1.0;
  static constexpr double kSeekRepeat = 0.5;
  static constexpr double kSeekFastAfter = 2.0;
  static constexpr double kSeekStep = 10.0;
  static constexpr double kSeekFastStep = 30.0;

  explicit Gestures(ProfileStyle style = ProfileStyle::kStick)
      : style_(style) {}

  void SetStyle(ProfileStyle style);
  ProfileStyle style() const { return style_; }
  void Reset();
  void Update(double now, const HandInput& input, int hand,
              std::vector<Fired>* fired, std::vector<Pulse>* pulses);

 private:
  struct Hold {
    bool down = false;
    bool armed = true;
    bool consumed = false;
    double since = 0.0;
  };

  ProfileStyle style_;
  Hold play_;
  Hold recenter_;
  Hold exit_;
  bool seek_armed_ = true;
  int seek_direction_ = 0;
  double seek_since_ = 0.0;
  double seek_next_ = 0.0;
};

// The action set, its actions and the suggested bindings on a session, then
// the inputs read at every frame while the session is focused.
class Input {
 public:
  using Log = std::function<void(const std::string&)>;

  Input(Runtime* runtime, Log log) : runtime_(runtime), log_(std::move(log)) {}

  // After xrCreateSession, before the session begins. |enabled| the instance
  // extensions on (a profile that needs one is suggested only with it);
  // |names| the localized names by action name (and "player" for the set),
  // English when absent. Never fatal: false when the view runs without
  // controllers, the reason logged.
  bool Setup(const std::vector<std::string>& enabled,
             const std::map<std::string, std::string>& names);

  // The profiles suggested and accepted
  const std::vector<std::string>& accepted() const { return accepted_; }
  bool ready() const { return ready_; }

  // The interaction profile of each hand read again (after the runtime's
  // XrEventDataInteractionProfileChanged); true for a hand whose changed
  bool ReadProfiles(bool changed[2]);
  const std::string& profile(int hand) const { return profiles_[hand]; }

  // One frame: reads the actions while |focused| (nothing otherwise, and the
  // gestures reset once focus is lost), the gestures, the pulses.
  void Update(double now, bool focused, std::vector<Fired>* fired);

  // Number of SyncActions calls made (the tests check none while unfocused)
  int64_t syncs() const { return syncs_; }

 private:
  uint64_t Make(const std::string& name, ActionType type,
                const std::map<std::string, std::string>& names);

  Runtime* runtime_;
  Log log_;
  bool ready_ = false;
  bool focused_ = false;
  XrPath hands_[2] = {XR_NULL_PATH, XR_NULL_PATH};
  uint64_t set_ = 0;
  std::map<std::string, uint64_t> actions_;
  std::vector<std::string> accepted_;
  std::string profiles_[2];
  Gestures gestures_[2];
  int64_t syncs_ = 0;
};

}  // namespace xr_probe

#endif  // XR_INPUT_H_
