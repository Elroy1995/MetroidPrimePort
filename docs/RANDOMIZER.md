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
  }
}
```

`item` is required; `amount` and `capacity` are optional and leave the vanilla
values untouched when absent. Unknown keys are ignored. Arrays, `null`, comments
and trailing commas are rejected — a malformed seed disables the randomizer with
a `randomizer: seed parse error at byte offset N` line on stderr rather than
applying half of it.

## Workflow

1. Dump every pickup location of a world:

   ```sh
   MP_RANDO_DUMP=1 <game> <disc.iso>
   # writes <user dir>/randomizer_locations.log:
   #   LOC 39F2DE28:B2701146:0000007E Missiles amount=5 capacity=5
   ```

2. Shuffle the dump into a seed:

   ```sh
   python3 tools/rando_seed.py --from-dump randomizer_locations.log \
       --out randomizer_seed.json --seed my-seed --shuffle-seed 1
   ```

   Locations not present in the seed keep their vanilla items, so a partial dump
   is a valid partial randomizer.

3. Play with the seed. Applied rewrites are appended to
   `randomizer_placements.log` (once per location per session) and collected
   checks to `randomizer_checks.log`.

The randomizer announces itself at startup (`randomizer: seed 'x', N placements`,
`randomizer: dump mode (MP_RANDO_DUMP)`) and is otherwise silent.

## Verified

- `port_randomizer_tests` covers the seed parser (valid, partial, malformed),
  the location-key format, item-name mapping and the no-seed no-op path.
- On a real USA v1.00 disc, dump mode logged a live pickup
  (`LOC 39F2DE28:B2701146:0000007E Missiles amount=5 capacity=5` — the Tallon
  Overworld landing site missile expansion), and a seed moved an item to that
  location (`PLACE 39F2DE28:B2701146:0000007E Missiles -> EnergyTanks
  amount=1 capacity=100`).

## Not done yet

- **Logic.** Placement logic belongs in an offline generator (randomprime /
  Archipelago's Metroid Prime world); the port only applies the result. Nothing
  in the port guarantees a seed is beatable.
- **Models.** `LoadPickup` takes the pickup's model from the area file, so a
  rewritten pickup still renders the original item's model. A
  `EItemType -> model/animation` table is needed alongside the rewrite.
- **Coverage.** `CScriptPickup` is handled; `CScriptPickupGenerator` drops and
  script-granted items (artifacts via `CArtifactDoll`, suit upgrades) are not, so
  a full randomizer needs those locations too.
- **Checks in the save.** Checks are logged to a sidecar file and counted per
  session; they are not persisted into the save game yet.
- **Archipelago.** A client needs a socket layer (the port has none) speaking
  AP's JSON protocol: send `LocationChecks`, grant `ReceivedItems` through
  `CPlayerState::InitializePowerUp`/`SetPickup`. The hooks above are the two
  contact points; the missing piece is transport.
