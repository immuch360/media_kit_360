// Immuch360: the Windows tests of the headset view (IMMUCH360-NOTE.md, patch
// 15), on GitHub's hosted runner: no GPU (ANGLE's Direct3D 11 device is the
// Basic Render Driver) and no OpenXR runtime. Built by test/xr/CMakeLists.txt
// against the ANGLE and the OpenXR loader of a Windows build of the example
// with MEDIA_KIT_360_XR on, run from the folder of that build.
//
// What it proves without a headset:
// - the active runtime read from the registry with none registered, and with
//   XR_RUNTIME_JSON naming a missing file: not registered, the file name only;
// - the bundled loader opens from the app's folder, and with no runtime the
//   instance fails cleanly (XR_ERROR_RUNTIME_UNAVAILABLE or _FAILURE), the
//   whole view then ends with noRuntime and cleans up; every action call
//   then fails cleanly; a missing loader gives loaderMissing;
// - the plugin's ANGLE display gives its Direct3D 11 device and adapter LUID
//   (EGL_EXT_device_query), the view's context is made, renderer C's test
//   pattern compiles; the Basic Render Driver stops a view with gpuTooWeak;
// - swapchain images as runtimes make them (R8G8B8A8_TYPELESS, SteamVR's
//   D3D11 images; B8G8R8A8_TYPELESS; an image typed R8G8B8A8_UNORM_SRGB) are
//   made drawable, in place or through the copy, and what the eye pass writes
//   is what the image holds, byte for byte: grey step 8 of the ramp is 136,
//   neither encoded (192) nor decoded (59) on the way (the gamma path);
// - the strip's bytes reach its 1024 x 160 image unchanged, row for row.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d11.h>
#include <wrl.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "../../projection_view.h"
#include "../../xr_angle_gpu.h"
#include "../../xr_controller.h"
#include "../../xr_input.h"
#include "../../xr_openxr_runtime.h"
#include "../../xr_runtime_info.h"

namespace {

int failures = 0;

void Check(bool ok, const std::string& what) {
  std::printf("%s: %s\n", ok ? "ok" : "FAILED", what.c_str());
  if (!ok) failures++;
}

std::wstring Folder() {
  wchar_t path[MAX_PATH * 2] = {0};
  const auto length = ::GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
  std::wstring folder(path, length);
  return folder.substr(0, folder.find_last_of(L"\\/"));
}

struct Pixel {
  int r, g, b;
};

// The centre pixel of |image| (RGBA or BGRA bytes), read through a staging
// texture of the same format
Pixel Centre(ID3D11Device* device, ID3D11Texture2D* image, bool bgra) {
  D3D11_TEXTURE2D_DESC desc{};
  image->GetDesc(&desc);
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.MiscFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
  device->GetImmediateContext(&context);
  if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) {
    return {-1, -1, -1};
  }
  context->CopyResource(staging.Get(), image);
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
    return {-1, -1, -1};
  }
  const auto* row = static_cast<const uint8_t*>(mapped.pData) +
                    static_cast<size_t>(desc.Height / 2) * mapped.RowPitch;
  const auto* p = row + static_cast<size_t>(desc.Width / 2) * 4;
  Pixel pixel = bgra ? Pixel{p[2], p[1], p[0]} : Pixel{p[0], p[1], p[2]};
  context->Unmap(staging.Get(), 0);
  return pixel;
}

// Every byte of |image| (RGBA or BGRA), as RGBA rows of |width| x |height|
// from its top left corner
std::vector<uint8_t> Bytes(ID3D11Device* device, ID3D11Texture2D* image,
                           bool bgra, uint32_t width, uint32_t height) {
  D3D11_TEXTURE2D_DESC desc{};
  image->GetDesc(&desc);
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.MiscFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
  device->GetImmediateContext(&context);
  std::vector<uint8_t> out;
  if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) {
    return out;
  }
  context->CopyResource(staging.Get(), image);
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
    return out;
  }
  out.resize(static_cast<size_t>(width) * height * 4);
  for (uint32_t y = 0; y < height; y++) {
    const auto* row = static_cast<const uint8_t*>(mapped.pData) +
                      static_cast<size_t>(y) * mapped.RowPitch;
    for (uint32_t x = 0; x < width; x++) {
      const auto* p = row + static_cast<size_t>(x) * 4;
      auto* o = out.data() + (static_cast<size_t>(y) * width + x) * 4;
      o[0] = bgra ? p[2] : p[0];
      o[1] = p[1];
      o[2] = bgra ? p[0] : p[2];
      o[3] = p[3];
    }
  }
  context->Unmap(staging.Get(), 0);
  return out;
}

