#!/usr/bin/env python3
"""Create item-randomizer seed JSON from location-dump logs.

Location keys use the C++ format ``%08X:%08X:%08X``: uppercase, zero-padded
eight-digit hexadecimal world asset ID, area asset ID, and entity/editor ID.
"""

import argparse
import json
import random
import re


LOCATION_LINE = re.compile(
    r"^LOC ([0-9A-Fa-f]{8}:[0-9A-Fa-f]{8}:[0-9A-Fa-f]{8}) "
    r"([A-Za-z0-9_]+) amount=(-?\d+) capacity=(-?\d+)\s*$"
)


def write_json(path, data):
    with open(path, "w", encoding="utf-8") as output:
        json.dump(data, output, indent=2)
        output.write("\n")
    print(path)


def sample(path):
    write_json(
        path,
        {
            "seed": "sample",
            "locations": {
                "00000001:00000010:00000100": {
                    "item": "Missiles",
                    "amount": 5,
                    "capacity": 5,
                },
                "00000001:00000020:00000200": {
                    "item": "EnergyTanks",
                    "amount": 1,
                    "capacity": 1,
                },
            },
        },
    )


def from_dump(log_path, out_path, seed_name, shuffle_seed):
    # Keep one entry per location key (using its last dump observation), since
    # the seed format is a mapping and cannot represent duplicate locations.
    locations = {}
    with open(log_path, "r", encoding="utf-8") as source:
        for line in source:
            match = LOCATION_LINE.match(line.rstrip("\r\n"))
            if match:
                key, item, amount, capacity = match.groups()
                locations[key.upper()] = (item, int(amount), int(capacity))

    if not locations:
        raise ValueError("dump contains no LOC lines in the expected format")

    keys = sorted(locations)
    placements = list(locations.values())
    if shuffle_seed is None:
        random.shuffle(placements)
    else:
        random.Random(shuffle_seed).shuffle(placements)

    seed_locations = {}
    for key, (item, amount, capacity) in zip(keys, placements):
        seed_locations[key] = {"item": item, "amount": amount, "capacity": capacity}
    write_json(out_path, {"seed": seed_name, "locations": seed_locations})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--sample", metavar="OUT", help="write a small sample seed")
    mode.add_argument("--from-dump", metavar="LOG", help="shuffle placements from a LOC dump")
    parser.add_argument("--out", metavar="OUT", help="output seed path for --from-dump")
    parser.add_argument("--seed", metavar="NAME", help="seed name (default: randomized)")
    parser.add_argument("--shuffle-seed", metavar="N", type=int, help="make shuffling deterministic")
    args = parser.parse_args()

    if args.sample:
        if args.out or args.seed is not None or args.shuffle_seed is not None:
            parser.error("--out, --seed, and --shuffle-seed apply only to --from-dump")
        sample(args.sample)
        return

    if not args.out:
        parser.error("--from-dump requires --out OUT")
    try:
        from_dump(args.from_dump, args.out, args.seed or "randomized", args.shuffle_seed)
    except (OSError, ValueError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
