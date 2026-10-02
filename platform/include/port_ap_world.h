#ifndef METROID_PRIME_PORT_PORT_AP_WORLD_H
#define METROID_PRIME_PORT_PORT_AP_WORLD_H

#include "port_skip_cutscenes.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

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
  // required_artifacts: how many artifacts open the Artifact Temple.
  int requiredArtifacts = 12;
  // elevator_mapping: area -> elevator room -> the room it leads to.
  std::map< std::string, std::map< std::string, std::string > > elevators;
  // door_color_randomization is on (the apworld only recolours its regions'
  // doors then; the mapping below reaches the rest either way).
  bool doorColorRandomization = false;
  // door_color_mapping: area -> a door's lock on the disc -> its lock here.
  bool hasDoorColors = false;
  std::map< std::string, std::map< std::string, std::string > > doorColors;
  // blast_shield_mapping: area -> room -> dock -> blast shield. When the seed
  // has one, the disc's own blast shields are gone.
  bool hasShields = false;
  std::map< std::string, std::map< std::string, std::map< int, std::string > > > shields;

  bool operator==(const Layout& other) const {
    return startRoom == other.startRoom && finalBosses == other.finalBosses &&
           requiredArtifacts == other.requiredArtifacts && elevators == other.elevators &&
           doorColorRandomization == other.doorColorRandomization &&
           hasDoorColors == other.hasDoorColors && doorColors == other.doorColors &&
           hasShields == other.hasShields && shields == other.shields;
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

// The seed has no Meta Ridley fight: the temple opens its portal as soon as
// the artifacts are counted.
bool SkipsRidley(const Layout& layout);

// What the seed changes in the Artifact Temple's script, as an op list for
// PortSkipCutscenes::ApplyOps (randomprime's patch_required_artifact_count
// and patch_artifact_temple_activate_portal_conditions). Empty for a retail
// temple.
std::vector< uint8_t > TempleOps(const Layout& layout);

// A door the seed changes, as the apworld would have told randomprime
// (RoomData.get_door_config_data, after the region pass that mirrors a blast
// shield onto the other side of its door).
struct DoorChange {
  // Index in the door table; stable for a build, used to remember a broken shield.
  int index = -1;
  // The same door seen from the room behind it, -1 when the table has none.
  int pair = -1;
  uint32_t mrea = 0;
  int dock = 0;
  // randomprime's door type ("Blue", "Wave Beam", "Power Beam Only", "Bomb",
  // "Disabled"...), empty when the door keeps its colour.
  std::string type;
  // The blast shield over it: empty for none said, "None" to remove the
  // disc's, else its type ("Missile", "Power Bomb", "Charge Beam"...).
  std::string shield;
  // The door lies flat (in a floor or ceiling).
  bool vertical = false;
  // The Door object, its rotation, its damageable triggers and shield actors.
  uint32_t doorId = 0;
  float rotation[3] = {0.f, 0.f, 0.f};
  uint32_t forces[2] = {0, 0};
  uint32_t shieldActors[2] = {0, 0};
};

// The doors of area `mrea` that the seed changes; docks without a door object
// are left out (randomprime can't patch those either).
std::vector< DoorChange > Doors(const Layout& layout, uint32_t mrea);

// The script patch (PortSkipCutscenes::ApplyOps) that gives a room's doors
// their types, as randomprime's patch_door does: the door's force field
// takes the type's textures and weaknesses, its shield the type's model, and
// a type the game doesn't have gets a scan saying what opens it (`scan` makes
// one from a text and may be empty). `objects` is the room's script; a door
// whose objects aren't what the disc has is left alone. Empty for no change.
using ScanMaker = std::function< uint32_t(const std::string& text) >;
std::vector< uint8_t > DoorOps(const std::vector< DoorChange >& doors,
                               const std::vector< PortSkipCutscenes::ScriptObject >& objects,
                               const ScanMaker& scan);

// Whether a door type needs this resource. Like a pickup's, it may sit in
// another world's PAK (plasma doors exist in four worlds only).
bool IsDoorDependency(uint32_t id);

// The doors of map area `mapa` whose icon changes: the door's editor id and
// its CMappableObject door colour (0 blue, 1 shield, 2 ice, 3 wave, 4 plasma).
struct MapDoor {
  uint32_t doorId = 0;
  int type = 0;
};
std::vector< MapDoor > MapDoors(const Layout& layout, uint32_t mapa);

} // namespace PortApWorld

#endif // METROID_PRIME_PORT_PORT_AP_WORLD_H
