# Dawn patches

Applied by `scripts/build-dawn.sh` on top of the Dawn commit Aurora pins.

| Patch | Why |
|---|---|
| `0001-opengl-egl-wayland-surfaces.patch` | Dawn's OpenGL ES backend cannot present to a Wayland surface (`SwapChainEGL.cpp` only handles Android, Windows and X11 windows). webOS windows are Wayland surfaces, so the swap chain now wraps the `wl_surface` in a `wl_egl_window` (libwayland-egl, loaded at run time) sized to the swap chain and resized on reconfigure. The EGL display must be created on the same `wl_display`; the app passes it with `RequestAdapterOptionsGetGLProc`. |
| `0002-common-posix-memalign-for-old-glibc.patch` | `aligned_alloc` does not exist in glibc before 2.16; webOS TVs build against glibc 2.12. Uses `posix_memalign`, whose memory is also released with `free()`. |
