// Immuch360: the headset view (IMMUCH360-NOTE.md, patch 15), the choices it
// makes that need neither a headset nor Windows: whether the runtime's
// graphics card is the plugin's, the swapchain format, the refresh rate, the
// states of an OpenXR session, what is shown (the content), the codes a view
// stops with, the layout of a flat screen and the guess of a runtime from its
// manifest. Only the core openxr.h (vendored, Apache 2.0 OR MIT) is included,
// so the CI tests all of it with any C++17 compiler (test/xr_probe_test.cc,
// test/xr_v1_test.cc). The namespace keeps the probe's name.

#ifndef XR_PROBE_CORE_H_
#define XR_PROBE_CORE_H_

#include <openxr/openxr.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace xr_probe {

// What the headset shows
enum class Mode {
  // A 360 or 180 video (any layout of renderer C) around the head
  kSphere,
  // The frame as a flat screen ahead, 2D or 3D (quad layers, one per eye)
  kScreen,
  // The test pattern of renderer C (kind 3): grey ramp, one patch per eye,
  // a bar moving at the content's rate
  kPattern,
  // No player: a dark grey room, and the strip when the app gives one
  kIdle,
};

// How a frame holds its two eyes
enum class Stereo { kMono, kLeftRight, kTopBottom };
// The probe's name of it
using ScreenStereo = Stereo;

// A flat 3D film's eyes: full (each eye has the picture's shape: 32:9 side by
// side for a 16:9 film) or half (each eye squeezed to half the frame, shown
// as the whole frame's shape). Mono ignores it.
enum class Squeeze { kFull, kHalf };

// What the app shows, as it gives it (XrContent in Dart)
struct ContentRequest {
  // The player (Player.handle) whose frame is shown; 0 for none
  int64_t handle = 0;
  Mode mode = Mode::kIdle;
  Stereo stereo = Stereo::kMono;
  Squeeze squeeze = Squeeze::kFull;
  // The content's frame rate, 0 when unknown
  double fps = 0.0;
  // The window's yaw: what was ahead in the window is ahead in the headset
  double yaw = 0.0;
  // The kind of video, for the log (never a URL, a path or a title)
  std::string label;
};

inline const char* ModeName(Mode mode) {
  switch (mode) {
    case Mode::kSphere:
      return "sphere";
    case Mode::kScreen:
      return "screen";
    case Mode::kPattern:
      return "pattern";
    case Mode::kIdle:
      return "idle";
  }
  return "?";
}

inline const char* StereoName(Stereo stereo) {
  switch (stereo) {
    case Stereo::kMono:
      return "mono";
    case Stereo::kLeftRight:
      return "leftRight";
    case Stereo::kTopBottom:
      return "topBottom";
  }
  return "?";
}

inline const char* SqueezeName(Squeeze squeeze) {
  return squeeze == Squeeze::kHalf ? "half" : "full";
}

inline Mode ModeOf(const std::string& name) {
  if (name == "sphere") return Mode::kSphere;
  if (name == "screen") return Mode::kScreen;
  if (name == "pattern") return Mode::kPattern;
  return Mode::kIdle;
}

inline Stereo StereoOf(const std::string& name) {
  if (name == "leftRight") return Stereo::kLeftRight;
  if (name == "topBottom") return Stereo::kTopBottom;
  return Stereo::kMono;
}

inline Squeeze SqueezeOf(const std::string& name) {
  return name == "half" ? Squeeze::kHalf : Squeeze::kFull;
}

// Why a view ended, or why it did not start: the codes of XrStopReason in
// Dart, each with an English detail in the log and the state
enum class StopReason {
  kNone,
  // The app asked (stop, or the plugin going away)
  kStoppedByApp,
  // The runtime ended the session without the app asking (its dashboard, the
  // headset's menu)
  kEndedFromHeadset,
  // No OpenXR runtime installed or active (XR_ERROR_RUNTIME_UNAVAILABLE,
  // XR_ERROR_RUNTIME_FAILURE at xrCreateInstance)
  kNoRuntime,
  // openxr_loader.dll could not be loaded
  kLoaderMissing,
  // The runtime had no headset within the wait
  kNoHeadset,
  // The headset is on another graphics card than the plugin's
  kOtherGpu,
  // The plugin's device is under the runtime's feature level, or is
  // Microsoft's Basic Render Driver (no graphics driver)
  kGpuTooWeak,
  // The instance or the session was lost (the runtime stopped or updated)
  kRuntimeLost,
  // A stop did not end the view in time: the runtime hangs in a call
  kRuntimeNotAnswering,
  // A view already runs
  kAlreadyRunning,
  // The plugin was built without MEDIA_KIT_360_XR
  kNotInBuild,
  // Not Windows
  kNotWindows,
  // Anything else, the call and its result in the detail
  kFailed,
};

