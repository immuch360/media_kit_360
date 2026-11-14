// This file is a part of media_kit
// (https://github.com/media-kit/media-kit).
//
// Copyright © 2021 & onwards, Hitesh Kumar Saini <saini123hitesh@gmail.com>.
// All rights reserved.
// Use of this source code is governed by MIT license that can be found in the
// LICENSE file.
#include "media_kit_video_plugin.h"
#include "utils.h"

#if MEDIA_KIT_360_XR
#include "xr_session.h"
#endif

#include <Windows.h>

#include <algorithm>
#include <memory>
#include <string>
#include <thread>

namespace media_kit_video {

namespace {

// Immuch360: the arguments of the renderer C calls (IMMUCH360-NOTE.md, patch
// 6), whatever number type the standard codec chose for them.

const flutter::EncodableValue* Find(const flutter::EncodableMap& map,
                                    const char* key) {
  auto entry = map.find(flutter::EncodableValue(key));
  return entry == map.end() ? nullptr : &entry->second;
}

int64_t ToInt64(const flutter::EncodableValue* value, int64_t fallback) {
  if (value == nullptr) {
    return fallback;
  }
  if (auto v = std::get_if<int32_t>(value)) {
    return *v;
  }
  if (auto v = std::get_if<int64_t>(value)) {
    return *v;
  }
  if (auto v = std::get_if<double>(value)) {
    return static_cast<int64_t>(*v);
  }
  if (auto v = std::get_if<std::string>(value)) {
    try {
      return std::stoll(*v);
    } catch (...) {
      return fallback;
    }
  }
  return fallback;
}

double ToDouble(const flutter::EncodableValue* value, double fallback) {
  if (value == nullptr) {
    return fallback;
  }
  if (auto v = std::get_if<double>(value)) {
    return *v;
  }
  if (auto v = std::get_if<int32_t>(value)) {
    return *v;
  }
  if (auto v = std::get_if<int64_t>(value)) {
    return static_cast<double>(*v);
  }
  return fallback;
}

std::string ToString(const flutter::EncodableValue* value) {
  if (value == nullptr) {
    return "";
  }
  if (auto v = std::get_if<std::string>(value)) {
    return *v;
  }
  return "";
}

bool ToBool(const flutter::EncodableValue* value, bool fallback) {
  if (value == nullptr) {
    return fallback;
  }
  if (auto v = std::get_if<bool>(value)) {
    return *v;
  }
  return fallback;
}

std::vector<float> ToFloats(const flutter::EncodableValue* value) {
  std::vector<float> floats;
  if (value == nullptr) {
    return floats;
  }
  if (auto list = std::get_if<flutter::EncodableList>(value)) {
    for (const auto& item : *list) {
      floats.push_back(static_cast<float>(ToDouble(&item, 0.0)));
    }
  } else if (auto doubles = std::get_if<std::vector<double>>(value)) {
    for (const auto item : *doubles) {
      floats.push_back(static_cast<float>(item));
    }
  } else if (auto singles = std::get_if<std::vector<float>>(value)) {
    floats = *singles;
  }
  return floats;
}

// Immuch360: the times of a stabilisation table (Int64List, or a list)
std::vector<int64_t> ToInt64s(const flutter::EncodableValue* value) {
  std::vector<int64_t> values;
  if (value == nullptr) {
    return values;
  }
  if (auto longs = std::get_if<std::vector<int64_t>>(value)) {
    values = *longs;
  } else if (auto ints = std::get_if<std::vector<int32_t>>(value)) {
    values.assign(ints->begin(), ints->end());
  } else if (auto list = std::get_if<flutter::EncodableList>(value)) {
    for (const auto& item : *list) {
      values.push_back(ToInt64(&item, 0));
    }
  }
  return values;
}

// Immuch360: the corrections of a stabilisation table (Float64List,
// Float32List, or a list)
std::vector<double> ToDoubles(const flutter::EncodableValue* value) {
  std::vector<double> values;
  if (value == nullptr) {
    return values;
  }
  if (auto doubles = std::get_if<std::vector<double>>(value)) {
    values = *doubles;
  } else if (auto singles = std::get_if<std::vector<float>>(value)) {
    values.assign(singles->begin(), singles->end());
  } else if (auto list = std::get_if<flutter::EncodableList>(value)) {
    for (const auto& item : *list) {
      values.push_back(ToDouble(&item, 0.0));
    }
  }
  return values;
}

void CopyFloats(const flutter::EncodableValue* value, float* out, size_t size) {
  const auto floats = ToFloats(value);
  if (floats.size() == size) {
    std::copy(floats.begin(), floats.end(), out);
  }
}

ProjectionSetup ToProjectionSetup(const flutter::EncodableMap& arguments) {
  ProjectionSetup setup;
  setup.kind = static_cast<int32_t>(ToInt64(Find(arguments, "kind"), 0));
  CopyFloats(Find(arguments, "eye"), setup.eye, 4);
  CopyFloats(Find(arguments, "crop"), setup.crop, 4);
  // Immuch360, kind 4 (patch 15)
  setup.aspect = static_cast<float>(ToDouble(Find(arguments, "aspect"), 0.0));
  setup.tracks = static_cast<int32_t>(ToInt64(Find(arguments, "tracks"), 1));
  CopyFloats(Find(arguments, "streamsEnabled"), setup.enabled, 2);
  if (auto uniforms = Find(arguments, "uniforms")) {
    if (auto map = std::get_if<flutter::EncodableMap>(uniforms)) {
      for (const auto& [key, values] : *map) {
        if (auto name = std::get_if<std::string>(&key)) {
          setup.uniforms[*name] = ToFloats(&values);
        }
      }
    }
  }
  setup.max_frame_width = static_cast<int32_t>(
      ToInt64(Find(arguments, "maxFrameWidth"), setup.max_frame_width));
  setup.max_frame_pixels =
      ToInt64(Find(arguments, "maxFramePixels"), setup.max_frame_pixels);
  setup.output_width =
      static_cast<int32_t>(ToInt64(Find(arguments, "outputWidth"), 1));
  setup.output_height =
      static_cast<int32_t>(ToInt64(Find(arguments, "outputHeight"), 1));
  setup.max_output_height = static_cast<int32_t>(
      ToInt64(Find(arguments, "maxOutputHeight"), setup.max_output_height));
  // Immuch360, two players for one raw file (patch 7)
  setup.partner_stream = ToBool(Find(arguments, "partnerStream"), false);
  setup.secondary = ToBool(Find(arguments, "secondary"), false);
  return setup;
}

}  // namespace

MediaKitVideoPlugin* MediaKitVideoPlugin::instance_ = nullptr;

void MediaKitVideoPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrarWindows* registrar) {
  auto plugin = std::make_unique<MediaKitVideoPlugin>(registrar);
  registrar->AddPlugin(std::move(plugin));
}

