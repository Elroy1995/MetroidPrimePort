#pragma once

// Writes a Metroid Prime Remastered world's room environments, one
// "<MREA id>.roomenv" per GameCube area (the format is read by port_room_env.h):
// each room's reflection probes with their HDR cubes, the world's tonemap, and
// the room's baked ambient grid (its LTPB), cropped to the room.
//
// Remastered's rooms are not the GameCube's areas. A world's master pak says
// where it puts each room (the RoomController components of its own ROOM), and
// the retail MLVL says where each area sits; Remastered moved the whole world's
// origin, so the shift that lands the most rooms on areas is found first. A room
// is then matched to the area at its place, using the doors (DoorMP1 against the
// area's door objects) to choose among areas that share an origin and as a check.
//
// Nothing here knows where bytes come from or go to, as with ConvertIO in
// port_remastered_convert.h: the retail resources come through RoomIO, the
// Remastered ones from the paks the caller opened.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "port_remastered_pak.h"

namespace PortRemastered {

struct RoomIO {
  // A retail (GameCube) resource by FourCC type ('MLVL', 'MREA') and id, from
  // the unmodded disc, in the decompressed form the game's own loader sees.
  std::function<bool(uint32_t type, uint32_t id, std::vector<uint8_t>& out)> retail;
  // Stores one output file, "<MREA id as 8 upper case hex>.roomenv".
  std::function<bool(const std::string& name, const std::vector<uint8_t>& data)> write;
  std::function<void(const std::string& line)> log;  // optional
  std::function<bool()> cancelled;                   // optional; asked before each room
};

// One pak of a world, with its name: the file name without the '!' and '.pak'.
struct RoomPak {
  std::string name;
  const Pak* pak = nullptr;
};

// Remastered's world directory names and the retail MLVL each one is.
struct RoomWorld {
  const char* dir;  // "Intro_Master": the pak is Worlds/MP1/!Intro_Master/!Intro_Master.pak
  uint32_t mlvl;
};
const std::vector<RoomWorld>& RoomWorlds();

// Writes the files of one world. `master` is the world's own pak, `rooms` its
// other paks; a room is a pak holding a ROOM asset, and `_Copy` rooms are
// skipped. A room that cannot be placed or has nothing to write is logged, not
// an error. `written` counts files stored; false (with `error`) when the world
// as a whole cannot be read. A room's reflection probes may name assets that
// live in another world's paks (a room used by two worlds is stored in both,
// not always completely), so `others` is searched for those after the world's
// own paks; nothing is written for them. The order of `rooms` only decides which
// rooms the world shift is read from, which moves it by rounding error.
bool WriteWorldRoomEnvs(uint32_t mlvl, const RoomPak& master, const std::vector<RoomPak>& rooms,
                        const std::vector<RoomPak>& others, const RoomIO& io, int& written, std::string& error);

}  // namespace PortRemastered
