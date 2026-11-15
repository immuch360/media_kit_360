// Immuch360: the headset view (IMMUCH360-NOTE.md, patch 15), the OpenXR
// runtime through the Khronos loader (openxr_loader.dll, OpenXR-SDK 1.1.65,
// Apache 2.0 OR MIT, bundled next to the app by the build with the MEDIA_KIT_360_XR
// option). The loader is opened from its full path at the first start only,
// never at the app's start, and the runtime it finds (the active one:
// SteamVR, Meta Horizon Link, Virtual Desktop, Pico Connect...) is loaded by
// it then. Windows and Direct3D 11 only (XR_KHR_D3D11_enable).

#ifndef XR_OPENXR_RUNTIME_H_
#define XR_OPENXR_RUNTIME_H_

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d11.h>

#ifndef XR_USE_PLATFORM_WIN32
#define XR_USE_PLATFORM_WIN32
#endif
#ifndef XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D11
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <map>
#include <string>

#include "xr_runtime.h"

namespace xr_probe {

class OpenXrRuntime : public Runtime {
 public:
  // |loader| the full path of openxr_loader.dll
  explicit OpenXrRuntime(std::wstring loader);
  ~OpenXrRuntime() override;

  XrResult CreateInstance(const std::vector<std::string>& optional,
                          RuntimeInfo* info) override;
  bool LoaderMissing() const override { return loader_missing_; }
  XrResult GetSystem(SystemInfo* info) override;
  XrResult CreateSession(void* device) override;
  XrResult SwapchainFormats(std::vector<int64_t>* formats) override;
  XrResult CreateSwapchain(int64_t format, uint32_t width, uint32_t height,
                           Swapchain* out) override;
  void DestroySwapchain(uint64_t id) override;
  XrResult PollEvent(Event* event) override;
  XrResult BeginSession() override;
  XrResult EndSession() override;
  XrResult RequestExitSession() override;
  XrResult WaitFrame(FrameState* state) override;
  XrResult BeginFrame() override;
  XrResult LocateViews(XrTime time, XrView views[2],
                       bool* orientation_valid) override;
  XrResult AcquireImage(uint64_t swapchain, uint32_t* index) override;
  XrResult WaitImage(uint64_t swapchain) override;
  XrResult ReleaseImage(uint64_t swapchain) override;
  XrResult EndFrame(XrTime time, const Layers& layers) override;
  XrResult RefreshRates(std::vector<float>* rates, float* current) override;
  XrResult RequestRefreshRate(float rate) override;
  XrResult StringToPath(const std::string& path, XrPath* out) override;
  XrResult CreateActionSet(const std::string& name,
                           const std::string& localized,
                           uint64_t* id) override;
  XrResult CreateAction(uint64_t set, const ActionSpec& spec,
                        uint64_t* id) override;
  XrResult SuggestBindings(
      XrPath profile, const std::vector<SuggestedBinding>& bindings) override;
  XrResult AttachActionSets(uint64_t set) override;
  XrResult SyncActions(uint64_t set) override;
  XrResult GetBoolean(uint64_t action, XrPath hand, BoolState* out) override;
  XrResult GetFloat(uint64_t action, XrPath hand, FloatState* out) override;
  XrResult GetVector2(uint64_t action, XrPath hand,
                      Vector2State* out) override;
  XrResult ApplyHaptic(uint64_t action, XrPath hand, float amplitude,
                       XrDuration duration) override;
  XrResult CurrentProfile(XrPath hand, std::string* profile) override;
  void DestroySession() override;
  void DestroyInstance() override;
  std::string ResultName(XrResult result) override;

 private:
  template <typename T>
  bool Load(const char* name, T* function);
  XrSwapchain SwapchainOf(uint64_t id) const;
  XrAction ActionOf(uint64_t id) const;

