#include "port_ap_protocol.h"

#include "port_randomizer.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

bool sPassed = true;

void Check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "[ap-tests] FAILED: %s\n", message);
    sPassed = false;
  }
}

bool Contains(const std::string& text, const std::string& value) {
  return text.find(value) != std::string::npos;
}

PortJson::Value Packet(const std::string& json) {
  PortJson::Value value;
  size_t offset = 0;
  const char* reason = nullptr;
  if (!PortJson::Parse(json, value, offset, &reason)) {
    Check(false, "test packet JSON parses");
  }
  return value;
}

std::string Read(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

// A per-process temporary directory, so parallel test runs cannot collide.
unsigned long TestProcessId() {
#ifdef _WIN32
  return static_cast<unsigned long>(_getpid());
#else
  return static_cast<unsigned long>(getpid());
#endif
}

} // namespace

int main() {
  using namespace PortAp::Protocol;
  const std::filesystem::path testDir = std::filesystem::temp_directory_path() /
      ("mp-ap-test-" + std::to_string(static_cast<long long>(TestProcessId())));
  std::filesystem::remove_all(testDir);
  std::filesystem::create_directories(testDir);

  const std::string fullConfig = R"({
    "server":"ws://127.0.0.1:38281", "slot":"Player1", "password":"secret",
    "game":"Metroid Prime", "items_handling":7,
    "version":{"major":0,"minor":6,"build":0}, "tags":["AP"],
    "locations":{
      "39F2DE28:B2701146:0000007E":123456,
      "39F2DE28:B2701146:0000007F":123456,
      "39F2DE28:B2701146:00000080":123457
    },
    "items":{
      "100":{"item":"EnergyTanks","display":"Energy Tank","amount":2,"capacity":3},
      "101":{"item":"Missiles"}
    },
    "future_setting":{"ignored":true}
  })";
  Config config = ParseConfig(fullConfig);
  Check(config.valid && config.error.empty(), "complete configuration parses");
  Check(config.server == "ws://127.0.0.1:38281" && config.slot == "Player1" &&
            config.password == "secret" && config.game == "Metroid Prime" &&
            config.itemsHandling == 7,
        "configuration fields parse");
  Check(config.versionMajor == 0 && config.versionMinor == 6 && config.versionBuild == 0 &&
            config.tags.size() == 1 && config.tags[0] == "AP",
        "version and tags parse");
  Check(config.locations.size() == 3 && config.items.size() == 2,
        "location and item maps parse");
  Check(config.items.at(100).itemType == PortRandomizer::ItemFromName("EnergyTanks") &&
            config.items.at(100).amount == 2 && config.items.at(100).capacity == 3 &&
            config.items.at(100).display == "Energy Tank" &&
            config.items.at(101).itemType == PortRandomizer::ItemFromName("Missiles") &&
            config.items.at(101).amount == 1 && config.items.at(101).capacity == 1 &&
            config.items.at(101).display == "Missiles",
        "item types, display names and optional defaults parse");

  Config missingServer = ParseConfig(R"({"slot":"Player1"})");
  Config missingSlot = ParseConfig(R"({"server":"ws://localhost"})");
  Check(!missingServer.valid && Contains(missingServer.error, "server"),
        "missing server is a clear configuration error");
  Check(!missingSlot.valid && Contains(missingSlot.error, "slot"),
        "missing slot is a clear configuration error");
  Config badLocation = ParseConfig(
      R"({"server":"ws://localhost","slot":"P","locations":{"not-a-key":1}})");
  Check(!badLocation.valid && Contains(badLocation.error, "location key"),
        "bad location key is rejected");
  Config badLocationId = ParseConfig(
      R"({"server":"ws://localhost","slot":"P","locations":{"00000001:00000002:00000003":1.5}})");
  Check(!badLocationId.valid && Contains(badLocationId.error, "integer"),
        "non-integer location id is rejected");
  Config unknownItem = ParseConfig(
      R"({"server":"ws://localhost","slot":"P","items":{"12":{"item":"Unobtainium"}}})");
  Check(!unknownItem.valid && Contains(unknownItem.error, "Unobtainium"),
        "unknown item name is rejected and named");
  Config defaults = ParseConfig(R"({"server":"ws://localhost","slot":"Default"})");
  Check(defaults.valid && defaults.game == "Metroid Prime" && defaults.password.empty() &&
            defaults.itemsHandling == 7 && defaults.versionMajor == 0 &&
            defaults.versionMinor == 6 && defaults.versionBuild == 0 && defaults.tags.empty() &&
            defaults.tlsCa.empty(),
        "optional configuration defaults apply");
  Config tlsCa = ParseConfig(
      R"({"server":"wss://archipelago.gg:38281","slot":"P","tls_ca":"certs/ca.pem"})");
  Check(tlsCa.valid && tlsCa.tlsCa == "certs/ca.pem", "tls_ca path parses as written");
  Config badTlsCa = ParseConfig(R"({"server":"wss://localhost","slot":"P","tls_ca":true})");
  Check(!badTlsCa.valid && Contains(badTlsCa.error, "tls_ca"),
        "non-string tls_ca is rejected and named");
  Config unknownTop = ParseConfig(R"({"server":"ws://localhost","slot":"P","extra":42})");
  Check(unknownTop.valid, "unknown top-level configuration key is ignored");

  const std::filesystem::path configPath = testDir / "full.json";
  {
    std::ofstream file(configPath);
    file << fullConfig;
  }
  const Config loadedConfig = LoadConfigFile(configPath.string());
  Check(loadedConfig.valid, "configuration loads from a file");
  const Config absentConfig = LoadConfigFile((testDir / "missing.json").string());
  Check(!absentConfig.valid && Contains(absentConfig.error, "missing.json"),
        "missing configuration file reports its path");

  Session session(config, State{});
  int64_t checkedId = -1;
  Check(!session.MarkLocationChecked("unknown", checkedId), "unknown location cannot be checked");
  Check(session.MarkLocationChecked("39F2DE28:B2701146:0000007E", checkedId) &&
            checkedId == 123456,
        "mapped location returns its ID and records it");
  Check(!session.MarkLocationChecked("39F2DE28:B2701146:0000007E", checkedId),
        "a recorded location cannot be checked twice");
  const std::vector<int64_t> allIds = session.AllLocationIds();
  Check(allIds == std::vector<int64_t>({123456, 123457}),
        "all location IDs are deduplicated and sorted");

  const std::string connect = session.BuildConnect();
  Check(Contains(connect, "\"name\":\"Player1\"") &&
            Contains(connect, "\"game\":\"Metroid Prime\"") &&
            Contains(connect, "\"items_handling\":7") &&
            Contains(connect, "\"class\":\"Version\"") &&
            Contains(connect, "\"major\":0") && Contains(connect, "\"minor\":6"),
        "Connect packet contains slot, game, handling and version");
  Check(connect == session.BuildConnect(), "Connect UUID stays stable for the process");
  Check(Session::BuildLocationChecks({7, 8}) ==
            R"({"cmd":"LocationChecks","locations":[7,8]})",
        "LocationChecks packet formatting");
  Check(Session::BuildSync() == R"({"cmd":"Sync"})", "Sync packet formatting");

  State initial;
  initial.checkedLocations.push_back(123456);
  Session packets(config, initial);
  std::vector<std::string> outgoing;
  std::vector<ItemGrant> grants;
  packets.HandlePacket(Packet(R"({"cmd":"RoomInfo","seed_name":"MP Seed Alpha"})"),
                       outgoing, grants);
  Check(packets.SeedName() == "MP Seed Alpha", "RoomInfo captures the seed name");
  packets.HandlePacket(Packet(
      R"({"cmd":"Connected","team":0,"slot":3,"players":[{"team":0,"slot":1,"alias":"Bob","name":"Bob's name"},{"team":0,"slot":3,"alias":"","name":"Player1"}],"checked_locations":[123457,123456,999]})"),
      outgoing, grants);
  Check(packets.HandshakeComplete() && packets.SlotDescription() == "slot 3, team 0",
        "Connected completes handshake and records slot description");
  Check(packets.PlayerName(1) == "Bob" && packets.PlayerName(3) == "Player1" &&
            packets.PlayerName(44) == "player 44",
        "Connected captures player aliases, falls back to name, and names unknown slots");
  Check(packets.GetState().checkedLocations == std::vector<int64_t>({123456, 123457, 999}),
        "Connected checked locations merge without duplicates");

  packets.HandlePacket(Packet(
      R"({"cmd":"ReceivedItems","index":0,"items":[{"item":100,"location":1,"player":1,"flags":0}]})"),
      outgoing, grants);
  Check(grants.size() == 1 && grants.back().itemId == 100 &&
            grants.back().itemType == PortRandomizer::ItemFromName("EnergyTanks") &&
            grants.back().amount == 2 && grants.back().capacity == 3 &&
            grants.back().display == "Energy Tank",
        "ReceivedItems object grants configured item fields");
  packets.HandlePacket(Packet(
      R"({"cmd":"ReceivedItems","index":1,"items":[[101,2,3,0]]})"),
      outgoing, grants);
  Check(grants.size() == 2 && grants.back().itemId == 101 &&
            grants.back().itemType == PortRandomizer::ItemFromName("Missiles") &&
            grants.back().amount == 1 && grants.back().capacity == 1 &&
            grants.back().display == "Missiles",
        "ReceivedItems four-element array grants configured item");
  packets.HandlePacket(Packet(R"({"cmd":"ReceivedItems","index":0,"items":[[100,1,1,0]]})"),
                      outgoing, grants);
  Check(grants.size() == 2 && packets.GetState().nextItemIndex == 2,
        "already processed item index is not granted twice");
  packets.HandlePacket(Packet(R"({"cmd":"ReceivedItems","index":2,"items":[[999,1,3,0]]})"),
                      outgoing, grants);
  Check(grants.size() == 3 && grants.back().itemType == -1 &&
            packets.LastError() == "unknown item id 999",
        "unknown received item is counted and reported");
  std::string notification;
  Check(packets.TakeNotification(notification) && notification == "Energy Tank from Bob",
        "received item notification includes the sender alias");
  Check(packets.TakeNotification(notification) && notification == "Missiles",
        "received item from our own slot has no sender suffix");
  Check(packets.TakeNotification(notification) && notification == "unknown item 999",
        "unknown item notification names its ID");
  Check(!packets.TakeNotification(notification), "notification queue reports empty after draining");
  packets.HandlePacket(Packet(R"({"cmd":"ReceivedItems","index":5,"items":[]})"),
                      outgoing, grants);
  Check(packets.Desynced() && packets.GetState().nextItemIndex == 5 &&
            !outgoing.empty() && outgoing.back() == Session::BuildSync(),
        "item index jump marks desync and queues Sync");

  std::string printParts = R"({"cmd":"PrintJSON","data":[{"text":"Hello"},{"text":"\nworld"}]})";
  packets.HandlePacket(Packet(printParts), outgoing, grants);
  Check(packets.LastMessage() == "Hello world", "PrintJSON joins text and flattens newlines");
  Check(packets.TakeNotification(notification) && notification == "Hello world",
        "PrintJSON text is queued as a notification");
  std::string longText(250, 'x');
  const std::string printLong = "{\"cmd\":\"PrintJSON\",\"data\":[{\"text\":\"" + longText + "\"}]}";
  packets.HandlePacket(Packet(printLong), outgoing, grants);
  Check(packets.LastMessage().size() == 203 && packets.LastMessage().substr(200) == "...",
        "PrintJSON truncates long output and marks the cut");
  Check(packets.TakeNotification(notification) && notification == packets.LastMessage(),
        "truncated PrintJSON message is queued as a notification");

  Session cappedNotifications(config, State{});
  for (int i = 0; i < 40; ++i) {
    const std::string message = "message " + std::to_string(i);
    cappedNotifications.HandlePacket(
        Packet("{\"cmd\":\"PrintJSON\",\"data\":[{\"text\":\"" + message + "\"}]}"),
        outgoing, grants);
  }
  bool newestThirtyTwo = true;
  for (int i = 8; i < 40; ++i) {
    newestThirtyTwo = cappedNotifications.TakeNotification(notification) &&
                      notification == "message " + std::to_string(i) && newestThirtyTwo;
  }
  Check(newestThirtyTwo && !cappedNotifications.TakeNotification(notification),
        "notification queue keeps only the newest 32 entries");
  packets.HandlePacket(Packet(R"({"cmd":"InvalidPacket","text":"bad command"})"), outgoing, grants);
  Check(packets.LastError() == "bad command", "InvalidPacket records text as error");
  packets.HandlePacket(Packet(R"({"cmd":"ConnectionRefused","errors":["bad password","unknown slot"]})"),
                      outgoing, grants);
  Check(!packets.HandshakeComplete() && packets.LastError() == "bad password, unknown slot",
        "ConnectionRefused joins error messages and clears handshake");

  State saved;
  saved.slot = "Player1";
  saved.seed = "MP Seed Alpha";
  saved.nextItemIndex = 23;
  saved.checkedLocations = {8, 10, 12};
  const std::filesystem::path statePath = testDir / "nested" / "archipelago_state.json";
  Check(SaveStateFile(statePath.string(), saved), "state save creates parent directory");
  const State reloaded = LoadStateFile(statePath.string());
  Check(reloaded.slot == saved.slot && reloaded.nextItemIndex == saved.nextItemIndex &&
            reloaded.checkedLocations == saved.checkedLocations && reloaded.seed == saved.seed,
        "saved state round-trips");
  const std::filesystem::path emptyStatePath = testDir / "empty-state.json";
  const State absentState = LoadStateFile(emptyStatePath.string());
  Check(absentState.slot.empty() && absentState.nextItemIndex == 0 &&
            absentState.checkedLocations.empty(),
        "absent state file is an empty state");
  Check(SaveStateFile(emptyStatePath.string(), State{}), "empty state saves");
  const State emptyReloaded = LoadStateFile(emptyStatePath.string());
  Check(emptyReloaded.nextItemIndex == 0 && emptyReloaded.checkedLocations.empty(),
        "empty state round-trips");
  Check(emptyReloaded.progressive.empty() && Contains(Read(emptyStatePath), "\"progressive\":{}"),
        "empty state writes an empty progressive map");

  // Progress belongs to the session that granted it. A different seed must not
  // inherit it, or the client claims locations it never collected and skips the
  // items the server still owes it.
  {
    const std::string configText = R"json({
      "server":"ws://localhost:38281", "slot":"P",
      "items":{"5031004":{"item":"Missiles","amount":5,"capacity":5}},
      "locations":{"39F2DE28:B2701146:0000007E":5031101}
    })json";
    const Config config = ParseConfig(configText);
    Check(config.valid, "seed-mismatch config parses");

    State carried;
    carried.slot = "P";
    carried.seed = "MP Seed Alpha";
    carried.nextItemIndex = 7;
    carried.checkedLocations = {5031101};
    carried.progressive[5031043] = 2;
    Session carriedSession(config, carried);
    std::vector<std::string> seedOutgoing;
    std::vector<ItemGrant> seedGrants;
    carriedSession.HandlePacket(Packet(R"({"cmd":"RoomInfo","seed_name":"MP Seed Beta"})"),
                                seedOutgoing, seedGrants);
    const State& afterSwitch = carriedSession.GetState();
    Check(afterSwitch.nextItemIndex == 0 && afterSwitch.checkedLocations.empty() &&
              afterSwitch.progressive.empty(),
          "a different seed discards the recorded checks, item index and progressive counts");
    Check(afterSwitch.seed == "MP Seed Beta", "the new seed is recorded");
    Check(carriedSession.SeedName() == "MP Seed Beta" && carriedSession.SeedName() != "",
          "SeedName still reports the server's seed");
    Check(carriedSession.ResetReason().find("MP Seed Alpha") != std::string::npos &&
              carriedSession.ResetReason().find("MP Seed Beta") != std::string::npos,
          "the reset says which seed the progress belonged to");
    const std::filesystem::path switchedPath = testDir / "switched-state.json";
    Check(SaveStateFile(switchedPath.string(), afterSwitch) &&
              LoadStateFile(switchedPath.string()).nextItemIndex == 0,
          "the reset progress is what gets written back");

    Session sameSession(config, carried);
    std::vector<std::string> sameOutgoing;
    std::vector<ItemGrant> sameGrants;
    sameSession.HandlePacket(Packet(R"({"cmd":"RoomInfo","seed_name":"MP Seed Alpha"})"), sameOutgoing,
                             sameGrants);
    const State& afterSame = sameSession.GetState();
    Check(afterSame.nextItemIndex == 7 && afterSame.checkedLocations ==
                std::vector<int64_t>({5031101}) && afterSame.progressive.count(5031043) == 1 &&
              afterSame.progressive.at(5031043) == 2,
          "the same seed keeps the recorded progress");
    Check(sameSession.ResetReason().empty(), "no reset is reported when the seed matches");

    State legacy;
    legacy.slot = "P";
    legacy.nextItemIndex = 4;
    legacy.checkedLocations = {5031101};
    Session legacySession(config, legacy);
    std::vector<std::string> legacyOutgoing;
    std::vector<ItemGrant> legacyGrants;
    legacySession.HandlePacket(Packet(R"({"cmd":"RoomInfo","seed_name":"MP Seed Alpha"})"),
                               legacyOutgoing, legacyGrants);
    Check(legacySession.GetState().nextItemIndex == 4 &&
              legacySession.GetState().seed == "MP Seed Alpha" && legacySession.ResetReason().empty(),
          "a state file with no recorded seed adopts the server's without discarding progress");

    // A file written before the seed existed still loads, with an unknown seed.
    const std::filesystem::path legacyPath = testDir / "legacy-state.json";
    {
      std::ofstream legacyFile(legacyPath, std::ios::binary | std::ios::trunc);
      legacyFile << R"({"slot":"P","next_item_index":5,"checked_locations":[5031101]})";
    }
    const State legacyLoaded = LoadStateFile(legacyPath.string());
    Check(legacyLoaded.seed.empty() && legacyLoaded.nextItemIndex == 5,
          "a pre-seed state file loads with an unknown seed and keeps its progress");
  }

  const std::string progressiveConfigText = R"json({
    "server":"ws://localhost", "slot":"P",
    "items":{
      "5031004":{"item":"Missiles","amount":5,"capacity":5,"display":"Missile Expansion"},
      "5031043":{"progressive":[
        {"item":"PowerBeam","amount":1,"capacity":1,"display":"Power Beam"},
        {"item":"ChargeBeam","amount":1,"capacity":1,"display":"Charge Beam"},
        {"item":"SuperMissile","amount":1,"capacity":1,"display":"Super Missile"}]},
      "5031047":{"item":"ChargeBeam","display":"Charge Beam (Power)"}
    }
  })json";
  const Config progressiveConfig = ParseConfig(progressiveConfigText);
  Check(progressiveConfig.valid && progressiveConfig.items.size() == 3,
        "progressive configuration parses");
  const ItemEntry& powerEntry = progressiveConfig.items.at(5031043);
  Check(powerEntry.IsProgressive() && powerEntry.progressive.size() == 3 &&
            powerEntry.progressive[0].itemType == PortRandomizer::ItemFromName("PowerBeam") &&
            powerEntry.progressive[1].itemType == PortRandomizer::ItemFromName("ChargeBeam") &&
            powerEntry.progressive[2].itemType == PortRandomizer::ItemFromName("SuperMissile") &&
            powerEntry.progressive[2].display == "Super Missile" &&
            powerEntry.progressive[2].itemId == 5031043,
        "progressive entry keeps its steps in order");
  Check(powerEntry.itemType == powerEntry.progressive[0].itemType &&
            powerEntry.display == "Power Beam",
        "progressive entry's flat fields mirror step 0");
  Check(!progressiveConfig.items.at(5031004).IsProgressive() &&
            progressiveConfig.items.at(5031004).amount == 5 &&
            progressiveConfig.items.at(5031047).itemType ==
                PortRandomizer::ItemFromName("ChargeBeam"),
        "flat entries still parse alongside progressive ones");

  Session progressive(progressiveConfig, State{});
  std::vector<ItemGrant> progressiveGrants;
  progressive.HandlePacket(Packet(
      R"({"cmd":"ReceivedItems","index":0,"items":[[5031043,1,1,0],[5031004,2,1,0],[5031043,3,1,0]]})"),
      outgoing, progressiveGrants);
  progressive.HandlePacket(Packet(
      R"({"cmd":"ReceivedItems","index":3,"items":[[999,4,1,0],[5031043,5,1,0]]})"),
      outgoing, progressiveGrants);
  Check(progressiveGrants.size() == 5 &&
            progressiveGrants[0].itemType == PortRandomizer::ItemFromName("PowerBeam") &&
            progressiveGrants[0].display == "Power Beam" &&
            progressiveGrants[1].itemType == PortRandomizer::ItemFromName("Missiles") &&
            progressiveGrants[1].amount == 5 &&
            progressiveGrants[2].itemType == PortRandomizer::ItemFromName("ChargeBeam") &&
            progressiveGrants[2].display == "Charge Beam" &&
            progressiveGrants[3].itemType == -1 &&
            progressiveGrants[4].itemType == PortRandomizer::ItemFromName("SuperMissile") &&
            progressiveGrants[4].display == "Super Missile" &&
            progressiveGrants[4].itemId == 5031043,
        "progressive copies grant steps 1, 2 and 3 in order");
  Check(progressive.GetState().progressive.size() == 1 &&
            progressive.GetState().progressive.at(5031043) == 3,
        "progressive count tracks copies and ignores flat and unknown ids");
  const char* expectedNotifications[] = {"Power Beam", "Missile Expansion", "Charge Beam",
                                         "unknown item 999", "Super Missile"};
  bool notificationsMatch = true;
  for (const char* expected : expectedNotifications)
    notificationsMatch = progressive.TakeNotification(notification) &&
                         notification == expected && notificationsMatch;
  Check(notificationsMatch, "notifications name the progressive step actually granted");

  // The tracker keeps the session's receipts so one that arrived while the
  // player was not watching the HUD is still readable. It is the same five
  // grants, with the step of a progressive sequence and who sent it.
  const std::vector< TrackedItem >& tracked = progressive.Tracked();
  Check(tracked.size() == 5, "the tracker records every grant, including unknown items");
  Check(tracked[0].name == "Power Beam" && tracked[0].step == 1 && tracked[0].total == 3 &&
            tracked[1].name == "Missile Expansion" && tracked[1].total == 1 &&
            tracked[2].name == "Charge Beam" && tracked[2].step == 2 && tracked[2].total == 3 &&
            tracked[3].name == "item 999" && tracked[4].name == "Super Missile" &&
            tracked[4].step == 3 && tracked[4].total == 3,
        "the tracker names each grant and its progressive step");
  bool noSender = true;
  for (const TrackedItem& item : tracked)
    noSender = item.from.empty() && noSender;
  Check(noSender, "items the player sent themselves have no sender");
  // A copy past the last step repeats it rather than counting past the end.
  // A fresh session, because the state carries the count of 3 across.
  {
    Session repeated(progressiveConfig, State{});
    std::vector<ItemGrant> repeatedGrants;
    repeated.HandlePacket(Packet(
        R"({"cmd":"ReceivedItems","index":0,"items":[[5031043,1,1,0],[5031043,2,1,0],)"
        R"([5031043,3,1,0],[5031043,4,1,0],[5031043,5,1,0]]})"),
        outgoing, repeatedGrants);
    const std::vector< TrackedItem >& more = repeated.Tracked();
    Check(more.size() == 5 && more[0].step == 1 && more[2].step == 3 && more[3].step == 3 &&
              more[4].step == 3 && more[4].name == "Super Missile",
          "copies past the last step stay on it");
  }
  {
    // Another player's item records who sent it, using the alias from Connected.
    Session shared(progressiveConfig, State{});
    std::vector<ItemGrant> sharedGrants;
    shared.HandlePacket(Packet(
                            R"({"cmd":"Connected","slot":1,"team":0,"players":)"
                            R"([{"slot":1,"alias":"Me"},{"slot":2,"alias":"Bob"}]})"),
                        outgoing, sharedGrants);
    shared.HandlePacket(
        Packet(R"({"cmd":"ReceivedItems","index":0,"items":[[5031004,1,2,0],[5031004,2,1,0]]})"),
        outgoing, sharedGrants);
    const std::vector< TrackedItem >& fromOthers = shared.Tracked();
    Check(fromOthers.size() == 2 && fromOthers[0].from == "Bob" && fromOthers[1].from.empty(),
          "an item from another player names them, one's own does not");
  }

  const std::filesystem::path progressiveStatePath = testDir / "progressive-state.json";
  State progressiveState = progressive.GetState();
  progressiveState.slot = "P";
  Check(SaveStateFile(progressiveStatePath.string(), progressiveState) &&
            Contains(Read(progressiveStatePath), "\"progressive\":{\"5031043\":3}"),
        "progressive counts are written to the state file");
  const State progressiveReloaded = LoadStateFile(progressiveStatePath.string());
  Check(progressiveReloaded.nextItemIndex == 5 &&
            progressiveReloaded.progressive == progressiveState.progressive,
        "progressive counts round-trip through the state file");

  Session resumed(progressiveConfig, progressiveReloaded);
  progressiveGrants.clear();
  resumed.HandlePacket(Packet(
      R"({"cmd":"ReceivedItems","index":0,"items":[[5031043,1,1,0],[5031004,2,1,0],[5031043,3,1,0],[999,4,1,0],[5031043,5,1,0],[5031043,6,1,0]]})"),
      outgoing, progressiveGrants);
  Check(progressiveGrants.size() == 1 &&
            progressiveGrants[0].itemType == PortRandomizer::ItemFromName("SuperMissile") &&
            progressiveGrants[0].display == "Super Missile" &&
            resumed.GetState().progressive.at(5031043) == 4,
        "after reload a 4th copy stays at the last step and old copies are skipped");

  State stale;
  stale.progressive[5031043] = 2;
  Session fresh(progressiveConfig, stale);
  progressiveGrants.clear();
  fresh.HandlePacket(Packet(R"({"cmd":"ReceivedItems","index":0,"items":[[5031043,1,1,0]]})"),
                     outgoing, progressiveGrants);
  Check(progressiveGrants.size() == 1 &&
            progressiveGrants[0].itemType == PortRandomizer::ItemFromName("PowerBeam") &&
            fresh.GetState().progressive.at(5031043) == 1,
        "a fresh inventory restarts progressive counts");

  State saturated;
  saturated.progressive[5031043] = std::numeric_limits<int64_t>::max();
  saturated.nextItemIndex = 1;
  Session capped(progressiveConfig, saturated);
  progressiveGrants.clear();
  capped.HandlePacket(Packet(R"({"cmd":"ReceivedItems","index":1,"items":[[5031043,1,1,0]]})"),
                      outgoing, progressiveGrants);
  Check(progressiveGrants.size() == 1 &&
            progressiveGrants[0].itemType == PortRandomizer::ItemFromName("SuperMissile") &&
            capped.GetState().progressive.at(5031043) == std::numeric_limits<int64_t>::max(),
        "progressive count saturates instead of overflowing");

  const std::filesystem::path oldStatePath = testDir / "old-state.json";
  {
    std::ofstream file(oldStatePath);
    file << R"({"slot":"P","next_item_index":4,"checked_locations":[1]})";
  }
  const State oldState = LoadStateFile(oldStatePath.string());
  Check(oldState.nextItemIndex == 4 && oldState.progressive.empty(),
        "a state file without progressive counts loads with zero counts");

  const Config emptyProgressive = ParseConfig(
      R"({"server":"ws://localhost","slot":"P","items":{"5031043":{"progressive":[]}}})");
  Check(!emptyProgressive.valid && Contains(emptyProgressive.error, "5031043") &&
            Contains(emptyProgressive.error, "progressive"),
        "empty progressive list is rejected and names the id");
  const Config badStep = ParseConfig(
      R"({"server":"ws://localhost","slot":"P","items":{"5031044":{"progressive":[{"item":"IceBeam"},{"amount":1}]}}})");
  Check(!badStep.valid && Contains(badStep.error, "5031044") &&
            Contains(badStep.error, "step 2"),
        "progressive step without an item name is rejected and named");
  const Config unknownStep = ParseConfig(
      R"({"server":"ws://localhost","slot":"P","items":{"5031045":{"progressive":[{"item":"Unobtainium"}]}}})");
  Check(!unknownStep.valid && Contains(unknownStep.error, "Unobtainium"),
        "unknown item name in a progressive step is rejected");
  const Config neither = ParseConfig(
      R"({"server":"ws://localhost","slot":"P","items":{"5031046":{"display":"Nothing"}}})");
  Check(!neither.valid && Contains(neither.error, "5031046"),
        "entry with neither item nor progressive is rejected and names the id");
  const Config both = ParseConfig(
      R"({"server":"ws://localhost","slot":"P","items":{"5031046":{"item":"PlasmaBeam","progressive":[{"item":"PlasmaBeam"}]}}})");
  Check(!both.valid && Contains(both.error, "5031046"),
        "entry with both item and progressive is rejected");

  std::filesystem::remove_all(testDir);
  if (!sPassed)
    return 1;
  std::puts("[ap-tests] passed");
  return 0;
}
