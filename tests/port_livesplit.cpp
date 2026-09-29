#include "port_livesplit.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
int sLine = 0;
void Check(bool condition) {
  if (!condition) {
    std::fprintf(stderr, "livesplit regression failed at line %d\n", sLine);
    std::abort();
  }
}
#define CHECK(cond)                                                                                \
  do {                                                                                             \
    sLine = __LINE__;                                                                              \
    Check(cond);                                                                                   \
  } while (0)

using Lines = std::vector<std::string>;
bool Is(const Lines& got, const Lines& want) { return got == want; }

// Item ids from CPlayerState::EItemType.
constexpr int kPowerBeam = 0, kIceBeam = 1, kMissiles = 4, kEnergyTanks = 24, kTruth = 29;
} // namespace

int main() {
  using PortLiveSplit::FormatTime;
  CHECK(FormatTime(0.0) == "0:00:00.000");
  CHECK(FormatTime(-3.0) == "0:00:00.000");
  CHECK(FormatTime(83.4567) == "0:01:23.457");
  CHECK(FormatTime(3600.0 * 2 + 61.5) == "2:01:01.500");

  CHECK(!PortLiveSplit::CountsAsUpgrade(kPowerBeam));
  CHECK(!PortLiveSplit::CountsAsUpgrade(kEnergyTanks));
  CHECK(PortLiveSplit::CountsAsUpgrade(kIceBeam));
  CHECK(PortLiveSplit::CountsAsUpgrade(kTruth));
  CHECK(!PortLiveSplit::CountsAsUpgrade(-1));
  CHECK(!PortLiveSplit::CountsAsUpgrade(PortLiveSplit::kItemCount));

  int caps[PortLiveSplit::kItemCount] = {};
  caps[kPowerBeam] = 1;

  // A new file: reset, start, game time under the port's control.
  {
    PortLiveSplit::Tracker t;
    Lines out;
    t.Tick(0.0, caps, true, out);
    CHECK(Is(out, {"reset", "starttimer", "pausegametime", "setgametime 0:00:00.000"}));
    CHECK(t.Running());

    // Game time goes out in 0.1 s steps.
    out.clear();
    t.Tick(0.05, caps, true, out);
    CHECK(out.empty());
    t.Tick(0.1, caps, true, out);
    CHECK(Is(out, {"setgametime 0:00:00.100"}));

    // A new upgrade splits at its time; an energy tank does not.
    out.clear();
    caps[kIceBeam] = 1;
    t.Tick(0.12, caps, true, out);
    CHECK(Is(out, {"setgametime 0:00:00.120", "split"}));
    out.clear();
    caps[kEnergyTanks] = 1;
    t.Tick(0.13, caps, true, out);
    CHECK(out.empty());

    // The missile launcher splits, an expansion does not.
    caps[kMissiles] = 5;
    t.Tick(0.14, caps, true, out);
    CHECK(Is(out, {"setgametime 0:00:00.140", "split"}));
    out.clear();
    caps[kMissiles] = 10;
    t.Tick(0.15, caps, true, out);
    CHECK(out.empty());

    // Losing an item (the frigate) and finding it again splits.
    caps[kIceBeam] = 0;
    t.Tick(0.16, caps, true, out);
    CHECK(out.empty());
    caps[kIceBeam] = 1;
    t.Tick(0.17, caps, true, out);
    CHECK(Is(out, {"setgametime 0:00:00.170", "split"}));

    // A world change ends the session; the run goes on and the new session's
    // snapshot does not split on what the file already has.
    out.clear();
    t.EndSession(0.2, out);
    CHECK(Is(out, {"setgametime 0:00:00.200"}));
    out.clear();
    t.Tick(5.0, caps, true, out);
    CHECK(out.empty());
    CHECK(t.Running());
    t.Tick(5.0, caps, true, out);
    CHECK(Is(out, {"setgametime 0:00:05.000"}));

    // The end: a last split, then nothing more.
    out.clear();
    t.EndGame(10.0, out);
    CHECK(Is(out, {"setgametime 0:00:10.000", "split"}));
    CHECK(!t.Running());
    out.clear();
    t.Tick(11.0, caps, true, out);
    t.EndGame(12.0, out);
    CHECK(out.empty());
  }

  // Loading a save does not start the timer; with upgrade splits off, nothing
  // splits but the end.
  {
    PortLiveSplit::Tracker t;
    Lines out;
    t.Tick(100.0, caps, true, out);
    CHECK(out.empty());
    CHECK(!t.Running());
    t.EndGame(200.0, out);
    CHECK(out.empty());

    PortLiveSplit::Tracker u;
    int none[PortLiveSplit::kItemCount] = {};
    u.Tick(0.0, none, false, out);
    out.clear();
    none[kIceBeam] = 1;
    u.Tick(0.01, none, false, out);
    CHECK(out.empty());
  }

  std::puts("livesplit tests passed");
  return 0;
}
