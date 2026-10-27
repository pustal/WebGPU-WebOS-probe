/**
 * @file Benchmark.h
 * @brief GPU workloads shaped like Aurora's rendering of a GameCube game:
 *        a shader compile test and a frame-time benchmark.
 *
 * The draw shader follows Aurora's generated GX shader (extern/aurora/lib/gx/
 * shader.cpp): vertices are read from storage buffers (or, where the vertex
 * stage has none, from r32uint textures) with byte-level
 * unpacking, per-draw data comes in as immediates, the fragment stage samples
 * two textures and runs three TEV stages with Aurora's 8-bit overflow
 * emulation, alpha test and fog. Each frame:
 *   1. streams that frame's vertex data to the GPU (Aurora re-uploads the
 *      game's vertices every frame),
 *   2. draws N meshes into an off-screen "EFB" colour + depth target,
 *   3. copies part of the EFB to a texture and draws it back (an EFB copy, as
 *      the game uses for effects),
 *   4. scales the EFB to the screen and overlays the report.
 */
#ifndef WGPUPROBE_BENCHMARK_H
#define WGPUPROBE_BENCHMARK_H

#include "GpuContext.h"
#include "Report.h"
#include "TextImage.h"

#include <webgpu/webgpu_cpp.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace probe
{

/** One benchmark configuration. */
struct Scenario {
    std::string name;
    std::uint32_t drawCount;   ///< meshes drawn per frame (48 triangles each)
    std::uint32_t uploadDraws; ///< meshes whose vertex data is re-uploaded per frame
    bool perDrawImmediates;    ///< set immediates before every draw (as Aurora does) or once
    std::uint32_t efbWidth;    ///< internal render resolution
    std::uint32_t efbHeight;
};

class Benchmark
{
public:
    /** Creates every resource the tests use. @return False (with a report line) on failure. */
    bool Initialize(GpuContext &context, Report &report);

    /** Times shader module + pipeline creation for several shader variants. */
    void RunShaderCompileTest(Report &report);

    /**
     * Runs @p scenario for a fixed number of frames, presenting each one, and
     * adds its average frame time to @p report. @p overlay is drawn on top of
     * every frame so the TV shows progress.
     */
    void RunScenario(const Scenario &scenario, Report &report, const TextImage &overlay);

    /** Presents one frame showing only @p overlay (and the last EFB contents behind it). */
    void ShowOverlay(const TextImage &overlay);

private:
    /** Off-screen target pair at one resolution. */
    struct Efb {
        std::uint32_t width  = 0;
        std::uint32_t height = 0;
        wgpu::Texture color;
        wgpu::TextureView colorView;
        wgpu::TextureView depthView;
        wgpu::BindGroup blitSource;   ///< samples color, for the final scale to the screen
    };

    wgpu::ShaderModule CreateShader(const std::string &wgsl);
    wgpu::RenderPipeline CreateDrawPipeline(float variant);
    wgpu::RenderPipeline CreateBlitPipeline(wgpu::TextureFormat format, bool blend);
    wgpu::Texture CreatePatternTexture(std::uint32_t size, std::uint32_t seed);
    /** r32uint texture of @p rows rows of vertex data (texture mode only). */
    wgpu::Texture CreateDataTexture(std::uint32_t rows);
    /** Uploads the first @p rows full rows of @p words into @p texture. */
    void WriteDataTexture(const wgpu::Texture &texture, const std::uint32_t *words, std::uint32_t rows);
    Efb &GetEfb(std::uint32_t width, std::uint32_t height);
    void UploadOverlay(const TextImage &overlay);

    /** What one frame does (see Scenario). */
    struct FrameWork {
        std::uint32_t drawCount   = 0;
        std::uint32_t uploadDraws = 0;
        bool perDrawImmediates    = true;
    };

    /** CPU time spent in each step of the measured frames, in ms. */
    struct FrameTimings {
        double upload  = 0.0; ///< WriteTexture / WriteBuffer of the vertex data
        double record  = 0.0; ///< command encoding in Dawn
        double submit  = 0.0; ///< Queue::Submit: Dawn's GL backend makes the GL calls here
        double present = 0.0; ///< Surface::Present: eglSwapBuffers, may wait for the GPU
    };

    /**
     * Records and submits one frame; adds the time of each step to @p timings if given.
     * @return False if no screen texture was available.
     */
    bool RenderFrame(const wgpu::RenderPipeline &drawPipeline, Efb &efb, const FrameWork &work,
                     FrameTimings *timings = nullptr);

    GpuContext *context_ = nullptr;
    wgpu::Device device_;

    wgpu::BindGroupLayout storageLayout_;
    wgpu::BindGroupLayout uniformLayout_;
    wgpu::BindGroupLayout textureLayout_;
    wgpu::BindGroupLayout blitLayout_;
    wgpu::PipelineLayout drawLayout_;
    wgpu::PipelineLayout blitPipelineLayout_;

    /** Vertex data in textures instead of storage buffers (see GpuContext::VertexStorageBuffers). */
    bool useTextures_ = false;
    wgpu::Texture vertexTexture_;
    wgpu::Texture matrixTexture_;
    wgpu::Buffer vertexBuffer_;
    wgpu::Buffer matrixBuffer_;
    wgpu::Buffer uniformBuffer_;
    std::vector<std::uint32_t> vertexWords_;   ///< CPU copy streamed every frame

    wgpu::Sampler sampler_;
    wgpu::BindGroup storageGroup_;
    wgpu::BindGroup uniformGroup_;
    wgpu::BindGroup textureGroup_;

    wgpu::RenderPipeline drawPipeline_;
    wgpu::RenderPipeline efbBlitPipeline_;     ///< EFB copy drawn back into the EFB
    wgpu::RenderPipeline screenBlitPipeline_;  ///< EFB scaled to the screen
    wgpu::RenderPipeline overlayPipeline_;     ///< report, alpha-blended on the screen

    wgpu::Texture efbCopy_;
    wgpu::BindGroup efbCopySource_;
    wgpu::Texture overlayTexture_;
    wgpu::BindGroup overlaySource_;

    std::vector<Efb> efbs_;
    Efb *lastEfb_ = nullptr;
};

} // namespace probe

#endif // WGPUPROBE_BENCHMARK_H
