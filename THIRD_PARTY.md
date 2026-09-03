# Third-party code

Everything DolBundler builds on, where it comes from, and under what license.
None of it is copied into this repository: each project is a git submodule,
pinned to a commit on a fork that carries the upstream's full history, so the
authorship of every line is in `git log` and a change made here can be offered
back. The license texts live in the projects themselves, at the paths given.

| Project | What it is here | Path | Upstream, and the fork pinned here | License |
|---|---|---|---|---|
| ModernGekko | the runtime around the recompiled code: module loading, the runner, the tools that port a disc | `ModernGekko/` | [ExpansionPak/ModernGekko](https://github.com/ExpansionPak/ModernGekko), pinned via [bigmah/ModernGekko](https://github.com/bigmah/ModernGekko) | GPL-3.0-or-later — `ModernGekko/LICENSE` |
| RecompCore | the Dolphin fork the recompiled code runs inside: memory, devices, GX, audio, input | `ModernGekko/vendor/dolphin/` | [aharonahdoot/RecompCore](https://github.com/aharonahdoot/RecompCore), continued as [ExpansionPak/RecompCore](https://github.com/ExpansionPak/RecompCore), pinned via [bigmah/RecompCore](https://github.com/bigmah/RecompCore) | Dolphin's: mostly GPL-2.0-or-later, GPLv3-compatible in aggregate — `ModernGekko/vendor/dolphin/COPYING`, per-file SPDX tags, texts in `LICENSES/` |
| DolRecomp | the recompiler: PowerPC to C or LLVM IR | `ModernGekko/vendor/dolphin/DolRecomp/` | [ExpansionPak/DolRecomp](https://github.com/ExpansionPak/DolRecomp), pinned via [bigmah/DolRecomp](https://github.com/bigmah/DolRecomp) | GPL-3.0-or-later — `ModernGekko/vendor/dolphin/DolRecomp/LICENSE` |
| gc_controller | the driver for the Nintendo Switch Online GameCube controller | `DolBundler/vendor/gc_controller/` | [bigmah/nso_gc_macos](https://github.com/bigmah/nso_gc_macos) | none declared yet; it is this project's author's own code and needs a license file before release |

The forks are on GitHub under `bigmah`, each on a `dolbundler` branch. What
this project has added to each is `git log upstream/<branch>..dolbundler` in
that checkout; `DolBundler/forks.sh upstream` prints the counts.

## Carried inside those projects

Pinned by the projects above rather than by this repository:

- **GXRuntime** (`ModernGekko/vendor/dolphin/GXRuntime/`) — a standalone
  runtime for recompiled games, by Tomoeko and aharonahdoot, part of the
  RecompCore tree. GPL-3.0-or-later. It vendors `graphics/aurora`, a fork of
  [encounter/aurora](https://github.com/encounter/aurora) (MIT); see its own
  `THIRD_PARTY.md`.
- **StrikersRecomp** and **module-template** (`ModernGekko/vendor/dolphin/`) —
  the example port and the packaging template that ship with RecompCore.
- **{fmt}** (`ModernGekko/vendor/fmt/`, MIT), **picojson**
  (`ModernGekko/vendor/picojson/`, BSD-2-Clause) and **dolphin_legacy**
  (`ModernGekko/vendor/dolphin_legacy/`, a subset of Dolphin, GPL-2.0-or-later
  with per-file SPDX tags) — copies that ModernGekko carries upstream.
- **Dolphin's externals** (`ModernGekko/vendor/dolphin/Externals/`) — SDL,
  curl, fmt, imgui, zlib-ng, zstd, lz4, mGBA, cubeb, enet, glslang,
  SPIRV-Cross, the Vulkan headers, libusb, hidapi, pugixml, rcheevos and about
  fifteen more. Each is a git submodule on its real upstream, pinned by
  RecompCore's `.gitmodules`, used unmodified, with its license file in its own
  directory. Dolphin ships the same set and its `COPYING` states the aggregate
  is GPLv3-compatible. The Qt and FFmpeg entries are Windows-only prebuilt
  binaries and are not part of any build this project makes.
- **Rust crates** — `DolBundler/gui` is a Dioxus application; its dependencies
  are fetched from crates.io at build time as declared in `Cargo.toml` and
  pinned by `Cargo.lock`, each under its own license (`cargo tree` lists them).

## This project's own code

`DolBundler/` and `ios/` are DolBundler's own and are GPL-3.0-or-later, see
[`LICENSE`](LICENSE).

## What is not here

No Nintendo code, no SDK, no disc image, no extracted game data and no keys.
Every path in this repository assumes you supply your own dump of a disc you
own; see the [legal notice](README.md#legal).
