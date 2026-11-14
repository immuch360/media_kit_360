// Immuch360: the headset probe's OpenXR runtime (IMMUCH360-NOTE.md, patch
// 15); see the header.

#include "xr_openxr_runtime.h"

#include <algorithm>
#include <cstring>

namespace xr_probe {

namespace {

constexpr const char* kD3D11 = "XR_KHR_D3D11_enable";
constexpr const char* kRefreshRate = "XR_FB_display_refresh_rate";

// strncpy without the deprecation warning of MSVC (C4996)
void CopyName(char* out, size_t size, const char* name) {
  const size_t length = (std::min)(size - 1, std::strlen(name));
  std::memcpy(out, name, length);
  out[length] = 0;
}

template <typename T>
T Typed(XrStructureType type) {
  T value{};
  value.type = type;
  return value;
}

}  // namespace

OpenXrRuntime::OpenXrRuntime(std::wstring loader)
    : loader_path_(std::move(loader)) {}

OpenXrRuntime::~OpenXrRuntime() {
  DestroySession();
  DestroyInstance();
  // The loader stays loaded: the runtime it loaded may keep threads of its
  // own until the process ends (SteamVR's client does)
}

template <typename T>
bool OpenXrRuntime::Load(const char* name, T* function) {
  PFN_xrVoidFunction pointer = nullptr;
  if (get_proc_ == nullptr ||
      !XR_SUCCEEDED(get_proc_(instance_, name, &pointer)) ||
      pointer == nullptr) {
    *function = nullptr;
    return false;
  }
  *function = reinterpret_cast<T>(pointer);
  return true;
}

XrResult OpenXrRuntime::CreateInstance(const std::vector<std::string>& optional,
                                       RuntimeInfo* info) {
  if (loader_ == nullptr) {
    // From its full path only: never a DLL of that name elsewhere on the
    // search path
    loader_ = ::LoadLibraryExW(loader_path_.c_str(), nullptr,
                               LOAD_WITH_ALTERED_SEARCH_PATH);
    if (loader_ == nullptr) {
      loader_error_ = "openxr_loader.dll could not be loaded (error " +
                      std::to_string(::GetLastError()) + ")";
      return XR_ERROR_INITIALIZATION_FAILED;
    }
    get_proc_ = reinterpret_cast<PFN_xrGetInstanceProcAddr>(
        ::GetProcAddress(loader_, "xrGetInstanceProcAddr"));
    if (get_proc_ == nullptr) {
      loader_error_ = "openxr_loader.dll has no xrGetInstanceProcAddr";
      return XR_ERROR_INITIALIZATION_FAILED;
    }
  }
  instance_ = XR_NULL_HANDLE;
  if (!Load("xrEnumerateInstanceExtensionProperties", &enumerate_extensions_) ||
      !Load("xrCreateInstance", &create_instance_)) {
    loader_error_ = "the loader's entry points";
    return XR_ERROR_INITIALIZATION_FAILED;
  }
  uint32_t count = 0;
  auto result = enumerate_extensions_(nullptr, 0, &count, nullptr);
  if (!XR_SUCCEEDED(result)) {
    return result;
  }
  std::vector<XrExtensionProperties> extensions(
      count, Typed<XrExtensionProperties>(XR_TYPE_EXTENSION_PROPERTIES));
  result = enumerate_extensions_(nullptr, count, &count, extensions.data());
  if (!XR_SUCCEEDED(result)) {
    return result;
  }
  info->offered.clear();
  for (uint32_t i = 0; i < count; i++) {
    info->offered.push_back(extensions[i].extensionName);
  }
  const auto offered = [info](const std::string& name) {
    for (const auto& known : info->offered) {
      if (known == name) return true;
    }
    return false;
  };
  if (!offered(kD3D11)) {
    loader_error_ = "the runtime has no XR_KHR_D3D11_enable";
    return XR_ERROR_EXTENSION_NOT_PRESENT;
  }
  std::vector<const char*> enabled = {kD3D11};
  info->enabled = {kD3D11};
  for (const auto& name : optional) {
    if (offered(name)) {
      enabled.push_back(name.c_str());
      info->enabled.push_back(name);
      refresh_rate_ = refresh_rate_ || name == kRefreshRate;
    }
  }
  auto create = Typed<XrInstanceCreateInfo>(XR_TYPE_INSTANCE_CREATE_INFO);
  CopyName(create.applicationInfo.applicationName,
           XR_MAX_APPLICATION_NAME_SIZE, "Immuch360");
  create.applicationInfo.applicationVersion = 1;
  CopyName(create.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE,
           "media_kit_360");
  create.applicationInfo.engineVersion = 1;
  // OpenXR 1.0: every PC runtime has it, not all have 1.1 yet
  create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  create.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
  create.enabledExtensionNames = enabled.data();
  result = create_instance_(&create, &instance_);
  if (!XR_SUCCEEDED(result)) {
    instance_ = XR_NULL_HANDLE;
    return result;
  }
  const bool loaded =
      Load("xrDestroyInstance", &destroy_instance_) &&
      Load("xrGetInstanceProperties", &instance_properties_) &&
      Load("xrResultToString", &result_to_string_) &&
      Load("xrGetSystem", &get_system_) &&
      Load("xrGetSystemProperties", &system_properties_) &&
      Load("xrEnumerateViewConfigurationViews", &view_configuration_views_) &&
      Load("xrEnumerateEnvironmentBlendModes", &blend_modes_) &&
      Load("xrGetD3D11GraphicsRequirementsKHR", &d3d11_requirements_) &&
      Load("xrCreateSession", &create_session_) &&
      Load("xrDestroySession", &destroy_session_) &&
      Load("xrCreateReferenceSpace", &create_space_) &&
      Load("xrDestroySpace", &destroy_space_) &&
      Load("xrEnumerateSwapchainFormats", &swapchain_formats_) &&
      Load("xrCreateSwapchain", &create_swapchain_) &&
      Load("xrDestroySwapchain", &destroy_swapchain_) &&
      Load("xrEnumerateSwapchainImages", &swapchain_images_) &&
      Load("xrPollEvent", &poll_event_) &&
      Load("xrBeginSession", &begin_session_) &&
      Load("xrEndSession", &end_session_) &&
      Load("xrRequestExitSession", &request_exit_) &&
      Load("xrWaitFrame", &wait_frame_) &&
      Load("xrBeginFrame", &begin_frame_) &&
      Load("xrEndFrame", &end_frame_) &&
      Load("xrLocateViews", &locate_views_) &&
      Load("xrAcquireSwapchainImage", &acquire_image_) &&
      Load("xrWaitSwapchainImage", &wait_image_) &&
      Load("xrReleaseSwapchainImage", &release_image_);
  if (!loaded) {
    loader_error_ = "the runtime lacks a core entry point";
    return XR_ERROR_INITIALIZATION_FAILED;
  }
  if (refresh_rate_) {
    refresh_rate_ =
        Load("xrEnumerateDisplayRefreshRatesFB", &refresh_rates_) &&
        Load("xrGetDisplayRefreshRateFB", &refresh_rate_now_) &&
        Load("xrRequestDisplayRefreshRateFB", &request_refresh_rate_);
  }
  auto properties = Typed<XrInstanceProperties>(XR_TYPE_INSTANCE_PROPERTIES);
  if (XR_SUCCEEDED(instance_properties_(instance_, &properties))) {
    info->name = properties.runtimeName;
    info->version = properties.runtimeVersion;
  }
  return XR_SUCCESS;
}

XrResult OpenXrRuntime::GetSystem(SystemInfo* info) {
  auto get = Typed<XrSystemGetInfo>(XR_TYPE_SYSTEM_GET_INFO);
  get.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
  auto result = get_system_(instance_, &get, &system_);
  if (!XR_SUCCEEDED(result)) {
    return result;
  }
  auto properties = Typed<XrSystemProperties>(XR_TYPE_SYSTEM_PROPERTIES);
  result = system_properties_(instance_, system_, &properties);
  if (!XR_SUCCEEDED(result)) {
    return result;
  }
  info->name = properties.systemName;
  info->vendor_id = properties.vendorId;
  info->max_swapchain_width = properties.graphicsProperties.maxSwapchainImageWidth;
  info->max_swapchain_height =
      properties.graphicsProperties.maxSwapchainImageHeight;
  info->max_layers = properties.graphicsProperties.maxLayerCount;
  info->orientation_tracking =
      properties.trackingProperties.orientationTracking == XR_TRUE;
  info->position_tracking =
      properties.trackingProperties.positionTracking == XR_TRUE;
  uint32_t count = 0;
  result = view_configuration_views_(instance_, system_,
                                     XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                     0, &count, nullptr);
  if (!XR_SUCCEEDED(result)) {
    return result;
  }
  std::vector<XrViewConfigurationView> views(
      count, Typed<XrViewConfigurationView>(XR_TYPE_VIEW_CONFIGURATION_VIEW));
  result = view_configuration_views_(instance_, system_,
                                     XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                     count, &count, views.data());
  if (!XR_SUCCEEDED(result) || count < 2) {
    return XR_SUCCEEDED(result) ? XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED
                                : result;
  }
  for (int eye = 0; eye < 2; eye++) {
    info->eye_width[eye] = views[eye].recommendedImageRectWidth;
    info->eye_height[eye] = views[eye].recommendedImageRectHeight;
    info->eye_max_width[eye] = views[eye].maxImageRectWidth;
    info->eye_max_height[eye] = views[eye].maxImageRectHeight;
  }
  auto requirements = Typed<XrGraphicsRequirementsD3D11KHR>(
      XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR);
  result = d3d11_requirements_(instance_, system_, &requirements);
  if (!XR_SUCCEEDED(result)) {
    return result;
  }
  info->adapter = {requirements.adapterLuid.LowPart,
                   requirements.adapterLuid.HighPart};
  info->min_feature_level = static_cast<int32_t>(requirements.minFeatureLevel);
  uint32_t blends = 0;
  info->blend_modes.clear();
  if (XR_SUCCEEDED(blend_modes_(instance_, system_,
                                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0,
                                &blends, nullptr)) &&
      blends > 0) {
    info->blend_modes.resize(blends);
    blend_modes_(instance_, system_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                 blends, &blends, info->blend_modes.data());
    blend_ = info->blend_modes[0];
    for (const auto mode : info->blend_modes) {
      if (mode == XR_ENVIRONMENT_BLEND_MODE_OPAQUE) {
        blend_ = mode;
      }
    }
  }
  return XR_SUCCESS;
}

XrResult OpenXrRuntime::CreateSession(void* device) {
  auto binding =
      Typed<XrGraphicsBindingD3D11KHR>(XR_TYPE_GRAPHICS_BINDING_D3D11_KHR);
  binding.device = static_cast<ID3D11Device*>(device);
  auto create = Typed<XrSessionCreateInfo>(XR_TYPE_SESSION_CREATE_INFO);
  create.next = &binding;
  create.systemId = system_;
  auto result = create_session_(instance_, &create, &session_);
  if (!XR_SUCCEEDED(result)) {
    session_ = XR_NULL_HANDLE;
    return result;
  }
  auto space =
      Typed<XrReferenceSpaceCreateInfo>(XR_TYPE_REFERENCE_SPACE_CREATE_INFO);
  space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
  space.poseInReferenceSpace.orientation.w = 1.0f;
  return create_space_(session_, &space, &local_);
}

XrResult OpenXrRuntime::SwapchainFormats(std::vector<int64_t>* formats) {
  uint32_t count = 0;
  auto result = swapchain_formats_(session_, 0, &count, nullptr);
  if (!XR_SUCCEEDED(result)) {
    return result;
  }
  formats->assign(count, 0);
  return swapchain_formats_(session_, count, &count, formats->data());
}

XrResult OpenXrRuntime::CreateSwapchain(int64_t format, uint32_t width,
                                        uint32_t height, Swapchain* out) {
  auto create = Typed<XrSwapchainCreateInfo>(XR_TYPE_SWAPCHAIN_CREATE_INFO);
  create.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                      XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
  create.format = format;
  create.sampleCount = 1;
  create.width = width;
  create.height = height;
  create.faceCount = 1;
  create.arraySize = 1;
  create.mipCount = 1;
  XrSwapchain swapchain = XR_NULL_HANDLE;
  auto result = create_swapchain_(session_, &create, &swapchain);
  if (!XR_SUCCEEDED(result)) {
    return result;
  }
  uint32_t count = 0;
  result = swapchain_images_(swapchain, 0, &count, nullptr);
  if (!XR_SUCCEEDED(result)) {
    destroy_swapchain_(swapchain);
    return result;
  }
  std::vector<XrSwapchainImageD3D11KHR> images(
      count,
      Typed<XrSwapchainImageD3D11KHR>(XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR));
  result = swapchain_images_(
      swapchain, count, &count,
      reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
  if (!XR_SUCCEEDED(result)) {
    destroy_swapchain_(swapchain);
    return result;
  }
  out->id = next_swapchain_++;
  out->format = format;
  out->width = width;
  out->height = height;
  out->images.clear();
  for (const auto& image : images) {
    out->images.push_back(image.texture);
  }
  swapchains_[out->id] = swapchain;
  return XR_SUCCESS;
}

XrSwapchain OpenXrRuntime::SwapchainOf(uint64_t id) const {
  const auto found = swapchains_.find(id);
  return found == swapchains_.end() ? XR_NULL_HANDLE : found->second;
}

void OpenXrRuntime::DestroySwapchain(uint64_t id) {
  const auto found = swapchains_.find(id);
  if (found != swapchains_.end()) {
    destroy_swapchain_(found->second);
    swapchains_.erase(found);
  }
}

bool OpenXrRuntime::PollEvent(Event* event) {
  auto buffer = Typed<XrEventDataBuffer>(XR_TYPE_EVENT_DATA_BUFFER);
  if (instance_ == XR_NULL_HANDLE ||
      poll_event_(instance_, &buffer) != XR_SUCCESS) {
    return false;
  }
  *event = Event{};
  switch (buffer.type) {
    case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
      const auto* changed =
          reinterpret_cast<const XrEventDataSessionStateChanged*>(&buffer);
      event->type = Event::Type::kSessionState;
      event->state = changed->state;
      break;
    }
    case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
      event->type = Event::Type::kInstanceLossPending;
      break;
    case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
      event->type = Event::Type::kReferenceSpaceChange;
      break;
    case XR_TYPE_EVENT_DATA_EVENTS_LOST: {
      const auto* lost =
          reinterpret_cast<const XrEventDataEventsLost*>(&buffer);
      event->type = Event::Type::kEventsLost;
      event->lost = lost->lostEventCount;
      break;
    }
    case XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB: {
      const auto* changed =
          reinterpret_cast<const XrEventDataDisplayRefreshRateChangedFB*>(
              &buffer);
      event->type = Event::Type::kRefreshRateChanged;
      event->from_rate = changed->fromDisplayRefreshRate;
      event->to_rate = changed->toDisplayRefreshRate;
      break;
    }
    default:
      event->type = Event::Type::kOther;
      event->other = buffer.type;
      break;
  }
  return true;
}

XrResult OpenXrRuntime::BeginSession() {
  auto begin = Typed<XrSessionBeginInfo>(XR_TYPE_SESSION_BEGIN_INFO);
  begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  return begin_session_(session_, &begin);
}

XrResult OpenXrRuntime::EndSession() { return end_session_(session_); }

XrResult OpenXrRuntime::RequestExitSession() { return request_exit_(session_); }

XrResult OpenXrRuntime::WaitFrame(FrameState* state) {
  auto wait = Typed<XrFrameWaitInfo>(XR_TYPE_FRAME_WAIT_INFO);
  auto frame = Typed<XrFrameState>(XR_TYPE_FRAME_STATE);
  const auto result = wait_frame_(session_, &wait, &frame);
  state->predicted_display_time = frame.predictedDisplayTime;
  state->predicted_display_period = frame.predictedDisplayPeriod;
  state->should_render = frame.shouldRender == XR_TRUE;
  return result;
}

XrResult OpenXrRuntime::BeginFrame() {
  auto begin = Typed<XrFrameBeginInfo>(XR_TYPE_FRAME_BEGIN_INFO);
  return begin_frame_(session_, &begin);
}

XrResult OpenXrRuntime::LocateViews(XrTime time, XrView views[2],
                                    bool* orientation_valid) {
  auto locate = Typed<XrViewLocateInfo>(XR_TYPE_VIEW_LOCATE_INFO);
  locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  locate.displayTime = time;
  locate.space = local_;
  auto state = Typed<XrViewState>(XR_TYPE_VIEW_STATE);
  uint32_t count = 0;
  for (int i = 0; i < 2; i++) {
    views[i].type = XR_TYPE_VIEW;
    views[i].next = nullptr;
  }
  const auto result = locate_views_(session_, &locate, &state, 2, &count, views);
  *orientation_valid = XR_SUCCEEDED(result) && count == 2 &&
                       (state.viewStateFlags &
                        XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
  return result;
}

XrResult OpenXrRuntime::AcquireImage(uint64_t swapchain, uint32_t* index) {
  auto acquire =
      Typed<XrSwapchainImageAcquireInfo>(XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO);
  return acquire_image_(SwapchainOf(swapchain), &acquire, index);
}

XrResult OpenXrRuntime::WaitImage(uint64_t swapchain) {
  auto wait = Typed<XrSwapchainImageWaitInfo>(XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO);
  wait.timeout = XR_INFINITE_DURATION;
  return wait_image_(SwapchainOf(swapchain), &wait);
}

XrResult OpenXrRuntime::ReleaseImage(uint64_t swapchain) {
  auto release =
      Typed<XrSwapchainImageReleaseInfo>(XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO);
  return release_image_(SwapchainOf(swapchain), &release);
}

XrResult OpenXrRuntime::EndFrame(XrTime time, const Layers& layers) {
  auto projection =
      Typed<XrCompositionLayerProjection>(XR_TYPE_COMPOSITION_LAYER_PROJECTION);
  XrCompositionLayerProjectionView views[2];
  std::vector<XrCompositionLayerQuad> quads;
  std::vector<const XrCompositionLayerBaseHeader*> list;
  if (layers.projection) {
    for (int eye = 0; eye < 2; eye++) {
      views[eye] = layers.views[eye];
      views[eye].subImage.swapchain = SwapchainOf(layers.view_swapchains[eye]);
    }
    projection.space = local_;
    projection.viewCount = 2;
    projection.views = views;
    list.push_back(
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection));
  }
  quads.reserve(layers.quads.size());
  for (const auto& quad : layers.quads) {
    auto layer = Typed<XrCompositionLayerQuad>(XR_TYPE_COMPOSITION_LAYER_QUAD);
    layer.space = local_;
    layer.eyeVisibility = quad.eye;
    layer.subImage.swapchain = SwapchainOf(quad.swapchain);
    layer.subImage.imageRect = quad.rect;
    layer.subImage.imageArrayIndex = 0;
    layer.pose = quad.pose;
    layer.size = quad.size;
    quads.push_back(layer);
  }
  for (const auto& quad : quads) {
    list.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad));
  }
  auto end = Typed<XrFrameEndInfo>(XR_TYPE_FRAME_END_INFO);
  end.displayTime = time;
  end.environmentBlendMode = blend_;
  end.layerCount = static_cast<uint32_t>(list.size());
  end.layers = list.empty() ? nullptr : list.data();
  return end_frame_(session_, &end);
}

