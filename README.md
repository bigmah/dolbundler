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

Every game is profiled on the Mac before it is compiled for the phone; a title
that has had a deeper look carries its result in the repository under
[`DolBundler/tuning/`](DolBundler/tuning/README.md), and the next send of it,
on any Mac, starts from there.

DolBundler is the glue layer. The heavy lifting is done by three projects it
drives, each a fork of its upstream, checked out here as a submodule:

| Piece | Role | Path | Forked from |
|---|---|---|---|
| `DolRecomp` | reads the disc's `main.dol`, decodes PowerPC, emits C or LLVM IR | `ModernGekko/vendor/dolphin/DolRecomp/` | [ExpansionPak/DolRecomp](https://github.com/ExpansionPak/DolRecomp) |
| `RecompCore` | the Dolphin fork the recompiled code runs inside: memory, devices, GX, audio | `ModernGekko/vendor/dolphin/` | [ExpansionPak/RecompCore](https://github.com/ExpansionPak/RecompCore), from [aharonahdoot/RecompCore](https://github.com/aharonahdoot/RecompCore) |
| `ModernGekko` | the runtime around it: module loading, the runner, the tools that port a disc | `ModernGekko/` | [ExpansionPak/ModernGekko](https://github.com/ExpansionPak/ModernGekko) |
| `DolBundler` | the Dioxus window, the pipeline, and the macOS app packaging | `DolBundler/` | this repo |

## Legal

**This project ships no game data and never will.** No disc image, no
extracted assets, no keys, no Nintendo code. Bring your own dump of a disc you
own; every path in this repo assumes you supply it yourself. Disc images are
`.gitignore`d at the repo root so they cannot be committed by accident.

DolBundler is a build tool. It compiles code you already own into a program
you run locally. Nothing it produces is redistributable, and the generated
apps hold no game data — each one points at your own extracted disc.

That holds for what the tools *write back* into this repo, too.
[`DolBundler/tuning/`](DolBundler/tuning/README.md) commits a per-title record
so a later build starts from what an earlier one measured, and those records
are measurements about a game and never any part of one: counters, timings,
guest addresses, a hash of your DOL, and the SDK routine names that Dolphin's
signature database already publishes. No disassembly, no bytes out of the DOL,
no screenshots. `tunegame` keeps it that way on its own — a report's
disassembly stays in the working store under Application Support, and the
committed copy points at `tunegame disasm`, which reads it back off the disc
you already own. Hold anything you add to the same line.

## Quick start

```sh
git clone --recursive https://github.com/bigmah/dolbundler.git
cd dolbundler
./DolBundler/build.sh
```

If you forgot `--recursive`, `build.sh` fetches the submodules for you.

The recursive clone brings the three forks with their history and Dolphin's
third-party externals at depth 1 (a few hundred MB together). The first run
builds the externals, builds the window, and installs `DolBundler.app` to
`~/Applications`. A cold build compiles all of Dolphin, so expect it to take a
while; later runs reuse the build directory.

Requirements: Xcode command line tools, `cmake`, `ninja`, `python3`, `cargo`.
Apple Silicon and Intel are both fine.

Full usage, the four pipeline steps, and the known limitations are in
[`DolBundler/README.md`](DolBundler/README.md).

## How the dependencies are wired

ModernGekko, RecompCore and DolRecomp are **forks, pinned as submodules**, one
inside the next, at the same paths their upstreams use:

```
dolbundler/
  DolBundler/                   the window, the pipeline, the app packaging
    vendor/gc_controller/       submodule: bigmah/nso_gc_macos
  ModernGekko/                  submodule: bigmah/ModernGekko, forked from ExpansionPak/ModernGekko
    vendor/dolphin/             submodule: bigmah/RecompCore, forked from ExpansionPak/RecompCore
      DolRecomp/                submodule: bigmah/DolRecomp, forked from ExpansionPak/DolRecomp
      Externals/                Dolphin's third-party deps, submodules on their real upstreams
```

Each fork's `dolbundler` branch is its upstream's full history plus what this
project has added, so `git log upstream/master..` inside any of them lists
exactly that, and a fix made here can go back upstream as an ordinary pull
request. [`THIRD_PARTY.md`](THIRD_PARTY.md) lists every piece, its license,
and where it came from.

Edit the runtime or the recompiler in place: `ModernGekko/` and
`ModernGekko/vendor/dolphin/DolRecomp/` are ordinary checkouts on their
`dolbundler` branches. A change is committed at the level it lives, and each
level above it then pins the new commit. `DolBundler/forks.sh` does the
bookkeeping:

```sh
./DolBundler/forks.sh status     # branch, unpushed commits, dirty files and pins, at every level
./DolBundler/forks.sh push       # push each fork, deepest first, and re-pin each level above it
./DolBundler/forks.sh upstream   # how far each fork has drifted from its upstream
./DolBundler/forks.sh checkout   # put every level back on its branch after a submodule update
```

The submodule URLs are `https://` so that anyone can clone. To push over SSH
with a specific key, rewrite them once in your global git config:

```sh
git config --global url."git@github.com:bigmah/".insteadOf "https://github.com/bigmah/"
```

**DolRecomp lives at `ModernGekko/vendor/dolphin/DolRecomp`.** It is built
through ModernGekko's CMake, so that is the tree to edit — a separate top-level
checkout would compile into nothing.

After changing the recompiler or the runtime, rebuild and re-check the game
patch coverage:

```sh
./DolBundler/build.sh
./DolBundler/src/check_game_patches.py
```

### The fixes that used to be patches

Three fixes that `build.sh` used to apply from `DolBundler/patches/` at build
time are ordinary commits on the ModernGekko fork now, and all three are
candidates for upstream pull requests:

- **Accept unpinned discs** — an unbranded ModernGekko build pins no disc ID
  and ships no disc preparer, so `PrepareDisc()` fell into a branch that
  rejected every image. Without this, no disc can be added at all.
- **DOL patch widths and conditionals** — the DOL patcher only parsed
  Dolphin's 32-bit `dword` form, so it refused to build any game whose INI used
  the `byte`, `word`, or four-field conditional forms. That is 21 of the 127
  games that ship enabled patches.
- **`moderngekko-run --list-controllers`** — prints the SDL gamepads Dolphin's
  input backend will see. DolBundler's per-game controller picker needs SDL's
  own name for a pad to write a working profile, and the runner is the only
  thing in the tree that both links SDL and can be asked without opening a
  window.

## Licensing

DolBundler is **GPL-3.0-or-later**. See [`LICENSE`](LICENSE).

Every upstream is GPL: ModernGekko and DolRecomp are GPL-3.0-or-later, and
RecompCore is Dolphin, GPL-2.0-or-later with GPLv3 parts, GPLv3-compatible as
a whole. DolBundler drives them, links against them, and embeds the runtime in
its iOS app, so licensing the whole repo GPL-3.0-or-later keeps the boundary
from being something anyone has to argue about.

[`THIRD_PARTY.md`](THIRD_PARTY.md) lists each piece with its license and
origin. ModernGekko's own provenance — its Dolphin and RecompCore lineage, and
the per-file SPDX identifiers that remain authoritative for that code — is in
[`ModernGekko/PROVENANCE.md`](ModernGekko/PROVENANCE.md).

## Credits

DolBundler exists because other people did the hard part. `DolRecomp` and
`ModernGekko` are by the [ExpansionPak](https://github.com/ExpansionPak)
project. ModernGekko runs on RecompCore by SpecialK / aharonahdoot, continued
by ExpansionPak, which carries GXRuntime and StrikersRecomp by Tomoeko and
aharonahdoot, and all of it stands on the Dolphin emulator. Their credits are
in their own repos and worth reading.

## Contributing

Issues and pull requests welcome. Two things to know first:

- Bugs in recompilation or in the runtime are fixed in the forks, checked out
  here under `ModernGekko/vendor/dolphin/DolRecomp/` and `ModernGekko/`. Their
  `dolbundler` branches sit on the upstream history, so a fix that is not
  DolBundler-specific is worth offering to
  [DolRecomp](https://github.com/ExpansionPak/DolRecomp) or
  [ModernGekko](https://github.com/ExpansionPak/ModernGekko) as well.
- Never attach a disc image, an extracted DOL, or game assets to an issue. A
  disc ID and the failing address are enough.
