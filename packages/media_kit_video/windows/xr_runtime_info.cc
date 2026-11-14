// Immuch360: the active OpenXR runtime (IMMUCH360-NOTE.md, patch 15); see
// the header.

#include "xr_runtime_info.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cwchar>

#include "xr_probe_core.h"

#ifndef RRF_SUBKEY_WOW6464KEY
#define RRF_SUBKEY_WOW6464KEY 0x00010000
#endif

namespace xr_session {

namespace {

std::string Utf8(const std::wstring& text) {
  if (text.empty()) return {};
  const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                         static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
  std::string out(size, '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                        static_cast<int>(text.size()), out.data(), size,
                        nullptr, nullptr);
  return out;
}

}  // namespace

RuntimeInfo ActiveRuntime() {
  RuntimeInfo info;
  std::wstring path;
  wchar_t from_environment[4096] = {0};
  const DWORD length =
      ::GetEnvironmentVariableW(L"XR_RUNTIME_JSON", from_environment, 4096);
  if (length > 0 && length < 4096) {
    path = from_environment;
    info.source = "environment";
  } else {
    DWORD size = 0;
    const DWORD flags = RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY;
    auto status =
        ::RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1",
                       L"ActiveRuntime", flags, nullptr, nullptr, &size);
    if (status == ERROR_SUCCESS && size > sizeof(wchar_t)) {
      std::wstring value(size / sizeof(wchar_t) + 1, L'\0');
      size = static_cast<DWORD>(value.size() * sizeof(wchar_t));
      status = ::RegGetValueW(HKEY_LOCAL_MACHINE,
                              L"SOFTWARE\\Khronos\\OpenXR\\1",
                              L"ActiveRuntime", flags, nullptr, value.data(),
                              &size);
      if (status == ERROR_SUCCESS) {
        value.resize(std::wcslen(value.c_str()));
        path = value;
        info.source = "registry";
      }
    }
  }
  if (!path.empty()) {
    const auto attributes = ::GetFileAttributesW(path.c_str());
    info.exists = attributes != INVALID_FILE_ATTRIBUTES &&
                  (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    const auto utf8 = Utf8(path);
    info.manifest_name = xr_probe::FileNameOf(utf8);
    info.runtime = xr_probe::GuessRuntime(utf8);
  }
  return info;
}

}  // namespace xr_session
