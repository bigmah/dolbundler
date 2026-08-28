#!/usr/bin/env python3
"""List an extracted game root so a browser can mount it over HTTP.

A WASMFS fetch directory only knows the children something inserted into it, so
there is no readdir and no way for the emulator to discover the disc tree by
itself. This writes the list the page hands back.

    ./make-manifest.py build-wasm/gexe52 > build-wasm/gexe52/.manifest
"""
import os
import sys


def main(root):
    entries = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        for name in sorted(filenames):
            if name == ".manifest" or name == ".DS_Store":
                continue
            full = os.path.join(dirpath, name)
            entries.append(os.path.relpath(full, root))
    for e in entries:
        print(e)
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
