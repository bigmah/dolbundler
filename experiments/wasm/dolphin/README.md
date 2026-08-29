# Dolphin, in WebAssembly, running recompiled guest code

The wasm sibling of `ios/`. Both targets forbid a JIT, so both take the same
shape: DolRecomp turns the disc's PowerPC into ordinary C on a Mac, that C is
compiled and **linked into the binary before it ships**, and Dolphin's
`StaticRecomp` CPU core jumps into it instead of interpreting. The only things
that differ are the compiler at the end and the window at the front.

```
  main.dol ──▶ dolrecomp ──▶ chunks/*.c ──▶ emcc ──▶  one .wasm  ──▶ Safari
                (Mac)                                  + Dolphin
```

Disney's Extreme Skate Adventure plays here at **60 fps**, with sound, from a
disc that is never downloaded, in a page.

## Where it stands

| | |
|---|---|
| interpreter, same binary | **5%** of realtime |
| module, Null backend, unthrottled | 160-250% |
| module, OpenGL, gameplay, unthrottled | **101-123%**, steady 60 fps |
| macOS Safari | 85-92% |
| module | 86.5 MB of wasm, 256-instruction chunks |

**And on the phone** (iPhone 15 Pro Max, Safari, warm, matched guest window
45-90 s): **CPU only 157%, with rendering 40%.** The recompiled code has
headroom on the device; the renderer does not, and that is the opposite of what
this Mac measures. See `OVER-THE-LINE.md`.

## Building

```sh
./build.sh --node          # the measurement build: NODERAWFS, no canvas
./build.sh                 # the browser build
./build.sh --no-module     # interpreter only, for a matched baseline
./build.sh --lto --ipo     # the slow, fully optimised link (unmeasured)
```

The recompiled module is supplied by path and never enters the repository:

```sh
DOLRECOMP_C_CHUNK_INSTRUCTIONS=256 \
ModernGekko/build/vendor/dolphin/DolRecomp/dolrecomp --gamecube --backend c -j14 \
    build-wasm/gexe52/sys/main.dol build-wasm/gexe52-c256
cp build-wasm/gexe52/sys/main.dol build-wasm/gexe52-c256/generated/main.dol
DOLWEB_MODULES='GEXE52=/abs/path/build-wasm/gexe52-c256/generated' ./build.sh
```

**The chunk size is not a detail.** A C-backend chunk is one function entered
through a `switch (ctx->pc)` over every instruction in it, so the chunk size is
the size of that switch -- and it is the same lever that took GEXE52 from 71.8%
to 105-107% on arm64. The C default is 4096. Measured here in the node harness,
Null, unthrottled, over the same guest window (45-90 s), which is the only way
to compare two builds on the same scene:

| instructions per chunk | chunks | wasm | speed |
|---|---|---|---|
| 4096 (the default) | 118 | 91.5 MB | 193.6% |
| 512 | 928 | 87.6 MB | 209.6% |
| **256** | **1855** | **86.5 MB** | **218.4%** |
| 128 (the floor) | 3709 | 86.0 MB | 194.8% |

A curve with a minimum, not a free win: past 256 the calls that leave a chunk
and return through the dispatcher cost more than the smaller switch saves.
`./bench-node.py` is the measurement.

`build.sh` also applies the Emscripten patches for the two Externals that are
nested git repositories (SFML, zlib-ng), which is where a fix would otherwise be
invisible to this repo's history.

## Running it in a browser

```sh
./make-manifest.py ../../../build-wasm/gexe52 > ../../../build-wasm/gexe52/.manifest
./stage-sys.sh ../../../build-wasm/sys GEXE52
python3 ../serve.py dolphin              # localhost
python3 ../serve.py dolphin --lan        # reachable from a phone, over HTTPS
```

Then `http://127.0.0.1:8712/index.html`, or from a phone the printed HTTPS
address. Useful query parameters: `?backend=Null` to take the renderer out of a
measurement, `?seconds=N` to stop after a budget, `?auto=1` to start without a
click, `?report=1` to POST progress back to `serve.py`, `?pad=1` to force the
on-screen controls, `?env=NAME=VALUE` for any of the emulator's knobs.

