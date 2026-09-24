#ifndef METROID_PRIME_PORT_PORT_AP_PROTOCOL_H
#define METROID_PRIME_PORT_PORT_AP_PROTOCOL_H
#include "port_json.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// Protocol core of the Archipelago client: the configuration and its item and
// location maps, the packet state machine, and the queues of outgoing packets
// and incoming items. It deliberately knows nothing about sockets or game
// state, so the tests drive it directly. port_apclient.cpp owns the socket
// thread and the step that turns ItemGrants into player-state changes.
namespace PortAp {
namespace Protocol {

// One item the server told us we received, resolved to the port's item model.
struct ItemGrant {
  int64_t itemId = 0;
  int itemType = -1; // CPlayerState::EItemType value; -1 when the id is unknown
  int amount = 1;
  int capacity = 1;
  std::string display; // name shown to the player; the item name when unset
};

// archipelago.json, as documented in docs/ARCHIPELAGO.md.
struct Config {
  std::string server; // ws://host[:port][/path]
  std::string slot;
  std::string password;
  std::string game = "Metroid Prime";
  int itemsHandling = 7;
  std::vector< std::string > tags;
  int versionMajor = 0;
  int versionMinor = 6;
  int versionBuild = 0;
  std::map< std::string, int64_t > locations; // randomizer key -> AP location id
  std::map< int64_t, ItemGrant > items;       // AP item id -> grant
  bool valid = false;
  std::string error; // why the configuration was rejected, for the log
};

// Parses configuration text. Never throws; `valid` reports whether a server and
// a slot were found. Unknown keys are ignored so the file can grow.
Config ParseConfig(const std::string& text);
// Reads and parses a file; a missing or unreadable file yields an invalid
// configuration whose `error` says so.
Config LoadConfigFile(const std::string& path);

// archipelago_state.json: what the client must remember between sessions so a
// reconnect does not hand the player the same items twice.
struct State {
  std::string slot;
  int64_t nextItemIndex = 0;
  std::vector< int64_t > checkedLocations;
};
State LoadStateFile(const std::string& path);
// Writes the state through a temporary file and renames it into place.
bool SaveStateFile(const std::string& path, const State& state);

// One connection's protocol state.
class Session {
public:
  Session(const Config& config, const State& state);

  // Handles one server packet (a JSON object carrying a "cmd"). Outgoing
  // packets are appended to `outgoing`, and items to grant to `granted`.
  void HandlePacket(const PortJson::Value& packet, std::vector< std::string >& outgoing,
                    std::vector< ItemGrant >& granted);

  // Human-readable notifications for the HUD and overlay, oldest first: item
  // receipts ("Energy Tank from Bob") and PrintJSON text. Drained by whoever
  // displays them, so nothing is shown twice. The queue is capped.
  bool TakeNotification(std::string& text);
  // Seed name from RoomInfo, or "" before it arrives.
  const std::string& SeedName() const { return mSeedName; }
  // Alias of a player slot from Connected, or "player <slot>" when unknown.
  std::string PlayerName(int64_t slot) const;

  // The Connect packet to send after RoomInfo.
  std::string BuildConnect() const;
  static std::string BuildLocationChecks(const std::vector< int64_t >& ids);
  static std::string BuildSync();

  // Records a collected location. False when the key has no id, or was already
  // checked; otherwise `id` is the AP location id to send.
  bool MarkLocationChecked(const std::string& locationKey, int64_t& id);
  // Every configured location id, for the MP_AP_SEND_ALL debug path.
  std::vector< int64_t > AllLocationIds() const;

  const Config& GetConfig() const { return mConfig; }
  const State& GetState() const { return mState; }
  void SetState(const State& state) { mState = state; }
  bool HandshakeComplete() const { return mHandshakeComplete; }
  // "slot 3, team 0" after Connected, empty before.
  const std::string& SlotDescription() const { return mSlotDescription; }
  // Most recent PrintJSON text, truncated for the overlay.
  const std::string& LastMessage() const { return mLastMessage; }
  // Most recent refusal or protocol complaint.
  const std::string& LastError() const { return mLastError; }
  // Set when ReceivedItems arrived with an index other than the expected one.
  bool Desynced() const { return mDesynced; }

private:
  Config mConfig;
  State mState;
  bool mHandshakeComplete = false;
  bool mDesynced = false;
  std::string mSlotDescription;
  std::string mLastMessage;
  std::string mLastError;
  std::string mSeedName;
  int64_t mOwnSlot = 0;
  std::map< int64_t, std::string > mPlayers;
  std::vector< std::string > mNotifications;
};

} // namespace Protocol
} // namespace PortAp

#endif // METROID_PRIME_PORT_PORT_AP_PROTOCOL_H
