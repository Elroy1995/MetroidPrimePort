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
flatpak-builder --force-clean --repo="$OUT/repo" "$OUT/build" "$MANIFEST"
flatpak build-bundle "$OUT/repo" "$OUT/MetroidPrimePort.flatpak" "$APP_ID"
echo "wrote $OUT/MetroidPrimePort.flatpak"
echo "install with: flatpak install --user $OUT/MetroidPrimePort.flatpak"
