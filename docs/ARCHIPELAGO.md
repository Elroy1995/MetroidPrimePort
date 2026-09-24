# Archipelago client

The port can join an Archipelago multiworld directly: a native client in
`platform/port_apclient.cpp` speaks the AP protocol over a WebSocket and hands
items to the player state, and collected pickups are reported back as location
checks. All of it is opt-in; with no configuration the port is unaffected.

This is the transport and protocol half. What makes a seed *playable* — the
placement logic and the location/item id tables — comes from the AP world
(`randomprime` / Archipelago's Metroid Prime world) and is not part of the port.

## How it connects

- `CScriptPickup::Touch` reports a collected pickup: `PortAp::QueueCheck()` looks
  the location key up in the configuration and queues the AP location id.
- `CStateManager::Update` calls `PortAp::Poll()` every simulation tick, which
  sends queued checks and grants queued items
  (`CPlayerState::InitializePowerUp` + `IncrPickUp`, the same calls the retail
  pickup makes, plus a health refresh for energy tanks).
- A background thread owns the socket: it connects, handshakes
  (RoomInfo → Connect → Connected), handles `ReceivedItems`/`PrintJSON`/
  `ConnectionRefused`/`InvalidPacket`, answers pings, and reconnects with
  backoff. Only the two queues cross between threads.

Transport is a minimal RFC 6455 client (`platform/port_ws.h`): plain `ws://`,
text frames, no TLS, no extensions and no per-message compression (servers still
accept uncompressed connections, but it is deprecated on their side).

## Configuration

Path: `$MP_AP_CONFIG`, else `<user dir>/archipelago.json` (`<user dir>` is
`$MP_USER_PATH`, else SDL's per-user path).

```json
{
  "server": "ws://127.0.0.1:38281",
  "slot": "Player1",
  "password": "",
  "game": "Metroid Prime",
  "items_handling": 7,
  "version": { "major": 0, "minor": 6, "build": 0 },
  "tags": [],
  "locations": {
    "39F2DE28:B2701146:0000007E": 123456
  },
  "items": {
    "1234": { "item": "EnergyTanks", "amount": 1, "capacity": 1 }
  }
}
```

| Key | Notes |
|---|---|
| `server` | Required. `ws://host[:port][/path]`. `wss://` is not supported. |
| `slot` | Required. Player name in the multiworld. |
| `game` | Default `Metroid Prime`; must match the AP world's game name. |
| `password` | Room password, default empty. |
| `items_handling` | Default 7 (other worlds' items, this world's items, starting inventory). |
| `version` | Protocol version sent in Connect, default 0.6.0. Must be compatible with the server's. |
| `tags` | Client tags, default empty. |
| `locations` | Randomizer key (`WORLD:AREA:ENTITY`, the seed's location key) to AP location id. |
| `items` | AP item id to the grant the port applies: `item` is a randomizer item name, `amount`/`capacity` default 1. |

Unknown keys are ignored so the file can grow. A configuration without a server
or slot, a malformed location key, or an unknown item name disables the client
and logs why.

Environment:

- `MP_AP_DISABLE=1` ignores the configuration.
- `MP_AP_CONFIG=<path>` selects the file.
- `MP_AP_SEND_ALL=1` debug: on connect, sends every configured location id.
  Use it to check a mapping against the server before playing — it marks them
  all as found, so do not use it on a real run.

## State

`archipelago_state.json` sits next to the configuration and remembers the last
processed item index and the checks already sent, so reconnecting does not hand
the player the same items twice. It is tied to the slot name; a file with a
different slot is ignored.

**Delete it when you start a new save file**, otherwise the client believes the
items it already granted belong to the new run and skips them.

## Producing the id maps

`locations` and `items` are data from the AP world for your seed, not something
the port can derive:

- Item ids and location ids are the ones in the world's data package.
- Map each AP location id to the matching `WORLD:AREA:ENTITY` key. The keys are
  the randomizer's own location keys, so `MP_RANDO_DUMP=1` (see
  `docs/RANDOMIZER.md`) gives you the key list per area to match names against.
- Map each item id to the randomizer item name and the amount/capacity the
  pickup should grant (e.g. missiles `amount`/`capacity` 5, an energy tank 1).

## Verified

- `port_apclient_tests` covers configuration parsing and its error cases, the
  location map, the packet state machine (Connected, ReceivedItems as objects
  and as four-element arrays, already-processed items, unknown item ids, index
  jumps forcing a `Sync`, ConnectionRefused, PrintJSON truncation), and the
  state file round trip.
- `port_ws_tests` pins SHA-1 to the RFC 3174 vectors, base64, the RFC 6455
  `Sec-WebSocket-Accept` example, URL parsing, frame encoding/decoding,
  fragmentation, control frames and the size limit.
- End to end against `tools/ap_fake_server.py`: the client performed the
  WebSocket handshake and sent a well-formed Connect; the server's items were
  granted to the player state (the HUD's missile readout went from 15 to the
  granted 250); with `MP_AP_SEND_ALL=1` the client sent
  `LocationChecks {"locations":[1001,1002]}`; and `archipelago_state.json`
  recorded `next_item_index 2`, so a reconnect does not re-grant.

## Not done yet

- **TLS.** `wss://` servers (including the hosted service) need a TLS stack;
  only plain `ws://` works, so use a self-hosted or tunnelled server.
- **Compression.** No `permessage-deflate`; Archipelago accepts uncompressed
  connections but marks them deprecated.
- **DeathLink, hints, chat, tracker.** Bounce/DeathLink, hint creation and the
  item tracker are unimplemented; `PrintJSON` is stored for the overlay but not
  displayed in-game yet.
- **A mapping generator.** The id maps must be produced from the AP world by
  hand or by a script that does not exist here yet.
- **Status in-game.** `PortAp::StatusText()` is available for the F1 overlay but
  is not shown there yet.
