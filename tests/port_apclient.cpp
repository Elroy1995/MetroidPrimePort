#include "port_ap_protocol.h"

#include "port_randomizer.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

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

} // namespace

int main() {
  using namespace PortAp::Protocol;
  const std::filesystem::path testDir = std::filesystem::temp_directory_path() /
      ("mp-ap-test-" + std::to_string(static_cast<long long>(getpid())));
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
      "100":{"item":"EnergyTanks","amount":2,"capacity":3},
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
            config.items.at(101).itemType == PortRandomizer::ItemFromName("Missiles") &&
            config.items.at(101).amount == 1 && config.items.at(101).capacity == 1,
        "item types and optional defaults parse");

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
            defaults.versionMinor == 6 && defaults.versionBuild == 0 && defaults.tags.empty(),
        "optional configuration defaults apply");
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
  packets.HandlePacket(Packet(R"({"cmd":"Connected","team":0,"slot":3,"checked_locations":[123457,123456,999]})"),
                      outgoing, grants);
  Check(packets.HandshakeComplete() && packets.SlotDescription() == "slot 3, team 0",
        "Connected completes handshake and records slot description");
  Check(packets.GetState().checkedLocations == std::vector<int64_t>({123456, 123457, 999}),
        "Connected checked locations merge without duplicates");

  packets.HandlePacket(Packet(
      R"({"cmd":"ReceivedItems","index":0,"items":[{"item":100,"location":1,"player":1,"flags":0}]})"),
      outgoing, grants);
  Check(grants.size() == 1 && grants.back().itemId == 100 &&
            grants.back().itemType == PortRandomizer::ItemFromName("EnergyTanks") &&
            grants.back().amount == 2 && grants.back().capacity == 3,
        "ReceivedItems object grants configured item fields");
  packets.HandlePacket(Packet(
      R"({"cmd":"ReceivedItems","index":1,"items":[[101,2,1,0]]})"),
      outgoing, grants);
  Check(grants.size() == 2 && grants.back().itemId == 101 &&
            grants.back().itemType == PortRandomizer::ItemFromName("Missiles") &&
            grants.back().amount == 1 && grants.back().capacity == 1,
        "ReceivedItems four-element array grants configured item");
  packets.HandlePacket(Packet(R"({"cmd":"ReceivedItems","index":0,"items":[[100,1,1,0]]})"),
                      outgoing, grants);
  Check(grants.size() == 2 && packets.GetState().nextItemIndex == 2,
        "already processed item index is not granted twice");
  packets.HandlePacket(Packet(R"({"cmd":"ReceivedItems","index":2,"items":[[999,1,1,0]]})"),
                      outgoing, grants);
  Check(grants.size() == 3 && grants.back().itemType == -1 &&
            packets.LastError() == "unknown item id 999",
        "unknown received item is counted and reported");
  packets.HandlePacket(Packet(R"({"cmd":"ReceivedItems","index":5,"items":[]})"),
                      outgoing, grants);
  Check(packets.Desynced() && packets.GetState().nextItemIndex == 5 &&
            !outgoing.empty() && outgoing.back() == Session::BuildSync(),
        "item index jump marks desync and queues Sync");

  std::string printParts = R"({"cmd":"PrintJSON","data":[{"text":"Hello"},{"text":"\nworld"}]})";
  packets.HandlePacket(Packet(printParts), outgoing, grants);
  Check(packets.LastMessage() == "Hello world", "PrintJSON joins text and flattens newlines");
  std::string longText(250, 'x');
  const std::string printLong = "{\"cmd\":\"PrintJSON\",\"data\":[{\"text\":\"" + longText + "\"}]}";
  packets.HandlePacket(Packet(printLong), outgoing, grants);
  Check(packets.LastMessage().size() == 203 && packets.LastMessage().substr(200) == "...",
        "PrintJSON truncates long output and marks the cut");
  packets.HandlePacket(Packet(R"({"cmd":"InvalidPacket","text":"bad command"})"), outgoing, grants);
  Check(packets.LastError() == "bad command", "InvalidPacket records text as error");
  packets.HandlePacket(Packet(R"({"cmd":"ConnectionRefused","errors":["bad password","unknown slot"]})"),
                      outgoing, grants);
  Check(!packets.HandshakeComplete() && packets.LastError() == "bad password, unknown slot",
        "ConnectionRefused joins error messages and clears handshake");

  State saved;
  saved.slot = "Player1";
  saved.nextItemIndex = 23;
  saved.checkedLocations = {8, 10, 12};
  const std::filesystem::path statePath = testDir / "nested" / "archipelago_state.json";
  Check(SaveStateFile(statePath.string(), saved), "state save creates parent directory");
  const State reloaded = LoadStateFile(statePath.string());
  Check(reloaded.slot == saved.slot && reloaded.nextItemIndex == saved.nextItemIndex &&
            reloaded.checkedLocations == saved.checkedLocations,
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

  std::filesystem::remove_all(testDir);
  if (!sPassed)
    return 1;
  std::puts("[ap-tests] passed");
  return 0;
}
