#!/usr/bin/env bash
# Run the build in desktop Safari and report the speed it reached.
#
# Safari matters because it is real WebKit on the real Metal driver, which makes
# it a closer stand-in for the phone than the iOS simulator -- though not a
# sufficient one: the canvas-transfer defect of 2026-08-31 was invisible here
# and in the simulator, and only the device showed it.
#
# The window is closed by its *title*, which is what the page's <title> says --
# "Dolphin in WebAssembly". Matching on "dolweb" or the host instead silently
# matches nothing, and a leftover window keeps emulating at 100% CPU: three
# consecutive measurements came back at 20-22% against a true 94-100% purely
# because their predecessors were still running. Close before as well as after,
# because the last run may have died before its own cleanup.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
PORT="${PORT:-8713}"
SECONDS_BUDGET="${1:-140}"
EXTRA="${2:-}"
TITLE="Dolphin in WebAssembly"
REPORTS="$HERE/../reports.jsonl"

close_all() {
  osascript -e "tell application \"Safari\" to close (every window whose name contains \"$TITLE\")" \
    >/dev/null 2>&1 || true
}

close_all
sleep 2

url="http://127.0.0.1:$PORT/index.html?auto=1&report=1"
[ -n "$EXTRA" ] && url="$url&$EXTRA"
echo "url: $url"

before=$(wc -l < "$REPORTS" 2>/dev/null || echo 0)
osascript -e "tell application \"Safari\" to make new document with properties {URL:\"$url\"}" \
  >/dev/null 2>&1
sleep "$SECONDS_BUDGET"
close_all

tail -n +$((before + 1)) "$REPORTS" 2>/dev/null | python3 -c "
import json,re,statistics,sys
v=[]
for line in sys.stdin:
    if not line.strip(): continue
    try: r=json.loads(line)
    except: continue
    for l in (r.get('perf') or []):
        m=re.search(r'([\d.]+) fps\s+([\d.]+)% speed', str(l))
        if m: v.append((float(m.group(1)), float(m.group(2))))
if not v:
    print('no perf samples'); raise SystemExit
f=[x[0] for x in v]; s=[x[1] for x in v]
h=len(s)//2
under=sum(1 for x in s if x<70)
print(f'{len(v)} samples')
print(f'  fps    median {statistics.median(f):.1f}')
print(f'  speed  median {statistics.median(s):.0f}%   second half {statistics.median(s[h:]):.0f}%')
print(f'  under 70%: {under}/{len(s)} ({100*under/len(s):.0f}%)')
"
