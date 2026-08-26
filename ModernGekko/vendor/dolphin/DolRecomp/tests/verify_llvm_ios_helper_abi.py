#!/usr/bin/env python3
"""Compare LLVM-emitter helper attributes with Xcode Clang's iOS C ABI."""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import tempfile


def declaration(ir: str, name: str) -> str:
    match = re.search(rf"^declare .*@{re.escape(name)}\([^\n]+", ir, re.MULTILINE)
    if not match:
        raise RuntimeError(f"missing declaration for {name}")
    return match.group(0)


def abi_shape(line: str) -> tuple[bool, tuple[tuple[str, bool], ...]]:
    before, parameters = line.split("(", 1)
    return_zeroext = "zeroext" in before
    shaped = []
    for parameter in parameters.rsplit(")", 1)[0].split(","):
        tokens = parameter.strip().split()
        base = next((token for token in tokens if token in {"ptr", "i1", "i8", "i16", "i32", "i64", "double"}), "")
        if base:
            shaped.append((base, "zeroext" in tokens))
    return return_zeroext, tuple(shaped)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--emitter-ir", type=pathlib.Path, required=True)
    parser.add_argument("--fixture", type=pathlib.Path, required=True)
    parser.add_argument("--gxruntime-include", type=pathlib.Path, required=True)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="dolllvm-abi-") as directory:
        clang_ir = pathlib.Path(directory) / "clang.ll"
        subprocess.run(
            ["xcrun", "--sdk", "iphoneos", "clang", "-target",
             "arm64-apple-ios17.0", "-S", "-emit-llvm", "-O0",
             "-I", str(args.gxruntime_include), str(args.fixture),
             "-o", str(clang_ir)],
            check=True,
        )
        expected = clang_ir.read_text(encoding="utf-8")
    actual = args.emitter_ir.read_text(encoding="utf-8")

    compared = (
        "ppc_fcmp",
        "ppc_cache_control",
        "ppc_fp_available",
        "ppc_fma",
        "ppc_psq_load",
        "dolrecomp_native_gate_allows",
    )
    for name in compared:
        expected_line = declaration(expected, name)
        actual_line = declaration(actual, name)
        if abi_shape(actual_line) != abi_shape(expected_line):
            raise RuntimeError(
                f"iOS ABI mismatch for {name}:\n  clang:  {expected_line}\n"
                f"  emitter: {actual_line}")
    print("iPhoneOS helper ABI matches Xcode Clang for: " + ", ".join(compared))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
