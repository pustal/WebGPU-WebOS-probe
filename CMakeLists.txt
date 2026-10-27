# WebGPU webOS probe: checks whether an LG webOS TV can run Aurora, the WebGPU
# (Dawn) renderer of the Luigi's Mansion PC port, through Dawn's OpenGL ES
# backend, and shows the result on the TV.
#
#   1. scripts/build-dawn.sh        (Dawn for webOS, built with clang)
#   2. cmake -B build-webos -DCMAKE_TOOLCHAIN_FILE=<sdk>/share/buildroot/toolchainfile.cmake
#            -DDAWN_SOURCE_DIR=... -DDAWN_BUILD_DIR=...
#      cmake --build build-webos --target webos-package
# or scripts/build-webos.sh for both.

cmake_minimum_required(VERSION 3.21)
project(WebGPUWebOSProbe VERSION 0.3.0 LANGUAGES C CXX)

set(PROBE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
list(APPEND CMAKE_MODULE_PATH "${PROBE_ROOT}/cmake")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(PROBE_APP_ID "org.webgpuwebosprobe.app" CACHE STRING "webOS application id")

# ---- Dawn, built by scripts/build-dawn.sh ----
set(DAWN_SOURCE_DIR "" CACHE PATH "Dawn source tree (scripts/build-dawn.sh: <work>/src)")
set(DAWN_BUILD_DIR "" CACHE PATH "Dawn build tree (scripts/build-dawn.sh: <work>/build)")
set(PROBE_DAWN_LIBRARY "${DAWN_BUILD_DIR}/src/dawn/native/libwebgpu_dawn.so")
if(NOT EXISTS "${PROBE_DAWN_LIBRARY}" OR NOT EXISTS "${DAWN_SOURCE_DIR}/include/dawn/native/DawnNative.h")
    message(FATAL_ERROR "Dawn not found: run scripts/build-dawn.sh and pass DAWN_SOURCE_DIR / DAWN_BUILD_DIR")
endif()
add_library(Dawn::webgpu_dawn SHARED IMPORTED GLOBAL)
set_target_properties(Dawn::webgpu_dawn PROPERTIES
    IMPORTED_LOCATION "${PROBE_DAWN_LIBRARY}"
    IMPORTED_SONAME "libwebgpu_dawn.so"
    INTERFACE_INCLUDE_DIRECTORIES "${DAWN_SOURCE_DIR}/include;${DAWN_BUILD_DIR}/gen/include")

# ---- SDL2: the SDL-webOS backport (remote keys, Back delivery, Wayland window) ----
set(SWM_SDL2_BACKPORT_RELEASE "release-2.30.12-webos.6" CACHE STRING "SDL-webOS release to bundle")
include(SDL2WebOSBackport)
set(PROBE_SDL2_RUNTIME_LIBRARY "${SWM_SDL2_RUNTIME_LIBRARY}")

add_executable(wgpuprobe
    src/main.cpp
    src/Benchmark.cpp
    src/FontData.cpp
    src/GpuContext.cpp
    src/Report.cpp
    src/TextImage.cpp
)
# Dawn's C++ headers use C++20 designated initialisers.
target_compile_features(wgpuprobe PRIVATE cxx_std_20)
set_target_properties(wgpuprobe PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(wgpuprobe PRIVATE "${PROBE_ROOT}/src")
target_compile_options(wgpuprobe PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion>)
# EGL and libwayland-egl are opened at run time (dlopen), so only libdl is linked for them.
target_link_libraries(wgpuprobe PRIVATE Dawn::webgpu_dawn SDL2::SDL2 ${CMAKE_DL_LIBS})

# The TV's C++ runtime is older than the toolchain's; carry our own.
target_link_options(wgpuprobe PRIVATE -static-libstdc++ -static-libgcc)
set_target_properties(wgpuprobe PROPERTIES
    BUILD_WITH_INSTALL_RPATH ON
    INSTALL_RPATH "\$ORIGIN/../lib")

include(WebOSPackage)
