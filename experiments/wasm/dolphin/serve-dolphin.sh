#!/usr/bin/env bash
# Put the browser build, the disc and the manifest where serve.py can see them,
# then serve. Symlinks rather than copies: the module is ~100 MB and the disc is
# 1.2 GB, and neither belongs in the repository.
#
#   ./serve-dolphin.sh                 # localhost
#   ./serve-dolphin.sh --lan           # reachable from a phone, over HTTPS
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
BUILD="${DOLWEB_BUILD:-$ROOT/build-wasm/web}"
GAME="${DOLWEB_GAME:-$ROOT/build-wasm/gexe52}"

[ -f "$BUILD/dolweb.js" ] || { echo "no browser build: run ./build.sh first" >&2; exit 1; }
[ -d "$GAME/sys" ] || { echo "no extracted game at $GAME" >&2; exit 1; }

"$HERE/make-manifest.py" "$GAME" > "$GAME/.manifest"
echo "manifest: $(wc -l < "$GAME/.manifest") files"

ln -sfn "$BUILD/dolweb.js" "$HERE/web/dolweb.js"
ln -sfn "$BUILD/dolweb.wasm" "$HERE/web/dolweb.wasm"
[ -f "$BUILD/dolweb.data" ] && ln -sfn "$BUILD/dolweb.data" "$HERE/web/dolweb.data"
ln -sfn "$GAME" "$HERE/web/game"

exec python3 "$HERE/../serve.py" dolphin "$@"