**A LAN address over plain HTTP is not a secure context**, and
SharedArrayBuffer is a secure-context feature — without it the emulator cannot
start its threads at all. `--lan` serves HTTPS with a self-signed certificate
for exactly this reason; Safari warns once.

Two things about the disc are not obvious and are load-bearing:

- **WASMFS, not emscripten's JS filesystem.** The JS one is per-worker JS state,
  so a file created on one thread is invisible to the thread Dolphin reads the
  disc on. WASMFS keeps its state in linear memory, which every thread shares.
- **A fetch directory cannot be listed.** It only knows the children something
  inserted, so the tree comes from `.manifest`, which `make-manifest.py` writes.

## Measuring

```sh
./run-node.sh 90                              # 90 s, Null backend, in node
node drive-dolphin.mjs --seconds 300 --backend OGL --audio 1 --press 200
```

`[perf]` lines carry the number that matters — **speed**, the fraction of real
time the guest is keeping up with. Read speed and not fps: a GameCube game's own
frame rate varies by scene, so fps alone says nothing about whether emulation is
keeping up. And **take the throttle off** (`MODERNGEKKO_EMULATION_SPEED=0`) or a
build with headroom reads 100% exactly like a build with none.

Three instruments were each built to answer a question that could not be
answered by reading code, and all three are worth keeping:

- `DOLWEB_TIME_GL=1` wraps the GL calls that force a round trip to the GPU
  process and reports per frame. Nothing in the source of a WebGL backend looks
  expensive; this is how to find out which call is. It is what found
  `glBufferSubData` at 6 ms a call.
- `STATICRECOMP_FALLBACK_HISTOGRAM=1` says which instructions leave the
  recompiled code for the chassis. It is what turned "319 million fallbacks"
  into "one instruction in one loop".
- `STATICRECOMP_TRACE_FILE` with `STATICRECOMP_TRACE_EVERY=8` prints the guest
  PC every few dispatches, which is how a boot that stops is located.

`STATICRECOMP_FALLBACK_RANGES=80000000-90000000` forces everything to the
interpreter in the same binary, which is the only honest baseline.

**What is left to finish this is in `OVER-THE-LINE.md`** -- the device numbers,
the two open defects, and the habit that found today's.

## What is still open

- **No device number.** Everything here is a Mac.
- **Dual core (`DOLWEB_CPU_THREAD=1`) fails to initialise the video backend.**
  Worth having: it is what moves the FIFO and texture work off the CPU thread.
- **The module is 91.6 MB**, which is 95% of the binary. `-Os` is 6% smaller and
  no faster. The LLVM backend would likely be smaller and faster, and it is
  **closer than the note here used to say**: with the WebAssembly components
  linked into `dolrecomp` and `wasm32` accepted by `supportedTarget`, it
  recompiles the whole disc for `wasm32-unknown-emscripten` — 7417 objects, no
  errors. The codegen was never the problem.

  What is left is exactly the CPUState layout, and it is now precisely bounded.
  Compiling `core/native_state_layout.h` against the generated header with
  `emcc` names every field that disagrees, and it is only the pointer-bearing
  tail: `external_read`, `external_write`, `ram`, `ram_size`, `downcount`,
  `exram`, `exram_size` and the SPR/cache-control pointers. Everything before
  them is u32/f64/u8, so **the prefix is byte-identical on wasm32 and arm64** --
  which means the fix is a walk over the tail with a 4-byte pointer size rather
  than a model of the whole struct.

  Two things have to move together: `pipeline.c` emits the offsets into the
  header with the host's `offsetof`, and the LLVM backend bakes the same host
  offsets into the code it generates. Doing only the first would produce a
  module that *passes* the layout check with wrong offsets inside it, which is
  worse than one that fails. Until both are done the static asserts refuse a
  wasm32 module at compile time, which is the right failure.
- **Savestates are not portable to wasm32** for the same reason. A state taken
  on arm64 fails to load with a save-marker mismatch, which costs the
  same-scene measurement discipline `ios/PERFORMANCE.md` recommends.
