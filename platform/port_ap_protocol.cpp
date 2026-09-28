#include "port_ap_protocol.h"

#include "port_randomizer.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <sstream>
#include <utility>

namespace PortAp {
namespace Protocol {
namespace {

const PortJson::Value* Member(const PortJson::Value& object, const char* name) {
  return object.Find(name);
}

bool Integer(const PortJson::Value* value, int64_t& result) {
  if (value == nullptr || !value->IsNumber())
    return false;
  const double number = value->AsNumber();
  constexpr double kInt64Limit = 9223372036854775808.0;
  if (!std::isfinite(number) || std::trunc(number) != number || number < -kInt64Limit ||
      number >= kInt64Limit)
    return false;
  result = value->AsInt();
  return true;
}

bool IntegerMember(const PortJson::Value& object, const char* name, int64_t& result) {
  return Integer(Member(object, name), result);
}

std::string EscapeJson(const std::string& text) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string escaped;
  escaped.reserve(text.size() + 2);
  for (unsigned char c : text) {
    switch (c) {
    case '"': escaped += "\\\""; break;
    case '\\': escaped += "\\\\"; break;
    case '\b': escaped += "\\b"; break;
    case '\f': escaped += "\\f"; break;
    case '\n': escaped += "\\n"; break;
    case '\r': escaped += "\\r"; break;
    case '\t': escaped += "\\t"; break;
    default:
      if (c < 0x20) {
        escaped += "\\u00";
        escaped.push_back(kHex[c >> 4]);
        escaped.push_back(kHex[c & 0x0f]);
      } else {
        escaped.push_back(static_cast<char>(c));
      }
      break;
    }
  }
  return escaped;
}

std::string Quote(const std::string& text) { return "\"" + EscapeJson(text) + "\""; }

bool ValidLocationKey(const std::string& key) {
  if (key.size() != 26 || key[8] != ':' || key[17] != ':')
    return false;
  for (size_t i = 0; i < key.size(); ++i) {
    if (i == 8 || i == 17)
      continue;
    const char c = key[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
      return false;
  }
  return true;
}

bool ParseItemKey(const std::string& key, int64_t& value) {
  if (key.empty())
    return false;
  for (char c : key) {
    if (c < '0' || c > '9')
      return false;
  }
  const auto parsed = std::from_chars(key.data(), key.data() + key.size(), value, 10);
  return parsed.ec == std::errc() && parsed.ptr == key.data() + key.size();
}

bool OptionalInt(const PortJson::Value& object, const char* field, int fallback, int& result,
                 std::string& error) {
  const PortJson::Value* value = Member(object, field);
  if (value == nullptr) {
    result = fallback;
    return true;
  }
  int64_t number = 0;
  if (!Integer(value, number) || number < std::numeric_limits<int>::min() ||
      number > std::numeric_limits<int>::max()) {
    error = std::string("items.") + field + " must be an integer";
    return false;
  }
  result = static_cast<int>(number);
  return true;
}

// Parses one grant object ({"item", "amount", "capacity", "display"}).
// `label` names it in errors: "item 12" or "item 12 progressive step 2".
bool ParseGrant(const PortJson::Value& object, const std::string& label, int64_t itemId,
                ItemGrant& grant, std::string& error) {
  if (!object.IsObject()) {
    error = label + " must be an object";
    return false;
  }
  const PortJson::Value* nameValue = Member(object, "item");
  if (nameValue == nullptr || !nameValue->IsString()) {
    error = label + " requires an item name string";
    return false;
  }
  const std::string& itemName = nameValue->AsString();
  const int itemType = PortRandomizer::ItemFromName(itemName.c_str());
  if (itemType < 0) {
    error = "unknown item name: " + itemName;
    return false;
  }
  grant.itemId = itemId;
  grant.itemType = itemType;
  // `display` is the player-facing name; it is independent of the
  // randomizer key in `item` and defaults to that resolved item's name.
  grant.display = itemName;
  const PortJson::Value* displayValue = Member(object, "display");
  if (displayValue != nullptr) {
    if (!displayValue->IsString()) {
      error = label + " display must be a string";
      return false;
    }
    grant.display = displayValue->AsString();
  }
  return OptionalInt(object, "amount", 1, grant.amount, error) &&
         OptionalInt(object, "capacity", 1, grant.capacity, error);
}

// Parses one "items" entry: a flat grant, or {"progressive": [grant, ...]}.
bool ParseItemEntry(const PortJson::Value& object, const std::string& key, int64_t itemId,
                    ItemEntry& entry, std::string& error) {
  const std::string label = "item " + key;
  if (!object.IsObject()) {
    error = label + " must be an object";
    return false;
  }
  const PortJson::Value* steps = Member(object, "progressive");
  if (steps == nullptr) {
    if (Member(object, "item") == nullptr) {
      error = label + " requires an item name string or a progressive list";
      return false;
    }
    return ParseGrant(object, label, itemId, entry, error);
  }
  if (Member(object, "item") != nullptr) {
    error = label + " cannot have both item and progressive";
    return false;
  }
  if (!steps->IsArray() || steps->AsArray().empty()) {
    error = label + " progressive must be a non-empty array of grants";
    return false;
  }
  const PortJson::Value::Elements& list = steps->AsArray();
  entry.progressive.reserve(list.size());
  for (size_t i = 0; i < list.size(); ++i) {
    ItemGrant step;
    if (!ParseGrant(list[i], label + " progressive step " + std::to_string(i + 1), itemId, step,
                    error))
      return false;
    entry.progressive.push_back(std::move(step));
  }
  static_cast<ItemGrant&>(entry) = entry.progressive.front();
  return true;
}

std::string ProcessUuid() {
  static const std::string uuid = [] {
    std::array<uint8_t, 16> bytes{};
    try {
      std::random_device random;
      for (uint8_t& byte : bytes)
        byte = static_cast<uint8_t>(random());
    } catch (...) {
      const uint64_t seed = static_cast<uint64_t>(
          std::chrono::high_resolution_clock::now().time_since_epoch().count());
      std::mt19937_64 random;
      random.seed(seed ^ reinterpret_cast<uintptr_t>(&bytes));
      for (uint8_t& byte : bytes)
        byte = static_cast<uint8_t>(random());
    }
    bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0f) | 0x40);
    bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3f) | 0x80);
    static constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    result.reserve(36);
    for (size_t i = 0; i < bytes.size(); ++i) {
      if (i == 4 || i == 6 || i == 8 || i == 10)
        result.push_back('-');
      result.push_back(kHex[bytes[i] >> 4]);
      result.push_back(kHex[bytes[i] & 0x0f]);
    }
    return result;
  }();
  return uuid;
}

