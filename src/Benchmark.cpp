/**
 * @file Benchmark.cpp
 * @brief Implementation of Benchmark (see Benchmark.h).
 */
#include "Benchmark.h"

#include <SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace probe
{
namespace
{

// ---- Workload sizes ----

constexpr std::uint32_t kTrianglesPerMesh  = 48;  ///< 4 x 6 quads
constexpr std::uint32_t kVerticesPerMesh   = kTrianglesPerMesh * 3;
constexpr std::uint32_t kWordsPerVertex    = 6;   ///< position xyz, colour rgba8, uv
constexpr std::uint32_t kBytesPerVertex    = kWordsPerVertex * 4;
constexpr std::uint32_t kMaxDraws          = 2500;
constexpr std::uint32_t kMatrixCount       = 256;
constexpr std::uint32_t kWordsPerMatrix    = 12;  ///< 3 x 4, row major
constexpr std::uint32_t kImmediateSize     = 64;  ///< Aurora's DrawImmediateData
constexpr std::uint32_t kEfbCopyWidth      = 640; ///< GameCube EFB copy size
constexpr std::uint32_t kEfbCopyHeight     = 480;
constexpr std::uint32_t kPatternSize       = 256;
/** Row width (in u32 words) of the vertex-data textures used without vertex storage buffers. */
constexpr std::uint32_t kDataTextureWidth  = 4096;
constexpr int kWarmupFrames                = 10;
constexpr int kMeasuredFrames              = 120;
constexpr wgpu::TextureFormat kEfbFormat   = wgpu::TextureFormat::RGBA8Unorm;
constexpr wgpu::TextureFormat kDepthFormat = wgpu::TextureFormat::Depth24Plus;

/** Frame time the game needs: it runs its logic at 30 updates per second. */
constexpr double kTargetFrameMs = 1000.0 / 30.0;

/** Per-draw data, laid out like Aurora's DrawImmediateData (64 bytes). */
struct DrawImmediates {
    std::uint32_t vtxStart;
    std::uint32_t currentPnMtx;
    std::uint32_t fogRangeBase;
    std::uint32_t pad;
    std::array<std::uint32_t, 12> arrayStart;
};
static_assert(sizeof(DrawImmediates) == kImmediateSize, "immediates must match Aurora's size");

/** Uniform block of the draw shader (std140-compatible layout). */
struct DrawUniforms {
    std::array<float, 16> projection;
    std::array<float, 4> konstColor;
    std::array<float, 4> fogColor;
    std::array<float, 4> fogParams;  ///< x: start, y: 1 / (end - start)
};

/**
 * Draw shader in the style of Aurora's generated GX shader. "{VARIANT}" is
 * replaced by a literal so that each variant is a distinct shader for the
 * compile test.
 */
constexpr const char *kDrawShader = R"(
struct Immediate {
    vtx_start: u32,
    current_pnmtx: u32,
    fog_range_base: u32,
    _pad: u32,
    array_start0: vec4u,
    array_start1: vec4u,
    array_start2: vec4u,
};
var<immediate> imm: Immediate;

struct Uniform {
    projection: mat4x4f,
    konst: vec4f,
    fog_color: vec4f,
    fog_params: vec4f,
};
{VERTEX_DATA}
@group(1) @binding(0) var<uniform> ubuf: Uniform;
@group(2) @binding(0) var tex0: texture_2d<f32>;
@group(2) @binding(1) var tex1: texture_2d<f32>;
@group(2) @binding(2) var samp: sampler;

const VARIANT: f32 = {VARIANT};

fn vbuf_f32(byte_off: u32) -> f32 { return bitcast<f32>(vbuf_word(byte_off >> 2u)); }
fn abuf_f32(byte_off: u32) -> f32 { return bitcast<f32>(abuf_word(byte_off >> 2u)); }
fn vbuf_u8_4(byte_off: u32) -> vec4u {
    let v = vbuf_word(byte_off >> 2u);
    return vec4u(v & 0xFFu, (v >> 8u) & 0xFFu, (v >> 16u) & 0xFFu, v >> 24u);
}
fn tev_overflow_vec4f(in: vec4f) -> vec4f {
    let byte_space = in * 255.0;
    return (byte_space - floor(byte_space / 256.0) * 256.0) / 255.0;
}

struct VertexOutput {
    @builtin(position) pos: vec4f,
    @location(0) color: vec4f,
    @location(1) uv0: vec2f,
    @location(2) uv1: vec2f,
    @location(3) fog_z: f32,
};

@vertex
fn vs_main(@builtin(vertex_index) vidx: u32) -> VertexOutput {
    let base = (imm.vtx_start + vidx) * 24u + imm.array_start0.x;
    let pos = vec4f(vbuf_f32(base), vbuf_f32(base + 4u), vbuf_f32(base + 8u), 1.0);
    let m = imm.current_pnmtx * 48u;
    let r0 = vec4f(abuf_f32(m), abuf_f32(m + 4u), abuf_f32(m + 8u), abuf_f32(m + 12u));
    let r1 = vec4f(abuf_f32(m + 16u), abuf_f32(m + 20u), abuf_f32(m + 24u), abuf_f32(m + 28u));
    let r2 = vec4f(abuf_f32(m + 32u), abuf_f32(m + 36u), abuf_f32(m + 40u), abuf_f32(m + 44u));
    let world = vec4f(dot(r0, pos), dot(r1, pos), dot(r2, pos), 1.0);

    var out: VertexOutput;
    out.pos = ubuf.projection * world;
    out.color = vec4f(vbuf_u8_4(base + 12u)) / 255.0;
    out.uv0 = vec2f(vbuf_f32(base + 16u), vbuf_f32(base + 20u));
    out.uv1 = out.uv0 * 2.0 + world.xy * (0.25 + VARIANT);
    out.fog_z = world.z;
    return out;
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let t0 = textureSample(tex0, samp, in.uv0);
    let t1 = textureSample(tex1, samp, in.uv1);
    // TEV stage 0: konst..tex0 by rasterised alpha.
    var prev = tev_overflow_vec4f(mix(ubuf.konst, t0, in.color.a));
    // TEV stage 1: modulate by the rasterised colour, scale 2.
    prev = clamp(prev * in.color * 2.0, vec4f(0.0), vec4f(1.0));
    // TEV stage 2: add tex1 with a -0.5 bias.
    prev = clamp(prev + (t1 - vec4f(0.5)) * (0.5 + VARIANT), vec4f(0.0), vec4f(1.0));
    // Alpha test.
    if (prev.a < 0.05) {
        discard;
    }
    // Linear fog.
    let fog = clamp((in.fog_z - ubuf.fog_params.x) * ubuf.fog_params.y, 0.0, 1.0);
    return vec4f(mix(prev.rgb, ubuf.fog_color.rgb, fog), prev.a);
}
)";

/** Vertex data as storage buffers: what Aurora does. */
constexpr const char *kVertexDataFromStorage = R"(
@group(0) @binding(0) var<storage, read> vbuf: array<u32>;
@group(0) @binding(1) var<storage, read> abuf: array<u32>;
fn vbuf_word(i: u32) -> u32 { return vbuf[i]; }
fn abuf_word(i: u32) -> u32 { return abuf[i]; }
)";

