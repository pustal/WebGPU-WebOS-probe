/**
 * @file GpuContext.cpp
 * @brief Implementation of GpuContext (see GpuContext.h).
 */
#include "GpuContext.h"

#include <SDL.h>
#include <SDL_syswm.h>

#include <dawn/native/DawnNative.h>
#include <dawn/native/OpenGLBackend.h>

#include <dlfcn.h>

#include <array>
#include <cstdio>
#include <string_view>
#include <vector>

namespace probe
{
namespace
{

// ---- What Aurora asks for (extern/aurora/lib/webgpu/gpu.cpp, initialize()) ----

/** Storage buffers Aurora binds in the vertex and in the fragment stage (vertex data, TEV state). */
constexpr std::uint32_t kAuroraStorageBuffersPerStage = 2;
/** Aurora's minimum uniform buffer offset alignment. */
constexpr std::uint32_t kAuroraUniformAlignment = 64;
/** Aurora's minimum storage buffer offset alignment. */
constexpr std::uint32_t kAuroraStorageAlignment = 16;
/** Size of gx::DrawImmediateData: Aurora pushes per-draw data as immediates. */
constexpr std::uint32_t kAuroraImmediateSize = 64;

/** Device toggles Aurora enables in release builds on non-Windows platforms. */
constexpr std::array<const char *, 5> kAuroraEnabledToggles = {
    "skip_validation",
    "disable_robustness",
    "disable_symbol_renaming",
    "enable_immediate_error_handling",
    "gl_allow_context_on_multi_threads",
};
/** Device toggles Aurora disables. */
constexpr std::array<const char *, 2> kAuroraDisabledToggles = {
    "timestamp_quantization",
    "use_spirv_reconvergence_mode",
};

/** Converts a WebGPU string view (not necessarily NUL-terminated) to std::string. */
std::string ToString(wgpu::StringView view)
{
    if (view.data == nullptr)
        return std::string();
    if (view.length == WGPU_STRLEN)
        return std::string(view.data);
    return std::string(view.data, view.length);
}

/** Name of a feature for the report; only the ones Aurora cares about are spelled out. */
const char *FeatureName(wgpu::FeatureName feature)
{
    switch (feature) {
        case wgpu::FeatureName::CoreFeaturesAndLimits: return "CoreFeaturesAndLimits";
        case wgpu::FeatureName::TextureCompressionBC: return "TextureCompressionBC";
        case wgpu::FeatureName::TextureCompressionETC2: return "TextureCompressionETC2";
        case wgpu::FeatureName::TextureCompressionASTC: return "TextureCompressionASTC";
        case wgpu::FeatureName::TextureComponentSwizzle: return "TextureComponentSwizzle";
        case wgpu::FeatureName::DualSourceBlending: return "DualSourceBlending";
        case wgpu::FeatureName::Float32Filterable: return "Float32Filterable";
        case wgpu::FeatureName::DepthClipControl: return "DepthClipControl";
        default: return nullptr;
    }
}

/** EGL entry points the probe needs before Dawn takes over; loaded at run time. */
struct EglLoader {
    using GetProcAddressFn = dawn::native::opengl::EGLFunctionPointerType (*)(const char *);
    using GetDisplayFn     = void *(*)(void *nativeDisplay);

    GetProcAddressFn getProcAddress = nullptr;
    GetDisplayFn getDisplay         = nullptr;

