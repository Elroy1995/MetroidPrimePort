#include "port_ap_world.h"

#include <cstdio>

#include "port_custom_res.h"
#include "port_json.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <sstream>

namespace PortApWorld {
namespace {

struct Room {
  int area;
  uint32_t mlvl;
  uint32_t mrea;
  uint32_t mapa;
  const char* name;
};

struct Elevator {
  const char* name;
  int area;
  uint32_t mlvl;
  uint32_t mrea;
  uint32_t editorId;
};

struct Door {
  int room;
  int dock;
  int dest;
  int defaultLock;
  int lock;
  int shield;
  int flags;
  int subDoor;
  uint32_t doorId;
  float rotation[3];
  uint32_t forces[2];
  uint32_t shields[2];
};

enum { kDoorExcluded = 1, kDoorVertical = 2 };

#include "port_ap_world_data.inc"

const int kDoorCount = static_cast< int >(sizeof(kDoors) / sizeof(kDoors[0]));

const uint32_t kTallonWorld = 0x39F2DE28;
const uint32_t kCraterWorld = 0xC13B09D1;
// randomprime's "Credits" destination: the ending cinematic's only room.
const Place kCredits = {0x13D79165, 0xB4B41C48};

// Names are compared the way randomprime does: without case or outer spaces.
std::string Folded(const std::string& text) {
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end && std::isspace(static_cast< unsigned char >(text[begin])))
    ++begin;
  while (end > begin && std::isspace(static_cast< unsigned char >(text[end - 1])))
    --end;
  std::string out = text.substr(begin, end - begin);
  for (char& c : out)
    c = static_cast< char >(std::tolower(static_cast< unsigned char >(c)));
  return out;
}

const Elevator* FindElevator(const std::string& name) {
  const std::string folded = Folded(name);
  for (const Elevator& elevator : kElevators) {
    if (Folded(elevator.name) == folded)
      return &elevator;
  }
  return nullptr;
}

// Index of `name` in a name table, -1 when it has no such name.
template < size_t N >
int NameIndex(const char* const (&names)[N], const std::string& name) {
  const std::string folded = Folded(name);
  for (size_t i = 0; i < N; ++i) {
    if (Folded(names[i]) == folded)
      return static_cast< int >(i);
  }
  return -1;
}

// The first door of the room behind `door` that leads back to `door`'s room
// (the apworld's RoomData.get_matching_door), -1 when there is none.
int PairedDoor(int door) {
  const Door& d = kDoors[door];
  if (d.dest < 0)
    return -1;
  for (int i = 0; i < kDoorCount; ++i) {
    if (kDoors[i].room == d.dest && kDoors[i].dest == d.room)
      return i;
  }
  return -1;
}

// What the apworld's region pass leaves on every door: its lock (-1 when the
// table's and the seed's leave it alone) and its blast shield (-1 not said),
// both as indices in kLocks / kShields.
struct Resolved {
  std::vector< int > lock;
  std::vector< int > shield;
};

Resolved Resolve(const Layout& layout) {
  enum { kBlue = 0 };
  const int kNoShield = NameIndex(kShields, "None");
  Resolved out;
  out.lock.resize(kDoorCount);
  out.shield.resize(kDoorCount);
  for (int i = 0; i < kDoorCount; ++i) {
    out.lock[i] = kDoors[i].lock;
    out.shield[i] = kDoors[i].shield;
  }
  if (layout.hasShields) {
    // apply_blast_shield_mapping: the disc's shields go, the seed's come.
    for (int i = 0; i < kDoorCount; ++i) {
      if (out.shield[i] >= 0)
        out.shield[i] = kNoShield;
    }
    for (const auto& area : layout.shields) {
      for (const auto& room : area.second) {
        const std::string name = Folded(room.first);
        for (int i = 0; i < kDoorCount; ++i) {
          const Room& r = kRooms[kDoors[i].room];
          if (Folded(kAreas[r.area]) != Folded(area.first) || Folded(r.name) != name)
            continue;
          const auto entry = room.second.find(kDoors[i].dock);
          if (entry == room.second.end())
            continue;
          const int shield = NameIndex(kShields, entry->second);
          if (shield >= 0)
            out.shield[i] = shield;
        }
      }
    }
  }
  // A shield on either side of a door ends up on both, and the door under it
  // is a plain one.
  auto mirror = [&](int door) {
    const int paired = PairedDoor(door);
    if (paired < 0)
      return;
    if (out.shield[paired] >= 0 && out.shield[paired] != kNoShield) {
      out.shield[door] = out.shield[paired];
      out.lock[door] = kBlue;
    } else if (out.shield[door] >= 0 && out.shield[paired] != kNoShield) {
      out.shield[paired] = out.shield[door];
      out.lock[door] = kBlue;
    }
  };
  for (int i = 0; i < kDoorCount; ++i) {
    const Door& d = kDoors[i];
    if (d.dest < 0)
      continue;
    if (layout.doorColorRandomization && layout.hasDoorColors && (d.flags & kDoorExcluded) == 0) {
      const auto area = layout.doorColors.find(kAreas[kRooms[d.room].area]);
      if (area != layout.doorColors.end()) {
        const auto entry = area->second.find(kLocks[d.defaultLock]);
        if (entry != area->second.end()) {
          const int lock = NameIndex(kLocks, entry->second);
          if (lock >= 0)
            out.lock[i] = lock;
        }
      }
    }
    mirror(i);
    if (d.subDoor >= 0) {
      for (int k = 0; k < kDoorCount; ++k) {
        if (kDoors[k].room == d.dest && kDoors[k].dock == d.subDoor) {
          mirror(k);
          break;
        }
      }
    }
  }
  return out;
}

std::string Quote(const std::string& text) {
  std::string out = "\"";
  for (const char c : text) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (static_cast< unsigned char >(c) < 0x20) {
      char escape[8];
      std::snprintf(escape, sizeof(escape), "\\u%04x", c);
      out += escape;
    } else {
      out += c;
    }
  }
  return out + "\"";
}

} // namespace

