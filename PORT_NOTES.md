# Metroid Prime port — working notes

## Item tracker, and the on-disc rewrite re-verified (2026-09-26)

- An item arriving while the player was not looking at the HUD used to be lost
  for good: the notification queue is capped at 32 and drained by whoever shows
  it. The session now keeps the receipts - display name, sender, progressive
  step - and the F1 overlay's Archipelago section lists them. No protocol change
  was needed; the data was already in `Session`, and the sender alias was already
  resolved for the notification text.
- One subtlety worth recording: an `ItemEntry`'s flat `display` field mirrors
  step 0, so reading it directly labelled every copy of a progressive item
  "Power Beam". The tracker resolves the step the grant actually used. Eight new
  assertions cover names, step numbering, an unknown item id, copies past the
  last step, and sender attribution.
- The 100-location randomizer path was re-verified end to end with the current
  binary: the full dump feeds `tools/rando_seed.py` (100 locations, 33 models),
  and a seeded run of the game rewrites pickups on disc, logging e.g.
  `PLACE 39F2DE28:B2701146:0000007E Missiles -> Wavebuster amount=1 capacity=1
  model=74A39FE6 acs=7C04E388` and an energy tank replacing a missile at
  `B9ABCD56:000801FB`. Six such rewrites across fourteen areas of the Tallon
  Overworld, each carrying a model and animation from the seed.
- Worth remembering when testing this by hand: `MP_RANDO_DUMP` takes precedence
  over `MP_RANDO_SEED`, and dump mode returns before any placement is applied.
  Running both makes the run look like it ignored the seed. A seed run wants
  `MP_RANDO_SWEEP=1` and no `MP_RANDO_DUMP`.


## A spec-shaped AP world table, so the location join is exercisable (2026-09-26)

- The join tool had nothing to run against: the upstream world's repository
  answers for metadata but 404s every content path, so `--strict` could only be
  driven by the tool's own built-in self-test fixture.
  `tools/make_ap_fixture.py` writes the two files `make_ap_config.py` reads - a
  per-world location table and a `PICKUP_LOCATIONS` list of level and entity ids
  - from a real port dump, and `tools/ap-world-fixture/Locations.py` is that
  generated file.
- What is real in it: every entity id, the area each lives in, the vanilla item,
  and the count of 100, which is retail Prime's number, all taken from the
  sweep. What is synthetic and documented as such: the AP location ids,
  assigned from 50310000 so none can be mistaken for a real one, and the names,
  which are derived from the dump. It stands in for the upstream table, it is
  not a copy of it.
- Verified end to end rather than merely generated: `--strict` over this table
  and the full dump reports **100 locations mapped, 0 unmapped, 82 areas clean,
  0 for review, 0 missing from the dump**; the config it writes carries all 100
  locations and 51 items with the four progressive beams; and the port connects
  with that generated config against `tools/ap_fake_server.py`, receives the
  three items it offers and records the progressive step (`next_item_index` 3,
  `progressive {"5031043":1}`).
- `tests/port_ap_fixture.py` is registered as a ctest target so this cannot rot:
  it checks the generator reproduces every location from the same dump, that
  `--strict` maps all of them, and that the written config carries every
  location, passes the server through and still emits four progressive beams.
  It builds its dump from the fixture, since a real sweep log is far too large
  to commit. 14/14 port tests pass on Linux and Windows.


## Android APK builds again, and the Archipelago client was missing from it (2026-09-26)

- `tools/android_apk.sh :app:assembleRelease` works from this tree with the
  cached SDK/NDK and produces an 11 MB RelWithDebInfo APK carrying
  `lib/arm64-v8a/libmetroid_prime_port.so`, with no disc image or game asset
  inside. All the toolchain is present: SDK at `~/android/sdk`, the pinned NDK
  at `~/android-ndk-cache/android-ndk-r29`, Gradle 8.7 in the user's wrapper
  cache, and a Rust toolchain under `build/android-rust`.
- It did not build at first: `platform/port_ws.cpp` used `IPPROTO_TCP` without
  including `<netinet/in.h>`, where Bionic declares it (glibc and Winsock both
  get it from `<netdb.h>`). So the entire WebSocket client — and therefore the
  entire Archipelago feature — was absent from every Android build until now.
- `wss://` on Android remains impossible as configured: the NDK ships no
  OpenSSL, so there is no `MP_HAVE_OPENSSL` and the client refuses a `wss://`
  server instead of downgrading it. A JNI `SSLSocket` backend or a vendored TLS
  library is the remaining work.
- The port's diagnostics were `fprintf(stderr, …)`, which Android discards, so a
  device run that exited during startup reported nothing. They now go to logcat
  under the `metroidprime` tag. That is what made the disc-path failure below
  readable rather than invisible; desktop output is unchanged.
- On-device findings from one run of the built APK, recorded for whoever picks
  this up: it installs and launches, Vulkan initialises on the device GPU,
  the surface and framebuffer come up (2351x1056), texture replacements load,
  and then it exits. The reason was a remembered `content://` disc URI that no
  longer opens — and because that is exactly what the port remembers, every
  later launch fails the same way. The fix is to ask again when a remembered
  `content://` URI fails to open; it is not applied because it cannot be
  verified without a device, and on-device testing is out of scope for now.


## wss:// proven by a handshake on Windows, not just a configure line (2026-09-26)

- The TLS end-to-end test skipped itself on Windows, so `wss://` there was a
  configure line saying OpenSSL 3.6.4 was found. It now runs there, ported
  rather than duplicated: a small platform shim covers the four POSIX-only
  spots (shell quoting for cmd.exe, a temp directory made from
  temp_directory_path, a Server object that is fork/exec on POSIX and
  CreateProcessA on Windows, and WSAStartup/closesocket for the free-port
  probe). FreeLoopbackPort deliberately never calls WSACleanup, matching
  `PortWs::EnsureWinsock` — a cleanup between the probe and the client connect
  would drop the reference count to zero.
- Three failures surfaced only by actually running it, all of them real:
  - `CreateProcessA` does not interpret `>`, so the server's log redirection
    was passed to python as arguments and it exited before binding. The log is
    now a CreateFileA handle passed through STARTF_USESTDHANDLES.
  - The "certificate for another host" case connected to `localhost`, which on
    Windows can resolve to `::1` where this IPv4 server is not listening, so it
    timed out instead of reaching the certificate check. It connects to
    127.0.0.2 with the server bound to all IPv4 interfaces: still loopback
    everywhere, still not in the certificate, and nothing depends on how a
    machine resolves a name.
  - The job then went green once and failed on the next run with "TLS handshake
    timed out" while all four rejection cases worked seconds later — the server
    was still starting. The test now polls a plain TCP connect for up to thirty
    seconds before the first handshake, and prints the server's log at the point
    of failure rather than only at the end.
