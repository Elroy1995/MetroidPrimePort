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
  int64_t index = 0;   // position in the slot's received-items list
};

// One item as the tracker shows it: the name a player reads, where it came
// from, and which step of a progressive sequence it was. A flat item is step 1.
struct TrackedItem {
  int64_t itemId = 0;
  std::string name;
  std::string from;  // player alias, empty when the item is the player's own
  int64_t step = 1;  // 1-based; the last step repeats for later copies
  int64_t total = 1; // steps in the sequence, 1 for a flat item
};

// A configured item: one grant, or a progressive sequence where the Nth copy
// received grants step N and later copies repeat the last step. The inherited
// fields are the flat grant, and mirror step 0 for a progressive item.
struct ItemEntry : ItemGrant {
  std::vector< ItemGrant > progressive; // empty for a flat item

  bool IsProgressive() const { return !progressive.empty(); }
  // The grant for a copy received after `count` earlier copies of this id.
  const ItemGrant& Step(int64_t count) const;
};

// archipelago.json, as documented in docs/ARCHIPELAGO.md.
struct Config {
  std::string server; // ws:// or wss://host[:port][/path]
  std::string slot;
  std::string password;
  std::string game = "Metroid Prime";
  // PEM CA bundle for wss:// servers the system trust store does not cover.
  // Empty means the system store. Stored as written; a relative path is
  // resolved against the config file's directory by the client.
  std::string tlsCa;
  int itemsHandling = 7;
  // DeathLink: when another player dies, this one dies too. Off unless the
  // configuration asks for it, so a session that never opted in is unaffected.
  bool deathLink = false;
  std::vector< std::string > tags;
  int versionMajor = 0;
  int versionMinor = 6;
  int versionBuild = 0;
  std::map< std::string, int64_t > locations; // randomizer key -> AP location id
  std::map< int64_t, ItemEntry > items;       // AP item id -> grant
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
  // Seed name this progress belongs to, learned from the server's RoomInfo. A
  // different seed means a different session, so the progress is discarded
  // rather than replayed into it. Empty in files written before this existed.
  std::string seed;
  int64_t nextItemIndex = 0;
  std::vector< int64_t > checkedLocations;
  // Progressive item id -> copies processed so far. Persisted because items
  // below nextItemIndex are skipped on reconnect, so the counts cannot be
  // rebuilt from what the server resends. Absent in older files: all zero.
  std::map< int64_t, int64_t > progressive;
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

  // Everything received this session, oldest first, for the item tracker. The
  // notification queue is the same information but capped and drained by
  // whoever displays it, so a player who misses a HUD line loses it for good;
  // this is the whole session and is bounded by its own cap.
  const std::vector< TrackedItem >& Tracked() const { return mTracked; }

  // DeathLink. `DeathsPending` is a count of bounces the server has sent that
  // the game has not applied yet, so one that arrives at the title screen is
  // not lost. `TakeDeathPending` returns how many are owed and clears them,
  // which the client calls once it has killed the player. `LastDeathSource` is
  // who to name (the sender's slot name), empty when the packet did not say.
  int DeathsPending() const { return mDeathsReceived; }
  int TakeDeathPending() {
    const int owed = mDeathsReceived;
    mDeathsReceived = 0;
    return owed;
  }
  const std::string& LastDeathSource() const { return mLastDeathSource; }
  // The DeathLink Bounce packet for a death of this client's own, or "" when
  // the configuration does not enable DeathLink. `cause` is the text other
  // players see; empty means "<slot> died".
  std::string BuildBounce(const std::string& cause = std::string()) const;
  // Whether the configuration asked for DeathLink at all, which the world's
  // RoomInfo is what actually agrees to; both must say yes.
  static bool DeathLinkEnabled(const Config& config);

  // Records a collected location. False when the key has no id, or was already
  // checked; otherwise `id` is the AP location id to send.
  bool MarkLocationChecked(const std::string& locationKey, int64_t& id);
  // Whether the location table has an id for this key at all, as opposed to it
  // being a key that was already checked. Both cases are a false return from
  // MarkLocationChecked, but only one is a problem, and it is a silent one: a
  // key with no id is dropped on the floor, so the session plays normally and
  // the server simply never records the check.
  bool KnowsLocation(const std::string& locationKey) const;
  // Every configured location id, for the MP_AP_SEND_ALL debug path.
  std::vector< int64_t > AllLocationIds() const;

  const Config& GetConfig() const { return mConfig; }
  const State& GetState() const { return mState; }
  // Lines the session up with a loaded game that holds the first `heldCount`
  // received items: the next full inventory (the reply to Connect or Sync) is
  // replayed from the start, granting only the items from `heldCount` on and
  // rebuilding the progressive counts from the ones before it.
  void RewindTo(int64_t heldCount);
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
  // Why the loaded progress was thrown away on the last RoomInfo, empty when
  // nothing was. The state itself is already reset when this is set.
  const std::string& ResetReason() const { return mResetReason; }
  // Copies the tail of the tracked receipts, newest last, into `out`, replacing
  // its contents. The HUD only cares about the last few; the F1 tracker calls
  // Tracked() directly for the whole session.
  void CopyRecentTracked(std::vector< TrackedItem >& out, size_t cap) const;

private:
  Config mConfig;
  State mState;
  bool mHandshakeComplete = false;
  bool mDesynced = false;
  // Received items below this index are in the loaded game already; see RewindTo.
  int64_t mGrantFrom = 0;
  std::string mSlotDescription;
  std::string mLastMessage;
  std::string mLastError;
  std::string mSeedName;
  std::string mResetReason;
  int64_t mOwnSlot = 0;
  std::map< int64_t, std::string > mPlayers;
  std::vector< std::string > mNotifications;
  std::vector< TrackedItem > mTracked;
  // DeathLink bookkeeping: bounces owed to the game, and who to blame.
  int mDeathsReceived = 0;
  std::string mLastDeathSource;
};

} // namespace Protocol
} // namespace PortAp

#endif // METROID_PRIME_PORT_PORT_AP_PROTOCOL_H