void Parse(const PortJson::Value& data, Layout& layout) {
  layout = Layout();
  const PortJson::Value* room = data.Find("starting_room_name");
  if (room != nullptr && room->IsString())
    layout.startRoom = room->AsString();
  const PortJson::Value* bosses = data.Find("final_bosses");
  if (bosses != nullptr && bosses->IsNumber()) {
    const int64_t value = bosses->AsInt();
    layout.finalBosses = value >= 0 && value <= 3 ? static_cast< int >(value) : 0;
  }
  const PortJson::Value* artifacts = data.Find("required_artifacts");
  if (artifacts != nullptr && artifacts->IsNumber()) {
    const int64_t value = artifacts->AsInt();
    layout.requiredArtifacts = value >= 0 && value <= 12 ? static_cast< int >(value) : 12;
  }
  const PortJson::Value* elevators = data.Find("elevator_mapping");
  if (elevators != nullptr && elevators->IsObject()) {
    for (const auto& area : elevators->AsObject()) {
      if (!area.second.IsObject())
        continue;
      for (const auto& entry : area.second.AsObject()) {
        if (entry.second.IsString())
          layout.elevators[area.first][entry.first] = entry.second.AsString();
      }
    }
  }
  // Both door mappings are {area: {"area": area, "type_mapping": {...}}}, and
  // absent when the seed doesn't randomize them.
  const PortJson::Value* colors = data.Find("door_color_mapping");
  if (colors != nullptr && colors->IsObject()) {
    layout.hasDoorColors = true;
    for (const auto& area : colors->AsObject()) {
      const PortJson::Value* types = area.second.IsObject() ? area.second.Find("type_mapping") : nullptr;
      if (types == nullptr || !types->IsObject())
        continue;
      for (const auto& entry : types->AsObject()) {
        if (entry.second.IsString())
          layout.doorColors[area.first][entry.first] = entry.second.AsString();
      }
    }
  }
  const PortJson::Value* colorOption = data.Find("door_color_randomization");
  layout.doorColorRandomization = colorOption != nullptr && colorOption->IsNumber()
                                      ? colorOption->AsInt() != 0
                                      : layout.hasDoorColors;
  const PortJson::Value* shields = data.Find("blast_shield_mapping");
  if (shields != nullptr && shields->IsObject()) {
    layout.hasShields = true;
    for (const auto& area : shields->AsObject()) {
      const PortJson::Value* rooms = area.second.IsObject() ? area.second.Find("type_mapping") : nullptr;
      if (rooms == nullptr || !rooms->IsObject())
        continue;
      for (const auto& room : rooms->AsObject()) {
        if (!room.second.IsObject())
          continue;
        for (const auto& entry : room.second.AsObject()) {
          char* end = nullptr;
          const long dock = std::strtol(entry.first.c_str(), &end, 10);
          if (end != entry.first.c_str() && *end == '\0' && entry.second.IsString())
            layout.shields[area.first][room.first][static_cast< int >(dock)] =
                entry.second.AsString();
        }
      }
    }
  }
}

