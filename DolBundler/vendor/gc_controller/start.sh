#!/bin/sh
# Builds if needed, then runs the driver in the foreground.
#
# Ctrl-C stops it, or run ./stop.sh from another terminal.
#
# Dolphin is deliberately not launched here — start it yourself. The only
# ordering constraint is the very first run: Dolphin scans its Pipes directory
# once at startup, so the FIFO has to exist by then. It persists on disk
# afterwards, so from the second run on the order does not matter. What does
# matter is that the driver is running while you play; nothing reaches Dolphin
# otherwise.
#
# Arguments are passed straight through, so `./start.sh --transport ble` and any
# other flag work as if you had run the binary directly.
set -e

DIR=$(cd "$(dirname "$0")" && pwd)
BIN="$DIR/target/release/gc_controller"

[ -x "$BIN" ] || cargo build --release --manifest-path "$DIR/Cargo.toml"

# exec so signals reach the driver directly rather than going through this shell.
exec "$BIN" "$@"