/**
 * Vertex data as r32uint textures, 4096 words per row: the workaround for GPUs
 * whose vertex stage cannot read storage buffers (Mali on webOS reports 0).
 */
constexpr const char *kVertexDataFromTextures = R"(
@group(0) @binding(0) var vtex: texture_2d<u32>;
@group(0) @binding(1) var atex: texture_2d<u32>;
fn vbuf_word(i: u32) -> u32 { return textureLoad(vtex, vec2u(i & 4095u, i >> 12u), 0).x; }
fn abuf_word(i: u32) -> u32 { return textureLoad(atex, vec2u(i & 4095u, i >> 12u), 0).x; }
)";

/** Full-screen triangle that copies a texture (used for the EFB copy, the scale-out and the overlay). */
constexpr const char *kBlitShader = R"(
@group(0) @binding(0) var src: texture_2d<f32>;
@group(0) @binding(1) var samp: sampler;

struct VertexOutput {
    @builtin(position) pos: vec4f,
    @location(0) uv: vec2f,
};

@vertex
fn vs_main(@builtin(vertex_index) vidx: u32) -> VertexOutput {
    let xy = vec2f(f32((vidx << 1u) & 2u), f32(vidx & 2u));
    var out: VertexOutput;
    out.pos = vec4f(xy * 2.0 - 1.0, 0.0, 1.0);
    out.uv = vec2f(xy.x, 1.0 - xy.y);
    return out;
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    return textureSample(src, samp, in.uv);
}
)";

/** Milliseconds since an arbitrary start. */
double NowMs()
{
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch()).count();
}

/** Bit pattern of a float, for packing into the u32 storage buffers. */
std::uint32_t FloatBits(float value)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

std::string FormatMs(double ms)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.1f ms", ms);
    return buffer;
}

wgpu::Buffer CreateBuffer(const wgpu::Device &device, std::uint64_t size, wgpu::BufferUsage usage)
{
    wgpu::BufferDescriptor descriptor;
    descriptor.size  = size;
    descriptor.usage = usage | wgpu::BufferUsage::CopyDst;
    return device.CreateBuffer(&descriptor);
}

wgpu::TextureView DefaultView(const wgpu::Texture &texture)
{
    return texture.CreateView();
}

} // namespace

wgpu::ShaderModule Benchmark::CreateShader(const std::string &wgsl)
{
    wgpu::ShaderSourceWGSL source;
    source.code = wgsl.c_str();
    wgpu::ShaderModuleDescriptor descriptor;
    descriptor.nextInChain = &source;
    return device_.CreateShaderModule(&descriptor);
}

