#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Register one recompiled GameCube/Wii game, and optionally bundle it.

Two jobs, and only the first runs by default:

  * always - decode the disc's banner into cover art and upsert the game into
    library.json, so DolBundler's list can show and launch it.
  * with --app - additionally build a double-clickable macOS .app.  The bundle
    is a thin launcher holding no game data and no runtime, only absolute paths
    to the extracted disc, the recompiled module, and the ModernGekko build.

The .app is opt-in because a library entry is enough to play, and every bundle
adds a permanent icon to ~/Applications that the user has to clean up by hand.
"""

import argparse
import datetime
import json
import os
import plistlib
import shutil
import struct
import subprocess
import sys
import zlib
from pathlib import Path

BANNER_W, BANNER_H = 96, 32
MASTER = 1024
ICONSET_SIZES = [
    ("icon_16x16.png", 16),
    ("icon_16x16@2x.png", 32),
    ("icon_32x32.png", 32),
    ("icon_32x32@2x.png", 64),
    ("icon_128x128.png", 128),
    ("icon_128x128@2x.png", 256),
    ("icon_256x256.png", 256),
    ("icon_256x256@2x.png", 512),
    ("icon_512x512.png", 512),
    ("icon_512x512@2x.png", 1024),
]
BACKDROP = (28, 28, 30)


# --- image helpers -----------------------------------------------------------
# An image is (width, height, bytearray of RGB triples).

def new_image(w, h, colour):
    return (w, h, bytearray(bytes(colour) * (w * h)))


def put(img, x, y, rgb):
    w, _, data = img
    i = (y * w + x) * 3
    data[i:i + 3] = bytes(rgb)


def get(img, x, y):
    w, _, data = img
    i = (y * w + x) * 3
    return data[i], data[i + 1], data[i + 2]


def write_png(img, path):
    w, h, data = img
    raw = bytearray()
    for y in range(h):
        raw.append(0)  # filter type 0
        raw += data[y * w * 3:(y + 1) * w * 3]

    def chunk(tag, payload):
        out = struct.pack(">I", len(payload)) + tag + payload
        return out + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + chunk(b"IEND", b"")
    )


def box_downsample(img, size):
    """Exact integer box filter. MASTER is divisible by every icon size."""
    w, h, _ = img
    assert w == h and w % size == 0
    factor = w // size
    out = new_image(size, size, (0, 0, 0))
    area = factor * factor
    for y in range(size):
        for x in range(size):
            r = g = b = 0
            for dy in range(factor):
                for dx in range(factor):
                    pr, pg, pb = get(img, x * factor + dx, y * factor + dy)
                    r += pr
                    g += pg
                    b += pb
            put(out, x, y, (r // area, g // area, b // area))
    return out


# --- banner decoding ---------------------------------------------------------

def rgb5a3(value):
    """GameCube RGB5A3: high bit set means opaque RGB555, else A3RGB444."""
    if value & 0x8000:
        r = (value >> 10) & 0x1F
        g = (value >> 5) & 0x1F
        b = value & 0x1F
        return (r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2)
    a = (value >> 12) & 0x7
    r = (value >> 8) & 0xF
    g = (value >> 4) & 0xF
    b = value & 0xF
    alpha = a / 7.0
    # Composite onto the backdrop now; the icon has no alpha channel.
    return tuple(
        int(((c << 4) | c) * alpha + BACKDROP[i] * (1 - alpha))
        for i, c in enumerate((r, g, b))
    )


def decode_banner(path):
    """Decode a BNR1/BNR2 96x32 RGB5A3 banner stored as 4x4 tiles."""
    try:
        data = path.read_bytes()
    except OSError:
        return None
    if len(data) < 0x20 + BANNER_W * BANNER_H * 2 or data[:3] != b"BNR":
        return None
    img = new_image(BANNER_W, BANNER_H, BACKDROP)
    offset = 0x20
    for tile_y in range(0, BANNER_H, 4):
        for tile_x in range(0, BANNER_W, 4):
            for row in range(4):
                for col in range(4):
                    (value,) = struct.unpack_from(">H", data, offset)
                    offset += 2
                    put(img, tile_x + col, tile_y + row, rgb5a3(value))
    return img


def fallback_art():
    """No banner: draw a plain disc so the icon still reads as a game."""
    img = new_image(BANNER_W, BANNER_H, BACKDROP)
    cx, cy, outer, inner = BANNER_W / 2, BANNER_H / 2, 15.0, 3.5
    for y in range(BANNER_H):
        for x in range(BANNER_W):
            d = ((x + 0.5 - cx) ** 2 + (y + 0.5 - cy) ** 2) ** 0.5
            if inner <= d <= outer:
                shade = int(90 + 90 * (1 - (d - inner) / (outer - inner)))
                put(img, x, y, (shade, shade, min(255, shade + 40)))
    return img


def build_master(art):
    """Upscale the banner with nearest-neighbour and centre it on the canvas."""
    scale = (MASTER * 7 // 8) // BANNER_W  # fills the canvas, keeps a margin
    aw, ah = BANNER_W * scale, BANNER_H * scale
    ox, oy = (MASTER - aw) // 2, (MASTER - ah) // 2
    canvas = new_image(MASTER, MASTER, BACKDROP)
    for y in range(ah):
        for x in range(aw):
            put(canvas, ox + x, oy + y, get(art, x // scale, y // scale))
    return canvas


def build_icns(art, destination):
    master = build_master(art)
    iconset = destination.parent / "icon.iconset"
    shutil.rmtree(iconset, ignore_errors=True)
    iconset.mkdir(parents=True)
    cache = {}
    for name, size in ICONSET_SIZES:
        if size not in cache:
            cache[size] = master if size == MASTER else box_downsample(master, size)
        write_png(cache[size], iconset / name)
    ok = subprocess.run(
        ["iconutil", "-c", "icns", str(iconset), "-o", str(destination)],
        capture_output=True,
    ).returncode == 0
    shutil.rmtree(iconset, ignore_errors=True)
    return ok


def write_cover(art, path, scale=4):
    """Wide banner art for the library list, upscaled nearest-neighbour."""
    w, h = BANNER_W * scale, BANNER_H * scale
    out = new_image(w, h, BACKDROP)
    for y in range(h):
        for x in range(w):
            put(out, x, y, get(art, x // scale, y // scale))
    path.parent.mkdir(parents=True, exist_ok=True)
    write_png(out, path)


def existing_app(path, disc_id):
    """The .app path a previous run recorded for this disc, or ""."""
    try:
        library = json.loads(Path(path).read_text())
    except (OSError, ValueError):
        return ""
    for game in library.get("games", []):
        if isinstance(game, dict) and game.get("disc_id") == disc_id:
            app = game.get("app", "")
            return app if app and Path(app).is_dir() else ""
    return ""


def upsert_library(path, entry):
    """Replace any entry with the same disc ID, newest first."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        library = json.loads(path.read_text())
        games = [g for g in library.get("games", []) if isinstance(g, dict)]
    except (OSError, ValueError):
        games = []
    games = [g for g in games if g.get("disc_id") != entry["disc_id"]]
    games.insert(0, entry)
    path.write_text(json.dumps({"games": games}, indent=2) + "\n")


