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
# (--lto --ipo are the default now: +14% CPU, see OVER-THE-LINE.md 0f)
```

The recompiled module is supplied by path and never enters the repository:

```sh
DOLRECOMP_C_CHUNK_INSTRUCTIONS=64 DOLRECOMP_DIRECT_CALLS=1 DOLRECOMP_TAIL_CALLS=1 \
ModernGekko/build/vendor/dolphin/DolRecomp/dolrecomp --gamecube --backend c -j14 \
    build-wasm/gexe52/sys/main.dol build-wasm/gexe52-c64-tc
cp build-wasm/gexe52/sys/main.dol build-wasm/gexe52-c64-tc/generated/main.dol
DOLWEB_MODULES='GEXE52=/abs/path/build-wasm/gexe52-c64-tc/generated' ./build.sh
```

`DOLRECOMP_DIRECT_CALLS=1 DOLRECOMP_TAIL_CALLS=1` is not optional: without them
every transfer between chunks returns to the chassis. With them, and the chunks
compiled with `-mtail-call` (build.sh's default), a cross-chunk `b`, a `bctr` and
the fallthrough off a chunk's end are real wasm tail calls, and a `bctrl` is a
gated call through the module's own chunk table -- **9.05 -> 0.94 chassis
dispatches per 1000 guest cycles**. `./ab-state.py A B` is the interleaved A/B
from the savestate that measured it.

**64 instructions per chunk now, not 128** (2026-09-01): with cross-chunk
transfers as real tail calls the cost of a boundary fell, and 64 measured +2.0%
in Chrome and +1.3-3.4% under the simulator's JSC against 128, at 90.0 MB.
The table below is the older sweep, taken when every crossing was a chassis
dispatch; its reasoning about where the minimum sits no longer applies.

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

**That table is V8's answer, and Safari's is not the same one.** Simulator
Safari, renderer on, two runs per build, wall seconds to reach a fixed guest
second — the only comparison here that reproduces:

| instructions per chunk | guest 125 | guest 200 |
|---|---|---|
| 4096 | 160, 165 | 240, 245 |
| **256** | 130, 130 | 205, 205 |
| **128** | 130, 130 | 205, 205 |

**About 1.2x, and 256 against 128 is a tie** — where node puts 128 well behind
256. A 4096-instruction chunk is one `switch` in one enormous wasm function, and
JSC is less willing than V8 to optimise those, so 4096 is clearly the wrong end;
below that this harness cannot separate them. **Nothing tuned in the node harness
that changes the shape of the generated code should be believed for the device
until it is re-measured in Safari** — which is where this default sat at 4096
while this file documented 256.

**Do not try to quote a gameplay frame rate from an `?acts=` run.** The last
scripted press is at guest 160; after it the skater goes wherever physics takes
him, so equal guest windows are not equal scenes. Over guest 200-260 the same
128 build measured 52.4 fps and then 34.4, and all three chunk sizes overlap.
See `OVER-THE-LINE.md` section 0a for the three wrong answers this produced.

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
on-screen controls, `?env=NAME=VALUE` for any of the emulator's knobs,
`?acts=g25:5,...` for a guest-anchored timeline of button presses, and `?log=N`
for Dolphin's own log (1 the boot narrative, 3 the video backend, 5 a line per
DVD read). **`?log=` is not free and must not be left on while measuring** — see
"Measuring" below; `?report=1` used to imply it, and that is what made every
browser figure here incomparable with a node one.

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

**To compare two builds or two renderer settings, measure from a savestate.**
Booting spreads +/- 15 %; the same scene entered from a state spreads 0.5 %,
which is the difference between being able to rank a change and not. Capture
once, then reuse:

```sh
# capture: throttled, acts= drives the skater into the level, page uploads it
node drive-dolphin.mjs --port 8713 --backend OGL --seconds 300 \
  --extra "savestate=oglplay:235&env=MODERNGEKKO_SAVE_STATE_AFTER=235:/user/oglplay.sav&acts=$ACTS"

# measure: unthrottled, single core, from the state
node drive-dolphin.mjs --port 8713 --backend OGL --seconds 55 \
  --extra "env=DOLWEB_STATE=/game/oglplay.sav&env=DOLWEB_CPU_THREAD=0&env=MODERNGEKKO_EMULATION_SPEED=0"