MediaKitVideoPlugin::MediaKitVideoPlugin(
    flutter::PluginRegistrarWindows* registrar)
    : registrar_(registrar),
      video_output_manager_(std::make_unique<VideoOutputManager>(registrar)) {
  instance_ = this;
  flutter_window_ =
      ::GetAncestor(registrar->GetView()->GetNativeWindow(), GA_ROOT);
  original_window_proc_ = reinterpret_cast<WNDPROC>(
      ::SetWindowLongPtr(flutter_window_, GWLP_WNDPROC,
                         reinterpret_cast<LONG_PTR>(WindowProcDelegate)));

  channel_ = std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
      registrar->messenger(), "com.alexmercerind/media_kit_video",
      &flutter::StandardMethodCodec::GetInstance());
  channel_->SetMethodCallHandler([&](const auto& call, auto result) {
    HandleMethodCall(call, std::move(result));
  });
}

MediaKitVideoPlugin::~MediaKitVideoPlugin() {
  if (flutter_window_ && original_window_proc_) {
    ::SetWindowLongPtr(flutter_window_, GWLP_WNDPROC,
                       reinterpret_cast<LONG_PTR>(original_window_proc_));
  }
  if (instance_ == this) {
    instance_ = nullptr;
  }
}

void MediaKitVideoPlugin::RunOnMainThread(std::function<void()> task) {
  if (!flutter_window_) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(main_thread_tasks_mutex_);
    main_thread_tasks_.push(std::move(task));
  }

  ::PostMessage(flutter_window_, kMainThreadTaskMessage, 0, 0);
}