std::string Text(const Layout& layout) {
  std::ostringstream text;
  text << "{\"starting_room_name\":" << Quote(layout.startRoom)
       << ",\"final_bosses\":" << layout.finalBosses
       << ",\"required_artifacts\":" << layout.requiredArtifacts << ",\"elevator_mapping\":{";
  bool firstArea = true;
  for (const auto& area : layout.elevators) {
    text << (firstArea ? "" : ",") << Quote(area.first) << ":{";
    firstArea = false;
    bool first = true;
    for (const auto& entry : area.second) {
      text << (first ? "" : ",") << Quote(entry.first) << ':' << Quote(entry.second);
      first = false;
    }
    text << '}';
  }
  text << "},\"door_color_randomization\":" << (layout.doorColorRandomization ? 1 : 0);
  if (layout.hasDoorColors) {
    text << ",\"door_color_mapping\":{";
    firstArea = true;
    for (const auto& area : layout.doorColors) {
      text << (firstArea ? "" : ",") << Quote(area.first) << ":{\"type_mapping\":{";
      firstArea = false;
      bool first = true;
      for (const auto& entry : area.second) {
        text << (first ? "" : ",") << Quote(entry.first) << ':' << Quote(entry.second);
        first = false;
      }
      text << "}}";
    }
    text << '}';
  }
  if (layout.hasShields) {
    text << ",\"blast_shield_mapping\":{";
    firstArea = true;
    for (const auto& area : layout.shields) {
      text << (firstArea ? "" : ",") << Quote(area.first) << ":{\"type_mapping\":{";
      firstArea = false;
      bool firstRoom = true;
      for (const auto& room : area.second) {
        text << (firstRoom ? "" : ",") << Quote(room.first) << ":{";
        firstRoom = false;
        bool first = true;
        for (const auto& entry : room.second) {
          text << (first ? "" : ",") << '"' << entry.first << "\":" << Quote(entry.second);
          first = false;
        }
        text << '}';
      }
      text << "}}";
    }
    text << '}';
  }
  text << '}';
  return text.str();
}

bool StartRoom(const Layout& layout, Place& out) {
  std::string name = Folded(layout.startRoom);
  if (name.empty())
    return false;
  // "Area: Room" (or randomprime's "Area:Room") picks among rooms two areas
  // both have; a bare name is the first area's, in the apworld's order.
  std::string area;
  const size_t colon = name.find(':');
  if (colon != std::string::npos) {
    area = Folded(name.substr(0, colon));
    name = Folded(name.substr(colon + 1));
  }
  for (const Room& room : kRooms) {
    if (Folded(room.name) != name || (!area.empty() && Folded(kAreas[room.area]) != area))
      continue;
    out.mlvl = room.mlvl;
    out.mrea = room.mrea;
    return true;
  }
  return false;
}

