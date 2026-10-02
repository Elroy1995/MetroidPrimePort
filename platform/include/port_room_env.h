#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// A room's lighting environment for PBR models: reflection probes, each a box of the world
// and a prefiltered HDR cube map of what surrounds it. A mod supplies one per area as
// <MREA id>.roomenv; the port reflects the cube of the probe a model stands in, in place
// of its own live probe. It can also hold the room's baked ambient light: a grid of
// points, each with the light arriving there by direction, which replaces the game's one
// ambient colour.
//
// The file is little endian:
//   'MPEV', u32 version (1 or 2), f32 tonemap[4], u32 probes, u32 cubes
//   probe: f32 worldToBox[12], f32 worldToCube[9], s32 layer, u32 cube, f32 scale, f32 blend
//   cube:  u32 size, u32 mips, u32 signed, u32 bytes, then BC6H blocks, every face of
//          mip 0, then of mip 1 and so on
// Version 2 goes on:
//   u32 grids
//   grid:  f32 worldToGrid[12], u32 size[3], then size[0] * size[1] * size[2] points of 24
//          bytes, x fastest and z slowest
//   point: half mean[3], half lobe[3], u8 sharpness[3], u8 direction[3][3] (of red, green
//          and blue, along the grid's axes; 0..255 is -1..1). A mean of zero is no point
//          (inside a wall).
namespace PortRoomEnv {

struct Probe {
  // Rows of world -> box; a point is inside when every coordinate is within -1..1.
  float worldToBox[12];
  // Rows of world direction -> cube lookup direction.
  float worldToCube[9];
  int32_t layer;
  uint32_t cube;
  float scale;
  float blend;
};

struct Cube {
  uint32_t size = 0;
  uint32_t mipCount = 0;
  bool isSigned = false;
  size_t offset = 0; // of the blocks, in File::data
  size_t length = 0;
};

struct Grid {
  // Rows of world -> grid; point (i, j, k) is at grid coordinate (i, j, k).
  float worldToGrid[12];
  uint32_t size[3] = {};
  size_t offset = 0; // of the points, in File::data
  float average = 0.f; // geometric mean of the lit points' luminance
};

struct File {
  float tonemap[4] = {};
  std::vector<Probe> probes;
  std::vector<Cube> cubes;
  std::vector<Grid> grids;
  std::vector<uint8_t> data;
};

// --- The file (port_room_env_file.cpp; no game or GX dependencies) -------------

// "1A2B3C4D.roomenv" (any case) -> 0x1A2B3C4D.
bool ParseFileName(const std::string& fileName, uint32_t& id);
bool Parse(std::vector<uint8_t>&& data, File& out, std::string& error);
// Bytes of BC6H a cube of this size has.
size_t CubeBytes(uint32_t size, uint32_t mipCount);

struct Pick {
  int probe = -1;
  bool inside = false;
  // Inside: the box's volume. Outside: the distance to the box. Smaller is better.
  float score = 0.f;
  bool Better(const Pick& other) const {
    return probe >= 0 && (other.probe < 0 || (inside != other.inside ? inside : score < other.score));
  }
};
// The probe for a point: the smallest box that holds it, else the nearest one.
Pick PickProbe(const File& file, const float pos[3]);

// The baked ambient at a point, as Remastered's shaders evaluate it: per colour channel c
// and for a surface normal n,
//   mean[c] - lobe[c] + 2 * lobe[c] * (1 + sharpness[c]) * q ^ (1 + 2 * sharpness[c]),
//   q = clamp(0.5 + 0.5 * dot(n, direction[c]), 0, 1)
// which averages to the mean over all n. The directions are in world space and not unit
// length.
struct Ambient {
  float mean[3];
  float lobe[3];
  float sharpness[3];
  float direction[3][3];
};
// Blends the points around `pos`, skipping the empty ones; false when there are none.
bool SampleGrid(const File& file, const Grid& grid, const float pos[3], Ambient& out);

// --- The game side (port_room_env.cpp) -----------------------------------------

// The areas in memory now. Loads the files of new ones and frees those of areas that left.
void SetLoadedAreas(const uint32_t* mreas, size_t count);

struct Selection {
  uint32_t cube = 0;       // for GXSetPBRCube; 0 when the room has none
  float params[4] = {};    // for GXSetPBRCube
  float worldToCube[9] = {};
  bool hasAmbient = false;
  // For GXSetPBRAmbient: scaled so that the game's ambient level multiplies it, with the
  // directions still in world space.
  float ambient[6][3] = {};
};
// The room cube and baked ambient for a model at `pos`; false when no loaded area has
// either (or MP_ROOM_ENV=0).
bool Select(const float pos[3], Selection& out);
// Forgets everything (the mods folder changed).
void Reset();
// 0 off, 1 on; the console's `roomenv`.
void SetEnabled(bool enabled);
bool Enabled();
// Areas with an environment, cubes on the GPU, ambient grids.
void Stats(int& areas, int& probes, int& cubes, int& grids);

} // namespace PortRoomEnv
