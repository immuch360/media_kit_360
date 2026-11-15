// Immuch360: the headset view (IMMUCH360-NOTE.md, patch 15), the two sides
// the controller (xr_controller.h) drives: the OpenXR runtime and the GPU that
// draws the eyes. On Windows they are xr_openxr_runtime.cc (the Khronos loader
// and whatever runtime is active: SteamVR, Meta Horizon Link, Virtual
// Desktop, Pico Connect) and xr_angle_gpu.cc (the plugin's ANGLE device and
// renderer C); the tests give fakes of both (test/xr_probe_test.cc,
// test/xr_v1_test.cc). Core openxr.h types only: no Windows, Direct3D or
// OpenGL header here.

#ifndef XR_RUNTIME_H_
#define XR_RUNTIME_H_

#include <openxr/openxr.h>

#include <cstdint>
#include <string>
#include <vector>

#include "projection_view.h"
#include "xr_probe_core.h"

namespace xr_probe {

struct RuntimeInfo {
  std::string name;
  uint64_t version = 0;
  // Every instance extension the runtime offers, and those enabled
  std::vector<std::string> offered;
  std::vector<std::string> enabled;
  // The API layers the loader lists (implicit ones run without being asked:
  // overlays, recorders, a known source of crashes)
  std::vector<std::string> api_layers;
};

struct SystemInfo {
  std::string name;
  uint32_t vendor_id = 0;
  uint32_t max_swapchain_width = 0;
  uint32_t max_swapchain_height = 0;
  uint32_t max_layers = 0;
  bool orientation_tracking = false;
  bool position_tracking = false;
  // The stereo view configuration: the size the runtime recommends per eye
  // (SteamVR's render scale included), its maximum
  uint32_t eye_width[2] = {0, 0};
  uint32_t eye_height[2] = {0, 0};
  uint32_t eye_max_width[2] = {0, 0};
  uint32_t eye_max_height[2] = {0, 0};
  // xrGetD3D11GraphicsRequirementsKHR
  AdapterId adapter;
  int32_t min_feature_level = 0;
  std::vector<XrEnvironmentBlendMode> blend_modes;
};

struct Event {
  enum class Type {
    kSessionState,
    kInstanceLossPending,
    kReferenceSpaceChange,
    kEventsLost,
    kRefreshRateChanged,
    kInteractionProfileChanged,
    kOther,
  };
  Type type = Type::kOther;
  XrSessionState state = XR_SESSION_STATE_UNKNOWN;
  float from_rate = 0.0f;
  float to_rate = 0.0f;
  uint32_t lost = 0;
  XrStructureType other = XR_TYPE_UNKNOWN;
};

struct FrameState {
  XrTime predicted_display_time = 0;
  XrDuration predicted_display_period = 0;
  bool should_render = false;
};

struct Swapchain {
  uint64_t id = 0;
  int64_t format = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  // ID3D11Texture2D* on Windows, anything a fake likes in the tests
  std::vector<void*> images;
};

struct QuadLayer {
  uint64_t swapchain = 0;
  XrEyeVisibility eye = XR_EYE_VISIBILITY_BOTH;
  XrRect2Di rect{};
  XrPosef pose{};
  XrExtent2Df size{};
};

// What xrEndFrame shows: nothing (the headset keeps its last frame or the
// runtime's), one projection layer from two swapchains, and quads over it in
// their order (the screen, then the strip)
struct Layers {
  bool projection = false;
  XrCompositionLayerProjectionView views[2]{};
  uint64_t view_swapchains[2] = {0, 0};
  std::vector<QuadLayer> quads;
};

// The action system (OpenXR 1.0, chapter 11), as the controller's input
// (xr_input.h) uses it. Ids are the runtime's own numbering of its handles.
enum class ActionType { kBoolean, kFloat, kVector2, kVibration };

struct ActionSpec {
  // ASCII, lower case (OpenXR's rule); |localized| UTF-8, the app's language
  std::string name;
  std::string localized;
  ActionType type = ActionType::kBoolean;
  // /user/hand/left and /user/hand/right
  std::vector<XrPath> subactions;
};

struct SuggestedBinding {
  uint64_t action = 0;
  XrPath binding = XR_NULL_PATH;
};

struct BoolState {
  bool value = false;
  bool active = false;
};

struct FloatState {
  float value = 0.0f;
  bool active = false;
};

struct Vector2State {
  float x = 0.0f;
  float y = 0.0f;
  bool active = false;
};

class Runtime {
 public:
  virtual ~Runtime() = default;

