#!/usr/bin/env bash
# Drive a page in headless Chrome and print the JSON it POSTs back.
#
#   ./run-headless.sh selftest.html
#   ./run-headless.sh '?iso=/disc.iso&auto=1&frames=300'
#
# Chrome is the only headless browser here with a working WebGPU adapter; Safari
# has no headless mode and the iOS Simulator's requestAdapter() returns null, so
# device checks stay manual. See PLAN.md milestone 6.
#
# --autoplay-policy lets ?audio=1 exercise the DSP -> AudioWorklet path without a
# gesture; --mute-audio keeps that off the machine's speakers. Both, always: a
# scripted run should never make noise in someone's room.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PAGE="${1:-selftest.html}"
WAIT="${WAIT:-90}"
# PROFILE_DIR keeps the browser profile between runs, which is the only way to
# test anything that persists -- OPFS holds the disc and the memory card.
PROFILE="${PROFILE_DIR:-$(mktemp -d)}"
mkdir -p "$PROFILE"
: > "$HERE/reports.jsonl"
"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" \
  --headless=new --enable-unsafe-webgpu --use-angle=metal \
  --enable-features=WebGPU --disable-gpu-sandbox \
  --window-size=1280,960 \
  --disable-background-timer-throttling \
  --disable-backgrounding-occluded-windows \
  --disable-renderer-backgrounding \
  --disable-gpu-vsync --disable-frame-rate-limit \
  --autoplay-policy=no-user-gesture-required \
  --mute-audio \
  --ignore-certificate-errors \
  --user-data-dir="$PROFILE" --no-first-run --no-default-browser-check \
  "${SCHEME:-http}://127.0.0.1:${PORT:-8712}/${PAGE}" >"$PROFILE/chrome.log" 2>&1 &
CHROME=$!
# Wait for the FINAL record, not the first: the page posts progress as it goes,
# so "the file is non-empty" fires a second after launch and cuts the run short.
for _ in $(seq 1 "$WAIT"); do
  grep -q '"wall_ms"\|"phase": *"setup-failed"\|"phase": *"boot-failed"' "$HERE/reports.jsonl" 2>/dev/null && break
  sleep 1
done
kill "$CHROME" 2>/dev/null || true
[ -n "${PROFILE_DIR:-}" ] || rm -rf "$PROFILE" 2>/dev/null || true
python3 - "$HERE/reports.jsonl" <<'PY'
import json, sys, pathlib
p = pathlib.Path(sys.argv[1])
if not p.exists() or not p.read_text().strip():
    print("NO REPORT (page never POSTed; check the browser console)")
    raise SystemExit(1)
lines = p.read_text().strip().splitlines()
final = [l for l in lines if '"wall_ms"' in l]
rec = json.loads((final or lines)[-1])
rec.pop("png", None)
rec.pop("log", None)
print(json.dumps(rec, indent=1))
PY
