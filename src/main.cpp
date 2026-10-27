/**
 * @file main.cpp
 * @brief WebGPU webOS probe: checks whether an LG webOS TV can run Aurora, the
 *        WebGPU (Dawn) renderer of the Luigi's Mansion PC port, through Dawn's
 *        OpenGL ES backend, and shows the answer on screen.
 *
 * Steps:
 *   1. Create a full-screen SDL window (no GL context: Dawn makes its own).
 *   2. Bring up Dawn on it with Aurora's limits and toggles (GpuContext).
 *   3. Time GX-style shader compiles and a set of frame benchmarks (Benchmark).
 *   4. Save the report as text and show it until Back.
 *
 * If WebGPU cannot be brought up at all, the report is shown with SDL's own
 * renderer instead.
 *
 * Keys: Back/Exit/Escape quit, OK/Enter runs the benchmarks again.
 */
#include "Benchmark.h"
#include "GpuContext.h"
#include "Report.h"
#include "TextImage.h"

#include <SDL.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace
{

/** Remote control scancodes from the SDL-webOS backport (SDL_SCANCODE_WEBOS_*). */
constexpr int kScancodeWebOSBack = 482;
constexpr int kScancodeWebOSExit = 505;

/** Size of the report image; it is stretched to the screen (2x on a 1080p panel). */
constexpr int kOverlayWidth  = 960;
constexpr int kOverlayHeight = 540;

const char *const kTitle = "WebGPU webOS probe - can this TV run Luigi's Mansion (Aurora)?";

/** Where the text report is written. */
std::string ReportPath()
{
    const char *home = std::getenv("HOME");
    return std::string(home && *home ? home : "/tmp") + "/webgpu-probe-report.txt";
}

void SaveReport(const probe::Report &report)
{
    const std::string text = report.ToText();
    std::printf("%s\n", text.c_str());
    std::ofstream out(ReportPath());
    if (out)
        out << kTitle << '\n' << text;
}

/** What a key press asks for. */
enum class KeyAction { None, Quit, Rerun };

KeyAction ActionFor(const SDL_Event &event)
{
    if (event.type == SDL_QUIT)
        return KeyAction::Quit;
    if (event.type == SDL_KEYDOWN) {
        const int scancode = event.key.keysym.scancode;
        if (scancode == kScancodeWebOSBack || scancode == kScancodeWebOSExit || scancode == SDL_SCANCODE_ESCAPE)
            return KeyAction::Quit;
        if (scancode == SDL_SCANCODE_RETURN || scancode == SDL_SCANCODE_KP_ENTER)
            return KeyAction::Rerun;
    }
    if (event.type == SDL_CONTROLLERBUTTONDOWN) {
        if (event.cbutton.button == SDL_CONTROLLER_BUTTON_B || event.cbutton.button == SDL_CONTROLLER_BUTTON_BACK)
            return KeyAction::Quit;
        if (event.cbutton.button == SDL_CONTROLLER_BUTTON_A)
            return KeyAction::Rerun;
    }
    return KeyAction::None;
}

/** Drains pending events. @return The strongest action asked for. */
KeyAction PollActions()
{
    KeyAction result = KeyAction::None;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_CONTROLLERDEVICEADDED)
            SDL_GameControllerOpen(event.cdevice.which);
        const KeyAction action = ActionFor(event);
        if (action == KeyAction::Quit)
            result = KeyAction::Quit;
        else if (action == KeyAction::Rerun && result == KeyAction::None)
            result = KeyAction::Rerun;
    }
    return result;
}

/** The benchmark sizes: Luigi's Mansion scenes are a few hundred to a few thousand draws. */
std::vector<probe::Scenario> Scenarios(std::uint32_t screenWidth, std::uint32_t screenHeight)
{
    return {
        { "300 draws, screen res", 300, screenWidth, screenHeight },
        { "1000 draws, screen res", 1000, screenWidth, screenHeight },
        { "2500 draws, screen res", 2500, screenWidth, screenHeight },
        { "1000 draws, 1280x720", 1000, 1280, 720 },
        { "1000 draws, 640x528 (GC)", 1000, 640, 528 },
    };
}