wgpu::Texture Benchmark::CreatePatternTexture(std::uint32_t size, std::uint32_t seed)
{
    wgpu::TextureDescriptor descriptor;
    descriptor.size   = { size, size, 1 };
    descriptor.format = wgpu::TextureFormat::RGBA8Unorm;
    descriptor.usage  = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    wgpu::Texture texture = device_.CreateTexture(&descriptor);

    // A checker with a gradient, so sampling is not trivially cacheable.
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(size) * size * 4);
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            std::uint8_t *p   = &pixels[(static_cast<std::size_t>(y) * size + x) * 4];
            const bool checker = (((x + seed) / 16) ^ (y / 16)) & 1u;
            p[0] = static_cast<std::uint8_t>(checker ? 230 : (x * 255 / size));
            p[1] = static_cast<std::uint8_t>((y * 255 / size) ^ (seed * 40));
            p[2] = static_cast<std::uint8_t>(checker ? 90 : 200);
            p[3] = static_cast<std::uint8_t>(checker ? 255 : 160);
        }
    }
    wgpu::TexelCopyTextureInfo destination;
    destination.texture = texture;
    wgpu::TexelCopyBufferLayout layout;
    layout.bytesPerRow  = size * 4;
    layout.rowsPerImage = size;
    const wgpu::Extent3D extent = { size, size, 1 };
    context_->Queue().WriteTexture(&destination, pixels.data(), pixels.size(), &layout, &extent);
    return texture;
}

wgpu::RenderPipeline Benchmark::CreateDrawPipeline(float variant)
{
    std::string wgsl(kDrawShader);
    char literal[32];
    std::snprintf(literal, sizeof(literal), "%.6f", static_cast<double>(variant));
    const std::string placeholder = "{VARIANT}";
    wgsl.replace(wgsl.find(placeholder), placeholder.size(), literal);
    const std::string dataPlaceholder = "{VERTEX_DATA}";
    wgsl.replace(wgsl.find(dataPlaceholder), dataPlaceholder.size(),
                 useTextures_ ? kVertexDataFromTextures : kVertexDataFromStorage);
    const wgpu::ShaderModule module = CreateShader(wgsl);

    wgpu::BlendState blend;
    blend.color = { wgpu::BlendOperation::Add, wgpu::BlendFactor::SrcAlpha, wgpu::BlendFactor::OneMinusSrcAlpha };
    blend.alpha = { wgpu::BlendOperation::Add, wgpu::BlendFactor::One, wgpu::BlendFactor::OneMinusSrcAlpha };
    wgpu::ColorTargetState target;
    target.format = kEfbFormat;
    target.blend  = &blend;

    wgpu::FragmentState fragment;
    fragment.module      = module;
    fragment.entryPoint  = "fs_main";
    fragment.targetCount = 1;
    fragment.targets     = &target;

    wgpu::DepthStencilState depth;
    depth.format            = kDepthFormat;
    depth.depthWriteEnabled = wgpu::OptionalBool::True;
    depth.depthCompare      = wgpu::CompareFunction::LessEqual;

    wgpu::RenderPipelineDescriptor descriptor;
    descriptor.layout             = drawLayout_;
    descriptor.vertex.module      = module;
    descriptor.vertex.entryPoint  = "vs_main";
    descriptor.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
    descriptor.primitive.cullMode = wgpu::CullMode::None;
    descriptor.depthStencil       = &depth;
    descriptor.fragment           = &fragment;
    return device_.CreateRenderPipeline(&descriptor);
}

wgpu::RenderPipeline Benchmark::CreateBlitPipeline(wgpu::TextureFormat format, bool blend)
{
    const wgpu::ShaderModule module = CreateShader(kBlitShader);

    wgpu::BlendState alphaBlend;
    alphaBlend.color = { wgpu::BlendOperation::Add, wgpu::BlendFactor::SrcAlpha, wgpu::BlendFactor::OneMinusSrcAlpha };
    alphaBlend.alpha = { wgpu::BlendOperation::Add, wgpu::BlendFactor::One, wgpu::BlendFactor::OneMinusSrcAlpha };
    wgpu::ColorTargetState target;
    target.format = format;
    target.blend  = blend ? &alphaBlend : nullptr;

    wgpu::FragmentState fragment;
    fragment.module      = module;
    fragment.entryPoint  = "fs_main";
    fragment.targetCount = 1;
    fragment.targets     = &target;

    wgpu::RenderPipelineDescriptor descriptor;
    descriptor.layout            = blitPipelineLayout_;
    descriptor.vertex.module     = module;
    descriptor.vertex.entryPoint = "vs_main";
    descriptor.fragment          = &fragment;
    return device_.CreateRenderPipeline(&descriptor);
}

