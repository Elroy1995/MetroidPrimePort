#include "probe.hpp"

#include "tex_copy_conv.hpp"
#include "../webgpu/gpu.hpp"

#include <array>
#include <unordered_map>

namespace aurora::gfx::probe {
using webgpu::g_device;

namespace {
constexpr uint32_t FaceCount = 6;
wgpu::Texture g_texture;
wgpu::TextureView g_cubeView;
wgpu::Sampler g_sampler;
std::array<std::array<TextureHandle, MipCount>, FaceCount> g_faces;
struct RoomCube {
  wgpu::Texture texture;
  wgpu::TextureView view;
};
std::unordered_map<uint32_t, RoomCube> g_roomCubes;

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
  g_roomCubes.clear();
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

void create_cube(uint32_t id, uint32_t size, uint32_t mipCount, const uint8_t* texels, size_t length) {
  constexpr auto format = wgpu::TextureFormat::RGBA16Float;
  constexpr uint32_t texelSize = 8;
  size_t needed = 0;
  for (uint32_t m = 0; m < mipCount; ++m) {
    const size_t edge = std::max(size >> m, 1u);
    needed += edge * edge * texelSize * FaceCount;
  }
  if (id == 0 || size == 0 || mipCount == 0 || length < needed) {
    return;
  }
  const wgpu::TextureDescriptor textureDescriptor{
      .label = "PBR room cube",
      .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst,
      .dimension = wgpu::TextureDimension::e2D,
      .size = {size, size, FaceCount},
      .format = format,
      .mipLevelCount = mipCount,
      .sampleCount = 1,
  };
  RoomCube cube;
  cube.texture = g_device.CreateTexture(&textureDescriptor);
  const uint8_t* in = texels;
  for (uint32_t f = 0; f < FaceCount; ++f) {
    for (uint32_t m = 0; m < mipCount; ++m) {
      const uint32_t edge = std::max(size >> m, 1u);
      const wgpu::TexelCopyTextureInfo dst{
          .texture = cube.texture,
          .mipLevel = m,
          .origin = {0, 0, f},
      };
      const wgpu::Extent3D extent{edge, edge, 1};
      const size_t bytes = size_t(edge) * edge * texelSize;
      // Through the frame's uploads: the render worker may be submitting the last frame, and the queue is not
      // safe to write from here meanwhile.
      queue_texture_upload_data(in, edge * texelSize, edge, dst, extent);
      in += bytes;
    }
  }
  const wgpu::TextureViewDescriptor cubeDescriptor{
      .label = "PBR room cube view",
      .format = format,
      .dimension = wgpu::TextureViewDimension::Cube,
      .baseMipLevel = 0,
      .mipLevelCount = mipCount,
      .baseArrayLayer = 0,
      .arrayLayerCount = FaceCount,
  };
  cube.view = cube.texture.CreateView(&cubeDescriptor);
  g_roomCubes[id] = std::move(cube);
}

void destroy_cube(uint32_t id) { g_roomCubes.erase(id); }

bool has_cube(uint32_t id) { return g_roomCubes.find(id) != g_roomCubes.end(); }

const wgpu::TextureView& cube_view(uint32_t id) {
  const auto found = g_roomCubes.find(id);
  if (found != g_roomCubes.end()) {
    return found->second.view;
  }
  return cube_view();
}
} // namespace aurora::gfx::probe
