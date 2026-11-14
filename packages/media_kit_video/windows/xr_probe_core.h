// Immuch360: the headset probe (IMMUCH360-NOTE.md, patch 15), the choices it
// makes that need neither a headset nor Windows: whether the runtime's
// graphics card is the plugin's, the swapchain format, the refresh rate, and
// the states of an OpenXR session. Only the core openxr.h (vendored, Apache
// 2.0 OR MIT) is included, so the CI tests all of it with any C++17 compiler
// (test/xr_probe_test.cc).

#ifndef XR_PROBE_CORE_H_
#define XR_PROBE_CORE_H_

#include <openxr/openxr.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace xr_probe {

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

}  // namespace xr_probe

#endif  // XR_PROBE_CORE_H_