bool Benchmark::Initialize(GpuContext &context, Report &report)
{
    context_     = &context;
    device_      = context.Device();
    useTextures_ = !context.VertexStorageBuffers();

    // ---- Layouts, as Aurora binds them: storage (vertex data), uniforms, textures ----
    std::array<wgpu::BindGroupLayoutEntry, 2> storageEntries;
    for (std::uint32_t i = 0; i < storageEntries.size(); ++i) {
        storageEntries[i].binding     = i;
        storageEntries[i].visibility  = wgpu::ShaderStage::Vertex;
        if (useTextures_)
            storageEntries[i].texture.sampleType = wgpu::TextureSampleType::Uint;
        else
            storageEntries[i].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
    }
    wgpu::BindGroupLayoutDescriptor storageDescriptor;
    storageDescriptor.entryCount = storageEntries.size();
    storageDescriptor.entries    = storageEntries.data();
    storageLayout_               = device_.CreateBindGroupLayout(&storageDescriptor);

    wgpu::BindGroupLayoutEntry uniformEntry;
    uniformEntry.binding     = 0;
    uniformEntry.visibility  = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
    uniformEntry.buffer.type = wgpu::BufferBindingType::Uniform;
    wgpu::BindGroupLayoutDescriptor uniformDescriptor;
    uniformDescriptor.entryCount = 1;
    uniformDescriptor.entries    = &uniformEntry;
    uniformLayout_               = device_.CreateBindGroupLayout(&uniformDescriptor);

    std::array<wgpu::BindGroupLayoutEntry, 3> textureEntries;
    for (std::uint32_t i = 0; i < 2; ++i) {
        textureEntries[i].binding            = i;
        textureEntries[i].visibility         = wgpu::ShaderStage::Fragment;
        textureEntries[i].texture.sampleType = wgpu::TextureSampleType::Float;
    }
    textureEntries[2].binding      = 2;
    textureEntries[2].visibility   = wgpu::ShaderStage::Fragment;
    textureEntries[2].sampler.type = wgpu::SamplerBindingType::Filtering;
    wgpu::BindGroupLayoutDescriptor textureDescriptor;
    textureDescriptor.entryCount = textureEntries.size();
    textureDescriptor.entries    = textureEntries.data();
    textureLayout_               = device_.CreateBindGroupLayout(&textureDescriptor);

    const std::array<wgpu::BindGroupLayout, 3> drawGroups = { storageLayout_, uniformLayout_, textureLayout_ };
    wgpu::PipelineLayoutDescriptor drawLayoutDescriptor;
    drawLayoutDescriptor.bindGroupLayoutCount = drawGroups.size();
    drawLayoutDescriptor.bindGroupLayouts     = drawGroups.data();
    drawLayoutDescriptor.immediateSize        = kImmediateSize;
    drawLayout_                               = device_.CreatePipelineLayout(&drawLayoutDescriptor);

    std::array<wgpu::BindGroupLayoutEntry, 2> blitEntries;
    blitEntries[0].binding            = 0;
    blitEntries[0].visibility         = wgpu::ShaderStage::Fragment;
    blitEntries[0].texture.sampleType = wgpu::TextureSampleType::Float;
    blitEntries[1].binding            = 1;
    blitEntries[1].visibility         = wgpu::ShaderStage::Fragment;
    blitEntries[1].sampler.type       = wgpu::SamplerBindingType::Filtering;
    wgpu::BindGroupLayoutDescriptor blitDescriptor;
    blitDescriptor.entryCount = blitEntries.size();
    blitDescriptor.entries    = blitEntries.data();
    blitLayout_               = device_.CreateBindGroupLayout(&blitDescriptor);
    wgpu::PipelineLayoutDescriptor blitLayoutDescriptor;
    blitLayoutDescriptor.bindGroupLayoutCount = 1;
    blitLayoutDescriptor.bindGroupLayouts     = &blitLayout_;
    blitPipelineLayout_                       = device_.CreatePipelineLayout(&blitLayoutDescriptor);

    // ---- Geometry: one small 4 x 6 quad grid per draw, each with its own vertices ----
    const std::size_t vertexWordCount = static_cast<std::size_t>(kMaxDraws) * kVerticesPerMesh * kWordsPerVertex;
    // Whole texture rows, so every upload in texture mode is a full-row copy.
    vertexWords_.resize((vertexWordCount + kDataTextureWidth - 1) / kDataTextureWidth * kDataTextureWidth);
    std::size_t word = 0;
    for (std::uint32_t mesh = 0; mesh < kMaxDraws; ++mesh) {
        const float z = 0.1f + 0.8f * static_cast<float>(mesh % 97) / 97.0f;
        for (std::uint32_t quad = 0; quad < kTrianglesPerMesh / 2; ++quad) {
            const float qx = static_cast<float>(quad % 4) * 0.02f;
            const float qy = static_cast<float>(quad / 4) * 0.0134f;
            const std::array<std::array<float, 2>, 6> corners = { {
                { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 0 }, { 1, 1 }, { 0, 1 },
            } };
            for (const auto &corner : corners) {
                // RGBA8 (little-endian): a per-mesh colour with alpha 0xC0.
                const std::uint32_t colour = 0xC0000000u | ((mesh * 2654435761u) & 0x00FFFFFFu);
                vertexWords_[word++] = FloatBits(qx + corner[0] * 0.02f);
                vertexWords_[word++] = FloatBits(qy + corner[1] * 0.0134f);
                vertexWords_[word++] = FloatBits(z);
                vertexWords_[word++] = colour;
                vertexWords_[word++] = FloatBits(corner[0] + static_cast<float>(quad % 4));
                vertexWords_[word++] = FloatBits(corner[1] + static_cast<float>(quad / 4));
            }
        }
    }

    // Position matrices: translations spread over the screen.
    std::vector<std::uint32_t> matrices(kDataTextureWidth); // one texture row; 256 matrices use 3072 words
    static_assert(kMatrixCount * kWordsPerMatrix <= kDataTextureWidth, "matrices must fit one row");
    for (std::uint32_t m = 0; m < kMatrixCount; ++m) {
        const float tx = -0.95f + 1.85f * static_cast<float>(m % 16) / 15.0f;
        const float ty = -0.95f + 1.85f * static_cast<float>(m / 16) / 15.0f;
        const std::array<float, kWordsPerMatrix> rows = {
            1, 0, 0, tx,
            0, 1, 0, ty,
            0, 0, 1, 0,
        };
        for (std::uint32_t i = 0; i < kWordsPerMatrix; ++i)
            matrices[static_cast<std::size_t>(m) * kWordsPerMatrix + i] = FloatBits(rows[i]);
    }
    if (useTextures_) {
        vertexTexture_ = CreateDataTexture(static_cast<std::uint32_t>(vertexWords_.size() / kDataTextureWidth));
        matrixTexture_ = CreateDataTexture(1);
        WriteDataTexture(matrixTexture_, matrices.data(), 1);
    } else {
        vertexBuffer_ = CreateBuffer(device_, vertexWords_.size() * 4, wgpu::BufferUsage::Storage);
        matrixBuffer_ = CreateBuffer(device_, matrices.size() * 4, wgpu::BufferUsage::Storage);
        context.Queue().WriteBuffer(matrixBuffer_, 0, matrices.data(), matrices.size() * 4);
    }

    const DrawUniforms uniforms = {
        { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 },
        { 0.9f, 0.6f, 0.3f, 1.0f },
        { 0.2f, 0.25f, 0.35f, 1.0f },
        { 0.5f, 1.0f / 0.4f, 0.0f, 0.0f },
    };
    uniformBuffer_ = CreateBuffer(device_, sizeof(DrawUniforms), wgpu::BufferUsage::Uniform);
    context.Queue().WriteBuffer(uniformBuffer_, 0, &uniforms, sizeof(uniforms));

    wgpu::SamplerDescriptor samplerDescriptor;
    samplerDescriptor.addressModeU = wgpu::AddressMode::Repeat;
    samplerDescriptor.addressModeV = wgpu::AddressMode::Repeat;
    samplerDescriptor.magFilter    = wgpu::FilterMode::Linear;
    samplerDescriptor.minFilter    = wgpu::FilterMode::Linear;
    sampler_                       = device_.CreateSampler(&samplerDescriptor);

    // ---- Bind groups ----
    std::array<wgpu::BindGroupEntry, 2> storageBindings;
    storageBindings[0].binding = 0;
    storageBindings[1].binding = 1;
    if (useTextures_) {
        storageBindings[0].textureView = DefaultView(vertexTexture_);
        storageBindings[1].textureView = DefaultView(matrixTexture_);
    } else {
        storageBindings[0].buffer = vertexBuffer_;
        storageBindings[1].buffer = matrixBuffer_;
    }
    wgpu::BindGroupDescriptor storageGroupDescriptor;
    storageGroupDescriptor.layout     = storageLayout_;
    storageGroupDescriptor.entryCount = storageBindings.size();
    storageGroupDescriptor.entries    = storageBindings.data();
    storageGroup_                     = device_.CreateBindGroup(&storageGroupDescriptor);

    wgpu::BindGroupEntry uniformBinding;
    uniformBinding.binding = 0;
    uniformBinding.buffer  = uniformBuffer_;
    wgpu::BindGroupDescriptor uniformGroupDescriptor;
    uniformGroupDescriptor.layout     = uniformLayout_;
    uniformGroupDescriptor.entryCount = 1;
    uniformGroupDescriptor.entries    = &uniformBinding;
    uniformGroup_                     = device_.CreateBindGroup(&uniformGroupDescriptor);

    const wgpu::Texture pattern0 = CreatePatternTexture(kPatternSize, 0);
    const wgpu::Texture pattern1 = CreatePatternTexture(kPatternSize, 3);
    std::array<wgpu::BindGroupEntry, 3> textureBindings;
    textureBindings[0].binding     = 0;
    textureBindings[0].textureView = DefaultView(pattern0);
    textureBindings[1].binding     = 1;
    textureBindings[1].textureView = DefaultView(pattern1);
    textureBindings[2].binding     = 2;
    textureBindings[2].sampler     = sampler_;
    wgpu::BindGroupDescriptor textureGroupDescriptor;
    textureGroupDescriptor.layout     = textureLayout_;
    textureGroupDescriptor.entryCount = textureBindings.size();
    textureGroupDescriptor.entries    = textureBindings.data();
    textureGroup_                     = device_.CreateBindGroup(&textureGroupDescriptor);

    // EFB copy target, sampled back into the EFB every frame.
    wgpu::TextureDescriptor copyDescriptor;
    copyDescriptor.size   = { kEfbCopyWidth, kEfbCopyHeight, 1 };
    copyDescriptor.format = kEfbFormat;
    copyDescriptor.usage  = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    efbCopy_              = device_.CreateTexture(&copyDescriptor);

    auto makeBlitSource = [this](const wgpu::Texture &texture) {
        std::array<wgpu::BindGroupEntry, 2> entries;
        entries[0].binding     = 0;
        entries[0].textureView = DefaultView(texture);
        entries[1].binding     = 1;
        entries[1].sampler     = sampler_;
        wgpu::BindGroupDescriptor descriptor;
        descriptor.layout     = blitLayout_;
        descriptor.entryCount = entries.size();
        descriptor.entries    = entries.data();
        return device_.CreateBindGroup(&descriptor);
    };
    efbCopySource_ = makeBlitSource(efbCopy_);

    // ---- Pipelines ----
    drawPipeline_       = CreateDrawPipeline(0.0f);
    efbBlitPipeline_    = CreateBlitPipeline(kEfbFormat, true);
    screenBlitPipeline_ = CreateBlitPipeline(context.SurfaceFormat(), false);
    overlayPipeline_    = CreateBlitPipeline(context.SurfaceFormat(), true);

    context.WaitForIdle();
    if (context.HadDeviceError()) {
        report.Add(Status::Fail, "Resources", context.LastDeviceError());
        return false;
    }
    report.Add(Status::Pass, "Resources",
               std::string(useTextures_ ? "texture" : "storage-buffer") + " vertex pulling, immediates, EFB copy");
    return true;
}

