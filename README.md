# DolBundler

Drop a GameCube or Wii disc image in, watch it recompile, and play it from the
library. The disc's PowerPC code is statically recompiled to native machine
code — the game runs as compiled code, not as interpreted instructions.

```
mario_party_4.iso  ──▶  DolBundler  ──▶  ▶ Mario Party 4   in the library
                                    ├──▶  ~/Applications/Mario Party 4.app
                                    │     (opt in, per game)
                                    └──▶  📱 an iPhone
                                          (opt in, per game)
```

Recompiling adds the game to DolBundler's library, and that is enough to play
it. Turning one into a standalone `.app` in `~/Applications` is a separate,
per-game step — press **Create App**, or pass `--app` on the command line.

Two recompiler backends produce the native code: `c` goes PowerPC → C → arm64
through the host compiler and is the default and the reference, and `llvm` goes
straight from DolIR to objects in process. **Settings → Recompile discs to**
picks between them. See [`DolBundler/README.md`](DolBundler/README.md#recompiler).

**iPhone** in the top right recompiles the library a second time — for
`arm64-apple-ios17.0` — links every game into the phone app, signs it, installs
it, and copies the discs over. Nothing is compiled on the phone: iOS will not
map a page executable without a valid code signature behind it, so guest code
has to be inside the signature before the app is installed. See
[On an iPhone](DolBundler/README.md#on-an-iphone), and
[`ios/README.md`](ios/README.md) for what the phone app is.

DolBundler is the glue layer. The heavy lifting is done by two projects it
drives, both vendored directly into this repo:

| Piece | Role | Path | Originally from |
|---|---|---|---|
| `DolRecomp` | reads the disc's `main.dol`, decodes PowerPC, emits C or bytecode | `ModernGekko/vendor/dolphin/DolRecomp/` | [ExpansionPak/DolRecomp](https://github.com/ExpansionPak/DolRecomp) |
| `ModernGekko` | the runtime: GX/Metal video, audio, input, memory, disc I/O | `ModernGekko/` | [ExpansionPak/ModernGekko](https://github.com/ExpansionPak/ModernGekko) |
| `DolBundler` | the Dioxus window, the pipeline, and the macOS app packaging | `DolBundler/` | this repo |

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

If you forgot `--recursive`, `build.sh` fetches Dolphin's externals for you.

All the code is in the clone; the first run fetches only Dolphin's third-party
externals (a few hundred MB), builds them, builds the window, and installs
`DolBundler.app` to `~/Applications`. A cold build compiles all of Dolphin, so
expect it to take a while; later runs reuse the build directory.

Requirements: Xcode command line tools, `cmake`, `ninja`, `python3`, `cargo`.
Apple Silicon and Intel are both fine.

Full usage, the four pipeline steps, and the known limitations are in
[`DolBundler/README.md`](DolBundler/README.md).

## How the dependencies are wired

This is a **monorepo**. ModernGekko, RecompCore (a Dolphin fork) and DolRecomp
are ordinary tracked directories here — not submodules, not forks to keep in
sync:

```
recomp_gc/
  DolBundler/                   the window, the pipeline, the app packaging
  ModernGekko/                  the runtime
    vendor/dolphin/             RecompCore, a Dolphin fork
      DolRecomp/                the recompiler
      Externals/                Dolphin's third-party deps — still submodules
```

The only submodules left are Dolphin's own third-party externals under
`ModernGekko/vendor/dolphin/Externals` — Qt, SDL, curl, imgui and about thirty
more. Those point at their real upstreams, they are pinned, and `build.sh`
initialises them on the first run.

Edit the runtime or the recompiler directly in this tree and commit like any
other change. There is no second repo to push to and nothing to re-pin.

One further project is **optional** and not pinned at all:
[`gc_controller`](https://github.com/bigmah/nso_gc_macos), a driver for the
Nintendo Switch Online GameCube controller. `build.sh` picks up a checkout
sitting beside this one, or wherever `GC_CONTROLLER_DIR` points, and DolBundler
then offers that pad as a controller. Nothing here needs it; without one the
controller picker just offers SDL gamepads. See
[`DolBundler/README.md`](DolBundler/README.md#gamecube-controllers).

**DolRecomp lives at `ModernGekko/vendor/dolphin/DolRecomp`.** It is built
through ModernGekko's CMake, so that is the tree to edit — a separate top-level
checkout would compile into nothing.

After changing the recompiler or the runtime, rebuild and re-check the game
patch coverage:

```sh
./DolBundler/build.sh
./DolBundler/src/check_game_patches.py
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

- Bugs in recompilation or in the runtime are fixed in this repo too, under
  `ModernGekko/vendor/dolphin/DolRecomp/` and `ModernGekko/`. Those trees began
  as [DolRecomp](https://github.com/ExpansionPak/DolRecomp) and
  [ModernGekko](https://github.com/ExpansionPak/ModernGekko); this copy has
  diverged and is not kept in sync with them.
- Never attach a disc image, an extracted DOL, or game assets to an issue. A
  disc ID and the failing address are enough.
