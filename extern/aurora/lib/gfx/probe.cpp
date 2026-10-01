#include "probe.hpp"

#include "tex_copy_conv.hpp"
#include "../webgpu/gpu.hpp"

#include <array>

namespace aurora::gfx::probe {
using webgpu::g_device;

namespace {
constexpr uint32_t FaceCount = 6;
wgpu::Texture g_texture;
wgpu::TextureView g_cubeView;
wgpu::Sampler g_sampler;
std::array<std::array<TextureHandle, MipCount>, FaceCount> g_faces;

void ensure() {
  if (g_texture) {
    return;
  }
  const auto format = webgpu::g_graphicsConfig.surfaceConfiguration.format;
  const wgpu::TextureDescriptor textureDescriptor{
      .label = "PBR Probe",
      .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::RenderAttachment,
      .dimension = wgpu::TextureDimension::e2D,
      .size = {Size, Size, FaceCount},
      .format = format,
      .mipLevelCount = MipCount,
      .sampleCount = 1,
  };
  g_texture = g_device.CreateTexture(&textureDescriptor);
  const wgpu::TextureViewDescriptor cubeDescriptor{
      .label = "PBR Probe cube view",
      .format = format,
      .dimension = wgpu::TextureViewDimension::Cube,
      .baseMipLevel = 0,
      .mipLevelCount = MipCount,
      .baseArrayLayer = 0,
      .arrayLayerCount = FaceCount,
  };
  g_cubeView = g_texture.CreateView(&cubeDescriptor);
  for (uint32_t f = 0; f < FaceCount; ++f) {
    for (uint32_t m = 0; m < MipCount; ++m) {
      const wgpu::TextureViewDescriptor viewDescriptor{
          .label = "PBR Probe face view",
          .format = format,
          .dimension = wgpu::TextureViewDimension::e2D,
          .baseMipLevel = m,
          .mipLevelCount = 1,
          .baseArrayLayer = f,
          .arrayLayerCount = 1,
      };
      auto view = g_texture.CreateView(&viewDescriptor);
      const wgpu::Extent3D size{Size >> m, Size >> m, 1};
      g_faces[f][m] = std::make_shared<TextureRef>(g_texture, view, view, size, format, 1, GX_TF_RGBA8);
    }
  }
  constexpr wgpu::SamplerDescriptor samplerDescriptor{
      .label = "PBR Probe sampler",
      .addressModeU = wgpu::AddressMode::ClampToEdge,
      .addressModeV = wgpu::AddressMode::ClampToEdge,
      .addressModeW = wgpu::AddressMode::ClampToEdge,
      .magFilter = wgpu::FilterMode::Linear,
      .minFilter = wgpu::FilterMode::Linear,
      .mipmapFilter = wgpu::MipmapFilterMode::Linear,
  };
  g_sampler = g_device.CreateSampler(&samplerDescriptor);
}
} // namespace

void shutdown() {
  for (auto& face : g_faces) {
    for (auto& mip : face) {
      mip.reset();
    }
  }
  g_sampler = {};
  g_cubeView = {};
  g_texture = {};
}

const wgpu::TextureView& cube_view() {
  ensure();
  return g_cubeView;
}

const wgpu::Sampler& sampler() {
  ensure();
  return g_sampler;
}

TextureHandle face(uint32_t face) {
  ensure();
  return g_faces[face % FaceCount][0];
}

void encode_mips(const wgpu::CommandEncoder& cmd, uint32_t face, Range uvRange) {
  ensure();
  const auto& mips = g_faces[face % FaceCount];
  for (uint32_t m = 1; m < MipCount; ++m) {
    // A 2:1 bilinear blit is a box filter.
    tex_copy_conv::blit(cmd, tex_copy_conv::ConvRequest{
                                 .fmt = GX_TF_RGBA8,
                                 .srcView = mips[m - 1]->sampleTextureView,
                                 .uniformRange = uvRange,
                                 .dst = mips[m],
                                 .sampleFilter = tex_copy_conv::SampleFilter::Linear,
                             });
  }
}
} // namespace aurora::gfx::probe
