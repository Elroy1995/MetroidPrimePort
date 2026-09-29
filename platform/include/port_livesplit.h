#pragma once

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

// Speedrun support: a LiveSplit Server client (LiveSplit > Control > Start TCP
// Server, port 16834 by default) that starts the timer on a new file, feeds it
// the game's in-game time and splits on upgrades and at the end of the game.
//
// The Tracker below turns per-tick game observations into LiveSplit Server
// commands. It is pure so it can be tested without the game; the glue in
// port_livesplit.cpp feeds it from CStateManager and hands the commands to a
// background socket thread.

namespace PortLiveSplit {

// CPlayerState::kIT_Max; the tracker only needs the count.
constexpr int kItemCount = 41;

// "H:MM:SS.fff", what LiveSplit's setgametime parses.
inline std::string FormatTime(double seconds) {
  if (!(seconds > 0.0))
    seconds = 0.0;
  const long long ms = static_cast<long long>(std::floor(seconds * 1000.0 + 0.5));
  char text[32];
  std::snprintf(text, sizeof(text), "%lld:%02lld:%02lld.%03lld", ms / 3600000, ms / 60000 % 60,
                ms / 1000 % 60, ms % 1000);
  return text;
}

// Whether gaining an item (its capacity going from 0 to more) is an upgrade
// split: not the starting equipment, energy tanks or refills. Missiles and
// power bombs split once, on the launcher and the main power bomb, since
// expansions only raise a capacity that is already above 0. Artifacts count.
inline bool CountsAsUpgrade(int item) {
  switch (item) {
  case 0:  // power beam
  case 5:  // scan visor
  case 17: // combat visor
  case 20: // power suit
  case 24: // energy tanks
  case 25: // unknown
  case 26: // health refill
  case 27: // unknown
    return false;
  default:
    return item >= 0 && item < kItemCount;
  }
}

class Tracker {
public:
  // How far the in-game time may run ahead of what LiveSplit was last told.
  static constexpr double kGameTimeStep = 0.1;

  // Called every game tick while a CStateManager runs. The first call of a
  // session takes a snapshot of the capacities, so what the file already has
  // never splits; an in-game time under a second there means a new file, which
  // resets and starts the timer.
  void Tick(double igt, const int* capacities, bool splitUpgrades, std::vector<std::string>& out) {
    if (!mSession) {
      mSession = true;
      for (int i = 0; i < kItemCount; ++i)
        mCapacities[i] = capacities[i];
      if (igt < 1.0) {
        mRunning = true;
        out.push_back("reset");
        out.push_back("starttimer");
        // LiveSplit's game time only moves when told, and the first split
        // records no game time unless it was set after the start.
        out.push_back("pausegametime");
        SendTime(igt, out);
      }
      return;
    }
    bool split = false;
    for (int i = 0; i < kItemCount; ++i) {
      if (mCapacities[i] <= 0 && capacities[i] > 0 && CountsAsUpgrade(i))
        split = true;
      mCapacities[i] = capacities[i];
    }
    if (!mRunning)
      return;
    if (split && splitUpgrades) {
      SendTime(igt, out);
      out.push_back("split");
    } else if (std::fabs(igt - mSentTime) >= kGameTimeStep) {
      SendTime(igt, out);
    }
  }

  // The final blow (the game's EndGame special function): the last split.
  void EndGame(double igt, std::vector<std::string>& out) {
    if (!mRunning)
      return;
    SendTime(igt, out);
    out.push_back("split");
    mRunning = false;
  }

  // The CStateManager went away (quit, world change, death). The next Tick
  // starts a new session; a run in progress carries on across it.
  void EndSession(double igt, std::vector<std::string>& out) {
    if (mRunning && igt != mSentTime)
      SendTime(igt, out);
    mSession = false;
  }

  bool Running() const { return mRunning; }

private:
  void SendTime(double igt, std::vector<std::string>& out) {
    out.push_back("setgametime " + FormatTime(igt));
    mSentTime = igt;
  }

  bool mSession = false;
  bool mRunning = false;
  double mSentTime = 0.0;
  int mCapacities[kItemCount] = {};
};

// Port glue (port_livesplit.cpp).

// The client's state, for the overlay.
enum EStatus { kStatus_Off, kStatus_Connecting, kStatus_Connected, kStatus_Failed };
EStatus Status();
// The last connection error, or an empty string.
std::string LastError();
// Connects to host:port while enabled (reconnecting every few seconds) and
// disconnects when not. Commands made while not connected are dropped.
void Configure(bool enabled, const std::string& address, bool splitUpgrades);
// Called from CStateManager: every tick after the play time advanced
// (`capacities` holds kItemCount entries), when the game ends, and when the
// manager is destroyed.
void GameTick(double igt, const int* capacities);
void GameEnd(double igt);
void GameSessionEnd(double igt);
// Sends a raw command line (console testing).
void SendRaw(const std::string& line);

} // namespace PortLiveSplit