LRESULT CALLBACK MediaKitVideoPlugin::WindowProcDelegate(HWND hwnd,
                                                         UINT message,
                                                         WPARAM wParam,
                                                         LPARAM lParam) {
  if (message == kMainThreadTaskMessage && instance_) {
    instance_->ProcessMainThreadTasks();
    return 0;
  }

  if (instance_ && instance_->original_window_proc_) {
    return ::CallWindowProc(instance_->original_window_proc_, hwnd, message,
                            wParam, lParam);
  }

  return ::DefWindowProc(hwnd, message, wParam, lParam);
}

void MediaKitVideoPlugin::ProcessMainThreadTasks() {
  std::queue<std::function<void()>> tasks_to_execute;

  {
    std::lock_guard<std::mutex> lock(main_thread_tasks_mutex_);
    tasks_to_execute.swap(main_thread_tasks_);
  }

  while (!tasks_to_execute.empty()) {
    auto task = std::move(tasks_to_execute.front());
    tasks_to_execute.pop();

    try {
      task();
    } catch (...) {
    }
  }
}

#if MEDIA_KIT_360_XR
namespace {

// Immuch360, the headset view (IMMUCH360-NOTE.md, patch 15): XrContent of
// lib/src/xr_headset.dart
xr_probe::ContentRequest ToXrContent(const flutter::EncodableValue* value) {
  xr_probe::ContentRequest content;
  const auto* map =
      value != nullptr ? std::get_if<flutter::EncodableMap>(value) : nullptr;
  if (map == nullptr) {
    return content;
  }
  content.handle = ToInt64(Find(*map, "handle"), 0);
  content.mode = xr_probe::ModeOf(ToString(Find(*map, "mode")));
  content.stereo = xr_probe::StereoOf(ToString(Find(*map, "stereo")));
  content.squeeze = xr_probe::SqueezeOf(ToString(Find(*map, "squeeze")));
  content.fps = ToDouble(Find(*map, "fps"), 0.0);
  content.yaw = ToDouble(Find(*map, "yaw"), 0.0);
  content.label = ToString(Find(*map, "label"));
  return content;
}

flutter::EncodableMap ToXrState(const xr_probe::PollAnswer& poll) {
  using flutter::EncodableValue;
  const auto& status = poll.status;
  flutter::EncodableList events;
  for (const auto& event : poll.events) {
    events.push_back(EncodableValue(flutter::EncodableMap{
        {EncodableValue("sequence"),
         EncodableValue(static_cast<int64_t>(event.sequence))},
        {EncodableValue("kind"),
         EncodableValue(std::string(xr_probe::EventKindName(event.kind)))},
        {EncodableValue("seconds"), EncodableValue(event.seconds)},
        {EncodableValue("on"), EncodableValue(event.on)},
        {EncodableValue("hand"),
         EncodableValue(std::string(event.hand == 0 ? "left" : "right"))},
        {EncodableValue("profile"), EncodableValue(event.profile)},
        {EncodableValue("time"), EncodableValue(event.time)},
    }));
  }
  return flutter::EncodableMap{
      {EncodableValue("compiled"), EncodableValue(true)},
      {EncodableValue("running"), EncodableValue(poll.running)},
      {EncodableValue("phase"), EncodableValue(status.phase)},
      {EncodableValue("sessionState"), EncodableValue(status.session_state)},
      {EncodableValue("visible"), EncodableValue(status.visible)},
      {EncodableValue("focused"), EncodableValue(status.focused)},
      {EncodableValue("reason"),
       EncodableValue(std::string(xr_probe::StopReasonCode(status.reason)))},
      {EncodableValue("detail"), EncodableValue(status.detail)},
      {EncodableValue("runtime"), EncodableValue(status.runtime)},
      {EncodableValue("system"), EncodableValue(status.system)},
      {EncodableValue("swapchainPath"), EncodableValue(status.swapchain_path)},
      {EncodableValue("mode"), EncodableValue(status.mode)},
      {EncodableValue("leftProfile"), EncodableValue(status.profiles[0])},
      {EncodableValue("rightProfile"), EncodableValue(status.profiles[1])},
      {EncodableValue("controllers"), EncodableValue(status.controllers)},
      {EncodableValue("lyingDown"), EncodableValue(status.lying_down)},
      {EncodableValue("refreshRate"),
       EncodableValue(static_cast<double>(status.refresh_rate))},
      {EncodableValue("refreshAsked"),
       EncodableValue(static_cast<double>(status.refresh_asked))},
      {EncodableValue("frames"), EncodableValue(status.frames)},
      {EncodableValue("framesShown"), EncodableValue(status.frames_shown)},
      {EncodableValue("framesLate"), EncodableValue(status.frames_late)},
      {EncodableValue("memoryTight"), EncodableValue(status.memory_tight)},
      {EncodableValue("memoryUsedMb"),
       EncodableValue(static_cast<int64_t>(status.memory_used_mb))},
      {EncodableValue("memoryBudgetMb"),
       EncodableValue(static_cast<int64_t>(status.memory_budget_mb))},
      {EncodableValue("statsAt"), EncodableValue(status.stats_at)},
      {EncodableValue("snapshotMs"), EncodableValue(status.snapshot_ms)},
      {EncodableValue("snapshotMaxMs"), EncodableValue(status.snapshot_max_ms)},
      {EncodableValue("stripUploads"), EncodableValue(status.strip_uploads)},
      {EncodableValue("stripShown"), EncodableValue(status.strip_shown)},
      {EncodableValue("instanceLost"), EncodableValue(status.instance_lost)},
      {EncodableValue("logPath"), EncodableValue(poll.log_path)},
      {EncodableValue("lastSequence"),
       EncodableValue(static_cast<int64_t>(poll.last_sequence))},
      {EncodableValue("dropped"),
       EncodableValue(static_cast<int64_t>(poll.dropped))},
      {EncodableValue("events"), EncodableValue(events)},
  };
}

}  // namespace
#endif

