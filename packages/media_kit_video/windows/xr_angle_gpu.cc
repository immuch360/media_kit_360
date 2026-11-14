// Immuch360: the headset probe's GPU side (IMMUCH360-NOTE.md, patch 15); see
// the header.

#include "xr_angle_gpu.h"

#include <d3d10.h>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <sstream>

#include "projection_shaders.h"
#include "projection_view.h"

#ifndef EGL_D3D_TEXTURE_ANGLE
#define EGL_D3D_TEXTURE_ANGLE 0x33A3
#endif
#ifndef EGL_TEXTURE_INTERNAL_FORMAT_ANGLE
#define EGL_TEXTURE_INTERNAL_FORMAT_ANGLE 0x345D
#endif
#ifndef EGL_D3D11_DEVICE_ANGLE
#define EGL_D3D11_DEVICE_ANGLE 0x33A1
#endif
#ifndef EGL_DEVICE_EXT
#define EGL_DEVICE_EXT 0x322C
#endif
#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1
#endif

namespace xr_probe {

namespace {

// The display the players use (ANGLESurfaceManager's first choice): ANGLE
// gives the same EGLDisplay for the same attributes, so the probe's context
// can join the players' share group
constexpr EGLint kDisplayAttributes[] = {
    EGL_PLATFORM_ANGLE_TYPE_ANGLE,
    EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE,
    EGL_PLATFORM_ANGLE_ENABLE_AUTOMATIC_TRIM_ANGLE,
    EGL_TRUE,
    EGL_NONE,
};

constexpr EGLint kConfigAttributes[] = {
    EGL_RED_SIZE,        8, EGL_GREEN_SIZE,   8, EGL_BLUE_SIZE, 8,
    EGL_ALPHA_SIZE,      8, EGL_DEPTH_SIZE,   0, EGL_SURFACE_TYPE,
    EGL_PBUFFER_BIT,        EGL_RENDERABLE_TYPE, 0x00000040 /* ES3 */,
    EGL_NONE,
};

constexpr EGLint kContextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3,
                                         EGL_NONE};

constexpr const char* kBlitFragment = R"glsl(#version 300 es
precision highp float;
uniform sampler2D uSource;
in vec2 vPosition;
out vec4 outColor;
void main() {
  // The first row of the frame and of the output are both the top
  outColor = vec4(texture(uSource, vPosition * 0.5 + 0.5).rgb, 1.0);
}
)glsl";

std::string Hex(uint64_t value) {
  std::ostringstream out;
  out << "0x" << std::hex << value;
  return out.str();
}

std::string Narrow(const wchar_t* text) {
  std::string out;
  for (; text != nullptr && *text != 0; text++) {
    out += *text < 128 ? static_cast<char>(*text) : '?';
  }
  return out;
}

bool HasExtension(const char* list, const char* name) {
  if (list == nullptr) return false;
  const std::string all = std::string(" ") + list + " ";
  return all.find(std::string(" ") + name + " ") != std::string::npos;
}

using GenVertexArrays = void(GL_APIENTRY*)(GLsizei, GLuint*);
using BindVertexArray = void(GL_APIENTRY*)(GLuint);
using DeleteVertexArrays = void(GL_APIENTRY*)(GLsizei, const GLuint*);

}  // namespace

AngleGpu::AngleGpu(Mode mode, ScreenStereo stereo, FrameSource source,
                   ShareContext share)
    : mode_(mode),
      stereo_(stereo),
      source_(std::move(source)),
      share_(std::move(share)) {}

AngleGpu::~AngleGpu() { Close(); }