# --- bundle ------------------------------------------------------------------

LAUNCHER = """#!/bin/bash
# Generated by DolBundler. Edit RUN_ARGS below to change how this game starts.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
source "$here/../Resources/game.conf"

fail() {{
  /usr/bin/osascript -e "display alert \\"$GAME_TITLE\\" message \\"$1\\" as critical" \\
    >/dev/null 2>&1
  exit 1
}}

[ -x "$RUNTIME_DIR/moderngekko-run" ] || \\
  fail "The ModernGekko runtime is missing from $RUNTIME_DIR. Rebuild it, then drop the disc image on DolBundler again."
[ -d "$GAME_ROOT" ] || \\
  fail "The extracted game files are missing from $GAME_ROOT. Drop the disc image on DolBundler again."
[ -f "$MODULE_PATH" ] || \\
  fail "The recompiled module is missing from $MODULE_PATH. Drop the disc image on DolBundler again to rebuild it."

mkdir -p "$USER_DIR/Logs"
exec "$RUNTIME_DIR/moderngekko-run" \\
  --game "$GAME_ROOT" \\
  --module "$MODULE_PATH" \\
  --user-dir "$USER_DIR" \\
  --title "$GAME_TITLE" \\
  --graphics "$GRAPHICS_BACKEND" \\
  >>"$USER_DIR/Logs/{disc_id}.log" 2>&1
"""


