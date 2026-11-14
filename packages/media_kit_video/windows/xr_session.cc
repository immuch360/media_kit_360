// Immuch360: the headset probe's interface for the plugin (IMMUCH360-NOTE.md,
// patch 15); see the header.

#include "xr_session.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

#include "angle_surface_manager.h"
#include "xr_angle_gpu.h"
#include "xr_openxr_runtime.h"

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
      }
    }
  }
  ~LogFile() {
    if (file_ != nullptr) std::fclose(file_);
  }
  bool open() const { return file_ != nullptr; }

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

// The frame source, cut off by |Stop| so that the player may go while the
// probe's thread still ends
struct Source {
  std::mutex mutex;
  FrameSource function;
};

struct Probe {
  std::thread thread;
  std::mutex mutex;
  std::condition_variable ended;
  bool done = false;
  bool stop_asked = false;
  xr_probe::Controller* controller = nullptr;
  xr_probe::Status last;
  std::shared_ptr<Source> source;
  std::string log_path;
};

std::mutex g_mutex;
std::shared_ptr<Probe> g_probe;

const char* ModeName(xr_probe::Mode mode) {
  switch (mode) {
    case xr_probe::Mode::kSphere:
      return "sphere";
    case xr_probe::Mode::kScreen:
      return "screen";
    case xr_probe::Mode::kPattern:
      return "pattern";
  }
  return "?";
}

const char* StereoName(xr_probe::ScreenStereo stereo) {
  switch (stereo) {
    case xr_probe::ScreenStereo::kMono:
      return "mono";
    case xr_probe::ScreenStereo::kLeftRight:
      return "left right";
    case xr_probe::ScreenStereo::kTopBottom:
      return "top bottom";
  }
  return "?";
}

void Body(std::shared_ptr<Probe> probe, StartOptions options) {
  auto log = std::make_shared<LogFile>(options.log_path);
  {
    const auto now = std::time(nullptr);
    char date[64] = {0};
    std::tm local{};
    localtime_s(&local, &now);
    std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", &local);
    log->Write(std::string("Immuch360 headset probe (media_kit_360 xr-probe), ") +
               date);
  }
  std::ostringstream described;
  described << "mode " << ModeName(options.mode) << ", stereo "
            << StereoName(options.stereo) << ", content " << options.fps
            << " fps, window yaw " << options.yaw << ", "
            << (options.label.empty() ? "no label" : options.label);
  log->Write(described.str());
  if (!options.log_path.empty() && !log->open()) {
    std::cout << "media_kit: XR: the log file could not be opened" << std::endl;
  }
  const auto source = probe->source;
  {
    xr_probe::OpenXrRuntime runtime(LoaderPath());
    xr_probe::AngleGpu gpu(
        options.mode, options.stereo,
        [source](ProjectionFrameSnapshot* out) {
          std::lock_guard<std::mutex> lock(source->mutex);
          return source->function && source->function(out);
        },
        [](EGLDisplay display) {
          return ANGLESurfaceManager::ShareContextFor(display, 3);
        });
    xr_probe::Options controller_options;
    controller_options.mode = options.mode;
    controller_options.stereo = options.stereo;
    controller_options.fps = options.fps;
    controller_options.yaw = options.yaw;
    controller_options.log = [log](const std::string& line) {
      log->Write(line);
    };
    xr_probe::Controller controller(&runtime, &gpu, controller_options);
    {
      std::lock_guard<std::mutex> lock(probe->mutex);
      probe->controller = &controller;
      if (probe->stop_asked) controller.RequestStop();
    }
    controller.Run();
    std::lock_guard<std::mutex> lock(probe->mutex);
    probe->last = controller.StatusNow();
    probe->controller = nullptr;
  }
  log->Write("probe ended");
  {
    std::lock_guard<std::mutex> lock(probe->mutex);
    probe->done = true;
  }
  probe->ended.notify_all();
}

std::shared_ptr<Probe> Current() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_probe;
}

}  // namespace

bool Start(const StartOptions& options, FrameSource source,
           std::string* reason) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_probe != nullptr) {
    std::unique_lock<std::mutex> probe_lock(g_probe->mutex);
    if (!g_probe->done) {
      *reason = "a headset view already runs";
      return false;
    }
    probe_lock.unlock();
    if (g_probe->thread.joinable()) g_probe->thread.join();
  }
  auto probe = std::make_shared<Probe>();
  probe->source = std::make_shared<Source>();
  probe->source->function = std::move(source);
  probe->log_path = options.log_path;
  probe->thread = std::thread(Body, probe, options);
  g_probe = probe;
  return true;
}

bool Stop(int wait_ms) {
  const auto probe = Current();
  if (probe == nullptr) {
    return true;
  }
  {
    std::lock_guard<std::mutex> lock(probe->source->mutex);
    probe->source->function = nullptr;
  }
  std::unique_lock<std::mutex> lock(probe->mutex);
  probe->stop_asked = true;
  if (probe->controller != nullptr) {
    probe->controller->RequestStop();
  }
  const bool ended = probe->ended.wait_for(
      lock, std::chrono::milliseconds(wait_ms), [&] { return probe->done; });
  lock.unlock();
  if (ended && probe->thread.joinable()) {
    probe->thread.join();
  }
  if (!ended) {
    std::cout << "media_kit: XR: the probe did not end within " << wait_ms
              << " ms (the runtime may hang); left to end on its own"
              << std::endl;
  }
  return ended;
}

void Recenter() {
  const auto probe = Current();
  if (probe == nullptr) return;
  std::lock_guard<std::mutex> lock(probe->mutex);
  if (probe->controller != nullptr) probe->controller->RequestRecenter();
}

State Now() {
  State state;
  const auto probe = Current();
  if (probe == nullptr) {
    state.status.phase = "never started";
    return state;
  }
  std::lock_guard<std::mutex> lock(probe->mutex);
  state.running = !probe->done;
  state.status = probe->controller != nullptr ? probe->controller->StatusNow()
                                              : probe->last;
  state.log_path = probe->log_path;
  return state;
}

}  // namespace xr_session
