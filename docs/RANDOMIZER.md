# Item randomizer (proof of concept)

The port can rewrite item pickups at load time from a seed file and record what
it did. This is the runtime half of a randomizer: item *placement* is generated
offline (the existing Python randomizers own the logic), and the port applies it
and reports checks back. Nothing here changes behaviour when no seed is present.

## How it works

- **Locations** are keyed by `%08X:%08X:%08X` — world asset id (MLVL), area asset
  id (MREA), entity editor id. All three are stable for a given disc, so a seed
  generated for `GM8E01_00` (USA v1.00) applies to any run.
- `ScriptLoader::LoadPickup` calls `PortRandomizer::ApplyPickup` before the
  pickup is constructed; a seed entry rewrites the item type, amount and
  capacity. This is the only place `CScriptPickup` is constructed, so every
  placed pickup goes through it.
- `CScriptPickup::Touch` calls `PortRandomizer::RecordCheck` when a pickup is
  collected.
- The port layer is `platform/port_randomizer.cpp` + `platform/include/port_randomizer.h`.
  It reads `MP_USER_PATH` (else `SDL_GetPrefPath`) for its files, matching the
  settings file.

Items are named after `CPlayerState::EItemType` without the `kIT_` prefix:
`Missiles`, `EnergyTanks`, `MorphBall`, `PowerBombs`, `Newborn` (artifacts), and
so on. `ItemFromName` is case-insensitive; `tests/port_randomizer.cpp`
drift-checks the table against the real enum.

## Seed file

Path: `$MP_RANDO_SEED` if set, else `randomizer_seed.json` in the user
directory. Schema:

```json
{
  "seed": "poc-0001",
  "locations": {
    "39F2DE28:B2701146:0000007E": { "item": "EnergyTanks", "amount": 1, "capacity": 100 }
  },
  "models": {
    "EnergyTanks": { "model": "86908399", "acs": "F37BCBC7", "character": 0, "animation": 0 }
  }
}
```

`item` is required; `amount` and `capacity` are optional and leave the vanilla
values untouched when absent. Unknown keys are ignored. Arrays, `null`, comments
and trailing commas are rejected — a malformed seed disables the randomizer with
a `randomizer: seed parse error at byte offset N` line on stderr rather than
applying half of it.

`models` is optional and maps an item name to the assets a pickup of that item
is drawn with. A rewritten pickup uses that entry so it looks like the item it
grants; without one it keeps the retail model (the item is still correct). The
port copies the entry into the pickup's static model and animation parameters
exactly as the area data would, and the engine already prefers the animation
when one is present. In the retail data a pickup's `model` and `acs` are both
ANCS assets, with the animation driving the visual.

## Workflow

1. Dump every pickup location:

   ```sh
   MP_RANDO_DUMP=1 MP_RANDO_SWEEP=1 <game> <disc.iso>
   # writes <user dir>/randomizer_locations.log:
   #   LOC 39F2DE28:B2701146:0000007E Missiles amount=5 capacity=5 \
   #       model=FFFFFFFF acs=90C6A0EE character=0 animation=0
   ```

   `MP_RANDO_SWEEP=1` makes one run visit every world and area (a world restart
   per area, waiting for streaming to go idle) so a complete dump needs no
   playthrough. It takes about twelve minutes and logs `[sweep]` progress. Add
   `MP_RANDO_SWEEP_WORLDS=<hex>[,<hex>...]` to tour only some worlds (the ids
   are the `WORLD:` column of the dump); the tour logs `complete: N worlds, M
   areas` when it finishes.

   The run needs a display, and on a GNOME Wayland session the port now selects
   the X11 backend by itself (see `docs/NATIVE_PORT.md` for why, and for
   `SDL_VIDEODRIVER` to override the choice):

   ```sh
   DISPLAY=:0 XAUTHORITY=/run/user/1000/.mutter-Xwaylandauth.XXXXX \
   MP_RANDO_DUMP=1 MP_RANDO_SWEEP=1 <game> <disc.iso>
   ```

   A full tour writes 2542 LOC lines covering 1333 distinct pickup keys in 180
   areas, of which **100 are item locations** — the same count retail Prime has,
   and a seed built from that one dump covers all of them. Areas also list their
   enemy drop templates as pickups (health and ammo refills with `capacity=0`);
   those are not item locations and are filtered out below. All eight worlds
   sweep to completion, Impact Crater included: its 12 areas are reached
   (`complete: 1 worlds, 12 areas`) and dump 85 pickups, every one of them a
   `capacity=0` drop, so it contributes no item locations — which matches retail,
   where the crater holds no items.

2. Shuffle the dump into a seed:

   ```sh
   python3 tools/rando_seed.py --from-dump randomizer_locations.log \
       --out randomizer_seed.json --seed my-seed --shuffle-seed 1
   ```

   Locations not present in the seed keep their vanilla items, so a partial dump
   is a valid partial randomizer. `--from-dump` may be repeated to merge several
   dumps, and `--include-drops` keeps the zero-capacity drop templates as
   locations. The tool also derives the `models` map from the dump, so a full
   dump gives full model coverage.

3. Play with the seed. Applied rewrites are appended to
   `randomizer_placements.log` (once per location per session, with the model
   used) and collected checks to `randomizer_checks.log`.

The randomizer announces itself at startup (`randomizer: seed 'x', N placements,
M item models`, `randomizer: dump mode (MP_RANDO_DUMP)`) and is otherwise silent.

## Verified

- `port_randomizer_tests` covers the seed parser (valid, partial, malformed,
  model entries with and without assets), the location-key format, item-name
  mapping, model lookup and the no-seed no-op path.
- On a real USA v1.00 disc, dump mode logged live pickups with their models, and
  a seed both moved items and swapped their models: Tallon Overworld's landing
  site missile expansion became an energy tank
  (`PLACE 39F2DE28:B2701146:0000007E Missiles -> EnergyTanks amount=1 capacity=100`),
  and in Chozo Ruins the swapped pickups resolved to the right assets
  (`PLACE 83F6FF6F:D5CDB809:0002012C Missiles -> EnergyTanks ... model=86908399
  acs=F37BCBC7`, both `ANCS`).
- Dumps of the first areas of Tallon Overworld and Chozo Ruins produced 5 real
  Chozo item locations and 2 item models after filtering drops.

## Not done yet

- **Logic.** Placement logic belongs in an offline generator (randomprime /
  Archipelago's Metroid Prime world); the port only applies the result. Nothing
  in the port guarantees a seed is beatable.
- **Model coverage.** Models come from a dump, so an item the dump never saw
  keeps its retail model. There is no built-in table yet.
- **Coverage.** `CScriptPickup` is handled; `CScriptPickupGenerator` drops and
  script-granted items (artifacts via `CArtifactDoll`, suit upgrades) are not, so
  a full randomizer needs those locations too. Drop templates must stay out of
  the seed, which the tool does by default.
- **Checks in the save.** Checks are logged to a sidecar file and counted per
  session; they are not persisted into the save game yet.
- **Archipelago.** Implemented; see `docs/ARCHIPELAGO.md`. Checks are reported
  from `CScriptPickup::Touch` and items are granted by `PortAp::Poll` each
  simulation tick, over a native WebSocket client. The Metroid Prime world's
  item and location tables are built in, so a server address and slot name are
  enough; `tools/make_ap_config.py` remains for checking the location table
  against a pickup dump.
