#include "bloom.hpp"

#include "../logging.hpp"
#include "../webgpu/gpu.hpp"

#include <aurora/gfx.hpp>

#include <algorithm>
#include <array>
#include <cstring>

// Remastered's bloom, as its CRenderPass_Bloom and shaders do it:
//  - a bright pass over the exposed colour X, at a quarter of the frame's size:
//    X * min(max(L - threshold, 0), 8) / L times tint 4, L being X's luminance;
//  - four downsamples to 1/64 (the centre four times and four corners a texel out, / 8);
//  - four upsamples back, each added to the next finer level, the coarsest first with
//    tint 0 (four edge taps a texel out and four corners half one, the corners doubled, / 12);
//  - the result b added to the frame as b / (1 + b).
// The EFB holds the tone-mapped colour gamma encoded, so the bright pass undoes the room's
// tone curve to get X back, and the frame is added to in linear terms.
namespace aurora::gfx::bloom {
namespace {
Module Log("aurora::gfx::bloom");
using webgpu::g_device;

constexpr uint32_t Levels = 5;
constexpr uint32_t PassCount = 1 + (Levels - 1) * 2 + 1;
constexpr uint64_t SlotSize = 256;
constexpr auto LevelFormat = wgpu::TextureFormat::RGBA16Float;

struct Uniform {
  float texel[4];
  float tint[4]; // w: the threshold
  float tone[3][4];
};
static_assert(sizeof(Uniform) <= SlotSize);

constexpr const char* ShaderSource = R"(
struct Params {
  texel: vec4f,
  tint: vec4f,
  tone: array<vec4f, 3>,
};
@group(0) @binding(0) var samp: sampler;
@group(0) @binding(1) var src: texture_2d<f32>;
@group(0) @binding(2) var<uniform> p: Params;
@group(0) @binding(3) var bloomTex: texture_2d<f32>;

struct VertexOutput {
  @builtin(position) pos: vec4f,
  @location(0) uv: vec2f,
};

@vertex
fn vs_main(@builtin(vertex_index) i: u32) -> VertexOutput {
  var corners = array<vec2f, 3>(vec2f(-1.0, -1.0), vec2f(3.0, -1.0), vec2f(-1.0, 3.0));
  var out: VertexOutput;
  let c = corners[i];
  out.pos = vec4f(c, 0.0, 1.0);
  out.uv = vec2f(c.x * 0.5 + 0.5, 0.5 - c.y * 0.5);
  return out;
}

// The tone curve, forwards (as the PBR shader draws it) for one channel.
fn tone(x: f32) -> f32 {
  if (x < p.tone[1].z) {
    return ((p.tone[0].x * x + p.tone[0].y) * x + p.tone[0].z) * x;
  }
  if (x < p.tone[1].w) {
    return p.tone[1].x * x + p.tone[1].y;
  }
  let st = max(p.tone[2].y * x + p.tone[2].z, 0.0);
  return p.tone[2].x * st / (1.0 + st) + p.tone[2].w;
}

// And back: the exposed level that drew as y (linear).
fn untone(y: f32) -> f32 {
  let mid = p.tone[1].z;
  let lineStart = p.tone[1].x * mid + p.tone[1].y;
  if (y < lineStart) {
    // The toe rises through the origin to the line's start; Newton from a straight guess.
    var x = y / max(lineStart, 1e-4) * mid;
    for (var n = 0; n < 4; n++) {
      let slope = (3.0 * p.tone[0].x * x + 2.0 * p.tone[0].y) * x + p.tone[0].z;
      x = clamp(x - (tone(x) - y) / max(slope, 1e-4), 0.0, mid);
    }
    return x;
  }
  let top = p.tone[2].w;
  if (y < top || p.tone[2].y <= 0.0) {
    return (y - p.tone[1].y) / p.tone[1].x;
  }
  let u = min((y - top) / max(p.tone[2].x, 1e-4), 0.999);
  return u / (1.0 - u) / p.tone[2].y + p.tone[1].w;
}

// A channel at white has lost its level: the shoulder's inverse runs off to hundreds there, which turned a
// saturated orange hull into a red flood. Cap it where the curve draws about 0.97 of white (4.0 on these rooms).
const MaxExposed = 4.0;

fn exposed(c: vec3f) -> vec3f {
  let y = pow(clamp(c, vec3f(0.0), vec3f(1.0)), vec3f(2.2));
  return min(vec3f(untone(y.r), untone(y.g), untone(y.b)), vec3f(MaxExposed));
}

@fragment
fn fs_bright(in: VertexOutput) -> @location(0) vec4f {
  let size = vec2i(textureDimensions(src));
  let base = vec2i(floor(in.pos.xy)) * 4;
  var sum = vec3f(0.0);
  for (var y = 0; y < 4; y++) {
    for (var x = 0; x < 4; x++) {
      let at = min(base + vec2i(x, y), size - vec2i(1));
      let c = exposed(textureLoad(src, at, 0).rgb);
      let l = dot(c, vec3f(0.2126, 0.7152, 0.0722));
      sum += c * (min(max(l - p.tint.w, 0.0), 8.0) / max(l, 0.001));
    }
  }
  return vec4f(sum / 16.0 * p.tint.rgb, 1.0);
}

@fragment
fn fs_down(in: VertexOutput) -> @location(0) vec4f {
  let t = p.texel.xy;
  var c = textureSampleLevel(src, samp, in.uv, 0.0).rgb * 4.0;
  c += textureSampleLevel(src, samp, in.uv + vec2f(-t.x, -t.y), 0.0).rgb;
  c += textureSampleLevel(src, samp, in.uv + vec2f(t.x, -t.y), 0.0).rgb;
  c += textureSampleLevel(src, samp, in.uv + vec2f(-t.x, t.y), 0.0).rgb;
  c += textureSampleLevel(src, samp, in.uv + vec2f(t.x, t.y), 0.0).rgb;
  return vec4f(c * 0.125, 1.0);
}

@fragment
fn fs_up(in: VertexOutput) -> @location(0) vec4f {
  let t = p.texel.xy;
  let h = t * 0.5;
  var c = textureSampleLevel(src, samp, in.uv + vec2f(-t.x, 0.0), 0.0).rgb;
  c += textureSampleLevel(src, samp, in.uv + vec2f(t.x, 0.0), 0.0).rgb;
  c += textureSampleLevel(src, samp, in.uv + vec2f(0.0, -t.y), 0.0).rgb;
  c += textureSampleLevel(src, samp, in.uv + vec2f(0.0, t.y), 0.0).rgb;
  c += textureSampleLevel(src, samp, in.uv + vec2f(-h.x, -h.y), 0.0).rgb * 2.0;
  c += textureSampleLevel(src, samp, in.uv + vec2f(h.x, -h.y), 0.0).rgb * 2.0;
  c += textureSampleLevel(src, samp, in.uv + vec2f(-h.x, h.y), 0.0).rgb * 2.0;
  c += textureSampleLevel(src, samp, in.uv + vec2f(h.x, h.y), 0.0).rgb * 2.0;
  return vec4f(c / 12.0 * p.tint.rgb, 1.0);
}

@fragment
fn fs_composite(in: VertexOutput) -> @location(0) vec4f {
  let size = vec2i(textureDimensions(src));
  let f = textureLoad(src, min(vec2i(floor(in.pos.xy)), size - vec2i(1)), 0);
  let b = max(textureSampleLevel(bloomTex, samp, in.uv, 0.0).rgb, vec3f(0.0));
  let lin = pow(f.rgb, vec3f(2.2)) + b / (1.0 + b);
  return vec4f(pow(clamp(lin, vec3f(0.0), vec3f(1.0)), vec3f(1.0 / 2.2)), f.a);
}
)";

struct Level {
  wgpu::Texture texture;
  wgpu::TextureView view;
  uint32_t width = 0;
  uint32_t height = 0;
};

struct State {
  EncoderTaskId task = InvalidEncoderTask;
  wgpu::ShaderModule module;
  wgpu::BindGroupLayout layout;
  wgpu::PipelineLayout pipelineLayout;
  wgpu::Sampler sampler;
  wgpu::Buffer uniforms;
  wgpu::RenderPipeline bright;
  wgpu::RenderPipeline down;
  wgpu::RenderPipeline up;
  wgpu::RenderPipeline composite;
  wgpu::TextureFormat compositeFormat = wgpu::TextureFormat::Undefined;
  uint32_t compositeSamples = 0;
  // The frame as it was, and the levels; remade when the frame's size or format changes.
  wgpu::Texture frame;
  wgpu::TextureView frameView;
  wgpu::TextureFormat frameFormat = wgpu::TextureFormat::Undefined;
  uint32_t width = 0;
  uint32_t height = 0;
  std::array<Level, Levels> levels;
};
State g_state;

wgpu::RenderPipeline make_pipeline(const char* label, const char* entry, wgpu::TextureFormat format,
                                   uint32_t samples, const wgpu::BlendState* blend) {
  const wgpu::ColorTargetState target{
      .format = format,
      .blend = blend,
      .writeMask = wgpu::ColorWriteMask::All,
  };
  const wgpu::FragmentState fragment{
      .module = g_state.module,
      .entryPoint = entry,
      .targetCount = 1,
      .targets = &target,
  };
  const wgpu::RenderPipelineDescriptor descriptor{
      .label = label,
      .layout = g_state.pipelineLayout,
      .vertex = {.module = g_state.module, .entryPoint = "vs_main"},
      .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList},
      .multisample = {.count = samples, .mask = UINT32_MAX},
      .fragment = &fragment,
  };
  return g_device.CreateRenderPipeline(&descriptor);
}

