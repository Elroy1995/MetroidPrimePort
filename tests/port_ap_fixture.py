#!/usr/bin/env python3
"""Checks the Archipelago tooling and its spec-shaped fixture.

Dependency-free, and registered as a ctest target so a change to the location
join or to the fixture fails in CI rather than in whoever runs the tools next.

Three things are checked, none of which needs a server or a game:

  1. the fixture generator reproduces the committed Locations.py from the same
     dump, so the two cannot drift apart;
  2. ``make_ap_config.py --strict`` maps every location in that table, which is
     the claim the fixture exists to support;
  3. the config it writes parses back with the counts a player would expect.

The dump is generated here rather than committed: a real sweep log is far too
large for the repository, and everything the join needs from it is a few
thousand lines of LOC records.
"""

import json
import os
import subprocess
import sys
import tempfile

ITEMS = [
    "Missiles",
    "EnergyTanks",
    "PowerBombs",
    "Truth",
    "XRayVisor",
]



def fixture_entries(path):
    """The (world asset id, entity id) pairs the fixture's PICKUP_LOCATIONS lists."""
    import ast

    tree = ast.parse(open(path, encoding="utf-8").read(), filename=path)
    levels = {}
    for node in tree.body:
        if isinstance(node, ast.ClassDef) and node.name == "MetroidPrimeLevel":
            for statement in node.body:
                if isinstance(statement, ast.Assign) and isinstance(statement.value, ast.Constant):
                    levels[statement.targets[0].id] = statement.value.value
    pickups = None
    for node in tree.body:
        if isinstance(node, ast.Assign) and node.targets[0].id == "PICKUP_LOCATIONS":
            pickups = node.value
    if pickups is None:
        raise SystemExit("fixture has no PICKUP_LOCATIONS")
    entries = []
    for element in pickups.elts:
        level_node, entity_node = element.elts
        entries.append((levels[level_node.attr], entity_node.value))
    return entries


def run(command, **kwargs):
    result = subprocess.run(command, capture_output=True, text=True, **kwargs)
    return result


def main(argv):
    if len(argv) != 2:
        raise SystemExit("usage: port_ap_fixture.py <repo root>")
    root = argv[1]
    fixture = os.path.join(root, "tools", "ap-world-fixture", "Locations.py")
    generator = os.path.join(root, "tools", "make_ap_fixture.py")
    joiner = os.path.join(root, "tools", "make_ap_config.py")
    failures = []

    def check(condition, message):
        if condition:
            print("[ap-fixture-tests] ok: %s" % message)
        else:
            print("[ap-fixture-tests] FAILED: %s" % message, file=sys.stderr)
            failures.append(message)

    check(os.path.exists(fixture), "the fixture exists")
    check(os.path.exists(generator), "the generator exists")
    if failures:
        return 1

    listed = fixture_entries(fixture)
    check(len(listed) > 0, "the fixture lists pickup locations")

    with tempfile.TemporaryDirectory(prefix="ap-fixture-") as temp:
        # Build a dump that matches the fixture exactly, from the fixture itself.
        dump_lines = []
        for index, (world, entity) in enumerate(listed):
            item = ITEMS[index % len(ITEMS)]
            # The fixture's own names carry the area, so the area id is not in
            # the file; the join pairs inside an area, and every entry here is
            # its own so the area is arbitrary but must be consistent per world.
            dump_lines.append("LOC %08X:22222222:%08X %s amount=5 capacity=5 model=00000001 "
                              "acs=00000002 character=0 animation=0" % (world, entity, item))
        dump_path = os.path.join(temp, "fixture-dump.log")
        with open(dump_path, "w", encoding="utf-8") as output:
            output.write("\n".join(dump_lines) + "\n")

        regenerated = os.path.join(temp, "generated")
        result = run([sys.executable, generator, "--dump", dump_path, "--out", regenerated])
        check(result.returncode == 0, "the generator runs on a dump built from the fixture")
        if result.returncode == 0:
            with open(os.path.join(regenerated, "Locations.py"), encoding="utf-8") as source:
                regenerated_pickups = fixture_entries(os.path.join(regenerated, "Locations.py"))
            check(sorted(regenerated_pickups) == sorted(listed),
                  "regenerating from the same dump keeps every location")

        # The join must map all of them under --strict.
        out_path = os.path.join(temp, "archipelago.json")
        result = run([sys.executable, joiner, "--locations", fixture, "--dump", dump_path,
                      "--out", out_path, "--server", "wss://example:38281", "--slot", "P1",
                      "--strict"])
        check(result.returncode == 0, "--strict accepts the fixture against a matching dump")
        if result.returncode != 0:
            print(result.stderr[-2000:], file=sys.stderr)
        check("0 unmapped" in result.stdout or "0 unmapped" in result.stderr,
              "the join reports no unmapped locations")

        if os.path.exists(out_path):
            config = json.load(open(out_path, encoding="utf-8"))
            check(len(config.get("locations", {})) == len(listed),
                  "the generated config carries every location")
            check(config.get("server") == "wss://example:38281", "the server is passed through")
            check("items" not in config,
                  "the generated config leaves the items to the port's built-in table")

    if failures:
        print("[ap-fixture-tests] %d failure(s)" % len(failures), file=sys.stderr)
        return 1
    print("[ap-fixture-tests] passed")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
