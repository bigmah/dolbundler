# Source provenance

## Absorbed into the DolBundler monorepo

`gc_controller` was an optional, separate checkout `build.sh` looked for beside
this repository. It is now vendored here as a flat snapshot, the same way
ModernGekko, RecompCore and DolRecomp were -- see `ModernGekko/PROVENANCE.md`.

| Tree | Path in the monorepo | Source | Revision |
|---|---|---|---|
| gc_controller | `DolBundler/vendor/gc_controller/` | `bigmah/nso_gc_macos` @ `main` | `dbbc1e203bd460c013b214737bf5faffd12bf87f` |

Absorbed on 2026-09-02, at a revision dated 2026-08-02. The source repository
retains the full commit history up to that revision; only the file contents
were copied here. This tree is not kept in sync with it.

## Why it is in-tree

The driver is what makes a Nintendo Switch Online GameCube controller usable at
all: the pad needs a proprietary handshake before it streams anything, so SDL
enumerates it and then sees nothing, which reads as a binding problem rather
than a missing driver. Carrying it beside the repo meant a clone that built
cleanly still could not drive the one controller the project most wants to
support, and said so only at the moment a game refused to start.

`build.sh` builds this copy in place, into `target/release/gc_controller` here,
and records the path in `toolchain.conf`. `GC_CONTROLLER_DIR` still overrides
it, so a checkout of your own is still what gets built if you point at one.