void AppendInt(std::string& out, int64_t value) { out += std::to_string(value); }

void AppendNotification(std::vector<std::string>& notifications, std::string text) {
  constexpr size_t kNotificationLimit = 32;
  if (notifications.size() >= kNotificationLimit)
    notifications.erase(notifications.begin());
  notifications.push_back(std::move(text));
}

// How much of a session's receipts the tracker keeps. Comfortably more than a
// play session sends, and bounded so a long one cannot grow without limit.
constexpr size_t kMaxTrackedItems = 256;

// The name to show for an item: the display name of the step that copy grants,
// falling back to the item's own name when the config gave none.
std::string ItemDisplay(const ItemEntry& entry, int64_t count) {
  const ItemGrant& step = entry.Step(count);
  if (!step.display.empty())
    return step.display;
  if (!entry.display.empty())
    return entry.display;
  if (step.itemType >= 0) {
    const char* name = PortRandomizer::ItemName(step.itemType);
    if (name != nullptr)
      return name;
  }
  return "item";
}


} // namespace

const ItemGrant& ItemEntry::Step(int64_t count) const {
  if (progressive.empty())
    return *this;
  if (count <= 0)
    return progressive.front();
  if (static_cast<uint64_t>(count) >= progressive.size())
    return progressive.back();
  return progressive[static_cast<size_t>(count)];
}

