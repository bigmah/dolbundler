#!/usr/bin/env python3
"""Guest throughput per session, for runs started from a savestate.

phone-window.py anchors to a guest second, which assumes the run booted: from a
state the guest clock starts wherever the state was captured (~120 billion ticks
for oglplay.sav), so there is no "guest second 125" to window on. This reports
the thing that is comparable instead -- guest ticks advanced per perf sample --
and groups it by session so a phone run and a Mac run can be read side by side.

Take the *ratio* between two sessions on the same device, never the absolute
number across devices: the sample interval (DOLWEB_PERF_INTERVAL) is fixed within
a run but not guaranteed equal between machines, and it cancels in a ratio.

Do not use "% speed" for this. With the throttle on it caps at exactly 100 and
cannot see a difference at all -- four desktop-Safari runs, 1x against 4x pixels,
all returned an identical 55.5 fps / 100%. And the page re-posts its last three
perf lines every time, so counting them without deduplicating by tick value
inflates a 95 s run to "182 samples".

  ./state-rate.py                      # every recent session
  ./state-rate.py --ua phone --last 6  # the last six phone sessions
"""
import argparse, json, os, re, sys, statistics

REPORTS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "reports.jsonl")
TICKS = re.compile(r"ticks=(\d+)")


def ua_label(ua):
    if "Headless" in ua:
        return "chrome"
    if "iPhone" in ua or "iPad" in ua:
        return "phone"
    if "Mac OS X" in ua and "Chrome" not in ua:
        return "safari-mac"
    return "other"


def runs(path, tail_bytes):
    """Group reports by the page load that produced them.

    Every report carries a `run` id minted once per page load. Before that
    existed this had to be reconstructed, and it could not be done reliably:
    `ms` is each page's own clock, so two pages posting at once interleave two
    unrelated clocks; arrival time cannot separate runs either, because a stale
    tab re-posting its last perf lines every five seconds keeps the stream
    gapless forever. Three successive heuristics all produced confident nonsense
    -- 8126 bogus sessions, then a per-sample step 100x too small, then a
    renderer cost of 0.62x against a directly measured 1.24x. Reports older than
    2026-09-01 have no id and are skipped rather than guessed at.
    """
    grouped = {}
    with open(path, "rb") as f:
        if tail_bytes:
            size = f.seek(0, 2)
            f.seek(max(0, size - tail_bytes))
            f.readline()
        for line in f:
            try:
                r = json.loads(line)
            except Exception:
                continue
            rid = r.get("run")
            if not rid:
                continue
            grouped.setdefault(rid, []).append(r)
    return grouped


def rate(run):
    # Drop the first two: a run from a state spends them settling, and a booted
    # run spends them in the loading screen.
    steps = [b - a for a, b in zip(run[2:], run[3:])]
    steps = [s for s in steps if s > 0]
    if not steps:
        return None
    return statistics.median(steps), len(run)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--path", default=REPORTS)
    ap.add_argument("--ua", default="", help="chrome, phone, safari-mac")
    ap.add_argument("--last", type=int, default=12)
    ap.add_argument("--tail-bytes", type=int, default=40_000_000)
    a = ap.parse_args()

    found = []
    for rid, rows in runs(a.path, a.tail_bytes).items():
        machine = ua_label(rows[-1].get("ua", ""))
        if a.ua and machine != a.ua:
            continue
        ticks = set()
        for r in rows:
            for text in (r.get("perf") or []) + (r.get("tail") or []):
                m = TICKS.search(str(text))
                if m:
                    ticks.add(int(m.group(1)))
        run = sorted(ticks)
        if len(run) < 8:
            continue  # too short to mean anything; a frozen page lands here
        got = rate(run)
        if got:
            med, n = got
            last = rows[-1]
            found.append((machine, last.get("backend", "?"), str(last.get("build", "?")), n, med))

    if not found:
        print("no sessions with enough samples")
        return
    print(f"{'machine':<11}{'backend':<9}{'build':<16}{'samples':>8}{'M ticks/sample':>17}")
    for row in found[-a.last:]:
        key, backend, build, n, med = row
        print(f"{key:<11}{backend:<9}{str(build):<16}{n:>8}{med/1e6:>17.1f}")

    # The comparison is the point: same machine, renderer on against renderer off.
    for key in sorted({r[0] for r in found[-a.last:]}):
        rows = [r for r in found[-a.last:] if r[0] == key]
        on = [r[4] for r in rows if r[1] == "OGL"]
        off = [r[4] for r in rows if r[1] == "Null"]
        if on and off:
            print(f"\n{key}: renderer costs {statistics.median(off)/statistics.median(on):.2f}x "
                  f"(OGL {statistics.median(on)/1e6:.1f} vs Null {statistics.median(off)/1e6:.1f}, "
                  f"{len(on)} on / {len(off)} off)")


if __name__ == "__main__":
    main()