inline const char* StopReasonCode(StopReason reason) {
  switch (reason) {
    case StopReason::kNone:
      return "none";
    case StopReason::kStoppedByApp:
      return "stoppedByApp";
    case StopReason::kEndedFromHeadset:
      return "endedFromHeadset";
    case StopReason::kNoRuntime:
      return "noRuntime";
    case StopReason::kLoaderMissing:
      return "loaderMissing";
    case StopReason::kNoHeadset:
      return "noHeadset";
    case StopReason::kOtherGpu:
      return "otherGpu";
    case StopReason::kGpuTooWeak:
      return "gpuTooWeak";
    case StopReason::kRuntimeLost:
      return "runtimeLost";
    case StopReason::kRuntimeNotAnswering:
      return "runtimeNotAnswering";
    case StopReason::kAlreadyRunning:
      return "alreadyRunning";
    case StopReason::kNotInBuild:
      return "notInBuild";
    case StopReason::kNotWindows:
      return "notWindows";
    case StopReason::kFailed:
      return "failed";
  }
  return "failed";
}

// A result that means the runtime is gone
inline bool IsLoss(XrResult result) {
  return result == XR_ERROR_INSTANCE_LOST || result == XR_ERROR_SESSION_LOST;
}

// The code of a failed xrCreateInstance (or of the loader before it)
inline StopReason ReasonOfInstanceFailure(XrResult result,
                                          bool loader_missing) {
  if (loader_missing) {
    return StopReason::kLoaderMissing;
  }
  if (result == XR_ERROR_RUNTIME_UNAVAILABLE ||
      result == XR_ERROR_RUNTIME_FAILURE) {
    return StopReason::kNoRuntime;
  }
  if (IsLoss(result)) {
    return StopReason::kRuntimeLost;
  }
  return StopReason::kFailed;
}

// The code of any other failed call
inline StopReason ReasonOfCallFailure(XrResult result) {
  return IsLoss(result) ? StopReason::kRuntimeLost : StopReason::kFailed;
}

// Microsoft's Basic Render Driver (WARP): what Windows gives with no graphics
// driver, and GitHub's runners. It draws, slowly, on the processor: never a
// headset.
constexpr uint32_t kBasicRenderVendor = 0x1414;
constexpr uint32_t kBasicRenderDevice = 0x8c;

inline bool IsBasicRenderDriver(uint32_t vendor, uint32_t device) {
  return vendor == kBasicRenderVendor && device == kBasicRenderDevice;
}

// A graphics adapter as Windows names it (LUID: LowPart, HighPart), and the
// Direct3D feature level of a device (D3D_FEATURE_LEVEL values: 0xb000 for
// 11_0).
struct AdapterId {
  uint32_t low = 0;
  int32_t high = 0;
  bool operator==(const AdapterId& other) const {
    return low == other.low && high == other.high;
  }
};

enum class AdapterVerdict {
  // The plugin's device is on the adapter the runtime renders on
  kSame,
  // The headset hangs on another graphics card than the one the app draws
  // with (a laptop whose headset port is on the integrated GPU, a PC with two
  // cards): in process this cannot work, a helper on that card could
  kOtherAdapter,
  // Same adapter, but the device's feature level is under the runtime's
  // minimum
  kFeatureLevelTooLow,
};

inline AdapterVerdict CheckAdapter(AdapterId device, int32_t device_level,
                                   AdapterId runtime, int32_t min_level) {
  if (!(device == runtime)) {
    return AdapterVerdict::kOtherAdapter;
  }
  if (device_level < min_level) {
    return AdapterVerdict::kFeatureLevelTooLow;
  }
  return AdapterVerdict::kSame;
}

inline std::string AdapterVerdictName(AdapterVerdict verdict) {
  switch (verdict) {
    case AdapterVerdict::kSame:
      return "same adapter";
    case AdapterVerdict::kOtherAdapter:
      return "the headset is on another graphics card";
    case AdapterVerdict::kFeatureLevelTooLow:
      return "feature level under the runtime's minimum";
  }
  return "?";
}

// DXGI_FORMAT values the probe knows (dxgiformat.h)
constexpr int64_t kDxgiR8G8B8A8Typeless = 27;
constexpr int64_t kDxgiR8G8B8A8Unorm = 28;
constexpr int64_t kDxgiR8G8B8A8UnormSrgb = 29;
constexpr int64_t kDxgiB8G8R8A8Unorm = 87;
constexpr int64_t kDxgiB8G8R8A8Typeless = 90;
constexpr int64_t kDxgiB8G8R8A8UnormSrgb = 91;

inline bool IsSrgb(int64_t format) {
  return format == kDxgiR8G8B8A8UnormSrgb || format == kDxgiB8G8R8A8UnormSrgb;
}

