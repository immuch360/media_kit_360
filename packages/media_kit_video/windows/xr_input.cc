// Immuch360: the headset view's controllers (IMMUCH360-NOTE.md, patch 15); see
// the header.

#include "xr_input.h"

#include <cmath>

namespace xr_probe {

namespace {

constexpr const char* kLeft = "/user/hand/left";
constexpr const char* kRight = "/user/hand/right";

// A small margin for times added up from frames
constexpr double kEpsilon = 1e-9;

void Both(std::vector<Binding>* out, const char* action,
          const std::string& input) {
  out->push_back({action, std::string(kLeft) + input});
  out->push_back({action, std::string(kRight) + input});
}

void One(std::vector<Binding>* out, const char* action, const char* hand,
         const std::string& input) {
  out->push_back({action, std::string(hand) + input});
}

// A localized name within OpenXR's 128 bytes, cut on a UTF-8 boundary
std::string Localized(const std::string& name) {
  constexpr size_t kMax = XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1;
  if (name.size() <= kMax) return name;
  size_t end = kMax;
  while (end > 0 && (static_cast<unsigned char>(name[end]) & 0xC0) == 0x80) {
    end--;
  }
  return name.substr(0, end);
}

const std::map<std::string, std::string>& EnglishNames() {
  static const std::map<std::string, std::string> names = {
      {kActionSet, "Video player"},
      {kActionPlayPause, "Play or pause"},
      {kActionSeek, "Seek"},
      {kActionSeekPress, "Seek on the pad"},
      {kActionRecenter, "Recenter or lie down"},
      {kActionExit, "Leave the headset view"},
      {kActionHaptic, "Vibration"},
  };
  return names;
}

std::string NameOf(const std::string& action,
                   const std::map<std::string, std::string>& names) {
  const auto given = names.find(action);
  if (given != names.end() && !given->second.empty()) {
    return Localized(given->second);
  }
  return EnglishNames().at(action);
}

enum class HoldStep { kNothing, kPressed, kHeld, kReleasedShort, kReleasedAfter };

}  // namespace

std::vector<ProfileBindings> BindingTable() {
  std::vector<ProfileBindings> table;
  {
    ProfileBindings p{"/interaction_profiles/khr/simple_controller", "", {}};
    Both(&p.bindings, kActionPlayPause, "/input/select/click");
    Both(&p.bindings, kActionExit, "/input/menu/click");
    Both(&p.bindings, kActionHaptic, "/output/haptic");
    table.push_back(p);
  }
  {
    ProfileBindings p{"/interaction_profiles/oculus/touch_controller", "", {}};
    Both(&p.bindings, kActionPlayPause, "/input/trigger/value");
    One(&p.bindings, kActionPlayPause, kRight, "/input/a/click");
    One(&p.bindings, kActionPlayPause, kLeft, "/input/x/click");
    Both(&p.bindings, kActionSeek, "/input/thumbstick");
    Both(&p.bindings, kActionRecenter, "/input/thumbstick/click");
    One(&p.bindings, kActionExit, kLeft, "/input/menu/click");
    One(&p.bindings, kActionExit, kRight, "/input/b/click");
    Both(&p.bindings, kActionHaptic, "/output/haptic");
    table.push_back(p);
  }
  {
    ProfileBindings p{"/interaction_profiles/valve/index_controller", "", {}};
    Both(&p.bindings, kActionPlayPause, "/input/trigger/click");
    Both(&p.bindings, kActionPlayPause, "/input/a/click");
    Both(&p.bindings, kActionSeek, "/input/thumbstick");
    Both(&p.bindings, kActionRecenter, "/input/thumbstick/click");
    Both(&p.bindings, kActionExit, "/input/b/click");
    Both(&p.bindings, kActionHaptic, "/output/haptic");
    table.push_back(p);
  }
  {
    ProfileBindings p{"/interaction_profiles/htc/vive_controller", "", {}};
    Both(&p.bindings, kActionPlayPause, "/input/trigger/click");
    Both(&p.bindings, kActionSeek, "/input/trackpad");
    Both(&p.bindings, kActionSeekPress, "/input/trackpad/click");
    Both(&p.bindings, kActionRecenter, "/input/squeeze/click");
    Both(&p.bindings, kActionExit, "/input/menu/click");
    Both(&p.bindings, kActionHaptic, "/output/haptic");
    table.push_back(p);
  }
  {
    ProfileBindings p{"/interaction_profiles/microsoft/motion_controller", "",
                      {}};
    Both(&p.bindings, kActionPlayPause, "/input/trigger/value");
    Both(&p.bindings, kActionSeek, "/input/thumbstick");
    Both(&p.bindings, kActionRecenter, "/input/thumbstick/click");
    Both(&p.bindings, kActionExit, "/input/menu/click");
    Both(&p.bindings, kActionHaptic, "/output/haptic");
    table.push_back(p);
  }
  {
    ProfileBindings p{"/interaction_profiles/hp/mixed_reality_controller",
                      "XR_EXT_hp_mixed_reality_controller",
                      {}};
    Both(&p.bindings, kActionPlayPause, "/input/trigger/value");
    One(&p.bindings, kActionPlayPause, kRight, "/input/a/click");
    One(&p.bindings, kActionPlayPause, kLeft, "/input/x/click");
    Both(&p.bindings, kActionSeek, "/input/thumbstick");
    Both(&p.bindings, kActionRecenter, "/input/thumbstick/click");
    Both(&p.bindings, kActionExit, "/input/menu/click");
    Both(&p.bindings, kActionHaptic, "/output/haptic");
    table.push_back(p);
  }
  // XR_BD_controller_interaction: the Pico 4 has Menu on the left hand only
  // (the right one's is the system button), the Neo3 on both
  for (const char* profile : {"/interaction_profiles/bytedance/pico4_controller",
                              "/interaction_profiles/bytedance/pico_neo3_controller"}) {
    ProfileBindings p{profile, "XR_BD_controller_interaction", {}};
    Both(&p.bindings, kActionPlayPause, "/input/trigger/value");
    One(&p.bindings, kActionPlayPause, kRight, "/input/a/click");
    One(&p.bindings, kActionPlayPause, kLeft, "/input/x/click");
    Both(&p.bindings, kActionSeek, "/input/thumbstick");
    Both(&p.bindings, kActionRecenter, "/input/thumbstick/click");
    if (std::string(profile).find("pico4") != std::string::npos) {
      One(&p.bindings, kActionExit, kLeft, "/input/menu/click");
      One(&p.bindings, kActionExit, kRight, "/input/b/click");
    } else {
      Both(&p.bindings, kActionExit, "/input/menu/click");
    }
    Both(&p.bindings, kActionHaptic, "/output/haptic");
    table.push_back(p);
  }
  return table;
}

std::vector<Binding> BindingsFor(const std::string& profile) {
  for (const auto& entry : BindingTable()) {
    if (entry.profile == profile) return entry.bindings;
  }
  return {};
}

ProfileStyle StyleOf(const std::string& profile) {
  if (profile == "/interaction_profiles/khr/simple_controller") {
    return ProfileStyle::kSimple;
  }
  if (profile == "/interaction_profiles/htc/vive_controller") {
    return ProfileStyle::kTrackpad;
  }
  return ProfileStyle::kStick;
}

std::vector<std::string> ControllerExtensions() {
  std::vector<std::string> out;
  for (const auto& entry : BindingTable()) {
    if (entry.extension.empty()) continue;
    bool known = false;
    for (const auto& name : out) known = known || name == entry.extension;
    if (!known) out.push_back(entry.extension);
  }
  return out;
}

const char* GestureName(Gesture gesture) {
  switch (gesture) {
    case Gesture::kPlayPause:
      return "playPause";
    case Gesture::kSeek:
      return "seek";
    case Gesture::kRecenter:
      return "recenter";
    case Gesture::kLyingDown:
      return "lyingDown";
    case Gesture::kExit:
      return "exit";
  }
  return "?";
}

void Gestures::SetStyle(ProfileStyle style) {
  if (style != style_) {
    style_ = style;
    Reset();
  }
}

void Gestures::Reset() {
  for (Hold* hold : {&play_, &recenter_, &exit_}) {
    *hold = Hold{};
    hold->armed = false;
  }
  seek_armed_ = false;
  seek_direction_ = 0;
}

namespace {

HoldStep Step(double now, bool pressed, double hold_seconds, bool* down,
              bool* armed, bool* consumed, double* since) {
  if (!*down) {
    if (!*armed) {
      if (!pressed) *armed = true;
      return HoldStep::kNothing;
    }
    if (pressed) {
      *down = true;
      *since = now;
      *consumed = false;
      return HoldStep::kPressed;
    }
    return HoldStep::kNothing;
  }
  if (!pressed) {
    *down = false;
    return *consumed ? HoldStep::kReleasedAfter : HoldStep::kReleasedShort;
  }
  if (!*consumed && now - *since + kEpsilon >= hold_seconds) {
    *consumed = true;
    return HoldStep::kHeld;
  }
  return HoldStep::kNothing;
}

}  // namespace

void Gestures::Update(double now, const HandInput& input, int hand,
                      std::vector<Fired>* fired, std::vector<Pulse>* pulses) {
  const auto fire = [&](Gesture gesture, double seconds) {
    fired->push_back({gesture, hand, seconds});
  };
  const auto pulse = [&](float amplitude, double seconds) {
    pulses->push_back({hand, amplitude, seconds});
  };

  // Play or pause: a trigger is a button above 0.75, released under 0.35
  const bool trigger =
      play_.down ? input.play >= 0.35f : input.play > 0.75f;
  switch (Step(now, trigger, kHold, &play_.down, &play_.armed, &play_.consumed,
               &play_.since)) {
    case HoldStep::kPressed:
      if (style_ != ProfileStyle::kSimple) {
        play_.consumed = true;
        fire(Gesture::kPlayPause, 0.0);
        pulse(0.4f, 0.02);
      }
      break;
    case HoldStep::kHeld:
      // Only the simple controller gets here (the others are consumed)
      fire(Gesture::kRecenter, 0.0);
      pulse(0.8f, 0.08);
      break;
    case HoldStep::kReleasedShort:
      if (style_ == ProfileStyle::kSimple) {
        fire(Gesture::kPlayPause, 0.0);
        pulse(0.4f, 0.02);
      }
      break;
    default:
      break;
  }

  // Recenter on a short press, lying down once held
  switch (Step(now, input.recenter, kHold, &recenter_.down, &recenter_.armed,
               &recenter_.consumed, &recenter_.since)) {
    case HoldStep::kReleasedShort:
      fire(Gesture::kRecenter, 0.0);
      pulse(0.4f, 0.02);
      break;
    case HoldStep::kHeld:
      fire(Gesture::kLyingDown, 0.0);
      pulse(0.8f, 0.08);
      break;
    default:
      break;
  }

  // Leave once held, never on a short press
  switch (Step(now, input.exit, kHold, &exit_.down, &exit_.armed,
               &exit_.consumed, &exit_.since)) {
    case HoldStep::kPressed:
      pulse(0.3f, 0.02);
      break;
    case HoldStep::kHeld:
      fire(Gesture::kExit, 0.0);
      pulse(0.9f, 0.12);
      break;
    default:
      break;
  }

  // Seek: a trackpad only while clicked
  const float x = style_ == ProfileStyle::kTrackpad && !input.seek_press
                      ? 0.0f
                      : input.seek_x;
  const float size = std::abs(x);
  if (!seek_armed_) {
    if (size < 0.3f) seek_armed_ = true;
    return;
  }
  const int direction = x > 0.0f ? 1 : -1;
  if (seek_direction_ != 0 &&
      (size < 0.3f || (size > 0.7f && direction != seek_direction_))) {
    seek_direction_ = 0;
  }
  if (seek_direction_ == 0) {
    if (size > 0.7f) {
      seek_direction_ = direction;
      seek_since_ = now;
      seek_next_ = now + kSeekRepeat;
      fire(Gesture::kSeek, direction * kSeekStep);
      pulse(0.25f, 0.01);
    }
    return;
  }
  if (now + kEpsilon >= seek_next_) {
    const bool fast = now - seek_since_ + kEpsilon >= kSeekFastAfter;
    fire(Gesture::kSeek, seek_direction_ * (fast ? kSeekFastStep : kSeekStep));
    pulse(0.25f, 0.01);
    seek_next_ += kSeekRepeat;
  }
}

uint64_t Input::Make(const std::string& name, ActionType type,
                     const std::map<std::string, std::string>& names) {
  ActionSpec spec;
  spec.name = name;
  spec.localized = NameOf(name, names);
  spec.type = type;
  spec.subactions = {hands_[0], hands_[1]};
  uint64_t id = 0;
  auto result = runtime_->CreateAction(set_, spec, &id);
  if (!XR_SUCCEEDED(result) && spec.localized != EnglishNames().at(name)) {
    // A name the runtime refuses (a duplicate): the English one
    log_("action " + name + ": the localized name refused (" +
         runtime_->ResultName(result) + "), the English one used");
    spec.localized = EnglishNames().at(name);
    result = runtime_->CreateAction(set_, spec, &id);
  }
  if (!XR_SUCCEEDED(result)) {
    log_("action " + name + ": " + runtime_->ResultName(result));
    return 0;
  }
  actions_[name] = id;
  return id;
}

bool Input::Setup(const std::vector<std::string>& enabled,
                  const std::map<std::string, std::string>& names) {
  if (!XR_SUCCEEDED(runtime_->StringToPath(kLeft, &hands_[0])) ||
      !XR_SUCCEEDED(runtime_->StringToPath(kRight, &hands_[1]))) {
    log_("controllers: the hand paths could not be made: no controllers");
    return false;
  }
  auto result = runtime_->CreateActionSet(kActionSet, NameOf(kActionSet, names),
                                          &set_);
  if (!XR_SUCCEEDED(result)) {
    result = runtime_->CreateActionSet(kActionSet,
                                       EnglishNames().at(kActionSet), &set_);
  }
  if (!XR_SUCCEEDED(result)) {
    log_("controllers: the action set: " + runtime_->ResultName(result) +
         ": no controllers");
    return false;
  }
  const bool made = Make(kActionPlayPause, ActionType::kFloat, names) != 0 &&
                    Make(kActionSeek, ActionType::kVector2, names) != 0 &&
                    Make(kActionSeekPress, ActionType::kBoolean, names) != 0 &&
                    Make(kActionRecenter, ActionType::kBoolean, names) != 0 &&
                    Make(kActionExit, ActionType::kBoolean, names) != 0 &&
                    Make(kActionHaptic, ActionType::kVibration, names) != 0;
  if (!made) {
    log_("controllers: an action could not be made: no controllers");
    return false;
  }
  for (const auto& entry : BindingTable()) {
    if (!entry.extension.empty()) {
      bool on = false;
      for (const auto& name : enabled) on = on || name == entry.extension;
      if (!on) {
        log_("controller profile " + entry.profile +
             ": not suggested (the runtime has no " + entry.extension + ")");
        continue;
      }
    }
    XrPath profile = XR_NULL_PATH;
    result = runtime_->StringToPath(entry.profile, &profile);
    std::vector<SuggestedBinding> bindings;
    for (const auto& binding : entry.bindings) {
      if (!XR_SUCCEEDED(result)) break;
      XrPath path = XR_NULL_PATH;
      result = runtime_->StringToPath(binding.path, &path);
      bindings.push_back({actions_[binding.action], path});
    }
    if (XR_SUCCEEDED(result)) {
      result = runtime_->SuggestBindings(profile, bindings);
    }
    if (XR_SUCCEEDED(result)) {
      accepted_.push_back(entry.profile);
      log_("controller profile " + entry.profile + ": accepted (" +
           std::to_string(bindings.size()) + " bindings)");
    } else {
      log_("controller profile " + entry.profile + ": rejected (" +
           runtime_->ResultName(result) + "), skipped");
    }
  }
  if (accepted_.empty()) {
    log_("controllers: no profile accepted: the view runs without "
         "controllers");
  }
  result = runtime_->AttachActionSets(set_);
  if (!XR_SUCCEEDED(result)) {
    log_("controllers: xrAttachSessionActionSets: " +
         runtime_->ResultName(result) + ": no controllers");
    accepted_.clear();
    return false;
  }
  ready_ = true;
  return !accepted_.empty();
}

bool Input::ReadProfiles(bool changed[2]) {
  bool any = false;
  for (int hand = 0; hand < 2; hand++) {
    changed[hand] = false;
    if (!ready_) continue;
    std::string profile;
    if (!XR_SUCCEEDED(runtime_->CurrentProfile(hands_[hand], &profile))) {
      continue;
    }
    if (profile != profiles_[hand]) {
      profiles_[hand] = profile;
      gestures_[hand].SetStyle(StyleOf(profile));
      changed[hand] = true;
      any = true;
    }
  }
  return any;
}

void Input::Update(double now, bool focused, std::vector<Fired>* fired) {
  if (!ready_) {
    return;
  }
  if (!focused) {
    if (focused_) {
      gestures_[0].Reset();
      gestures_[1].Reset();
      log_("controllers: focus lost, the gestures reset");
    }
    focused_ = false;
    return;
  }
  focused_ = true;
  syncs_++;
  if (!XR_SUCCEEDED(runtime_->SyncActions(set_))) {
    return;
  }
  std::vector<Pulse> pulses;
  for (int hand = 0; hand < 2; hand++) {
    HandInput input;
    FloatState play;
    if (XR_SUCCEEDED(runtime_->GetFloat(actions_[kActionPlayPause],
                                        hands_[hand], &play)) &&
        play.active) {
      input.play = play.value;
    }
    Vector2State seek;
    if (XR_SUCCEEDED(runtime_->GetVector2(actions_[kActionSeek], hands_[hand],
                                          &seek)) &&
        seek.active) {
      input.seek_x = seek.x;
    }
    BoolState state;
    if (XR_SUCCEEDED(runtime_->GetBoolean(actions_[kActionSeekPress],
                                          hands_[hand], &state))) {
      input.seek_press = state.active && state.value;
    }
    state = BoolState{};
    if (XR_SUCCEEDED(runtime_->GetBoolean(actions_[kActionRecenter],
                                          hands_[hand], &state))) {
      input.recenter = state.active && state.value;
    }
    state = BoolState{};
    if (XR_SUCCEEDED(runtime_->GetBoolean(actions_[kActionExit], hands_[hand],
                                          &state))) {
      input.exit = state.active && state.value;
    }
    gestures_[hand].Update(now, input, hand, fired, &pulses);
  }
  for (const auto& pulse : pulses) {
    runtime_->ApplyHaptic(actions_[kActionHaptic], hands_[pulse.hand],
                          pulse.amplitude,
                          static_cast<XrDuration>(pulse.seconds * 1e9));
  }
}

}  // namespace xr_probe
