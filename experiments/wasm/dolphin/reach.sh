#!/usr/bin/env bash
# How long does this build take to reach a fixed point in the *game*?
#
# This is the measurement to compare two builds with. `?ab=`'s windowed speed
# cannot do it: the browser's disc fetches freeze the guest clock for a wall
# time that varies run to run, so the same build over guest 125-155 came back
# at 84.1% and then 38.3% while its wall time to guest 125 was 150 s both
# times. Equal guest ranges are equal scenes and equal work; the only variable
# left is the machine.
#
#   ./reach.sh                 # to guest 125, Null, throttle off
#   MARKS=25,60,100,125 TO=125 ./reach.sh
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PORT="${PORT:-8713}"
TO="${TO:-125}"
MARKS="${MARKS:-25,60,100,125}"
BACKEND="${BACKEND:-Null}"
ACTS="${ACTS:-g25:5,g40:5,g52:5,g70:0,g76:0,g82:0,g110:15:9000,g135:15:9000,g160:15:9000}"
DEADLINE="${DEADLINE:-400}"
# Guest seconds to keep running past TO, so the window after it has samples.
LINGER="${LINGER:-30}"
EXTRA="${1:-}"
TITLE="Dolphin in WebAssembly"
REPORTS="$HERE/../reports.jsonl"

close_all() {
  osascript -e "tell application \"Safari\" to close (every window whose name contains \"$TITLE\")" \
    >/dev/null 2>&1 || true
}

close_all
sleep 2
url="http://127.0.0.1:$PORT/index.html?auto=1&report=1&backend=$BACKEND&seconds=0"
url="$url&env=MODERNGEKKO_EMULATION_SPEED=0&acts=$ACTS"
[ -n "$EXTRA" ] && url="$url&$EXTRA"
echo "url: $url"

before=$(wc -l < "$REPORTS" 2>/dev/null || echo 0)
osascript -e "tell application \"Safari\" to make new document with properties {URL:\"$url\"}" \
  >/dev/null 2>&1

deadline=$((SECONDS + DEADLINE))
while [ "$SECONDS" -lt "$deadline" ]; do
  sleep 5
  # Stop as soon as the guest is past the last mark; a fixed sleep would spend
  # the difference between a fast build and a slow one doing nothing.
  reached=$(tail -n +$((before + 1)) "$REPORTS" 2>/dev/null | python3 -c "
import json,re,sys
hz=486000000.0
best=0.0
for line in sys.stdin:
    try: r=json.loads(line)
    except: continue
    for l in (r.get('perf') or []):
        m=re.search(r'cpu=0 ticks=(\d+)', str(l))
        if m: best=max(best, int(m.group(1))/hz)
print(int(best))
" 2>/dev/null || echo 0)
  # LINGER keeps the run going past the mark. With the renderer on, the marks
  # themselves flatter: dual core lets the emulated CPU run ahead of the GPU, so
  # guest seconds per wall second says nothing about frames reaching the screen
  # -- the OpenGL run passed guest 125 in 48 s against Null's 65 s while
  # producing 9.5 fps. Judge a renderer by fps, which needs samples *after* the
  # mark to have any.
  [ "${reached:-0}" -ge "$((TO + LINGER))" ] && break
done
close_all
sleep 1
"$HERE/phone-window.py" --machine safari-mac --marks "$MARKS" --from "$TO" --to 100000
