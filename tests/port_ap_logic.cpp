#include "port_ap_logic.h"

#include "port_ap_metroidprime.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
int sLine = 0;
void Check(bool condition) {
  if (!condition) {
    std::fprintf(stderr, "AP logic regression failed at line %d\n", sLine);
    std::abort();
  }
}
#define CHECK(cond)                                                                                \
  do {                                                                                             \
    sLine = __LINE__;                                                                              \
    Check(cond);                                                                                   \
  } while (0)

using PortApLogic::Items;
using PortApLogic::Level;
using PortApLogic::Options;

constexpr int64_t kBase = PortAp::MetroidPrime::kItemBase;

Level At(const Options& options, const Items& items, const char* room, const char* section) {
  size_t count = 0;
  const PortApLogic::Check* checks = PortApLogic::Checks(count);
  const std::vector< Level > levels = PortApLogic::Evaluate(options, items);
  for (size_t i = 0; i < count; ++i) {
    if (std::strcmp(checks[i].room, room) == 0 && std::strcmp(checks[i].section, section) == 0)
      return levels[i];
  }
  CHECK(false);
  return Level::None;
}

size_t CountOf(const Options& options, const Items& items, Level level) {
  size_t out = 0;
  for (Level each : PortApLogic::Evaluate(options, items))
    out += each == level ? 1 : 0;
  return out;
}
} // namespace

int main() {
  // One check per AP location, in the same order.
  size_t count = 0;
  const PortApLogic::Check* checks = PortApLogic::Checks(count);
  size_t locationCount = 0;
  const PortAp::MetroidPrime::Location* locations = PortAp::MetroidPrime::Locations(locationCount);
  CHECK(count == 100 && locationCount == count);
  for (size_t i = 0; i < count; ++i) {
    CHECK(checks[i].id == locations[i].id);
    CHECK(checks[i].area[0] != 0 && checks[i].room[0] != 0 && checks[i].section != nullptr);
  }

  // With nothing, the only thing in sight is the Landing Site pickup, behind
  // its Morph Ball tunnel: the tracker shows it blue.
  {
    const Options options;
    const Items items;
    CHECK(PortApLogic::Evaluate(options, items).size() == count);
    CHECK(CountOf(options, items, Level::Normal) == 0);
    CHECK(At(options, items, "Landing Site", "Morph Ball tunnel") == Level::Inspect);
    CHECK(At(options, items, "Elite Quarters", "Omega Pirate") == Level::None);
  }

  // The Morph Ball puts it in logic; the Hive Totem needs a way to Chozo Ruins.
  {
    const Options options;
    Items items{{kBase + 16, 1}};
    CHECK(At(options, items, "Landing Site", "Morph Ball tunnel") == Level::Normal);
    CHECK(At(options, items, "Hive Totem", "Hive Mecha") == Level::None);
    items = {{kBase + 0, 1}, {kBase + 5, 1}};
    CHECK(At(options, items, "Hive Totem", "Hive Mecha") == Level::Normal);
    // Alcove without Space Jump is a trick: a sequence break with no tricks in logic,
    CHECK(At(options, items, "Alcove", "") == Level::SequenceBreak);
    // in logic once the seed allows easy tricks, or the trick by name,
    Options easy;
    easy.trickDifficulty = 0;
    CHECK(At(easy, items, "Alcove", "") == Level::Normal);
    Options allowed;
    allowed.trickAllow = {"Alcove Escape"};
    // (one trick is not enough: the way in is the Landing Site scan dash)
    CHECK(At(allowed, items, "Alcove", "") == Level::SequenceBreak);
    allowed.trickAllow.push_back("Landing Site Scan Dash");
    CHECK(At(allowed, items, "Alcove", "") == Level::Normal);
    // and out again when the seed denies it; the allow list wins over the deny list.
    easy.trickDeny = {"Alcove Escape"};
    CHECK(At(easy, items, "Alcove", "") == Level::SequenceBreak);
    easy.trickAllow = {"Alcove Escape"};
    CHECK(At(easy, items, "Alcove", "") == Level::Normal);
  }

  // A Missile Expansion is five missiles, and counts for nothing without the
  // launcher when the seed has one.
  {
    Options options;
    Items items{{kBase + 0, 1}, {kBase + 5, 1}, {kBase + 16, 1}, {kBase + 4, 1}};
    CHECK(At(options, items, "Ruined Gallery", "Missile Wall") == Level::Normal);
    options.mainMissile = true;
    CHECK(At(options, items, "Ruined Gallery", "Missile Wall") != Level::Normal);
    items[kBase + 43] = 1;
    items.erase(kBase + 4);
    CHECK(At(options, items, "Ruined Gallery", "Missile Wall") == Level::Normal);
  }

  // Everything is in logic with every item, whichever way the beams come.
  {
    Options options;
    Items items;
    for (int i = 0; i <= 45; ++i)
      items[kBase + i] = 20;
    CHECK(CountOf(options, items, Level::Normal) == count);
    // Without the X-Ray Visor the Omega Pirate is out, whatever the seed removes.
    items.erase(kBase + 13);
    options.removeXray = 2;
    CHECK(At(options, items, "Elite Quarters", "Omega Pirate") == Level::None);

    options = Options();
    options.progressiveBeams = true;
    items.clear();
    for (int i = 4; i <= 45; ++i) {
      if (i != 10 && i != 11 && i != 14 && i != 8 && i != 28)
        items[kBase + i] = 20;
    }
    for (int beam : {49, 51, 52, 53})
      items[kBase + beam] = 3;
    CHECK(CountOf(options, items, Level::Normal) == count);
    // The second Power Beam is its charge, the third the Super Missile.
    items[kBase + 49] = 2;
    const size_t charged = CountOf(options, items, Level::Normal);
    CHECK(charged < count);
    items[kBase + 49] = 1;
    CHECK(CountOf(options, items, Level::Normal) <= charged);
  }

  // Tricks are named as the seed's allow and deny lists name them.
  CHECK(PortApLogic::TrickName(0) != nullptr && PortApLogic::TrickName(100000) == nullptr);
  bool found = false;
  for (size_t i = 0; PortApLogic::TrickName(i) != nullptr; ++i)
    found = found || std::strcmp(PortApLogic::TrickName(i), "Alcove Escape") == 0;
  CHECK(found);

  Options a, b;
  CHECK(a == b);
  b.trickDeny = {"Alcove Escape"};
  CHECK(a != b);

  std::puts("AP logic tests passed");
  return 0;
}