- Evidence, in the CI log rather than inferred: Linux and Windows both complete
  a verified handshake and all four rejections, and both jobs now run
  `port_ws_tests` directly because ctest only prints a failing test's output.
  `wss://` on Android is untouched and still needs a TLS backend.


## Windows and Linux CI green over the randomizer and Archipelago work (2026-09-25)

- All 28 commits of randomizer and Archipelago work had never been through CI,
  because they sat unpushed. The first Windows run over them failed in five
  places, all of them things only clang-cl sees:
  - `platform/port_ws.cpp` includes winsock2.h, whose windows.h `min`/`max`
    macros break every `std::max` in the file. `NOMINMAX` now goes in first.
  - `tests/port_randomizer.cpp` and `tests/port_apclient.cpp` used `<unistd.h>`,
    `fork` and `waitpid`. The randomizer's four cases that need an unloaded
    process now re-run the test binary with the case name and directory as
    arguments (fork/execv, CreateProcessA), and both files' pid and environment
    helpers are spelled per platform.
  - The self-spawn built its Windows command line by wrapping narrow pointers in
    `std::wstring`, which has no such constructor; and `argv[0]` is whatever
    ctest used, which need not be a path the process can spawn itself by, so the
    executable's own path is read with `GetModuleFileNameA` on Windows.
  - The test then died with `0xc0000409` and no output. Phase markers named the
    phase, and the bytes named the cause: the test read the child's dump log
    with `std::ios::binary` while the port appends through a text-mode
    `ofstream`, so on Windows the file ended CRLF and no retry could match a
    line ending in a bare LF. That was a bug in the test, not the port; the
    markers, exit-code reporting and byte dump stay because they are what turned
    an unexplained fast-fail into that one line.
- Windows is now green: build, `ctest -L port` 13/13, the FIFO regressions, the
  packaged startup check and the artifact upload. `native-linux` is green on the
  same commit, and the configure step reports **OpenSSL 3.6.4 found: wss://
  enabled** on Windows, which is the first evidence that `wss://` is available
  there rather than merely possible. The TLS end-to-end test still skips itself
  on Windows, so that is a configure-level fact and not yet a handshake.


## Archipelago state belongs to a session, not to a file (2026-09-25)

- The recorded checks and item index were only invalidated by a slot change, so a
  state file left over from a different multiworld was silently replayed: the
  client claimed locations the save never collected and skipped the items the
  server still owed it. `archipelago_state.json` now records the seed name from
  the server's `RoomInfo`; a different seed discards the progress, logs why, and
  continues from empty. A file with no seed in it (written before this) adopts
  the server's and keeps its progress, so no one loses a session to the upgrade.
- Verified on disc against `tools/ap_fake_server.py`: a state file recorded
  against "MP Seed Alpha" connecting to the fake seed is discarded, the file is
  rewritten empty with the new seed, and the server sees no `LocationChecks` for
  the stale location; the same file with the server's own seed keeps its
  progress and re-sends the check on connect. Eight new assertions in
  `port_apclient_tests` cover both paths, the round trip and the legacy file.
- `MP_AP_RESET_STATE=1` discards the state before connecting. A new game or an
  older save on the same slot *and* seed is indistinguishable from continued
  progress, so this stays the manual way out; the memory card exposes no play
  time or save identity to detect it with.

## Impact Crater sweeps clean; the Wayland hang is not the port (2026-09-25)

- Impact Crater (`C13B09D1`) is no longer the sweep's stopping point. It reaches
  all 12 of its areas (`[sweep] complete: 1 worlds, 12 areas`) and dumps 85
  pickups, all `capacity=0` drops, so it contributes 0 item locations — which
  agrees with retail, where the crater holds no items. Every world is now
  covered, and one clean tour of all eight (`complete: 8 worlds, 276 areas`,
  ~12 minutes) yields the whole table: 2542 LOC lines, 1333 distinct pickup
  keys across 180 areas, and exactly **100 capacity-granting item locations** —
  50 missile expansions, 14 energy tanks, 5 power bomb expansions and the rest of
  retail's set, which is the count vanilla Prime has. `tools/rando_seed.py`
  turns that single dump into a seed with all 100 locations and 33 models.
  The earlier "4969 lines / 328 locations" figures came from merging ten partial
  runs and counted the same locations repeatedly; one tour needs no merging.
- A tour that long ends in `vkAllocateMemory failed with
  VK_ERROR_OUT_OF_DEVICE_MEMORY` while the frontend loads its first texture, and
  the process aborts. The dump is already complete when that happens, so it is
  cosmetic for the sweep, but a full eight-world tour is a ~37000-frame session
  and VRAM is not returned as fast as it is taken.
- The "unreproduced free" that ended earlier crater sweeps was never in the port.
  The run had stopped making progress because SDL3 was using its Wayland backend,
  where `SDL_ShowWindow` dispatches into libdecor's client-side decorations and
  never returns; the process sat at 100% with no frames and no log output past
  `Using surface format`. A backtrace of a hung instance pinned it, and
  `SDL_VIDEODRIVER=x11` runs normally (3481 frames in 60 s). Documented under
  "A Wayland session hangs at startup" in `docs/NATIVE_PORT.md`; no code change.
- Consequence for tooling: the sweep recipe needs `DISPLAY`, `XAUTHORITY` and
  `SDL_VIDEODRIVER=x11` on a GNOME Wayland session; unattended runs that set only
  the first two hang silently instead of failing.

## World restart crash, full location sweep, audio bug (2026-09-25)

- Restarting the current world used to crash after about ten restarts: the
  retiring mapper kept updating after its PAK's resource table was retired and
  locked a new STRG against it, dereferencing a null `CDvdFile`. `CStateManager`
  now stops the outgoing simulation as soon as a restart is requested and
  `CAutoMapper::Update` returns while `GetWantsToQuit()`, so no scripts or loads
  run against the retired world. Fifteen consecutive restarts are clean under
  ASan (`MP_SMOKE_WORLD_RESTARTS`, `MP_SMOKE_WORLD_TICKS`).
- `MP_RANDO_SWEEP=1` (+ `MP_RANDO_DUMP=1`) walks all eight worlds and their
  areas in one run (~12 minutes) and now produces a full dump in a single pass —
  see the section above for the counts and what they mean. `tools/make_ap_config.py
  --strict` maps a matching AP table with 100/100 entries and 0 areas for review,
  and the seed tool derives 33 item models from it.
- Two port-side fixes were needed in vendored code along the way:
  - `extern/aurora/lib/gx/shader.cpp`: a texcoord generator whose source
    register is still `GX_MAX_TEXGENSRC` ("no source", what the game leaves
    behind for a disabled texcoord) aborted the process. It now emits a constant
    texcoord, matching the existing skip in the shader dump.
  - `SStreamInfo::x0_fileName` and `CDSPStream::x10_fileName` were non-owning
    pointers into a `CDSPStreamManager` that is reassigned and freed while the
    stream is live, so `activate ok file=...` printed garbage and a long sweep
    died with `free(): invalid size`. Both are now owned `rstl::string`s.
    The sweeps that still ended in Impact Crater were not failing at all; see
    the section above.