bool AngleGpu::Open(GpuInfo* info, std::string* error) {
  auto get_platform_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
      eglGetProcAddress("eglGetPlatformDisplayEXT"));
  if (get_platform_display == nullptr) {
    *error = "no eglGetPlatformDisplayEXT";
    return false;
  }
  display_ = get_platform_display(EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY,
                                  kDisplayAttributes);
  if (display_ == EGL_NO_DISPLAY || !eglInitialize(display_, nullptr, nullptr)) {
    *error = "the ANGLE Direct3D 11 display (EGL " + Hex(eglGetError()) + ")";
    display_ = EGL_NO_DISPLAY;
    return false;
  }
  // EGL_EXT_device_query: this ANGLE lists it among the client extensions
  // (IMMUCH360-NOTE.md, patch 1)
  const char* client = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
  const char* own = eglQueryString(display_, EGL_EXTENSIONS);
  if (!HasExtension(client, "EGL_EXT_device_query") &&
      !HasExtension(own, "EGL_EXT_device_query")) {
    *error = "ANGLE has no EGL_EXT_device_query";
    return false;
  }
  auto query_display = reinterpret_cast<PFNEGLQUERYDISPLAYATTRIBEXTPROC>(
      eglGetProcAddress("eglQueryDisplayAttribEXT"));
  auto query_device = reinterpret_cast<PFNEGLQUERYDEVICEATTRIBEXTPROC>(
      eglGetProcAddress("eglQueryDeviceAttribEXT"));
  EGLAttrib device = 0;
  EGLAttrib d3d11 = 0;
  if (query_display == nullptr || query_device == nullptr ||
      !query_display(display_, EGL_DEVICE_EXT, &device) || device == 0 ||
      !query_device(reinterpret_cast<EGLDeviceEXT>(device),
                    EGL_D3D11_DEVICE_ANGLE, &d3d11) ||
      d3d11 == 0) {
    *error = "the Direct3D 11 device of ANGLE (EGL " + Hex(eglGetError()) + ")";
    return false;
  }
  device_ = reinterpret_cast<ID3D11Device*>(d3d11);
  device_->GetImmediateContext(&immediate_);
  info->device = device_.Get();
  info->feature_level = static_cast<int32_t>(device_->GetFeatureLevel());
  Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
  Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
  DXGI_ADAPTER_DESC desc{};
  if (SUCCEEDED(device_.As(&dxgi_device)) &&
      SUCCEEDED(dxgi_device->GetAdapter(&adapter)) &&
      SUCCEEDED(adapter->GetDesc(&desc))) {
    info->adapter = {desc.AdapterLuid.LowPart, desc.AdapterLuid.HighPart};
    info->adapter_name = Narrow(desc.Description);
    info->dedicated_video_memory = desc.DedicatedVideoMemory;
    adapter.As(&adapter3_);
  }
  // The runtime calls into this device from the probe's thread (xrEndFrame,
  // the swapchain images) while the players draw from theirs: the immediate
  // context must take its own lock (mpv's d3d11va turns it on as well)
  Microsoft::WRL::ComPtr<ID3D10Multithread> multithread;
  if (SUCCEEDED(device_.As(&multithread))) {
    info->was_multithread_protected =
        multithread->GetMultithreadProtected() == TRUE;
    multithread->SetMultithreadProtected(TRUE);
  }
  EGLint count = 0;
  if (!eglChooseConfig(display_, kConfigAttributes, &config_, 1, &count) ||
      count == 0) {
    *error = "no ES 3.0 pbuffer configuration";
    return false;
  }
  const EGLContext share = share_ ? share_(display_) : EGL_NO_CONTEXT;
  context_ = eglCreateContext(display_, config_, share, kContextAttributes);
  info->shares_players = context_ != EGL_NO_CONTEXT && share != EGL_NO_CONTEXT;
  if (context_ == EGL_NO_CONTEXT && share != EGL_NO_CONTEXT) {
    std::cout << "media_kit: XR: the players' share group refused the probe's "
                 "context (EGL "
              << Hex(eglGetError()) << ")" << std::endl;
    context_ =
        eglCreateContext(display_, config_, EGL_NO_CONTEXT, kContextAttributes);
  }
  if (context_ == EGL_NO_CONTEXT) {
    *error = "the probe's ES 3.0 context (EGL " + Hex(eglGetError()) + ")";
    return false;
  }
  if (!eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, context_)) {
    *error = "the probe's context current (EGL " + Hex(eglGetError()) + ")";
    return false;
  }
  if (!renderer_.Prepare(3)) {
    *error = "renderer C: " + renderer_.error();
    return false;
  }
  info->gl_renderer = renderer_.gl_renderer();
  // The passes compiled now, not at the first frame of the headset
  if (mode_ == Mode::kPattern && !renderer_.PrepareProgram(3)) {
    *error = "the test pattern: " + renderer_.error();
    return false;
  }
  if (mode_ == Mode::kScreen && !PrepareBlit()) {
    *error = "the screen pass";
    return false;
  }
  return true;
}

