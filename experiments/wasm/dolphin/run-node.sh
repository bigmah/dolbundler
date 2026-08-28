#!/usr/bin/env bash
# Run the wasm measurement build and report how long the guest took to reach a
# fixed point in its own boot. Guest-relative markers are the only honest
# comparison across builds: host wall clock alone says nothing about how much
# emulated work happened.
#
#   ./run-node.sh [seconds] [backend]
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
BUILD="${DOLWEB_BUILD:-$ROOT/build-wasm/node}"
GAME="${DOLWEB_GAME:-$ROOT/build-wasm/gexe52}"
USER_DIR="${DOLWEB_USER:-$ROOT/build-wasm/user}"
SECONDS_BUDGET="${1:-90}"
BACKEND="${2:-Null}"
LOG="${DOLWEB_LOG:-$ROOT/build-wasm/run.log}"

node "$BUILD/dolweb.js" "$GAME" "$USER_DIR" "$BACKEND" "$SECONDS_BUDGET" 2>&1 | tee "$LOG"
echo
"$HERE/report.py" "$LOG"
