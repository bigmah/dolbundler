#!/usr/bin/env python3
"""List A/B results from reports.jsonl, attributed to the machine that ran them.

The user-agent does NOT identify the machine: simulator Safari sends the same
iPhone UA as the phone, so filtering reports by `ua` silently mixes a simulator
result into a device one. (It has already produced one wrong reading in this
project -- a simulator's 50.5% reported as the phone's.)

The honest discriminator is the `capabilities` report the page posts on load,
which carries `renderer`, `screen` and `cores`. Every later row in the file
belongs to whichever capabilities row most recently preceded it, so we walk the
file in order and label as we go.

  ./ab-results.py                 every result, labelled
  ./ab-results.py --since 15:10   only runs after a wall-clock time today
  ./ab-results.py --machine phone only the real device
"""
import argparse
import json
import os
import sys

REPORTS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "reports.jsonl")


def machine_of(caps):
    """Name the machine behind a capabilities row."""
    if caps is None:
        return "unknown"
    renderer = caps.get("renderer") or ""
    cores = caps.get("cores")
    screen = caps.get("screen") or ""
    if "ANGLE" in renderer:
        return "chrome-mac"
    if renderer == "Apple GPU":
        # The phone is a 4-core A17 at 430x932; the simulator borrows the Mac's
        # core count and its own device's screen. Neither the UA nor the
        # renderer string separates them -- only these do.
        if cores == 4 and screen.startswith("430x932"):
            return "phone"
        return "simulator"
    return renderer or "unknown"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--since", help="only rows received at/after this timestamp prefix, e.g. 15:10 or 2026-08-29T15")
    ap.add_argument("--machine", help="only this machine (phone, simulator, chrome-mac)")
    ap.add_argument("--path", default=REPORTS)
    args = ap.parse_args()

    caps = None
    out = []
    with open(args.path) as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            try:
                row = json.loads(line)
            except json.JSONDecodeError:
                continue
            if row.get("phase") == "capabilities":
                caps = row
                continue
            if row.get("phase") != "ab-result":
                continue
            got = row.get("received", "")
            if args.since and args.since not in got and got[11:] < args.since and args.since not in got[:len(args.since)]:
                # tolerate both "15:10" and a full-prefix form
                if not got.startswith(args.since) and got[11:16] < args.since:
                    continue
            name = machine_of(caps)
            if args.machine and name != args.machine:
                continue
            out.append((got, name, row))

    if not out:
        print("no results matched", file=sys.stderr)
        return 1
    for got, name, r in out:
        print(f"{got[:19]}  {name:10s} {str(r.get('abBackend')):5s} "
              f"{r.get('speed')}%  median {r.get('median')}  "
              f"p25 {r.get('p25')} p75 {r.get('p75')}  "
              f"range {r.get('min')}-{r.get('max')}  window {r.get('window')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
