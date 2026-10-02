// The .roomenv file: parsing, and picking the probe for a point. See port_room_env.h.
#include "port_room_env.h"

#include <cmath>
#include <cstring>

namespace PortRoomEnv {
namespace {

constexpr uint32_t kMagic = 0x5645504D; // 'MPEV'
constexpr uint32_t kVersion = 1;
constexpr size_t kHeaderSize = 32;
constexpr size_t kProbeSize = 100;
constexpr size_t kCubeHeaderSize = 16;
constexpr uint32_t kMaxProbes = 4096;
constexpr uint32_t kMaxCubeSize = 1024;

uint32_t Get32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

float GetFloat(const uint8_t* p) {
  const uint32_t bits = Get32(p);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

int HexDigit(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  c = char(c | 0x20);
  return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

} // namespace

bool ParseFileName(const std::string& fileName, uint32_t& id) {
  static const char kSuffix[] = ".roomenv";
  if (fileName.size() != 8 + sizeof(kSuffix) - 1) {
    return false;
  }
  for (size_t i = 0; i + 1 < sizeof(kSuffix); ++i) {
    const char c = fileName[8 + i];
    if ((c >= 'A' && c <= 'Z' ? char(c | 0x20) : c) != kSuffix[i]) {
      return false;
    }
  }
  id = 0;
  for (size_t i = 0; i < 8; ++i) {
    const int digit = HexDigit(fileName[i]);
    if (digit < 0) {
      return false;
    }
    id = (id << 4) | uint32_t(digit);
  }
  return true;
}

size_t CubeBytes(uint32_t size, uint32_t mipCount) {
  size_t bytes = 0;
  for (uint32_t mip = 0; mip < mipCount; ++mip) {
    const size_t edge = size >> mip > 0 ? size >> mip : 1;
    const size_t blocks = (edge + 3) / 4;
    bytes += blocks * blocks * 16 * 6;
  }
  return bytes;
}

bool Parse(std::vector<uint8_t>&& data, File& out, std::string& error) {
  out = {};
  if (data.size() < kHeaderSize || Get32(data.data()) != kMagic) {
    error = "not a room environment";
    return false;
  }
  if (Get32(data.data() + 4) != kVersion) {
    error = "unknown version " + std::to_string(Get32(data.data() + 4));
    return false;
  }
  for (int i = 0; i < 4; ++i) {
    out.tonemap[i] = GetFloat(data.data() + 8 + i * 4);
  }
  const uint32_t probes = Get32(data.data() + 24);
  const uint32_t cubes = Get32(data.data() + 28);
  if (probes > kMaxProbes || cubes > kMaxProbes || data.size() - kHeaderSize < size_t(probes) * kProbeSize) {
    error = "cut short";
    return false;
  }
  size_t at = kHeaderSize;
  out.probes.resize(probes);
  for (Probe& probe : out.probes) {
    const uint8_t* p = data.data() + at;
    for (int i = 0; i < 12; ++i) {
      probe.worldToBox[i] = GetFloat(p + i * 4);
    }
    for (int i = 0; i < 9; ++i) {
      probe.worldToCube[i] = GetFloat(p + 48 + i * 4);
    }
    probe.layer = int32_t(Get32(p + 84));
    probe.cube = Get32(p + 88);
    probe.scale = GetFloat(p + 92);
    probe.blend = GetFloat(p + 96);
    if (probe.cube >= cubes) {
      error = "a probe names a cube the file does not have";
      return false;
    }
    for (int i = 0; i < 21; ++i) {
      if (!std::isfinite(i < 12 ? probe.worldToBox[i] : probe.worldToCube[i - 12])) {
        error = "a probe is not finite";
        return false;
      }
    }
    at += kProbeSize;
  }
  out.cubes.resize(cubes);
  for (Cube& cube : out.cubes) {
    if (data.size() - at < kCubeHeaderSize) {
      error = "cut short";
      return false;
    }
    const uint8_t* p = data.data() + at;
    cube.size = Get32(p);
    cube.mipCount = Get32(p + 4);
    cube.isSigned = Get32(p + 8) != 0;
    cube.length = Get32(p + 12);
    cube.offset = at + kCubeHeaderSize;
    if (cube.size == 0 || cube.size > kMaxCubeSize || (cube.size & (cube.size - 1)) != 0 || cube.mipCount == 0 ||
        cube.mipCount > 11 || (cube.size >> (cube.mipCount - 1)) == 0) {
      error = "bad cube size";
      return false;
    }
    if (cube.length != CubeBytes(cube.size, cube.mipCount) || data.size() - cube.offset < cube.length) {
      error = "cut short";
      return false;
    }
    at = cube.offset + cube.length;
  }
  out.data = std::move(data);
  return true;
}

Pick PickProbe(const File& file, const float pos[3]) {
  Pick best;
  for (size_t i = 0; i < file.probes.size(); ++i) {
    const float* m = file.probes[i].worldToBox;
    Pick pick;
    pick.probe = int(i);
    pick.inside = true;
    float volume = 8.f;
    float distance2 = 0.f;
    for (int row = 0; row < 3; ++row) {
      const float* r = m + row * 4;
      const float u = r[0] * pos[0] + r[1] * pos[1] + r[2] * pos[2] + r[3];
      // The row's length is 1 / half extent.
      const float scale = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
      const float half = scale > 1e-12f ? 1.f / scale : 0.f;
      volume *= half;
      const float over = std::fabs(u) - 1.f;
      if (!(over <= 0.f)) {
        pick.inside = false;
        distance2 += over * half * over * half;
      }
    }
    pick.score = pick.inside ? volume : std::sqrt(distance2);
    if (pick.Better(best)) {
      best = pick;
    }
  }
  return best;
}

} // namespace PortRoomEnv
