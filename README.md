# WebGPU webOS probe

Checks whether an LG webOS TV can run [Aurora](https://github.com/encounter/aurora),
the WebGPU renderer used by the
[Luigi's Mansion PC port](https://github.com/rob646464/luigis-mansion-recompiled),
and shows the answer on the TV. It is the first step of a possible webOS port,
like [ES3-WebOS-probe](https://github.com/pustal/ES3-WebOS-probe) was for
[Animal Crossing](https://github.com/pustal/Animal-Crossing-WebOS).

Aurora draws through [Dawn](https://dawn.googlesource.com/dawn). The port
uses Dawn's Vulkan backend on PCs, but webOS has no Vulkan, so the probe uses
Dawn's OpenGL ES backend, which needs OpenGL ES 3.1 (webOS 6 Mali drivers
expose 3.2).

## What it checks

| Section | Check |
|---|---|
| WebGPU | Dawn starts on the app's Wayland window, finds the GPU, has the limits Aurora asks for (two storage buffers in the vertex and fragment stages, 64 bytes of immediates), creates a device with Aurora's limits and toggles, and can present |
| Shaders | Time to compile a pipeline shaped like Aurora's generated GX shaders (WGSL, translated to GLSL ES by Dawn, then compiled by the TV's driver) |
| Frame time | Frames with 300 to 2500 draws in that shader style. Each frame streams its vertex data, draws into an off-screen EFB, makes an EFB copy and draws it back, then scales to the screen. Luigi's Mansion runs at 30 fps (33.3 ms per frame) |

If the vertex stage cannot read storage buffers (the webOS 6 Mali-G51 driver
reports 0, which Aurora does not support), the probe says so and runs the
benchmarks with vertex data in `r32uint` textures instead: the change a webOS
build of Aurora would need.

Each result is marked PASS, WARN or FAIL. The report is also written to
`webgpu-probe-report.txt` in the app's home folder.

Keys: **OK** runs the benchmarks again, **Back** quits.

## Results (LG webOS 6, Mali-G51, driver r9p0, probe 0.3.0)

- Dawn's OpenGL ES backend starts and presents on webOS (with the patches in
  `patches/`).
- **The driver allows no storage buffers in the vertex stage**, which Aurora
  requires; vertex data would have to come from textures (an Aurora patch).
- With that workaround, 1000 GX-style draws take about 72 ms per frame
  (30 fps needs 33 ms), at any resolution. Split per frame: about 50 ms in
  `Queue::Submit` (Dawn's GL backend making the GL calls, about 47 µs per
  draw), about 15 ms streaming vertex data, 5 ms of it from per-draw
  immediates; recording commands is about 1 ms.
- 300 draws run at 29 ms. A pipeline compiles in about 120 ms.

So the cost is in issuing GL draws on the TV's CPU, not in the GPU's pixel
work: Aurora through Dawn fits only scenes of a few hundred draws.

## Building

You need Linux x86_64 (or a Linux VM/container on macOS), the webosbrew
toolchain ([buildroot-nc4 releases](https://github.com/openlgtv/buildroot-nc4/releases),
run its `relocate-sdk.sh` once), clang and lld 17 or newer, CMake 3.25+,
Ninja, Python 3, git and `ares-package` (`npm install -g @webos-tools/cli`).

```sh
export WEBOS_SDK=$HOME/arm-webos-linux-gnueabi_sdk-buildroot
scripts/build-webos.sh
```

The first build downloads Dawn and its dependencies (about 1.4 GB) and
compiles it, which takes a while. The IPK and its Homebrew Channel manifest
land in `dist/`.

Dawn is built with clang (`cmake/WebOSClangToolchain.cmake`) because the
webosbrew GCC 12 cannot compile it. clang uses the SDK's sysroot and C++
library, so the result links with the GCC-built probe. Two small patches make
Dawn work on webOS; see [patches/README.md](patches/README.md).

## Licence

MIT (see [LICENSE](LICENSE)). See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)
for the bundled libraries.
