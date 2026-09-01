#!/usr/bin/env python3
"""Interleaved A/B of two builds from the gameplay savestate, in headless Chrome.

    ./ab-state.py web128 web128-tc            # 3 rounds, OGL
    ./ab-state.py web128 web128-tc --rounds 4 --backend Null

Each run reports guest ticks per perf sample (the same statistic state-rate.py
reads out of reports.jsonl, computed here from the driver's own [perf] lines so
a run is attributed to the build that produced it), and the deterministic
dispatch rate from the shutdown line. Arms alternate A/B/A/B so drift lands on
both; quote the median and the spread, never a single pair.
"""
import argparse, os, re, statistics, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
STATE_ENV = ("env=DOLWEB_STATE=/game/oglplay.sav&env=DOLWEB_CPU_THREAD=0"
             "&env=MODERNGEKKO_EMULATION_SPEED=0")


def run_once(build, backend, seconds, extra):
    subprocess.run([os.path.join(HERE, "use-build.sh"), build], check=True,
                   stdout=subprocess.DEVNULL)
    cmd = ["node", os.path.join(HERE, "drive-dolphin.mjs"), "--backend", backend,
           "--seconds", str(seconds), "--shot", "/dev/null",
           "--extra", STATE_ENV + (("&" + extra) if extra else "")]
    out = subprocess.run(cmd, capture_output=True, text=True).stdout
    ticks = []
    for m in re.finditer(r"\[perf\].*ticks=(\d+)", out):
        t = int(m.group(1))
        if not ticks or t != ticks[-1]:
            ticks.append(t)
    steps = [b - a for a, b in zip(ticks[2:], ticks[3:]) if b > a]
    rate = statistics.median(steps) / 1e6 if steps else float("nan")
    m = re.search(r"shutdown: native=(\d+).*?cycles=(\d+)", out)
    disp = (int(m.group(1)) * 1000.0 / int(m.group(2))) if m else float("nan")
    smc = re.search(r"smc_failed=(\d+)", out)
    return rate, disp, len(ticks), (smc.group(1) if smc else "?"), out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--backend", default="OGL")
    ap.add_argument("--seconds", type=int, default=55)
    ap.add_argument("--extra", default="")
    ap.add_argument("--log", default="")
    args = ap.parse_args()

    results = {args.a: [], args.b: []}
    for r in range(args.rounds):
        for build in (args.a, args.b):
            rate, disp, n, smc, out = run_once(build, args.backend, args.seconds, args.extra)
            results[build].append(rate)
            print(f"round {r + 1} {build:<14} {rate:8.1f} M ticks/sample   "
                  f"{disp:6.2f} dispatches/1000 cycles   samples={n} smc_failed={smc}",
                  flush=True)
            if args.log:
                with open(args.log, "a") as f:
                    f.write(f"=== {build} round {r + 1}\n{out}\n")
    for build in (args.a, args.b):
        v = results[build]
        print(f"{build:<14} median {statistics.median(v):8.1f}  "
              f"min {min(v):8.1f}  max {max(v):8.1f}  "
              f"spread {100 * (max(v) - min(v)) / statistics.median(v):.1f}%")
    ma, mb = statistics.median(results[args.a]), statistics.median(results[args.b])
    print(f"{args.b} / {args.a} = {mb / ma:.4f}  ({100 * (mb / ma - 1):+.1f}%)")
    lo_b, hi_a = min(results[args.b]), max(results[args.a])
    print("arms " + ("do NOT overlap" if lo_b > hi_a or max(results[args.b]) < min(results[args.a])
                     else "OVERLAP"))


if __name__ == "__main__":
    main()