void ensure_pipelines() {
  if (g_state.module) {
    return;
  }
  wgpu::ShaderSourceWGSL source{};
  source.code = ShaderSource;
  const wgpu::ShaderModuleDescriptor moduleDescriptor{.nextInChain = &source, .label = "Bloom Module"};
  g_state.module = g_device.CreateShaderModule(&moduleDescriptor);
  const std::array entries{
      wgpu::BindGroupLayoutEntry{
          .binding = 0,
          .visibility = wgpu::ShaderStage::Fragment,
          .sampler = {.type = wgpu::SamplerBindingType::Filtering},
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 1,
          .visibility = wgpu::ShaderStage::Fragment,
          .texture = {.sampleType = wgpu::TextureSampleType::Float, .viewDimension = wgpu::TextureViewDimension::e2D},
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 2,
          .visibility = wgpu::ShaderStage::Fragment,
          .buffer = {.type = wgpu::BufferBindingType::Uniform, .minBindingSize = sizeof(Uniform)},
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 3,
          .visibility = wgpu::ShaderStage::Fragment,
          .texture = {.sampleType = wgpu::TextureSampleType::Float, .viewDimension = wgpu::TextureViewDimension::e2D},
      },
  };
  const wgpu::BindGroupLayoutDescriptor layoutDescriptor{
      .label = "Bloom Bind Group Layout",
      .entryCount = entries.size(),
      .entries = entries.data(),
  };
  g_state.layout = g_device.CreateBindGroupLayout(&layoutDescriptor);
  const wgpu::PipelineLayoutDescriptor pipelineLayoutDescriptor{
      .label = "Bloom Pipeline Layout",
      .bindGroupLayoutCount = 1,
      .bindGroupLayouts = &g_state.layout,
  };
  g_state.pipelineLayout = g_device.CreatePipelineLayout(&pipelineLayoutDescriptor);
  const wgpu::SamplerDescriptor samplerDescriptor{
      .label = "Bloom Sampler",
      .addressModeU = wgpu::AddressMode::ClampToEdge,
      .addressModeV = wgpu::AddressMode::ClampToEdge,
      .magFilter = wgpu::FilterMode::Linear,
      .minFilter = wgpu::FilterMode::Linear,
  };
  g_state.sampler = g_device.CreateSampler(&samplerDescriptor);
  const wgpu::BufferDescriptor bufferDescriptor{
      .label = "Bloom Uniforms",
      .usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
      .size = SlotSize * PassCount,
  };
  g_state.uniforms = g_device.CreateBuffer(&bufferDescriptor);
  g_state.bright = make_pipeline("Bloom Bright Pass", "fs_bright", LevelFormat, 1, nullptr);
  g_state.down = make_pipeline("Bloom Downsample", "fs_down", LevelFormat, 1, nullptr);
  const wgpu::BlendState add{
      .color = {.operation = wgpu::BlendOperation::Add,
                .srcFactor = wgpu::BlendFactor::One,
                .dstFactor = wgpu::BlendFactor::One},
      .alpha = {.operation = wgpu::BlendOperation::Add,
                .srcFactor = wgpu::BlendFactor::Zero,
                .dstFactor = wgpu::BlendFactor::One},
  };
  g_state.up = make_pipeline("Bloom Upsample", "fs_up", LevelFormat, 1, &add);
}