## Archipelago progressive items (2026-09-25)

- The AP world's progressive beam items work: a config entry can carry
  `progressive`, a list of grants applied in order as copies arrive, and the
  count lives in `archipelago_state.json` so a reconnect resumes at the right
  step (verified on disc: two copies of id 5031043 recorded
  `"progressive":{"5031043":2}`). `tools/make_ap_config.py` emits the four beam
  sequences and maps the tracker-only ids 47-50 to the Charge Beam.
- The HUD notification names the resolved step ("Charge Beam"), not the item id,
  so a progressive grant still reads sensibly.
- Note for future edits to `tests/port_apclient.cpp`: JSON fixtures that contain
  `)"` (for example `"Charge Beam (Power)"`) must use a delimited raw string
  (`R"json(...)json"`), otherwise the fixture ends early and the file stops
  compiling.

## Archipelago over TLS (2026-09-25)

- `wss://` works through OpenSSL, enabled by CMake when it finds it (`OpenSSL
  found: wss:// enabled`) and optional otherwise: Android refuses `wss://` with
  a clear error instead of downgrading to plaintext. Verification is always on
  (system trust store or the config's `tls_ca`, TLS 1.2 floor, host-name check);
  there is deliberately no insecure switch.
- `port_ws_tests` runs a real TLS handshake against `tools/ap_fake_server.py
  --tls` with a generated CA and asserts four rejections (wrong CA, host-name
  mismatch, system store only, missing CA file); with the game, a `wss://`
  config connected and granted items, and a missing CA was refused.
- Worth knowing: OpenSSL writes with `write()`, which raises SIGPIPE when the
  server has gone, so the TLS calls block it on the calling thread and swallow
  one that arrived, rather than letting a dead server kill the process.

## Archipelago generator, status and notifications (2026-09-24)

- `tools/make_ap_config.py` joins the AP world's location table (parsed with
  `ast`) to a port location dump and writes `archipelago.json` plus, from the
  spoiler, a `randomizer_seed.json` whose remote-item locations become
  zero-amount placeholders (the real item arrives over the network). Locations
  are joined by the entity id's area index; the report prints the per-area id
  delta, `--strict` refuses mixed-delta areas, and unpaired areas are reported
  rather than guessed. Verified against the real AP data shape: Chozo Ruins
  Main Plaza flagged for review (`+1,+1,+1,+3`), Ruined Fountain clean at `+1`.
- Received items and server messages now show as HUD memos, one every two
  seconds, using the config's optional per-item `display` name; `PortAp` also
  exposes the seed name and a notification queue for the F1 overlay's Session
  tab (connection, seed, items, checks, last message).
- Observation: the port's screenshot capture does not include Aurora's ImGui
  layer — a front-end run with `MP_SHOW_DEBUG_UI=1` produced a screenshot with
  no overlay, as every checked-in screenshot also has none — so the overlay
  cannot be verified from a capture. The HUD (game-drawn) notifications are
  captured normally.

## Archipelago client (2026-09-24)

- The port can join an Archipelago multiworld natively: a background thread owns
  a minimal RFC 6455 WebSocket client (`platform/port_ws.*`) and the AP JSON
  protocol (`platform/port_ap_protocol.*`); the game reports collected pickups
  from `CScriptPickup::Touch` (`PortAp::QueueCheck`) and receives items in
  `PortAp::Poll`, called from `CStateManager::Update` next to the other port
  hooks. `CPlayerState::InitializePowerUp`/`IncrPickUp` are the grant calls, the
  same ones the retail pickup makes.
- Verified end to end against `tools/ap_fake_server.py` (a dependency-free
  WebSocket AP server): handshake and Connect, items granted into the player
  state (the HUD missile readout went from 15 to the granted 250),
  `LocationChecks` sent, and `archipelago_state.json` remembering the processed
  item index so a reconnect does not re-grant. See `docs/ARCHIPELAGO.md`.
- Only plain `ws://` (no TLS) and no per-message compression, which Archipelago
  marks deprecated; DeathLink, hints, chat, an in-game status line and a
  generator for the location/item id maps are still missing. `PortWs` was
  written because the server speaks WebSocket, not raw TCP, and no WebSocket
  dependency is vendored.
- Follow-up: `platform/port_json.*` is now a general JSON parser while
  `platform/port_randomizer.cpp` still carries its own seed parser. Folding the
  seed reader onto `PortJson` would leave one parser in the port.

## Randomizer: pickup models and drop filtering (2026-09-24)

- Rewritten pickups now draw the item they grant. `tools/rando_seed.py` derives
  a `models` map from the dump and the seed's `locations`; `LoadPickup` copies
  the entry into the pickup's static model and animation parameters exactly as
  the area data does. Verified on disc: an energy tank swapped into a missile
  location resolved to `model=86908399 acs=F37BCBC7`, both `ANCS` assets.
- Real item pickups are animated (`ANCS`) in the retail data; `model` and `acs`
  are both set and the engine prefers the animation, so the rewrite mirrors
  both fields instead of picking one.
- Areas also list their enemy drop templates as pickups (health and ammo
  refills with `capacity=0`). They are not item locations; the seed tool filters
  them by default (`--include-drops` keeps them). Confirmed against Chozo Ruins
  and Tallon Overworld dumps.
- Item models come from a dump, so a full dump gives full model coverage; an
  item the dump never saw keeps its retail model. See `docs/RANDOMIZER.md`.

## Item randomizer proof of concept (2026-09-24)

- Item pickups can be rewritten from a seed file at load time. The gameplay
  hooks are in `ScriptLoader::LoadPickup` (rewrite the item/capacity/amount
  before `CScriptPickup` is constructed) and `CScriptPickup::Touch` (record the
  check); the port layer is `platform/port_randomizer.cpp` with
  `MP_RANDO_DUMP` / `MP_RANDO_SEED`. Nothing changes when no seed is set.
- Verified on a real USA v1.00 disc: dump mode logged a live pickup
  (`LOC 39F2DE28:B2701146:0000007E Missiles amount=5 capacity=5`, Tallon
  Overworld landing site) and a seed moved an item to that location
  (`PLACE ... Missiles -> EnergyTanks amount=1 capacity=100`). See
  `docs/RANDOMIZER.md` for the format, the `tools/rando_seed.py` helper, and
  what is still missing (placement logic, pickup models, non-pickup item
  grants, check persistence, the Archipelago transport).