wgpu::Texture Benchmark::CreateDataTexture(std::uint32_t rows)
{
    wgpu::TextureDescriptor descriptor;
    descriptor.size   = { kDataTextureWidth, rows, 1 };
    descriptor.format = wgpu::TextureFormat::R32Uint;
    descriptor.usage  = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    return device_.CreateTexture(&descriptor);
}

void Benchmark::WriteDataTexture(const wgpu::Texture &texture, const std::uint32_t *words, std::uint32_t rows)
{
    wgpu::TexelCopyTextureInfo destination;
    destination.texture = texture;
    wgpu::TexelCopyBufferLayout layout;
    layout.bytesPerRow          = kDataTextureWidth * 4;
    layout.rowsPerImage         = rows;
    const wgpu::Extent3D extent = { kDataTextureWidth, rows, 1 };
    context_->Queue().WriteTexture(&destination, words, static_cast<std::size_t>(rows) * kDataTextureWidth * 4,
                                   &layout, &extent);
}

Benchmark::Efb &Benchmark::GetEfb(std::uint32_t width, std::uint32_t height)
{
    for (Efb &efb : efbs_) {
        if (efb.width == width && efb.height == height)
            return efb;
    }
    Efb efb;
    efb.width  = width;
    efb.height = height;

    wgpu::TextureDescriptor colorDescriptor;
    colorDescriptor.size   = { width, height, 1 };
    colorDescriptor.format = kEfbFormat;
    colorDescriptor.usage  = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding |
                            wgpu::TextureUsage::CopySrc;
    efb.color     = device_.CreateTexture(&colorDescriptor);
    efb.colorView = DefaultView(efb.color);

    wgpu::TextureDescriptor depthDescriptor;
    depthDescriptor.size   = { width, height, 1 };
    depthDescriptor.format = kDepthFormat;
    depthDescriptor.usage  = wgpu::TextureUsage::RenderAttachment;
    efb.depthView          = DefaultView(device_.CreateTexture(&depthDescriptor));

    std::array<wgpu::BindGroupEntry, 2> entries;
    entries[0].binding     = 0;
    entries[0].textureView = efb.colorView;
    entries[1].binding     = 1;
    entries[1].sampler     = sampler_;
    wgpu::BindGroupDescriptor descriptor;
    descriptor.layout     = blitLayout_;
    descriptor.entryCount = entries.size();
    descriptor.entries    = entries.data();
    efb.blitSource        = device_.CreateBindGroup(&descriptor);

    efbs_.push_back(std::move(efb));
    return efbs_.back();
}