Config ParseConfig(const std::string& text) {
  Config config;
  try {
    PortJson::Value root;
    size_t errorOffset = 0;
    const char* errorReason = nullptr;
    if (!PortJson::Parse(text, root, errorOffset, &errorReason)) {
      config.error = "invalid JSON at byte " + std::to_string(errorOffset) + ": " +
                     (errorReason != nullptr ? errorReason : "parse error");
      return config;
    }
    if (!root.IsObject()) {
      config.error = "configuration must be a JSON object";
      return config;
    }

    const PortJson::Value* server = Member(root, "server");
    if (server != nullptr && server->IsString())
      config.server = server->AsString();
    const PortJson::Value* slot = Member(root, "slot");
    if (slot != nullptr && slot->IsString())
      config.slot = slot->AsString();
    if (config.server.empty()) {
      config.error = "configuration requires a non-empty server string";
      return config;
    }
    if (config.slot.empty()) {
      config.error = "configuration requires a non-empty slot string";
      return config;
    }

    const PortJson::Value* value = Member(root, "password");
    if (value != nullptr) {
      if (!value->IsString()) {
        config.error = "password must be a string";
        return config;
      }
      config.password = value->AsString();
    }
    value = Member(root, "game");
    if (value != nullptr) {
      if (!value->IsString()) {
        config.error = "game must be a string";
        return config;
      }
      config.game = value->AsString();
    }
    value = Member(root, "tls_ca");
    if (value != nullptr) {
      if (!value->IsString()) {
        config.error = "tls_ca must be a string";
        return config;
      }
      config.tlsCa = value->AsString();
    }
    int64_t integer = 0;
    value = Member(root, "items_handling");
    if (value != nullptr) {
      if (!Integer(value, integer) || integer < std::numeric_limits<int>::min() ||
          integer > std::numeric_limits<int>::max()) {
        config.error = "items_handling must be an integer";
        return config;
      }
      config.itemsHandling = static_cast<int>(integer);
    }
    value = Member(root, "death_link");
    if (value != nullptr) {
      if (!value->IsBool()) {
        config.error = "death_link must be a boolean";
        return config;
      }
      config.deathLink = value->AsBool();
    }
    value = Member(root, "version");
    if (value != nullptr) {
      if (!value->IsObject()) {
        config.error = "version must be an object";
        return config;
      }
      const char* names[] = {"major", "minor", "build"};
      int* destinations[] = {&config.versionMajor, &config.versionMinor, &config.versionBuild};
      for (size_t i = 0; i < 3; ++i) {
        const PortJson::Value* component = Member(*value, names[i]);
        if (component == nullptr)
          continue;
        if (!Integer(component, integer) || integer < std::numeric_limits<int>::min() ||
            integer > std::numeric_limits<int>::max()) {
          config.error = std::string("version.") + names[i] + " must be an integer";
          return config;
        }
        *destinations[i] = static_cast<int>(integer);
      }
    }
    value = Member(root, "tags");
    if (value != nullptr) {
      if (!value->IsArray()) {
        config.error = "tags must be an array of strings";
        return config;
      }
      for (const PortJson::Value& tag : value->AsArray()) {
        if (!tag.IsString()) {
          config.error = "tags must be an array of strings";
          return config;
        }
        config.tags.push_back(tag.AsString());
      }
    }
    value = Member(root, "locations");
    if (value != nullptr) {
      if (!value->IsObject()) {
        config.error = "locations must be an object";
        return config;
      }
      for (const auto& entry : value->AsObject()) {
        if (!ValidLocationKey(entry.first)) {
          config.error = "invalid location key: " + entry.first;
          return config;
        }
        if (!Integer(&entry.second, integer)) {
          config.error = "location id for " + entry.first + " must be an integer";
          return config;
        }
        config.locations[entry.first] = integer;
      }
    }
    value = Member(root, "items");
    if (value != nullptr) {
      if (!value->IsObject()) {
        config.error = "items must be an object";
        return config;
      }
      for (const auto& entry : value->AsObject()) {
        int64_t itemId = 0;
        if (!ParseItemKey(entry.first, itemId)) {
          config.error = "invalid decimal item id: " + entry.first;
          return config;
        }
        ItemEntry item;
        if (!ParseItemEntry(entry.second, entry.first, itemId, item, config.error))
          return config;
        config.items[itemId] = std::move(item);
      }
    }
    config.valid = true;
    config.error.clear();
  } catch (const std::exception& error) {
    config.valid = false;
    config.error = std::string("configuration error: ") + error.what();
  } catch (...) {
    config.valid = false;
    config.error = "configuration error: unexpected failure";
  }
  return config;
}

