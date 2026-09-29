#ifndef METROID_PRIME_PORT_PORT_APCLIENT_H
#define METROID_PRIME_PORT_PORT_APCLIENT_H
#include <cstdint>
#include <string>
#include <vector>

class CStateManager;

// Archipelago client.
//
// A background thread owns a WebSocket to the server and speaks the AP JSON
// protocol; the game thread only touches two queues. Location checks collected
// in game are queued with QueueCheck() and sent on that thread; items the server
// sends are queued and granted to the player state by Poll(), which the game
// calls once per simulation tick. Nothing here runs when no configuration is
// present.
//
// Configuration: $MP_AP_CONFIG, else <user dir>/archipelago.json. Each slot's
// game in each seed has a directory beside it, archipelago_games/<slot>-<seed>,
// holding its save card, game.json (how to reconnect) and archipelago_state.json
// (the last processed item index and the checks already sent, so reconnecting
// does not re-grant items).
//
// Environment:
//   MP_AP_DISABLE=1  ignore the configuration entirely
//   MP_AP_CONFIG=... configuration path
//   MP_AP_SEND_ALL=1 debug: on connect, queue every configured location id, to
//                    validate the map against the server (marks them all found)
namespace PortAp {

// Loads the configuration and starts the client thread once. Idempotent, never
// throws, and a no-op when disabled or unconfigured.
void EnsureLoaded();
// The overlay's Connect screen. The details live in the configuration file
// (other keys there are kept), so they survive a restart.
struct ConnectionDetails {
  std::string server;
  std::string slot;
  std::string password;
  bool enabled = true; // false after Disconnect: kept, but not connected at launch
};
ConnectionDetails SavedConnection();
// Saves the details and restarts the client with them, on a background thread
// (the status line shows the progress). False, with `error`, when the details
// are incomplete or the file cannot be written; nothing changes then.
bool Connect(const ConnectionDetails& details, std::string& error);
// Ends the session and keeps it off at the next launch, until Connect.
bool Disconnect(std::string& error);
// The directory whose memory card the running Archipelago game saves to, or ""
// for the default card (no session, or its seed not known yet).
std::string SaveCardDirectory();
// Where the configuration is read from and saved to.
std::string ConfigFilePath();
// A configuration with a server and a slot was loaded.
bool Enabled();
// The handshake with the server finished.
bool Connected();
// Short line for the F1 overlay: state, items received, checks sent.
const char* StatusText();
// Items granted to the player this session.
int ItemCount();
// Location checks sent this session.
int CheckCount();
// Most recent PrintJSON text, or "" when there has not been one.
const char* LastMessage();
// Drains the oldest human-readable notification (an item receipt or a
// PrintJSON line) into `text`. False when the queue is empty. Call from the
// game thread.
bool TakeNotification(std::string& text);
// One line of the item tracker: the item's display name, who sent it, and the
// step of a progressive sequence. `step` and `total` describe a flat item as
// 1 of 1. `from` is empty for an item the player sent themselves. The tracker
// list, oldest first; this reads the session's records and is safe from the
// overlay's thread.
struct TrackedItem {
  std::string name;
  std::string from;
  int step = 1;
  int total = 1;
};
std::vector< TrackedItem > TrackedItems();
// Seed name from the server's RoomInfo, or "" before it arrives.
const char* SeedName();

// Queues the configured location id for `locationKey` (the randomizer's
// "world:area:entity" key). No-op when unconfigured, unmapped, or already sent.
void QueueCheck(const char* locationKey);

// With the built-in tables, the pickups at this slot's locations hold the
// multiworld's items rather than the retail ones: touching one sends the check
// and grants nothing locally, and its "acquired" memo stays quiet. This holds
// for an AP game (a save some session has given items to) while the client is
// off too.
bool OwnsPickup(uint32_t world, uint32_t area, uint32_t entity);
bool OwnsMemo(uint32_t world, uint32_t area, uint32_t entity);
// Records an owned pickup in the game's save, so the check reaches the server
// on a later connection if this one cannot send it. Call before QueueCheck.
void RecordPickup(uint32_t world, uint32_t area, uint32_t entity);
// Shows on the HUD what an owned pickup held ("Found X for Bob"), or its
// location name when the server has not said yet. Call after QueueCheck.
void AnnouncePickup(uint32_t world, uint32_t area, uint32_t entity);

// A spawn point's Reset message replaced the whole inventory. Any received items
// the game held are gone, so the next Poll rewinds the session and the server
// replays them. No-op when AP is off.
void OnInventoryReset();

// The world a new game starts in, or 0 for the retail start. With the built-in
// tables this is Tallon Overworld (its first area is the Landing Site): the
// seeds the port supports skip the frigate.
uint32_t NewGameWorld();

// The connected seed wants heat to hurt through every suit but the Varia Suit
// (the AP world's non_varia_heat_damage, on by default). False when AP is off.
bool VariaOnlyHeatProtection();

// Spring Ball as the connected seed has it, for a seed on the built-in tables
// whose slot_data has arrived: 0 off, 1 once the Morph Ball Bombs are held,
// 2 on (its item, or the first Progressive Bomb, has been received). -1 when
// no such seed is connected, so the port's own setting applies.
int SpringBallRule();

// Called once per simulation tick by CStateManager::Update. Sends queued checks
// and grants queued items to the player state.
void Poll(CStateManager& mgr);

} // namespace PortAp

#endif // METROID_PRIME_PORT_PORT_APCLIENT_H
