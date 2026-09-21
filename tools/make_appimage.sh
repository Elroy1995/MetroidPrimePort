#!/usr/bin/env bash
# Packages the port as a distributable AppImage: the executable plus its HD
# texture replacements, with a launcher and icon. The disc image is not
# included (it is the user's own), so the port asks for it on first launch and
# remembers the answer.
#
# Usage: tools/make_appimage.sh [build-dir] [output-dir]
#
# appimagetool is fetched into the output directory on first use; it needs
# network access to pull the AppImage runtime unless --runtime-file is used.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${1:-"$ROOT/build/port-gcc"}
OUT=${2:-"$ROOT/build/appimage"}
BIN="$BUILD/metroid_prime_port"
TEXTURES="$ROOT/textures"
ICON="$ROOT/assets/metroid-prime.png"
TOOL="$OUT/appimagetool-x86_64.AppImage"
TOOL_URL="https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage"

if [[ ! -x "$BIN" ]]; then
    echo "no executable at $BIN - build first" >&2
    exit 1
fi
if [[ ! -d "$TEXTURES" ]]; then
    echo "no texture replacements at $TEXTURES" >&2
    exit 1
fi

APPDIR="$OUT/MetroidPrime.AppDir"
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin"

install -m755 "$BIN" "$APPDIR/usr/bin/metroid_prime_port"
# The port looks for replacements next to the executable.
cp -r "$TEXTURES" "$APPDIR/usr/bin/textures"
install -m644 "$ICON" "$APPDIR/metroid-prime.png"

cat > "$APPDIR/AppRun" <<'EOF'
#!/bin/sh
HERE=$(dirname "$(readlink -f "$0")")
exec "$HERE/usr/bin/metroid_prime_port" "$@"
EOF
chmod 755 "$APPDIR/AppRun"

cat > "$APPDIR/metroid-prime.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Metroid Prime
Comment=Native recompilation of Metroid Prime
Exec=metroid_prime_port
Icon=metroid-prime
Categories=Game;
Terminal=false
EOF

if [[ ! -x "$TOOL" ]]; then
    echo "fetching appimagetool"
    curl -sSL -o "$TOOL" "$TOOL_URL"
    chmod +x "$TOOL"
fi

# FUSE is not available everywhere; the tool can unpack itself instead.
RUN=()
if ! "$TOOL" --version >/dev/null 2>&1; then
    RUN=(--appimage-extract-and-run)
fi

ARCH=x86_64 "$TOOL" "${RUN[@]}" "$APPDIR" "$OUT/MetroidPrime-x86_64.AppImage"
echo "wrote $OUT/MetroidPrime-x86_64.AppImage"