Config LoadConfigFile(const std::string& path) {
  try {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
      Config config;
      config.error = "could not open configuration file: " + path;
      return config;
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    if (file.bad()) {
      Config config;
      config.error = "could not read configuration file: " + path;
      return config;
    }
    return ParseConfig(contents.str());
  } catch (const std::exception& error) {
    Config config;
    config.error = "could not read configuration file " + path + ": " + error.what();
    return config;
  } catch (...) {
    Config config;
    config.error = "could not read configuration file: " + path;
    return config;
  }
}

State LoadStateFile(const std::string& path) {
  State state;
  try {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
      return state;
    std::ostringstream contents;
    contents << file.rdbuf();
    if (file.bad())
      return state;
    PortJson::Value root;
    size_t errorOffset = 0;
    const char* errorReason = nullptr;
    if (!PortJson::Parse(contents.str(), root, errorOffset, &errorReason) || !root.IsObject())
      return state;
    const PortJson::Value* slot = Member(root, "slot");
    if (slot != nullptr && slot->IsString())
      state.slot = slot->AsString();
    // Absent in files written before the seed was recorded: an unknown seed,
    // which the first RoomInfo adopts rather than treating as a mismatch.
    const PortJson::Value* seed = Member(root, "seed");
    if (seed != nullptr && seed->IsString())
      state.seed = seed->AsString();
    int64_t number = 0;
    if (IntegerMember(root, "next_item_index", number) && number >= 0)
      state.nextItemIndex = number;
    const PortJson::Value* checks = Member(root, "checked_locations");
    if (checks != nullptr && checks->IsArray()) {
      for (const PortJson::Value& check : checks->AsArray()) {
        if (Integer(&check, number) &&
            std::find(state.checkedLocations.begin(), state.checkedLocations.end(), number) ==
                state.checkedLocations.end())
          state.checkedLocations.push_back(number);
      }
    }
    const PortJson::Value* progressive = Member(root, "progressive");
    if (progressive != nullptr && progressive->IsObject()) {
      for (const auto& entry : progressive->AsObject()) {
        int64_t itemId = 0;
        if (ParseItemKey(entry.first, itemId) && Integer(&entry.second, number) && number > 0)
          state.progressive[itemId] = number;
      }
    }
  } catch (...) {
    return State();
  }
  return state;
}

bool SaveStateFile(const std::string& path, const State& state) {
  try {
    const std::filesystem::path target(path);
    if (!target.parent_path().empty())
      std::filesystem::create_directories(target.parent_path());
    const std::string temporary = path + ".tmp";
    {
      std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
      if (!file.is_open())
        return false;
      file << "{\"slot\":" << Quote(state.slot) << ",\"seed\":" << Quote(state.seed)
           << ",\"next_item_index\":";
      file << state.nextItemIndex << ",\"checked_locations\":[";
      for (size_t i = 0; i < state.checkedLocations.size(); ++i) {
        if (i != 0)
          file << ',';
        file << state.checkedLocations[i];
      }
      file << "],\"progressive\":{";
      bool firstCount = true;
      for (const auto& count : state.progressive) {
        if (!firstCount)
          file << ',';
        file << '"' << count.first << "\":" << count.second;
        firstCount = false;
      }
      file << "}}";
      file.flush();
      if (!file)
        return false;
      file.close();
      if (!file)
        return false;
    }
    if (std::rename(temporary.c_str(), path.c_str()) == 0)
      return true;
#ifdef _WIN32
    // The C rename operation replaces an existing destination on POSIX, but
    // not on Windows. Retry there after removing the old state file.
    std::error_code filesystemError;
    if (!std::filesystem::exists(target, filesystemError) || filesystemError)
      return false;
    std::filesystem::remove(target, filesystemError);
    if (filesystemError)
      return false;
    return std::rename(temporary.c_str(), path.c_str()) == 0;
#else
    return false;
#endif
  } catch (...) {
    return false;
  }
}

Session::Session(const Config& config, const State& state) : mConfig(config), mState(state) {}

