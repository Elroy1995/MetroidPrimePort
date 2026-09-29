#include "port_skip_cutscenes.h"

#include "port_apclient.h"
#include "port_debug.h"
#include "port_log.h"
#include "port_randomizer.h"

#include <algorithm>

namespace PortSkipCutscenes {
namespace {

struct SkipRoom {
  uint32_t mrea;
  uint32_t offset;
  uint32_t size;
};

#include "port_skip_cutscenes_data.inc"

using PickupRoom = SkipRoom;
struct PickupModelEntry {
  int key;
  uint32_t model, acs, character, animation;
};

#include "port_ap_pickups_data.inc"

const uint32_t kLandingSite = 0xB2701146;

const SkipRoom* FindRoom(const SkipRoom* begin, const SkipRoom* end, uint32_t mreaId) {
  const SkipRoom* room = std::lower_bound(
      begin, end, mreaId, [](const SkipRoom& r, uint32_t id) { return r.mrea < id; });
  return room != end && room->mrea == mreaId ? room : nullptr;
}

// randomprime's pickup patches (tools/gen_ap_pickup_patches.py), on top of the
// skippable ones: a randomized pickup no longer plays its retail item's
// cutscene, and what that cutscene's end did moves onto a relay the pickup
// fires. Only in AP games, where every pickup holds a multiworld item.
void PatchPickups(uint32_t mreaId, const uint8_t* scly, size_t size, std::vector< uint8_t >& out) {
  out.clear();
  const SkipRoom* room =
      FindRoom(kPickupRooms, kPickupRooms + sizeof(kPickupRooms) / sizeof(kPickupRooms[0]), mreaId);
  if (room == nullptr)
    return;
  const int misses = ApplyOps(scly, size, kPickupOps + room->offset, room->size, out);
  if (misses != 0) {
    PortLog::Write("archipelago: room %08X pickup patch doesn't match (%d), left unpatched\n",
                   mreaId, misses);
    out.clear();
  }
}

} // namespace

bool Forced() { return PortRandomizer::Enabled() || PortAp::RandomizedGame(); }

bool Active() { return PortDebug::SkippableCutscenes() || Forced(); }

bool PatchArea(uint32_t mreaId, const uint8_t* scly, size_t size, std::vector< uint8_t >& out) {
  out.clear();
  if (!Active())
    return false;
  const SkipRoom* room =
      FindRoom(kSkipRooms, kSkipRooms + sizeof(kSkipRooms) / sizeof(kSkipRooms[0]), mreaId);
  if (room != nullptr) {
    const int misses = ApplyOps(scly, size, kSkipOps + room->offset, room->size, out);
    if (misses != 0) {
      // A mod replaced the room, or the disc isn't GM8E01 v1.00: the patch
      // could leave the script half-edited, so keep the room as it is. The
      // pickup patch below assumes this one ran, so it is left off too.
      PortLog::Write("skippable cutscenes: room %08X doesn't match (%d), left unpatched\n",
                     mreaId, misses);
      out.clear();
      return false;
    }
  }
  const bool apGame = PortAp::RandomizedGame();
  if (room != nullptr && mreaId == kLandingSite && apGame) {
    // Archipelago games start with Samus already out of the ship, as
    // randomprime's patch_landing_site_cutscene_triggers does.
    std::vector< uint8_t > landed;
    if (ApplyOps(out.data(), out.size(), kLandingOps, sizeof(kLandingOps), landed) == 0)
      out.swap(landed);
    else
      PortLog::Write("skippable cutscenes: Landing Site intro skip doesn't match, left as is\n");
  }
  if (apGame) {
    std::vector< uint8_t > pickups;
    if (room != nullptr)
      PatchPickups(mreaId, out.data(), out.size(), pickups);
    else
      PatchPickups(mreaId, scly, size, pickups);
    if (!pickups.empty())
      out.swap(pickups);
  }
  return !out.empty();
}

bool PickupModel(int key, PortRandomizer::PickupModel& out) {
  for (const PickupModelEntry& entry : kPickupModels) {
    if (entry.key == key) {
      out.model = entry.model;
      out.acs = entry.acs;
      out.character = entry.character;
      out.animation = entry.animation;
      return true;
    }
  }
  return false;
}

} // namespace PortSkipCutscenes