void Benchmark::UploadOverlay(const TextImage &overlay)
{
    const auto width  = static_cast<std::uint32_t>(overlay.Width());
    const auto height = static_cast<std::uint32_t>(overlay.Height());
    if (!overlayTexture_) {
        wgpu::TextureDescriptor descriptor;
        descriptor.size   = { width, height, 1 };
        descriptor.format = wgpu::TextureFormat::RGBA8Unorm;
        descriptor.usage  = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
        overlayTexture_   = device_.CreateTexture(&descriptor);

        std::array<wgpu::BindGroupEntry, 2> entries;
        entries[0].binding     = 0;
        entries[0].textureView = DefaultView(overlayTexture_);
        entries[1].binding     = 1;
        entries[1].sampler     = sampler_;
        wgpu::BindGroupDescriptor groupDescriptor;
        groupDescriptor.layout     = blitLayout_;
        groupDescriptor.entryCount = entries.size();
        groupDescriptor.entries    = entries.data();
        overlaySource_             = device_.CreateBindGroup(&groupDescriptor);
    }
    wgpu::TexelCopyTextureInfo destination;
    destination.texture = overlayTexture_;
    wgpu::TexelCopyBufferLayout layout;
    layout.bytesPerRow  = static_cast<std::uint32_t>(overlay.Pitch());
    layout.rowsPerImage = height;
    const wgpu::Extent3D extent = { width, height, 1 };
    context_->Queue().WriteTexture(&destination, overlay.Pixels().data(), overlay.Pixels().size(), &layout, &extent);
}

