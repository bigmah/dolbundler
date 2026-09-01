#!/usr/bin/env python3
"""Speed over a guest-anchored window, from a plain ?report=1 session.

`?ab=` computes this itself and posts one row, but its Null half is a black
screen -- on a phone that is the half someone closes before it finishes. A
plain unthrottled run renders normally and stays watchable, so it is the better
device instrument; this is the arithmetic ?ab would have done.

Speed is guest seconds advanced per wall second across the window, not a median
of the per-sample percentages: a median of samples cannot see a change of a few
percent (measured: it read two builds 7% apart the wrong way round).

  ./phone-window.py --machine phone --from 125 --to 175
"""
import argparse, json, os, re, sys

REPORTS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "reports.jsonl")
PERF = re.compile(r"([\d.]+) fps\s+(\d+)% speed\s+pc=(0x[0-9a-f]+).*?ticks=(\d+)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--path", default=REPORTS)
    ap.add_argument("--machine", default="", help="phone, simulator, chrome-mac, safari-mac")
    ap.add_argument("--from", dest="lo", type=float, default=125.0)
    ap.add_argument("--to", dest="hi", type=float, default=175.0)
    ap.add_argument("--session", type=int, default=-1, help="-1 = the last one")
    # Wall seconds to reach fixed guest seconds. This is the measurement to
    # reach for when comparing two builds: the guest range is identical by
    # construction, so unlike a windowed speed it cannot be comparing two
    # different scenes -- which is how a 4096-chunk build and a 256-chunk one
    # came back at 84% and 209% with nothing in either number saying that one
    # was in the level and the other in the menus.
    ap.add_argument("--marks", default="",
                    help="comma-separated guest seconds, e.g. 25,60,100,125")
    args = ap.parse_args()

    # Sessions INTERLEAVE in this file. A phone and a Mac reporting at the same
    # time write alternating lines, so "the rows between two capabilities posts"
    # is not a session -- it is a shuffle of two, and reading it as one produced
    # a Mac curve that jumped from guest 63 s to guest 142 s in a single sample
    # because a phone row landed in the middle of it.
    #
    # What does separate them: the user agent (phone and Mac differ), and `ms`,
    # which counts from that page's own load and therefore only ever increases
    # within a session. A capabilities post or a backwards `ms` starts a new one.
    sessions, open_by_ua = [], {}
    for line in open(args.path):
        try:
            row = json.loads(line)
        except json.JSONDecodeError:
            continue
        ua = row.get("ua") or ""
        ms = row.get("ms")
        cur = open_by_ua.get(ua)
        if row.get("phase") == "capabilities" or cur is None or \
                (ms is not None and cur["last_ms"] is not None and ms < cur["last_ms"]):
            cur = {"caps": row if row.get("phase") == "capabilities" else (cur or {}).get("caps"),
                   "rows": [], "received": row.get("received", ""), "last_ms": None}
            open_by_ua[ua] = cur
            sessions.append(cur)
            if row.get("phase") == "capabilities":
                continue
        if ms is not None:
            cur["last_ms"] = ms
        cur["rows"].append(row)

    def machine_of(c):
        # Three machines answer to "Apple GPU", and telling them apart matters:
        # the simulator sends the *phone's* user agent, and desktop Safari sends
        # a Mac one. Renderer alone has already reported a simulator result as
        # the phone's once in this project.
        c = c or {}
        r = c.get("renderer") or ""
        if "ANGLE" in r:
            return "chrome-mac"
        if r == "Apple GPU":
            if "iPhone" not in (c.get("ua") or ""):
                return "safari-mac"
            if c.get("cores") == 4 and (c.get("screen") or "").startswith("430x932"):
                return "phone"
            return "simulator"
        return r or "unknown"

    picked = [s for s in sessions if not args.machine or machine_of(s["caps"]) == args.machine]
    if not picked:
        print("no sessions matched", file=sys.stderr)
        return 1
    s = picked[args.session]

    hz = 0
    for row in s["rows"]:
        for line in (row.get("perf") or []) + [str(row.get("phase") or "")]:
            m = re.search(r"guest clock (\d+) Hz", str(line))
            if m:
                hz = int(m.group(1))
    # The clock line is in the boot log rather than a perf line on some builds;
    # 486 MHz is the Gekko and the only value this has ever reported.
    hz = hz or 486000000

    # The page posts the last three [perf] lines every time, so the same line
    # arrives in up to three consecutive rows. Keying on ticks and keeping the
    # first arrival is what turns that back into one sample per emitted line --
    # without it a session of 40 samples reads as 118 and the wall clock
    # attributed to each is the row's, not the line's.
    seen = {}
    for row in s["rows"]:
        ms = row.get("ms")
        if ms is None:
            continue
        for line in (row.get("perf") or []):
            if "cpu=0" not in str(line):
                continue  # cpu=1 is the second core's view of the same moment
            m = PERF.search(str(line))
            if not m:
                continue
            ticks = int(m.group(4))
            if ticks in seen:
                continue
            seen[ticks] = (ms / 1000.0, float(m.group(1)), int(m.group(2)),
                           ticks / hz, m.group(3))
    pts = [seen[k] for k in sorted(seen)]
    if not pts:
        print("no perf samples in that session", file=sys.stderr)
        return 1

    inwin = [p for p in pts if args.lo <= p[3] <= args.hi]
    build = next((r.get('build') for r in s['rows'] if r.get('build')), '?')
    print(f"session {s['received'][:19]}  {machine_of(s['caps'])}  "
          f"build {build}  "
          f"{len(pts)} samples, guest {pts[0][3]:.0f}-{pts[-1][3]:.0f}s")
    # Did this run reach the game, or is it one of the black ones? A running
    # game visits many guest PCs; a stuck one visits a handful. Calibrated
    # against screenshots on 2026-08-31: 54-58 distinct PCs on runs confirmed in
    # Andy's Room, 5-6 on runs that came back with a black canvas -- and a black
    # run still posts a plausible 58.7 fps at 100% speed, which is how one got
    # read as a broken module for most of an afternoon.
    npc = len({p[4] for p in pts})
    verdict = ("looks like it reached the game" if npc >= 30 else
               "SUSPECT: too few PCs -- likely stuck or a black canvas"
               if npc <= 10 else "inconclusive")
    print(f"  {npc} distinct guest PCs -- {verdict}")
    if args.marks:
        cells = []
        for mark in [float(m) for m in args.marks.split(",") if m.strip()]:
            hit = next((p for p in pts if p[3] >= mark), None)
            cells.append(f"g{mark:.0f}@{hit[0]:.0f}s" if hit else f"g{mark:.0f}@--")
        print("  wall seconds to reach:  " + "  ".join(cells))
    if len(inwin) < 2:
        print(f"  guest {args.lo:.0f}-{args.hi:.0f}s: not reached "
              f"(furthest {pts[-1][3]:.1f}s)")
        return 0
    guest = inwin[-1][3] - inwin[0][3]
    wall = inwin[-1][0] - inwin[0][0]
    # Two samples a millisecond apart are not a measurement. reach.sh stops the
    # run the moment the guest passes its mark, which can leave exactly one
    # sample inside the window it then asks about.
    if wall <= 0.5:
        print(f"  guest {args.lo:.0f}-{args.hi:.0f}s: too few samples "
              f"({len(inwin)} over {wall:.2f}s wall) -- let the run go further "
              f"past the mark")
        return 0
    fps = sorted(p[1] for p in inwin)
    spd = sorted(p[2] for p in inwin)
    print(f"  guest {args.lo:.0f}-{args.hi:.0f}s: {100 * guest / wall:.1f}% "
          f"(guest {guest:.1f}s / wall {wall:.1f}s, {len(inwin)} samples)")
    print(f"  fps median {fps[len(fps) // 2]:.1f}   "
          f"speed median {spd[len(spd) // 2]}%  "
          f"range {spd[0]}-{spd[-1]}  under 70%: "
          f"{sum(1 for x in spd if x < 70)}/{len(spd)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
