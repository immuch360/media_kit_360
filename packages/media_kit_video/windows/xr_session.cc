// Immuch360: the headset view's interface for the plugin (IMMUCH360-NOTE.md,
// patch 15), its Windows parts; see the header.

#include "xr_session.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <memory>
#include <mutex>

#include "angle_surface_manager.h"
#include "xr_angle_gpu.h"
#include "xr_openxr_runtime.h"

// The plugin's commit, when CMake could read it (windows/CMakeLists.txt),
// given bare and made a string here
#ifndef MEDIA_KIT_360_COMMIT
#define MEDIA_KIT_360_COMMIT unknown
#endif
#define MEDIA_KIT_360_TEXT2(x) #x
#define MEDIA_KIT_360_TEXT(x) MEDIA_KIT_360_TEXT2(x)

namespace xr_session {

namespace {

std::wstring Wide(const std::string& text) {
  if (text.empty()) return {};
  const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                         static_cast<int>(text.size()),
                                         nullptr, 0);
  std::wstring out(size, L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                        static_cast<int>(text.size()), out.data(), size);
  return out;
}

// openxr_loader.dll next to the app's executable, where the build bundles it
std::wstring LoaderPath() {
  wchar_t path[MAX_PATH * 2] = {0};
  const auto length = ::GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
  std::wstring folder(path, length);
  const auto slash = folder.find_last_of(L"\\/");
  folder = slash == std::wstring::npos ? L"." : folder.substr(0, slash);
  return folder + L"\\openxr_loader.dll";
}

class LogFile {
 public:
  explicit LogFile(const std::string& path) {
    start_ = std::chrono::steady_clock::now();
    if (!path.empty()) {
      if (::_wfopen_s(&file_, Wide(path).c_str(), L"wb") != 0) {
        file_ = nullptr;
        std::cout << "media_kit: XR: the log file could not be opened"
                  << std::endl;
      }
    }
    const auto now = std::time(nullptr);
    char date[64] = {0};
    std::tm local{};
    localtime_s(&local, &now);
    std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", &local);
    Write(std::string("log opened ") + date);
  }
  ~LogFile() {
    if (file_ != nullptr) std::fclose(file_);
  }

  void Write(const std::string& line) {
    const double seconds = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - start_)
                               .count();
    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "%9.3f ", seconds);
    std::lock_guard<std::mutex> lock(mutex_);
    std::cout << "media_kit: XR: " << line << std::endl;
    if (file_ != nullptr) {
      std::fputs(stamp, file_);
      std::fputs(line.c_str(), file_);
      std::fputs("\n", file_);
      std::fflush(file_);
    }
  }

 private:
  std::FILE* file_ = nullptr;
  std::mutex mutex_;
  std::chrono::steady_clock::time_point start_;
};

std::mutex g_source_mutex;
FrameSource g_source;

xr_probe::ViewParts MakeParts(const xr_probe::StartRequest& request,
                              std::shared_ptr<xr_probe::SourceGate> gate) {
  FrameSource source;
  {
    std::lock_guard<std::mutex> lock(g_source_mutex);
    source = g_source;
  }
  xr_probe::ViewParts parts;
  auto log = std::make_shared<LogFile>(request.log_path);
  parts.log = [log](const std::string& line) { log->Write(line); };
  parts.runtime = std::make_unique<xr_probe::OpenXrRuntime>(LoaderPath());
  parts.gpu = std::make_unique<xr_probe::AngleGpu>(
      // Read under the gate: once a stop closed it, no frame of a player is
      // read again (the app may dispose the player)
      [gate, source](int64_t handle, ProjectionFrameSnapshot* out) {
        std::lock_guard<std::mutex> lock(gate->mutex);
        return gate->open && source && source(handle, out);
      },
      [](EGLDisplay display) {
        return ANGLESurfaceManager::ShareContextFor(display, 3);
      },
      [](EGLDisplay display) { ANGLESurfaceManager::RetainDisplay(display); },
      [](EGLDisplay display) { ANGLESurfaceManager::ReleaseDisplay(display); });
  return parts;
}

// Made once, never destroyed: no destructor at the DLL's unload, under the
// loader lock, and never a joinable thread in a static; the plugin's
// teardown ends or detaches the view before it goes
xr_probe::SessionCore& Core() {
  static xr_probe::SessionCore* core = [] {
    xr_probe::CoreOptions options;
    options.build = MEDIA_KIT_360_TEXT(MEDIA_KIT_360_COMMIT);
    return new xr_probe::SessionCore(MakeParts, options);
  }();
  return *core;
}

}  // namespace

xr_probe::StartAnswer Start(const xr_probe::StartRequest& request,
                            FrameSource source) {
  {
    std::lock_guard<std::mutex> lock(g_source_mutex);
    g_source = std::move(source);
  }
  return Core().Start(request);
}

bool Stop(int wait_ms) { return Core().Stop(wait_ms); }

void SetContent(const xr_probe::ContentRequest& content) {
  Core().SetContent(content);
}

void SetContentRate(double fps) { Core().SetContentRate(fps); }

void SetStrip(std::vector<uint8_t> rgba, uint32_t width, uint32_t height,
              bool visible) {
  Core().SetStrip(std::move(rgba), width, height, visible);
}

void Recenter(bool lying_down) { Core().Recenter(lying_down); }

xr_probe::PollAnswer Poll(uint64_t after) { return Core().Poll(after); }

void Teardown(int wait_ms) {
  Core().Teardown(wait_ms);
  std::lock_guard<std::mutex> lock(g_source_mutex);
  g_source = nullptr;
}

}  // namespace xr_session
