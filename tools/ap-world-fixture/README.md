# Spec-shaped Archipelago fixture

`Locations.py` here is a **stand-in** for the upstream Archipelago world's
file, not a copy of it. It exists so CI and offline use have something for
`tools/make_ap_config.py` to join against.

**Prefer the real file.** The world is at
`https://github.com/UltiNaruto/MetroidAPrime` (`MetroidAPPrime`, with two
"P"s, gets a GitHub 404 that reads like a private repository rather than a
typo). Clone it and use `src/Locations.py` directly; the join does not care
which table it is given.

```sh
git clone --depth 1 https://github.com/UltiNaruto/MetroidAPrime
python3 tools/make_ap_config.py \
    --locations MetroidAPrime/src/Locations.py \
    --dump randomizer_locations.log \
    --server wss://host:38281 --slot Player1 \
    --out archipelago.json --strict
```

This file is generated from a real port dump by `tools/make_ap_fixture.py`:

```sh
python3 tools/make_ap_fixture.py \
    --dump <a full randomizer_locations.log> \
    --out tools/ap-world-fixture
```

What is real and what is not:

- **Real**: the entity ids, the areas they live in, the item each location
  holds in the vanilla game, and the count — 100 locations, which is retail
  Prime's number, and also the real world's count.
- **Synthetic**: the AP location ids, assigned from `FIXTURE_LOCATION_BASE`
  (50310000) so one can never be mistaken for a real id, and the location
  names, which are derived from the dump rather than taken from the world.

`ctest -R port_ap_fixture` checks the fixture cannot drift from its generator
and that the join still maps it completely.