// An eye looking at (yaw, pitch) degrees, a narrow field of view
XrView Looking(double yaw, double pitch) {
  using projection_view::kDegree;
  const double y = yaw * kDegree / 2, p = pitch * kDegree / 2;
  // About +y by -yaw (to the right), then about +x by +pitch (up)
  const double qy[4] = {0, std::sin(-y), 0, std::cos(-y)};
  const double qx[4] = {std::sin(p), 0, 0, std::cos(p)};
  XrView view{};
  view.type = XR_TYPE_VIEW;
  view.pose.orientation.x = static_cast<float>(qy[3] * qx[0]);
  view.pose.orientation.y = static_cast<float>(qy[1] * qx[3]);
  view.pose.orientation.z = static_cast<float>(-qy[1] * qx[0]);
  view.pose.orientation.w = static_cast<float>(qy[3] * qx[3]);
  const float a = static_cast<float>(std::atan(0.02));
  view.fov = {-a, a, a, -a};
  return view;
}

// The eye's view as the controller gives it (upright, the anchor ahead)
ProjectionView ViewOf(const XrView& view) {
  ProjectionView out;
  projection_view::AnchoredEyeView(
      {view.pose.orientation.x, view.pose.orientation.y,
       view.pose.orientation.z, view.pose.orientation.w},
      view.fov.angleLeft, view.fov.angleRight, view.fov.angleUp,
      view.fov.angleDown, projection_view::AnchorAt(0, 0), 0.0, &out);
  out.sharp = false;
  return out;
}

xr_probe::ContentRequest Pattern() {
  xr_probe::ContentRequest content;
  content.mode = xr_probe::Mode::kPattern;
  return content;
}

void TestImages(const char* name, DXGI_FORMAT texture_format,
                int64_t swapchain_format) {
  xr_probe::AngleGpu gpu(nullptr, nullptr);
  gpu.SetContent(Pattern());
  xr_probe::GpuInfo info;
  std::string error;
  const bool open = gpu.Open(&info, &error);
  Check(open, std::string(name) + ": the ANGLE device and the probe's context " +
                  error);
  if (!open) return;
  auto* device = static_cast<ID3D11Device*>(info.device);
  std::printf("  adapter %s, vendor %x, device %x, LUID %08lx:%08lx, "
              "feature level %x, %s\n",
              info.adapter_name.c_str(), info.vendor_id, info.device_id,
              static_cast<unsigned long>(info.adapter.high),
              static_cast<unsigned long>(info.adapter.low),
              static_cast<unsigned>(info.feature_level),
              info.gl_renderer.c_str());
  Check(info.adapter.low != 0 || info.adapter.high != 0,
        std::string(name) + ": an adapter LUID");
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = 128;
  desc.Height = 128;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = texture_format;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> textures[3];
  xr_probe::Swapchain swapchain;
  swapchain.id = 1;
  swapchain.format = swapchain_format;
  swapchain.width = 128;
  swapchain.height = 128;
  for (auto& texture : textures) {
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture))) {
      Check(false, std::string(name) + ": the images");
      return;
    }
    swapchain.images.push_back(texture.Get());
  }
  std::string path;
  const bool wrapped = gpu.WrapImages(swapchain, &path, &error);
  std::printf("  %s\n", wrapped ? path.c_str() : error.c_str());
  Check(wrapped, std::string(name) + ": drawable");
  if (!wrapped) return;
  const bool bgra = xr_probe::IsBgra(swapchain_format);
  // Ahead on the horizon, 3 degrees right: step 8 of the ramp, 8 / 15
  Check(gpu.DrawEye(swapchain, 1, 0, ViewOf(Looking(3, 0)), 0.0, &error),
        std::string(name) + ": the eye drawn " + error);
  gpu.Finish();
  auto pixel = Centre(device, textures[1].Get(), bgra);
  std::printf("  ramp step 8: %d %d %d\n", pixel.r, pixel.g, pixel.b);
  Check(std::abs(pixel.r - 136) <= 1 && pixel.g == pixel.r &&
            pixel.b == pixel.r,
        std::string(name) + ": the value written is the value stored (136)");
  // The left eye's red patch, 24 degrees left and 16 up; the image ahead of
  // it (index 2) for the right eye has none
  Check(gpu.DrawEye(swapchain, 2, 0, ViewOf(Looking(-24, 16)), 0.0, &error),
        std::string(name) + ": the left eye");
  gpu.Finish();
  pixel = Centre(device, textures[2].Get(), bgra);
  Check(pixel.r == 255 && pixel.g == 0 && pixel.b == 0,
        std::string(name) + ": red in the left eye, as RGB (" +
            std::to_string(pixel.r) + " " + std::to_string(pixel.g) + " " +
            std::to_string(pixel.b) + ")");
  Check(gpu.DrawEye(swapchain, 0, 1, ViewOf(Looking(-24, 16)), 0.0, &error),
        std::string(name) + ": the right eye");
  gpu.Finish();
  pixel = Centre(device, textures[0].Get(), bgra);
  Check(!(pixel.r == 255 && pixel.g == 0 && pixel.b == 0),
        std::string(name) + ": no red patch in the right eye");
  gpu.UnwrapImages(swapchain.id);
  gpu.Close();
}