    /** Opens the TV's libEGL. @return False if it or its entry points are missing. */
    bool Load()
    {
        void *lib = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
        if (lib == nullptr)
            lib = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
        if (lib == nullptr)
            return false;
        getProcAddress = reinterpret_cast<GetProcAddressFn>(dlsym(lib, "eglGetProcAddress"));
        getDisplay     = reinterpret_cast<GetDisplayFn>(dlsym(lib, "eglGetDisplay"));
        return getProcAddress != nullptr && getDisplay != nullptr;
    }
};

EglLoader g_egl;

} // namespace

bool GpuContext::Initialize(SDL_Window *window, Report &report)
{
    report.Section("WebGPU (Dawn, OpenGL ES backend)");
    return CreateInstance(report) && CreateSurface(window, report) && RequestAdapter(report) &&
           RequestDevice(report) && ConfigureSurface(report);
}

bool GpuContext::CreateInstance(Report &report)
{
    // Same instance setup as Aurora: TimedWaitAny for blocking waits, unsafe APIs allowed.
    const std::array<wgpu::InstanceFeatureName, 1> features = { wgpu::InstanceFeatureName::TimedWaitAny };
    const std::array<const char *, 1> toggles               = { "allow_unsafe_apis" };

    wgpu::DawnTogglesDescriptor togglesDescriptor;
    togglesDescriptor.enabledToggleCount = toggles.size();
    togglesDescriptor.enabledToggles     = toggles.data();

    wgpu::InstanceDescriptor descriptor;
    descriptor.nextInChain          = &togglesDescriptor;
    descriptor.requiredFeatureCount = features.size();
    descriptor.requiredFeatures     = features.data();

    instance_ = wgpu::CreateInstance(&descriptor);
    if (!instance_) {
        report.Add(Status::Fail, "Instance", "wgpu::CreateInstance failed");
        return false;
    }
    report.Add(Status::Pass, "Instance", "created");
    return true;
}

bool GpuContext::CreateSurface(SDL_Window *window, Report &report)
{
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_WAYLAND) {
        report.Add(Status::Fail, "Window", std::string("not a Wayland window: ") + SDL_GetError());
        return false;
    }
    wlDisplay_ = info.info.wl.display;
    wlSurface_ = info.info.wl.surface;

    int width  = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(window, &width, &height);
    width_  = static_cast<std::uint32_t>(width > 0 ? width : 1920);
    height_ = static_cast<std::uint32_t>(height > 0 ? height : 1080);
    report.Add(Status::Pass, "Window", "Wayland, " + std::to_string(width_) + "x" + std::to_string(height_));

    if (!g_egl.Load()) {
        report.Add(Status::Fail, "EGL", "libEGL could not be loaded");
        return false;
    }
    eglDisplay_ = g_egl.getDisplay(wlDisplay_);
    if (eglDisplay_ == nullptr) {
        report.Add(Status::Fail, "EGL", "eglGetDisplay(wl_display) returned no display");
        return false;
    }

    wgpu::SurfaceSourceWaylandSurface source;
    source.display = wlDisplay_;
    source.surface = wlSurface_;
    wgpu::SurfaceDescriptor descriptor;
    descriptor.nextInChain = &source;
    surface_               = instance_.CreateSurface(&descriptor);
    if (!surface_) {
        report.Add(Status::Fail, "Surface", "CreateSurface(Wayland) failed");
        return false;
    }
    report.Add(Status::Pass, "Surface", "Wayland surface wrapped");
    return true;
}

bool GpuContext::RequestAdapter(Report &report)
{
    // Point Dawn at the EGL display on SDL's Wayland connection.
    dawn::native::opengl::RequestAdapterOptionsGetGLProc glOptions;
    glOptions.getProc = g_egl.getProcAddress;
    glOptions.display = eglDisplay_;

    wgpu::RequestAdapterOptions options;
    options.nextInChain       = &glOptions;
    options.featureLevel      = wgpu::FeatureLevel::Compatibility;
    options.backendType       = wgpu::BackendType::OpenGLES;
    options.compatibleSurface = surface_;

    std::string message;
    const wgpu::Future future = instance_.RequestAdapter(
        &options, wgpu::CallbackMode::WaitAnyOnly,
        [this, &message](wgpu::RequestAdapterStatus status, wgpu::Adapter adapter, wgpu::StringView text) {
            if (status == wgpu::RequestAdapterStatus::Success)
                adapter_ = std::move(adapter);
            else
                message = ToString(text);
        });
    instance_.WaitAny(future, UINT64_MAX);

    if (!adapter_) {
        report.Add(Status::Fail, "Adapter", message.empty() ? "no OpenGL ES adapter" : message);
        return false;
    }
    ReportAdapter(report);
    return true;
}

