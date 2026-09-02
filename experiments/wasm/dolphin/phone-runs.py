#!/usr/bin/env python3
"""Per-run summary of what the phone reported: speed, fps, GPU-thread and
main-thread busy fractions, guest ticks per sample. One line per run.

  ./phone-runs.py --last 8            # the last eight runs from any iPhone UA
  ./phone-runs.py --ua chrome

The simulator's UA also says iPhone; tell them apart by `screen` (the
capabilities report) -- a real phone is 430x932@3, the simulator 402x874@3 or
whatever device it emulates -- or by which one you queued.
"""
import argparse, json, os, re, statistics

REPORTS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "reports.jsonl")
PERF = re.compile(r"\[perf\]\s+([\d.]+) fps\s+(\d+)% speed.*?ticks=(\d+).*?gpu=(\d+)%")
CPUT = re.compile(r"cputime: events=(\d+)% throttle=(\d+)% gpuwait=(\d+)% \((\d+)/s\)")


def ua_label(ua):
    if "Headless" in ua:
        return "chrome"
    if "iPhone" in ua or "iPad" in ua:
        return "phone"
    if "Mac OS X" in ua and "Chrome" not in ua:
        return "safari-mac"
    return "other"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ua", default="phone")
    ap.add_argument("--last", type=int, default=10)
    ap.add_argument("--tail-bytes", type=int, default=60_000_000)
    a = ap.parse_args()
    runs, order = {}, []
    with open(REPORTS, "rb") as f:
        size = f.seek(0, 2)
        f.seek(max(0, size - a.tail_bytes))
        f.readline()
        for line in f:
            try:
                r = json.loads(line)
            except Exception:
                continue
            rid = r.get("run")
            if not rid:
                continue
            if rid not in runs:
                runs[rid] = {"ua": ua_label(r.get("ua", "")), "perf": {}, "main": [],
                             "build": "?", "backend": "?", "screen": "", "ms": 0, "heap": 0}
                order.append(rid)
            run = runs[rid]
            run["build"] = r.get("build") or run["build"]
            run["backend"] = r.get("backend") or run["backend"]
            run["screen"] = r.get("screen") or run["screen"]
            run["ms"] = max(run["ms"], r.get("ms") or 0)
            run["heap"] = max(run["heap"], r.get("heapMB") or 0)
            if r.get("main") and r["main"].get("wallMs"):
                run["main"].append(r["main"])
            for text in (r.get("perf") or []) + (r.get("tail") or []):
                m = PERF.search(str(text))
                if m:
                    c = CPUT.search(str(text))
                    cput = (int(c.group(1)), int(c.group(2)), int(c.group(3)), int(c.group(4))) if c else None
                    run["perf"][int(m.group(3))] = (float(m.group(1)), int(m.group(2)), int(m.group(4)), cput)
    print(f"{'run':<9}{'machine':<8}{'build':<15}{'bk':<5}{'n':>4}{'secs':>5}{'speed%':>8}{'fps':>7}"
          f"{'gpu%':>6}{'mbox%':>7}{'lag%':>6}{'Mt/s':>8}{'heap':>6}")
    shown = 0
    for rid in reversed(order):
        run = runs[rid]
        if a.ua and run["ua"] != a.ua:
            continue
        ticks = sorted(run["perf"])
        if len(ticks) < 4:
            continue
        rows = [run["perf"][t] for t in ticks][2:]
        speed = statistics.median(r[1] for r in rows)
        fps = statistics.median(r[0] for r in rows)
        gpu = statistics.median(r[2] for r in rows)
        steps = [b - a_ for a_, b in zip(ticks[2:], ticks[3:]) if b > a_]
        rate = statistics.median(steps) / 1e6 if steps else float("nan")
        mains = run["main"][2:] or run["main"]
        mbox = statistics.median(100.0 * m["mailboxMs"] / m["wallMs"] for m in mains) if mains else float("nan")
        lag = statistics.median(100.0 * m["lagMs"] / m["wallMs"] for m in mains) if mains else float("nan")
        cput = [r[3] for r in rows if r[3]]
        cpus = ""
        if cput:
            cpus = (f"  cpu: events {statistics.median(c[0] for c in cput):.0f}%"
                    f" throttle {statistics.median(c[1] for c in cput):.0f}%"
                    f" gpuwait {statistics.median(c[2] for c in cput):.0f}%"
                    f" ({statistics.median(c[3] for c in cput):.0f}/s)")
        print(f"{rid:<9}{run['ua']:<8}{run['build'][:14]:<15}{run['backend'][:4]:<5}{len(rows):>4}"
              f"{run['ms'] / 1000:>5.0f}{speed:>8.0f}{fps:>7.1f}{gpu:>6.0f}{mbox:>7.1f}{lag:>6.1f}"
              f"{rate:>8.1f}{run['heap']:>6.0f}{cpus}")
        shown += 1
        if shown >= a.last:
            break


if __name__ == "__main__":
    main()
