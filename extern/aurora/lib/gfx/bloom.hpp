#pragma once

#include <cstdint>

// Port extension: Remastered's frame bloom (CRenderPass_Bloom), run on the finished EFB
// between two passes (see GXPortBloom).
namespace aurora::gfx::bloom {
struct Params {
  float threshold;
  float tints[5][3];
  float tone[3][4];
};

// Records the bloom at this point of the frame. False when it could not be recorded.
bool push(const Params& params);
void shutdown();
} // namespace aurora::gfx::bloom
