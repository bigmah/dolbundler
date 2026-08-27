#!/usr/bin/env python3
"""Wrap assembled PowerPC into a GameCube DOL that DolRecomp will accept.

The benchmark kernels in bench.s are ordinary Gekko code; DolRecomp reads DOLs,
not object files, so this puts a 0x100-byte DOL header in front of the .text
bytes. One text section at 0x80003100, which is also the entry point.

Ships no game data: the input is bench.s, assembled here.
"""
import struct
import sys

TEXT_ADDR = 0x80003100
FILE_OFF = 0x100


def main(text_path: str, out_path: str) -> int:
    text = open(text_path, "rb").read()
    header = bytearray(FILE_OFF)

    def w32(off: int, value: int) -> None:
        struct.pack_into(">I", header, off, value)

    w32(0x00, FILE_OFF)       # text[0] file offset
    w32(0x48, TEXT_ADDR)      # text[0] load address
    w32(0x90, len(text))      # text[0] size
    w32(0xD8, 0x80100000)     # bss address
    w32(0xDC, 0x1000)         # bss size
    w32(0xE0, TEXT_ADDR)      # entry point

    open(out_path, "wb").write(bytes(header) + text)
    print(f"{out_path}: {FILE_OFF + len(text)} bytes, entry 0x{TEXT_ADDR:08X}")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("usage: make_dol.py <text.bin> <out.dol>", file=sys.stderr)
        raise SystemExit(2)
    raise SystemExit(main(sys.argv[1], sys.argv[2]))