- Observation, not from this feature: restarting the world repeatedly (the F1
  overlay's world teleport path) crashed the ASan build after about ten
  restarts, in `CDvdFile::IsARAMFileLoaded` under `CAutoMapper::Update ->
  CStringTable::Lock`. Worth reproducing on a normal build before trusting the
  repeated world-teleport path.

## Remote-test regressions (2026-09-19)

- A CachyOS/Radeon 8060S crash in `CCubeMaterial::GetFlags` during door opening
  was reproduced locally by evicting/restoring area 0 geometry. Surface headers
  were converted from big endian again after an ARAM round trip. Conversion is
  now tracked for the lifetime of the area payload, independently of rebuilding
  its model instances. Three repeated eviction/restoration cycles pass under ASan.
- The old tick-remainder heuristic could produce only 45 simulation ticks/second
  at 60 rendered FPS with ±5 us jitter. A double-precision fixed-step accumulator
  now preserves fractional time and bounded scheduling debt. Runtime logging
  measures about 60 render FPS / 60 simulation TPS when capped, and 60 TPS when
  uncapped. F1 shows measured rates separately from the target setting.
- Free mouse movement now translates A/D instead of using console turning torque
  which mouse look immediately overwrote. Input is normalized, movement respects
  native physics, and mouse heading is applied before movement without consuming
  the same delta twice. The smoke driver verifies strafe before and after F1.
- MusyX ambient loops active in the first area included non-block-aligned loop
  starts (e.g. sample 832) and loop ends before the resource length. The software
  decoder now advances predictor history sample-by-sample, uses the actual loop
  end and loop context, and preserves streaming history across circular-buffer
  wraps. Signed PCM8 and wide intermediate Q15 products were corrected too.
  Golden-sample tests pass; confirmation of the audible static fix on the remote
  PulseAudio setup is still needed. `MP_AUDIO_STATS=1` reports mixer rate/clipping.
- Build revisions are generated at build time and exposed by `--version`, the
  launch log and F1. `MP_TRACE_TIMING=1` logs actual render and simulation rates.

Diagnostics and reproduction flags are documented in `docs/NATIVE_PORT.md`.
Validation: nine native checks pass in GCC and Clang/ASan builds. The combined
real-disc run passes three area reload cycles and the mouse scenario including
free strafe before/after F1, and exits cleanly. Steady capped timing reports
60 FPS / 60 TPS; uncapped presentation retains 60 TPS. Runtime leak detection
was disabled; allocation mismatch and invalid-access detection were enabled.

## Mouse aim / arm-cannon integration (2026-09-19)

- Mouse mode now uses immediate pitch/yaw rather than the retail 60°/s camera
  easing. Pitch is limited to ±1.52 radians, yaw is normalized, and independent
  X/Y inversion controls are available (normal, non-inverted Y by default).
- Playable first-person state owns mouse aim. Lock-on tracks the effective camera
  and discards hidden deltas; cinematics, scripted input locks, morph ball and
  menus relinquish ownership and rebase on return. Jump/fall camera pitching no
  longer replaces mouse pitch. The GC crosshair is independent of holding R.
- LMB drives fire/charge/release, RMB drives lock-on, and MMB drives missiles via
  the ordinary PAD input path. Existing mappings remain available. UI/focus
  changes cancel charging and require a neutral mouse-button state on recapture.
- Uncapped free mouse aim uses current rotation with interpolated translation.
  Held weapon geometry and muzzle effects use a simulation-time view, restoring
  the world view afterwards; the old assertion below about automatic viewmodel
  anchoring has been corrected.
- The real-disc mouse smoke scenario verifies firing, charged shots, missiles,
  lock-on release, jump/morph handoffs, crosshair state, and uncapped cannon/view
  alignment. It also exposed and fixed ImGui cloned draw-list allocator mismatch
  and a deferred MusyX stream stop that could read a freed host stream buffer.

See `docs/NATIVE_PORT.md` for controls, opt-outs, and the opt-in smoke driver.

Validation: GCC and Clang/ASan builds plus all six native regression executables
pass. The real-disc mouse run produced normal/charged/missile shots, observed live
projectiles, held lock-on for 60 ticks, checked 947 weapon views, and completed
jump, cinematic interruption, UI cancellation and morph/unmorph handoffs. A separate
1,800-frame mouse-disabled lifecycle run passed. Runtime ASan leak detection was
disabled; allocation mismatch and use-after-free checks remained enabled.

## Current native-port hardening (2026-09-19)

Current build/run instructions are in [docs/NATIVE_PORT.md](docs/NATIVE_PORT.md).
The dated sections below are a historical bring-up log, not a current blocker
list or reproducible build guide.

- Aurora/MusyX are now vendored source snapshots with exact provenance in
  `extern/README.md`. Clean clones do not depend on unpublished fork commits.
- The native game uses an OBJECT target and no duplicate-symbol suppression.
  Windows packaging includes runtime DLLs; Linux GCC/Clang and Windows clang-cl
  CI run asset-free regression tests.
- Native font resources are read from the validated user's DOL at startup.
  Generated asset arrays are no longer a native-build prerequisite.
- Host arrays and game-heap owners have explicit, different deleters. GUI,
  animation, collision, movie, resource and streaming owners have been corrected.
- MusyX addresses stay pointer-sized on Windows; group resources are pinned only
  while pushed, voices are retired under the IRQ mutex, mute is atomic, and
  shutdown joins the mixer before freeing its state. Missing devices run silently.
- AI audio supports enable-after-disabled startup, callback self-unregistration,
  and explicit teardown. Failed frame acquisition skips rendering and frame-based
  retirement; hidden windows keep a bounded event pump.
- DVD/ARAM state changes share a mutex, failed reads terminate with diagnostics,
  and resource EOF, AGSC section bounds, PATH counts/indices/ranges/cycles and DMA
  ranges are checked. Empty retail PATH resources are supported.
- Mouse capture ignores overlay/unfocused motion; the cursor-warp workaround is
  removed. Slow frames use bounded fixed-step catch-up. Native reset avoids the
  console reboot/cancel-all path.
- Save bit fields are MSB-first; CRC writes are big-endian, with legacy native CRC
  reads accepted. Save compatibility still needs a directed retail round trip.
- CARD initialization supplies the four-byte game ID; optional callbacks are
  null-safe, unmount commits successfully, and CARD readiness handles absent
  channels. A filesystem regression covers format/create/write/status/rename,
  physical remount/read/delete with the game's null-callback usage.

Validation: GCC 15 and Clang 19 builds; asset-free CTest regressions; a real-disc
2,400-frame AddressSanitizer lifecycle run through hide/restore, audio re-enable,
uncapped rendering, restart, and clean exit, with allocation mismatch checking
enabled. Leak detection was disabled for that runtime run. Windows runtime and
full-game traversal remain separate validation tasks.
All 205 GPU-free FIFO/GX tests also pass under ThreadSanitizer. Full-game TSan
reaches reports in uninstrumented GLib/libdbus/nod startup paths first, so it has
not validated the full game/audio/DVD thread interaction.
The CARD filesystem regression passes with null callbacks and a physical
remount. An isolated real-disc profile produced `MetroidPrime A.gci` and was
loaded by a subsequent process. A clean source clone builds without a disc or
generated game headers; five native regression executables pass.

Goal: turn the Metroid Prime recompilation experiment into an actual, maintainable
PC port by building the PrimeDecomp matching decompilation against the Aurora
compatibility layer (MIT), instead of a static-recomp module on a Dolphin-derived
runtime.

## Architecture

- Game + engine: PrimeDecomp/prime `src/` + `include/` (real C++, targets
  `GM8E01_00`, the same disc we own).
- Platform/GPU/input/disc/saves/UI: `extern/aurora` (MIT). Aurora provides a
  drop-in `dolphin/*` SDK API plus an SDL3 + WebGPU(Dawn) app layer and a
  performant GX implementation.
- Assets: read from the user's own disc (`orig/GM8E01_00/`); nothing is
  distributed.
- Integration reference: Dusklight (Twilight Princess port) — same CMake shape
  (`extern/aurora`, decomp sources listed in a `files.cmake`, `aurora::*` libs).

## Status (2026-09-18)

- Repo cloned, branch `port`; `extern/musyx` and `extern/aurora` submodules present.
- Matching decomp builds and reproduces the retail DOL byte-for-byte:
  `build/GM8E01_00/main.dol` sha1 `949c5ed7368aef547e0b0db1c3678f466e2afbff`.
  Report: SDK 100%, Core Engine (Kyoto) 90.99%, Game 87.50% matched.
  `objdiff.json` is available for reference diffing.
- Aurora `examples/simple` builds on this machine (prebuilt Dawn for
  linux-x86_64 is fetched automatically by CMake).

### Port scaffold build status (2026-09-18)

`CMakeLists.txt` + `files.cmake` (632 sources) + `platform/{compat.h,main.cpp}` build
`mp_game` against Aurora. First full compile went 1269 -> 138 errors after:
- `platform/compat.h` (force-included) restoring the SDK `AUTO`/`AUTO_REF`/
  `AUTO_CONST_REF` macros Aurora omits;
- excluding `src/NESemu/modwrapper.cpp` (raw PowerPC asm) and
  `src/MetroidPrime/TypesMatch.cpp` (decomp-only type scaffolding);
- `-Wno-narrowing`.

Remaining 138 errors are a bounded compatibility-shim queue:
- GX declarations Aurora implements but does not declare in headers:
  `GXSetTexCopyDst/Src`, `GXPixModeSync`, `GXCopyTex`, `GXSetTevColor`,
  `GXInitLightPos/Attn`, `GXSetCullMode`; plus a `GXSetArray` signature
  difference (Aurora adds a 5th arg).
- Missing SDK headers: `dolphin/arq.h`, `dolphin/thp/THPInfo.h`.
- `OSContext` member mismatches (`gpr`, `srr0`) in the game's own view.
- `COBBTree::CNode::operator new` placement-new mismatch.

### Build clears (2026-09-18)

`metroid_prime_port` now **compiles and links**: all 632 decomp sources build
against Aurora with zero errors and produce a 64.5 MB executable that
initializes Aurora and a Vulkan device.

What clearing the remaining errors required:
- `platform/compat.h` (force-included): libc, the GX/SI/PAD/OS/CARD umbrellas,
  `AUTO*`, `nofralloc`, `__abs`, and the `triggerL/R` -> `triggerLeft/Right`
  mapping for Aurora's `TARGET_PC` `PADStatus`.
- `platform/include/dolphin/{arq,gba,PPCArch,thp/*}.h`: SDK headers Aurora omits
  (arq.h re-exports Aurora's `ar.h`; the rest are the original SDK declarations).
- `platform/include/dolphin/gx/GXShims.h` + `platform/shims.cpp`: GX token,
  breakpoint, and write-gather-pipe entry points Aurora lacks, plus GBA/PPC
  shims.
- Decomp source fixes (port-only, documented in code comments): `RAssertDolphin`
  OSContext dump guarded for opaque PC `OSContext`; `rstl/string.hpp` declares
  the member specializations `rstl_strings.cpp` defines (clang requires
  declaration before instantiation); `GXSetArray` call updated to Aurora's
  5-arg form; small conversion casts; `CARDFormatAsync` declaration only (Aurora
  defines it); case-corrected `Kyoto/CCrc32.hpp` include.
- CMake: `LINK_GROUP:RESCAN` around `aurora::core`/`aurora::gx` to break their
  static-library cycle; excluded `src/NESemu` (raw PowerPC asm).

### Boot progress (2026-09-18)

`metroid_prime_port` now boots well into initialization under Aurora:
1. Aurora initializes (Vulkan device, 2240x1680 framebuffer, CARD, ARAM `0x1000000`).
2. Disc mounts via `aurora_dvd_open` (user's ISO).
3. Game entry runs: `CMain` ctor, `RsMain`, `CGameGlobalObjects` ctor, default font
   (zlib) load, `InitializeSubsystems` (AR/ARQ), `PostInitialize`.
4. GX commands reach Aurora's FIFO worker and a frame is presented.

Issues fixed along the way:
- zlib ABI: the bundled zlib 1.1.3 declares a non-standard 3-arg `inflateInit2_`;
  retargeted to the standard 4-arg form so it matches the linked zlib-ng.
- `TOneStatic<T>` 1-arg `operator new` had no definition.
- The guest stack "paint" in `InitializeSubsystems` wrote to address 0 (no emulated
  guest stack); skipped it on PC and gave the dummy `OSThread` a MEM1 stack range.
- Aurora aborted when the game's error handler called `PADRead` before `PADInit`.

Current blocker: the game's custom `CGameAllocator` (main heap) returns null for a
64 KB allocation in `CDvdFile::StartARAMFileLoad` (loading `aram:Tweaks.pak`),
which triggers the error handler (and then Aurora's `PADRead before PADInit`
fatal). Two separate issues:

1. **Heap sizing (fixed).** Aurora's internal framebuffer defaulted to the
   window-scaled 2240x1680, so the game's two `x2c_frameBufferSize` allocations
   took ~15 MB of the 24 MB MEM1, leaving only a ~10 MB game heap. Pinning
   `windowWidth/Height` to 640x480 raised the heap to ~20 MB. (Aurora still
   scales the internal fb to 1120x840 @1.75; a true 640x480 fb needs the DPI
   scale forced to 1.)
2. **Allocator free-list (open).** With ~20 MB free the 64 KB allocation still
   fails, so `CGameAllocator`'s free-block/split bookkeeping is not surviving on
   the 64-bit host (`SGameMemInfo` packs flags in low pointer bits and uses
   `sizeof(SGameMemInfo)`, which is ~4x larger than on GameCube). Needs a focused
   pass over `FindFreeBlock`/`FixupAllocPtrs`/`AddFreeEntryToFreeList`.

Temporary memory diagnostics are in `CGameAllocator::Initialize` and
`COsContext::OpenWindow` (stderr prints of heap/arena/framebuffer sizes).

### Bring-up fixes after the allocator (2026-09-18)

The game now runs through `PostInitialize`/`AddPaksAndFactories` pak loading with
no crashes. Fixes landed:
- **`CGameAllocator` overflow** (root cause of the OOM): `Alloc(0x20)`/`Alloc(0x1c)`
  for `CSmallAllocPool`/`CMediumAllocPool` were 32-bit object sizes; on x86-64 the
  objects are larger and overflowed the next free block's header. Now use
  `sizeof(...)`. Free list stays healthy.
- **ARQ recursion**: Aurora's `ARQPostRequest` invoked the ARAM completion callback
  synchronously, so `CDvdFile::PingARAMTransfer <-> HandleARAMInterrupt` recursed to
  stack overflow. Callbacks are now queued and drained by an iterative `ARQPoll()`
  at explicit pump/wait points. Running the poll inside `ARQPostRequest` was still
  too early: multi-chunk transfers observed stale length/interrupt state and left
  `aram:MiscData.pak` permanently loading.
- **GX breakpoint / VI retrace**: `CGraphics::EndScene` spins on
  `mNumBreakpointsWaiting`, which only a VI retrace decrements. `GXEnableBreakPt`
  now pulses the registered breakpoint + pre/post retrace callbacks (stored by the
  VI shims) so frames complete.
- **`delete` on CMemory memory**: `rstl::aligned_allocator::deallocate` used
  `delete[]` while `allocate` used `CMemory::Alloc`; on clang the game's
  `operator delete`->`CMemory::Free` is MWCC-only, so glibc freed a game pointer.
  Routed the free to `CMemory::Free`; `CDvdFileARAM` buffers now use `rs_new`.

The apparent frame-slot deadlock was normal frame pacing. The repeated
`CGraphics::EndScene` caller was the initial pak-loading loop; after fixing ARQ
completion ordering and synchronous ARAM waits, startup reaches the main loop
(`MP frame` verified through frame 361). Resource buffers passed to `delete`-based
owners now use matching host allocations, and palette frame state is initialized
so ARAM palette storage follows delayed `CMemory::Free` cleanup.

Further host bring-up fixes now sustain the main loop through at least frame 48,601:
- GameCube AGSC payloads are big-endian 32-bit MusyX structures, while the vendored
  host runtime expects native-endian structures (including 64-bit sample directory
  pointers). `CAudioGrpSetLoc` now converts the pool, project, and sample directory
  into host-native layout on little-endian hosts: big-endian `GROUP_DATA`,
  `POOL_DATA`, `MEM_DATA`/`FX_TAB`, ID lists, and `SDIR_DATA_INTER` are byte-swapped
  and expanded to native `SDIR_DATA`; raw curve bodies and PCM samples are left
  untouched. MusyX group push/pop is enabled again and `sndPushGroup` succeeds for
  the boot groups. A `s32`->`size_t` cast in `dataAddSampleReference` fixes truncation
  of 64-bit sample bases.
- CMDL header fields, section sizes, bounds, and per-surface metadata are byte-swapped
  before model setup. Surface parent/next links are expanded in place for 64-bit hosts
  while raw GX display lists remain untouched.
- Movie buffers owned by `single_ptr`/`auto_ptr` use matching host allocations and
  rounded DVD request sizes instead of placing `CMemory::Alloc` pointers behind
  host `delete` owners.
- Aurora keyboard input has initial GameCube mappings when no saved mapping exists:
  WASD and IJKL drive the sticks, X/Z/C/V map A/B/X/Y, Return maps Start, and the
  arrow keys map the D-pad. Existing user mappings and physical controllers win.
- PATH version-4 resources are read field-by-field into native vectors. Packed
  big-endian node, link, region, connectivity, and octree fields are converted and
  their 32-bit indices are rebased only after vector storage is stable.
- CMDL and MREA geometry now share native surface-header conversion, including
  material indices, display-list lengths, pointer-sized renderer links, normals,
  and optional bounds.
- MusyX now uses the upstream `origin/sdl3` PC backend (merged into the vendored
  submodule, with conflicts resolved in favor of the host `s64` typedef and the SDL
  mutex IRQ). It provides a software voice mixer covering ADPCM/PCM decode, pitch
  resampling, ADSR envelopes, and studio/AUX mixing, and feeds interleaved `s16`
  stereo to an SDL3 audio stream. PC sequence playback is enabled again and the
  audio thread drives `snd_handle_irq`.
- Big-endian `ARR` song payloads from `CSNG` resources are converted in place for
  little-endian hosts before sequencing: header offsets, the 64-entry track table,
  per-track `TENTRY` arrays, the `MTRACK` tempo list, the pattern table, pattern
  headers, and `NOTE_DATA` note streams are byte-swapped. Byte-oriented pitch-bend
  and modulation streams need no conversion.
- Native text rendering checks explicit string lengths before dereferencing the
  next character. Palette entries, MREA section buffers, and map buffers now return
  to the allocator that created them during runtime and delayed shutdown cleanup.
- AGSC group buffers are retained for the session on PC. `hwSaveSample` never copies
  samples into ARAM here, and the MusyX 2.0.0 `sndPopGroup` path can leave voices
  referencing sample data after a group is popped, so freeing the buffer left the
  audio thread reading unmapped memory (confirmed with AddressSanitizer).
- `CCameraFilterPass::DrawRandomStatic` previously faked a random main-memory
  address as its texture source (the GameCube renderer ignored the pointer, the PC
  renderer hashes it). It now samples a real scratch buffer of noise, sized for the
  tiled IA4 extent.
- DVD ARAM streaming state is serialized with a recursive mutex: `OSDisableInterrupts`
  is a no-op on PC, and the DVD worker and main threads raced on the transfer
  counters until `mBufferLen` went negative and `ARQPostRequest` memcpy'd a huge
  length. Non-positive transfer lengths are also treated as complete.
- Vertex array byte sizes are threaded through to `GXSetArray`. Aurora uploads
  `size` bytes of each attribute array, and the port was passing 0, so every
  array-based draw (MREA world geometry, CMDL models, skinned models) uploaded
  zero bytes and rendered nothing while immediate-mode effects and the HUD still
  drew. Positions, normals, colors, and UVs now carry the section sizes from the
  MREA/CMDL loaders.
- The PC MusyX mixer render thread is paced to real time. It previously free-ran
  (measured ~29x real time) because it only throttled on the SDL queue depth,
  overrunning the stream and producing dropouts; it now sleeps until the next
  160-sample frame is due, which removed the buffer-boundary discontinuities.
- The AGSC sample directory's trailing ADPCM info blocks are now preserved and
  converted for little-endian hosts, and each entry's `extraData` offset is rebased
  onto the native `SDIR_DATA` array (whose entries are larger than the disc's
  32-bit form). Without this, in-level voices decoded with garbage coefficients and
  produced noise.
- The disc sample directory ends with a 4-byte `0xFFFFFFFF` terminator rather than
  a full entry, so the ADPCM info blocks start at `(count - 1) * entrySize + 4`
  and not `count * entrySize`. The old base was 28 bytes too high, which skipped
  the first block and left samples whose information begins there reading
  coefficients from the entry table; the charge-beam looping layer (id 209) was
  the audible case and buzzed continuously. `MP_VALIDATE_SAMPLES=1` scans every
  loaded sample directory and reports any ADPCM sample whose rebased `extraData`
  is missing or does not hold `numCoef == 8` (verified clean across the front end
  and first areas).
- In-game streamed music now plays. The PC MusyX ARAM layer was entirely stubbed
  (`aramAllocateStreamBuffer` returned 0, `aramGetStreamBufferAddress` returned
  NULL, `aramUploadData` did nothing), so streamed voices got a null sample address
  and the software mixer skipped them; the stream never advanced and
  `UpdateStream` was never called. Each ARAM stream buffer is now backed by host
  memory and `hwFlushStream` keeps the full 64-bit host pointer instead of
  truncating it to `u32` (which faulted on the first upload).
- Streamed audio (front-end music) now decodes correctly: `DecodeMonoAndMix` wrote
  the decoded samples *on top of* the buffer still being played, feeding the
  output back into itself until it saturated into full-scale noise. It writes the
  decoded samples now, `IsReady` waits for every chunk rather than only the last,
  and the non-ARAM `CDvdFile` read is blocking (`DVDReadPrio`) so playback cannot
  start on unfilled buffers.
- `CARAMToken::UpdateAllDMAs` pumps `ARQPoll` before refreshing status. Aurora
  defers ARQ completion callbacks until `ARQPoll` runs on the main thread, but
  the map/pause texture eviction and room-transition code spin on a token
  (`while (texture.IsARAMTransferInProgress()) UpdateAllDMAs();`) and so never
  reached the main-loop poll; the DMA never completed and the game froze with
  audio still playing (opening the map always hung).
- The GameCube audio-interface DMA path is implemented in `platform/ai_dma.cpp`:
  the registered DMA callback is driven from the main loop (`AIPortPoll`) at the
  buffer rate and the submitted buffer is fed to its own SDL stream. Streamed
  audio (front-end/in-game music via `CStaticAudioPlayer`, movie audio)
  previously had no output at all because the AI functions were no-ops. It runs on
  the main thread because the guest mixer is not thread-safe, and the port keeps
  the full 64-bit DMA pointer that the SDK's 32-bit `AIGetDMAStartAddr` truncates.
  `CMoviePlayer::StaticMyAudioCallback` reads the previous DMA buffer through the
  same 64-bit accessor; the truncated form resolved to unrelated memory whose
  bytes were then mixed as audio. `MP_DISABLE_AI_AUDIO=1` isolates this path.
- Streamed in-game music (`Audio/*.dsp` software streams) was silent on Android
  only. ARM compilers default plain `char` to unsigned, so the `-1` companion
  sentinels in `CDSPStreamManager`'s `char` fields read back as 255: the
  header-read completion then took the companion path with a 255 index, read
  `g_Streams[255]` out of bounds and discarded the stream, so no streamed voice
  was ever created and all streamed audio (cutscene and area music) went quiet
  while MusyX sound effects kept working. CMake now passes `-fsigned-char` to the
  game, port and MusyX targets (`mp_signed_char`), matching the x86/Windows
  behaviour the port is verified against, and `platform/compat.h` asserts the
  invariant so losing the flag fails the build instead of muting the music.
- Streamed music stopped refilling part-way through a track and looped the few
  seconds it already had. `CDSPStream::BufferStream` started the async disc read
  before publishing either `xec_readsPending` or the destination-half selector,
  and the completion runs on Aurora's DVD worker thread: a fast completion
  decremented the count before this call assigned it, wrapping the uchar to 255,
  after which every refill saw a read outstanding, `UpdateStream` returned 0
  forever and the mixer looped its buffer. The guest serialized this with
  `OSDisableInterrupts`, a no-op on PC, so both values are published before the
  read starts, and a completion for a read the stream no longer owns is dropped
  instead of counted down. Verified on device: the 99-second intro track now
  plays to its end (`end of stream`) instead of freezing at half the file. The
  tracing is in the `mpstream`/`mpstream-mx` logcat tags, with the per-chunk
  detail behind `MP_STREAM_TRACE=1`.
- Android touch sticks and triggers reached the game wrong in three ways. The
  overlay sends normalised -1..1 deflections but `SDL_SetJoystickVirtualAxis`
  takes a Sint16 joystick value, so partial deflection truncated to 0 and the
  player could neither move nor aim; the Y axis was negated (SDL's gamepad +Y is
  down, which Aurora inverts for the GameCube stick); and a released trigger sent
  0, which is the axis *centre* rather than a trigger's resting minimum, so the
  game stayed locked on and strafing after the player let go. Axes now map the
  full -32768..32767 range and triggers release at the minimum.
- Controls still held when the debug overlay opened were never released: the
  overlay stops claiming touches as soon as it is visible, so the matching
  releases never arrived and whatever was down stayed down for the session. Held
  controls are released on that transition.
- Skinned vertex generation advances its output cursor explicitly. On the console
  the write-gather pipe advances itself as data is written, so `BuildPoints`,
  `BuildNormals`, and `Calculate`'s padding pass all reused one `pipe` value; on PC
  that made every bone overwrite the same offset and left the rest of the
  workspace uninitialised (vertex explosions). The vertex/normal workspaces are
  also one contiguous allocation, matching the points-then-normals write order.
- Runtime-generated vertex arrays are marked host-native (`le=true`) instead of
  big-endian, matching the port's native skinning/workspace writes. `ClearArray`
  forces the backend to drop its cached copy, which the skinned path needs because
  the workspace pointer is reused every frame. Map-screen mappable-object and area
  arrays are also host-native and provide their real byte sizes.
- `GXSetDrawSync`/`GXReadDrawSync` are real FIFO-ordered tokens in Aurora rather
  than a shim that echoed the last token. The skinned-model circular workspace
  frees a buffer once its token is readable, so an echoed token let the game reuse
  a workspace before Aurora's FIFO thread had copied its vertices, corrupting
  intermittent draws (the reported geometry explosion). The token command is
  processed by the FIFO worker in the same order as the draw that references the
  data, so the fence now holds.
- `F12` asynchronously reads back the resolved EFB and saves a 640x480 BMP under
  `screenshots/`. This avoids compositor-dependent tools and provides captures for
  diagnosing rendering regressions.

Host shutdown now completes cleanly. Game-heap buffers owned by `CGBASupport`,
`CStaticAudioPlayer`, and `SMediumAllocPuddle` are released through `CMemory` instead
of host `delete`, and the PC build skips the guest-stack usage scan that it does not
initialize.

## Next steps

Automated PAD input advances through the front end, loads the first room,
constructs `CInGameGuiManager` and `CMFGame`, and runs beyond frame 48,000 with an
AddressSanitizer-clean run past frame 21,000 and a normal exit on forced SIGTERM.

Verified:
- World, actor, and skinned geometry render (array sizes and array endianness were
  the draw-stopping bugs); the front end, HUD, and combat visor draw correctly.
- Left-stick input moves the player; the walk stays grounded and is constrained by
  room collision, so input, physics, and collision are live.
- The player stays alive (`CPlayer::x9f4_deathTime` remains 0).
- Streamed (front-end) audio decodes to tonal PCM, and in-level MusyX frames are
  tonal rather than noise after the ADPCM info-block conversion.
- Both SDL audio paths maintain a bounded queue instead of depending on exact
  5 ms thread/main-loop scheduling. MusyX also reads raw PCM16 sample payloads as
  GameCube big-endian data instead of host-endian data.
- In-game `.dsp` stream headers are converted from GameCube endianness after the
  DVD read. Without this, the native sample-rate check rejected every stream;
  traced intro playback now allocates a stream at 32000 Hz.
- GPU captures from the current build show the publisher screen, `[ PRESS START ]`
  title screen, and no-memory-card dialog rendering correctly. The user's reported
  blank front end therefore needs a capture at the exact failing transition.

Remaining:
1. Directed input needed to reach and open a door; wandering for ~48k frames never
   triggered a second `CWorld::TravelToArea`, so room transitions are unverified.
2. Retest title music, spaceship music, and effects by ear after queue-depth and
   PCM16-endianness fixes; verify pitch/tempo and that spaceship music starts.
3. Confirm the draw-sync fence removed the intermittent skinned geometry
   explosion with an F12 capture at the failing frame if it still occurs.
4. Replace the session-long AGSC buffer retention with a bounded lifetime.
5. Verify CARD saves and the remaining menu flows.
6. Reproduce the reported blank front-end screen using `F12`; current automated
   captures do not reproduce it.

Wayland presentation keeps the game EFB locked to its configured 640x480 with
`VISetFrameBufferScale(1)`, while Aurora scales that image to the native high-DPI
swapchain. This prevents the title background from disappearing at fractional
display scales. Compositor vsync is disabled because it misses presentation
intervals around the game's own async-idle work; an absolute 60 Hz deadline is
used instead.

Debug shortcuts: `F10` toggles the 60 FPS deadline/unlimited mode; `F12` saves
the resolved framebuffer under `screenshots/`. Unlimited mode changes only the
presentation rate; simulation, input, SFX, and streamed audio remain on Prime's
fixed 60 Hz clock.

Debug env flags for fast iteration: `MP_FAST_BOOT=1` skips the pre-front-end and
drives the title through file select into a new game without input;
`MP_SKIP_CUTSCENES=1` skips cutscenes that set a cinematic skip object and
fast-forwards the ones that do not (the opening frigate sequence deliberately has
no skip object), so control is granted in roughly 15 seconds instead of minutes.

HD textures: `MP_TEXTURES=<dir>` loads replacements with Aurora's
`tex1_<w>x<h>[_m]_<texhash>[_<tluthash>]_<format>.dds|.png` convention (hash
fields may be `$` wildcards). The format may be Aurora's numeric GX format or
Dolphin's name (`CMPR`, `RGBA8`, `C8`, ...); Dolphin also hashes with
`XXH64(data, size, 0)` and uses the same `tex1_` layout, so Dolphin packs should
resolve once the hashed size and paletted tlut handling are confirmed against a
real pack.

Lock-on uses `CPlayer::WithinOrbitScreenBox`/`WithinOrbitScreenEllipse`, which
compare the target's live-viewport screen position against the player tweak's
fixed 640x480 coordinates. Those coordinates are now scaled by the viewport size;
otherwise the lock-on zone sits left of the reticle in widescreen and centred
targets are never acquired.

Aspect ratio is selectable via `MP_ASPECT=4:3|16:9|window` (or the debug
overlay's Render tab, applied live): 4:3 is the original 640x480, 16:9 is a fixed
854x480, and `window` tracks the window on `AURORA_WINDOW_RESIZED`. Each frame the
game recomputes the render-mode width (`CGraphics::PortResizeFrameBuffer`) and
refreshes `CCameraManager`'s cached aspect; Aurora derives the internal EFB from
the render mode, so the horizontal FOV, culling frustum, and present all widen
together. The port enables `AURORA_VIEWPORT_FIT`, so the EFB matches the selected
aspect and the present letterboxes rather than stretching when the window shape
differs. The HUD is anchored to the view edges and scales with it.

Mouse aim (`MP_MOUSE_AIM=1`, or the debug overlay's Input tab) integrates relative
motion into world yaw/pitch in playable first person. Its state handoff, camera
response, buttons and crosshair are described in the current mouse section above.
`MP_MOUSE_SENS` sets radians per pixel (default 0.0035).

The former Wayland cursor-warp workaround and large-delta rejection were removed
during native hardening. SDL relative capture now owns cursor locking; focus/UI
changes clear pending motion instead of guessing which deltas came from a warp.

`F1` toggles an in-game debug overlay (Aurora's ImGui) with sections for
Performance (frame limiter, FPS), Cutscenes (skip and speed), Render (vsync and
internal EFB scale), Audio (mute the streamed/AI path or MusyX independently),
and Session (restart to menu, screenshot). The same settings are read from
`MP_FAST_BOOT`, `MP_SKIP_CUTSCENES`, `MP_CUTSCENE_SPEED`, and
`MP_SHOW_DEBUG_UI` at startup, and the overlay writes them live. `MP_DISABLE_AI_AUDIO`
still exists for isolating streamed audio at startup.

Unlimited presentation interpolates the active world camera between simulation
transforms. Free mouse look keeps current rotation so the reticle and shot agree;
translation remains interpolated. Camera switches, large translations and
non-mouse camera cuts reset interpolation. A world-space gun transform does not
automatically stay camera-relative when only the camera is interpolated: the
held-weapon render pass now uses its matching simulation view explicitly. Actor
and weapon animations still update at the fixed simulation rate.

`assets/initial_pipeline_cache.db` contains machine-independent Aurora pipeline
descriptions collected from the title, menus, and intro gameplay. CMake copies
it beside the executable; Aurora merges it into each user's persistent cache
and compiles the entries on its background pipeline thread.

## Licensing

- Aurora and the checked-in MusyX snapshot: MIT (see their `LICENSE` files).
  Port-specific code: ours. Preserve individual source notices as well.
- The decompiled game/engine source and the game assets remain Nintendo's; this
  is the usual decomp-port situation. Ship no assets; require the user's disc.
- The earlier recomp path (GPL: DolRecomp / ModernGekko) is preserved separately
  and is not linked into the port.

## Preserved recomp baseline

- git tag `recomp-baseline-2026-09-18` in the MetroidPrimeRecomp repo.
- Backup bundle and build artifacts under `/home/odran/backups/` (`mpr-*`).