inline bool IsBgra(int64_t format) {
  return format == kDxgiB8G8R8A8Unorm || format == kDxgiB8G8R8A8UnormSrgb ||
         format == kDxgiB8G8R8A8Typeless;
}

inline std::string DxgiFormatName(int64_t format) {
  switch (format) {
    case kDxgiR8G8B8A8Typeless:
      return "R8G8B8A8_TYPELESS";
    case kDxgiR8G8B8A8Unorm:
      return "R8G8B8A8_UNORM";
    case kDxgiR8G8B8A8UnormSrgb:
      return "R8G8B8A8_UNORM_SRGB";
    case kDxgiB8G8R8A8Unorm:
      return "B8G8R8A8_UNORM";
    case kDxgiB8G8R8A8Typeless:
      return "B8G8R8A8_TYPELESS";
    case kDxgiB8G8R8A8UnormSrgb:
      return "B8G8R8A8_UNORM_SRGB";
    default:
      return "DXGI format " + std::to_string(format);
  }
}

// The swapchain format, among those the runtime offers (its order is its
// preference): an 8 bit sRGB one first, since the video's values are already
// gamma encoded and the runtime decodes an sRGB image before it composes in
// linear light; written through a linear view, they reach the headset as
// they are. A plain UNORM one only when no sRGB one is offered (the runtime
// would then take the values as linear: too dark, the ramp tells). 0 when
// none of the four is offered.
inline int64_t ChooseSwapchainFormat(const std::vector<int64_t>& offered) {
  for (const auto wanted : {kDxgiR8G8B8A8UnormSrgb, kDxgiB8G8R8A8UnormSrgb,
                            kDxgiR8G8B8A8Unorm, kDxgiB8G8R8A8Unorm}) {
    for (const auto format : offered) {
      if (format == wanted) {
        return format;
      }
    }
  }
  return 0;
}

// The display rate for content of |fps| frames a second (0 when unknown),
// among |rates|: a whole multiple of it (within 0.5 %, so 23.976 and 29.97
// count), the one nearest |current|, the higher on a tie; |current| when no
// rate is a multiple (25 and 50 fps on a Quest 3: the judder stays) or the
// content rate is unknown.
inline float ChooseRefreshRate(const std::vector<float>& rates, float current,
                               double fps) {
  if (fps < 1.0 || rates.empty()) {
    return current;
  }
  float best = 0.0f;
  for (const auto rate : rates) {
    const double ratio = rate / fps;
    const double whole = std::round(ratio);
    if (whole < 1.0 || std::abs(ratio - whole) > 0.005 * whole) {
      continue;
    }
    if (best == 0.0f) {
      best = rate;
      continue;
    }
    const double distance = std::abs(rate - current);
    const double best_distance = std::abs(best - current);
    if (distance < best_distance - 1e-3 ||
        (std::abs(distance - best_distance) <= 1e-3 && rate > best)) {
      best = rate;
    }
  }
  return best == 0.0f ? current : best;
}

inline std::string SessionStateName(XrSessionState state) {
  switch (state) {
    case XR_SESSION_STATE_UNKNOWN:
      return "unknown";
    case XR_SESSION_STATE_IDLE:
      return "idle";
    case XR_SESSION_STATE_READY:
      return "ready";
    case XR_SESSION_STATE_SYNCHRONIZED:
      return "synchronized";
    case XR_SESSION_STATE_VISIBLE:
      return "visible";
    case XR_SESSION_STATE_FOCUSED:
      return "focused";
    case XR_SESSION_STATE_STOPPING:
      return "stopping";
    case XR_SESSION_STATE_LOSS_PENDING:
      return "loss pending";
    case XR_SESSION_STATE_EXITING:
      return "exiting";
    default:
      return "state " + std::to_string(static_cast<int>(state));
  }
}

// What the session's state asks of the app (OpenXR 1.0, section 9.3, the
// session lifecycle): begin it once READY, end it once STOPPING (a headset
// taken off, the runtime's dashboard, a user who leaves), give up once
// EXITING or LOSS_PENDING. The other states change nothing the app does: the
// frame loop goes on, and |FrameState.should_render| says whether anything
// shows.
enum class SessionAction { kNone, kBegin, kEnd, kExit };

class SessionStateMachine {
 public:
  // The next state from the runtime, and what to do about it.
  SessionAction On(XrSessionState state) {
    state_ = state;
    switch (state) {
      case XR_SESSION_STATE_READY:
        return running_ ? SessionAction::kNone : SessionAction::kBegin;
      case XR_SESSION_STATE_STOPPING:
        return running_ ? SessionAction::kEnd : SessionAction::kNone;
      case XR_SESSION_STATE_EXITING:
      case XR_SESSION_STATE_LOSS_PENDING:
        return SessionAction::kExit;
      default:
        return SessionAction::kNone;
    }
  }

