#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Report Dolphin GameSettings patches that ModernGekko's DOL patcher rejects.

`moderngekko-port` bakes every enabled `[OnFrame]` patch into the DOL before
recompiling it, and refuses to build a game whose patch lines it cannot parse.
Run this after bumping the vendored Dolphin tree to see whether any newly
shipped INI uses a form the patcher does not handle yet.

    ./src/check_game_patches.py [path/to/Data/Sys/GameSettings]
"""

import collections
import pathlib
import sys

WIDTHS = {"byte", "word", "dword"}
DEFAULT_ROOT = (
    pathlib.Path(__file__).resolve().parents[2]
    / "ModernGekko/vendor/dolphin/Data/Sys/GameSettings"
)


def enabled_patch_names(lines):
    names, section = set(), ""
    for line in lines:
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1]
        elif section == "OnFrame_Enabled" and line.startswith("$"):
            names.add(line[1:])
    return names


def enabled_patch_lines(lines, enabled):
    section, name = "", ""
    for line in lines:
        if line.startswith("[") and line.endswith("]"):
            section, name = line[1:-1], ""
        elif section != "OnFrame":
            continue
        elif line.startswith("$"):
            name = line[1:]
        elif line and line[0] not in "#;" and name in enabled:
            yield line


def classify(line):
    """Mirror LoadDefaultDolPatches in tools/moderngekko_port.cpp."""
    parts = line.split(":")
    if len(parts) not in (3, 4):
        return "malformed", None
    if parts[1] not in WIDTHS:
        return "unknown width", parts[1]
    return ("conditional" if len(parts) == 4 else parts[1]), None


def main():
    root = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_ROOT
    if not root.is_dir():
        sys.exit(f"no GameSettings directory at {root}")

    supported = collections.Counter()
    rejected = collections.defaultdict(list)
    games = 0

    for ini in sorted(root.glob("*.ini")):
        lines = [line.strip() for line in ini.read_text(errors="replace").splitlines()]
        enabled = enabled_patch_names(lines)
        if not enabled:
            continue
        games += 1
        title = lines[0].lstrip("# ").strip() if lines and lines[0].startswith("#") else ini.stem
        for line in enabled_patch_lines(lines, enabled):
            kind, detail = classify(line)
            if kind in ("malformed", "unknown width"):
                rejected[f"{kind}{f' {detail!r}' if detail else ''}"].append((title, line))
            else:
                supported[kind] += 1

    print(f"{games} games ship enabled OnFrame patches")
    print("handled:", dict(supported) or "none")
    if not rejected:
        print("\nevery enabled patch line parses")
        return 0
    print()
    for kind, entries in sorted(rejected.items()):
        titles = sorted({title for title, _ in entries})
        print(f"{kind}: {len(entries)} line(s) across {len(titles)} game(s)")
        for title, line in entries:
            print(f"    {title}\n        {line}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
