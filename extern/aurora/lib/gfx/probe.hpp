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
} // namespace aurora::gfx::probe