bool Benchmark::RenderFrame(const wgpu::RenderPipeline &drawPipeline, Efb &efb, const FrameWork &work,
                            FrameTimings *timings)
{
    const wgpu::Queue &queue    = context_->Queue();
    const std::uint32_t drawCount = work.drawCount;
    double mark                 = NowMs();
    // Adds the time since the last mark to one of the timing buckets.
    auto lap = [&mark, timings](double FrameTimings::*bucket) {
        const double now = NowMs();
        if (timings)
            timings->*bucket += now - mark;
        mark = now;
    };

    // 1. Stream this frame's vertices, as Aurora does with the game's vertex data.
    if (work.uploadDraws > 0) {
        const std::size_t bytes = static_cast<std::size_t>(work.uploadDraws) * kVerticesPerMesh * kBytesPerVertex;
        if (useTextures_) {
            const std::size_t rowBytes = static_cast<std::size_t>(kDataTextureWidth) * 4;
            WriteDataTexture(vertexTexture_, vertexWords_.data(), static_cast<std::uint32_t>((bytes + rowBytes - 1) / rowBytes));
        } else {
            queue.WriteBuffer(vertexBuffer_, 0, vertexWords_.data(), bytes);
        }
    }
    lap(&FrameTimings::upload);

    const wgpu::CommandEncoder encoder = device_.CreateCommandEncoder();

    // 2. The game's draws into the EFB.
    {
        wgpu::RenderPassColorAttachment color;
        color.view       = efb.colorView;
        color.loadOp     = wgpu::LoadOp::Clear;
        color.storeOp    = wgpu::StoreOp::Store;
        color.clearValue = { 0.1, 0.12, 0.2, 1.0 };
        wgpu::RenderPassDepthStencilAttachment depth;
        depth.view            = efb.depthView;
        depth.depthLoadOp     = wgpu::LoadOp::Clear;
        depth.depthStoreOp    = wgpu::StoreOp::Discard;
        depth.depthClearValue = 1.0f;
        wgpu::RenderPassDescriptor pass;
        pass.colorAttachmentCount   = 1;
        pass.colorAttachments       = &color;
        pass.depthStencilAttachment = &depth;

        const wgpu::RenderPassEncoder encoderPass = encoder.BeginRenderPass(&pass);
        if (drawCount > 0) {
            encoderPass.SetPipeline(drawPipeline);
            encoderPass.SetBindGroup(0, storageGroup_);
            encoderPass.SetBindGroup(1, uniformGroup_);
            encoderPass.SetBindGroup(2, textureGroup_);
            DrawImmediates immediates = {};
            if (!work.perDrawImmediates)
                encoderPass.SetImmediates(0, &immediates, sizeof(immediates));
            for (std::uint32_t i = 0; i < drawCount; ++i) {
                if (work.perDrawImmediates) {
                    immediates.vtxStart     = i * kVerticesPerMesh;
                    immediates.currentPnMtx = (i * 37u) % kMatrixCount;
                    encoderPass.SetImmediates(0, &immediates, sizeof(immediates));
                    encoderPass.Draw(kVerticesPerMesh);
                } else {
                    // Same meshes, positioned through the draw's first vertex instead.
                    encoderPass.Draw(kVerticesPerMesh, 1, i * kVerticesPerMesh);
                }
            }
        }
        encoderPass.End();
    }

    // 3. EFB copy, then draw it back into a corner of the EFB.
    if (drawCount > 0) {
        wgpu::TexelCopyTextureInfo source;
        source.texture = efb.color;
        wgpu::TexelCopyTextureInfo destination;
        destination.texture = efbCopy_;
        const wgpu::Extent3D extent = { std::min(kEfbCopyWidth, efb.width), std::min(kEfbCopyHeight, efb.height), 1 };
        encoder.CopyTextureToTexture(&source, &destination, &extent);

        wgpu::RenderPassColorAttachment color;
        color.view    = efb.colorView;
        color.loadOp  = wgpu::LoadOp::Load;
        color.storeOp = wgpu::StoreOp::Store;
        wgpu::RenderPassDescriptor pass;
        pass.colorAttachmentCount = 1;
        pass.colorAttachments     = &color;
        const wgpu::RenderPassEncoder encoderPass = encoder.BeginRenderPass(&pass);
        encoderPass.SetViewport(0.0f, 0.0f, static_cast<float>(efb.width) / 4.0f,
                                static_cast<float>(efb.height) / 4.0f, 0.0f, 1.0f);
        encoderPass.SetPipeline(efbBlitPipeline_);
        encoderPass.SetBindGroup(0, efbCopySource_);
        encoderPass.Draw(3);
        encoderPass.End();
    }

    // 4. Scale the EFB to the screen, then the report on top.
    const wgpu::Texture screen = context_->AcquireFrame();
    if (!screen)
        return false;
    {
        wgpu::RenderPassColorAttachment color;
        color.view    = DefaultView(screen);
        color.loadOp  = wgpu::LoadOp::Clear;
        color.storeOp = wgpu::StoreOp::Store;
        wgpu::RenderPassDescriptor pass;
        pass.colorAttachmentCount = 1;
        pass.colorAttachments     = &color;
        const wgpu::RenderPassEncoder encoderPass = encoder.BeginRenderPass(&pass);
        encoderPass.SetPipeline(screenBlitPipeline_);
        encoderPass.SetBindGroup(0, efb.blitSource);
        encoderPass.Draw(3);
        if (overlaySource_) {
            encoderPass.SetPipeline(overlayPipeline_);
            encoderPass.SetBindGroup(0, overlaySource_);
            encoderPass.Draw(3);
        }
        encoderPass.End();
    }

    const wgpu::CommandBuffer commands = encoder.Finish();
    lap(&FrameTimings::record);
    queue.Submit(1, &commands);
    lap(&FrameTimings::submit);
    context_->Present();
    context_->ProcessEvents();
    lap(&FrameTimings::present);
    lastEfb_ = &efb;
    return true;
}