bool AngleGpu::PrepareBlit() {
  const auto compile = [](GLenum type, const char* source) {
    const auto shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
      glDeleteShader(shader);
      return 0u;
    }
    return shader;
  };
  const auto vertex =
      compile(GL_VERTEX_SHADER, projection_shaders::kVertexSource);
  const auto fragment = compile(GL_FRAGMENT_SHADER, kBlitFragment);
  if (vertex == 0 || fragment == 0) {
    return false;
  }
  blit_program_ = glCreateProgram();
  glAttachShader(blit_program_, vertex);
  glAttachShader(blit_program_, fragment);
  glLinkProgram(blit_program_);
  glDeleteShader(vertex);
  glDeleteShader(fragment);
  GLint ok = GL_FALSE;
  glGetProgramiv(blit_program_, GL_LINK_STATUS, &ok);
  auto gen = reinterpret_cast<GenVertexArrays>(
      eglGetProcAddress("glGenVertexArrays"));
  if (ok != GL_TRUE || gen == nullptr) {
    return false;
  }
  gen(1, &blit_vertex_array_);
  return true;
}

EGLSurface AngleGpu::Pbuffer(ID3D11Texture2D* texture, uint32_t width,
                             uint32_t height, bool bgra, std::string* tried) {
  const EGLint w = static_cast<EGLint>(width);
  const EGLint h = static_cast<EGLint>(height);
  const EGLint format = bgra ? GL_BGRA_EXT : GL_RGBA;
  // A TYPELESS image needs its format named; the colour space linear, so that
  // the values written are those stored (the runtime decodes the sRGB image)
  const EGLint linear_named[] = {EGL_WIDTH,
                                 w,
                                 EGL_HEIGHT,
                                 h,
                                 EGL_TEXTURE_INTERNAL_FORMAT_ANGLE,
                                 format,
                                 EGL_GL_COLORSPACE,
                                 EGL_GL_COLORSPACE_LINEAR,
                                 EGL_NONE};
  const EGLint named[] = {EGL_WIDTH,  w, EGL_HEIGHT, h,
                          EGL_TEXTURE_INTERNAL_FORMAT_ANGLE, format, EGL_NONE};
  const EGLint plain[] = {EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE};
  const EGLint* attempts[] = {linear_named, named, plain};
  const char* names[] = {"internal format and linear colour space",
                         "internal format", "size only"};
  for (int i = 0; i < 3; i++) {
    const auto surface = eglCreatePbufferFromClientBuffer(
        display_, EGL_D3D_TEXTURE_ANGLE,
        reinterpret_cast<EGLClientBuffer>(texture), config_, attempts[i]);
    if (surface != EGL_NO_SURFACE) {
      *tried += std::string(tried->empty() ? "" : "; ") + names[i] + ": ok";
      return surface;
    }
    *tried += std::string(tried->empty() ? "" : "; ") + names[i] + ": EGL " +
              Hex(eglGetError());
  }
  return EGL_NO_SURFACE;
}