XrResult OpenXrRuntime::RefreshRates(std::vector<float>* rates, float* current) {
  if (!refresh_rate_ || session_ == XR_NULL_HANDLE) {
    return XR_ERROR_FUNCTION_UNSUPPORTED;
  }
  uint32_t count = 0;
  auto result = refresh_rates_(session_, 0, &count, nullptr);
  if (!XR_SUCCEEDED(result)) {
    return result;
  }
  rates->assign(count, 0.0f);
  result = refresh_rates_(session_, count, &count, rates->data());
  if (!XR_SUCCEEDED(result)) {
    return result;
  }
  return refresh_rate_now_(session_, current);
}

XrResult OpenXrRuntime::RequestRefreshRate(float rate) {
  if (!refresh_rate_ || session_ == XR_NULL_HANDLE) {
    return XR_ERROR_FUNCTION_UNSUPPORTED;
  }
  return request_refresh_rate_(session_, rate);
}

void OpenXrRuntime::DestroySession() {
  for (auto& [id, swapchain] : swapchains_) {
    destroy_swapchain_(swapchain);
  }
  swapchains_.clear();
  if (local_ != XR_NULL_HANDLE) {
    destroy_space_(local_);
    local_ = XR_NULL_HANDLE;
  }
  if (session_ != XR_NULL_HANDLE) {
    destroy_session_(session_);
    session_ = XR_NULL_HANDLE;
  }
}