void ensure_targets(uint32_t width, uint32_t height, wgpu::TextureFormat format) {
  if (g_state.frame && g_state.width == width && g_state.height == height && g_state.frameFormat == format) {
    return;
  }
  g_state.width = width;
  g_state.height = height;
  g_state.frameFormat = format;
  const wgpu::TextureDescriptor frameDescriptor{
      .label = "Bloom Frame Copy",
      .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst,
      .size = {width, height, 1},
      .format = format,
  };
  g_state.frame = g_device.CreateTexture(&frameDescriptor);
  g_state.frameView = g_state.frame.CreateView();
  uint32_t w = (width + 3) / 4;
  uint32_t h = (height + 3) / 4;
  for (auto& level : g_state.levels) {
    level.width = std::max(w, 1u);
    level.height = std::max(h, 1u);
    const wgpu::TextureDescriptor descriptor{
        .label = "Bloom Level",
        .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding,
        .size = {level.width, level.height, 1},
        .format = LevelFormat,
    };
    level.texture = g_device.CreateTexture(&descriptor);
    level.view = level.texture.CreateView();
    w = (w + 1) / 2;
    h = (h + 1) / 2;
  }
}

void draw(const wgpu::CommandEncoder& cmd, const wgpu::RenderPipeline& pipeline, uint32_t slot,
          const wgpu::TextureView& source, const wgpu::TextureView& bloomSource, const wgpu::TextureView& target,
          bool load, const wgpu::TextureView& resolve = {}) {
  const std::array entries{
      wgpu::BindGroupEntry{.binding = 0, .sampler = g_state.sampler},
      wgpu::BindGroupEntry{.binding = 1, .textureView = source},
      wgpu::BindGroupEntry{.binding = 2, .buffer = g_state.uniforms, .offset = slot * SlotSize, .size = sizeof(Uniform)},
      wgpu::BindGroupEntry{.binding = 3, .textureView = bloomSource},
  };
  const wgpu::BindGroupDescriptor groupDescriptor{
      .layout = g_state.layout,
      .entryCount = entries.size(),
      .entries = entries.data(),
  };
  const auto group = g_device.CreateBindGroup(&groupDescriptor);
  const wgpu::RenderPassColorAttachment attachment{
      .view = target,
      .resolveTarget = resolve,
      .loadOp = load ? wgpu::LoadOp::Load : wgpu::LoadOp::Clear,
      .storeOp = wgpu::StoreOp::Store,
      .clearValue = {0.0, 0.0, 0.0, 0.0},
  };
  const wgpu::RenderPassDescriptor passDescriptor{
      .label = "Bloom Pass",
      .colorAttachmentCount = 1,
      .colorAttachments = &attachment,
  };
  const auto pass = cmd.BeginRenderPass(&passDescriptor);
  pass.SetPipeline(pipeline);
  pass.SetBindGroup(0, group);
  pass.Draw(3);
  pass.End();
}