void TestStrip() {
  xr_probe::AngleGpu gpu(nullptr, nullptr);
  gpu.SetContent(Pattern());
  xr_probe::GpuInfo info;
  std::string error;
  if (!gpu.Open(&info, &error)) {
    Check(false, "strip: the ANGLE device " + error);
    return;
  }
  auto* device = static_cast<ID3D11Device*>(info.device);
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = xr_probe::kStripWidth;
  desc.Height = xr_probe::kStripHeight;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> textures[3];
  xr_probe::Swapchain swapchain;
  swapchain.id = 9;
  swapchain.format = xr_probe::kDxgiR8G8B8A8UnormSrgb;
  swapchain.width = desc.Width;
  swapchain.height = desc.Height;
  for (auto& texture : textures) {
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture))) {
      Check(false, "strip: the images");
      return;
    }
    swapchain.images.push_back(texture.Get());
  }
  std::string path;
  Check(gpu.WrapImages(swapchain, &path, &error), "strip: drawable " + error);
  std::printf("  strip: %s\n", path.c_str());
  // Every byte distinct enough to tell a row or a column off, alpha opaque
  const auto made = [](uint32_t width, uint32_t height) {
    std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
    for (uint32_t y = 0; y < height; y++) {
      for (uint32_t x = 0; x < width; x++) {
        auto* p = rgba.data() + (static_cast<size_t>(y) * width + x) * 4;
        p[0] = static_cast<uint8_t>(x & 255);
        p[1] = static_cast<uint8_t>(y);
        p[2] = static_cast<uint8_t>((x * 7 + y * 3) & 255);
        p[3] = 255;
      }
    }
    return rgba;
  };
  const auto full = made(xr_probe::kStripWidth, xr_probe::kStripHeight);
  Check(gpu.UploadStrip(swapchain, 1, full.data(), xr_probe::kStripWidth,
                        xr_probe::kStripHeight, &error),
        "strip: uploaded " + error);
  gpu.Finish();
  const auto back = Bytes(device, textures[1].Get(), false,
                          xr_probe::kStripWidth, xr_probe::kStripHeight);
  size_t differ = 0;
  size_t first = back.size();
  for (size_t i = 0; i < back.size() && i < full.size(); i++) {
    if (back[i] != full[i]) {
      differ++;
      if (first == back.size()) first = i;
    }
  }
  if (differ > 0 && first < back.size()) {
    const auto pixel = first / 4;
    std::printf("  first difference at x %zu, y %zu: %d %d %d %d, written "
                "%d %d %d %d\n",
                pixel % xr_probe::kStripWidth, pixel / xr_probe::kStripWidth,
                back[pixel * 4], back[pixel * 4 + 1], back[pixel * 4 + 2],
                back[pixel * 4 + 3], full[pixel * 4], full[pixel * 4 + 1],
                full[pixel * 4 + 2], full[pixel * 4 + 3]);
  }
  Check(back.size() == full.size() && differ == 0,
        "strip: 1024 x 160 bytes read back unchanged, the first row on top (" +
            std::to_string(differ) + " differ)");
  // A smaller image lands in the top left corner
  const auto part = made(600, 100);
  Check(gpu.UploadStrip(swapchain, 2, part.data(), 600, 100, &error),
        "strip: a smaller image " + error);
  gpu.Finish();
  const auto corner = Bytes(device, textures[2].Get(), false, 600, 100);
  Check(corner == part, "strip: the smaller image in the top left corner");
  Check(!gpu.UploadStrip(swapchain, 0, full.data(), 2048, 160, &error),
        "strip: a too wide image refused");
  gpu.UnwrapImages(swapchain.id);
  gpu.Close();
}

