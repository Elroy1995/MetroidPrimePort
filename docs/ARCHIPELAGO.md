# Archipelago client

The port can join an Archipelago multiworld directly: a native client in
`platform/port_apclient.cpp` speaks the AP protocol over a WebSocket and hands
items to the player state, and collected pickups are reported back as location
checks. All of it is opt-in; with no configuration the port is unaffected.

This is the transport and protocol half. What makes a seed *playable* — the
placement logic and the location/item id tables — comes from the AP world
(`randomprime` / Archipelago's Metroid Prime world) and is not part of the port.
The port consumes that world's `Locations.py`; when the world's own file is not
to hand, the spec-shaped stand-in described under
[Producing the id maps](#producing-the-id-maps) keeps the join, the generator
and the tests exercisable.

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

Transport is a minimal RFC 6455 client (`platform/port_ws.h`): text frames, no
extensions and no per-message compression (servers still accept uncompressed
connections, but it is deprecated on their side). `ws://` needs nothing extra;
`wss://` uses OpenSSL when the build finds it.

### TLS

`wss://` is verified by default — no option disables it — with the system trust
store or the `tls_ca` file from the configuration (a relative path is resolved
against the configuration file's directory), TLS 1.2 as the floor, and the
certificate checked against the host name.

OpenSSL is an optional build dependency: CMake enables it when it finds it and
prints `OpenSSL <version> found: wss:// enabled`. Android's NDK ships no
OpenSSL, so Android builds refuse `wss://` with `wss:// is not supported: this
build has no TLS (built without OpenSSL)` rather than downgrading to plaintext;
Windows needs OpenSSL provided to CMake.

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
| `server` | Required. `ws://host[:port][/path]` or `wss://...`. |
| `tls_ca` | Optional PEM CA bundle to verify a `wss://` server that is not in the system trust store. Relative paths resolve against this file's directory. |
| `slot` | Required. Player name in the multiworld. |
| `game` | Default `Metroid Prime`; must match the AP world's game name. |
| `password` | Room password, default empty. |
| `items_handling` | Default 7 (other worlds' items, this world's items, starting inventory). |
| `version` | Protocol version sent in Connect, default 0.6.0. Must be compatible with the server's. |
| `tags` | Client tags, default empty. |
| `locations` | Randomizer key (`WORLD:AREA:ENTITY`, the seed's location key) to AP location id. |
| `items` | AP item id to the grant the port applies: `item` is a randomizer item name, `amount`/`capacity` default 1, and an optional `display` is the name shown in the HUD notification (the item name when unset). An entry may instead carry `progressive`, a non-empty list of grants applied in order as more copies of that item arrive. |

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
processed item index, the checks already sent, and the progressive step counts,
so reconnecting does not hand the player the same items twice.

It is tied to the **slot name and the seed**. A file with a different slot is
ignored, and so is progress recorded against a different seed: the first
`RoomInfo` compares the server's seed name with the one in the file, and when
they differ it throws the progress away, says so in the log, and continues with
an empty state. That is deliberate — those checks were only true for the session
that granted them, and replaying them into a different multiworld makes the
client claim locations it never collected while skipping the items the server
still owes it. A file written before the seed was recorded has no seed in it, so
the first connect adopts the server's and keeps the progress.

On every connect the whole recorded check list is re-sent, so a reconnect or a
reloaded save re-announces what was already collected.

**Set `MP_AP_RESET_STATE=1` to discard the file before connecting.** That is the
way out for the one case which cannot be detected: a new game, or an older save
loaded on the same slot *and* the same seed, looks exactly like continued
progress, and the client would skip items the server believes were granted.

## Producing the id maps

`locations` and `items` come from the AP world for your seed. `tools/make_ap_config.py`
reads the world's `Locations.py` (parsed with `ast`, never imported) and a
randomizer location dump, and writes both `archipelago.json` and a
`randomizer_seed.json` from the spoiler log:

```sh
# 1. Dump every world's item locations once (see docs/RANDOMIZER.md), then:
python3 tools/make_ap_config.py \
    --locations /path/to/MetroidAPrime/Locations.py \
    --dump randomizer_locations.log \
    --server ws://host:38281 --slot Player1 \
    --spoiler spoiler.json \
    --out archipelago.json --seed-out randomizer_seed.json
```

### The world's own file

The world is `github.com/UltiNaruto/MetroidAPPrime` - note the single "P" in
`Prime`. `MetroidAPPrime` is a different name, and asking for it gets a
confusing 404 from GitHub that looks exactly like a private repository. Its
`src/Locations.py` is what this tool wants:

```sh
git clone --depth 1 https://github.com/UltiNaruto/MetroidAPPrime
python3 tools/make_ap_config.py \
    --locations MetroidAPPrime/src/Locations.py \
    --dump randomizer_locations.log \
    --server wss://host:38281 --slot Player1 \
    --out archipelago.json
```

Against the real table and a full port dump, the join maps **95 of 100**
locations with 75 areas clean. The five it does not map are all dump coverage,
not pairing, and it says so per location rather than guessing:

- `Chozo Ruins: Main Plaza - Locked Door` and `Chozo Ruins: Ruined Shrine -
  Plated Beetle` have no counterpart in the dump: those pickups sit in layers a
  plain area tour does not build.
- Two more are count mismatches - an area with two AP locations and one dumped
  pickup - and a third is the same shape in Phazon Mines.

The areas with a mixed id delta are reported as **review** rather than mapped on
a guess, which is why `--strict` refuses to write. Filling them is
`--extra` (see below), and the honest way to get more is a dump that visits
those layers.

### When the world's file is not to hand

`tools/ap-world-fixture/Locations.py` is a **spec-shaped stand-in** generated
from a real port dump: the entity ids, areas, vanilla items and the count (100)
are real, while the AP ids are synthetic and assigned from 50310000 so none can
be mistaken for a real one. Regenerate it with:

```sh
python3 tools/make_ap_fixture.py --dump randomizer_locations.log \
    --out tools/ap-world-fixture
```

`ctest -R port_ap_fixture` checks that the generator reproduces the committed
file, that `--strict` maps every location in it, and that the config it writes
carries all of them plus the four progressive beams. It is a stand-in for CI and
offline use; prefer the world's own `Locations.py` for anything real.

How it joins the two sides, and what to watch for:

- The AP table gives each location a name and an id, and `PICKUP_LOCATIONS` gives
  the matching in-game entity id, whose high 16 bits are the area index. The
  dump gives the port's `world:area:entity` keys, which carry the same area
  index. Areas are joined by that index, and the pickups inside an area are
  paired by id delta, never by rank.
- The tool prints a per-area report with the pairing and the id delta between
  the AP entity and the port's editor id. A uniform delta means the pairing is
  clean; a mixed one is flagged **review** (`--strict` makes that fatal). Real
  data has such a case in Chozo Ruins Main Plaza, so expect to look at a few.
- Areas missing from the dump are reported, never guessed: to map a world you
  must have dumped it.
- `--report` and the warnings are the point — a wrong mapping reports other
  players' checks, so treat any flagged area as unverified until it is confirmed
  in game (`MP_AP_SEND_ALL=1` against the server, or by noting that the first
  pickup grants what the spoiler says).

The spoiler seed turns each location into a pickup: your own items are placed
normally, and a location holding another player's item becomes a placeholder
(`UnknownItem1` with zero amount) so the in-game pickup grants nothing locally
while the real item arrives over the network.

Item ids in `archipelago.json` mirror the AP world's `Items.py`, whose ids
correspond 1:1 with the port's `CPlayerState::EItemType` for 0-28, and whose
29-40 are the artifacts.

The AP world's progressive beam items use the `progressive` form and follow its
`PROGRESSIVE_ITEM_MAPPING`: the first copy of id 5031043 grants the Power Beam,
the second the Charge Beam and the third the Super Missile (and likewise for the
Ice, Wave and Plasma beams). How many copies have arrived is kept in
`archipelago_state.json` so a reconnect resumes at the right step instead of
starting over; a fresh inventory (or deleting the state file) starts from the
first step again. Ids 47-50, which the server only sends for tracking, are
mapped to the Charge Beam so an unexpected one is harmless.

## Notifications

Received items and server messages are queued as text and shown one at a time,
about every two seconds, as a HUD memo (`"Missile Expansion"`, `"Energy Tank"`).
The F1 overlay's Session tab shows the connection state, seed name, item and
check counts and the last server message, but the port's screenshot capture does
not include Aurora's UI layer, so an F1 screenshot will not show it.

## Verified

- `port_apclient_tests` covers configuration parsing and its error cases, the
  location map, the packet state machine (Connected, ReceivedItems as objects
  and as four-element arrays, already-processed items, unknown item ids, index
  jumps forcing a `Sync`, ConnectionRefused, PrintJSON truncation), and the
  state file round trip.
- `port_ws_tests` pins SHA-1 to the RFC 3174 vectors, base64, the RFC 6455
  `Sec-WebSocket-Accept` example, URL parsing, frame encoding/decoding,
  fragmentation, control frames and the size limit. Its TLS test generates a CA
  and server certificate, runs `tools/ap_fake_server.py --tls`, completes a
  handshake and asserts four rejections: wrong CA, host-name mismatch, system
  trust store only, and a missing CA file.
- With the game: `wss://127.0.0.1` with a `tls_ca` connected, sent its Connect
  and received items; a missing CA was refused and never fell back to plaintext.
- End to end against `tools/ap_fake_server.py`: the client performed the
  WebSocket handshake and sent a well-formed Connect; the server's items were
  granted to the player state (the HUD's missile readout went from 15 to the
  granted 250); with `MP_AP_SEND_ALL=1` the client sent
  `LocationChecks {"locations":[1001,1002]}`; and `archipelago_state.json`
  recorded `next_item_index 2`, so a reconnect does not re-grant.
- The notification path was captured on screen: an item whose config carries
  `"display": "Energy Tank"` produced that text as a HUD memo.
- `tools/make_ap_config.py --self-test` passes, and a run against the real
  AP `Locations.py` shape plus a real Chozo Ruins dump mapped 5 locations, with
  the clean areas at a uniform `+1` delta and Main Plaza flagged for review.

### What the 100/100 join does and does not prove

The fixture check (`ctest -R port_ap_fixture`, 100/100 mapped, nothing missing)
is worth stating precisely, because it is easy to over-read. **The fixture is
generated from the same port dump that produces the keys.**
`tools/make_ap_fixture.py` reads `randomizer_locations.log` and writes both the
`Locations.py` and the `PICKUP_LOCATIONS` list, so the table's keys and the
game's keys come from one source. The 100/100 therefore proves the port's
**internal consistency** — that a key the game produces resolves to exactly the
id the table assigns, with nothing lost in between — and it pins the join against
regressions. It does **not** prove that a real Archipelago world uses the same
key scheme.

That is the live risk, and it is not hypothetical: if the upstream world keys its
locations by anything other than `world:area:entity` — a room name, a tuple, a
different hash — then every location the player collects fails to resolve. Before
this was fixed that failure was **silent**. `MarkLocationChecked` answers false
both for a key it has already recorded and for a key it has never heard of;
`QueueCheck` treated both as "nothing to do" and returned. The session connects,
items arrive, the HUD updates, the seed plays — and the server records zero
checks, with nothing in any log to explain why.

`Session::KnowsLocation` now separates the two cases and `QueueCheck` reports the
second one once, naming the offending key and pointing here. It does not spam per
pickup, because a legitimate session can touch an unconfigured location and that
should not become noise. So the failure mode is gone, but the underlying
assumption is still unverified: it can only be closed against a real world file,
which is why the gap below is still a gap and not a done item.

What *is* now pinned, device-free and in CI: the key format itself
(`tests/port_randomizer.cpp` asserts `FormatLocationKey(1,2,3)` produces
`00000001:00000002:00000003`), and the distinction between "already recorded" and
"never configured" (`tests/port_apclient.cpp`).

## Not done yet

Each entry says whether it is a decision or a gap, and why. "Descoped" means
the port deliberately does not do this, and a multiworld session without it
still works; anything marked as a gap still limits a session.

- **TLS on Android — a gap, and the only one that stops a session.** `wss://`
  needs OpenSSL found at configure time. Linux and Windows are both verified
  against a real TLS server, including every rejection case, in CI. Android's
  NDK has no OpenSSL, so a JNI `SSLSocket` backend or a vendored TLS library is
  what that platform needs. Plain `ws://` works on Android today, so an Android
  player can join a multiworld on a server that offers no TLS, but not one that
  requires it.
- **Compression — descoped.** The client does not offer `permessage-deflate`.
  Archipelago still accepts uncompressed connections and only marks them
  deprecated, so a session is not limited by it; the cost is bandwidth on large
  seeds, not correctness. Adding it would mean inflating and deflating message
  frames in `PortWs`, which is the one part of this client written for
  correctness rather than size, and a compression bug is a desync rather than a
  failed connection. It is the cheapest item here to add later and the least
  valuable.
- **Item tracker — done.** The F1 overlay's Archipelago section lists what the
  session received: display name, who sent it, and the step of a progressive
  sequence. A receipt no longer disappears because the player was not looking
  at the HUD when it arrived.
- **Chat — descoped.** `PrintJSON` is received, queued and shown on the HUD and
  in the overlay, so what the server says reaches the player. What is missing is
  sending: there is no text input, so the client cannot answer a hint request or
  talk to other players. A session plays without it.
- **Hints — descoped, and it depends on chat.** Archipelago hints are requested
  over chat (`!hint <name>`), so hint support needs the chat input above before
  it can mean anything. Without it the F1 tracker shows which items have arrived
  and in what order, which is the part a solo player actually uses.
- **DeathLink — done, opt-in.** Set `"death_link": true` in `archipelago.json`
  and a `Bounce` from another player kills this one, naming them; this client's
  own deaths are announced once. A bounce is owed to the game rather than
  applied on the socket thread, so one that arrives during a load is not lost,
  and an echo of this client's own death is ignored rather than killing the
  player twice. The mechanism is the same one the world's own client uses:
  clearing the alive flag, which is what drives the death sequence in the game.
- **Mapping verification — a gap in one direction only.** The join itself is
  checked against the spec-shaped fixture (`ctest -R port_ap_fixture`): 100/100
  mapped, no area for review, no area missing. But that fixture is generated from
  the same dump that produces the keys, so the check proves internal consistency
  rather than agreement with a real world — see "What the 100/100 join does and
  does not prove" above. What remains unverified is whether a real
  Archipelago world keys its locations the way this port does. It does not need
  a played seed to narrow: the key format is a pure function of
  (world, area, entity) and is already pinned by a test against real dumped
  keys. It needs a real world file. Until one is reachable, the client now at
  least says so out loud rather than dropping every check silently.