bool TeleporterDestination(const Layout& layout, uint32_t mlvl, uint32_t editorId,
                           const Place& retail, Place& out) {
  // The Artifact Temple's portal leads to the Crater; without Metroid Prime
  // in the seed it leads to the credits (the apworld's temple_dest).
  if (mlvl == kTallonWorld && retail.mlvl == kCraterWorld) {
    if (layout.finalBosses != 1 && layout.finalBosses != 3)
      return false;
    out = kCredits;
    return true;
  }
  for (const Elevator& elevator : kElevators) {
    // The id's top bits are the layer, which a room patch may have changed.
    if (elevator.mlvl != mlvl || ((elevator.editorId ^ editorId) & 0x03FFFFFF) != 0)
      continue;
    const auto area = layout.elevators.find(kAreas[elevator.area]);
    if (area == layout.elevators.end())
      return false;
    const auto entry = area->second.find(elevator.name);
    if (entry == area->second.end())
      return false;
    const Elevator* target = FindElevator(entry->second);
    if (target == nullptr)
      return false;
    out.mlvl = target->mlvl;
    out.mrea = target->mrea;
    return out.mlvl != retail.mlvl || out.mrea != retail.mrea;
  }
  return false;
}

bool SkipsRidley(const Layout& layout) { return layout.finalBosses >= 2; }

std::vector< uint8_t > TempleOps(const Layout& layout) {
  std::vector< uint8_t > ops;
  auto u16 = [&](uint32_t v) {
    ops.push_back(uint8_t(v >> 8));
    ops.push_back(uint8_t(v));
  };
  auto u32 = [&](uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8)
      ops.push_back(uint8_t(v >> shift));
  };
  auto conn = [&](uint8_t op, uint32_t sender, uint32_t state, uint32_t msg, uint32_t target) {
    ops.push_back(op);
    u32(sender);
    u32(state);
    u32(msg);
    u32(target);
  };
  // One run of bytes at `off` in the properties after the object's name.
  auto edit = [&](uint32_t id, uint32_t off, std::initializer_list< uint8_t > data) {
    ops.push_back(2);
    u32(id);
    u16(0);
    u16(1);
    u16(off);
    u16(static_cast< uint32_t >(data.size()));
    ops.insert(ops.end(), data);
  };
  enum { kZero = 9 };
  enum { kActivate = 1, kDeactivate = 4, kDecrement = 5, kResetAndStart = 11, kSetToZero = 13 };

  if (layout.requiredArtifacts != 12) {
    const uint32_t kCounter = 0x0410011F;  // Counter - Monoliths left to Activate
    const uint32_t kComplete = 0x0410057B; // Relay Monoliths Complete
    // The counter stops at zero instead of starting over, since more stones
    // than it counts may light up.
    if (layout.requiredArtifacts == 0)
      edit(kComplete, 0, {1});
    else
      edit(kCounter, 0, {0, 0, 0, uint8_t(layout.requiredArtifacts)});
    edit(kCounter, 8, {0});
    // The stones of the artifacts that weren't needed light up as well.
    for (uint32_t relay : {0x0010001Fu, 0x0010007Eu, 0x00100032u, 0x0010006Bu, 0x00100045u,
                           0x00100058u, 0x001000DDu, 0x001000CAu, 0x001000F0u, 0x001000B7u,
                           0x00100091u, 0x001000A4u})
      conn(4, kComplete, kZero, kSetToZero, relay);
  }

  if (SkipsRidley(layout)) {
    const uint32_t kPortalTimer = 0x3410039A; // Timer- center teleport
    const uint32_t kTotemCine = 0x341004DA;   // Relay - start totem cine
    const uint32_t kAfterIntro = 0x38100213;  // !Relay End of Ridley Intro Cinematic
    edit(kPortalTimer, 0, {0x3D, 0xCC, 0xCC, 0xCD}); // 0.1 s
    // The totem cinematic goes straight to what follows Ridley's intro...
    conn(3, kTotemCine, kZero, kSetToZero, 0x341001CA);
    conn(4, kTotemCine, kZero, kSetToZero, kAfterIntro);
    // ...which leaves the temple as the fight would: portal open, the fight's
    // layers off, the totem and the stones gone.
    conn(3, kAfterIntro, kZero, 20 /* Play */, 0x001002DB);
    conn(4, kAfterIntro, kZero, kActivate, 0x001002D2);
    conn(4, kAfterIntro, kZero, kSetToZero, 0x38100541);
    for (uint32_t layerOff : {0x04100482u, 0x04100581u, 0x04100309u})
      conn(4, kAfterIntro, kZero, kDecrement, layerOff);
    conn(4, kAfterIntro, kZero, kResetAndStart, kPortalTimer);
    for (uint32_t part : {0x341001C5u, 0x341001C7u, 0x341001C8u})
      conn(4, kAfterIntro, kZero, kDeactivate, part);
    for (uint32_t i = 0; i < 12; ++i) {
      conn(4, kAfterIntro, kZero, kDeactivate, 0x0410000E + i * 0x13); // hint stone
      conn(4, kAfterIntro, kZero, kDeactivate, 0x04100170 + i);        // hologram
      conn(4, kAfterIntro, kZero, kDeactivate, 0x0410001C + i * 0x13); // blue lines
    }
  }
  return ops;
}