void Session::HandlePacket(const PortJson::Value& packet, std::vector<std::string>& outgoing,
                           std::vector<ItemGrant>& granted) try {
  if (!packet.IsObject())
    return;
  const PortJson::Value* commandValue = Member(packet, "cmd");
  if (commandValue == nullptr || !commandValue->IsString())
    return;
  const std::string& command = commandValue->AsString();
  if (command == "RoomInfo") {
    mHandshakeComplete = false;
    mSlotDescription.clear();
    mOwnSlot = 0;
    mSeedName.clear();
    const PortJson::Value* seedName = Member(packet, "seed_name");
    if (seedName != nullptr && seedName->IsString())
      mSeedName = seedName->AsString();
    // The recorded checks and item index are only true for the multiworld that
    // granted them. A different seed is a different session, so keeping them
    // would claim locations this slot never collected and skip the items the
    // server owes it. Discard them and say so; an empty recorded seed (a file
    // from before this was tracked) is adopted instead of treated as a mismatch.
    if (!mSeedName.empty() && mState.seed != mSeedName) {
      if (!mState.seed.empty()) {
        mState.nextItemIndex = 0;
        mState.checkedLocations.clear();
        mState.progressive.clear();
        mGrantFrom = 0;
        mResetReason = "recorded for seed \"" + mState.seed + "\", server has \"" + mSeedName + "\"";
      }
      mState.seed = mSeedName;
    }
  } else if (command == "ConnectionRefused") {
    mHandshakeComplete = false;
    mSlotDescription.clear();
    mOwnSlot = 0;
    mLastError.clear();
    const PortJson::Value* errors = Member(packet, "errors");
    if (errors != nullptr && errors->IsArray()) {
      bool firstError = true;
      for (const PortJson::Value& error : errors->AsArray()) {
        if (!error.IsString())
          continue;
        if (!firstError)
          mLastError += ", ";
        mLastError += error.AsString();
        firstError = false;
      }
    }
  } else if (command == "Connected") {
    mHandshakeComplete = true;
    int64_t slot = 0;
    int64_t team = 0;
    IntegerMember(packet, "slot", slot);
    IntegerMember(packet, "team", team);
    mSlotDescription = "slot " + std::to_string(slot) + ", team " + std::to_string(team);
    mOwnSlot = slot;
    mPlayers.clear();
    const PortJson::Value* players = Member(packet, "players");
    if (players != nullptr && players->IsArray()) {
      for (const PortJson::Value& player : players->AsArray()) {
        if (!player.IsObject())
          continue;
        int64_t playerSlot = 0;
        if (!IntegerMember(player, "slot", playerSlot))
          continue;
        std::string name;
        const PortJson::Value* alias = Member(player, "alias");
        if (alias != nullptr && alias->IsString())
          name = alias->AsString();
        if (name.empty()) {
          const PortJson::Value* playerName = Member(player, "name");
          if (playerName != nullptr && playerName->IsString())
            name = playerName->AsString();
        }
        if (!name.empty())
          mPlayers[playerSlot] = std::move(name);
      }
    }
    const PortJson::Value* checked = Member(packet, "checked_locations");
    if (checked != nullptr && checked->IsArray()) {
      for (const PortJson::Value& location : checked->AsArray()) {
        int64_t id = 0;
        if (Integer(&location, id) &&
            std::find(mState.checkedLocations.begin(), mState.checkedLocations.end(), id) ==
                mState.checkedLocations.end())
          mState.checkedLocations.push_back(id);
      }
    }
  } else if (command == "ReceivedItems") {
    int64_t index = 0;
    IntegerMember(packet, "index", index);
    const PortJson::Value* items = Member(packet, "items");
    const PortJson::Value::Elements empty;
    const auto& itemList = items != nullptr && items->IsArray() ? items->AsArray() : empty;
    const int64_t expectedIndex = mState.nextItemIndex;
    // Nothing processed yet: the server is starting a fresh inventory, so the
    // progressive counts start over and the replay rebuilds them.
    if (index == 0 && expectedIndex == 0)
      mState.progressive.clear();
    // Index 0 is the whole inventory (the reply to Connect or Sync), which
    // closes any gap; the loop below skips what was already processed.
    if (index == 0)
      mDesynced = false;
    if (index > 0 && index != expectedIndex) {
      mDesynced = true;
      outgoing.push_back(BuildSync());
      // Items past a gap are left for the Sync reply. Taking them now would
      // move nextItemIndex past the missing ones, and the replay would then
      // skip those as already received.
      if (index > expectedIndex)
        return;
    }
    for (size_t position = 0; position < itemList.size(); ++position) {
      int64_t itemId = 0;
      const PortJson::Value& item = itemList[position];
      const PortJson::Value* itemValue = nullptr;
      if (item.IsObject()) {
        itemValue = Member(item, "item");
      } else if (item.IsArray() && item.Size() == 4) {
        itemValue = &item.AsArray()[0];
      }
      if (itemValue == nullptr || !Integer(itemValue, itemId))
        continue;
      if (position > static_cast<size_t>(std::numeric_limits<int64_t>::max()) ||
          index > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(position))
        continue;
      const int64_t receivedIndex = index + static_cast<int64_t>(position);
      if (receivedIndex < mState.nextItemIndex)
        continue;
      // Below mGrantFrom the loaded save already holds the item: the replay
      // only rebuilds the progressive counts and the tracker.
      const bool alreadyHeld = receivedIndex < mGrantFrom;
      const auto found = mConfig.items.find(itemId);
      std::string notification;
      // Copies of this item already processed, which is what makes a
      // progressive grant a step rather than a repeat of the first one. Needed
      // by the tracker below, which is outside the entry branch.
      int64_t seenBefore = 0;
      if (found != mConfig.items.end()) {
        const ItemEntry& entry = found->second;
        int64_t count = 0;
        if (entry.IsProgressive()) {
          int64_t& stored = mState.progressive[itemId];
          count = stored;
          if (stored < std::numeric_limits<int64_t>::max())
            ++stored;
        }
        ItemGrant grant = entry.Step(count);
        grant.itemId = itemId;
        grant.index = receivedIndex;
        seenBefore = count;
        notification = grant.display;
        if (!alreadyHeld)
          granted.push_back(std::move(grant));
      } else {
        ItemGrant grant;
        grant.itemId = itemId;
        grant.itemType = -1;
        grant.index = receivedIndex;
        if (!alreadyHeld)
          granted.push_back(grant);
        mLastError = "unknown item id " + std::to_string(itemId);
        notification = "unknown item " + std::to_string(itemId);
      }

      int64_t itemPlayer = 0;
      bool hasPlayer = false;
      if (item.IsObject()) {
        hasPlayer = IntegerMember(item, "player", itemPlayer);
      } else if (item.IsArray() && item.Size() == 4) {
        hasPlayer = Integer(&item.AsArray()[2], itemPlayer);
      }
      if (hasPlayer && mOwnSlot != 0 && itemPlayer != mOwnSlot)
        notification += " from " + PlayerName(itemPlayer);
      if (!alreadyHeld)
        AppendNotification(mNotifications, std::move(notification));
      // The tracker keeps the same receipt with its parts separated, so a
      // session's worth of items stays readable after the HUD line is gone.
      {
        TrackedItem tracked;
        tracked.itemId = itemId;
        const auto entryIt = mConfig.items.find(itemId);
        if (entryIt != mConfig.items.end()) {
          // The step's own display name, which is what the grant resolves to.
          // The entry's flat fields mirror step 0, so a progressive item needs
          // the step rather than the entry to be named correctly.
          tracked.name = ItemDisplay(entryIt->second, seenBefore);
          if (entryIt->second.IsProgressive()) {
            tracked.total = static_cast< int64_t >(entryIt->second.progressive.size());
            tracked.step = seenBefore + 1;
            if (tracked.step > tracked.total)
              tracked.step = tracked.total;
          }
        } else {
          tracked.name = "item " + std::to_string(itemId);
        }
        if (hasPlayer && mOwnSlot != 0 && itemPlayer != mOwnSlot)
          tracked.from = PlayerName(itemPlayer);
        // A long session can send a lot of items; keep the tail, which is what
        // a player is still looking at.
        mTracked.push_back(std::move(tracked));
        if (mTracked.size() > kMaxTrackedItems)
          mTracked.erase(mTracked.begin(), mTracked.end() - kMaxTrackedItems);
      }
    }
    int64_t endIndex = index;
    if (itemList.size() <= static_cast<size_t>(std::numeric_limits<int64_t>::max()) &&
        index <= std::numeric_limits<int64_t>::max() - static_cast<int64_t>(itemList.size()))
      endIndex = index + static_cast<int64_t>(itemList.size());
    mState.nextItemIndex = std::max(mState.nextItemIndex, endIndex);
  } else if (command == "PrintJSON") {
    std::string message;
    const PortJson::Value* data = Member(packet, "data");
    if (data != nullptr && data->IsArray()) {
      for (const PortJson::Value& part : data->AsArray()) {
        const PortJson::Value* text = Member(part, "text");
        if (text != nullptr && text->IsString())
          message += text->AsString();
      }
    }
    std::string normalized;
    normalized.reserve(message.size());
    bool previousWasNewline = false;
    for (char c : message) {
      if (c == '\r' || c == '\n') {
        if (!previousWasNewline)
          normalized.push_back(' ');
        previousWasNewline = true;
      } else {
        normalized.push_back(c);
        previousWasNewline = false;
      }
    }
    constexpr size_t kMessageLimit = 200;
    if (normalized.size() > kMessageLimit) {
      normalized.resize(kMessageLimit);
      normalized += "...";
    }
    mLastMessage = std::move(normalized);
    AppendNotification(mNotifications, mLastMessage);
  } else if (command == "InvalidPacket") {
    const PortJson::Value* text = Member(packet, "text");
    mLastError = text != nullptr && text->IsString() ? text->AsString() : std::string();
  } else if (command == "Bounced") {
    // DeathLink: someone else in the multiworld died. The server relays a
    // client's Bounce as Bounced to every client whose tags match, so a
    // DeathLink is a Bounced carrying the "DeathLink" tag; other bounces
    // (trackers, game-specific links) are none of the game's business. The
    // data has `time`, an optional `cause`, and `source`, the dead player's
    // name as that client sent it.
    //
    // The server echoes a bounce to its sender too, so one whose source is
    // this client's own slot name is a death it already announced, and acting
    // on it would kill the player a second time. A missing source is still
    // somebody else's death and still counts.
    if (!mConfig.deathLink)
      return;
    const PortJson::Value* tags = Member(packet, "tags");
    bool deathLink = false;
    if (tags != nullptr && tags->IsArray()) {
      for (const PortJson::Value& tag : tags->AsArray())
        deathLink = deathLink || (tag.IsString() && tag.AsString() == "DeathLink");
    }
    if (!deathLink)
      return;
    const PortJson::Value* data = Member(packet, "data");
    const PortJson::Value* source =
        data != nullptr && data->IsObject() ? Member(*data, "source") : nullptr;
    const PortJson::Value* cause =
        data != nullptr && data->IsObject() ? Member(*data, "cause") : nullptr;
    const std::string sourceName =
        source != nullptr && source->IsString() ? source->AsString() : std::string();
    if (!sourceName.empty() && sourceName == mConfig.slot)
      return; // our own death, coming back to us
    mLastDeathSource = sourceName;
    ++mDeathsReceived;
    // Counted even when the game is not running: a bounce that arrives at the
    // title screen must not be lost, or the next run would neither die nor show
    // it. Poll applies it and clears the counter.
    // The cause is another client's free text, so it is capped like PrintJSON.
    if (cause != nullptr && cause->IsString() && !cause->AsString().empty())
      AppendNotification(mNotifications, cause->AsString().substr(0, 200));
    else
      AppendNotification(mNotifications,
                         sourceName.empty() ? "Someone died" : sourceName + " died");
  }
  } catch (...) {
    // Malformed packets and allocation failures must not escape into the client.
  }

