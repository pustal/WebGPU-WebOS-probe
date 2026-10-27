/**
 * @file GpuContext.h
 * @brief Brings up WebGPU (Dawn's OpenGL ES backend) on an SDL2 window the way
 *        Aurora, the Luigi's Mansion port's renderer, would need it on a webOS TV.
 *
 * Steps, each recorded in the report:
 *   1. Take the Wayland display and surface from the SDL window.
 *   2. Create an EGL display on that same Wayland connection and hand it to Dawn
 *      (Dawn would otherwise open EGL_DEFAULT_DISPLAY, which the TV's driver
 *      does not tie to the app's window).
 *   3. Request an OpenGL ES adapter in compatibility mode, list its limits and
 *      features against what Aurora asks for.
 *   4. Create a device with Aurora's limits and toggles, and configure the
 *      window's surface for presenting.
 */
#ifndef WGPUPROBE_GPU_CONTEXT_H
#define WGPUPROBE_GPU_CONTEXT_H

#include "Report.h"

#include <webgpu/webgpu_cpp.h>

#include <cstdint>
#include <string>

struct SDL_Window;

namespace probe
{

class GpuContext
{
public:
    /**
     * Runs steps 1-4 for @p window.
     * @return True if a device and a configured surface are ready. On false the
     *         report says which step failed; the object must not be used to draw.
     */
    bool Initialize(SDL_Window *window, Report &report);

    /** Lets Dawn run callbacks and finish background work; call once per frame. */
    void ProcessEvents();

    /** Blocks until the GPU has finished all submitted work. */
    void WaitForIdle();

    /** Next texture to draw into for the window, or null if it could not be acquired. */
    wgpu::Texture AcquireFrame();

    /** Shows the frame acquired with AcquireFrame(). */
    void Present();

    const wgpu::Device &Device() const { return device_; }
    const wgpu::Queue &Queue() const { return queue_; }
    wgpu::TextureFormat SurfaceFormat() const { return surfaceFormat_; }
    std::uint32_t Width() const { return width_; }
    std::uint32_t Height() const { return height_; }

    /** True while an uncaptured device error has been seen (the last message is kept). */
    bool HadDeviceError() const { return !lastDeviceError_.empty(); }
    const std::string &LastDeviceError() const { return lastDeviceError_; }

private:
    bool CreateInstance(Report &report);
    bool CreateSurface(SDL_Window *window, Report &report);
    bool RequestAdapter(Report &report);
    void ReportAdapter(Report &report);
    bool RequestDevice(Report &report);
    bool ConfigureSurface(Report &report);

    wgpu::Instance instance_;
    wgpu::Surface surface_;
    wgpu::Adapter adapter_;
    wgpu::Device device_;
    wgpu::Queue queue_;
    wgpu::TextureFormat surfaceFormat_ = wgpu::TextureFormat::Undefined;
    std::uint32_t width_  = 0;
    std::uint32_t height_ = 0;

    void *wlDisplay_  = nullptr; ///< struct wl_display *, owned by SDL
    void *wlSurface_  = nullptr; ///< struct wl_surface *, owned by SDL
    void *eglDisplay_ = nullptr; ///< EGLDisplay on wlDisplay_

    std::string lastDeviceError_;
};

} // namespace probe

#endif // WGPUPROBE_GPU_CONTEXT_H