  // Whether xrBeginSession succeeded (the frame loop runs) or xrEndSession
  // was called (it stops).
  void Began() { running_ = true; }
  void Ended() { running_ = false; }

  bool running() const { return running_; }
  XrSessionState state() const { return state_; }
  // Visible or focused: the headset shows the app's frames
  bool visible() const {
    return state_ == XR_SESSION_STATE_VISIBLE ||
           state_ == XR_SESSION_STATE_FOCUSED;
  }

 private:
  XrSessionState state_ = XR_SESSION_STATE_UNKNOWN;
  bool running_ = false;
};

// The layout of a flat film on its screen: one quad (mono) or one per eye,
// the part of the frame each eye shows, and the shape of what an eye sees
// (width / height), from the frame's size and the eyes' squeeze as the app
// gives them (no guess from the frame's shape).
struct ScreenLayout {
  int quads = 1;
  XrRect2Di rects[2]{};
  double eye_aspect = 1.0;
};

inline ScreenLayout LayoutScreen(uint32_t width, uint32_t height, Stereo stereo,
                                 Squeeze squeeze) {
  ScreenLayout layout;
  const auto w = static_cast<int32_t>(width);
  const auto h = static_cast<int32_t>(height);
  const double aspect =
      static_cast<double>(width) / static_cast<double>((std::max)(1u, height));
  layout.rects[0] = {{0, 0}, {w, h}};
  layout.rects[1] = layout.rects[0];
  layout.eye_aspect = aspect;
  if (stereo == Stereo::kLeftRight) {
    layout.quads = 2;
    layout.rects[0] = {{0, 0}, {w / 2, h}};
    layout.rects[1] = {{w / 2, 0}, {w / 2, h}};
    layout.eye_aspect = squeeze == Squeeze::kFull ? aspect / 2.0 : aspect;
  } else if (stereo == Stereo::kTopBottom) {
    layout.quads = 2;
    layout.rects[0] = {{0, 0}, {w, h / 2}};
    layout.rects[1] = {{0, h / 2}, {w, h / 2}};
    layout.eye_aspect = squeeze == Squeeze::kFull ? aspect * 2.0 : aspect;
  }
  return layout;
}

// Whether the session shows the app's frames to the user (VISIBLE or
// FOCUSED), watched over time: off once it has not for more than |delay|
// seconds (the headset taken off, a dashboard over the app), on again when it
// shows again after an off. +1 on, -1 off, 0 nothing to tell.
class VisibilityWatch {
 public:
  explicit VisibilityWatch(double delay = 2.0) : delay_(delay) {}

  int Update(double now, bool visible) {
    if (visible) {
      seen_ = true;
      hidden_since_ = -1.0;
      if (reported_off_) {
        reported_off_ = false;
        return 1;
      }
      return 0;
    }
    // Not yet shown since the start: nothing was visible to leave
    if (!seen_ || reported_off_) {
      return 0;
    }
    if (hidden_since_ < 0.0) {
      hidden_since_ = now;
    }
    if (now - hidden_since_ > delay_) {
      reported_off_ = true;
      return -1;
    }
    return 0;
  }

 private:
  double delay_;
  bool seen_ = false;
  bool reported_off_ = false;
  double hidden_since_ = -1.0;
};

// The file name of a path, either separator
inline std::string FileNameOf(const std::string& path) {
  const auto slash = path.find_last_of("\\/");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

// The runtime a manifest belongs to, guessed from its path (folders and file
// name, case ignored): what the app shows before any DLL is loaded. "" when
// it is not one the app knows ("an OpenXR runtime").
inline std::string GuessRuntime(const std::string& manifest_path) {
  std::string path = manifest_path;
  std::transform(path.begin(), path.end(), path.begin(), [](char c) {
    return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  });
  const auto has = [&path](const char* part) {
    return path.find(part) != std::string::npos;
  };
  if (has("steamxr") || has("steamvr")) return "SteamVR";
  if (has("oculus") || has("meta horizon") || has("meta-horizon")) {
    return "Meta Horizon Link";
  }
  if (has("virtualdesktop") || has("virtual desktop")) {
    return "Virtual Desktop";
  }
  if (has("pico")) return "Pico Connect";
  if (has("mixedrealityruntime") || has("mixed reality")) {
    return "Windows Mixed Reality";
  }
  if (has("varjo")) return "Varjo";
  if (has("vive") || has("htc")) return "VIVE";
  if (has("pimax")) return "Pimax";
  if (has("monado")) return "Monado";
  if (has("wivrn")) return "WiVRn";
  return "";
}

}  // namespace xr_probe

#endif  // XR_PROBE_CORE_H_