  std::wstring loader_path_;
  HMODULE loader_ = nullptr;
  bool loader_missing_ = false;
  std::string loader_error_;
  PFN_xrGetInstanceProcAddr get_proc_ = nullptr;
  XrInstance instance_ = XR_NULL_HANDLE;
  XrSystemId system_ = XR_NULL_SYSTEM_ID;
  XrSession session_ = XR_NULL_HANDLE;
  XrSpace local_ = XR_NULL_HANDLE;
  XrEnvironmentBlendMode blend_ = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  bool refresh_rate_ = false;
  uint64_t next_swapchain_ = 1;
  std::map<uint64_t, XrSwapchain> swapchains_;
  bool actions_loaded_ = false;
  uint64_t next_action_ = 1;
  std::map<uint64_t, XrActionSet> action_sets_;
  std::map<uint64_t, XrAction> actions_;

  PFN_xrEnumerateInstanceExtensionProperties enumerate_extensions_ = nullptr;
  PFN_xrCreateInstance create_instance_ = nullptr;
  PFN_xrDestroyInstance destroy_instance_ = nullptr;
  PFN_xrGetInstanceProperties instance_properties_ = nullptr;
  PFN_xrResultToString result_to_string_ = nullptr;
  PFN_xrGetSystem get_system_ = nullptr;
  PFN_xrGetSystemProperties system_properties_ = nullptr;
  PFN_xrEnumerateViewConfigurationViews view_configuration_views_ = nullptr;
  PFN_xrEnumerateEnvironmentBlendModes blend_modes_ = nullptr;
  PFN_xrGetD3D11GraphicsRequirementsKHR d3d11_requirements_ = nullptr;
  PFN_xrCreateSession create_session_ = nullptr;
  PFN_xrDestroySession destroy_session_ = nullptr;
  PFN_xrCreateReferenceSpace create_space_ = nullptr;
  PFN_xrDestroySpace destroy_space_ = nullptr;
  PFN_xrEnumerateSwapchainFormats swapchain_formats_ = nullptr;
  PFN_xrCreateSwapchain create_swapchain_ = nullptr;
  PFN_xrDestroySwapchain destroy_swapchain_ = nullptr;
  PFN_xrEnumerateSwapchainImages swapchain_images_ = nullptr;
  PFN_xrPollEvent poll_event_ = nullptr;
  PFN_xrBeginSession begin_session_ = nullptr;
  PFN_xrEndSession end_session_ = nullptr;
  PFN_xrRequestExitSession request_exit_ = nullptr;
  PFN_xrWaitFrame wait_frame_ = nullptr;
  PFN_xrBeginFrame begin_frame_ = nullptr;
  PFN_xrEndFrame end_frame_ = nullptr;
  PFN_xrLocateViews locate_views_ = nullptr;
  PFN_xrAcquireSwapchainImage acquire_image_ = nullptr;
  PFN_xrWaitSwapchainImage wait_image_ = nullptr;
  PFN_xrReleaseSwapchainImage release_image_ = nullptr;
  PFN_xrEnumerateDisplayRefreshRatesFB refresh_rates_ = nullptr;
  PFN_xrGetDisplayRefreshRateFB refresh_rate_now_ = nullptr;
  PFN_xrRequestDisplayRefreshRateFB request_refresh_rate_ = nullptr;
  PFN_xrEnumerateApiLayerProperties api_layers_ = nullptr;
  PFN_xrStringToPath string_to_path_ = nullptr;
  PFN_xrPathToString path_to_string_ = nullptr;
  PFN_xrCreateActionSet create_action_set_ = nullptr;
  PFN_xrCreateAction create_action_ = nullptr;
  PFN_xrSuggestInteractionProfileBindings suggest_bindings_ = nullptr;
  PFN_xrAttachSessionActionSets attach_action_sets_ = nullptr;
  PFN_xrSyncActions sync_actions_ = nullptr;
  PFN_xrGetActionStateBoolean action_boolean_ = nullptr;
  PFN_xrGetActionStateFloat action_float_ = nullptr;
  PFN_xrGetActionStateVector2f action_vector2_ = nullptr;
  PFN_xrApplyHapticFeedback apply_haptic_ = nullptr;
  PFN_xrGetCurrentInteractionProfile current_profile_ = nullptr;
};

}  // namespace xr_probe

#endif  // XR_OPENXR_RUNTIME_H_
