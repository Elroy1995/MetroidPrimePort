#include "port_randomizer.h"

#include "MetroidPrime/Player/CPlayerState.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

bool sPassed = true;

void Check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "[randomizer-tests] FAILED: %s\n", message);
    sPassed = false;
  }
}

bool Contains(const std::string& text, const std::string& value) {
  return text.find(value) != std::string::npos;
}

// These tests set environment variables, which Windows spells differently and
// which needs the _s form to be safe.
bool SetEnv(const char* name, const char* value) {
#ifdef _WIN32
  return _putenv_s(name, value) == 0;
#else
  return setenv(name, value, 1) == 0;
#endif
}

bool UnsetEnv(const char* name) {
#ifdef _WIN32
  return _putenv_s(name, "") == 0;
#else
  return unsetenv(name) == 0;
#endif
}

unsigned long ProcessId() {
#ifdef _WIN32
  return static_cast<unsigned long>(GetCurrentProcessId());
#else
  return static_cast<unsigned long>(getpid());
#endif
}

// Several cases here must run in a process that has not already loaded a seed:
// the loader keeps its state in a function-local static, so a second load in the
// same process would reuse the first one's result and prove nothing. Rather than
// reach for a reload API that production has no use for, the test re-runs itself
// with the case name and the shared directory as arguments. fork/execv on POSIX,
// CreateProcessW on Windows.
int RunChild(const char* self, const char* name, const std::filesystem::path& testDir) {
  const std::string dir = testDir.string();
#ifdef _WIN32
  // The ANSI entry point takes the command line as plain bytes, which is what
  // the paths here are; the wide one would need a real encoding conversion for
  // no gain in a test.
  std::string command = "\"";
  command += self;
  command += "\" ";
  command += name;
  command += " \"";
  command += dir;
  command += "\"";
  std::vector<char> mutableCommand(command.begin(), command.end());
  mutableCommand.push_back('\0');
  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (CreateProcessA(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                     &startup, &process) == 0)
    return -1;
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return static_cast<int>(code);
#else
  const pid_t child = fork();
  if (child < 0)
    return -1;
  if (child == 0) {
    std::vector<char> selfPath(self, self + std::strlen(self));
    selfPath.push_back('\0');
    std::vector<char> caseName(name, name + std::strlen(name));
    caseName.push_back('\0');
    std::vector<char> caseDir(dir.begin(), dir.end());
    caseDir.push_back('\0');
    char* args[] = {selfPath.data(), caseName.data(), caseDir.data(), nullptr};
    execv(selfPath.data(), args);
    _exit(127);
  }
  int status = 0;
  if (waitpid(child, &status, 0) != child)
    return -1;
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

} // namespace