std::vector< DoorChange > Doors(const Layout& layout, uint32_t mrea) {
  std::vector< DoorChange > out;
  if (!layout.hasDoorColors && !layout.hasShields)
    return out;
  // DoorShieldFromBlastShieldType: the door a blast shield sits on, per kShields.
  static const char* const kUnderShield[] = {"Bomb",       "Blue", "Plasma Beam",     "Ice Beam",
                                             "Wave Beam",  "Blue", "Power Beam Only", "Blue",
                                             "Disabled",   "Blue"};
  const int kDisabled = NameIndex(kShields, "Disabled");
  const Resolved resolved = Resolve(layout);
  for (int i = 0; i < kDoorCount; ++i) {
    const Door& d = kDoors[i];
    const Room& room = kRooms[d.room];
    if (room.mrea != mrea || d.doorId == 0)
      continue;
    DoorChange change;
    if (resolved.lock[i] >= 0 && resolved.lock[i] != d.defaultLock) {
      change.type = kLocks[resolved.lock[i]];
    } else if (layout.hasDoorColors) {
      const auto area = layout.doorColors.find(kAreas[room.area]);
      if (area != layout.doorColors.end()) {
        const auto entry = area->second.find(kLocks[d.defaultLock]);
        if (entry != area->second.end())
          change.type = entry->second;
      }
    }
    if (resolved.shield[i] >= 0) {
      change.type = kUnderShield[resolved.shield[i]];
      if (resolved.shield[i] != kDisabled)
        change.shield = kShields[resolved.shield[i]];
    }
    if (change.type.empty() && change.shield.empty())
      continue;
    change.index = i;
    change.pair = PairedDoor(i);
    change.mrea = mrea;
    change.dock = d.dock;
    change.vertical = (d.flags & kDoorVertical) != 0;
    change.doorId = d.doorId;
    for (int k = 0; k < 3; ++k)
      change.rotation[k] = d.rotation[k];
    for (int k = 0; k < 2; ++k) {
      change.forces[k] = d.forces[k];
      change.shieldActors[k] = d.shields[k];
    }
    out.push_back(change);
  }
  return out;
}