void GpuContext::ReportAdapter(Report &report)
{
    wgpu::AdapterInfo info;
    adapter_.GetInfo(&info);
    report.Add(Status::Pass, "Adapter", ToString(info.vendor) + " " + ToString(info.device));
    report.Add(Status::Info, "Driver", ToString(info.description));

    wgpu::CompatibilityModeLimits compat;
    wgpu::Limits limits;
    limits.nextInChain = &compat;
    adapter_.GetLimits(&limits);

    const bool storageOk = compat.maxStorageBuffersInVertexStage >= kAuroraStorageBuffersPerStage &&
                           compat.maxStorageBuffersInFragmentStage >= kAuroraStorageBuffersPerStage &&
                           limits.maxStorageBuffersPerShaderStage >= kAuroraStorageBuffersPerStage;
    report.Add(storageOk ? Status::Pass : Status::Fail, "Storage buffers",
               "vertex " + std::to_string(compat.maxStorageBuffersInVertexStage) + ", fragment " +
                   std::to_string(compat.maxStorageBuffersInFragmentStage) + " (Aurora needs " +
                   std::to_string(kAuroraStorageBuffersPerStage) + ")");
    report.Add(limits.maxImmediateSize >= kAuroraImmediateSize ? Status::Pass : Status::Fail, "Immediates",
               std::to_string(limits.maxImmediateSize) + " bytes (Aurora needs " +
                   std::to_string(kAuroraImmediateSize) + ")");
    report.Add(Status::Info, "Textures",
               "2D up to " + std::to_string(limits.maxTextureDimension2D) + ", " +
                   std::to_string(limits.maxSampledTexturesPerShaderStage) + " per stage");

    wgpu::SupportedFeatures features;
    adapter_.GetFeatures(&features);
    std::string list;
    for (std::size_t i = 0; i < features.featureCount; ++i) {
        if (const char *name = FeatureName(features.features[i])) {
            if (!list.empty())
                list += ", ";
            list += name;
        }
    }
    report.Add(Status::Info, "Features", list.empty() ? "(none of interest)" : list);
}

bool GpuContext::RequestDevice(Report &report)
{
    wgpu::Limits supported;
    adapter_.GetLimits(&supported);

    // Aurora's required limits (see the constants above).
    wgpu::CompatibilityModeLimits compat;
    compat.maxStorageBuffersInVertexStage   = kAuroraStorageBuffersPerStage;
    compat.maxStorageBuffersInFragmentStage = kAuroraStorageBuffersPerStage;
    wgpu::Limits required;
    required.nextInChain                     = &compat;
    required.maxTextureDimension2D           = supported.maxTextureDimension2D;
    required.maxStorageBuffersPerShaderStage = kAuroraStorageBuffersPerStage;
    required.minUniformBufferOffsetAlignment =
        supported.minUniformBufferOffsetAlignment < kAuroraUniformAlignment ? kAuroraUniformAlignment
                                                                            : supported.minUniformBufferOffsetAlignment;
    required.minStorageBufferOffsetAlignment =
        supported.minStorageBufferOffsetAlignment < kAuroraStorageAlignment ? kAuroraStorageAlignment
                                                                            : supported.minStorageBufferOffsetAlignment;
    required.maxImmediateSize = kAuroraImmediateSize;

    wgpu::DawnTogglesDescriptor toggles;
    toggles.enabledToggleCount  = kAuroraEnabledToggles.size();
    toggles.enabledToggles      = kAuroraEnabledToggles.data();
    toggles.disabledToggleCount = kAuroraDisabledToggles.size();
    toggles.disabledToggles     = kAuroraDisabledToggles.data();

    wgpu::DeviceDescriptor descriptor;
    descriptor.nextInChain   = &toggles;
    descriptor.requiredLimits = &required;
    // Repeatable callbacks cannot capture, so the context comes in as userdata.
    descriptor.SetUncapturedErrorCallback(
        [](const wgpu::Device &, wgpu::ErrorType, wgpu::StringView text, GpuContext *self) {
            self->lastDeviceError_ = ToString(text);
            std::fprintf(stderr, "[WebGPU] device error: %s\n", self->lastDeviceError_.c_str());
        },
        this);
    descriptor.SetDeviceLostCallback(
        wgpu::CallbackMode::AllowSpontaneous,
        [](const wgpu::Device &, wgpu::DeviceLostReason reason, wgpu::StringView text) {
            if (reason != wgpu::DeviceLostReason::Destroyed)
                std::fprintf(stderr, "[WebGPU] device lost: %s\n", ToString(text).c_str());
        });

    std::string message;
    const wgpu::Future future = adapter_.RequestDevice(
        &descriptor, wgpu::CallbackMode::WaitAnyOnly,
        [this, &message](wgpu::RequestDeviceStatus status, wgpu::Device device, wgpu::StringView text) {
            if (status == wgpu::RequestDeviceStatus::Success)
                device_ = std::move(device);
            else
                message = ToString(text);
        });
    instance_.WaitAny(future, UINT64_MAX);

    if (!device_) {
        report.Add(Status::Fail, "Device", "with Aurora's limits: " + message);
        return false;
    }
    queue_ = device_.GetQueue();
    report.Add(Status::Pass, "Device", "created with Aurora's limits and toggles");
    return true;
}