/** Runs every test, showing progress on screen; fills @p report. */
void RunTests(probe::GpuContext &gpu, probe::Benchmark &benchmark, probe::TextImage &image, probe::Report &report)
{
    report.Section("Shaders");
    image.Render(kTitle, report, "Compiling shaders...");
    benchmark.ShowOverlay(image);
    benchmark.RunShaderCompileTest(report);

    report.Section("Frame time (Luigi's Mansion runs at 30 fps: 33.3 ms)");
    for (const probe::Scenario &scenario : Scenarios(gpu.Width(), gpu.Height())) {
        image.Render(kTitle, report, "Running: " + scenario.name + "...");
        benchmark.RunScenario(scenario, report, image);
        if (gpu.HadDeviceError())
            break;
    }
}

/** Shows @p image with SDL's renderer until a quit key (used when WebGPU did not come up). */
void ShowWithSdlRenderer(SDL_Window *window, const probe::TextImage &image)
{
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, 0);
    if (!renderer)
        return;
    SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC,
                                             image.Width(), image.Height());
    if (texture) {
        SDL_UpdateTexture(texture, nullptr, image.Pixels().data(), image.Pitch());
        while (PollActions() != KeyAction::Quit) {
            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, texture, nullptr, nullptr);
            SDL_RenderPresent(renderer);
            SDL_Delay(16);
        }
        SDL_DestroyTexture(texture);
    }
    SDL_DestroyRenderer(renderer);
}

/**
 * Runs the probe on @p window until the player quits. The WebGPU objects live
 * only inside this function, so they are released before the window is.
 */
void RunProbe(SDL_Window *window)
{
    probe::Report report;
    probe::TextImage image(kOverlayWidth, kOverlayHeight);
    probe::GpuContext gpu;
    probe::Benchmark benchmark;

    const bool ready = gpu.Initialize(window, report) && benchmark.Initialize(gpu, report);
    if (!ready) {
        SaveReport(report);
        image.Render(kTitle, report, "WebGPU could not start. Report: " + ReportPath() + "   Back: quit");
        ShowWithSdlRenderer(window, image);
        return;
    }

    const std::size_t setupFindings = report.Findings().size();
    RunTests(gpu, benchmark, image, report);
    SaveReport(report);

    const std::string footer = "OK: run again   Back: quit   Report: " + ReportPath();
    image.Render(kTitle, report, footer);
    for (;;) {
        const KeyAction action = PollActions();
        if (action == KeyAction::Quit)
            break;
        if (action == KeyAction::Rerun) {
            // Keep the setup findings, replace the measurements.
            probe::Report rerun;
            for (std::size_t i = 0; i < setupFindings; ++i) {
                const probe::Finding &f = report.Findings()[i];
                if (f.label.empty())
                    rerun.Section(f.value);
                else
                    rerun.Add(f.status, f.label, f.value);
            }
            report = rerun;
            RunTests(gpu, benchmark, image, report);
            SaveReport(report);
            image.Render(kTitle, report, footer);
        }
        benchmark.ShowOverlay(image);
        SDL_Delay(30);
    }
}

} // namespace

int main(int /*argc*/, char * /*argv*/[])
{
    // Let the app receive Back and Exit instead of webOS closing it.
    SDL_SetHint("SDL_WEBOS_ACCESS_POLICY_KEYS_BACK", "true");
    SDL_SetHint("SDL_WEBOS_ACCESS_POLICY_KEYS_EXIT", "true");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return EXIT_FAILURE;
    }
    SDL_Window *window = SDL_CreateWindow("WebGPU webOS probe", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1920,
                                          1080, SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return EXIT_FAILURE;
    }

    RunProbe(window);

    SDL_DestroyWindow(window);
    SDL_Quit();
    return EXIT_SUCCESS;
}