bool Session::TakeNotification(std::string& text) {
  try {
    if (mNotifications.empty()) {
      text.clear();
      return false;
    }
    text = std::move(mNotifications.front());
    mNotifications.erase(mNotifications.begin());
    return true;
  } catch (...) {
    return false;
  }
}

void Session::CopyRecentTracked(std::vector< TrackedItem >& out, size_t cap) const {
  out.clear();
  if (cap == 0)
    return;
  const size_t count = std::min(cap, mTracked.size());
  out.assign(mTracked.end() - static_cast< ptrdiff_t >(count), mTracked.end());
}

std::string Session::PlayerName(int64_t slot) const {
  try {
    const auto player = mPlayers.find(slot);
    if (player != mPlayers.end() && !player->second.empty())
      return player->second;
    return "player " + std::to_string(slot);
  } catch (...) {
    return std::string();
  }
}

std::string Session::BuildConnect() const {
  std::string result = "{\"cmd\":\"Connect\",\"password\":" + Quote(mConfig.password) +
                       ",\"game\":" + Quote(mConfig.game) + ",\"name\":" + Quote(mConfig.slot) +
                       ",\"uuid\":" + Quote(ProcessUuid()) + ",\"version\":{\"class\":\"Version\",\"major\":" +
                       std::to_string(mConfig.versionMajor) + ",\"minor\":" +
                       std::to_string(mConfig.versionMinor) + ",\"build\":" +
                       std::to_string(mConfig.versionBuild) + "},\"items_handling\":" +
                       std::to_string(mConfig.itemsHandling) + ",\"tags\":[";
  for (size_t i = 0; i < mConfig.tags.size(); ++i) {
    if (i != 0)
      result.push_back(',');
    result += Quote(mConfig.tags[i]);
  }
  // The server routes DeathLink bounces by tag, so a client without it would
  // neither receive other players' deaths nor be expected to send its own.
  if (mConfig.deathLink &&
      std::find(mConfig.tags.begin(), mConfig.tags.end(), "DeathLink") == mConfig.tags.end()) {
    if (!mConfig.tags.empty())
      result.push_back(',');
    result += "\"DeathLink\"";
  }
  result += "],\"slot_data\":false}";
  return result;
}

