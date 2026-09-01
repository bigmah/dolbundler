#!/usr/bin/env bash
# One A/B pair (Null then OpenGL) in desktop Safari, over a guest-anchored
# window, and wait for the *result* rather than for a clock.
#
# `safari-run.sh` sleeps a fixed number of seconds and then reads whatever
# accumulated. That is fine for a speed histogram and wrong for an A/B: the run
# takes as long as the build is slow, so a fixed budget either wastes minutes on
# a fast build or truncates a slow one -- and a truncated A/B looks exactly like
# a build that never reached the level.
#
# So this polls reports.jsonl for the two `ab-result` rows the page posts, and
# gives up only on a deadline. Everything else is safari-run.sh's: the window is
# closed by its *title*, before as well as after, because a leftover window
# keeps emulating and the next measurement reads the contention.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
PORT="${PORT:-8713}"
# Guest 125-155 s is inside Andy's House. Anchored at 45 the window lands in the
# attract loop and the title, where the renderer is nearly free and every number
# is 2x too kind -- which is what made this file's earlier readings wrong.
AB="${AB:-30}"
ABFROM="${ABFROM:-125}"
ACTS="${ACTS:-g25:5,g40:5,g52:5,g70:0,g76:0,g82:0,g110:15:9000,g135:15:9000,g160:15:9000}"
DEADLINE="${DEADLINE:-900}"
# How many halves to wait for. WANT=1 stops after Null, which is the CPU
# question on its own -- and the OpenGL half has to boot through the intro
# movies, which the renderer plays at about a tenth of realtime.
WANT="${WANT:-2}"
export WANT
EXTRA="${1:-}"
TITLE="Dolphin in WebAssembly"
REPORTS="$HERE/../reports.jsonl"

close_all() {
  osascript -e "tell application \"Safari\" to close (every window whose name contains \"$TITLE\")" \
    >/dev/null 2>&1 || true
}

close_all
sleep 2

url="http://127.0.0.1:$PORT/index.html?auto=1&report=1&ab=$AB&abfrom=$ABFROM&acts=$ACTS"
[ -n "$EXTRA" ] && url="$url&$EXTRA"
echo "url: $url"

before=$(wc -l < "$REPORTS" 2>/dev/null || echo 0)
osascript -e "tell application \"Safari\" to make new document with properties {URL:\"$url\"}" \
  >/dev/null 2>&1

deadline=$((SECONDS + DEADLINE))
while [ "$SECONDS" -lt "$deadline" ]; do
  sleep 10
  got=$(tail -n +$((before + 1)) "$REPORTS" 2>/dev/null |
        grep -c '"phase": "ab-result"' || true)
  [ "${got:-0}" -ge "$WANT" ] && break
done
close_all

tail -n +$((before + 1)) "$REPORTS" | python3 -c "
import json,os,sys
want = int(os.environ.get('WANT', '2'))
n=0
for line in sys.stdin:
    try: r=json.loads(line)
    except: continue
    if r.get('phase')!='ab-result': continue
    n+=1
    print(f\"  {r['abBackend']:5s} {r['speed']:6.1f}%  median {r['median']:3d}  \"
          f\"p25 {r['p25']} p75 {r['p75']}  range {r['min']}-{r['max']}  \"
          f\"guest {r['guest']}s / wall {r['wall']}s  window {r['window']}\")
    # The scene, not just the window. A guest-anchored window opens at the same
    # guest second in every build; it does not follow that the game is in the
    # same place when it does. Two builds read 84% and 209% over guest 125-155,
    # with sample ranges 64-106 and 189-228 -- the level and the menus -- and
    # nothing in either result said which was which.
    acts = r.get('acts') or []
    if acts:
        print(f\"        acts {len(acts)} fired, last control {acts[-1]['c']} \"
              f\"at guest {acts[-1]['g']}s / wall {acts[-1]['w']}s\")
    else:
        print('        acts: none recorded')
if n < want: print(f'  INCOMPLETE: {n} of {want} halves')
"
