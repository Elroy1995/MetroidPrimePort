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
// Configuration: $MP_AP_CONFIG, else <user dir>/archipelago.json. State (the
// last processed item index and the checks already sent) lives beside it in
// archipelago_state.json so reconnecting does not re-grant items.
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

// A spawn point's Reset message replaced the whole inventory. Any received items
// the game held are gone, so the next Poll rewinds the session and the server
// replays them. No-op when AP is off.
void OnInventoryReset();

// Called once per simulation tick by CStateManager::Update. Sends queued checks
// and grants queued items to the player state.
void Poll(CStateManager& mgr);

} // namespace PortAp

#endif // METROID_PRIME_PORT_PORT_APCLIENT_H
