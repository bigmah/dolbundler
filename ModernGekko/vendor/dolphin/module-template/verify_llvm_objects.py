#!/usr/bin/env python3
"""Inventory and validate undefined symbols in DolRecomp LLVM objects."""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys


JOURNAL = {"g_mem_write_journal", "g_mem_write_journal_user"}
PROFILE_RUNTIME = {"__llvm_profile_instrument_target", "__llvm_profile_runtime"}


def symbols(nm: str, paths: list[pathlib.Path]) -> tuple[set[str], set[str]]:
    # Inventory in sizeable batches. One nm per object turns a full title into
    # minutes of process overhead, while putting every object on one command
    # line exceeds macOS ARG_MAX on larger games (Melee has 15,000+ chunks).
    defined: set[str] = set()
    undefined: set[str] = set()
    batch_size = 512
    for first in range(0, len(paths), batch_size):
        batch = paths[first : first + batch_size]
        command = [nm, "-g", *(str(path) for path in batch)]
        result = subprocess.run(command, check=False, text=True, capture_output=True)
        if result.returncode:
            raise RuntimeError(
                f"{nm} -g <objects {first + 1}..{first + len(batch)}>: "
                f"{result.stderr.strip()}")
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


# wasm objects reference these three by name and wasm-ld supplies them; they are
# not symbols a module is reaching for. Listing them rather than pattern-matching
# on "starts with an underscore" keeps the audit's whole point intact: anything
# else undefined is still a module asking the chassis for something.
WASM_LINKER_GLOBALS = frozenset({
    "__indirect_function_table",
    "_indirect_function_table",
    "__memory_base",
    "_memory_base",
    "__stack_pointer",
    "_stack_pointer",
    "__table_base",
    "_table_base",
})

# The one libm entry point the paired-single lowering emits. The C backend gets
# it from the same libm; on a target where the compiler open-codes an FMA it
# does not appear at all.
LIBM = frozenset({"fma", "fmaf"})


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--nm", required=True)
    parser.add_argument("--cpu-header", type=pathlib.Path, required=True)
    parser.add_argument("--symbol-prefix", default="")
    parser.add_argument("--allow-profile-runtime", action="store_true")
    parser.add_argument("--object-list", type=pathlib.Path)
    parser.add_argument("objects", nargs="*", type=pathlib.Path)
    args = parser.parse_args()
    if args.object_list:
        args.objects.extend(
            pathlib.Path(line)
            for line in args.object_list.read_text(encoding="utf-8").splitlines()
            if line
        )
    if not args.objects:
        parser.error("provide objects or --object-list")
    generated = re.compile(
        re.escape(args.symbol_prefix) + r"func_[0-9A-Fa-f]{8}(?:_budget)?$")

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
        "wasm linker global": [],
        "libm": [],
        "unexpected": [],
    }
    for name in sorted(undefined):
        if generated.fullmatch(name) and name in defined:
            categories["generated cross-chunk"].append(name)
        elif name in gx_helpers or name in {
            args.symbol_prefix + "dolrecomp_native_gate",
            args.symbol_prefix + "dolrecomp_native_gate_allows",
        }:
            categories["GXRuntime CPU helper"].append(name)
        elif name in JOURNAL:
            categories["memory journal"].append(name)
        elif args.allow_profile_runtime and name in PROFILE_RUNTIME:
            categories["LLVM profile runtime"].append(name)
        elif name in WASM_LINKER_GLOBALS:
            categories["wasm linker global"].append(name)
        elif name in LIBM:
            categories["libm"].append(name)
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