std::string Session::BuildLocationChecks(const std::vector<int64_t>& ids) {
  std::string result = "{\"cmd\":\"LocationChecks\",\"locations\":[";
  for (size_t i = 0; i < ids.size(); ++i) {
    if (i != 0)
      result.push_back(',');
    AppendInt(result, ids[i]);
  }
  result += "]}";
  return result;
}

std::string Session::BuildSync() { return "{\"cmd\":\"Sync\"}"; }

bool Session::DeathLinkEnabled(const Config& config) {
  return config.deathLink;
}

std::string Session::BuildBounce(const std::string& cause) const {
  // A client that dies in DeathLink announces it with a Bounce aimed at the
  // "DeathLink" tag; the server relays it as Bounced to every client carrying
  // that tag, this one included. `time` is required (clients use it to spot
  // duplicates) and `source` is this slot's name, which is also how the echo
  // is recognised in HandlePacket. Sent only when the configuration asked for
  // DeathLink, so a normal session never emits one.
  if (!mConfig.deathLink)
    return std::string();
  // Unix seconds with millisecond precision, formatted by hand so the locale
  // cannot turn the decimal point into a comma.
  const int64_t millis = std::chrono::duration_cast< std::chrono::milliseconds >(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
  char time[32];
  std::snprintf(time, sizeof(time), "%lld.%03lld", static_cast< long long >(millis / 1000),
                static_cast< long long >(millis % 1000));
  std::string packet = "{\"cmd\":\"Bounce\",\"tags\":[\"DeathLink\"],\"data\":{\"time\":";
  packet += time;
  packet += ",\"source\":" + Quote(mConfig.slot);
  packet += ",\"cause\":" + Quote(cause.empty() ? mConfig.slot + " died" : cause);
  packet += "}}";
  return packet;
}

void Session::RewindTo(int64_t heldCount) {
  mState.nextItemIndex = 0;
  mState.progressive.clear();
  mTracked.clear();
  mGrantFrom = std::max<int64_t>(0, heldCount);
}

bool Session::MarkLocationChecked(const std::string& locationKey, int64_t& id) {
  const auto found = mConfig.locations.find(locationKey);
  if (found == mConfig.locations.end() ||
      std::find(mState.checkedLocations.begin(), mState.checkedLocations.end(), found->second) !=
          mState.checkedLocations.end())
    return false;
  id = found->second;
  mState.checkedLocations.push_back(id);
  return true;
}

bool Session::KnowsLocation(const std::string& locationKey) const {
  return mConfig.locations.find(locationKey) != mConfig.locations.end();
}

std::vector<int64_t> Session::AllLocationIds() const {
  std::vector<int64_t> ids;
  ids.reserve(mConfig.locations.size());
  for (const auto& location : mConfig.locations)
    ids.push_back(location.second);
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  return ids;
}

} // namespace Protocol
} // namespace PortAp
