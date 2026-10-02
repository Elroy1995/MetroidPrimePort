#include "port_ap_world.h"

#include <cstdio>

#include "port_json.h"

#include <cctype>
#include <cstring>
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
       << ",\"final_bosses\":" << layout.finalBosses << ",\"elevator_mapping\":{";
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

} // namespace PortApWorld
