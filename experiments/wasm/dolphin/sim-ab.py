#!/usr/bin/env python3
"""Interleaved A/B of two builds in the iOS Simulator's Safari, from a savestate.

    ./sim-ab.py snap-c64 snap-c64-c1                 # 2 rounds, Null, oglplay.sav
    ./sim-ab.py A B --rounds 3 --backend OGL --state ollie.sav

The simulator runs the phone's JavaScriptCore, and from a gameplay state it
reads within 1% of the device for the CPU half (OVER-THE-LINE.md 0j). Each run
is one sim-run.sh; the number read back is guest ticks per perf sample for that
run, from reports.jsonl, exactly as state-rate.py computes it. Arms alternate
A/B/A/B so the machine's drift lands on both; quote the median and the spread.
"""
import argparse, json, os, re, statistics, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPORTS = os.path.join(HERE, "..", "reports.jsonl")
TICKS = re.compile(r"ticks=(\d+)")


def run_rate(run_id):
    ticks = set()
    with open(REPORTS, "rb") as f:
        size = f.seek(0, 2)
        f.seek(max(0, size - 40_000_000))
        f.readline()
        for line in f:
            try:
                r = json.loads(line)
            except Exception:
                continue
            if r.get("run") != run_id:
                continue
            for text in (r.get("perf") or []) + (r.get("tail") or []):
                m = TICKS.search(str(text))
                if m:
                    ticks.add(int(m.group(1)))
    run = sorted(ticks)
    steps = [b - a for a, b in zip(run[2:], run[3:]) if b > a]
    if not steps:
        return float("nan"), len(run)
    return statistics.median(steps) / 1e6, len(run)


def new_run_id(before):
    """The run this arm produced: a run id that first appears after `before`
    and comes from the simulator (its UA says iPhone).

    Not "the last run id in the file": a stale page -- a headless Chrome the
    probe harness left behind, a tab from yesterday -- keeps posting its last
    perf lines every five seconds, and the first version of this took whichever
    of them happened to post last. Both arms then read the same stale run to
    the decimal (973.0, 124 samples, twice), which is how it was caught.
    """
    with open(REPORTS) as f:
        lines = f.readlines()
    seen = set()
    for line in lines[:before]:
        try:
            r = json.loads(line)
        except Exception:
            continue
        if r.get("run"):
            seen.add(r["run"])
    for line in lines[before:]:
        try:
            r = json.loads(line)
        except Exception:
            continue
        rid = r.get("run")
        if rid and rid not in seen and "iPhone" in (r.get("ua") or ""):
            return rid
    return None


def run_once(build, backend, seconds, extra, out):
    subprocess.run([os.path.join(HERE, "use-build.sh"), build], check=True,
                   stdout=subprocess.DEVNULL)
    before = sum(1 for _ in open(REPORTS)) if os.path.exists(REPORTS) else 0
    env = dict(os.environ, SIM_OUT=out)
    subprocess.run([os.path.join(HERE, "sim-run.sh"), "--backend", backend,
                    "--seconds", str(seconds), "--shot-every", "30",
                    "--extra", extra], env=env,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    rid = new_run_id(before)
    if not rid:
        return float("nan"), 0
    return run_rate(rid)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--rounds", type=int, default=2)
    ap.add_argument("--backend", default="Null")
    ap.add_argument("--seconds", type=int, default=75)
    ap.add_argument("--state", default="oglplay.sav")
    ap.add_argument("--extra", default="")
    # Per-arm extras, for an A/B of one build against itself under a knob:
    #   ./sim-ab.py snap-c64-h4 snap-c64-h4 --extra-b env=MODERNGEKKO_NO_GATHER_FAST=1
    ap.add_argument("--extra-a", default="")
    ap.add_argument("--extra-b", default="")
    ap.add_argument("--out", default=os.path.join(HERE, "sim-shots-ab"))
    args = ap.parse_args()
    extra = (f"env=DOLWEB_STATE=/game/{args.state}&env=DOLWEB_CPU_THREAD=0"
             f"&env=MODERNGEKKO_EMULATION_SPEED=0")
    if args.extra:
        extra += "&" + args.extra
    arms = [(args.a, extra + (("&" + args.extra_a) if args.extra_a else ""), args.a + ("+A" if args.extra_a else "")),
            (args.b, extra + (("&" + args.extra_b) if args.extra_b else ""), args.b + ("+B" if args.extra_b else ""))]
    results = {arms[0][2]: [], arms[1][2]: []}
    for r in range(args.rounds):
        for build, arm_extra, label in arms:
            rate, n = run_once(build, args.backend, args.seconds, arm_extra, args.out)
            results[label].append(rate)
            print(f"round {r + 1} {label:<18} {rate:8.1f} M ticks/sample  samples={n}",
                  flush=True)
    for build, rates in results.items():
        good = [x for x in rates if x == x]
        if good:
            print(f"{build:<16} median {statistics.median(good):8.1f}  "
                  f"range {min(good):.1f}-{max(good):.1f}  n={len(good)}")
    a = [x for x in results[arms[0][2]] if x == x]
    b = [x for x in results[arms[1][2]] if x == x]
    if a and b:
        print(f"{arms[1][2]} / {arms[0][2]} = {statistics.median(b) / statistics.median(a):.3f}"
              f"  (arms {'overlap' if max(a) >= min(b) and max(b) >= min(a) else 'do not overlap'})")


if __name__ == "__main__":
    main()
