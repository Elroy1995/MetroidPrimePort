#ifndef METROID_PRIME_PORT_PORT_APCLIENT_H
#define METROID_PRIME_PORT_PORT_APCLIENT_H
#include <cstdint>

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

// Queues the configured location id for `locationKey` (the randomizer's
// "world:area:entity" key). No-op when unconfigured, unmapped, or already sent.
void QueueCheck(const char* locationKey);

// Called once per simulation tick by CStateManager::Update. Sends queued checks
// and grants queued items to the player state.
void Poll(CStateManager& mgr);

} // namespace PortAp

#endif // METROID_PRIME_PORT_PORT_APCLIENT_H