  // The loader, the instance with XR_KHR_D3D11_enable and those of
  // |optional| the runtime offers. XR_ERROR_RUNTIME_UNAVAILABLE (or
  // XR_ERROR_RUNTIME_FAILURE) when no OpenXR runtime is installed or active.
  virtual XrResult CreateInstance(const std::vector<std::string>& optional,
                                  RuntimeInfo* info) = 0;
  // True when the last CreateInstance failed because the loader itself could
  // not be loaded (no openxr_loader.dll next to the app)
  virtual bool LoaderMissing() const { return false; }
  // The head mounted display; XR_ERROR_FORM_FACTOR_UNAVAILABLE while none is
  // connected (a Quest not yet linked).
  virtual XrResult GetSystem(SystemInfo* info) = 0;
  // A session on |device| (ID3D11Device*), its LOCAL space.
  virtual XrResult CreateSession(void* device) = 0;
  virtual XrResult SwapchainFormats(std::vector<int64_t>* formats) = 0;
  virtual XrResult CreateSwapchain(int64_t format, uint32_t width,
                                   uint32_t height, Swapchain* out) = 0;
  virtual void DestroySwapchain(uint64_t id) = 0;
  // XR_SUCCESS with |event| filled, XR_EVENT_UNAVAILABLE once no event waits,
  // or the error of xrPollEvent (XR_ERROR_INSTANCE_LOST when the runtime quit
  // while the session was not running, so no frame call could see it).
  virtual XrResult PollEvent(Event* event) = 0;
  virtual XrResult BeginSession() = 0;
  virtual XrResult EndSession() = 0;
  virtual XrResult RequestExitSession() = 0;
  virtual XrResult WaitFrame(FrameState* state) = 0;
  virtual XrResult BeginFrame() = 0;
  // The two eyes at |time| in the LOCAL space; |orientation_valid| false
  // while the headset does not track (the view configuration's flags).
  virtual XrResult LocateViews(XrTime time, XrView views[2],
                               bool* orientation_valid) = 0;
  virtual XrResult AcquireImage(uint64_t swapchain, uint32_t* index) = 0;
  virtual XrResult WaitImage(uint64_t swapchain) = 0;
  virtual XrResult ReleaseImage(uint64_t swapchain) = 0;
  virtual XrResult EndFrame(XrTime time, const Layers& layers) = 0;
  // XR_FB_display_refresh_rate; XR_ERROR_FUNCTION_UNSUPPORTED without it.
  virtual XrResult RefreshRates(std::vector<float>* rates, float* current) = 0;
  virtual XrResult RequestRefreshRate(float rate) = 0;

  // The action system, once the instance exists (the action set and its
  // actions) and the session (attaching, syncing, reading). Every call fails
  // with an error, never crashes, when what it needs is missing.
  virtual XrResult StringToPath(const std::string& path, XrPath* out) = 0;
  virtual XrResult CreateActionSet(const std::string& name,
                                   const std::string& localized,
                                   uint64_t* id) = 0;
  virtual XrResult CreateAction(uint64_t set, const ActionSpec& spec,
                                uint64_t* id) = 0;
  // xrSuggestInteractionProfileBindings: XR_ERROR_PATH_UNSUPPORTED for a
  // profile or a path the runtime does not know
  virtual XrResult SuggestBindings(
      XrPath profile, const std::vector<SuggestedBinding>& bindings) = 0;
  virtual XrResult AttachActionSets(uint64_t set) = 0;
  // XR_SESSION_NOT_FOCUSED (a success) while the session is not focused
  virtual XrResult SyncActions(uint64_t set) = 0;
  virtual XrResult GetBoolean(uint64_t action, XrPath hand, BoolState* out) = 0;
  virtual XrResult GetFloat(uint64_t action, XrPath hand, FloatState* out) = 0;
  virtual XrResult GetVector2(uint64_t action, XrPath hand,
                              Vector2State* out) = 0;
  virtual XrResult ApplyHaptic(uint64_t action, XrPath hand, float amplitude,
                               XrDuration duration) = 0;
  // The interaction profile bound to |hand| ("" while none)
  virtual XrResult CurrentProfile(XrPath hand, std::string* profile) = 0;