bool AngleGpu::WrapImages(const Swapchain& swapchain, std::string* path,
                          std::string* error) {
  if (swapchain.images.empty()) {
    *error = "no images";
    return false;
  }
  Wrapped wrapped;
  for (auto* image : swapchain.images) {
    wrapped.images.push_back(static_cast<ID3D11Texture2D*>(image));
  }
  D3D11_TEXTURE2D_DESC desc{};
  wrapped.images[0]->GetDesc(&desc);
  const bool bgra = IsBgra(swapchain.format);
  std::string described = "image " + DxgiFormatName(desc.Format) + " " +
                          std::to_string(desc.Width) + " x " +
                          std::to_string(desc.Height) + ", bind " +
                          Hex(desc.BindFlags) + ", misc " + Hex(desc.MiscFlags) +
                          ", array " + std::to_string(desc.ArraySize);
  // In place only when the image is typeless or plain UNORM: an image made
  // with an sRGB format would be written through an sRGB view, the values
  // encoded twice (too bright)
  const bool typeless = desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
                        desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
                        desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                        desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
  std::string tried;
  if (typeless) {
    for (auto* image : wrapped.images) {
      const auto surface =
          Pbuffer(image, swapchain.width, swapchain.height, bgra, &tried);
      if (surface == EGL_NO_SURFACE) {
        for (auto made : wrapped.surfaces) eglDestroySurface(display_, made);
        wrapped.surfaces.clear();
        break;
      }
      wrapped.surfaces.push_back(surface);
    }
  } else {
    tried = "not typeless";
  }
  if (!wrapped.surfaces.empty()) {
    *path = "direct (" + described + "; " + tried + ")";
    wrapped_[swapchain.id] = std::move(wrapped);
    return true;
  }
  // The copy: a texture of the image's family, plain UNORM, drawn and copied
  // into the image as raw bytes
  D3D11_TEXTURE2D_DESC own{};
  own.Width = swapchain.width;
  own.Height = swapchain.height;
  own.MipLevels = 1;
  own.ArraySize = 1;
  own.Format = bgra ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
  own.SampleDesc.Count = 1;
  own.Usage = D3D11_USAGE_DEFAULT;
  own.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  if (FAILED(device_->CreateTexture2D(&own, nullptr, &wrapped.copy))) {
    *error = "the copy texture; " + described + "; " + tried;
    return false;
  }
  std::string copy_tried;
  wrapped.copy_surface = Pbuffer(wrapped.copy.Get(), swapchain.width,
                                 swapchain.height, bgra, &copy_tried);
  if (wrapped.copy_surface == EGL_NO_SURFACE) {
    *error = "the copy texture's pbuffer (" + copy_tried + "); " + described;
    return false;
  }
  *path = "copy (" + described + "; in place: " + tried + "; copy texture: " +
          copy_tried + ")";
  wrapped_[swapchain.id] = std::move(wrapped);
  return true;
}

void AngleGpu::UnwrapImages(uint64_t swapchain) {
  const auto found = wrapped_.find(swapchain);
  if (found == wrapped_.end()) {
    return;
  }
  eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, context_);
  for (auto surface : found->second.surfaces) {
    eglDestroySurface(display_, surface);
  }
  if (found->second.copy_surface != EGL_NO_SURFACE) {
    eglDestroySurface(display_, found->second.copy_surface);
  }
  wrapped_.erase(found);
}

bool AngleGpu::Current(const Swapchain& swapchain, uint32_t index,
                       std::string* error) {
  const auto found = wrapped_.find(swapchain.id);
  if (found == wrapped_.end()) {
    *error = "swapchain not wrapped";
    return false;
  }
  const auto& wrapped = found->second;
  EGLSurface surface = wrapped.copy_surface;
  if (!wrapped.surfaces.empty()) {
    if (index >= wrapped.surfaces.size()) {
      *error = "image " + std::to_string(index) + " out of range";
      return false;
    }
    surface = wrapped.surfaces[index];
  }
  if (!eglMakeCurrent(display_, surface, surface, context_)) {
    *error = "eglMakeCurrent (EGL " + Hex(eglGetError()) + ")";
    return false;
  }
  return true;
}

void AngleGpu::Present(const Swapchain& swapchain, uint32_t index) {
  // The draws reach the immediate context, where the runtime's work on the
  // image comes after them
  glFlush();
  const auto& wrapped = wrapped_[swapchain.id];
  if (wrapped.surfaces.empty() && wrapped.copy && index < wrapped.images.size()) {
    immediate_->CopyResource(wrapped.images[index], wrapped.copy.Get());
  }
}

