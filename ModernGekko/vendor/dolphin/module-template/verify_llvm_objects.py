#!/usr/bin/env python3
"""Inventory and validate undefined symbols in DolRecomp LLVM objects."""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys


GENERATED = re.compile(r"func_[0-9A-Fa-f]{8}(?:_budget)?$")
JOURNAL = {"g_mem_write_journal", "g_mem_write_journal_user"}
PROFILE_RUNTIME = {"__llvm_profile_instrument_target", "__llvm_profile_runtime"}


def symbols(nm: str, paths: list[pathlib.Path]) -> tuple[set[str], set[str]]:
    # One inventory process matters for a full title: GEXE52 has thousands of
    # objects, and launching nm twice per object turns a configure-time safety
    # check into minutes of process overhead.
    command = [nm, "-g", *(str(path) for path in paths)]
    result = subprocess.run(command, check=False, text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(
            f"{nm} -g <{len(paths)} objects>: {result.stderr.strip()}")
    defined: set[str] = set()
    undefined: set[str] = set()
    for line in result.stdout.splitlines():
        fields = line.split()
        if not fields or line.endswith(":"):
            continue
        name = fields[-1].removeprefix("_")
        if len(fields) >= 2 and fields[-2].upper() == "U":
            undefined.add(name)
        else:
            defined.add(name)
    return defined, undefined


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--nm", required=True)
    parser.add_argument("--cpu-header", type=pathlib.Path, required=True)
    parser.add_argument("--allow-profile-runtime", action="store_true")
    parser.add_argument("objects", nargs="+", type=pathlib.Path)
    args = parser.parse_args()

    cpu_text = args.cpu_header.read_text(encoding="utf-8")
    gx_helpers = set(re.findall(r"\b(ppc_[A-Za-z0-9_]+)\s*\(", cpu_text))
    try:
        defined, undefined = symbols(args.nm, args.objects)
    except RuntimeError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    categories: dict[str, list[str]] = {
        "generated cross-chunk": [],
        "GXRuntime CPU helper": [],
        "memory journal": [],
        "LLVM profile runtime": [],
        "unexpected": [],
    }
    for name in sorted(undefined):
        if GENERATED.fullmatch(name) and name in defined:
            categories["generated cross-chunk"].append(name)
        elif name in gx_helpers or name in {
            "dolrecomp_native_gate", "dolrecomp_native_gate_allows"
        }:
            categories["GXRuntime CPU helper"].append(name)
        elif name in JOURNAL:
            categories["memory journal"].append(name)
        elif args.allow_profile_runtime and name in PROFILE_RUNTIME:
            categories["LLVM profile runtime"].append(name)
        else:
            categories["unexpected"].append(name)

    print("DolRecomp LLVM undefined-symbol inventory:")
    for category, names in categories.items():
        print(f"  {category}: {len(names)}")
        limit = 8 if category == "generated cross-chunk" else len(names)
        for name in names[:limit]:
            print(f"    {name}")
        if len(names) > limit:
            print(f"    ... {len(names) - limit} more (all resolved in manifest)")
    if categories["unexpected"]:
        print("error: unexpected undefined symbols in generated objects",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
