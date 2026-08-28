#!/usr/bin/env bash
# Stage the part of Dolphin's Sys tree a GameCube boot actually reads.
#
# The whole tree is 14 MB and 2 631 files, most of it GameSettings INIs for
# titles this build will never see and Wii title databases it has no use for.
# What a GameCube game does read is small: the DSP ROMs, the IPL fonts (a game
# that draws its menus with the system font shows an empty menu without them),
# the post-processing shaders, and its own per-title INI.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
SRC="$ROOT/ModernGekko/vendor/dolphin/Data/Sys"
DEST="${1:-$ROOT/build-wasm/sys}"
GAME_ID="${2:-}"

rm -rf "$DEST"
mkdir -p "$DEST"
for dir in GC Shaders Resources Wii; do
  [ -d "$SRC/$dir" ] && cp -R "$SRC/$dir" "$DEST/$dir"
done
if [ -n "$GAME_ID" ] && [ -f "$SRC/GameSettings/$GAME_ID.ini" ]; then
  mkdir -p "$DEST/GameSettings"
  cp "$SRC/GameSettings/$GAME_ID.ini" "$DEST/GameSettings/"
fi
find "$DEST" -name .DS_Store -delete
du -sh "$DEST"
