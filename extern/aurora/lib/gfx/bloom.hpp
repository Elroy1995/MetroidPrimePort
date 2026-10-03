#pragma once

#include <cstdint>

// Port extension: Remastered's frame bloom (CRenderPass_Bloom) and colour grade, run on the
// finished EFB between two passes (see GXPortPostProcess).
namespace aurora::gfx::bloom {
constexpr uint32_t GradeLutSize = 33;

struct Params {
  float threshold;
  float tints[5][3];
  float tone[3][4];
  uint32_t bloom;     // 0: grade only
  uint32_t gradeA;    // LUT ids from set_grade_lut; 0 is the identity
  uint32_t gradeB;
  float gradeWeight;  // 0 draws A, 1 draws B
};
static_assert(sizeof(Params) <= 128);

// Records the bloom at this point of the frame. False when it could not be recorded.
bool push(const Params& params);
// Stores a 33^3 RGBA8 LUT (red fastest) under id (non-zero), uploaded before its next use.
void set_grade_lut(uint32_t id, const uint8_t* rgba);
void shutdown();
} // namespace aurora::gfx::bloom
