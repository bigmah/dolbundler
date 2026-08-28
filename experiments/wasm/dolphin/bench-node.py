#!/usr/bin/env python3
"""Measure the node build over a guest tick window.

Wall-clock windows compare different scenes: a faster build reaches a given
moment of the attract loop sooner, so two runs of the same length cover
different content and the answer is whatever the game happened to be showing.
The guest's own clock does not have that problem -- equal tick ranges are equal
scenes -- and it is what the browser harness's ?ab=N uses for the same reason.

    ./bench-node.py --build ../../../build-wasm/node --from 45 --window 45

Prints guest seconds covered per wall second over the window, which is the
number: 100% means the emulator is keeping up.
"""
import argparse
import os
import re
import subprocess
import sys
import time

PERF = re.compile(r"\[perf\].*?cpu=(\d+) ticks=(\d+)")
CLOCK = re.compile(r"guest clock (\d+) Hz")


def main():
    ap = argparse.ArgumentParser()
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../.."))
    ap.add_argument("--build", default=os.path.join(root, "build-wasm/node"))
    ap.add_argument("--game", default=os.path.join(root, "build-wasm/gexe52"))
    ap.add_argument("--user", default=os.path.join(root, "build-wasm/user"))
    ap.add_argument("--backend", default="Null")
    ap.add_argument("--from", dest="start", type=float, default=45.0,
                    help="window start, in guest seconds since boot")
    ap.add_argument("--window", type=float, default=45.0,
                    help="window length, in guest seconds")
    ap.add_argument("--label", default="")
    ap.add_argument("--log", default="")
    ap.add_argument("env", nargs="*", help="KEY=VALUE passed to the harness")
    args = ap.parse_args()

    cmd = ["node", os.path.join(args.build, "dolweb.js"), args.game, args.user,
           args.backend, "0"]
    # Unthrottled, or a build with headroom reads 100% exactly like one without.
    if not any(e.startswith("MODERNGEKKO_EMULATION_SPEED") for e in args.env):
        cmd.append("MODERNGEKKO_EMULATION_SPEED=0")
    cmd += args.env

    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, bufsize=1)
    log = open(args.log, "w") if args.log else None
    tps = 0
    t_open = ticks_open = None
    speeds = []
    result = None
    try:
        for line in proc.stdout:
            if log:
                log.write(line)
            m = CLOCK.search(line)
            if m:
                tps = int(m.group(1))
                continue
            m = PERF.search(line)
            if not m or not tps:
                continue
            cpu, ticks = int(m.group(1)), int(m.group(2))
            if cpu != 0:
                continue
            now = time.monotonic()
            guest = ticks / tps
            if t_open is None:
                if guest < args.start:
                    continue
                t_open, ticks_open = now, ticks
                print("window open at guest %.1f s" % guest, file=sys.stderr)
                continue
            sp = re.search(r"(\d+)% speed", line)
            if sp:
                speeds.append(int(sp.group(1)))
            if guest >= args.start + args.window:
                covered = (ticks - ticks_open) / tps
                wall = now - t_open
                result = (100.0 * covered / wall, covered, wall)
                break
    finally:
        proc.kill()
        if log:
            log.close()

    if not result:
        print("never reached the window (tps=%d)" % tps)
        return 1
    speeds.sort()
    q = lambda f: speeds[min(len(speeds) - 1, int(len(speeds) * f))] if speeds else 0
    print("%-12s speed %.1f%%  (guest %.1f s in %.1f s wall)  samples %d  "
          "p25 %d med %d p75 %d"
          % (args.label or args.backend, result[0], result[1], result[2],
             len(speeds), q(.25), q(.5), q(.75)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