void encode(const EncoderTaskContext& ctx, const wgpu::CommandEncoder& cmd, const void* payload, size_t payloadSize,
            void*) {
  if (payloadSize != sizeof(Params)) {
    return;
  }
  Params params;
  std::memcpy(&params, payload, sizeof(params));
  const auto& source = webgpu::present_source();
  const auto& target = webgpu::g_frameBuffer;
  const uint32_t samples = webgpu::g_graphicsConfig.msaaSamples > 1 ? webgpu::g_graphicsConfig.msaaSamples : 1;
  const auto format = webgpu::g_graphicsConfig.surfaceConfiguration.format;
  const uint32_t width = source.size.width;
  const uint32_t height = source.size.height;
  if (width == 0 || height == 0 || !source.texture || !target.view) {
    return;
  }
  ensure_pipelines();
  ensure_targets(width, height, format);
  if (!g_state.composite || g_state.compositeFormat != format || g_state.compositeSamples != samples) {
    g_state.composite = make_pipeline("Bloom Composite", "fs_composite", format, samples, nullptr);
    g_state.compositeFormat = format;
    g_state.compositeSamples = samples;
  }

  // Every pass's uniforms: bright, the downsamples, the upsamples, the composite.
  std::array<std::array<uint8_t, SlotSize>, PassCount> slots{};
  const auto put = [&](uint32_t slot, float texelX, float texelY, const float* tint) {
    Uniform u{};
    u.texel[0] = texelX;
    u.texel[1] = texelY;
    if (tint != nullptr) {
      std::memcpy(u.tint, tint, sizeof(float) * 3);
    }
    u.tint[3] = params.threshold;
    std::memcpy(u.tone, params.tone, sizeof(u.tone));
    std::memcpy(slots[slot].data(), &u, sizeof(u));
  };
  uint32_t slot = 0;
  put(slot++, 1.f / float(width), 1.f / float(height), params.tints[4]);
  for (uint32_t i = 1; i < Levels; ++i) {
    const auto& from = g_state.levels[i - 1];
    put(slot++, 1.f / float(from.width), 1.f / float(from.height), nullptr);
  }
  for (uint32_t i = Levels - 1; i-- > 0;) {
    const auto& from = g_state.levels[i + 1];
    put(slot++, 1.f / float(from.width), 1.f / float(from.height), params.tints[Levels - 2 - i]);
  }
  put(slot++, 0.f, 0.f, nullptr);
  ctx.queue.WriteBuffer(g_state.uniforms, 0, slots.data(), sizeof(slots));

  const wgpu::TexelCopyTextureInfo copySource{.texture = source.texture};
  const wgpu::TexelCopyTextureInfo copyTarget{.texture = g_state.frame};
  const wgpu::Extent3D copySize{width, height, 1};
  cmd.CopyTextureToTexture(&copySource, &copyTarget, &copySize);

  slot = 0;
  auto& levels = g_state.levels;
  draw(cmd, g_state.bright, slot++, g_state.frameView, g_state.frameView, levels[0].view, false);
  for (uint32_t i = 1; i < Levels; ++i) {
    draw(cmd, g_state.down, slot++, levels[i - 1].view, levels[i - 1].view, levels[i].view, false);
  }
  for (uint32_t i = Levels - 1; i-- > 0;) {
    draw(cmd, g_state.up, slot++, levels[i + 1].view, levels[i + 1].view, levels[i].view, true);
  }
  draw(cmd, g_state.composite, slot++, g_state.frameView, levels[0].view, target.view, true,
       samples > 1 ? webgpu::g_frameBufferResolved.view : wgpu::TextureView{});
}
} // namespace

bool push(const Params& params) {
  if (g_state.task == InvalidEncoderTask) {
    g_state.task = register_encoder_task_type(EncoderTaskDescriptor{.label = "Bloom", .callback = encode});
    if (g_state.task == InvalidEncoderTask) {
      Log.warn("could not register the bloom task");
      return false;
    }
  }
  return push_encoder_task(g_state.task, &params, sizeof(params));
}

void shutdown() {
  const auto task = g_state.task;
  g_state = {};
  if (task != InvalidEncoderTask) {
    unregister_encoder_task_type(task);
  }
}
} // namespace aurora::gfx::bloom
