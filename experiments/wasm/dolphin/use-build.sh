#!/usr/bin/env bash
# Point the served page at one of several builds, so two of them can be
# compared in the same browser without a rebuild between the readings.
#
#   ./use-build.sh web          # build-wasm/web
#   ./use-build.sh web256       # build-wasm/web256
#
# web/ holds symlinks rather than copies for exactly this reason; the README
# describes the swap by hand, and doing it by hand is how a measurement ends up
# attributed to the wrong build.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
NAME="${1:?usage: use-build.sh <build dir under build-wasm>}"
SRC="$ROOT/build-wasm/$NAME"
[ -f "$SRC/dolweb.wasm" ] || { echo "no dolweb.wasm in $SRC" >&2; exit 1; }
for f in dolweb.js dolweb.wasm dolweb.data; do
  [ -e "$SRC/$f" ] || continue
  ln -sfn "$SRC/$f" "$HERE/web/$f"
done
printf 'web/ -> %s  (' "$NAME"
ls -lL "$SRC/dolweb.wasm" | awk '{printf "%.1f MB wasm", $5/1048576}'
printf ')\n'
