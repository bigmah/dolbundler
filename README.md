# DolBundler

Drop a GameCube or Wii disc image in, watch it recompile, and play it from the
library. The disc's PowerPC code is statically recompiled to native machine
code — the game runs as compiled code, not as interpreted instructions.

```
mario_party_4.iso  ──▶  DolBundler  ──▶  ▶ Mario Party 4   in the library
                                    └──▶  ~/Applications/Mario Party 4.app
                                          (opt in, per game)
```

Recompiling adds the game to DolBundler's library, and that is enough to play
it. Turning one into a standalone `.app` in `~/Applications` is a separate,
per-game step — press **Create App**, or pass `--app` on the command line.

DolBundler is the glue layer. The heavy lifting belongs to two upstream
projects it drives:

| Piece | Role | Upstream |
|---|---|---|
| `DolRecomp` | reads the disc's `main.dol`, decodes PowerPC, emits C | [ExpansionPak/DolRecomp](https://github.com/ExpansionPak/DolRecomp) |
| `ModernGekko` | the runtime: GX/Metal video, audio, input, memory, disc I/O | [ExpansionPak/ModernGekko](https://github.com/ExpansionPak/ModernGekko) |
| `DolBundler/` | the Dioxus window, the pipeline, and the macOS app packaging | this repo |

## Legal

**This project ships no game data and never will.** No disc image, no
extracted assets, no keys, no Nintendo code. Bring your own dump of a disc you
own; every path in this repo assumes you supply it yourself. Disc images are
`.gitignore`d at the repo root so they cannot be committed by accident.

DolBundler is a build tool. It compiles code you already own into a program
you run locally. Nothing it produces is redistributable, and the generated
apps hold no game data — each one points at your own extracted disc.

## Quick start

```sh
git clone --recursive https://github.com/<you>/recomp_gc.git
cd recomp_gc
./DolBundler/build.sh
```

If you forgot `--recursive`, `build.sh` initialises the submodule for you.

The first run fetches ModernGekko's vendored RecompCore/Dolphin tree (a few
hundred MB), builds it, builds the window, and installs `DolBundler.app` to
`~/Applications`. A cold build compiles all of Dolphin, so expect it to take a
while; later runs reuse the build directory.

Requirements: Xcode command line tools, `cmake`, `ninja`, `python3`, `cargo`.
Apple Silicon and Intel are both fine.

Full usage, the four pipeline steps, and the known limitations are in
[`DolBundler/README.md`](DolBundler/README.md).

## How the dependencies are wired

`ModernGekko` is a **pinned git submodule** at the repo root. It in turn pins
RecompCore (a Dolphin fork), which pins DolRecomp:

```
recomp_gc/
  ModernGekko/                  submodule → ExpansionPak/ModernGekko
    vendor/dolphin/             submodule → ExpansionPak/RecompCore
      DolRecomp/                submodule → ExpansionPak/DolRecomp
```

**DolRecomp is not a direct dependency of this repo.** It is built from
`ModernGekko/vendor/dolphin/DolRecomp` through that chain. If you need to hack
on the recompiler, that is the tree to edit — a separate top-level checkout
would compile into nothing.

Pinning is deliberate. `DolBundler/patches/` carries three fixes written
against a specific ModernGekko revision, and `build.sh` refuses to guess if
they no longer apply. To move to a newer upstream:

```sh
git -C ModernGekko fetch origin
git -C ModernGekko checkout <new-sha>
./DolBundler/build.sh              # will fail loudly if a patch no longer applies
./DolBundler/src/check_game_patches.py
git add ModernGekko && git commit
```

### The patches

Three fixes live in `DolBundler/patches/` as ordinary diffs, applied
idempotently by `build.sh`. All three are candidates for upstreaming rather
than carrying here forever:

- `0001-accept-unpinned-discs` — an unbranded ModernGekko build pins no disc ID
  and ships no disc preparer, so `PrepareDisc()` falls into a branch that
  rejects every image. Without this, no disc can be added at all.
- `0002-dol-patch-widths-and-conditionals` — the DOL patcher only parsed
  Dolphin's 32-bit `dword` form, so it refused to build any game whose INI used
  the `byte`, `word`, or four-field conditional forms. That is 21 of the 127
  games that ship enabled patches.
- `0003-list-controllers` — adds `moderngekko-run --list-controllers`, printing
  the SDL gamepads Dolphin's input backend will see. DolBundler's per-game
  controller picker needs SDL's own name for a pad to write a working profile,
  and the runner is the only thing in the tree that both links SDL and can be
  asked without opening a window.

## Licensing

DolBundler is **GPL-3.0-or-later**. See [`LICENSE`](LICENSE).

Both upstreams are GPL-3.0-or-later, and `DolBundler/patches/` contains
modified source from ModernGekko's `tools/`, which makes those files derivative
works of a GPL program. Licensing the whole repo the same way keeps the
boundary from being something anyone has to argue about.

ModernGekko's own provenance — its Dolphin and RecompCore lineage, and the
per-file SPDX identifiers that remain authoritative for that code — is
documented in [`ModernGekko/PROVENANCE.md`](ModernGekko/PROVENANCE.md).

## Credits

DolBundler exists because other people did the hard part. `DolRecomp` and
`ModernGekko` are by the [ExpansionPak](https://github.com/ExpansionPak)
project; ModernGekko builds on RecompCore by SpecialK / aharonahdoot, which
builds on the Dolphin emulator. Their credits are in their own repos and worth
reading.

## Contributing

Issues and pull requests welcome. Two things to know first:

- Bugs in recompilation or in the runtime belong upstream, in
  [DolRecomp](https://github.com/ExpansionPak/DolRecomp) or
  [ModernGekko](https://github.com/ExpansionPak/ModernGekko) — not here. This
  repo is the macOS packaging and the pipeline that drives them.
- Never attach a disc image, an extracted DOL, or game assets to an issue. A
  disc ID and the failing address are enough.