void OpenXrRuntime::DestroyInstance() {
  if (instance_ != XR_NULL_HANDLE) {
    destroy_instance_(instance_);
    instance_ = XR_NULL_HANDLE;
  }
}

std::string OpenXrRuntime::ResultName(XrResult result) {
  if (result == XR_ERROR_INITIALIZATION_FAILED && !loader_error_.empty()) {
    return loader_error_;
  }
  if (result == XR_ERROR_EXTENSION_NOT_PRESENT && !loader_error_.empty()) {
    return loader_error_;
  }
  char name[XR_MAX_RESULT_STRING_SIZE] = {0};
  if (instance_ != XR_NULL_HANDLE && result_to_string_ != nullptr &&
      XR_SUCCEEDED(result_to_string_(instance_, result, name))) {
    return name;
  }
  switch (result) {
    case XR_ERROR_RUNTIME_UNAVAILABLE:
      return "XR_ERROR_RUNTIME_UNAVAILABLE";
    case XR_ERROR_RUNTIME_FAILURE:
      return "XR_ERROR_RUNTIME_FAILURE";
    case XR_ERROR_INSTANCE_LOST:
      return "XR_ERROR_INSTANCE_LOST";
    case XR_ERROR_FORM_FACTOR_UNAVAILABLE:
      return "XR_ERROR_FORM_FACTOR_UNAVAILABLE";
    default:
      return "XrResult " + std::to_string(static_cast<int>(result));
  }
}

}  // namespace xr_probe
