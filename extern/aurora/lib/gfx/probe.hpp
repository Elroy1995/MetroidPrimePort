#pragma once

#include "texture.hpp"
#include "types.hpp"

// Port extension: the environment probe the PBR path reflects (GX_AURORA_COPY_PROBE_FACE).
// One cube map, written a face at a time from an EFB copy and blurred down its mip chain,
// so the shader can pick a mip by roughness.
namespace aurora::gfx::probe {
constexpr uint32_t Size = 128;
constexpr uint32_t MipCount = 6; // 128 .. 4

void shutdown();
const wgpu::TextureView& cube_view();
const wgpu::Sampler& sampler();
// Mip 0 of one face, as a resolve target.
TextureHandle face(uint32_t face);
// Rebuilds a face's mips from its mip 0. `uvRange` is an identity UV transform uniform.
void encode_mips(const wgpu::CommandEncoder& cmd, uint32_t face, Range uvRange);

// Room cubes (GX_AURORA_CREATE_PBR_CUBE): prefiltered HDR cube maps the game supplies, which
// a PBR draw can reflect instead of the probe above. `texels` is RGBA16Float, every mip of
// face 0 from the largest down, then face 1 and so on.
void create_cube(uint32_t id, uint32_t size, uint32_t mipCount, const uint8_t* texels, size_t length);
void destroy_cube(uint32_t id);
// The view of a room cube, or the probe's when there is no such cube.
const wgpu::TextureView& cube_view(uint32_t id);
bool has_cube(uint32_t id);
} // namespace aurora::gfx::probe
