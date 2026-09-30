#include "port_ap_metroidprime.h"
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

  // MarkLocationChecked answers false for two different reasons, and only one is
  // a fault. KnowsLocation is what tells them apart, and the client uses it to
  // report a location the table has never heard of instead of dropping it in
  // silence - a session that collects everything and reports nothing looks, from
  // the player's side, exactly like a session that is working.
  Check(!session.KnowsLocation("unknown"),
        "a location absent from the table is not known");
  Check(session.KnowsLocation("39F2DE28:B2701146:0000007E"),
        "a location in the table stays known after being recorded");
  Check(!session.KnowsLocation(""),
        "an empty key is not known");
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
  packets.HandlePacket(Packet(R"({"cmd":"ReceivedItems","index":5,"items":[[101,2,3,0]]})"),
                      outgoing, grants);
  Check(packets.Desynced() && packets.GetState().nextItemIndex == 3 && grants.size() == 3 &&
            !outgoing.empty() && outgoing.back() == Session::BuildSync(),
        "item index jump marks desync, queues Sync, and holds the items past the gap");
  packets.HandlePacket(
      Packet(R"({"cmd":"ReceivedItems","index":0,"items":)"
             R"([[100,1,1,0],[101,2,3,0],[999,1,3,0],[100,4,5,0],[101,6,7,0],[100,8,9,0]]})"),
      outgoing, grants);
  Check(!packets.Desynced() && packets.GetState().nextItemIndex == 6 && grants.size() == 6 &&
            grants[3].itemId == 100 && grants[4].itemId == 101 && grants[5].itemId == 100,
        "the Sync reply grants the gap and the held items once each");
  while (packets.TakeNotification(notification)) {
  }

  std::string printParts = R"({"cmd":"PrintJSON","data":[{"text":"Hello"},{"text":"\nworld"}]})";
  packets.HandlePacket(Packet(printParts), outgoing, grants);
  Check(packets.LastMessage() == "Hello world", "PrintJSON joins text and flattens newlines");
  Check(packets.TakeNotification(notification) && notification == "Hello world",
        "PrintJSON text is queued as a notification");
  {
    ChatLine line;
    while (packets.TakeChatLine(line)) {
    }
    packets.HandlePacket(
        Packet(R"({"cmd":"PrintJSON","type":"CommandResult","data":[{"text":"line one\nline two\n"}]})"),
        outgoing, grants);
    Check(packets.TakeChatLine(line) && line.type == "CommandResult" &&
              line.text == "line one\nline two" && !packets.TakeChatLine(line),
          "the chat log keeps a reply's line breaks, without the trailing one");
    while (packets.TakeNotification(notification)) {
    }
  }
  Check(Session::BuildSay("hi \"all\"\n") == R"({"cmd":"Say","text":"hi \"all\"\n"})",
        "Say packet formatting escapes the text");
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

  // The overlay's Connect screen rewrites archipelago.json; the rest of the
  // file is the player's and must survive it.
  {
    const std::filesystem::path connectionPath = testDir / "connect" / "archipelago.json";
    Connection connection;
    connection.server = "archipelago.gg:38281";
    connection.slot = "Samus";
    std::string error;
    Check(SaveConnectionFile(connectionPath.string(), connection, error),
          "connection saves to a new file");
    const Config fresh = LoadConfigFile(connectionPath.string());
    Check(fresh.valid && fresh.builtin && fresh.server == "archipelago.gg:38281" &&
              fresh.slot == "Samus" && fresh.password.empty(),
          "a saved connection alone is a valid built-in configuration");
    Check(!Contains(Read(connectionPath), "password") && !Contains(Read(connectionPath), "enabled"),
          "an empty password and an enabled connection are not written");

    {
      std::ofstream file(connectionPath, std::ios::binary | std::ios::trunc);
      file << R"({"slot":"Old","death_link":true,"tls_ca":"ca.pem","extra":[1,2.5,null,"x\"y"],)"
              R"("locations":{"39F2DE28:B2701146:0000007E":5031158},"server":"ws://a:1"})";
    }
    connection.password = "pw";
    Check(SaveConnectionFile(connectionPath.string(), connection, error),
          "connection saves over an existing file");
    const std::string rewritten = Read(connectionPath);
    Check(Contains(rewritten, R"("slot":"Samus")") && Contains(rewritten, R"("death_link":true)") &&
              Contains(rewritten, R"("tls_ca":"ca.pem")") &&
              Contains(rewritten, R"("extra":[1,2.5,null,"x\"y"])") &&
              Contains(rewritten, R"("39F2DE28:B2701146:0000007E":5031158)") &&
              Contains(rewritten, R"("password":"pw")") &&
              rewritten.find("\"slot\"") < rewritten.find("\"death_link\""),
          "other keys and their order survive a connection save");
    const Config merged = LoadConfigFile(connectionPath.string());
    Check(merged.valid && merged.deathLink && merged.password == "pw" &&
              merged.locations.size() == 1,
          "the merged file still parses as the same configuration");

    connection.enabled = false;
    Check(SaveConnectionFile(connectionPath.string(), connection, error),
          "a disconnect saves");
    const Config disabled = LoadConfigFile(connectionPath.string());
    const Connection kept = LoadConnectionFile(connectionPath.string());
    Check(!disabled.valid && Contains(disabled.error, "enabled"),
          "a disconnected configuration does not start the client");
    Check(!kept.enabled && kept.server == "archipelago.gg:38281" && kept.slot == "Samus" &&
              kept.password == "pw",
          "a disconnected configuration keeps its details for the Connect screen");
    connection.enabled = true;
    Check(SaveConnectionFile(connectionPath.string(), connection, error) &&
              LoadConfigFile(connectionPath.string()).valid,
          "connecting again re-enables it");

    connection.seed = "S1";
    connection.lastPlayed = 1790000000;
    Check(SaveConnectionFile(connectionPath.string(), connection, error),
          "a connection with a seed saves");
    const Connection seeded = LoadConnectionFile(connectionPath.string());
    Check(seeded.seed == "S1" && seeded.lastPlayed == 1790000000 &&
              Contains(Read(connectionPath), R"("last_played":1790000000)"),
          "the seed and the last played time round-trip");
    connection.seed.clear();
    connection.lastPlayed = 0;
    Check(SaveConnectionFile(connectionPath.string(), connection, error) &&
              !Contains(Read(connectionPath), "seed") &&
              !Contains(Read(connectionPath), "last_played"),
          "an unknown seed and time are removed, not written empty");

    {
      std::ofstream file(connectionPath, std::ios::binary | std::ios::trunc);
      file << "{ not json";
    }
    Check(!SaveConnectionFile(connectionPath.string(), connection, error) && !error.empty() &&
              Read(connectionPath) == "{ not json",
          "a broken file is reported, not overwritten");
    const Connection absent = LoadConnectionFile((testDir / "none.json").string());
    Check(absent.server.empty() && absent.slot.empty() && absent.enabled,
          "an absent file has no connection");
  }

  // Each slot's game in each seed has a directory: plain names stay readable,
  // anything else is made safe and told apart by a hash.
  {
    Check(GameDirectoryName("Samus", "12345678901234567890") == "Samus-12345678901234567890",
          "a plain slot and seed name the directory as they are");
    const std::string odd = GameDirectoryName("Sa/mus", "..");
    Check(odd.find('/') == std::string::npos && odd.rfind("Sa_mus-_..-", 0) == 0 &&
              odd.size() == std::string("Sa_mus-_..-").size() + 8,
          "separators and dot names are replaced, with a hash appended");
    Check(GameDirectoryName("a/b", "s") != GameDirectoryName("a?b", "s"),
          "names that clean to the same text stay apart");
    Check(GameDirectoryName(std::string(60, 'x'), "s").size() == 40 + 3 + 8,
          "a long slot is cut to 40 bytes");
  }

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
    // A loaded save holding the first two of five received items: the session
    // rewinds, and the full replay grants only the three the save is missing,
    // with the progressive steps counted from the items it already holds.
    const std::string allFive = R"({"cmd":"ReceivedItems","index":0,"items":[[5031043,1,1,0],)"
                                R"([5031004,2,1,0],[5031043,3,1,0],[999,4,1,0],[5031043,5,1,0]]})";
    Session rewound(progressiveConfig, State{});
    std::vector<ItemGrant> firstRun;
    rewound.HandlePacket(Packet(allFive), outgoing, firstRun);
    Check(firstRun.size() == 5 && firstRun[0].index == 0 && firstRun[4].index == 4,
          "grants carry their received-item index");
    while (rewound.TakeNotification(notification)) {
    }
    rewound.RewindTo(2);
    Check(rewound.GetState().nextItemIndex == 0 && rewound.GetState().progressive.empty() &&
              rewound.Tracked().empty(),
          "a rewind clears the item index, progressive counts and tracker");
    std::vector<ItemGrant> replayed;
    outgoing.clear();
    rewound.HandlePacket(Packet(R"({"cmd":"ReceivedItems","index":5,"items":[[5031004,6,1,0]]})"),
                         outgoing, replayed);
    Check(replayed.empty() && !outgoing.empty() && outgoing.back() == Session::BuildSync(),
          "a live item before the replay waits for the Sync reply");
    rewound.HandlePacket(Packet(allFive), outgoing, replayed);
    Check(replayed.size() == 3 && replayed[0].index == 2 &&
              replayed[0].itemType == PortRandomizer::ItemFromName("ChargeBeam") &&
              replayed[1].index == 3 && replayed[1].itemType == -1 && replayed[2].index == 4 &&
              replayed[2].itemType == PortRandomizer::ItemFromName("SuperMissile"),
          "the replay grants only the items the save is missing, at the right steps");
    Check(rewound.GetState().nextItemIndex == 5 &&
              rewound.GetState().progressive.at(5031043) == 3 && rewound.Tracked().size() == 5,
          "the replay restores the index, progressive count and tracker");
    int replayNotes = 0;
    while (rewound.TakeNotification(notification))
      ++replayNotes;
    Check(replayNotes == 3, "only the regranted items are announced");
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

  // DeathLink. A bounce from the server is owed to the game rather than applied
  // here, so one that arrives while nothing is running is not lost, and it is
  // cleared once taken so it is not applied twice.
  {
    const std::string deathConfigText = R"json({
      "server":"ws://localhost", "slot":"P", "death_link":true,
      "items":{"5031004":{"item":"Missiles","amount":5,"capacity":5}}
    })json";
    const Config deathConfig = ParseConfig(deathConfigText);
    Check(deathConfig.valid && deathConfig.deathLink, "death_link parses and is on");
    const Config noDeath = ParseConfig(R"json({"server":"ws://localhost","slot":"P"})json");
    Check(noDeath.valid && !noDeath.deathLink, "death_link is off unless asked for");
    const Config badDeath = ParseConfig(R"json({"server":"w","slot":"P","death_link":"yes"})json");
    Check(!badDeath.valid && badDeath.error.find("death_link") != std::string::npos,
          "a non-boolean death_link is rejected with a reason");

    Session deaths(deathConfig, State{});
    std::vector<ItemGrant> deathGrants;
    deaths.HandlePacket(Packet(R"({"cmd":"Connected","slot":1,"team":0,"players":)"
                               R"([{"slot":1,"alias":"Me"},{"slot":2,"alias":"Bob"}]})"),
                        outgoing, deathGrants);
    Check(deaths.DeathsPending() == 0, "no death is owed before a bounce arrives");
    deaths.HandlePacket(Packet(R"({"cmd":"Bounced","tags":["DeathLink"],)"
                               R"("data":{"time":1.5,"source":"Bob"}})"),
                        outgoing, deathGrants);
    Check(deaths.DeathsPending() == 1, "a bounce from another player is owed to the game");
    Check(deaths.LastDeathSource() == "Bob", "the bounce names who died");
    Check(deaths.TakeDeathPending() == 1 && deaths.DeathsPending() == 0,
          "taking a death clears it so it is not applied twice");
    deaths.HandlePacket(Packet(R"({"cmd":"Bounced","tags":["DeathLink"],)"
                               R"("data":{"time":2.5,"source":"P"}})"),
                        outgoing, deathGrants);
    Check(deaths.DeathsPending() == 0, "a bounce from this client is not a death of its own");
    Check(deaths.TakeNotification(notification) && notification.find("Bob") != std::string::npos,
          "a bounce raises a notification naming who died");
    deaths.HandlePacket(Packet(R"({"cmd":"Bounced","tags":["Tracker"],"data":{"source":"Bob"}})"),
                        outgoing, deathGrants);
    deaths.HandlePacket(Packet(R"({"cmd":"Bounced","data":{"source":"Bob"}})"), outgoing,
                        deathGrants);
    deaths.HandlePacket(Packet(R"({"cmd":"Bounce","tags":["DeathLink"],"data":{"source":"Bob"}})"),
                        outgoing, deathGrants);
    Check(deaths.DeathsPending() == 0,
          "only a Bounced carrying the DeathLink tag is a death");
    deaths.HandlePacket(Packet(R"({"cmd":"Bounced","tags":["DeathLink"],)"
                               R"("data":{"time":3,"source":"Bob","cause":"Bob fell"}})"),
                        outgoing, deathGrants);
    Check(deaths.TakeNotification(notification) && notification == "Bob fell",
          "a bounce with a cause shows the cause");
    deaths.TakeDeathPending();

    // Two bounces before the game runs are two deaths, not one.
    deaths.HandlePacket(Packet(R"({"cmd":"Bounced","tags":["DeathLink"],"data":{"source":"Bob"}})"),
                        outgoing, deathGrants);
    deaths.HandlePacket(Packet(R"({"cmd":"Bounced","tags":["DeathLink"],"data":{"source":"Bob"}})"),
                        outgoing, deathGrants);
    Check(deaths.TakeDeathPending() == 2, "bounces that arrive together are all owed");

    Session deathsOff(noDeath, State{});
    deathsOff.HandlePacket(Packet(R"({"cmd":"Bounced","tags":["DeathLink"],"data":{"source":"Bob"}})"),
                           outgoing, deathGrants);
    Check(deathsOff.DeathsPending() == 0, "a client without death_link ignores bounces");

    const std::string bounce = deaths.BuildBounce();
    Check(bounce.find("\"cmd\":\"Bounce\"") != std::string::npos &&
              bounce.find("\"tags\":[\"DeathLink\"]") != std::string::npos &&
              bounce.find("\"time\":") != std::string::npos &&
              bounce.find("\"source\":\"P\"") != std::string::npos &&
              bounce.find("\"cause\":\"P died\"") != std::string::npos,
          "an enabled client builds a DeathLink Bounce with time, source and cause");
    PortJson::Value bounceJson;
    size_t bounceOffset = 0;
    const char* bounceReason = nullptr;
    Check(PortJson::Parse(bounce, bounceJson, bounceOffset, &bounceReason),
          "the built Bounce is valid JSON");
    Check(deaths.BuildConnect().find("\"tags\":[\"DeathLink\"]") != std::string::npos,
          "a DeathLink client connects with the DeathLink tag");
    Check(Session(noDeath, State{}).BuildConnect().find("DeathLink") == std::string::npos,
          "a client without death_link does not carry the tag");
    Check(Session(noDeath, State{}).BuildBounce().empty(),
          "a client without death_link sends no Bounce");
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

  // Built-in Metroid Prime tables: a config with only the connection details.
  const Config builtin = ParseConfig(R"({"server":"ws://localhost","slot":"P"})");
  Check(builtin.valid && builtin.builtin, "a bare Metroid Prime config uses the built-in tables");
  Check(builtin.locations.size() == 100 &&
            builtin.locations.count("39F2DE28:B2701146:0000007E") == 1 &&
            builtin.locations.at("39F2DE28:B2701146:0000007E") == 5031158,
        "built-in locations are keyed world:area:pickup");
  Check(builtin.items.count(5031004) == 1 && builtin.items.count(5031049) == 1 &&
            builtin.items.at(5031049).IsProgressive(),
        "built-in items include expansions and progressive beams");
  Check(!ParseConfig(R"({"server":"ws://localhost","slot":"P","game":"Other"})").builtin &&
            !config.builtin,
        "other games and hand-written tables do not get the built-in tables");
  const Config ownLocations = ParseConfig(
      R"({"server":"ws://localhost","slot":"P","locations":{"39F2DE28:B2701146:0000007E":5031101}})");
  Check(ownLocations.valid && ownLocations.builtin && ownLocations.locations.size() == 1 &&
            ownLocations.locations.at("39F2DE28:B2701146:0000007E") == 5031101 &&
            builtin.items.size() == ownLocations.items.size(),
        "a config with only locations keeps them and takes the built-in items");
  {
    Session session(builtin, State());
    Check(Contains(session.BuildConnect(), "\"slot_data\":true"),
          "the built-in tables ask for slot_data");
    Check(Contains(Session(config, State()).BuildConnect(), "\"slot_data\":false"),
          "a hand-written config does not ask for slot_data");
    Check(Contains(Session::BuildGoal(), "\"status\":30"), "goal is StatusUpdate 30");
    std::vector<std::string> outgoing;
    std::vector<ItemGrant> grants;
    session.HandlePacket(Packet(R"({"cmd":"Connected","slot":1,"team":0,"players":[],
        "checked_locations":[],"slot_data":{"missile_launcher":1,"main_power_bomb":0,
        "death_link":1,"elevator_randomization":true,"starting_room_name":"Landing Site",
        "spring_ball":1,"non_varia_heat_damage":1,"required_artifacts":12,
        "etank_capacity":100,"shuffle_unlimited_missiles":0,"pre_scan_elevators":1}})"),
                         outgoing, grants);
    const SlotData& slot = session.GetSlotData();
    Check(slot.received && slot.requireMissileLauncher && !slot.requireMainPowerBomb,
          "slot_data main-item requirements parse");
    Check(slot.variaOnlyHeat, "non_varia_heat_damage parses");
    Check(slot.springBall == 1, "spring_ball parses");
    Check(slot.preScanElevators, "pre_scan_elevators parses");
    Check(slot.warnings.size() == 1 && Contains(slot.warnings[0], "elevator"),
          "only the unsupported option is warned about");
    Check(session.GetConfig().deathLink && outgoing.size() == 4 &&
              Contains(outgoing[0], "ConnectUpdate") && Contains(outgoing[0], "DeathLink"),
          "the seed's DeathLink option adds the tag");
    Check(outgoing.size() == 4 && Contains(outgoing[1], "\"cmd\":\"LocationScouts\"") &&
              Contains(outgoing[1], "5031100") && Contains(outgoing[1], "5031199") &&
              Contains(outgoing[1], "\"create_as_hint\":0"),
          "the built-in tables scout every location without hinting");
    Check(outgoing.size() == 4 && outgoing[2] == R"({"cmd":"Get","keys":["_read_hints_0_1"]})" &&
              outgoing[3] == R"({"cmd":"SetNotify","keys":["_read_hints_0_1"]})",
          "and read and follow this slot's hints");
    outgoing.clear();
    session.HandlePacket(Packet(R"({"cmd":"ReceivedItems","index":0,"items":[
        {"item":5031004,"location":1,"player":1,"flags":0},
        {"item":5031043,"location":2,"player":1,"flags":0},
        {"item":5031004,"location":3,"player":1,"flags":0},
        {"item":5031049,"location":4,"player":1,"flags":0},
        {"item":5031049,"location":5,"player":1,"flags":0},
        {"item":5031041,"location":6,"player":1,"flags":0}]})"),
                         outgoing, grants);
    const int missiles = PortRandomizer::ItemFromName("Missiles");
    Check(grants.size() == 6 && grants[0].itemType == missiles && grants[0].capacity == 0 &&
              grants[1].itemType == missiles && grants[1].capacity == 10 &&
              grants[1].amount == 10 && grants[2].capacity == 5,
          "missile capacity waits for the launcher when the seed requires it");
    Check(grants.size() == 6 && grants[3].itemType == PortRandomizer::ItemFromName("PowerBeam") &&
              grants[4].itemType == PortRandomizer::ItemFromName("ChargeBeam"),
          "progressive power beam steps to the charge beam");
    Check(grants.size() == 6 && grants[5].itemType < 0 && session.ReceivedCount(5031041) == 1,
          "unlimited missiles is counted, not granted");
  }
  {
    Session session(builtin, State());
    std::vector<std::string> outgoing;
    std::vector<ItemGrant> grants;
    session.HandlePacket(Packet(R"({"cmd":"Connected","slot":1,"team":0,"players":[],
        "checked_locations":[],"slot_data":{"missile_launcher":0,"main_power_bomb":0}})"),
                         outgoing, grants);
    session.HandlePacket(Packet(R"({"cmd":"ReceivedItems","index":0,"items":[
        {"item":5031007,"location":1,"player":1,"flags":0},
        {"item":5031007,"location":2,"player":1,"flags":0},
        {"item":5031044,"location":3,"player":1,"flags":0}]})"),
                         outgoing, grants);
    Check(grants.size() == 3 && grants[0].capacity == 4 && grants[1].capacity == 1 &&
              grants[2].capacity == 1,
          "without the main requirement the first expansion carries the main amount");
    Check(outgoing.size() == 3 && !Contains(outgoing[0], "ConnectUpdate"),
          "no DeathLink update when the seed has it off");
    Check(session.GetSlotData().warnings.empty() && session.GetSlotData().springBall == 0,
          "a seed without spring_ball has none and gets no warning");
    Check(!session.GetSlotData().preScanElevators, "a seed without pre_scan_elevators scans nothing");
  }
  {
    Session session(builtin, State());
    std::vector<std::string> outgoing;
    std::vector<ItemGrant> grants;
    session.HandlePacket(Packet(R"({"cmd":"Connected","slot":1,"team":0,"players":[],
        "checked_locations":[],"slot_data":{"spring_ball":3}})"),
                         outgoing, grants);
    const SlotData& slot = session.GetSlotData();
    Check(slot.springBall == 3 && slot.warnings.empty(),
          "Spring Ball as a progressive item is supported");
  }
  {
    const Config optedOut =
        ParseConfig(R"({"server":"ws://localhost","slot":"P","death_link":false})");
    Session session(optedOut, State());
    std::vector<std::string> outgoing;
    std::vector<ItemGrant> grants;
    session.HandlePacket(Packet(R"({"cmd":"Connected","slot":1,"team":0,"players":[],
        "checked_locations":[],"slot_data":{"death_link":true}})"),
                         outgoing, grants);
    Check(outgoing.size() == 3 && !Contains(outgoing[0], "ConnectUpdate") &&
              !session.GetConfig().deathLink,
          "archipelago.json's death_link overrides the seed's");
  }
  {
    // Names from DataPackage, what sits at each location from LocationInfo,
    // and no second HUD line for a find the pickup already announced.
    Session session(builtin, State());
    std::vector<std::string> outgoing;
    std::vector<ItemGrant> grants;
    session.HandlePacket(Packet(R"({"cmd":"Connected","slot":1,"team":0,
        "players":[{"team":0,"slot":1,"alias":"Samus","name":"Samus"},
                   {"team":0,"slot":2,"alias":"Link","name":"Link"}],
        "slot_info":{"1":{"name":"Samus","game":"Metroid Prime"},
                     "2":{"name":"Link","game":"A Link to the Past"}},
        "checked_locations":[],"slot_data":{}})"),
                         outgoing, grants);
    Check(!outgoing.empty() && Contains(outgoing[0], "\"cmd\":\"GetDataPackage\"") &&
              Contains(outgoing[0], "A Link to the Past") && Contains(outgoing[0], "Metroid Prime"),
          "Connected asks for every game's names");
    session.HandlePacket(Packet(R"({"cmd":"DataPackage","data":{"games":{
        "A Link to the Past":{"item_name_to_id":{"Hookshot":10},
                              "location_name_to_id":{"Link's House":20}}}}})"),
                         outgoing, grants);
    session.HandlePacket(Packet(R"({"cmd":"LocationInfo","locations":[
        {"item":10,"location":5031158,"player":2,"flags":1},
        {"item":5031024,"location":5031100,"player":1,"flags":1}]})"),
                         outgoing, grants);
    Check(session.LocationText(5031158) == "Found Hookshot for Link" &&
              session.LocationText(5031100) == "Found Energy Tank" &&
              session.LocationText(5031101).empty(),
          "scouted locations read as what they hold and for whom");
    Check(session.ItemName(10, 2) == "Hookshot" && session.ItemName(99, 2) == "item 99" &&
              session.LocationName(5031158, 1) == "Tallon Overworld: Landing Site",
          "item and location names follow the receiving slot's game");
    int64_t scoutedItem = 0;
    bool sameGame = true;
    int64_t scoutedFlags = 0;
    Check(session.ScoutedAt(5031158, scoutedItem, sameGame, &scoutedFlags) && scoutedItem == 10 &&
              !sameGame && scoutedFlags == 1,
          "another game's item is scouted as such, with its classification");
    Check(session.ScoutedAt(5031100, scoutedItem, sameGame) && scoutedItem == 5031024 &&
              sameGame && !session.ScoutedAt(5031101, scoutedItem, sameGame),
          "an own item is scouted as this game's; an unscouted location isn't");
    Check(session.ScanText(5031158) == "Hookshot\nfor Link (A Link to the Past)" &&
              session.ScanText(5031100) == "Energy Tank\nfor you" &&
              session.ScanText(5031101).empty(),
          "a pickup scans as what it holds, and for whom when that's another player");
    session.HandlePacket(Packet(R"({"cmd":"DataPackage","data":{"games":{
        "A Link to the Past":{"item_name_to_id":{"Bow & Arrows":11}}}}})"),
                         outgoing, grants);
    session.HandlePacket(Packet(R"({"cmd":"LocationInfo","locations":[
        {"item":11,"location":5031103,"player":2,"flags":1}]})"),
                         outgoing, grants);
    Check(session.ScanText(5031103) == "Bow && Arrows\nfor Link (A Link to the Past)",
          "an '&' in a name is escaped for the game's text markup");
    bool askedHints = false;
    for (const std::string& packet : outgoing)
      askedHints = askedHints || (Contains(packet, "\"cmd\":\"Get\"") && Contains(packet, "_read_hints_0_1"));
    Check(askedHints, "Connected asks for this slot's hints");
    {
      using namespace PortAp::MetroidPrime;
      const int64_t truth = kItemBase + kArtifactTruth;
      const int64_t strength = kItemBase + kArtifactTruth + 1;
      const int64_t elder = kItemBase + kArtifactTruth + 2;
      session.HandlePacket(Packet(R"({"cmd":"Retrieved","keys":{"_read_hints_0_1":[
          {"receiving_player":1,"finding_player":2,"location":20,"item":)" +
                                  std::to_string(truth) + R"(,"found":false},
          {"receiving_player":2,"finding_player":1,"location":5031100,"item":10,"found":false}]}})"),
                           outgoing, grants);
      session.HandlePacket(Packet(R"({"cmd":"LocationInfo","locations":[
          {"item":)" + std::to_string(strength) + R"(,"location":5031102,"player":1,"flags":1}]})"),
                           outgoing, grants);
      const std::string truthHint = session.ArtifactHint(truth);
      Check(Contains(truthHint, "#d4cc33;Link's&pop;") && Contains(truthHint, "#89a1ff;Link's House&pop;") &&
                Contains(truthHint, ItemName(truth)),
            "a hinted artifact names the player and location it's at");
      Check(Contains(session.ArtifactHint(strength), "#d4cc33;your&pop;"),
            "an artifact in this world is known from the scouts");
      Check(Contains(session.ArtifactHint(elder), "has not been collected."),
            "an unknown artifact keeps the AP world's fallback text");
      Check(session.ArtifactHint(10).empty(), "only this game's items get a totem hint");
    }
    {
      using namespace PortAp::MetroidPrime;
      Check(PickupModelKey(kItemBase + 16) == 16 && PickupModelKey(kItemBase + 42) == 7 &&
                PickupModelKey(kItemBase + 44) == kModelMainPowerBomb &&
                PickupModelKey(kItemBase + 54) == 6 && PickupModelKey(kItemBase + 57) == 10 &&
                PickupModelKey(kItemBase + 99) == kModelOtherGame &&
                PickupModelKey(10) == kModelOtherGame &&
                PickupModelKey(kItemBase + kSpringBall) == kModelOtherProgression &&
                PickupModelKey(kItemBase + 49) == 11,
            "AP items map to the retail pickup that shows them");
      Check(OtherGameModelKey(1) == kModelOtherProgression &&
                OtherGameModelKey(3) == kModelOtherProgression &&
                OtherGameModelKey(2) == kModelOtherUseful && OtherGameModelKey(0) == kModelOtherGame &&
                OtherGameModelKey(4) == kModelOtherGame,
            "other games' items look like Cog, Zoomer or Nothing by classification");
    }
    std::string notification;
    while (session.TakeNotification(notification)) {
    }
    session.HandlePacket(Packet(R"j({"cmd":"PrintJSON","type":"ItemSend","receiving":2,
        "item":{"item":10,"location":20,"player":2,"flags":1},"data":[
        {"type":"player_id","text":"2"},{"text":" sent "},
        {"type":"item_id","text":"10","player":2,"flags":1},{"text":" to "},
        {"type":"player_id","text":"1"},{"text":" ("},
        {"type":"location_id","text":"20","player":2},{"text":")"}]})j"),
                         outgoing, grants);
    Check(session.TakeNotification(notification) &&
              notification == "Link sent Hookshot to Samus (Link's House)",
          "PrintJSON ids read as player, item and location names");
    Check(session.AnnounceLocation(5031158) == "Found Hookshot for Link",
          "the pickup announces its scouted item");
    session.HandlePacket(Packet(R"({"cmd":"PrintJSON","type":"ItemSend","receiving":2,
        "item":{"item":10,"location":5031158,"player":1,"flags":1},"data":[
        {"type":"player_id","text":"1"},{"text":" sent "},
        {"type":"item_id","text":"10","player":2,"flags":1}]})"),
                         outgoing, grants);
    session.HandlePacket(Packet(R"({"cmd":"ReceivedItems","index":0,"items":[
        {"item":5031024,"location":5031100,"player":1,"flags":1}]})"),
                         outgoing, grants);
    Check(session.TakeNotification(notification) && notification == "Energy Tank" &&
              !session.TakeNotification(notification),
          "a find the pickup did not announce is still shown, an announced one is not");
    Check(session.LastMessage() == "Samus sent Hookshot",
          "an announced find still reaches the overlay's last message");
    ChatLine line;
    Check(session.TakeChatLine(line) && line.type == "ItemSend" &&
              line.text == "Link sent Hookshot to Samus (Link's House)" &&
              session.TakeChatLine(line) && line.text == "Samus sent Hookshot" &&
              !session.TakeChatLine(line),
          "the chat log keeps every PrintJSON, announced finds included");
  }

  std::filesystem::remove_all(testDir);
  if (!sPassed)
    return 1;
  std::puts("[ap-tests] passed");
  return 0;
}
