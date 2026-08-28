#!/usr/bin/env python3
"""Turn a dolweb run log into the two numbers that decide anything.

Dolphin timestamps every log line with mm:ss:mmm of host time, and the guest
announces its own progress through OSREPORT. The interval between two OSREPORT
markers is therefore a fixed amount of *guest* work measured in host time,
which is what makes two builds comparable. The shutdown line carries coverage:
with no JIT in this build, a fallback step is the plain interpreter.
"""
import re
import sys

TS = re.compile(r"^(\d+):(\d+):(\d+)\s")


def stamp(line):
    m = TS.match(line)
    if not m:
        return None
    mm, ss, ms = (int(x) for x in m.groups())
    return mm * 60 + ss + ms / 1000.0


def main(path):
    marks = []
    shutdown = None
    attached = None
    for line in open(path, errors="replace"):
        if "[dolweb] disc" in line:
            attached = line.strip()
        if "StaticRecomp: shutdown" in line:
            shutdown = line.strip()
        t = stamp(line)
        if t is None:
            continue
        if "OSREPORT" in line:
            text = line.split("N[OSREPORT]:", 1)[-1].strip()
            if text:
                marks.append((t, text))

    if attached:
        print(attached)
    if not marks:
        print("no OSREPORT markers in the log")
        return 1

    first, last = marks[0], marks[-1]
    print(f"guest markers: {len(marks)}")
    print(f"  first  {first[1][:60]!r}")
    print(f"  last   {last[1][:60]!r}")
    print(f"  span   {last[0] - first[0]:.2f} s host time")

    # The interval the Dolphin-wasm experiment quoted: main() to the intro.
    def find(needle):
        for t, text in marks:
            if needle in text:
                return t
        return None

    a, b = find("Start of main()"), find("Bink:")
    if a is not None and b is not None:
        print(f"  main()->Bink  {b - a:.2f} s   (native 1.61, wasm interp 26.2)")
    if shutdown:
        print(shutdown.split("N[PowerPC]:", 1)[-1].strip())
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "build-wasm/run.log"))
