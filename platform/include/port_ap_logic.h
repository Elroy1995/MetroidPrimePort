#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// Which Archipelago checks the player can reach with the items received so
// far, for the tracker list and the map's dot colours. The rules are the
// Metroid Prime AP PopTracker pack's, compiled by tools/gen_ap_logic.py and
// evaluated the way PopTracker does, so the colours mean what they do there.
namespace PortApLogic {

enum class Level : uint8_t {
  None,          // red: out of logic
  Inspect,       // blue: the item can be seen, not collected
  SequenceBreak, // yellow: reachable, but not by the seed's logic
  Normal,        // green: in logic
};

// The seed options that change logic, with the AP world's own values.
struct Options {
  int trickDifficulty = -1; // -1 no tricks, 0 easy, 1 medium, 2 hard
  int combatLogic = 0;      // -1 none, 0 normal, 1 minimal
  int removeXray = 0;       // 0 none, 1 some, 2 all but Omega Pirate
  int removeThermal = 0;
  bool flaahgraPowerBombs = false;
  bool progressiveBeams = false;
  bool mainMissile = false;    // missiles need the Missile Launcher
  bool mainPowerBomb = false;  // power bombs need the main Power Bomb
  bool variaOnlyHeat = false;
  bool preScanElevators = false;
  // Trick names (the AP world's display names) in or out of logic whatever the difficulty.
  std::vector< std::string > trickAllow;
  std::vector< std::string > trickDeny;

  bool operator==(const Options& other) const;
  bool operator!=(const Options& other) const { return !(*this == other); }
};

// Received copies of each AP item id (with the item base).
using Items = std::map< int64_t, int64_t >;

struct Check {
  int64_t id;          // AP location id
  const char* area;    // "Chozo Ruins"
  const char* room;    // "Main Plaza"
  const char* section; // "Half-Pipe", or "" for a room with one pickup
};

// Every location, in the order of MetroidPrime::Locations.
const Check* Checks(size_t& count);

// The level of every location, indexed as Checks.
std::vector< Level > Evaluate(const Options& options, const Items& items);

// The name of trick `index`, or null past the end; for checking a seed's lists.
const char* TrickName(size_t index);

} // namespace PortApLogic
