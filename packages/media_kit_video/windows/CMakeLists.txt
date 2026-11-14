# This file is a part of media_kit (https://github.com/media-kit/media-kit).
#
# Copyright © 2021 & onwards, Hitesh Kumar Saini <saini123hitesh@gmail.com>.
# All rights reserved.
# Use of this source code is governed by MIT license that can be found in the LICENSE file.

cmake_minimum_required(VERSION 3.14)

set(PROJECT_NAME "media_kit_video")
set(CMAKE_CXX_STANDARD 17)
project(${PROJECT_NAME} LANGUAGES CXX)

set(PLUGIN_NAME "media_kit_video_plugin")

# This is shipped as part of package:media_kit_libs_*** package(s).
# Must be built before this target.
set(LIBMPV_SRC "${CMAKE_BINARY_DIR}/libmpv")
set(ANGLE_SRC "${CMAKE_BINARY_DIR}/ANGLE")

# Deal with MSVC incompatiblity
add_compile_definitions(_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)

# Immuch360: the headset probe (IMMUCH360-NOTE.md, patch 15), OpenXR in the
# plugin, off unless the app asks for it: set(MEDIA_KIT_360_XR ON) in the app's
# windows/CMakeLists.txt before the plugins are added, or MEDIA_KIT_360_XR=1 in
# the environment of the build. With it on, the plugin compiles xr_*.cc
# against the vendored OpenXR headers (third_party/openxr, Apache 2.0 OR MIT)
# and the build downloads the Khronos loader (openxr_loader.dll of the
# OpenXR.Loader package of OpenXR-SDK, Apache 2.0 OR MIT), checked by its
# SHA-256, bundled next to the app's executable. The plugin opens it only when
# a headset view starts. x64 only: the package has no ARM64 build of it.
if(NOT DEFINED MEDIA_KIT_360_XR AND DEFINED ENV{MEDIA_KIT_360_XR})
  set(MEDIA_KIT_360_XR "$ENV{MEDIA_KIT_360_XR}")
endif()
set(MEDIA_KIT_360_XR_ON OFF)
if(MEDIA_KIT_360_XR AND MEDIA_KIT_LIBS_AVAILABLE)
  if(CMAKE_SYSTEM_PROCESSOR STREQUAL "ARM64" OR CMAKE_GENERATOR_PLATFORM STREQUAL "ARM64")
    message(NOTICE "media_kit: the headset probe is x64 only: off for ARM64")
  else()
    set(MEDIA_KIT_360_XR_ON ON)
  endif()
endif()
set(OPENXR_LOADER_VERSION "1.1.65")
set(OPENXR_LOADER_SHA256 "05638c526f07ba3a5f1084bbc3308d654d5167f844ffa1e35515325cb72e0c34")
set(OPENXR_LOADER_URL "https://github.com/KhronosGroup/OpenXR-SDK/releases/download/release-${OPENXR_LOADER_VERSION}/OpenXR.Loader.${OPENXR_LOADER_VERSION}.nupkg")
set(OPENXR_LOADER_DIR "${CMAKE_BINARY_DIR}/openxr-loader-${OPENXR_LOADER_VERSION}")
set(OPENXR_LOADER_DLL "${OPENXR_LOADER_DIR}/native/x64/release/bin/openxr_loader.dll")
if(MEDIA_KIT_360_XR_ON)
  if(NOT EXISTS "${OPENXR_LOADER_DLL}")
    set(OPENXR_LOADER_ARCHIVE "${CMAKE_BINARY_DIR}/OpenXR.Loader.${OPENXR_LOADER_VERSION}.nupkg")
    if(EXISTS "${OPENXR_LOADER_ARCHIVE}")
      file(SHA256 "${OPENXR_LOADER_ARCHIVE}" OPENXR_LOADER_HAVE)
    endif()
    if(NOT OPENXR_LOADER_HAVE STREQUAL OPENXR_LOADER_SHA256)
      message(STATUS "media_kit: downloading the OpenXR loader ${OPENXR_LOADER_VERSION}")
      file(DOWNLOAD "${OPENXR_LOADER_URL}" "${OPENXR_LOADER_ARCHIVE}"
        EXPECTED_HASH SHA256=${OPENXR_LOADER_SHA256}
        TLS_VERIFY ON
        SHOW_PROGRESS)
    endif()
    # A .nupkg is a ZIP: the x64 loader only
    file(ARCHIVE_EXTRACT INPUT "${OPENXR_LOADER_ARCHIVE}" DESTINATION "${OPENXR_LOADER_DIR}"
      PATTERNS "native/x64/release/bin/openxr_loader.dll")
  endif()
  if(NOT EXISTS "${OPENXR_LOADER_DLL}")
    message(FATAL_ERROR "media_kit: the OpenXR loader is not in its package")
  endif()
  message(STATUS "media_kit: the headset probe is on (OpenXR loader ${OPENXR_LOADER_VERSION})")
  set(MEDIA_KIT_360_XR_SOURCES
    "xr_angle_gpu.cc"
    "xr_controller.cc"
    "xr_openxr_runtime.cc"
    "xr_session.cc"
  )
