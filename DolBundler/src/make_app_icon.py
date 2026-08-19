#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Draw DolBundler's own app icon: a disc with an arrow dropping into it.

Everything is rendered at 1024 and box-downsampled to each icon size, so the
downsample doubles as the antialiasing pass.
"""

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from make_game_app import ICONSET_SIZES, MASTER, box_downsample, new_image, put, write_png

TOP = (58, 74, 116)
BOTTOM = (22, 26, 40)
DISC = (120, 172, 255)
DISC_DIM = (74, 112, 178)
ARROW = (236, 243, 255)
RADIUS = 0.22  # corner radius as a fraction of the canvas


def rounded_alpha(x, y, size, radius):
    """Coverage of a rounded square at a point, 0.0 to 1.0."""
    dx = max(radius - x, x - (size - radius), 0.0)
    dy = max(radius - y, y - (size - radius), 0.0)
    if dx == 0.0 or dy == 0.0:
        inside = x >= 0 and y >= 0 and x <= size and y <= size
        return 1.0 if inside else 0.0
    distance = (dx * dx + dy * dy) ** 0.5
    return max(0.0, min(1.0, radius - distance + 0.5))


def blend(base, colour, alpha):
    return tuple(int(base[i] * (1 - alpha) + colour[i] * alpha) for i in range(3))


def draw():
    size = MASTER
    radius = size * RADIUS
    canvas = new_image(size, size, (0, 0, 0))

    cx = size / 2
    disc_cy = size * 0.63
    outer = size * 0.235
    inner = size * 0.072
    ring = size * 0.052

    # Arrow: a shaft plus a triangular head, pointing down into the disc.
    shaft_half = size * 0.045
    shaft_top = size * 0.145
    shaft_bottom = size * 0.315
    head_top = shaft_bottom
    head_tip = size * 0.435
    head_half = size * 0.125

    for py in range(size):
        y = py + 0.5
        # Vertical gradient for the plate.
        t = y / size
        plate = tuple(int(TOP[i] * (1 - t) + BOTTOM[i] * t) for i in range(3))
        for px in range(size):
            x = px + 0.5
            coverage = rounded_alpha(x, y, size, radius)
            if coverage <= 0.0:
                put(canvas, px, py, (0, 0, 0))
                continue
            colour = plate

            distance = ((x - cx) ** 2 + (y - disc_cy) ** 2) ** 0.5
            if distance <= outer:
                # A lit ring, darker toward the middle, with a centre hole.
                if distance >= inner:
                    edge = min(1.0, outer - distance)
                    shade = DISC if distance > outer - ring else DISC_DIM
                    colour = blend(colour, shade, edge)
                else:
                    colour = blend(colour, BOTTOM, min(1.0, inner - distance))

            in_shaft = shaft_top <= y <= shaft_bottom and abs(x - cx) <= shaft_half
            in_head = False
            if head_top <= y <= head_tip:
                span = head_half * (1 - (y - head_top) / (head_tip - head_top))
                in_head = abs(x - cx) <= span
            if in_shaft or in_head:
                colour = ARROW

            put(canvas, px, py, blend((0, 0, 0), colour, coverage))
    return canvas


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    destination = Path(args.out)
    destination.parent.mkdir(parents=True, exist_ok=True)
    master = draw()

    iconset = destination.parent / "dolbundler.iconset"
    shutil.rmtree(iconset, ignore_errors=True)
    iconset.mkdir(parents=True)
    cache = {}
    for name, size in ICONSET_SIZES:
        if size not in cache:
            cache[size] = master if size == MASTER else box_downsample(master, size)
        write_png(cache[size], iconset / name)
    result = subprocess.run(
        ["iconutil", "-c", "icns", str(iconset), "-o", str(destination)],
        capture_output=True,
    )
    shutil.rmtree(iconset, ignore_errors=True)
    if result.returncode != 0:
        sys.exit(result.stderr.decode(errors="replace"))
    print(destination)


if __name__ == "__main__":
    main()