def shell_quote(value):
    return "'" + str(value).replace("'", "'\\''") + "'"


def build_bundle(args, art):
    """Write <out-dir>/<name>.app and return its path."""
    bundle = Path(args.out_dir) / f"{args.name}.app"
    shutil.rmtree(bundle, ignore_errors=True)
    macos = bundle / "Contents" / "MacOS"
    resources = bundle / "Contents" / "Resources"
    macos.mkdir(parents=True)
    resources.mkdir(parents=True)

    info = {
        "CFBundleName": args.name,
        "CFBundleDisplayName": args.name,
        "CFBundleExecutable": "run",
        "CFBundleIdentifier": f"gc.dolbundler.game.{args.disc_id}",
        "CFBundleIconFile": "icon",
        "CFBundleInfoDictionaryVersion": "6.0",
        "CFBundlePackageType": "APPL",
        "CFBundleShortVersionString": "1.0",
        "CFBundleVersion": "1",
        "LSApplicationCategoryType": "public.app-category.games",
        "LSMinimumSystemVersion": "13.0",
        "NSHighResolutionCapable": True,
        "NSSupportsAutomaticGraphicsSwitching": False,
    }
    (bundle / "Contents" / "Info.plist").write_bytes(plistlib.dumps(info))

    conf = "\n".join(
        f"{key}={shell_quote(value)}"
        for key, value in [
            ("GAME_TITLE", args.name),
            ("DISC_ID", args.disc_id),
            ("GAME_ROOT", args.game_root),
            ("MODULE_PATH", args.module),
            ("RUNTIME_DIR", args.runtime),
            ("USER_DIR", args.user_dir),
            ("GRAPHICS_BACKEND", args.graphics),
        ]
    )
    (resources / "game.conf").write_text(conf + "\n")

    launcher = macos / "run"
    launcher.write_text(LAUNCHER.format(disc_id=args.disc_id))
    launcher.chmod(0o755)

    if not build_icns(art, resources / "icon.icns"):
        print("warning: could not build an icon; the app will use the generic one",
              file=sys.stderr)

    # Finder caches bundle metadata aggressively; touching the bundle and
    # re-registering it makes a rebuilt app pick up its new name and icon.
    os.utime(bundle)
    subprocess.run(
        ["/System/Library/Frameworks/CoreServices.framework/Frameworks/"
         "LaunchServices.framework/Support/lsregister", "-f", str(bundle)],
        capture_output=True,
    )
    return bundle


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--name", required=True)
    parser.add_argument("--disc-id", required=True)
    parser.add_argument("--game-root", required=True)
    parser.add_argument("--module", required=True)
    parser.add_argument("--runtime", required=True)
    parser.add_argument("--user-dir", required=True)
    parser.add_argument("--graphics", default="Metal")
    parser.add_argument("--platform", default="")
    parser.add_argument("--app", action="store_true",
                        help="also build a .app bundle in --out-dir")
    parser.add_argument("--out-dir", help="where --app writes the bundle")
    parser.add_argument("--library", help="library.json to upsert this game into")
    parser.add_argument("--covers", help="directory to write the GUI cover art into")
    parser.add_argument("--source-image", default="")
    args = parser.parse_args()

    if args.app and not args.out_dir:
        parser.error("--app needs --out-dir")

    banner = Path(args.game_root) / "files" / "opening.bnr"
    art = decode_banner(banner) if banner.is_file() else None
    art = art or fallback_art()

    bundle = build_bundle(args, art) if args.app else None

    cover = ""
    if args.covers:
        cover = str(Path(args.covers) / f"{args.disc_id}.png")
        write_cover(art, Path(cover))

    if args.library:
        # Preserve a bundle built by an earlier run: re-registering a game
        # without --app must not orphan an .app the user already asked for.
        previous = existing_app(args.library, args.disc_id)
        upsert_library(args.library, {
            "disc_id": args.disc_id,
            "name": args.name,
            "platform": args.platform,
            "game_root": args.game_root,
            "module": args.module,
            "app": str(bundle) if bundle else previous,
            "cover": cover,
            "source_image": args.source_image,
            "added": datetime.datetime.now(datetime.timezone.utc)
                     .replace(microsecond=0).isoformat(),
        })

    if bundle:
        print(bundle)


if __name__ == "__main__":
    main()
