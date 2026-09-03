# DolBundler

Drop a GameCube or Wii disc image in, watch it recompile, and play it from the
library. The disc's PowerPC code is statically recompiled to native machine
code; the game runs as compiled code, not interpreted.

```
mario_party_4.iso  ──▶  DolBundler  ──▶  ▶ Mario Party 4   in the library
                                    ├──▶  ~/Applications/Mario Party 4.app   (opt in)
                                    └──▶  📱 an iPhone                       (opt in)
```

Two backends produce the native code: `c` (PowerPC → C → arm64, the default
and the reference) and `llvm` (DolIR → objects, in process). **iPhone**
recompiles the library for iOS, links every game into the phone app, signs and
installs it; nothing is compiled on the phone. Every game is profiled on the
Mac first, and a title that has had a deeper look keeps its result under
[`DolBundler/tuning/`](DolBundler/tuning/README.md).

## Quick start

```sh
git clone --recursive https://github.com/bigmah/dolbundler.git
cd dolbundler
./DolBundler/build.sh
```

Requirements: Xcode command line tools, `cmake`, `ninja`, `python3`, `cargo`.
The first build compiles all of Dolphin and takes a while. Usage, the pipeline
steps, the iPhone flow and the known limitations are in
[`DolBundler/README.md`](DolBundler/README.md).

## What is in here

DolBundler is the glue. The heavy lifting is three upstream projects, each a
fork pinned as a submodule, one inside the next:

| Path | What | Fork of |
|---|---|---|
| `DolBundler/` | the Dioxus window, the pipeline, the app packaging | this repo |
| `ModernGekko/` | the runtime: module loading, the runner, the porting tools | [ExpansionPak/ModernGekko](https://github.com/ExpansionPak/ModernGekko) |
| `ModernGekko/vendor/dolphin/` | RecompCore, the Dolphin fork the recompiled code runs inside | [ExpansionPak/RecompCore](https://github.com/ExpansionPak/RecompCore) |
| `ModernGekko/vendor/dolphin/DolRecomp/` | the recompiler: PowerPC to C or LLVM IR | [ExpansionPak/DolRecomp](https://github.com/ExpansionPak/DolRecomp) |
| `DolBundler/vendor/gc_controller/` | driver for the Switch Online GameCube pad | [bigmah/nso_gc_macos](https://github.com/bigmah/nso_gc_macos) |

The forks (`bigmah/ModernGekko`, `bigmah/RecompCore`, `bigmah/DolRecomp`,
branch `dolbundler`) carry their upstream's full history plus this project's
changes. Edit them in place; `DolBundler/forks.sh` pushes through the chain:

```sh
./DolBundler/forks.sh status     # branch, unpushed commits, dirty files, pins
./DolBundler/forks.sh push       # push each fork, deepest first, re-pin above it
./DolBundler/forks.sh upstream   # drift from each upstream
./DolBundler/forks.sh checkout   # back onto the branches after a submodule update
```

[`THIRD_PARTY.md`](THIRD_PARTY.md) lists every piece with its license.

## Legal

**No game data ships here, ever.** No disc image, extracted asset, key or
Nintendo code; bring your own dump of a disc you own. Disc images are
`.gitignore`d. The per-title records under `DolBundler/tuning/` are
measurements (counters, timings, addresses, a hash of the DOL), never bytes of
a game.

## License

GPL-3.0-or-later, see [`LICENSE`](LICENSE). Every upstream is GPL as well;
[`THIRD_PARTY.md`](THIRD_PARTY.md) has the details.

## Credits

DolRecomp and ModernGekko are by [ExpansionPak](https://github.com/ExpansionPak).
RecompCore is by SpecialK / aharonahdoot, continued by ExpansionPak, with
GXRuntime and StrikersRecomp by Tomoeko and aharonahdoot. All of it stands on
the Dolphin emulator.

## Contributing

Issues and pull requests welcome. Fixes to the recompiler or the runtime go in
the forks; if one is not DolBundler-specific, offer it upstream too. Never
attach a disc image, a DOL or game assets to an issue: a disc ID and the
failing address are enough.
