// Room environments at run time: which areas have one, their cubes on the GPU, and the
// cube and ambient for a model. See port_room_env.h.
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
  float average = 0.f;  // luminance over every direction, as stored
  uint32_t mipCount = 0;
};

struct Area {
  File file;
  std::vector<GpuCube> cubes;
  float exposure = 0.f; // what takes the room's radiance to the display's range; 0: unknown
};

// Areas in memory; one without a file has an empty File.
std::unordered_map<uint32_t, Area> sAreas;
uint32_t sNextCube = 1;
int sEnabled = -1;
int sExposure = -1;
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

// A cube's average luminance, from its last mip (a texel or four a face).
float CubeAverage(const File& file, const Cube& cube) {
  const uint32_t edge = std::max(cube.size >> (cube.mipCount - 1), 1u);
  const size_t blockBytes = size_t((edge + 3) / 4) * ((edge + 3) / 4) * 16;
  const uint8_t* blocks = file.data.data() + cube.offset + cube.length - blockBytes * 6;
  std::vector<uint16_t> texels(size_t(edge) * edge * 4);
  double sum = 0.0;
  for (uint32_t face = 0; face < 6; ++face) {
    PortRemastered::DecodeBc6hFace(blocks + blockBytes * face, edge, cube.isSigned, texels.data());
    for (size_t i = 0; i < size_t(edge) * edge; ++i) {
      sum += 0.2126 * HalfToFloat(texels[i * 4]) + 0.7152 * HalfToFloat(texels[i * 4 + 1]) +
             0.0722 * HalfToFloat(texels[i * 4 + 2]);
    }
  }
  return float(sum / (6.0 * edge * edge));
}