// A GPU side that is not the Basic Render Driver, for the view with the real
// loader and no runtime
class StubGpu : public xr_probe::Gpu {
 public:
  bool Open(xr_probe::GpuInfo* info, std::string*) override {
    info->adapter_name = "stub";
    info->vendor_id = 0x10de;
    return true;
  }
  void SetContent(const xr_probe::ContentRequest&) override {}
  bool WrapImages(const xr_probe::Swapchain&, std::string*,
                  std::string*) override {
    return false;
  }
  void UnwrapImages(uint64_t) override {}
  bool DrawEye(const xr_probe::Swapchain&, uint32_t, int,
               const ProjectionView&, double, std::string*) override {
    return false;
  }
  bool DrawScreen(const xr_probe::Swapchain&, uint32_t,
                  std::string*) override {
    return false;
  }
  bool UploadStrip(const xr_probe::Swapchain&, uint32_t, const uint8_t*,
                   uint32_t, uint32_t, std::string*) override {
    return false;
  }
  void Finish() override {}
  xr_probe::FrameInfo FrameNow() override { return {}; }
  xr_probe::GpuMemory Memory() override { return {}; }
  void Close() override {}
};

xr_probe::Status RunView(xr_probe::Runtime* runtime, xr_probe::Gpu* gpu) {
  xr_probe::Options options;
  options.content = Pattern();
  options.log = [](const std::string& line) {
    std::printf("  log: %s\n", line.c_str());
  };
  xr_probe::Controller controller(runtime, gpu, options);
  controller.Run();
  return controller.StatusNow();
}

void TestRuntimeInfo() {
  ::SetEnvironmentVariableW(L"XR_RUNTIME_JSON", nullptr);
  auto info = xr_session::ActiveRuntime();
  std::printf("  active runtime: %s, %s, %s\n", info.source.c_str(),
              info.manifest_name.c_str(), info.exists ? "present" : "absent");
  Check(!info.registered(), "runtime info: none registered on the runner");
  ::SetEnvironmentVariableW(
      L"XR_RUNTIME_JSON",
      L"C:\\no\\such\\folder\\Steam\\steamxr_win64.json");
  info = xr_session::ActiveRuntime();
  Check(info.source == "environment" && !info.exists && !info.registered(),
        "runtime info: XR_RUNTIME_JSON naming a missing file is not "
        "registered");
  Check(info.manifest_name == "steamxr_win64.json",
        "runtime info: the file name only (" + info.manifest_name + ")");
  Check(info.runtime == "SteamVR", "runtime info: guessed " + info.runtime);
  ::SetEnvironmentVariableW(L"XR_RUNTIME_JSON", nullptr);
}

void TestActionsWithoutRuntime(const std::wstring& folder) {
  xr_probe::OpenXrRuntime runtime(folder + L"\\openxr_loader.dll");
  xr_probe::RuntimeInfo info;
  runtime.CreateInstance({}, &info);
  XrPath path = XR_NULL_PATH;
  uint64_t id = 0;
  xr_probe::BoolState b;
  xr_probe::FloatState f;
  xr_probe::Vector2State v;
  std::string profile;
  const bool all_fail =
      !XR_SUCCEEDED(runtime.StringToPath("/user/hand/left", &path)) &&
      !XR_SUCCEEDED(runtime.CreateActionSet("player", "Video player", &id)) &&
      !XR_SUCCEEDED(runtime.CreateAction(1, {}, &id)) &&
      !XR_SUCCEEDED(runtime.SuggestBindings(1, {})) &&
      !XR_SUCCEEDED(runtime.AttachActionSets(1)) &&
      !XR_SUCCEEDED(runtime.SyncActions(1)) &&
      !XR_SUCCEEDED(runtime.GetBoolean(1, 1, &b)) &&
      !XR_SUCCEEDED(runtime.GetFloat(1, 1, &f)) &&
      !XR_SUCCEEDED(runtime.GetVector2(1, 1, &v)) &&
      !XR_SUCCEEDED(runtime.ApplyHaptic(1, 1, 0.5f, 1000000)) &&
      !XR_SUCCEEDED(runtime.CurrentProfile(1, &profile));
  Check(all_fail, "every action call fails cleanly with no runtime");
  xr_probe::Input input(&runtime, [](const std::string& line) {
    std::printf("  input: %s\n", line.c_str());
  });
  Check(!input.Setup({}, {}) && !input.ready(),
        "the controllers' setup gives up cleanly");
}

}  // namespace

