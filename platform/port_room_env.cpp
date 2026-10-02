// Room environments at run time: which areas have one, their cubes on the GPU, and the
// cube for a model. See port_room_env.h.
#include "port_room_env.h"

#include "port_gci.h"
#include "port_log.h"
#include "port_mods.h"
#include "port_remastered_txtr.h"

#include <dolphin/gx/GXExtra.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <unordered_map>

namespace PortRoomEnv {
namespace {

struct GpuCube {
  uint32_t id = 0;      // 0: not made yet (or it failed)
  bool failed = false;
  float average = 0.f;  // luminance over every direction
  uint32_t mipCount = 0;
};

struct Area {
  File file;
  std::vector<GpuCube> cubes;
};

// Areas in memory; one without a file has an empty File.
std::unordered_map<uint32_t, Area> sAreas;
uint32_t sNextCube = 1;
int sEnabled = -1;
// Model draws come in runs at one position.
bool sLastValid = false;
float sLastPos[3];
bool sLastFound = false;
Selection sLast;

float HalfToFloat(uint16_t h) {
  const int exponent = (h >> 10) & 0x1F;
  const int mantissa = h & 0x3FF;
  float value;
  if (exponent == 0) {
    value = std::ldexp(float(mantissa), -24);
  } else if (exponent == 31) {
    value = mantissa == 0 ? 65504.f : 0.f; // infinity is clamped, a NaN is dropped
  } else {
    value = std::ldexp(float(mantissa | 0x400), exponent - 25);
  }
  return (h & 0x8000) != 0 ? -value : value;
}

float EnvFloat(const char* name, float fallback) {
  const char* const text = std::getenv(name);
  return text != nullptr && text[0] != '\0' ? float(std::atof(text)) : fallback;
}

void Free(Area& area) {
  for (GpuCube& cube : area.cubes) {
    if (cube.id != 0) {
      GXDestroyPBRCube(cube.id);
    }
  }
  area.cubes.clear();
}

void Load(uint32_t mrea, Area& area) {
  const std::string path = PortMods::RoomEnvPath(mrea);
  if (path.empty()) {
    return;
  }
  std::ifstream in(PortGci::PathFromString(path), std::ios::binary);
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::string error;
  if (!in || !Parse(std::move(data), area.file, error)) {
    PortLog::Write("room env: %s: %s\n", path.c_str(), error.empty() ? "cannot read" : error.c_str());
    area.file = {};
    return;
  }
  area.cubes.resize(area.file.cubes.size());
}

// Decodes a cube and hands it to the GPU.
void Upload(const File& file, const Cube& cube, GpuCube& gpu) {
  // RGBA16Float, every mip of face 0, then face 1 (GXCreatePBRCube); the file has every
  // face of mip 0, then mip 1.
  std::vector<size_t> mipOffset(cube.mipCount);
  size_t perFace = 0;
  for (uint32_t mip = 0; mip < cube.mipCount; ++mip) {
    const size_t edge = std::max(cube.size >> mip, 1u);
    mipOffset[mip] = perFace;
    perFace += edge * edge * 8;
  }
  std::vector<uint16_t> texels(perFace * 6 / 2);
  const uint8_t* blocks = file.data.data() + cube.offset;
  double sum = 0.0;
  for (uint32_t mip = 0; mip < cube.mipCount; ++mip) {
    const uint32_t edge = std::max(cube.size >> mip, 1u);
    const size_t blockBytes = size_t((edge + 3) / 4) * ((edge + 3) / 4) * 16;
    for (uint32_t face = 0; face < 6; ++face) {
      uint16_t* out = texels.data() + (perFace * face + mipOffset[mip]) / 2;
      PortRemastered::DecodeBc6hFace(blocks, edge, cube.isSigned, out);
      blocks += blockBytes;
      if (mip + 1 == cube.mipCount) {
        for (size_t i = 0; i < size_t(edge) * edge; ++i) {
          sum += (0.2126 * HalfToFloat(out[i * 4]) + 0.7152 * HalfToFloat(out[i * 4 + 1]) +
                  0.0722 * HalfToFloat(out[i * 4 + 2])) /
                 (6.0 * edge * edge);
        }
      }
    }
  }
  if (!(sum > 1e-6)) {
    gpu.failed = true; // black: nothing to reflect, and no exposure to set by it
    return;
  }
  gpu.id = sNextCube++;
  if (sNextCube == 0) {
    sNextCube = 1;
  }
  gpu.average = float(sum);
  gpu.mipCount = cube.mipCount;
  GXCreatePBRCube(gpu.id, cube.size, cube.mipCount, texels.data(), uint32_t(texels.size() * 2));
}

} // namespace

bool Enabled() {
  if (sEnabled < 0) {
    const char* const env = std::getenv("MP_ROOM_ENV");
    sEnabled = env != nullptr && env[0] == '0' ? 0 : 1;
  }
  return sEnabled != 0;
}

void SetEnabled(bool enabled) {
  sEnabled = enabled ? 1 : 0;
  sLastValid = false;
}

void Reset() {
  for (auto& [mrea, area] : sAreas) {
    Free(area);
  }
  sAreas.clear();
  sLastValid = false;
}

void SetLoadedAreas(const uint32_t* mreas, size_t count) {
  bool changed = false;
  for (auto it = sAreas.begin(); it != sAreas.end();) {
    if (std::find(mreas, mreas + count, it->first) == mreas + count) {
      Free(it->second);
      it = sAreas.erase(it);
      changed = true;
    } else {
      ++it;
    }
  }
  for (size_t i = 0; i < count; ++i) {
    if (sAreas.find(mreas[i]) == sAreas.end()) {
      Load(mreas[i], sAreas[mreas[i]]);
      changed = true;
    }
  }
  if (changed) {
    sLastValid = false;
  }
}

bool Select(const float pos[3], Selection& out) {
  if (sLastValid && std::memcmp(pos, sLastPos, sizeof(sLastPos)) == 0) {
    out = sLast;
    return sLastFound;
  }
  std::memcpy(sLastPos, pos, sizeof(sLastPos));
  sLastValid = true;
  sLastFound = false;
  if (!Enabled()) {
    return false;
  }
  Area* bestArea = nullptr;
  Pick best;
  for (auto& [mrea, area] : sAreas) {
    const Pick pick = PickProbe(area.file, pos);
    if (pick.Better(best)) {
      best = pick;
      bestArea = &area;
    }
  }
  if (bestArea == nullptr) {
    return false;
  }
  const Probe& probe = bestArea->file.probes[best.probe];
  GpuCube& gpu = bestArea->cubes[probe.cube];
  if (gpu.id == 0 && !gpu.failed) {
    Upload(bestArea->file, bestArea->file.cubes[probe.cube], gpu);
  }
  if (gpu.id == 0) {
    return false;
  }
  // The cube is exposed so that its average direction is middle grey, which is what
  // Remastered's auto exposure aims for (its Tonemap's key is 0.18 too); the lamps in it
  // then come out many times brighter than white, as they should.
  static const float gain = EnvFloat("MP_ROOM_ENV_GAIN", 1.f);
  static const float lod = EnvFloat("MP_ROOM_ENV_LOD", 5.f);
  static const float ambient = EnvFloat("MP_ROOM_ENV_AMBIENT", 1.f);
  const float grey = 0.18f * gain;
  sLast.cube = gpu.id;
  sLast.params[0] = grey / gpu.average;
  sLast.params[1] = std::min(lod, float(gpu.mipCount - 1));
  sLast.params[2] = float(gpu.mipCount > 2 ? gpu.mipCount - 2 : 0);
  sLast.params[3] = ambient > 0.f ? 1.f / grey : 0.f;
  std::memcpy(sLast.worldToCube, probe.worldToCube, sizeof(sLast.worldToCube));
  sLastFound = true;
  out = sLast;
  return true;
}

void Stats(int& areas, int& probes, int& cubes) {
  areas = probes = cubes = 0;
  for (const auto& [mrea, area] : sAreas) {
    if (!area.file.probes.empty()) {
      ++areas;
      probes += int(area.file.probes.size());
    }
    for (const GpuCube& cube : area.cubes) {
      cubes += cube.id != 0 ? 1 : 0;
    }
  }
}

} // namespace PortRoomEnv