```

Measure **in the level, not the menu** -- gameplay is twice the work and the two
disagree about results. Interleave the arms A/B/A/B rather than running three of
each, and quote the spread. Savestates under OGL only started working on
2026-09-01; see OVER-THE-LINE.md section 0h for why, and 0i for what the harness
then said about the renderer.

**On a phone, read the result with `./state-rate.py --ua phone`.** It prints guest
ticks per perf sample per run, and the renderer-on/off ratio when it sees both.
Do not read "% speed" for this: with the throttle on it caps at exactly 100 and
cannot see a difference at all. Runs are grouped by the `run` id the page mints
per load -- reports from before 2026-09-01 lack it and are skipped.

`--workerlog 1` makes `drive-dolphin.mjs` print the wasm stack when the emulator
traps. Without it a worker's crash reaches the page as a bare `ErrorEvent` with
no frames, and the build has no name section, so see 0h for the `build.ninja`
`--emit-symbol-map` trick that turns `$func17915` into a C++ name in minutes.

`[perf]` lines carry the number that matters — **speed**, the fraction of real
time the guest is keeping up with. Read speed and not fps: a GameCube game's own
frame rate varies by scene, so fps alone says nothing about whether emulation is
keeping up. And **take the throttle off** (`MODERNGEKKO_EMULATION_SPEED=0`) or a
build with headroom reads 100% exactly like a build with none.

**Do not turn the log on to measure.** `?log=N` gives Dolphin's own log; in the
browser it is not a diagnostic you can leave on, because a `printf` from the
emulation thread is a postMessage to the main thread and that is the thread
WASMFS proxies every disc read through. Desktop Safari, Null, over the same 150
wall seconds: **105.9% with it off, 45.7% with it on, and a second run with it on
stalled at guest second 1.1** and stopped posting reports at all. Until
2026-08-31 `?report=1` turned it on by itself, which is why nothing in
`OVER-THE-LINE.md` older than that is comparable with a node figure.

**`safari-run.sh`, `ab-safari.sh` and `reach.sh` drive the machine's own
Safari**, which means they open and close windows in whatever browser the person
at the keyboard is using. Fine on an idle Mac, rude on a busy one, and a
measurement taken beside a video is not a measurement anyway. `sim-run.sh` puts
the page in the iOS Simulator instead — the same WebKit, out of the way — and is
the one to reach for when the Mac is in use.

**The build directories left in `build-wasm/`, and what is in them.** They exist
so two builds can be compared without a rebuild between the readings, and the
names do not say what they hold:

| dir | module | notes |
|---|---|---|
| `web` | 4096-chunk | the old default; **stale**, keep only as the slow control |
| `web256` | 256-chunk | superseded by 128 |
| `web128` | 128-chunk | no LTO — the control for the LTO measurement |
| `web128-lto` | 128-chunk | **what `./build.sh` now produces**: LTO + IPO, +14% |
| `web128-deep` | 128-chunk | call depth 128 / loop budget 2048; unresolved, see 0g |
| `web128p` | 128-chunk | `--profiling-funcs`, for readable profiles only |

That is 5.3 GB. Delete all but `web128-lto` if you are not mid-comparison.
`./use-build.sh <name>` points the served page at one of them — note that
`use-build.sh web` serves the *4096* build, which is not what `./build.sh`
builds any more.

Two builds can be compared in the same browser without a rebuild between the
readings — `web/` holds symlinks for exactly that:

```sh
./use-build.sh web256      # point the served page at build-wasm/web256
./ab-safari.sh             # one Null/OpenGL pair, in the level, guest-anchored
./phone-window.py --machine safari-mac --from 125 --to 175
```

`ab-safari.sh` waits for the two `ab-result` rows rather than for a clock, so a
slow build is not silently truncated into a fast one's window.
`phone-window.py` does the same arithmetic for a plain `?report=1` session, which
is the better instrument on a phone: `?ab=`'s Null half is a black screen, and
that is the half someone closes before it finishes.

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

  **The layout is done.** `dolnative_target_layout()` walks the pointer-bearing
  tail for a given pointer size, `pipeline.c` publishes the target's offsets and
  hash, and the emitters read the same layout through `llvm_target_layout.h`, so
  the header cannot describe something the objects do not contain. The check that
  refused every previous wasm32 module now passes with zero assertion failures:

      #define DOLRECOMP_NATIVE_OFFSET_RAM       3428ull   (was 3456)
      #define DOLRECOMP_NATIVE_SIZE_RAM         4ull      (was 8)
      #define DOLRECOMP_NATIVE_OFFSET_DOWNCOUNT 3440ull   (was 3480)

  Two guards keep it honest. `dolnative_target_layout_matches_host()` checks the
  walker against the host compiler before any other pointer size is trusted, and
  emission refuses if it fails -- which it did immediately, because DolRecomp's
  `cpu.h` and GXRuntime's agree up to `exram_size` and then diverge (`u32
  spr[1024]` against `spr_read`/`spr_write`). The walker stops at `EXRAM_SIZE`
  now, where `DOLNATIVE_LAYOUT_FIELDS` stops.

  **Two more gates stood behind the layout, and both are the kind that read as
  a mystery if nobody wrote them down:**

  - The LLVM object symbol audit did not know wasm. A wasm object references
    `__indirect_function_table`, `__memory_base` and `__stack_pointer` by name
    and wasm-ld supplies them; `fma` is the one libm entry point the
    paired-single lowering emits. They are listed in `verify_llvm_objects.py`
    rather than pattern-matched, so anything else undefined is still caught.
  - **wasm objects carry their feature set**, and the link fails with
    `--shared-memory is disallowed by chunk_....o because it was not compiled
    with 'atomics' or 'bulk-memory' features`. The backend now emits
    `+atomics,+bulk-memory,+mutable-globals,+sign-ext` for a wasm triple. This
    is the same trap `build.sh` documents for the C backend, where `-pthread`
    has to reach every translation unit and not only the link.

  Worth knowing for the build shape: the LLVM path links objects directly
  instead of compiling 1855 C files, so the browser build is **1258 ninja
  targets against the C backend's 3113**.

  **Where it actually got to.** The module links, loads, and the chassis accepts
  it -- the layout hash the chassis computes for wasm32 matches the one
  `dolrecomp` published, which is the thing that was blocked:

      [staticrecomp] module identity: backend=llvm
                     target=wasm32-unknown-emscripten layout=4ea3df172a3fc06c
      [staticrecomp] module loaded: entry=0x80003100 code_ranges=3 smc_ranges=129

  Then it dies in the boot with `RangeError: Maximum call stack size exceeded`.
  That is the LLVM backend's whole shape showing up: it emits per-function entry
  points and *real calls* where the C backend returns through the dispatcher, so
  guest recursion becomes host-stack recursion.

  **A bigger stack is not the fix, and that is measured rather than assumed.**
  `-sSTACK_SIZE` is a cache variable now (`DOLWEB_STACK_SIZE`); 8 MB and 64 MB
  fail identically, and 256 MB fails differently and earlier -- `Aborted()`
  before the first tick, because a quarter-gigabyte stack out of a 512 MB
  `INITIAL_MEMORY` leaves nothing to start in. So the recursion is unbounded,
  not merely deep.

  What it needs is a depth cap: the same thing `DOLRECOMP_C_MAX_CALL_DEPTH`
  does for the C backend, which falls back to the dispatcher once calls nest too
  far. The C backend has one because it had to; this backend never needed one on
  a host with a real stack, and on wasm it does.

  **To pick this up**, the pieces are on disk and the browser build is
  configured against them:

      build-wasm/gexe52-llvm-wasm32   the module (259 MB of wasm objects)
      build-wasm/web-llvm             the browser build that links it -- DELETED
                                      2026-08-29 to reclaim 552 MB. It is a
                                      CMake build over the module below, so it
                                      costs a configure and a link to recreate;
                                      the module is the expensive artifact and
                                      is still here.

  Regenerate the module with:

      DOLRECOMP_LLVM_TARGET=wasm32-unknown-emscripten \
        build-dolrecomp-llvm20/dolrecomp --gamecube --backend llvm -j10 \
        build-wasm/gexe52/sys/main.dol <outdir>
      cp build-wasm/gexe52/sys/main.dol <outdir>/generated/

  and swap `experiments/wasm/dolphin/web/dolweb.{js,wasm,data}` between
  `build-wasm/web` and `build-wasm/web-llvm` to compare the two backends in the
  same page. Delete both directories if you are not working on this -- neither
  is needed by the C build, which is what `web/` points at.

  **And it is 213.8 MB against the C backend's 86.5 MB** -- 2.5x *larger*, not
  smaller, which is the opposite of what this file used to predict. On a phone
  whose heap already reaches 614 MB, a 213 MB module is not shippable, so size
  is this route's central problem rather than its promise.

  **The pass pipeline is not where it comes from**, which is measured:
  `DOLRECOMP_LLVM_PIPELINE=size` drops `loop-vectorize`, `slp-vectorizer`,
  `vector-combine` and the inliner -- vectorising for a target built without
  SIMD -- and the objects come out at **212.89 MB either way**, identical to two
  decimal places. (The build id changes, so the pipeline really did, and these
  are not cached objects.) So the size is inherent to what the backend emits for
  this target, and the next place to look is the emission shape itself: 7417
  objects each carrying a per-function entry point and a dispatch preamble,
  against the C path's 1855 translation units.

  Historical note, kept because it bounded the work: what was left was exactly
  the CPUState layout, and it was precisely bounded.
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
