#!/usr/bin/env bash
# build-webos.sh
#
# One-shot build of the probe's webOS IPK: builds Dawn first if needed
# (scripts/build-dawn.sh), then the probe with the webosbrew toolchain.
#
# Requirements:
#   - The webosbrew toolchain (arm-webos-linux-gnueabi_sdk-buildroot), unpacked
#     and relocated (run its relocate-sdk.sh once). Point WEBOS_SDK at it, e.g.
#       export WEBOS_SDK=$HOME/arm-webos-linux-gnueabi_sdk-buildroot
#     Releases: https://github.com/openlgtv/buildroot-nc4/releases
#     The toolchain is Linux x86_64 only; on macOS build inside a Linux VM or container.
#   - clang and lld 17+ (for Dawn), Python 3, git
#   - ares-package from @webos-tools/cli (npm install -g @webos-tools/cli)
#   - CMake 3.25+ and Ninja
#
# Usage: scripts/build-webos.sh [extra cmake args...]
#        DAWN_WORK=<dir> to keep Dawn's sources and build elsewhere.
# Output: dist/org.webgpuwebosprobe.app_<version>_arm.ipk

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${WEBOS_SDK:?Set WEBOS_SDK to the unpacked arm-webos-linux-gnueabi_sdk-buildroot directory}"
dawn_work="${DAWN_WORK:-${repo_root}/third_party/dawn-work}"

if [[ ! -f "${dawn_work}/build/src/dawn/native/libwebgpu_dawn.so" ]]; then
    "${repo_root}/scripts/build-dawn.sh" "${dawn_work}"
fi

toolchain_file="${WEBOS_SDK}/share/buildroot/toolchainfile.cmake"
cmake -S "${repo_root}" -B "${repo_root}/build-webos" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${toolchain_file}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DDAWN_SOURCE_DIR="${dawn_work}/src" \
    -DDAWN_BUILD_DIR="${dawn_work}/build" \
    "$@"
cmake --build "${repo_root}/build-webos" --parallel
cmake --build "${repo_root}/build-webos" --target webos-package
