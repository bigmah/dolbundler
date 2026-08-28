# Dolphin, in WebAssembly, running recompiled guest code

The wasm sibling of `ios/`. Both targets forbid a JIT, so both take the same
shape: DolRecomp turns the disc's PowerPC into ordinary C on a Mac, that C is
compiled and **linked into the binary before it ships**, and Dolphin's
`StaticRecomp` CPU core jumps into it instead of interpreting. The only thing
that differs is the compiler at the end and the window at the front.

```
  main.dol ──▶ dolrecomp ──▶ chunks/*.c ──▶ emcc ──▶  one .wasm  ──▶ Safari
                (Mac)                                  + Dolphin
```

## Building

```sh
./build.sh --node          # the measurement build: NODERAWFS, no canvas
./build.sh                 # the browser build
./build.sh --no-module     # interpreter only, for a matched baseline
./build.sh --lto --ipo     # the slow, fully optimised link
```

The recompiled module is supplied by path and never enters the repository:

```sh
./ModernGekko/build/dolrecomp --gamecube --backend c -j14 \
    build-wasm/gexe52/sys/main.dol build-wasm/gexe52-c
cp build-wasm/gexe52/sys/main.dol build-wasm/gexe52-c/generated/main.dol
DOLWEB_MODULES='GEXE52=/abs/path/build-wasm/gexe52-c/generated' ./build.sh --node
```

`build.sh` also applies the Emscripten patches for the two Externals that are
nested git repositories (SFML, zlib-ng), which is where a fix would otherwise be
invisible to this repo's history.

## Measuring

```sh
./run-node.sh 90            # 90 s of Disney skate, Null backend
```

`[perf]` lines carry the number that matters -- **speed**, the fraction of real
time the guest is keeping up with. Read speed and not fps: a GameCube game's own
frame rate varies by scene, so fps alone says nothing about whether emulation is
keeping up.

The shutdown line carries the other one:

```
StaticRecomp: shutdown. native_dispatches=... fallback_steps=...
```

With no JIT in this build, `fallback_steps` is the *plain interpreter*, which
costs about 16x native. Coverage is therefore a speed cliff and not a rounding
error, and a title that looks fine on a desktop -- where the fallback is a JIT --
can be far worse here.

## Running it in a browser

```sh
./make-manifest.py ../../../build-wasm/gexe52 > ../../../build-wasm/gexe52/.manifest
./stage-sys.sh ../../../build-wasm/sys GEXE52
python3 ../serve.py dolphin
```

The disc is mounted over HTTP with WASMFS's fetch backend, so nothing is
downloaded up front: the boot touches a few tens of megabytes and the movies
stream. Two things about that are not obvious and are load-bearing:

- **WASMFS, not emscripten's JS filesystem.** The JS one is per-worker JS state,
  so a file created on one thread is invisible to the thread Dolphin reads the
  disc on. WASMFS keeps its state in linear memory, which every thread shares.
- **A fetch directory cannot be listed.** It only knows the children something
  inserted, so the tree comes from `.manifest`, which `make-manifest.py` writes.

The page must be cross-origin isolated (`serve.py` sends COOP/COEP) or there is
no `SharedArrayBuffer` and the emulator cannot start its threads.