  virtual void DestroySession() = 0;
  virtual void DestroyInstance() = 0;
  virtual std::string ResultName(XrResult result) = 0;
};

struct GpuInfo {
  std::string adapter_name;
  AdapterId adapter;
  int32_t feature_level = 0;
  // DXGI_ADAPTER_DESC's VendorId and DeviceId (0x1414 and 0x8c: the Basic
  // Render Driver)
  uint32_t vendor_id = 0;
  uint32_t device_id = 0;
  uint64_t dedicated_video_memory = 0;
  std::string gl_renderer;
  // ID3D11Device* of the plugin's ANGLE display
  void* device = nullptr;
  // Whether the device's immediate context had multithread protection before
  // the view turned it on
  bool was_multithread_protected = false;
  // Whether the view's context shares the players' textures
  bool shares_players = false;
};

struct GpuMemory {
  bool known = false;
  uint64_t usage = 0;
  uint64_t budget = 0;
};

// The frame of the player shown, as the plugin's frame source gives it.
struct FrameInfo {
  bool has_frame = false;
  int32_t frame_width = 0;
  int32_t frame_height = 0;
  // Frames drawn by the player since it started, to tell a new one
  uint64_t frame_count = 0;
};
// The probe's name of it
using Content = FrameInfo;

// The time the view's thread spent taking the player's frame (under the
// player's lock) since the last call
struct SnapshotTiming {
  int64_t count = 0;
  double total_ms = 0.0;
  double max_ms = 0.0;
};

class Gpu {
 public:
  virtual ~Gpu() = default;
  virtual bool Open(GpuInfo* info, std::string* error) = 0;
  // What is shown from now on (the player, the mode, the stereo layout)
  virtual void SetContent(const ContentRequest& content) = 0;
  // Every image of |swapchain| made drawable; |path| tells how ("direct":
  // drawn into the image itself through a linear view; "copy": drawn into a
  // texture of the view, copied into the image).
  virtual bool WrapImages(const Swapchain& swapchain, std::string* path,
                          std::string* error) = 0;
  virtual void UnwrapImages(uint64_t swapchain) = 0;
  // Eye |eye| (0 left, 1 right) through |view| (projection_view.h, the
  // anchor applied) into image |index|; |time| the content's clock (the
  // pattern's bar). The idle mode clears to dark grey. False with |error|
  // when nothing could be drawn.
  virtual bool DrawEye(const Swapchain& swapchain, uint32_t index, int eye,
                       const ProjectionView& view, double time,
                       std::string* error) = 0;
  // The frame as it is into image |index| (the screen of a flat film).
  virtual bool DrawScreen(const Swapchain& swapchain, uint32_t index,
                          std::string* error) = 0;
  // The strip's pixels (RGBA, |width| x |height|, the first row on top) into
  // image |index|, from its top left corner.
  virtual bool UploadStrip(const Swapchain& swapchain, uint32_t index,
                           const uint8_t* rgba, uint32_t width,
                           uint32_t height, std::string* error) = 0;
  // The GPU work so far, finished: the view times it every few seconds.
  virtual void Finish() = 0;
  virtual FrameInfo FrameNow() = 0;
  virtual GpuMemory Memory() = 0;
  virtual SnapshotTiming TakeSnapshotTiming() { return {}; }
  virtual void Close() = 0;
};

}  // namespace xr_probe

#endif  // XR_RUNTIME_H_