endif()

if(MEDIA_KIT_LIBS_AVAILABLE)
  include_directories(
    "${LIBMPV_SRC}/include"
    "${ANGLE_SRC}/include"
  )

  add_library(
    ${PLUGIN_NAME} SHARED
    "angle_surface_manager.cc"
    "media_kit_video_plugin_c_api.cc"
    "media_kit_video_plugin.cc"
    "projection_renderer.cc"
    "video_output_manager.cc"
    "video_output.cc"
    "utils.cc"
    ${MEDIA_KIT_360_XR_SOURCES}
    ${PLUGIN_SOURCES}
  )

  apply_standard_settings(${PLUGIN_NAME})

  # Immuch360: the OpenXR headers for every source (xr_probe_core.h is
  # portable), the probe compiled in only with MEDIA_KIT_360_XR (patch 15)
  target_include_directories(${PLUGIN_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/openxr/include")
  if(MEDIA_KIT_360_XR_ON)
    target_compile_definitions(${PLUGIN_NAME} PRIVATE MEDIA_KIT_360_XR=1)
    target_link_libraries(${PLUGIN_NAME} PRIVATE d3d11 dxgi)
  endif()

  set_target_properties(
    ${PLUGIN_NAME} PROPERTIES
    CXX_VISIBILITY_PRESET hidden
  )
  target_compile_definitions(
    ${PLUGIN_NAME} PRIVATE
    FLUTTER_PLUGIN_IMPL
  )

  target_include_directories(
    ${PLUGIN_NAME} INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
  )

  target_link_libraries(
    ${PLUGIN_NAME} PRIVATE
    flutter
    flutter_wrapper_plugin

    # Link to libmpv & ANGLE.
    "${LIBMPV_SRC}/libmpv.dll.a"
    "${ANGLE_SRC}/lib/libEGL.dll.lib"
    "${ANGLE_SRC}/lib/libGLESv2.dll.lib"
  )
else()
  message(NOTICE "media_kit: WARNING: package:media_kit_libs_*** not found.")

  add_library(
    ${PLUGIN_NAME} SHARED
    "media_kit_video_plugin_c_api.cc"
    ${PLUGIN_SOURCES}
  )

  apply_standard_settings(${PLUGIN_NAME})

  set_target_properties(
    ${PLUGIN_NAME} PROPERTIES
    CXX_VISIBILITY_PRESET hidden
  )

  target_compile_definitions(
    ${PLUGIN_NAME} PRIVATE
    FLUTTER_PLUGIN_IMPL

    # Add macro to indicate that libmpv & ANGLE from package:media_kit_libs_*** are not found.
    "MEDIA_KIT_LIBS_NOT_FOUND=1"
  )

  target_include_directories(
    ${PLUGIN_NAME} INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
  )

  target_link_libraries(
    ${PLUGIN_NAME} PRIVATE
    flutter
    flutter_wrapper_plugin
  )
endif()

# Immuch360: the OpenXR loader next to the executable when the probe is on
# (patch 15)
if(MEDIA_KIT_360_XR_ON)
  set(MEDIA_KIT_VIDEO_BUNDLED "${OPENXR_LOADER_DLL}")
else()
  set(MEDIA_KIT_VIDEO_BUNDLED "")
endif()
set(
  media_kit_video_bundled_libraries
  "${MEDIA_KIT_VIDEO_BUNDLED}"
  PARENT_SCOPE
)
