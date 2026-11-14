# Immuch360: the tests of the headset probe built by MSVC (IMMUCH360-NOTE.md,
# patch 15), with the warnings of the plugin's build (Flutter's
# apply_standard_settings: /W4 /WX /wd4100):
# - xr_probe_test: the portable tests (test/xr_probe_test.cc), also run by g++
#   on Linux;
# - xr_windows_test: the loader, the ANGLE device and the swapchain images on
#   the hosted runner (xr_windows_test.cc).
# MEDIA_KIT_360_ANGLE: the ANGLE folder of a Windows build of the example
# (example/build/windows/x64/ANGLE: include and lib).

cmake_minimum_required(VERSION 3.18)
project(media_kit_360_xr_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(PLUGIN "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(MEDIA_KIT_360_ANGLE "" CACHE PATH "The ANGLE folder of a Windows build")

add_executable(xr_probe_test
  "${PLUGIN}/test/xr_probe_test.cc"
  "${PLUGIN}/xr_controller.cc"
)
target_include_directories(xr_probe_test PRIVATE
  "${PLUGIN}" "${PLUGIN}/third_party/openxr/include")
target_compile_options(xr_probe_test PRIVATE /W4 /WX /wd4100)

add_executable(xr_windows_test
  "${CMAKE_CURRENT_SOURCE_DIR}/xr_windows_test.cc"
  "${PLUGIN}/xr_angle_gpu.cc"
  "${PLUGIN}/xr_controller.cc"
  "${PLUGIN}/xr_openxr_runtime.cc"
  "${PLUGIN}/projection_renderer.cc"
)
target_include_directories(xr_windows_test PRIVATE
  "${PLUGIN}" "${PLUGIN}/third_party/openxr/include"
  "${MEDIA_KIT_360_ANGLE}/include")
target_compile_definitions(xr_windows_test PRIVATE MEDIA_KIT_360_XR=1)
target_compile_options(xr_windows_test PRIVATE /W4 /WX /wd4100)
target_link_libraries(xr_windows_test PRIVATE
  "${MEDIA_KIT_360_ANGLE}/lib/libEGL.dll.lib"
  "${MEDIA_KIT_360_ANGLE}/lib/libGLESv2.dll.lib"
  d3d11 dxgi)