namespace {

enum { kNormal = 1, kReflect = 2, kImmune = 3 };

// A CDamageVulnerability as a script stores it. randomprime's door_meta.rs
// tables: `name` is a door type (kLocks) or a blast shield type (kShields).
std::vector< uint8_t > Vulnerability(const std::string& name) {
  // 15 weapons (power, ice, wave, plasma, bomb, power bomb, missile, boost
  // ball, phazon, four enemy weapons, two unknown), then the charged beams
  // and the beam combos (power, ice, wave, plasma).
  uint8_t weapon[15] = {kReflect, kReflect, kReflect, kReflect, kImmune,
                        kReflect, kReflect, kImmune,  kImmune,  kImmune,
                        kImmune,  kImmune,  kImmune,  kReflect, kReflect};
  uint8_t charged[5] = {kReflect, kReflect, kReflect, kReflect, kNormal};
  uint8_t combo[5] = {kReflect, kReflect, kReflect, kReflect, kNormal};
  auto all = [&](uint8_t v) {
    for (int i = 0; i < 13; ++i)
      weapon[i] = v;
    for (int i = 0; i < 5; ++i)
      charged[i] = combo[i] = v;
  };
  auto beam = [&](int i) { weapon[i] = charged[i] = combo[i] = kNormal; };
  if (name == "Blue") {
    all(kNormal);
    weapon[7] = kReflect;
    for (int i = 9; i < 13; ++i)
      weapon[i] = kImmune;
  } else if (name == "Disabled") {
    all(kImmune);
  } else if (name == "Power Beam Only") {
    beam(0);
  } else if (name == "Ice Beam") {
    beam(1);
  } else if (name == "Wave Beam") {
    beam(2);
  } else if (name == "Plasma Beam") {
    beam(3);
  } else if (name == "Bomb") {
    weapon[4] = kNormal;
  } else if (name == "Power Bomb") {
    weapon[5] = kNormal;
  } else if (name == "Missile") {
    weapon[6] = kNormal;
    for (int i = 0; i < 4; ++i)
      combo[i] = kNormal;
  } else if (name == "Charge Beam") {
    for (int i = 0; i < 4; ++i)
      charged[i] = kNormal;
  } else if (name == "Super Missile") {
    combo[0] = kNormal;
  } else if (name == "Ice Spreader") {
    combo[1] = kNormal;
  } else if (name == "Wavebuster") {
    combo[2] = kNormal;
  } else if (name == "Flamethrower") {
    combo[3] = kNormal;
  }
  std::vector< uint8_t > out;
  auto u32 = [&](uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8)
      out.push_back(uint8_t(v >> shift));
  };
  u32(18);
  for (uint8_t v : weapon)
    u32(v);
  u32(kNormal);
  u32(5);
  for (uint8_t v : charged)
    u32(v);
  u32(5);
  for (uint8_t v : combo)
    u32(v);
  return out;
}

constexpr uint32_t kDoorPattern = 0x544A9892; // testb.TXTR

struct DoorType {
  const char* name;
  uint32_t cmdl, cmdlVertical;
  uint32_t pattern0, pattern1, color;
  int mapType;      // CMappableObject::EMappableObjectType
  const char* scan; // what opens it, for types the game doesn't have
};

const DoorType kDoorTypes[] = {
    {"Blue", 0x0734977A, 0x18D0AEE6, kDoorPattern, kDoorPattern, 0x8A7F3683, 0, nullptr},
    {"Wave Beam", 0x33188D1B, 0x095B0B93, kDoorPattern, kDoorPattern, 0xF68DF7F1, 3, nullptr},
    {"Ice Beam", 0x59649E9D, 0xB7A8A4C9, kDoorPattern, kDoorPattern, 0xBE4CD99D, 2, nullptr},
    {"Plasma Beam", 0xBBBA1EC7, PortCustomRes::kDoorPlasmaVerticalCmdl, kDoorPattern, kDoorPattern,
     0xFC095F6C, 4, nullptr},
    {"Missile", PortCustomRes::kDoorMissileCmdl, PortCustomRes::kDoorMissileCmdl + 1, kDoorPattern,
     kDoorPattern, 0x8344BEC8, 1,
     "This door will open with &push;&main-color=#D91818;Missiles&pop;."},
    {"Power Beam Only", PortCustomRes::kDoorPowerCmdl, PortCustomRes::kDoorPowerCmdl + 1,
     kDoorPattern, kDoorPattern, 0x1D588B22, 0,
     "This door will only open with &push;&main-color=#D91818;Power Beam&pop;."},
    {"Bomb", PortCustomRes::kDoorBombCmdl, PortCustomRes::kDoorBombCmdl + 1,
     PortCustomRes::kDoorBombPatternTxtr, 0xCFA9DFF3, PortCustomRes::kDoorBombColorTxtr, 0,
     "This door will open with &push;&main-color=#D91818;Morph Ball Bombs&pop;."},
    {"Disabled", PortCustomRes::kDoorDisabledCmdl, PortCustomRes::kDoorDisabledCmdl + 1,
     kDoorPattern, kDoorPattern, 0x717AABCE, 1, "This door cannot be opened."},
};