// The exposure Remastered's auto exposure would settle on for the room as a whole. Its
// exposure values are log2 of the radiance plus a constant: over the 275 rooms the middle
// of the hint's range is 7.77 above log2 of the median probe's radiance (slope 1.08,
// spread one stop), and a room at its value comes out at the key, 0.18.
float RoomExposure(const Area& area) {
  constexpr float kEvOffset = 7.77f;
  std::vector<float> levels;
  for (const Probe& probe : area.file.probes) {
    const float level = area.cubes[probe.cube].average * probe.scale;
    if (level > 0.f && std::isfinite(level)) {
      levels.push_back(level);
    }
  }
  float level = 0.f;
  if (!levels.empty()) {
    std::nth_element(levels.begin(), levels.begin() + levels.size() / 2, levels.end());
    level = levels[levels.size() / 2];
  } else {
    // The grid's points are the same radiance.
    for (const Grid& grid : area.file.grids) {
      level = std::max(level, grid.average);
    }
  }
  if (!(level > 0.f)) {
    return 0.f;
  }
  float ev = std::log2(level) + kEvOffset;
  if (area.file.exposure[0] != 0.f || area.file.exposure[1] != 0.f) {
    ev = std::min(std::max(ev, area.file.exposure[0]), area.file.exposure[1]);
  }
  const float key = area.file.tonemap[1] > 0.f && area.file.tonemap[1] < 1.f ? area.file.tonemap[1] : 0.18f;
  return key * std::exp2(kEvOffset - ev); // key / level when the hint does not bind
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
  for (size_t i = 0; i < area.cubes.size(); ++i) {
    area.cubes[i].average = CubeAverage(area.file, area.file.cubes[i]);
    // Black: nothing to reflect, and no exposure to set by it.
    area.cubes[i].failed = !(area.cubes[i].average > 1e-6f);
  }
  area.exposure = RoomExposure(area);
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
  for (uint32_t mip = 0; mip < cube.mipCount; ++mip) {
    const uint32_t edge = std::max(cube.size >> mip, 1u);
    const size_t blockBytes = size_t((edge + 3) / 4) * ((edge + 3) / 4) * 16;
    for (uint32_t face = 0; face < 6; ++face) {
      uint16_t* out = texels.data() + (perFace * face + mipOffset[mip]) / 2;
      PortRemastered::DecodeBc6hFace(blocks, edge, cube.isSigned, out);
      blocks += blockBytes;
    }
  }
  gpu.id = sNextCube++;
  if (sNextCube == 0) {
    sNextCube = 1;
  }
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

bool RoomExposed() {
  if (sExposure < 0) {
    const char* const env = std::getenv("MP_ROOM_ENV_EXPOSURE");
    sExposure = env != nullptr && env[0] == '1' ? 1 : 0;
  }
  return sExposure != 0;
}

void SetRoomExposed(bool on) {
  sExposure = on ? 1 : 0;
  sLastValid = false;
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
  static const float gain = EnvFloat("MP_ROOM_ENV_GAIN", 1.f);
  static const float lod = EnvFloat("MP_ROOM_ENV_LOD", 5.f);
  static const float ambient = EnvFloat("MP_ROOM_ENV_AMBIENT", 1.f);
  const float grey = 0.18f * gain;
  sLast = {};
  Area* bestArea = nullptr;
  Pick best;
  for (auto& [mrea, area] : sAreas) {
    const Pick pick = PickProbe(area.file, pos);
    if (pick.Better(best)) {
      best = pick;
      bestArea = &area;
    }
  }
  if (bestArea != nullptr) {
    const Probe& probe = bestArea->file.probes[best.probe];
    GpuCube& gpu = bestArea->cubes[probe.cube];
    if (gpu.id == 0 && !gpu.failed) {
      Upload(bestArea->file, bestArea->file.cubes[probe.cube], gpu);
    }
    if (gpu.id != 0) {
      // The cube is exposed so that its average direction is middle grey, which is what
      // Remastered's auto exposure aims for (its Tonemap's key is 0.18 too); the lamps in
      // it then come out many times brighter than white, as they should.
      // With the room's exposure the cube keeps its level instead: a probe in a dark
      // corner reflects a dark corner.
      const bool room = RoomExposed() && bestArea->exposure > 0.f;
      sLast.cube = gpu.id;
      sLast.params[0] = room ? bestArea->exposure * probe.scale * gain : grey / gpu.average;
      sLast.params[1] = std::min(lod, float(gpu.mipCount - 1));
      sLast.params[2] = float(gpu.mipCount > 2 ? gpu.mipCount - 2 : 0);
      sLast.params[3] = ambient > 0.f ? 1.f / (room ? gpu.average * sLast.params[0] : grey) : 0.f;
      std::memcpy(sLast.worldToCube, probe.worldToCube, sizeof(sLast.worldToCube));
    }
  }
  if (ambient > 0.f) {
    // A model's origin is often on the floor, where the grid has no point for it, so the
    // spot a metre up counts too.
    const float above[3] = {pos[0], pos[1], pos[2] + 1.f};
    Ambient sample;
    float average = 0.f;
    float roomExposure = 0.f;
    for (const float* spot : {pos, above}) {
      for (auto& [mrea, area] : sAreas) {
        for (const Grid& grid : area.file.grids) {
          if (!sLast.hasAmbient && grid.average > 0.f && SampleGrid(area.file, grid, spot, sample)) {
            sLast.hasAmbient = true;
            average = grid.average;
            roomExposure = area.exposure;
          }
        }
      }
    }
    if (sLast.hasAmbient) {
      // The grid gives the light's colour and direction; how bright it is stays the game's
      // ambient, which the shader multiplies in. The baked levels are HDR that Remastered
      // exposes by what is on screen (one room spans 0.0001 to 100), and the world around
      // the model is still lit the retail way. Of the level only this is kept: a spot
      // darker or brighter than its room is, within a factor of two.
      const float luminance = 0.2126f * sample.mean[0] + 0.7152f * sample.mean[1] + 0.0722f * sample.mean[2];
      const float level = std::min(std::max(std::sqrt(luminance / average), 0.5f), 2.f);
      float exposure = luminance > 0.f ? level / luminance * ambient : 0.f;
      if (RoomExposed() && roomExposure > 0.f) {
        // Or the baked level itself, at the room's exposure: the game's ambient is left out.
        exposure = roomExposure * ambient * gain;
        sLast.ambientAbsolute = true;
      }
      for (int i = 0; i < 3; ++i) {
        sLast.ambient[0][i] = (sample.mean[i] - sample.lobe[i]) * exposure;
        sLast.ambient[1][i] = 2.f * sample.lobe[i] * (1.f + sample.sharpness[i]) * exposure;
        sLast.ambient[2][i] = 1.f + 2.f * sample.sharpness[i];
        std::memcpy(sLast.ambient[3 + i], sample.direction[i], sizeof(sample.direction[i]));
      }
    }
  }
  sLastFound = sLast.cube != 0 || sLast.hasAmbient;
  out = sLast;
  return sLastFound;
}

void Stats(int& areas, int& probes, int& cubes, int& grids) {
  areas = probes = cubes = grids = 0;
  for (const auto& [mrea, area] : sAreas) {
    if (!area.file.probes.empty() || !area.file.grids.empty()) {
      ++areas;
      probes += int(area.file.probes.size());
      grids += int(area.file.grids.size());
    }
    for (const GpuCube& cube : area.cubes) {
      cubes += cube.id != 0 ? 1 : 0;
    }
  }
}

} // namespace PortRoomEnv
