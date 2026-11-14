// Immuch360: the headset probe (IMMUCH360-NOTE.md, patch 15), the GPU side on
// Windows: the Direct3D 11 device of the plugin's ANGLE display (EGL_EXT_device_query,
// EGL_D3D11_DEVICE_ANGLE), the one the players draw with, handed to the
// OpenXR runtime; a context of the probe's thread in the players' share
// group, so that the intermediate texture of a player (its decoded frame) is
// read where it is, with no copy; the swapchain images made drawable as ANGLE
// pbuffers (EGL_ANGLE_d3d_texture_client_buffer), in place when ANGLE takes
// the runtime's image through a linear view ("direct"), else through a
// texture of the probe copied into the image ("copy"); and each eye drawn by
// renderer C (ProjectionRenderer::DrawFrame) from the eye's pose and its four
// tangents.

#ifndef XR_ANGLE_GPU_H_
#define XR_ANGLE_GPU_H_

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <wrl.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "projection_renderer.h"
#include "xr_controller.h"
#include "xr_runtime.h"

namespace xr_probe {

class AngleGpu : public Gpu {
 public:
  // The frame of the player shown, false once it is gone
  using FrameSource = std::function<bool(ProjectionFrameSnapshot*)>;
  // A context of the players to share with, EGL_NO_CONTEXT when none
  using ShareContext = std::function<EGLContext(EGLDisplay)>;

  AngleGpu(Mode mode, ScreenStereo stereo, FrameSource source,
           ShareContext share);
  ~AngleGpu() override;

  bool Open(GpuInfo* info, std::string* error) override;
  bool WrapImages(const Swapchain& swapchain, std::string* path,
                  std::string* error) override;
  void UnwrapImages(uint64_t swapchain) override;
  bool DrawEye(const Swapchain& swapchain, uint32_t index, int eye,
               const XrView& view, double yaw_offset, double time,
               std::string* error) override;
  bool DrawScreen(const Swapchain& swapchain, uint32_t index,
                  std::string* error) override;
  void Finish() override;
  Content ContentNow() override;
  GpuMemory Memory() override;
  void Close() override;

 private:
  struct Wrapped {
    // One pbuffer per image ("direct"), or one over |copy| ("copy")
    std::vector<EGLSurface> surfaces;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> copy;
    EGLSurface copy_surface = EGL_NO_SURFACE;
    std::vector<ID3D11Texture2D*> images;
  };

  // The pbuffer over |texture|, trying the attributes of a linear view first;
  // EGL_NO_SURFACE with the EGL errors in |tried|
  EGLSurface Pbuffer(ID3D11Texture2D* texture, uint32_t width, uint32_t height,
                     bool bgra, std::string* tried);
  bool Current(const Swapchain& swapchain, uint32_t index,
               std::string* error);
  void Present(const Swapchain& swapchain, uint32_t index);
  bool PrepareBlit();

  Mode mode_;
  ScreenStereo stereo_;
  FrameSource source_;
  ShareContext share_;
  EGLDisplay display_ = EGL_NO_DISPLAY;
  EGLConfig config_ = nullptr;
  EGLContext context_ = EGL_NO_CONTEXT;
  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> immediate_;
  Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3_;
  ProjectionRenderer renderer_;
  GLuint blit_program_ = 0;
  GLuint blit_vertex_array_ = 0;
  std::map<uint64_t, Wrapped> wrapped_;
  bool waiting_logged_ = false;
};

}  // namespace xr_probe

#endif  // XR_ANGLE_GPU_H_
