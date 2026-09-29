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

const uint32_t kLandingSite = 0xB2701146;

} // namespace

bool Forced() { return PortRandomizer::Enabled() || PortAp::RandomizedGame(); }

bool Active() { return PortDebug::SkippableCutscenes() || Forced(); }

bool PatchArea(uint32_t mreaId, const uint8_t* scly, size_t size, std::vector< uint8_t >& out) {
  out.clear();
  if (!Active())
    return false;
  const SkipRoom* end = kSkipRooms + sizeof(kSkipRooms) / sizeof(kSkipRooms[0]);
  const SkipRoom* room = std::lower_bound(
      kSkipRooms, end, mreaId, [](const SkipRoom& r, uint32_t id) { return r.mrea < id; });
  if (room == end || room->mrea != mreaId)
    return false;
  const int misses = ApplyOps(scly, size, kSkipOps + room->offset, room->size, out);
  if (misses != 0) {
    // A mod replaced the room, or the disc isn't GM8E01 v1.00: the patch
    // could leave the script half-edited, so keep the room as it is.
    PortLog::Write("skippable cutscenes: room %08X doesn't match (%d), left unpatched\n", mreaId,
                   misses);
    out.clear();
    return false;
  }
  if (mreaId == kLandingSite && PortAp::RandomizedGame()) {
    // Archipelago games start with Samus already out of the ship, as
    // randomprime's patch_landing_site_cutscene_triggers does.
    std::vector< uint8_t > landed;
    if (ApplyOps(out.data(), out.size(), kLandingOps, sizeof(kLandingOps), landed) == 0)
      out.swap(landed);
    else
      PortLog::Write("skippable cutscenes: Landing Site intro skip doesn't match, left as is\n");
  }
  return true;
}

} // namespace PortSkipCutscenes
