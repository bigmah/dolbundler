#!/usr/bin/env bash
# Queue a run for the phone's lobby page (web/lobby.html), and say whether the
# phone is listening.
#
#   ./queue-phone.sh 'backend=OGL&seconds=90&env=DOLWEB_STATE=/game/ollie.sav'
#
# The URL always gets auto=1, report=1 and lobby=1; the page returns to the
# lobby when its budget is spent, and the queue (next-url.txt, one per line)
# is handed out in order, so a whole comparison can be queued at once. Name
# the build in the query (build=snap-c64-h) rather than relying on the web/
# symlinks, which the simulator harness switches under you. Read the
# results with ./state-rate.py --ua phone and ./main-thread.py --ua phone.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
Q="${1:?usage: queue-phone.sh '<query string>'}"
url="/index.html?auto=1&report=1&lobby=1&$Q"
printf '%s\n' "$url" >> "$HERE/../next-url.txt"
seen="$HERE/../lobby-seen.txt"
if [ -f "$seen" ]; then
  age=$(( $(date +%s) - $(cut -d' ' -f1 "$seen") ))
  if [ "$age" -lt 10 ]; then echo "queued $url  (lobby polled ${age}s ago)";
  else echo "queued $url  (WARNING: lobby last polled ${age}s ago -- is the phone on lobby.html?)"; fi
else
  echo "queued $url  (WARNING: no phone has ever polled the lobby)"
fi
