#ifndef METROID_PRIME_PORT_PORT_AP_WORLD_H
#define METROID_PRIME_PORT_PORT_AP_WORLD_H

#include <cstdint>
#include <map>
#include <string>

namespace PortJson {
class Value;
}

// The layout of an Archipelago seed: what the MetroidAPrime apworld would
// have patched into the disc (through randomprime) and the port does to the
// running game instead. The names are the apworld's; the ids behind them come
// from randomprime's tables (tools/gen_ap_world.py).
namespace PortApWorld {

struct Layout {
  // starting_room_name: a room's name without its area.
  std::string startRoom;
  // final_bosses: 0 both, 1 Meta Ridley only, 2 Metroid Prime only, 3 neither.
  int finalBosses = 0;
  // elevator_mapping: area -> elevator room -> the room it leads to.
  std::map< std::string, std::map< std::string, std::string > > elevators;

  bool operator==(const Layout& other) const {
    return startRoom == other.startRoom && finalBosses == other.finalBosses &&
           elevators == other.elevators;
  }
  bool operator!=(const Layout& other) const { return !(*this == other); }
};

// Reads a layout from a slot_data object, or from Text()'s copy of one.
void Parse(const PortJson::Value& data, Layout& layout);
// The layout as a JSON object under the slot_data names.
std::string Text(const Layout& layout);

struct Place {
  uint32_t mlvl = 0;
  uint32_t mrea = 0;
};

// Where a new game starts. False when the layout names no room, or one the
// tables don't have (the retail start then).
bool StartRoom(const Layout& layout, Place& out);

// Where the world teleporter `editorId` of world `mlvl`, which leads to
// `retail` on the disc, leads in this seed. False when it is left alone.
bool TeleporterDestination(const Layout& layout, uint32_t mlvl, uint32_t editorId,
                           const Place& retail, Place& out);

} // namespace PortApWorld

#endif // METROID_PRIME_PORT_PORT_AP_WORLD_H