const DoorType* FindDoorType(const std::string& name) {
  for (const DoorType& type : kDoorTypes)
    if (name == type.name)
      return &type;
  return nullptr;
}

uint32_t Read32(const std::vector< uint8_t >& data, size_t at) {
  return uint32_t(data[at]) << 24 | uint32_t(data[at + 1]) << 16 | uint32_t(data[at + 2]) << 8 |
         data[at + 3];
}

float ReadFloat(const std::vector< uint8_t >& data, size_t at) {
  const uint32_t bits = Read32(data, at);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

// Where an object's properties start, after its name; 0 when it has none.
size_t NameEnd(const std::vector< uint8_t >& props) {
  for (size_t i = 4; i < props.size(); ++i)
    if (props[i] == 0)
      return i + 1;
  return 0;
}

// Script object types, and where things are in their properties (after the
// name): a DamageableTrigger's weaknesses and its three textures, an Actor's
// model, and in a Door the animation set, the light parameters' count, the
// scan and, from the end, the orbit position.
enum { kActorType = 0x00, kDoorType = 0x03, kDamageableTriggerType = 0x1A };
enum {
  kForceVulnerability = 36,
  kForceTextures = 156,
  kForceSize = 170,
  kActorModel = 196,
  kDoorRotation = 12,
  kDoorAnimSet = 36,
  kDoorLightCount = 52,
  kDoorScanCount = 123,
  kDoorScan = 127,
  kDoorTail13 = 43, // orbit position to the end, for 13 properties
};

} // namespace