int main() {
  const auto folder = Folder();
  TestRuntimeInfo();
  // The bundled loader, no runtime on the runner
  {
    xr_probe::OpenXrRuntime runtime(folder + L"\\openxr_loader.dll");
    xr_probe::RuntimeInfo info;
    const auto result =
        runtime.CreateInstance({"XR_FB_display_refresh_rate"}, &info);
    std::printf("  xrCreateInstance: %s, API layers %zu\n",
                runtime.ResultName(result).c_str(), info.api_layers.size());
    Check(result == XR_ERROR_RUNTIME_UNAVAILABLE ||
              result == XR_ERROR_RUNTIME_FAILURE ||
              result == XR_ERROR_INSTANCE_LOST,
          "the loader opens and says there is no runtime");
    Check(!runtime.LoaderMissing(), "the loader is there");
  }
  {
    xr_probe::OpenXrRuntime runtime(folder + L"\\no_such_loader.dll");
    xr_probe::RuntimeInfo info;
    const auto result = runtime.CreateInstance({}, &info);
    Check(result == XR_ERROR_INITIALIZATION_FAILED &&
              runtime.LoaderMissing() &&
              runtime.ResultName(result).find("could not be loaded") !=
                  std::string::npos,
          "a missing loader: " + runtime.ResultName(result));
  }
  {
    xr_probe::OpenXrRuntime runtime(folder + L"\\no_such_loader.dll");
    StubGpu gpu;
    const auto status = RunView(&runtime, &gpu);
    Check(status.reason == xr_probe::StopReason::kLoaderMissing,
          std::string("a missing loader stops the view with loaderMissing: ") +
              xr_probe::StopReasonCode(status.reason));
  }
  TestActionsWithoutRuntime(folder);
  // The whole view with the real loader, a GPU side that is not WARP: no
  // runtime
  {
    xr_probe::OpenXrRuntime runtime(folder + L"\\openxr_loader.dll");
    StubGpu gpu;
    const auto status = RunView(&runtime, &gpu);
    Check(status.phase == "stopped" &&
              status.reason == xr_probe::StopReason::kNoRuntime &&
              status.detail.rfind("no OpenXR runtime", 0) == 0,
          "the view without a runtime: noRuntime, " + status.detail);
  }
  // The whole view with the real GPU side: the runner's Basic Render Driver
  {
    xr_probe::OpenXrRuntime runtime(folder + L"\\openxr_loader.dll");
    xr_probe::AngleGpu gpu(nullptr, nullptr);
    const auto status = RunView(&runtime, &gpu);
    Check(status.phase == "stopped" &&
              status.reason == xr_probe::StopReason::kGpuTooWeak,
          std::string("the runner's WARP device: gpuTooWeak, ") +
              xr_probe::StopReasonCode(status.reason) + ", " + status.detail);
  }
  TestImages("R8G8B8A8_TYPELESS (SteamVR's images)",
             DXGI_FORMAT_R8G8B8A8_TYPELESS, xr_probe::kDxgiR8G8B8A8UnormSrgb);
  TestImages("B8G8R8A8_TYPELESS", DXGI_FORMAT_B8G8R8A8_TYPELESS,
             xr_probe::kDxgiB8G8R8A8UnormSrgb);
  TestImages("R8G8B8A8_UNORM_SRGB typed image", DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
             xr_probe::kDxgiR8G8B8A8UnormSrgb);
  TestStrip();
  std::printf("%d failed\n", failures);
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
