#pragma once

#include <cstddef>
#include <cstdint>

// Pickup dots on the map, like randomprime's: a plain white dot where each of
// the 100 item pickups sits, until it is collected. Deliberately one colour
// for every item, so the map doesn't spoil a randomized game. CMapWorld draws
// them (grep PortMapPickups).
namespace PortMapPickups {

struct Dot {
  uint32_t world; // MLVL
  uint32_t area;  // MREA
  uint32_t relay; // memory relay the pickup activates when collected
  float pos[3];   // world space
};

// The setting, or forced on in randomizer and Archipelago games.
bool Active();
// True when a randomized game forces it on regardless of the setting.
bool Forced();

// Every pickup, from tools/gen_map_pickups.py.
const Dot* Dots(size_t& count);

} // namespace PortMapPickups
