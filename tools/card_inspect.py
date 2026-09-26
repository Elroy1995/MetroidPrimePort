#!/usr/bin/env python3
"""Report the state of every .gci save file on an Aurora memory card.

Answers the three questions that actually matter when a save looks wrong: does
the file exist, does its save region hold any data, and is its CRC valid.

    tools/card_inspect.py                       # the default card for this build
    tools/card_inspect.py /path/to/"Card A"     # any card directory

Layout, from extern/aurora's CardGciFolder: a 64-byte GCI "File" header, then an
8192-byte payload. Within the payload:

    [0:4]      CRC-32 of payload[4:]
    [4:68]     comment (game name, date, time)
    [68:5188]  banner and icon
    [5188:8192] save data

CRC WARNING, because this has cost real time twice. CCRC32::Calculate in
src/Kyoto/CCrc32.cpp is a RAW CRC-32: there is no final complement, unlike
zlib.crc32. So a valid file compares as:

    raw = (zlib.crc32(payload[4:]) & 0xFFFFFFFF) ^ 0xFFFFFFFF

against the stored big-endian value, NOT against plain zlib.crc32. The wrong form
reports every good file as corrupt. And note that a valid CRC only proves the
container is intact - it says nothing about whether the save inside it is any
good, which is the whole reason this prints the non-zero count too.
"""

import glob
import os
import sys
import zlib

# The card follows the executable, not MP_USER_PATH: CARDSetBasePath is handed
# SDL_GetBasePath(). Every build directory therefore has its own card.
DEFAULT_CARD = os.path.join("build", "smoke-gcc", "USA", "Card A")

PAYLOAD_OFFSET = 64
SAVE_REGION = (5188, 8192)


def inspect(path: str) -> None:
    with open(path, "rb") as handle:
        data = handle.read()

    payload = data[PAYLOAD_OFFSET:]
    raw = (zlib.crc32(payload[4:]) & 0xFFFFFFFF) ^ 0xFFFFFFFF
    stored = int.from_bytes(payload[0:4], "big")

    start, end = SAVE_REGION
    region = payload[start:end]
    nonzero = sum(1 for byte in region if byte)

    print(
        f"  {path}: size={len(data)} payload={len(payload)} "
        f"region_nonzero={nonzero}/{len(region)} "
        f"crc={'VALID' if raw == stored else 'INVALID'} "
        f"(stored={stored:08x} calc={raw:08x}) "
        f"region[0:8]={region[:8].hex()}"
    )


def main() -> int:
    root = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_CARD
    files = sorted(glob.glob(os.path.join(root, "**", "*.gci"), recursive=True))
    if not files:
        print(f"  (no .gci files under {root})")
        return 1
    for path in files:
        inspect(path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