void MediaKitVideoPlugin::HandleXr(
    const std::string& method,
    const flutter::EncodableMap& arguments,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  const auto value = [](auto v) { return flutter::EncodableValue(v); };
  const auto ok = [&]() {
    result->Success(flutter::EncodableValue(
        flutter::EncodableMap{{value("ok"), value(true)}}));
  };
#if MEDIA_KIT_360_XR
  if (method == "VideoOutputManager.XrRuntimeInfo") {
    // The registry only: no DLL loaded
    const auto info = xr_session::ActiveRuntime();
    result->Success(flutter::EncodableValue(flutter::EncodableMap{
        {value("compiled"), value(true)},
        {value("registered"), value(info.registered())},
        {value("source"), value(info.source)},
        {value("manifestName"), value(info.manifest_name)},
        {value("exists"), value(info.exists)},
        {value("runtime"), value(info.runtime)},
    }));
  } else if (method == "VideoOutputManager.XrStart") {
    xr_probe::StartRequest request;
    request.content = ToXrContent(Find(arguments, "content"));
    request.log_path = ToString(Find(arguments, "logPath"));
    request.match_refresh_rate =
        ToBool(Find(arguments, "matchRefreshRate"), true);
    request.app_version = ToString(Find(arguments, "appVersion"));
    if (const auto* names = Find(arguments, "actionNames")) {
      if (const auto* map = std::get_if<flutter::EncodableMap>(names)) {
        for (const auto& [key, name] : *map) {
          if (const auto* text = std::get_if<std::string>(&key)) {
            request.action_names[*text] = ToString(&name);
          }
        }
      }
    }
    auto* manager = video_output_manager_.get();
    const auto answer = xr_session::Start(
        request, [manager](int64_t handle, ProjectionFrameSnapshot* out) {
          return manager->XrSnapshot(handle, out);
        });
    flutter::EncodableMap reply{{value("ok"), value(answer.ok)}};
    if (!answer.ok) {
      reply[value("reason")] =
          value(std::string(xr_probe::StopReasonCode(answer.reason)));
      reply[value("detail")] = value(answer.detail);
    }
    result->Success(flutter::EncodableValue(reply));
  } else if (method == "VideoOutputManager.XrSetContent") {
    xr_session::SetContent(ToXrContent(Find(arguments, "content")));
    ok();
  } else if (method == "VideoOutputManager.XrSetContentRate") {
    xr_session::SetContentRate(ToDouble(Find(arguments, "fps"), 0.0));
    ok();
  } else if (method == "VideoOutputManager.XrSetStrip") {
    std::vector<uint8_t> rgba;
    if (const auto* bytes = Find(arguments, "rgba")) {
      if (const auto* list = std::get_if<std::vector<uint8_t>>(bytes)) {
        rgba = *list;
      }
    }
    const auto width =
        static_cast<uint32_t>((std::max)(int64_t{0}, ToInt64(Find(arguments, "width"), 0)));
    const auto height =
        static_cast<uint32_t>((std::max)(int64_t{0}, ToInt64(Find(arguments, "height"), 0)));
    xr_session::SetStrip(std::move(rgba), width, height,
                         ToBool(Find(arguments, "visible"), false));
    ok();
  } else if (method == "VideoOutputManager.XrRecenter") {
    xr_session::Recenter(ToBool(Find(arguments, "lyingDown"), false));
    ok();
  } else if (method == "VideoOutputManager.XrPoll") {
    const auto after = ToInt64(Find(arguments, "after"), 0);
    result->Success(flutter::EncodableValue(
        ToXrState(xr_session::Poll(static_cast<uint64_t>((std::max)(int64_t{0}, after))))));
  } else if (method == "VideoOutputManager.XrStop") {
    // The runtime may take a moment to end its session: waited for on a
    // thread, answered on the platform thread
    std::shared_ptr<flutter::MethodResult<flutter::EncodableValue>> reply =
        std::move(result);
    std::thread([this, reply]() {
      const bool ended = xr_session::Stop();
      RunOnMainThread([reply, ended]() {
        reply->Success(flutter::EncodableValue(flutter::EncodableMap{
            {flutter::EncodableValue("ok"), flutter::EncodableValue(ended)}}));
      });
    }).detach();
  } else {
    result->NotImplemented();
  }
#else
  (void)arguments;
  (void)ok;
  const auto not_in_build = value(std::string("notInBuild"));
  const auto detail = value(
      std::string("this build has no headset view (MEDIA_KIT_360_XR off)"));
  if (method == "VideoOutputManager.XrPoll") {
    result->Success(flutter::EncodableValue(flutter::EncodableMap{
        {value("compiled"), value(false)},
        {value("running"), value(false)},
        {value("phase"), value(std::string("not in this build"))},
        {value("reason"), not_in_build},
        {value("detail"), detail},
    }));
  } else if (method == "VideoOutputManager.XrRuntimeInfo") {
    result->Success(flutter::EncodableValue(flutter::EncodableMap{
        {value("compiled"), value(false)},
        {value("registered"), value(false)},
    }));
  } else {
    result->Success(flutter::EncodableValue(flutter::EncodableMap{
        {value("ok"), value(false)},
        {value("reason"), not_in_build},
        {value("detail"), detail},
    }));
  }
#endif
}