bool GpuContext::ConfigureSurface(Report &report)
{
    wgpu::SurfaceCapabilities capabilities;
    if (surface_.GetCapabilities(adapter_, &capabilities) != wgpu::Status::Success || capabilities.formatCount == 0) {
        report.Add(Status::Fail, "Present", "surface reports no formats");
        return false;
    }
    surfaceFormat_ = capabilities.formats[0];

    // Prefer presenting without vsync so the benchmark measures the GPU, not the panel.
    wgpu::PresentMode presentMode = wgpu::PresentMode::Fifo;
    for (std::size_t i = 0; i < capabilities.presentModeCount; ++i) {
        if (capabilities.presentModes[i] == wgpu::PresentMode::Immediate)
            presentMode = wgpu::PresentMode::Immediate;
    }

    wgpu::SurfaceConfiguration configuration;
    configuration.device      = device_;
    configuration.format      = surfaceFormat_;
    configuration.usage       = wgpu::TextureUsage::RenderAttachment;
    configuration.width       = width_;
    configuration.height      = height_;
    configuration.alphaMode   = wgpu::CompositeAlphaMode::Auto;
    configuration.presentMode = presentMode;
    surface_.Configure(&configuration);

    // Acquire once to find out whether the EGL window surface could be created.
    wgpu::SurfaceTexture probeTexture;
    surface_.GetCurrentTexture(&probeTexture);
    const bool ok = probeTexture.status == wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal ||
                    probeTexture.status == wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal;
    if (!ok) {
        report.Add(Status::Fail, "Present",
                   "could not get a frame from the surface" +
                       (lastDeviceError_.empty() ? std::string() : ": " + lastDeviceError_));
        return false;
    }
    surface_.Present();
    report.Add(Status::Pass, "Present",
               std::string(presentMode == wgpu::PresentMode::Immediate ? "no vsync" : "vsync") + ", " +
                   std::to_string(width_) + "x" + std::to_string(height_));
    return true;
}

void GpuContext::ProcessEvents()
{
    instance_.ProcessEvents();
}

void GpuContext::WaitForIdle()
{
    const wgpu::Future future =
        queue_.OnSubmittedWorkDone(wgpu::CallbackMode::WaitAnyOnly, [](wgpu::QueueWorkDoneStatus, wgpu::StringView) {});
    instance_.WaitAny(future, UINT64_MAX);
}

wgpu::Texture GpuContext::AcquireFrame()
{
    wgpu::SurfaceTexture frame;
    surface_.GetCurrentTexture(&frame);
    if (frame.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal &&
        frame.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal)
        return nullptr;
    return frame.texture;
}

void GpuContext::Present()
{
    surface_.Present();
}

} // namespace probe