std::vector< uint8_t > DoorOps(const std::vector< DoorChange >& doors,
                               const std::vector< PortSkipCutscenes::ScriptObject >& objects,
                               const ScanMaker& scan) {
  std::vector< uint8_t > ops;
  auto u16 = [&](uint32_t v) {
    ops.push_back(uint8_t(v >> 8));
    ops.push_back(uint8_t(v));
  };
  auto u32 = [&](uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8)
      ops.push_back(uint8_t(v >> shift));
  };
  struct Run {
    uint32_t off;
    std::vector< uint8_t > data;
  };
  auto edit = [&](uint32_t id, const std::vector< Run >& runs) {
    ops.push_back(2);
    u32(id);
    u16(0);
    u16(static_cast< uint32_t >(runs.size()));
    for (const Run& run : runs) {
      u16(run.off);
      u16(static_cast< uint32_t >(run.data.size()));
      ops.insert(ops.end(), run.data.begin(), run.data.end());
    }
  };
  auto bytes = [](std::initializer_list< uint32_t > values) {
    std::vector< uint8_t > out;
    for (uint32_t v : values)
      for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back(uint8_t(v >> shift));
    return out;
  };
  // The object, when it is of that type and has `size` bytes of properties.
  auto find = [&](uint32_t id, uint8_t type,
                  size_t size) -> const PortSkipCutscenes::ScriptObject* {
    for (const PortSkipCutscenes::ScriptObject& o : objects) {
      if (o.id != id)
        continue;
      const size_t start = NameEnd(o.props);
      return o.type == type && start != 0 && o.props.size() >= start + size ? &o : nullptr;
    }
    return nullptr;
  };

  for (const DoorChange& door : doors) {
    const DoorType* type = FindDoorType(door.type);
    if (type == nullptr)
      continue;
    bool matches = true;
    for (int k = 0; k < 2; ++k) {
      if (door.forces[k] != 0)
        matches = matches && find(door.forces[k], kDamageableTriggerType, kForceSize) != nullptr;
      if (door.shieldActors[k] != 0)
        matches = matches && find(door.shieldActors[k], kActorType, kActorModel + 4) != nullptr;
    }
    if (!matches)
      continue;
    for (int k = 0; k < 2; ++k) {
      if (door.forces[k] != 0)
        edit(door.forces[k],
             {{kForceVulnerability, Vulnerability(door.type)},
              {kForceTextures, bytes({type->pattern0, type->pattern1, type->color})}});
      if (door.shieldActors[k] != 0)
        edit(door.shieldActors[k],
             {{kActorModel, bytes({door.vertical ? type->cmdlVertical : type->cmdl})}});
    }

    // The scan sits on the door itself, which a blast shield would cover.
    const PortSkipCutscenes::ScriptObject* object =
        find(door.doorId, kDoorType, kDoorScan + 4 + kDoorTail13 + 1);
    if (type->scan == nullptr || !door.shield.empty() || !scan || object == nullptr)
      continue;
    const std::vector< uint8_t >& props = object->props;
    const size_t start = NameEnd(props);
    const uint32_t count = Read32(props, 0);
    if ((count != 13 && count != 14) || Read32(props, start + kDoorLightCount) != 14 ||
        Read32(props, start + kDoorScanCount) != 1)
      continue;
    const uint32_t id = scan(type->scan);
    if (id == 0)
      continue;
    std::vector< Run > runs{{kDoorScan, bytes({id})}};
    // Vertical and morph ball doors are looked at from the side they face.
    const float pitch = ReadFloat(props, start + kDoorRotation);
    const bool hatch = Read32(props, start + kDoorAnimSet) == 0xF57DD484;
    float height = 0.f;
    if (hatch && pitch > -90.f && pitch < 90.f)
      height = -2.5f;
    else if (hatch && pitch > -270.f && pitch < -90.f)
      height = 2.5f;
    else if (count == 14 && props.back() != 0)
      height = 1.f;
    if (height != 0.f) {
      uint32_t bits;
      std::memcpy(&bits, &height, sizeof(bits));
      const size_t tail = kDoorTail13 + (count == 14 ? 1 : 0);
      runs.push_back({static_cast< uint32_t >(props.size() - tail - start), bytes({0, 0, bits})});
    }
    edit(door.doorId, runs);
  }
  return ops;
}

bool IsDoorDependency(uint32_t id) {
  static const uint32_t kIds[] = {
      // shields, their rims, and the force field's textures
      0x0734977A, 0x33188D1B, 0x59649E9D, 0xBBBA1EC7, 0x18D0AEE6, 0x095B0B93, 0xB7A8A4C9,
      0x88ED4593, 0xAB031EA9, 0xF6870C9F, 0x61A6945B, 0x459582C1, 0x717AABCE,
      0x8A7F3683, 0x1D588B22, 0xF68DF7F1, 0xBE4CD99D, 0xFC095F6C, 0x8344BEC8,
      0x544A9892, 0xCFA9DFF3,
  };
  for (uint32_t known : kIds)
    if (known == id)
      return true;
  return false;
}

std::vector< MapDoor > MapDoors(const Layout& layout, uint32_t mapa) {
  std::vector< MapDoor > out;
  for (const Room& room : kRooms) {
    if (room.mapa != mapa)
      continue;
    for (const DoorChange& door : Doors(layout, room.mrea)) {
      const DoorType* type = FindDoorType(door.type);
      if (type == nullptr)
        continue;
      const bool shielded = !door.shield.empty() && door.shield != "None";
      out.push_back({door.doorId, shielded ? 1 : type->mapType});
    }
    break;
  }
  return out;
}

} // namespace PortApWorld
