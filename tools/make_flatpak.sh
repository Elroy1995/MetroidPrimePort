#!/usr/bin/env bash
# Builds a Flatpak bundle of the port.
#
# A Flatpak brings its own runtime, so unlike the AppImage it does not care what
# glibc the host has; the GPU driver still comes from the host. The disc image
# is not included and is read-only from inside the sandbox, so the port asks for
# it on first launch as usual.
#
# Usage: tools/make_flatpak.sh [output-dir]
#
# Needs flatpak and flatpak-builder, and downloads the runtime and SDK on first
# use. The build compiles the whole game, so expect it to take a while.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-"$ROOT/build/flatpak"}
MANIFEST="$ROOT/flatpak/io.github.odrannnn.metroidprimeport.yml"
APP_ID=$(sed -n 's/^app-id: *//p' "$MANIFEST")

for tool in flatpak flatpak-builder; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "$tool is not installed; install flatpak and flatpak-builder" >&2
        exit 1
    fi
done

# The manifest's git source points at the repository, so the tree has to be
# committed for the build to see the current code.
if [[ -n "$(git -C "$ROOT" status --porcelain)" ]]; then
    echo "warning: uncommitted changes are not part of a git source" >&2
fi

mkdir -p "$OUT"
# rofiles-fuse gives the build a source tree whose files are immutable, so a
# build step cannot modify the checkout it came from. It needs FUSE, and the
# fusermount3 helper is setuid - which does not help once the caller is already
# inside a user namespace, because a setuid binary cannot gain privilege inside
# one. On such a host every attempt fails with
#   fusermount3: mount failed: Permission denied
# even with user_allow_other set in /etc/fuse.conf and even though a manual
# rofiles-fuse mount as the same user succeeds. The flag below swaps the
# mechanism and is otherwise equivalent, so try it first and fall back only if
# the real thing is refused.
# --share=network is needed at BUILD time and has nothing to do with the
# manifest's finish-args: the build fetches Dawn, SDL, abseil, fmt, zstd and the
# rest over HTTPS through FetchContent, and flatpak-builder gives the build no
# network by default. Without it every fetch dies with
#   getaddrinfo(3) failed for github.com:443
# during the configure step, on Dawn first. The installed app is unaffected -
# finish-args still governs what the game itself gets, and it does not ask for
# the network.
build_with_rofiles() {
    flatpak-builder --share=network --force-clean \
        --repo="$OUT/repo" "$OUT/build" "$MANIFEST" "$@"
}

if ! build_with_rofiles; then
    echo >&2
    echo "note: retrying without rofiles-fuse (see the comment above)" >&2
    build_with_rofiles --disable-rofiles-fuse
fi
flatpak build-bundle "$OUT/repo" "$OUT/MetroidPrimePort.flatpak" "$APP_ID"
echo "wrote $OUT/MetroidPrimePort.flatpak"
echo "install with: flatpak install --user $OUT/MetroidPrimePort.flatpak"