void Benchmark::ShowOverlay(const TextImage &overlay)
{
    UploadOverlay(overlay);
    Efb &efb = lastEfb_ ? *lastEfb_ : GetEfb(context_->Width(), context_->Height());
    RenderFrame(drawPipeline_, efb, FrameWork{});
}

void Benchmark::RunShaderCompileTest(Report &report)
{
    // Each variant is a distinct shader, so the driver compiles it from scratch.
    constexpr int kVariants = 6;
    Efb &efb                = GetEfb(context_->Width(), context_->Height());
    std::vector<double> times;
    for (int i = 0; i < kVariants; ++i) {
        const double start                  = NowMs();
        const wgpu::RenderPipeline pipeline = CreateDrawPipeline(0.001f * static_cast<float>(i + 1));
        // Draw once so any compile the driver deferred to first use is counted too.
        RenderFrame(pipeline, efb, FrameWork{ 1, 1, true });
        context_->WaitForIdle();
        times.push_back(NowMs() - start);
    }
    if (context_->HadDeviceError()) {
        report.Add(Status::Fail, "Shader compile", context_->LastDeviceError());
        return;
    }
    double rest = 0.0;
    for (std::size_t i = 1; i < times.size(); ++i)
        rest += times[i];
    rest /= static_cast<double>(times.size() - 1);
    // Aurora compiles every pipeline before the game starts (shader precache); a few
    // hundred of these is a one-off wait, but each must still compile reliably.
    report.Add(rest < 100.0 ? Status::Pass : Status::Warn, "Shader compile",
               "GX-style pipeline: first " + FormatMs(times[0]) + ", then " + FormatMs(rest) + " each");
}

void Benchmark::RunScenario(const Scenario &scenario, Report &report, const TextImage &overlay)
{
    UploadOverlay(overlay);
    Efb &efb = GetEfb(scenario.efbWidth, scenario.efbHeight);

    const FrameWork work = { scenario.drawCount, scenario.uploadDraws, scenario.perDrawImmediates };
    for (int i = 0; i < kWarmupFrames; ++i)
        RenderFrame(drawPipeline_, efb, work);
    context_->WaitForIdle();

    FrameTimings timings;
    const double start = NowMs();
    int presented      = 0;
    for (int i = 0; i < kMeasuredFrames; ++i) {
        if (RenderFrame(drawPipeline_, efb, work, &timings))
            ++presented;
        // Keep the TV's input queue drained so the app stays responsive.
        SDL_PumpEvents();
    }
    context_->WaitForIdle();
    const double frameMs = (NowMs() - start) / kMeasuredFrames;

    if (context_->HadDeviceError()) {
        report.Add(Status::Fail, scenario.name, context_->LastDeviceError());
        return;
    }
    if (presented < kMeasuredFrames) {
        report.Add(Status::Warn, scenario.name,
                   FormatMs(frameMs) + " per frame, but only " + std::to_string(presented) + " frames were shown");
        return;
    }
    const Status status = frameMs <= kTargetFrameMs        ? Status::Pass
                          : frameMs <= kTargetFrameMs * 1.5 ? Status::Warn
                                                            : Status::Fail;
    char fps[32];
    std::snprintf(fps, sizeof(fps), " (%.0f fps)", 1000.0 / frameMs);
    report.Add(status, scenario.name, FormatMs(frameMs) + " per frame" + fps);

    // Where the CPU time went, per frame (short enough for the screen's value column).
    char split[64];
    std::snprintf(split, sizeof(split), "up %.0f rec %.0f sub %.0f pres %.0f ms",
                  timings.upload / kMeasuredFrames, timings.record / kMeasuredFrames,
                  timings.submit / kMeasuredFrames, timings.present / kMeasuredFrames);
    report.Add(Status::Info, "  | time split", split);
}

} // namespace probe
