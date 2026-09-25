# Spec-shaped Archipelago fixture

`Locations.py` here is a **stand-in** for the upstream Archipelago world's
file, not a copy of it. The world lives in a separate repository whose contents
have been unreachable, which left `tools/make_ap_config.py --strict` with
nothing to run against.

It is generated from a real port dump by `tools/make_ap_fixture.py`:

```sh
python3 tools/make_ap_fixture.py \
    --dump <a full randomizer_locations.log> \
    --out tools/ap-world-fixture
```

What is real and what is not:

- **Real**: the entity ids, the areas they live in, the item each location
  holds in the vanilla game, and the count - 100 locations, which is retail
  Prime's number. Every entity id comes from the port's own sweep.
- **Synthetic**: the AP location ids, which are assigned from
  `FIXTURE_LOCATION_BASE` (50310000) so one can never be mistaken for a real
  id, and the location names, which are derived from the dump rather than taken
  from the world.

It keeps the join, the generator and the config parse exercisable end to end. It
is not a substitute for the real file: replace it and re-run `--strict` when
the world's contents are reachable.
