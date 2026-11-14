// Immuch360: the Windows tests of the headset probe (IMMUCH360-NOTE.md, patch
// 15), on GitHub's hosted runner: no GPU (ANGLE's Direct3D 11 device is the
// Basic Render Driver) and no OpenXR runtime. Built by test/xr/CMakeLists.txt
// against the ANGLE and the OpenXR loader of a Windows build of the example
// with MEDIA_KIT_360_XR on, run from the folder of that build.
//
// What it proves without a headset:
// - the bundled loader opens from the app's folder, and with no runtime the
//   instance fails cleanly (XR_ERROR_RUNTIME_UNAVAILABLE or _FAILURE), the
//   whole probe then ends with "no OpenXR runtime" and cleans up;
// - the plugin's ANGLE display gives its Direct3D 11 device and adapter LUID
//   (EGL_EXT_device_query), the probe's context is made, renderer C's test
//   pattern compiles;
// - swapchain images as runtimes make them (R8G8B8A8_TYPELESS, SteamVR's
//   D3D11 images; B8G8R8A8_TYPELESS; an image typed R8G8B8A8_UNORM_SRGB) are
//   made drawable, in place or through the copy, and what the eye pass writes
//   is what the image holds, byte for byte: grey step 8 of the ramp is 136,
//   neither encoded (192) nor decoded (59) on the way (the gamma path).

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

#include "../../projection_view.h"
#include "../../xr_angle_gpu.h"
#include "../../xr_controller.h"
#include "../../xr_openxr_runtime.h"

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

void TestImages(const char* name, DXGI_FORMAT texture_format,
                int64_t swapchain_format) {
  xr_probe::AngleGpu gpu(xr_probe::Mode::kPattern,
                         xr_probe::ScreenStereo::kMono, nullptr, nullptr);
  xr_probe::GpuInfo info;
  std::string error;
  const bool open = gpu.Open(&info, &error);
  Check(open, std::string(name) + ": the ANGLE device and the probe's context " +
                  error);
  if (!open) return;
  auto* device = static_cast<ID3D11Device*>(info.device);
  std::printf("  adapter %s, LUID %08lx:%08lx, feature level %x, %s\n",
              info.adapter_name.c_str(),
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
  Check(gpu.DrawEye(swapchain, 1, 0, Looking(3, 0), 0.0, 0.0, &error),
        std::string(name) + ": the eye drawn " + error);
  gpu.Finish();
  auto pixel = Centre(device, textures[1].Get(), bgra);
  std::printf("  ramp step 8: %d %d %d\n", pixel.r, pixel.g, pixel.b);
  Check(std::abs(pixel.r - 136) <= 1 && pixel.g == pixel.r &&
            pixel.b == pixel.r,
        std::string(name) + ": the value written is the value stored (136)");
  // The left eye's red patch, 24 degrees left and 16 up; the image ahead of
  // it (index 2) for the right eye has none
  Check(gpu.DrawEye(swapchain, 2, 0, Looking(-24, 16), 0.0, 0.0, &error),
        std::string(name) + ": the left eye");
  gpu.Finish();
  pixel = Centre(device, textures[2].Get(), bgra);
  Check(pixel.r == 255 && pixel.g == 0 && pixel.b == 0,
        std::string(name) + ": red in the left eye, as RGB (" +
            std::to_string(pixel.r) + " " + std::to_string(pixel.g) + " " +
            std::to_string(pixel.b) + ")");
  Check(gpu.DrawEye(swapchain, 0, 1, Looking(-24, 16), 0.0, 0.0, &error),
        std::string(name) + ": the right eye");
  gpu.Finish();
  pixel = Centre(device, textures[0].Get(), bgra);
  Check(!(pixel.r == 255 && pixel.g == 0 && pixel.b == 0),
        std::string(name) + ": no red patch in the right eye");
  gpu.UnwrapImages(swapchain.id);
  gpu.Close();
}

}  // namespace

int main() {
  const auto folder = Folder();
  // The bundled loader, no runtime on the runner
  {
    xr_probe::OpenXrRuntime runtime(folder + L"\\openxr_loader.dll");
    xr_probe::RuntimeInfo info;
    const auto result =
        runtime.CreateInstance({"XR_FB_display_refresh_rate"}, &info);
    std::printf("  xrCreateInstance: %s\n", runtime.ResultName(result).c_str());
    Check(result == XR_ERROR_RUNTIME_UNAVAILABLE ||
              result == XR_ERROR_RUNTIME_FAILURE ||
              result == XR_ERROR_INSTANCE_LOST,
          "the loader opens and says there is no runtime");
  }
  {
    xr_probe::OpenXrRuntime runtime(folder + L"\\no_such_loader.dll");
    xr_probe::RuntimeInfo info;
    const auto result = runtime.CreateInstance({}, &info);
    Check(result == XR_ERROR_INITIALIZATION_FAILED &&
              runtime.ResultName(result).find("could not be loaded") !=
                  std::string::npos,
          "a missing loader: " + runtime.ResultName(result));
  }
  // The whole probe with the real loader and the real GPU side
  {
    xr_probe::OpenXrRuntime runtime(folder + L"\\openxr_loader.dll");
    xr_probe::AngleGpu gpu(xr_probe::Mode::kPattern,
                           xr_probe::ScreenStereo::kMono, nullptr, nullptr);
    xr_probe::Options options;
    options.log = [](const std::string& line) {
      std::printf("  log: %s\n", line.c_str());
    };
    xr_probe::Controller controller(&runtime, &gpu, options);
    controller.Run();
    const auto status = controller.StatusNow();
    Check(status.phase == "stopped" &&
              status.reason.rfind("no OpenXR runtime", 0) == 0,
          "the probe without a runtime: " + status.reason);
  }
  TestImages("R8G8B8A8_TYPELESS (SteamVR's images)",
             DXGI_FORMAT_R8G8B8A8_TYPELESS, xr_probe::kDxgiR8G8B8A8UnormSrgb);
  TestImages("B8G8R8A8_TYPELESS", DXGI_FORMAT_B8G8R8A8_TYPELESS,
             xr_probe::kDxgiB8G8R8A8UnormSrgb);
  TestImages("R8G8B8A8_UNORM_SRGB typed image", DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
             xr_probe::kDxgiR8G8B8A8UnormSrgb);
  std::printf("%d failed\n", failures);
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
