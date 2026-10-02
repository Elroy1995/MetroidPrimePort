#include "port_ap_world.h"

#include <cstdio>

#include "port_json.h"

#include <cctype>
#include <cstring>
#include <initializer_list>
#include <sstream>

namespace PortApWorld {
namespace {

struct Room {
  int area;
  uint32_t mlvl;
  uint32_t mrea;
  const char* name;
};

struct Elevator {
  const char* name;
  int area;
  uint32_t mlvl;
  uint32_t mrea;
  uint32_t editorId;
};

#include "port_ap_world_data.inc"

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
  text << "}}";
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

} // namespace PortApWorld