int main(int argc, char** argv) {
  using namespace PortRandomizer;

  // Child mode: a single case, named on the command line, sharing the parent's
  // temporary directory so the seed files it reads are the ones just written.
  if (argc == 3) {
    const std::string which = argv[1];
    const std::filesystem::path childDir = argv[2];
    if (which == "malformed") {
      if (!SetEnv("MP_RANDO_SEED", (childDir / "malformed.json").string().c_str()))
        return 2;
      EnsureLoaded();
      int item = CPlayerState::kIT_Missiles;
      int capacity = 10;
      int amount = 5;
      const bool applied = ApplyPickup(1, 2, 3, item, capacity, amount);
      return !Enabled() && !applied && item == CPlayerState::kIT_Missiles && CheckCount() == 0 ? 0 : 1;
    }
    if (which == "no-seed") {
      if (!UnsetEnv("MP_RANDO_SEED"))
        return 2;
      EnsureLoaded();
      int item = CPlayerState::kIT_Missiles;
      int capacity = 10;
      int amount = 5;
      const bool applied = ApplyPickup(1, 2, 3, item, capacity, amount);
      RecordCheck(1, 2, 3, item);
      const bool untouched = !Enabled() && !DumpEnabled() && !applied &&
                             item == CPlayerState::kIT_Missiles && capacity == 10 && amount == 5 &&
                             CheckCount() == 0 &&
                             !std::filesystem::exists(childDir / "randomizer_checks.log");
      return untouched ? 0 : 1;
    }
    if (which == "dump") {
      if (!UnsetEnv("MP_RANDO_SEED") || !SetEnv("MP_RANDO_DUMP", "1"))
        return 2;
      EnsureLoaded();
      int item = CPlayerState::kIT_Missiles;
      int capacity = 5;
      int amount = 5;
      PickupModel model;
      model.model = 0x0000ABCD;
      const bool applied = ApplyPickup(1, 2, 3, item, capacity, amount, model);
      return !applied && DumpEnabled() ? 0 : 1;
    }
    if (which == "bad-model") {
      if (!SetEnv("MP_RANDO_SEED", (childDir / "bad-model.json").string().c_str()))
        return 2;
      EnsureLoaded();
      int item = CPlayerState::kIT_Missiles;
      int capacity = 10;
      int amount = 5;
      const bool applied = ApplyPickup(1, 2, 3, item, capacity, amount);
      return !Enabled() && !applied && item == CPlayerState::kIT_Missiles ? 0 : 1;
    }
    return 3;
  }

  struct ExpectedItem {
    const char* name;
    int value;
  };
  const ExpectedItem items[] = {
      {"PowerBeam", CPlayerState::kIT_PowerBeam},
      {"IceBeam", CPlayerState::kIT_IceBeam},
      {"WaveBeam", CPlayerState::kIT_WaveBeam},
      {"PlasmaBeam", CPlayerState::kIT_PlasmaBeam},
      {"Missiles", CPlayerState::kIT_Missiles},
      {"ScanVisor", CPlayerState::kIT_ScanVisor},
      {"MorphBallBombs", CPlayerState::kIT_MorphBallBombs},
      {"PowerBombs", CPlayerState::kIT_PowerBombs},
      {"Flamethrower", CPlayerState::kIT_Flamethrower},
      {"ThermalVisor", CPlayerState::kIT_ThermalVisor},
      {"ChargeBeam", CPlayerState::kIT_ChargeBeam},
      {"SuperMissile", CPlayerState::kIT_SuperMissile},
      {"GrappleBeam", CPlayerState::kIT_GrappleBeam},
      {"XRayVisor", CPlayerState::kIT_XRayVisor},
      {"IceSpreader", CPlayerState::kIT_IceSpreader},
      {"SpaceJumpBoots", CPlayerState::kIT_SpaceJumpBoots},
      {"MorphBall", CPlayerState::kIT_MorphBall},
      {"CombatVisor", CPlayerState::kIT_CombatVisor},
      {"BoostBall", CPlayerState::kIT_BoostBall},
      {"SpiderBall", CPlayerState::kIT_SpiderBall},
      {"PowerSuit", CPlayerState::kIT_PowerSuit},
      {"GravitySuit", CPlayerState::kIT_GravitySuit},
      {"VariaSuit", CPlayerState::kIT_VariaSuit},
      {"PhazonSuit", CPlayerState::kIT_PhazonSuit},
      {"EnergyTanks", CPlayerState::kIT_EnergyTanks},
      {"UnknownItem1", CPlayerState::kIT_UnknownItem1},
      {"HealthRefill", CPlayerState::kIT_HealthRefill},
      {"UnknownItem2", CPlayerState::kIT_UnknownItem2},
      {"Wavebuster", CPlayerState::kIT_Wavebuster},
      {"Truth", CPlayerState::kIT_Truth},
      {"Strength", CPlayerState::kIT_Strength},
      {"Elder", CPlayerState::kIT_Elder},
      {"Wild", CPlayerState::kIT_Wild},
      {"Lifegiver", CPlayerState::kIT_Lifegiver},
      {"Warrior", CPlayerState::kIT_Warrior},
      {"Chozo", CPlayerState::kIT_Chozo},
      {"Nature", CPlayerState::kIT_Nature},
      {"Sun", CPlayerState::kIT_Sun},
      {"World", CPlayerState::kIT_World},
      {"Spirit", CPlayerState::kIT_Spirit},
      {"Newborn", CPlayerState::kIT_Newborn},
  };
  Check(CPlayerState::kIT_Max == 41, "retail item enum max drifted");
  for (int i = 0; i < CPlayerState::kIT_Max; ++i) {
    Check(ItemFromName(ItemName(i)) == i, "item name/type round-trip failed");
    Check(items[i].value == i, "retail item enum order drifted");
    Check(ItemFromName(items[i].name) == items[i].value, "retail item mapping drifted");
    Check(std::string(ItemName(items[i].value)) == items[i].name,
          "retail item display name drifted");
  }
  Check(ItemFromName("mIsSiLeS") == CPlayerState::kIT_Missiles,
        "item-name lookup should be case-insensitive");
  Check(ItemFromName("Missiles ") == -1 && ItemFromName("not an item") == -1,
        "item-name lookup should require an exact match");
  Check(ItemName(-1) == std::string("Unknown") && ItemName(41) == std::string("Unknown"),
        "out-of-range item should be Unknown");

  const std::filesystem::path testDir = std::filesystem::temp_directory_path() /
                                        ("mp-rando-test-" +
                                         std::to_string(static_cast<long long>(ProcessId())));
  std::filesystem::remove_all(testDir);
  std::filesystem::create_directories(testDir);
  const std::filesystem::path validSeed = testDir / "valid.json";
  const std::filesystem::path malformedSeed = testDir / "malformed.json";
  {
    std::ofstream out(validSeed);
    out << R"({"seed":"test-seed","ignored":false,"locations":{
      "00000001:00000002:00000003":{"item":"EnergyTanks","amount":4,"capacity":6,"extra":{"ok":true}},
      "00000001:00000002:00000004":{"item":"PowerBombs"}
    },"models":{
      "EnergyTanks":{"model":"0x0000ABCD","acs":"0","character":0,"animation":0},
      "PowerBombs":{"model":"00000010","acs":"00000020","character":3,"animation":7}
    }})";
  }
  {
    std::ofstream out(malformedSeed);
    out << R"({"seed":"bad","locations":{"00000001:00000002:00000003":{"item":"Missiles"},}})";
  }
  const std::filesystem::path badModelSeed = testDir / "bad-model.json";
  {
    std::ofstream out(badModelSeed);
    out << R"({"seed":"bad-model","locations":{},"models":{"Missiles":{"acs":"0"}}})";
  }
  Check(SetEnv("MP_USER_PATH", testDir.string().c_str()), "set MP_USER_PATH");
  Check(SetEnv("MP_RANDO_SEED", validSeed.string().c_str()), "set valid MP_RANDO_SEED");
  Check(UnsetEnv("MP_RANDO_DUMP"), "unset MP_RANDO_DUMP");

  // Each of these needs a process that has not already loaded a seed, so the
  // test re-runs itself rather than exposing a production reload API.
  Check(RunChild(argv[0], "malformed", testDir) == 0,
        "malformed seed must disable randomizer without partial application");
  Check(RunChild(argv[0], "no-seed", testDir) == 0,
        "without seed/dump, hooks must leave pickups untouched and not log checks");
  const int dumpResult = RunChild(argv[0], "dump", testDir);
  Check(dumpResult == 0, "dump mode must log the location and not apply a placement");
  if (dumpResult == 0) {
    std::ifstream dumpLog(testDir / "randomizer_locations.log");
    const std::string dumpText((std::istreambuf_iterator<char>(dumpLog)),
                               std::istreambuf_iterator<char>());
    Check(Contains(dumpText,
                   "LOC 00000001:00000002:00000003 Missiles amount=5 capacity=5 "
                   "model=0000ABCD acs=00000000 character=0 animation=0\n"),
          "dump line should carry the pickup model");
    std::filesystem::remove(testDir / "randomizer_locations.log");
  }
  Check(RunChild(argv[0], "bad-model", testDir) == 0,
        "a model entry without a model or acs asset must disable the seed");

  EnsureLoaded();
  Check(Enabled(), "valid seed with placements should enable randomizer");
  Check(std::string(SeedName()) == "test-seed", "seed name should load");
  Check(!DumpEnabled(), "dump mode should default off");
  char formatted[32];
  FormatLocationKey(1, 2, 3, formatted, sizeof(formatted));
  Check(std::string(formatted) == "00000001:00000002:00000003", "location key format mismatch");

  int item = CPlayerState::kIT_Missiles;
  int capacity = 2;
  int amount = 3;
  Check(ApplyPickup(1, 2, 3, item, capacity, amount), "known placement should apply");
  Check(item == CPlayerState::kIT_EnergyTanks && capacity == 6 && amount == 4,
        "placement should rewrite item, capacity, and amount");

  item = CPlayerState::kIT_Missiles;
  capacity = 7;
  amount = 8;
  Check(!ApplyPickup(9, 9, 9, item, capacity, amount), "unknown location should not apply");
  Check(item == CPlayerState::kIT_Missiles && capacity == 7 && amount == 8,
        "unknown location should leave pickup fields untouched");

  item = CPlayerState::kIT_Missiles;
  capacity = 7;
  amount = 8;
  Check(ApplyPickup(1, 2, 4, item, capacity, amount), "optional-field placement should apply");
  Check(item == CPlayerState::kIT_PowerBombs && capacity == 7 && amount == 8,
        "omitted capacity and amount must remain untouched");

  PickupModel pickupModel;
  Check(ModelForItem(CPlayerState::kIT_EnergyTanks, pickupModel) &&
            pickupModel.model == 0x0000ABCD && pickupModel.acs == 0 &&
            pickupModel.character == 0 && pickupModel.animation == 0,
        "seed model should load, accepting a 0x prefix");
  Check(ModelForItem(CPlayerState::kIT_PowerBombs, pickupModel) &&
            pickupModel.model == 0x00000010 && pickupModel.acs == 0x00000020 &&
            pickupModel.character == 3 && pickupModel.animation == 7,
        "seed model should carry an animated model's acs/character/animation");
  Check(!ModelForItem(CPlayerState::kIT_Missiles, pickupModel),
        "an item the seed has no model for should report none");

  Check(CheckCount() == 0, "check count starts at zero");
  RecordCheck(1, 2, 3, item);
  Check(CheckCount() == 1, "recorded check increments count");
  Check(Contains(StatusText(), "rando: test-seed, 1 checks"), "overlay status should report seed/count");
  std::ifstream checks(testDir / "randomizer_checks.log");
  const std::string checkLog((std::istreambuf_iterator<char>(checks)), std::istreambuf_iterator<char>());
  Check(Contains(checkLog, "CHECK 00000001:00000002:00000003 PowerBombs\n"),
        "collected check should be logged");

  std::filesystem::remove_all(testDir);
  UnsetEnv("MP_USER_PATH");
  UnsetEnv("MP_RANDO_SEED");
  UnsetEnv("MP_RANDO_DUMP");
  if (!sPassed)
    return 1;
  std::puts("[randomizer-tests] passed");
  return 0;
}
