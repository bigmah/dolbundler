#!/usr/bin/env python3
"""The browser main thread, per run: what the proxied GL work costs it.

Reads the `main` field dolweb-page.js puts in every report -- time inside the
wasm export that runs proxied work (`mailbox`), the delay a task waits for the
thread (`lag`), and with ?glprobe=1 the WebGL calls themselves (`gl`) -- and
prints each as a percentage of wall time, median over the run's intervals.

  ./main-thread.py --ua phone --last 6
"""
import argparse, json, os, statistics

REPORTS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "reports.jsonl")


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
    ap.add_argument("--ua", default="")
    ap.add_argument("--last", type=int, default=8)
    ap.add_argument("--tail-bytes", type=int, default=40_000_000)
    a = ap.parse_args()
    runs = {}
    order = []
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
            m = r.get("main")
            if not rid or not m or not m.get("wallMs"):
                continue
            if rid not in runs:
                runs[rid] = {"ua": ua_label(r.get("ua", "")), "backend": r.get("backend", "?"),
                             "build": str(r.get("build", "?")), "rows": []}
                order.append(rid)
            runs[rid]["rows"].append(m)
    print(f"{'machine':<11}{'backend':<9}{'build':<16}{'n':>4}{'mailbox%':>10}{'/s':>7}{'lag%':>7}{'gl%':>7}{'gl/s':>8}")
    shown = 0
    for rid in reversed(order):
        run = runs[rid]
        if a.ua and run["ua"] != a.ua:
            continue
        rows = run["rows"][2:] or run["rows"]
        if not rows:
            continue
        pct = lambda k: statistics.median(100.0 * r.get(k, 0) / r["wallMs"] for r in rows)
        per_s = lambda k: statistics.median(1000.0 * r.get(k, 0) / r["wallMs"] for r in rows)
        gl = pct("glMs") if any("glMs" in r for r in rows) else float("nan")
        gls = per_s("glCalls") if any("glCalls" in r for r in rows) else float("nan")
        print(f"{run['ua']:<11}{run['backend']:<9}{run['build']:<16}{len(rows):>4}"
              f"{pct('mailboxMs'):>10.1f}{per_s('mailboxCalls'):>7.0f}{pct('lagMs'):>7.1f}"
              f"{gl:>7.1f}{gls:>8.0f}")
        shown += 1
        if shown >= a.last:
            break


if __name__ == "__main__":
    main()
