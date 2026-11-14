// Immuch360: the headset view (IMMUCH360-NOTE.md, patch 15), the active
// OpenXR runtime read from Windows without loading anything (no loader, no
// runtime DLL): what the app shows before any headset view starts.

#ifndef XR_RUNTIME_INFO_H_
#define XR_RUNTIME_INFO_H_

#include <string>

namespace xr_session {

// The active OpenXR runtime as the loader would find it, read without
// loading anything: XR_RUNTIME_JSON first (the loader's override), else
// HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime in the 64 bit view.
struct RuntimeInfo {
  // "environment", "registry" or "none"
  std::string source = "none";
  // The manifest's file name only (never the folders: no user path leaves
  // the plugin)
  std::string manifest_name;
  // Whether the manifest file exists
  bool exists = false;
  // The runtime guessed from the path ("SteamVR", "Meta Horizon Link"...),
  // "" when unknown
  std::string runtime;
  // A manifest named and present
  bool registered() const { return source != "none" && exists; }
};

RuntimeInfo ActiveRuntime();

}  // namespace xr_session

#endif  // XR_RUNTIME_INFO_H_