void MediaKitVideoPlugin::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue>& method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  if (method_call.method_name().compare("VideoOutputManager.Create") == 0) {
    auto arguments = std::get<flutter::EncodableMap>(*method_call.arguments());
    auto handle =
        std::get<std::string>(arguments[flutter::EncodableValue("handle")]);
    auto configuration = std::get<flutter::EncodableMap>(
        arguments[flutter::EncodableValue("configuration")]);

    auto handle_value = std::stoll(handle);
    auto configuration_value = VideoOutputConfiguration{};

    auto configuration_width =
        std::get<std::string>(configuration[flutter::EncodableValue("width")]);
    auto configuration_height =
        std::get<std::string>(configuration[flutter::EncodableValue("height")]);
    auto configuration_enable_hardware_acceleration = std::get<bool>(
        configuration[flutter::EncodableValue("enableHardwareAcceleration")]);
    if (configuration_width.compare("null") != 0) {
      configuration_value.width =
          static_cast<int64_t>(std::stoll(configuration_width.c_str()));
    }
    if (configuration_height.compare("null") != 0) {
      configuration_value.height =
          static_cast<int64_t>(std::stoll(configuration_height.c_str()));
    }
    configuration_value.enable_hardware_acceleration =
        configuration_enable_hardware_acceleration;
    // Immuch360: the render height cap of patch 3, also for the first
    // texture (patch 11); absent or "null", no cap
    configuration_value.max_output_height = std::max<int64_t>(
        0, ToInt64(Find(configuration, "maxOutputHeight"), 0));

    video_output_manager_->Create(
        handle_value, configuration_value,
        [this, handle = handle_value](auto id, auto width, auto height) {
          RunOnMainThread([=]() {
            channel_->InvokeMethod(
                "VideoOutput.Resize",
                std::make_unique<flutter::EncodableValue>(flutter::EncodableMap{
                    {
                        flutter::EncodableValue("handle"),
                        flutter::EncodableValue(handle),
                    },
                    {
                        flutter::EncodableValue("id"),
                        flutter::EncodableValue(id),
                    },
                    {
                        flutter::EncodableValue("rect"),
                        flutter::EncodableValue(flutter::EncodableMap{
                            {
                                flutter::EncodableValue("left"),
                                flutter::EncodableValue(0),
                            },
                            {
                                flutter::EncodableValue("top"),
                                flutter::EncodableValue(0),
                            },
                            {
                                flutter::EncodableValue("width"),
                                flutter::EncodableValue(width),
                            },
                            {
                                flutter::EncodableValue("height"),
                                flutter::EncodableValue(height),
                            },
                        }),
                    },
                }),
                nullptr);
          });
        });
    result->Success(flutter::EncodableValue(std::monostate{}));
  } else if (method_call.method_name().compare("VideoOutputManager.Dispose") ==
             0) {
    auto arguments = std::get<flutter::EncodableMap>(*method_call.arguments());
    auto handle =
        std::get<std::string>(arguments[flutter::EncodableValue("handle")]);
    auto handle_value = static_cast<int64_t>(std::stoll(handle.c_str()));
    video_output_manager_->Dispose(handle_value);
    result->Success(flutter::EncodableValue(std::monostate{}));
  } else if (method_call.method_name().compare("VideoOutputManager.SetSize") ==
             0) {
    auto arguments = std::get<flutter::EncodableMap>(*method_call.arguments());
    auto handle =
        std::get<std::string>(arguments[flutter::EncodableValue("handle")]);
    auto width =
        std::get<std::string>(arguments[flutter::EncodableValue("width")]);
    auto height =
        std::get<std::string>(arguments[flutter::EncodableValue("height")]);
    auto handle_value = static_cast<int64_t>(std::stoll(handle.c_str()));
    auto width_value = std::optional<int64_t>{};
    auto height_value = std::optional<int64_t>{};
    if (width.compare("null") != 0) {
      width_value = static_cast<int64_t>(std::stoll(width.c_str()));
    }
    if (height.compare("null") != 0) {
      height_value = static_cast<int64_t>(std::stoll(height.c_str()));
    }
    video_output_manager_->SetSize(handle_value, width_value, height_value);
    result->Success(flutter::EncodableValue(std::monostate{}));
  } else if (method_call.method_name().compare(
                 "VideoOutputManager.SetProjection") == 0) {
    // Immuch360: renderer C on or off (IMMUCH360-NOTE.md, patch 6). The
    // answer comes once the thread pool made the change, on the platform
    // thread as Flutter wants it.
    const auto& arguments =
        std::get<flutter::EncodableMap>(*method_call.arguments());
    const auto handle = ToInt64(Find(arguments, "handle"), 0);
    std::optional<ProjectionSetup> setup;
    if (ToBool(Find(arguments, "enabled"), false)) {
      setup = ToProjectionSetup(arguments);
    }
    std::shared_ptr<flutter::MethodResult<flutter::EncodableValue>> reply =
        std::move(result);
    video_output_manager_->SetProjection(
        handle, setup, [this, reply](flutter::EncodableMap answer) {
          RunOnMainThread([reply, answer]() {
            reply->Success(flutter::EncodableValue(answer));
          });
        });
  } else if (method_call.method_name().compare(
                 "VideoOutputManager.SetPartner") == 0) {
    // Immuch360: the second stream of a raw file from another player
    // (IMMUCH360-NOTE.md, patch 7), answered once the thread pool paired them
    const auto& arguments =
        std::get<flutter::EncodableMap>(*method_call.arguments());
    std::shared_ptr<flutter::MethodResult<flutter::EncodableValue>> reply =
        std::move(result);
    video_output_manager_->SetPartner(
        ToInt64(Find(arguments, "handle"), 0),
        ToInt64(Find(arguments, "partner"), 0),
        [this, reply](flutter::EncodableMap answer) {
          RunOnMainThread([reply, answer]() {
            reply->Success(flutter::EncodableValue(answer));
          });
        });
  } else if (method_call.method_name().compare(
                 "VideoOutputManager.SetView") == 0) {
    const auto& arguments =
        std::get<flutter::EncodableMap>(*method_call.arguments());
    ProjectionView view;
    view.yaw = static_cast<float>(ToDouble(Find(arguments, "yaw"), 0.0));
    view.pitch = static_cast<float>(ToDouble(Find(arguments, "pitch"), 0.0));
    view.fov = static_cast<float>(ToDouble(Find(arguments, "fov"), 90.0));
    view.sharp = ToBool(Find(arguments, "sharp"), true);
    const auto known = video_output_manager_->SetView(
        ToInt64(Find(arguments, "handle"), 0), view);
    result->Success(flutter::EncodableValue(known));
  } else if (method_call.method_name().compare(
                 "VideoOutputManager.SetStabilization") == 0) {
    // Immuch360: the stabilisation of a raw 360 video (patch 14). "table"
    // false keeps the corrections given before; true replaces them with
    // "timesUs" and "corrections", or removes them when those are missing.
    const auto& arguments =
        std::get<flutter::EncodableMap>(*method_call.arguments());
    flutter::EncodableMap answer;
    std::optional<std::shared_ptr<const stabilization::Table>> table;
    if (ToBool(Find(arguments, "table"), false)) {
      auto times = ToInt64s(Find(arguments, "timesUs"));
      auto corrections = ToDoubles(Find(arguments, "corrections"));
      std::shared_ptr<const stabilization::Table> made;
      if (!times.empty()) {
        std::string reason;
        made = stabilization::Table::Of(std::move(times),
                                        std::move(corrections), &reason);
        if (made == nullptr) {
          answer[flutter::EncodableValue("reason")] =
              flutter::EncodableValue(reason);
        }
      }
      table = made;
    }
    const bool refused = answer.count(flutter::EncodableValue("reason")) > 0;
    const auto known =
        !refused && video_output_manager_->SetStabilization(
                        ToInt64(Find(arguments, "handle"), 0),
                        ToBool(Find(arguments, "enabled"), false), table);
    if (!refused && !known) {
      answer[flutter::EncodableValue("reason")] =
          flutter::EncodableValue("no video output");
    }
    answer[flutter::EncodableValue("ok")] = flutter::EncodableValue(known);
    result->Success(flutter::EncodableValue(answer));
  } else if (method_call.method_name().compare(
                 "VideoOutputManager.ProjectionStats") == 0) {
    const auto& arguments =
        std::get<flutter::EncodableMap>(*method_call.arguments());
    const auto stats = video_output_manager_->ProjectionStats(
        ToInt64(Find(arguments, "handle"), 0),
        ToBool(Find(arguments, "probe"), false));
    if (stats.has_value()) {
      result->Success(flutter::EncodableValue(stats.value()));
    } else {
      result->Success(flutter::EncodableValue(std::monostate{}));
    }
  } else if (method_call.method_name().rfind("VideoOutputManager.Xr", 0) ==
             0) {
    // Immuch360: the headset probe (IMMUCH360-NOTE.md, patch 15), only in a
    // build with the MEDIA_KIT_360_XR option of CMake
    const auto* arguments =
        std::get_if<flutter::EncodableMap>(method_call.arguments());
    const flutter::EncodableMap empty;
    HandleXr(method_call.method_name(), arguments ? *arguments : empty,
             std::move(result));
  } else if (method_call.method_name().compare("Utils.EnterNativeFullscreen") ==
             0) {
    auto window =
        ::GetAncestor(registrar_->GetView()->GetNativeWindow(), GA_ROOT);
    Utils::EnterNativeFullscreen(window);
    result->Success(flutter::EncodableValue(std::monostate{}));
  } else if (method_call.method_name().compare("Utils.ExitNativeFullscreen") ==
             0) {
    auto window =
        ::GetAncestor(registrar_->GetView()->GetNativeWindow(), GA_ROOT);
    Utils::ExitNativeFullscreen(window);
    result->Success(flutter::EncodableValue(std::monostate{}));
  } else {
    result->NotImplemented();
  }
}

}  // namespace media_kit_video
