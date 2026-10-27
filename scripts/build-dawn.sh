#!/usr/bin/env bash
# build-dawn.sh
#
# Builds Dawn (WebGPU) for LG webOS TVs, as libwebgpu_dawn.so with only the
# OpenGL ES backend, for the probe to link against.
#
# - Source: the Dawn commit Aurora pins (encounter/dawn, AURORA_DAWN_REF in
#   extern/aurora/cmake/AuroraDependencyVersions.cmake), so the probe tests the
#   same Dawn the Luigi's Mansion port would ship.
# - Patches in patches/ are applied on top (see patches/README.md).
# - Compiled with the host's clang (cmake/WebOSClangToolchain.cmake): the
#   webosbrew GCC 12 cannot compile current Dawn.
#
# Requirements: WEBOS_SDK (as for build-webos.sh), clang and lld 17+, git,
# Python 3, CMake 3.25+, Ninja.
#
# Usage: scripts/build-dawn.sh [work directory, default: third_party/dawn-work]
# Output: <work>/build/src/dawn/native/libwebgpu_dawn.so and headers; prints
#         the cmake arguments that point the probe at them.

set -euo pipefail

readonly DAWN_REPO="https://github.com/encounter/dawn.git"
readonly DAWN_REF="1155e0ed531126f33a1279afa029349651ca1c93"

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work_dir="${1:-${repo_root}/third_party/dawn-work}"
: "${WEBOS_SDK:?Set WEBOS_SDK to the unpacked arm-webos-linux-gnueabi_sdk-buildroot directory}"

src_dir="${work_dir}/src"
build_dir="${work_dir}/build"
mkdir -p "${work_dir}"

if [[ ! -d "${src_dir}/.git" ]]; then
    git init -q "${src_dir}"
    git -C "${src_dir}" remote add origin "${DAWN_REPO}"
    git -C "${src_dir}" fetch -q --depth 1 origin "${DAWN_REF}"
    git -C "${src_dir}" checkout -q FETCH_HEAD
    for patch in "${repo_root}"/patches/*.patch; do
        echo "Applying $(basename "${patch}")"
        git -C "${src_dir}" apply "${patch}"
    done
    # Dawn's own dependency fetcher (a subset of gclient sync).
    (cd "${src_dir}" && python3 tools/fetch_dawn_dependencies.py --shallow)
fi

cmake -S "${src_dir}" -B "${build_dir}" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${repo_root}/cmake/WebOSClangToolchain.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DDAWN_BUILD_MONOLITHIC_LIBRARY=SHARED \
    -DDAWN_ENABLE_OPENGLES=ON -DDAWN_ENABLE_DESKTOP_GL=OFF -DDAWN_ENABLE_VULKAN=OFF \
    -DDAWN_ENABLE_NULL=OFF -DDAWN_ENABLE_D3D11=OFF -DDAWN_ENABLE_D3D12=OFF -DDAWN_ENABLE_METAL=OFF \
    -DDAWN_USE_WAYLAND=ON -DDAWN_USE_X11=OFF -DDAWN_USE_GLFW=OFF \
    -DDAWN_BUILD_SAMPLES=OFF -DDAWN_BUILD_TESTS=OFF -DDAWN_BUILD_PROTOBUF=OFF \
    -DDAWN_ENABLE_SPIRV_VALIDATION=OFF \
    -DDAWN_SUPPORTS_CXX_MODULES=OFF -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
    -DTINT_BUILD_TESTS=OFF -DTINT_BUILD_CMD_TOOLS=OFF -DTINT_BUILD_GLSL_VALIDATOR=OFF \
    -DTINT_BUILD_GLSL_WRITER=ON -DTINT_BUILD_SPV_READER=OFF -DTINT_BUILD_HLSL_WRITER=OFF \
    -DTINT_BUILD_IR_BINARY=OFF \
    -DTINT_BUILD_NULL_WRITER=ON  # the GL backend uses it even with the Null backend off
cmake --build "${build_dir}" --target webgpu_dawn --parallel

echo
echo "Dawn is ready. Configure the probe with:"
echo "  -DDAWN_SOURCE_DIR=${src_dir} -DDAWN_BUILD_DIR=${build_dir}"