bool AngleGpu::DrawEye(const Swapchain& swapchain, uint32_t index, int eye,
                       const XrView& view, double yaw_offset, double time,
                       std::string* error) {
  if (!Current(swapchain, index, error)) {
    return false;
  }
  ProjectionView eye_view;
  projection_view::EyeView(
      {view.pose.orientation.x, view.pose.orientation.y,
       view.pose.orientation.z, view.pose.orientation.w},
      view.fov.angleLeft, view.fov.angleRight, view.fov.angleUp,
      view.fov.angleDown, yaw_offset, &eye_view);
  // The head never rests in a headset: the bilinear filter, the cheap one
  eye_view.sharp = false;
  const auto width = static_cast<int32_t>(swapchain.width);
  const auto height = static_cast<int32_t>(swapchain.height);
  bool drawn = false;
  if (mode_ == Mode::kPattern) {
    ProjectionSetup setup;
    setup.kind = 3;
    drawn = renderer_.DrawFrame({}, setup, eye_view, width, height,
                                {static_cast<float>(eye),
                                 static_cast<float>(time)});
  } else {
    ProjectionFrameSnapshot snapshot;
    if (source_ && source_(&snapshot) && snapshot.has_frame &&
        snapshot.setup != nullptr) {
      auto setup = *snapshot.setup;
      // Each eye its own part of a stereo frame (the window shows one)
      if (setup.kind == 0 && stereo_ != ScreenStereo::kMono) {
        const float left_right[2][4] = {{0, 0, 0.5f, 1}, {0.5f, 0, 0.5f, 1}};
        const float top_bottom[2][4] = {{0, 0, 1, 0.5f}, {0, 0.5f, 1, 0.5f}};
        const auto* rect = stereo_ == ScreenStereo::kLeftRight
                               ? left_right[eye]
                               : top_bottom[eye];
        std::copy(rect, rect + 4, setup.eye);
      }
      drawn = renderer_.DrawFrame(snapshot.frame, setup, eye_view, width,
                                  height);
      if (!drawn) {
        *error = "renderer C: " + renderer_.error();
      }
    } else {
      if (!waiting_logged_) {
        std::cout << "media_kit: XR: no frame of the player yet" << std::endl;
        waiting_logged_ = true;
      }
      // Black until the player has a frame: the head tracking shows anyway
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      glViewport(0, 0, width, height);
      glClearColor(0, 0, 0, 1);
      glClear(GL_COLOR_BUFFER_BIT);
      drawn = true;
    }
  }
  Present(swapchain, index);
  return drawn;
}

bool AngleGpu::DrawScreen(const Swapchain& swapchain, uint32_t index,
                          std::string* error) {
  ProjectionFrameSnapshot snapshot;
  if (!source_ || !source_(&snapshot) || !snapshot.has_frame) {
    *error = "no frame";
    return false;
  }
  if (!Current(swapchain, index, error)) {
    return false;
  }
  auto bind = reinterpret_cast<BindVertexArray>(
      eglGetProcAddress("glBindVertexArray"));
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, static_cast<GLsizei>(swapchain.width),
             static_cast<GLsizei>(swapchain.height));
  glDisable(GL_BLEND);
  glDisable(GL_SCISSOR_TEST);
  glUseProgram(blit_program_);
  if (bind != nullptr) bind(blit_vertex_array_);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, snapshot.frame.texture);
  glUniform1i(glGetUniformLocation(blit_program_, "uSource"), 0);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glBindTexture(GL_TEXTURE_2D, 0);
  if (bind != nullptr) bind(0);
  glUseProgram(0);
  Present(swapchain, index);
  return true;
}

void AngleGpu::Finish() { glFinish(); }

Content AngleGpu::ContentNow() {
  Content content;
  ProjectionFrameSnapshot snapshot;
  if (source_ && source_(&snapshot)) {
    content.has_frame = snapshot.has_frame;
    content.frame_width = snapshot.frame.width;
    content.frame_height = snapshot.frame.height;
    content.frame_count = snapshot.count;
  }
  return content;
}

GpuMemory AngleGpu::Memory() {
  GpuMemory memory;
  DXGI_QUERY_VIDEO_MEMORY_INFO info{};
  if (adapter3_ && SUCCEEDED(adapter3_->QueryVideoMemoryInfo(
                       0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
    memory.known = true;
    memory.usage = info.CurrentUsage;
    memory.budget = info.Budget;
  }
  return memory;
}

void AngleGpu::Close() {
  if (display_ == EGL_NO_DISPLAY) {
    return;
  }
  if (context_ != EGL_NO_CONTEXT) {
    eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, context_);
    std::vector<uint64_t> ids;
    for (const auto& [id, wrapped] : wrapped_) ids.push_back(id);
    for (const auto id : ids) UnwrapImages(id);
    renderer_.Release();
    if (blit_program_ != 0) {
      glDeleteProgram(blit_program_);
      blit_program_ = 0;
    }
    auto remove = reinterpret_cast<DeleteVertexArrays>(
        eglGetProcAddress("glDeleteVertexArrays"));
    if (blit_vertex_array_ != 0 && remove != nullptr) {
      remove(1, &blit_vertex_array_);
      blit_vertex_array_ = 0;
    }
    eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display_, context_);
    context_ = EGL_NO_CONTEXT;
  }
  // The display is the players': never terminated here
  display_ = EGL_NO_DISPLAY;
  immediate_.Reset();
  adapter3_.Reset();
  device_.Reset();
}

}  // namespace xr_probe
