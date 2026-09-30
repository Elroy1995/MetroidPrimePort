#include "port_map_pickups.h"

#include "port_debug.h"
#include "port_skip_cutscenes.h"

namespace PortMapPickups {
namespace {

const Dot kDots[] = {
#include "port_map_pickups.inc"
};

} // namespace

bool Forced() { return PortSkipCutscenes::Forced(); }

bool Active() { return PortDebug::MapPickups() || Forced(); }

const Dot* Dots(size_t& count) {
  count = sizeof(kDots) / sizeof(kDots[0]);
  return kDots;
}

} // namespace PortMapPickups
