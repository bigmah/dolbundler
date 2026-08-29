#!/usr/bin/env bash
# Run the browser build in the iOS Simulator's Safari, and collect what it says.
#
#   ./sim-run.sh --seconds 120 --extra 'acts=g25:5,g40:5,g52:5'
#   ./sim-run.sh --backend Null --seconds 90
#
# Why the simulator: Safari has no headless mode and no remote-control API, so
# every browser measurement so far has been Chrome -- a different engine, a
# different WebGL implementation, and not the one this port has to land in.
# Simulator Safari is the same WebKit as the phone, on the same host network
# (127.0.0.1 reaches serve.py, and localhost is a secure context, so
# SharedArrayBuffer works without the LAN certificate dance).
#
# What it is *not* is a performance proxy: the GPU underneath is the Mac's, not
# an A17's. Read it for correctness, and take speed to the device.
#
# The page drives itself here -- ?acts= for the input timeline, ?report=1 to
# POST progress back to serve.py -- because nothing outside it can.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPORTS="$HERE/../reports.jsonl"

DEVICE="${SIM_DEVICE:-iPhone 17 Pro}"
BACKEND=OGL
SECONDS_BUDGET=120
SHOT_EVERY=20
OUT="${SIM_OUT:-$HERE/sim-shots}"
EXTRA=""
PORT="${PORT:-8712}"

while [ $# -gt 0 ]; do
  case "$1" in
    --device) DEVICE="$2"; shift 2 ;;
    --backend) BACKEND="$2"; shift 2 ;;
    --seconds) SECONDS_BUDGET="$2"; shift 2 ;;
    --shot-every) SHOT_EVERY="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --extra) EXTRA="$2"; shift 2 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

curl -sf -o /dev/null "http://127.0.0.1:$PORT/index.html" || {
  echo "nothing serving on $PORT -- run: python3 ../serve.py dolphin" >&2; exit 1; }

udid=$(xcrun simctl list devices available -j |
  python3 -c "
import json,sys
name=sys.argv[1]
for runtime, devs in json.load(sys.stdin)['devices'].items():
    for d in devs:
        if d['name'] == name:
            print(d['udid']); raise SystemExit
raise SystemExit('no simulator named ' + name)
" "$DEVICE")
echo "simulator: $DEVICE ($udid)"
xcrun simctl bootstatus "$udid" -b >/dev/null 2>&1 || xcrun simctl boot "$udid" || true
xcrun simctl bootstatus "$udid" -b >/dev/null

# Where reports.jsonl is now, so the run's own lines can be separated from every
# previous run's afterwards.
before=$( [ -f "$REPORTS" ] && wc -l < "$REPORTS" || echo 0 )

mkdir -p "$OUT"
rm -f "$OUT"/shot-*.png
url="http://127.0.0.1:$PORT/index.html?auto=1&report=1&backend=$BACKEND"
[ -n "$EXTRA" ] && url="$url&$EXTRA"
echo "url: $url"
# about:blank first: openurl on the page Safari is already showing does not
# reload it, so a second run would measure the first one still running.
xcrun simctl openurl "$udid" "about:blank" >/dev/null 2>&1 || true
sleep 2
xcrun simctl openurl "$udid" "$url"

n=0
elapsed=0
while [ "$elapsed" -lt "$SECONDS_BUDGET" ]; do
  sleep "$SHOT_EVERY"
  elapsed=$((elapsed + SHOT_EVERY))
  n=$((n + 1))
  xcrun simctl io "$udid" screenshot --type png \
    "$OUT/shot-$(printf %02d "$n").png" >/dev/null 2>&1 || true
  echo "  ${elapsed}s -> $OUT/shot-$(printf %02d "$n").png"
done

echo "--- what the page reported ---"
if [ -f "$REPORTS" ]; then
  tail -n +$((before + 1)) "$REPORTS" |
    python3 -c "
import json,sys
rows=[json.loads(l) for l in sys.stdin if l.strip()]
if not rows:
    print('no reports: the page never got far enough to POST')
    raise SystemExit
for r in rows[:1] + rows[-1:]:
    print(f\"  ms={r.get('ms')} phase={r.get('phase','-')} heapMB={r.get('heapMB')}\")
    for line in (r.get('perf') or [])[-2:]:
        print('   ', line)
speeds=[]
import re
for r in rows:
    for line in (r.get('perf') or []):
        m=re.search(r'(\d+)% speed', line)
        if m: speeds.append(int(m.group(1)))
if speeds:
    tail=sorted(speeds[len(speeds)//2:])
    print(f'  median speed over the second half: {tail[len(tail)//2]}%')
"
fi
