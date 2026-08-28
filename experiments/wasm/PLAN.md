# GXRuntime in the Browser Plan

*(The repository root's `PLAN.md` is the LLVM ARM64 iOS AOT plan, which shipped.
This is a separate line of work and does not touch it.)*

## Objective

Run a GameCube game the user supplies, in Safari on an iPhone, with the guest
code recompiled to WebAssembly and GXRuntime as the runtime — no Mac in the
loop after the first build, and one binary for every game.

The first end-to-end target is **Super Mario Strikers (`G4QE01`, NTSC)**, because
`StrikersRecomp` is the integration client GXRuntime is developed against and
is therefore the title with the least unknown runtime coverage. Disney skate
(GEXE52) is the second, because this repo already has the most measurement
history for it.

"Over the line" means: a disc the user owns, imported in the browser, boots to
gameplay at a frame rate a person would accept, on an iPhone, in Safari.

**Reached 2026-08-27.** See the status section below for the numbers, on the
phone and off it. Disney skate reached the screen the same day -- boot, SDK,
first frames -- and is stalled on a loading screen; see "A second disc".

## Why this shape and not the obvious one

Not negotiable, and worth restating because it kills every simpler design:

- **WASM needs a JIT to be viable.** Interpreted, it is 18–95x slower than
  native — far below the DolVM interpreter that was already too slow. An in-app
  WASM runtime (wasm3, WAMR, in-process JavaScriptCore) is not an option.
- **The only JIT on iOS is WebKit's**, in the out-of-process `WebContent`.
- **Therefore there is no hybrid.** Guest RAM in WASM linear memory cannot be
  shared with a native Metal renderer. If the guest code is WASM, *everything*
  is WASM, inside one web view: CPU, GX, DSP, presentation.

So the browser build is not a port of the iOS app. It is the product, and any
future iOS app is a WKWebView shell around it.

## Status (2026-08-27)

**A GameCube game plays in a browser, including on an iPhone.** Super Mario
Strikers (`G4QE01`) boots from a user-supplied disc, gets through the health
screen, the intro cutscene and the front-end menus on its own input, and reaches
a live match -- scoreboard, clock, both teams, shadows -- rendering through
WebGPU, with the runtime, the renderer, the disc and the audio all inside one
page and the guest code interpreted. ~18 fps in a match on an M4 desktop;
**31.7 fps over 4 000 frames on an iPhone 15 Pro Max in Safari**, nothing
installed.

| Milestone | State |
|---|---|
| 0 — where the 11 fps goes | **Answered, and the premise was wrong.** In-match reached in the browser and it runs at ~18 fps with the renderer and ~40 fps without on WebKit, so it is not CPU-bound in the way the gate assumed. The ~11 fps figure predates the texture-cache fix below. |
| 1 — the web backend | **Done.** `backends/web/` + `web/dolweb.js`; verified by GPU readback, not by eye (`web/selftest.html`). |
| 2 — replay a trace, count the gaps | Superseded: the counters come straight out of a live browser run, which is a better sample than a recorded trace. No pixel comparison against Aurora yet. |
| 3 — close the gaps | Little to close: 7 503 skipped draws out of 598 580 in a gameplay run is 1.25%, and 6 537 of those are `cull_all` (state, not a gap). The real remainder is 966 vertex-decode failures and 7 699 unresolved texture matrices. |
| 4 — boot a game in the browser | **Done.** Disc import, OPFS persistence, memory card, the frame loop, input through the real pad path, and audio through an AudioWorklet -- boot, intro, menus and a live match all reached. **The returning-visit path is verified end to end**: with no `?iso` and no picker, the page finds the 648 MB disc in storage, attaches it and boots. Touch pad untested on a device. |
| 5 — guest code as WebAssembly | Not started. |
| 6 — iOS device bring-up | **Done.** On an iPhone 15 Pro Max in Safari: WebGPU renders through the real backend, the 106 MB module loads in 1.5 s, and the game runs 4 000 frames at 31.7 fps. In-match on the device is the one figure still unmeasured. |
| 7 — recompile on the device | Not started. |
| 8 — make it fast enough | Measured, and better than forecast: **~0.5x of native on V8, ~1.0x on WebKit** (14-16M guest block dispatches/s against native's ~15M). One texture-cache fix was worth 2.8x on texture-heavy scenes. |

### Playable, not just running (2026-08-27)

Three things stood between "a game renders in Safari" and "a game you can play
in Safari", all in the page rather than the runtime.

- **The pad.** Each control was its own element with its own pointer capture,
  positioned as a percentage of a 4:3 canvas. No second finger, no roll-off, no
  D-pad, and a stick with a third of its travel dead. It is one hit-tested
  surface now: multi-touch, roll-on/roll-off, lost-pointer recovery, a rescaled
  radial deadzone, and D-pad bits from the stick past a threshold with
  hysteresis (plenty of menus read only those). A gamepad no longer clobbers
  the touch state -- attaching any HID device used to disable the on-screen pad.
- **Zoom and scroll.** `user-scalable=no` has been advisory on iOS since 10, so
  double-tap zoomed the game and a drag rubber-banded the page. `touch-action`,
  refused `gesture*`/`dblclick`, and a fixed non-scrolling body stop all of it.
- **Orientation.** There was one layout, and in landscape on a phone its 4:3
  canvas was taller than the screen. The picture is fitted to the viewport now
  and the pad placed against it -- over the screen edges in landscape, in the
  free area below the picture in portrait -- with safe-area insets and an
  immersive mode. Control positions are offsets from an anchored corner rather
  than fractions of the box, followed by a collision-relaxation pass, so one
  table serves both orientations.
- **Audio was wired and never heard**, and the reason was the unlock: a single
  `{once: true}` pointerdown listener that fired on the first tap of the page --
  the disc picker, before a session exists. It did nothing and removed itself.
  It retries on every gesture now. Three further iOS fixes: the context runs at
  the *hardware* rate with the worklet resampling (asking for the guest's
  32 kHz is the fragile path), `navigator.audioSession.type = 'playback'` so
  the ring switch does not silently mute a game, and a suspended context is
  resumed on `visibilitychange`. The worklet primes before its first sample and
  re-primes on a starve rather than clicking per sample.

`drive.mjs` is a CDP harness that emulates a phone with a real touch screen and
asserts the pad: every button, two fingers at once, stick-to-D-pad, rolling
B->A, no zoom, no scroll, nothing off-screen. Passing in both orientations.

### A second disc: Disney's Extreme Skate Adventure (2026-08-27)

**It boots and renders.** GEXE52 compiles to an 82.7 MB module, brings up the
September 2002 SDK (OS, VI, ARQ, AI, AX, DSP all announce themselves) and puts
18 draws and 2 presented frames on screen, 0 skipped. It then stalls on a
loading screen.

The client was Strikers all the way down and is per-title now: everything one
disc knows lives in `runtime/host/game_<ID>.h`, every intercept with an SDK name
registers *by name* through the generated table, and hooks a title has not had
identified are zero -- which installs nothing rather than something wrong.
`web/build.sh --game GEXE52`; the page picks its module from the disc's ID.

**The gate was the SDK intercept table**, which came from a decomp symbol map
that exists for Strikers and for nothing else. `tools/sdk_signatures.py` derives
it from the binary -- see that file, and the commit, for the method and its
measured accuracy (371/371 on Strikers against itself, 0 wrong; 315 intercepts
for GEXE52). Two findings shaped it: the naive "matches must increase in both
address spaces" constraint is wrong, because whole libraries move between games
(Strikers links `os.a` after `gx.a`, Skate 200 KB earlier), and Dolphin's
`totaldb.dsy` collides so badly it "matches" 90% of a game -- usable only after
dropping any name it claims twice.

Four defects stood between "it compiles" and "it renders", all in the hardware
model, all latent for Strikers because Strikers replaces its whole audio init:
the DSP reset bit was stored rather than honoured; the DSPCR interrupt status
bits are write-1-clear and were not; there were no mailboxes at all; and the
command processor was unmapped, so its status register read "still busy".

**What is left**, in order: the loading-screen stall (the guest is scheduling
threads and waiting, so it is a completion the host never delivers -- the same
shape as the five Strikers defects below); then audio, which needs an AX
stand-in the way Strikers needed a MusyX one; then whatever the rest of the
game asks for.

### What it took, beyond the plan

The plan assumed StrikersRecomp booted. In this tree it did not, and five real
defects stood between "the plan" and "a game on screen". All are fixed:

1. **DolRecomp's chunked emission hid the HLE dispatcher.** A `bl` to a function
   inside the caller's own ~16 KB chunk became a plain `goto`, so it never
   reached `ppc_host_call` and every SDK intercept in that chunk silently ran
   the guest's own code instead. New opt-in emitter mode
   (`DOLRECOMP_HLE_LOCAL_CALLS=1`, `emit_set_hle_local_calls`) puts the host
   check back in front of the goto. Without it the guest hung in
   `__OSInitAudioSystem` waiting on a DSP reset bit.
2. **Intercepts that tail-call had their jump overwritten.** `hle_dispatch` ran
   `hle_return()` unconditionally after every intercept, clobbering the `pc` an
   intercept had set to a guest callback (`dol_hle_aramUploadData` says as much
   in a comment). Now it only returns when the intercept left the pc alone.
3. **The ARQ DMA completion callback never ran.** `dol_hle_ARQPostRequest` set
   `cpu->pc = callback` into the teeth of (2); it now queues the callback, which
   is both faithful to an asynchronous DMA and survives `hle_return`. The guest
   busy-waits on a flag that callback clears, so `aramInit` hung forever.
4. **The DVD read completion callback was never invoked** — the code printed a
   note saying so. Every asynchronous resource load stalled. Now queued.
5. **`GXDrawDone()` slept forever.** Nothing raised the PE FINISH interrupt, so
   the game's main thread parked on the GX finish queue at its first frame and
   the OS idle loop was the only thing left running. The FIFO peek in
   `runtime/host/mmio.c` now commits PE finish on BP 0x45 = 0x02.

And one that was the browser port's own fault: the host policy that needs *a
graphics backend* was spelled `#ifdef STRIKERSRECOMP_AURORA`, which left the web
build with no `GXBegin`, no `GXCallDisplayList`, no `GXCopyDisp` and therefore no
present at all. It is `STRIKERSRECOMP_HAS_BACKEND` now.

### On the phone (iPhone 15 Pro Max, iOS 26.6.1, Safari)

**Both risks the plan called fatal are retired.**

| question | answer |
|---|---|
| WebGPU on a real iPhone | **yes** — `requestAdapter()` returns an adapter, and `web/device.html` rendered real GX FIFO bytes through the actual web backend to pixels: 45.8% EFB coverage, 64+ colours, verified by GPU readback, identical to the desktop result |
| does a 106 MB module survive | **yes** — compiled in 1 544 ms, instantiated in 5 ms, tab alive |
| **does the game run on it** | **yes** — 4 000 frames at **31.7 fps** through boot, the intro and the front-end menus, 213 265 draws planned and **0 skipped**, texture cache at 99.97%, tab alive throughout |

The disc came over Wi-Fi from the Mac in 9 s and went straight into OPFS, so the
phone keeps it; the module was ready 2.8 s later. By segment: 37.8 fps during
boot, then 31-34 through the menus at 74 draws/frame. Audio did not start
because a scripted run has no user gesture and iOS demands one — on a real tap
it does.

Everything else the build needs is there too: `crossOriginIsolated` true and
SharedArrayBuffer available (so threads are on the table), AudioWorklet, wasm
SIMD128, OffscreenCanvas, OPFS with a **39 GB quota** and a verified 1 MB round
trip, and wasm memory that grows to 4 032 MB. Four cores, DPR 3, 430x932.
The adapter reports `maxBufferSize` 1 GB and `minUniformBufferOffsetAlignment`
**32** — a quarter of the desktop's 256, so the frame stream's uniform padding
is conservative there and could be tightened for the device.

**And the thing that nearly hid all of it:** over plain HTTP from a LAN address
the same phone reported **no `navigator.gpu`, no OPFS, no AudioWorklet and no
SharedArrayBuffer**. Every one of those is a secure-context feature, and a LAN
IP over HTTP is not a secure context — while `127.0.0.1` is, which is exactly
why the Simulator showed a clean bill of health and a real phone did not.
`serve.py --lan` now serves HTTPS with a self-signed certificate (Safari warns
once; tap through) and all of it comes back. A shipping build serving from the
bundle or from HTTPS never meets this, but any LAN test does.

### What the browser reports in the Simulator (iOS 18.7)

`web/caps.html` is the ten-minute check milestone 6 asks for, run through the
`ios/` WKWebView shell:

| capability | result |
|---|---|
| `crossOriginIsolated` | **true** over HTTP — SharedArrayBuffer and threads are on the table |
| `SharedArrayBuffer` | present |
| wasm SIMD128 | present |
| AudioWorklet / OffscreenCanvas | present |
| OPFS | present, **9.8 GB quota**, and a 1 MB write reads back identical — a 1.4 GB disc fits |
| wasm memory grown to | 2 624 MB |
| `navigator.gpu` | present |
| `requestAdapter()` | **null** — the Simulator has no adapter, exactly as the plan predicted |

So the Simulator answers everything except the one question that matters most,
and rendering on a real device stays unverified.

### Speed, at scenes rather than in the abstract

Headless Chrome on an M4, 640x528 EFB, guest code interpreted:

| scene | draws/frame | frames/s |
|---|---|---|
| boot + intro cutscene | 1-25 | 23-25 |
| front-end menus | ~58 | **30** (the guest's own rate; not CPU-limited) |
| in match | ~270 | **~18** |

For context the native Aurora build sits at a vsync-capped 60 at a 23-draw menu,
so the browser is not the limiting factor until a scene gets heavy.

**And the engine that matters is much faster than the one that was measured.**
Running the same page with `?norender=1` (no WebGPU work, guest only) through the
`ios/` shell in the Simulator — real iOS WebKit, on the Mac's CPU — against
headless Chrome on the same machine and the same 900 frames, which executed a
bit-identical 340 192 497 guest blocks in both:

| engine | frames/s | guest blocks/s |
|---|---|---|
| Chrome (V8) | 17.6 | 6.6M |
| **iOS WebKit (JSC)** | **42.6** | **16.2M** |

At a heavier scene JSC held 14.1-14.6M blocks/s against V8's 8.5-9.7M. Native
arm64 runs ~15M. So **on WebKit the recompiled guest costs roughly nothing, not
the 0.5x the four-kernel benchmark predicted** — that benchmark measured what
JSC does to four tight loops, and a real game is not four tight loops. The
0.5-0.6x figure in `README.md` should be read as a V8 number.

The caveat is unavoidable and worth stating plainly: the Simulator runs on the
Mac's M4, so this compares *engines*, not devices. What it does establish is
that the browser tax on iPhone is a WebKit question, and WebKit answers it well.

**One fix moved the heavy scenes 2.8x.** The texture cache hashed
`tlut_available` bytes of the palette region to key a CI texture -- and
`tlut_available` is "whatever the resolver can still reach from that address",
i.e. the rest of MEM1. So every paletted draw hashed megabytes, and any
unrelated write nearby invalidated the entry. In a menu that meant 66 628
texture decodes across 3 007 frames; after hashing `tlut_entries * 2` bytes it
is 1 276, the hit rate is 99.97%, and the scene went 6.5 -> 18 fps. **The same
bug is in the native substrate** (`graphics/aurora/lib/gfx/gxcore_draw.cpp`) and
is fixed there too, so GXRuntime's own ~11 fps in-match figure predates it.

Audio is proven end to end: the DSP path fills the ring and the AudioWorklet
drains it (`?audio=1` in the harness, with `--mute-audio` -- a scripted run
should never make noise in someone's room).

Storage is proven too, which matters because it is what a returning visitor
hits every time. `PROFILE_DIR=... ./run-headless.sh` keeps the browser profile
between runs, so the two-visit sequence is scriptable:

```sh
P=/tmp/dolweb-profile
PROFILE_DIR=$P ./run-headless.sh '?iso=/disc.iso&store=1&auto=1&frames=120&seconds=60'
PROFILE_DIR=$P ./run-headless.sh '?auto=1&frames=400&seconds=90'   # no ?iso, no picker
```

The second run reports `disc-restored` and boots. A memory card round-trips the
same way, with a size floor so the runtime's 40-byte stub is not mistaken for a
save.

### The two measurements that decide the iOS story

- **Module size: ~106 MB** for 665k guest instructions (~155 bytes each, against
  37 for native arm64), and **three separate attempts failed to move it**:

  | lever | result |
  |---|---|
  | chunks at `-Os` instead of `-O3` | 105.8 -> 100.8 MB (-5%) |
  | `DOLRECOMP_C_CHUNK_INSTRUCTIONS=1024` (a quarter the default) | 105.8 -> 101.9 MB (-4%), but **45% faster to build** |
  | `wasm-opt -Oz` | 105.8 -> **95.6 MB (-10%)**, the only real lever -- but 19.5 minutes and **9.7 GB resident** |

  None of it turned out to matter: the phone loads the unoptimised 106 MB module
  in 1.5 s and plays thousands of frames on it. Size is a startup-latency
  question, not the jetsam question the plan feared.

  And a trap in the last one: `--all-features` makes wasm-opt emit compact
  imports, which Chrome rejects outright (*"Invalid import kind 127"*). Pass the
  eight features the module's own `target_features` section declares, or nothing
  at all and let wasm-opt read that section itself.

  The section breakdown says why none of these get far: the module is **100.0% code**,
  2 846 function bodies of which the top 200 are 103 MB. It is not structuring
  overhead and it is not dead weight — it is the emitted shape, one `ctx->pc =`
  store and a handful of wasm instructions per guest instruction, times 665k.

  The honest conclusion is that a *whole-game static* module cannot be made
  small, because it carries every instruction in the game whether or not the
  game ever executes it. The lever that actually exists is milestone 7: emit
  only what is reached, on the device. At the ~16x engine-metadata blowup
  measured earlier this is ~1.6 GB against a WebContent jetsam limit, and
  **only a phone can say whether that is fatal** — `web/device.html` asks it.
- **Guest throughput: ~0.5x native**, 6.9-8.4M block dispatches/s in the browser
  against ~15M natively — the kernel benchmark's 0.5-0.6x, confirmed on a whole
  real game rather than four kernels.

### Coverage, from a live run rather than a trace

gxcore's gap counters over ~42 000 planned draws of boot + intro:

| counter | count |
|---|---|
| draws_planned | 42 836 |
| **draws_skipped** | **0** |
| tlut_texture (CI/paletted) | 14 166 |
| tev_multi_texmap | 7 790 |
| efb_copies / efb_display_copies | 11 725 / 11 729 |

Zero skipped draws is the headline: milestone 3 has nothing to close on this
path. The CI-texture and multi-texmap counters are demand signals, not failures —
both are handled.

### Reproducing it

```sh
cd experiments/wasm
web/build-selftest.sh                       # the backend, no game (seconds)
web/build.sh --generate /path/to/your.iso   # the full client (minutes)
python3 serve.py game --iso /path/to/your.iso
```

Then open `http://127.0.0.1:8712/`, choose your disc once (it is streamed into
OPFS and loads itself next time), and press Boot. Keyboard is J/K/U/I = A/B/X/Y,
WASD = stick, Enter = Start; a phone gets the on-screen pad automatically and
audio starts on the first touch.

Scripted, in headless Chrome — always muted, and it POSTs a JSON report plus a
filmstrip of EFB readbacks:

```sh
./run-headless.sh selftest.html                   # the backend alone
./run-headless.sh '?iso=/disc.iso&auto=1&frames=4000&seconds=380&shots=700&input=mash'
```

On an actual phone — no install, no signing, no disc:

```sh
python3 serve.py game --iso /path/to/your.iso --lan   # prints a LAN URL
```

Open `http://<that address>/device.html` in Safari on the phone. It reports its
capabilities, renders real GX FIFO bytes through WebGPU and reads the pixels
back, then compiles and instantiates the whole-game module — POSTing after each
stage, so if the tab is killed by the memory limit the last report that arrived
says exactly where. `reports.jsonl` collects them on the Mac.

Under real iOS WebKit, through the WKWebView shell in the Simulator (which has
no WebGPU adapter, hence `norender`):

```sh
URL="http://127.0.0.1:8712/caps.html" ios/build-sim.sh          # what the engine offers
URL="http://127.0.0.1:8712/?iso=/disc.iso&auto=1&norender=1&frames=4000&seconds=380&input=mash" \
  ios/build-sim.sh                                             # guest throughput
```

Useful query parameters: `trace=1` (mirror every log line to the console, the
only channel a headless page reliably has), `env=STRIKERS_HLE_LOG,...` (turn on
the runtime's own diagnostics, which are environment-gated and would otherwise
be unreachable in wasm), `budget=`/`ms=` (per-slice guest budget), `audio=1`,
`shots=N`, `heartbeat=N`.

## Current State

Measured 2026-08-26; see `README.md` here for the numbers and how to reproduce.

**Already true, verified:**

- DolRecomp's generated C compiles to `wasm32` unchanged and produces
  bit-identical guest results. A whole game is one 29 MB `.wasm`, which JSC
  compiles in 48–81 ms.
- **All of GXRuntime builds for `wasm32` with no source changes** — the hardware
  model (CPU, MMIO bus, DVD, EXI, SI, DI, interrupts, VI clock, memory card,
  ARAM, HLE, savestates) and gxcore, the render sink, the retail GX frontend,
  trace I/O and `dolgx_replay`.
- **gxcore already emits WGSL**, the browser's own shader language. There is no
  shader translation layer to write.
- Real GX register writes render correctly through WebGPU in Safari
  (`spike/`, 83 KB of wasm).
- iOS WebKit has WebGPU, WebGL2, wasm SIMD, AudioWorklet, SharedArrayBuffer and
  cross-origin isolation.

**The two facts that shape every milestone below:**

1. **The host boundary is one struct.** `include/gxruntime/platform.h` is 114
   lines and `DolPlatformOps` is 23 function pointers — present, `gx_write`,
   display lists, arrays, textures, TLUTs, copy destination, VI config, four
   pad entries, two audio entries. `src/headless_backend.c` implements the whole
   thing in 103 lines. A web backend is a third implementation of that struct,
   not a port.
2. **`StrikersRecomp` is the client template.** It compiles DolRecomp's
   `generated.c` + `chunks/` directly into an executable and links
   `GXRuntime::runtime` plus `GXRuntime::aurora`. The browser client is the same
   CMake shape with `GXRuntime::web` instead of Aurora, and `emcc` instead of
   `clang`.

**The number that decides whether this is worth doing:** GXRuntime's own README
puts in-match performance at **~11 fps**. The browser costs a further ~0.5–0.6x
on CPU-bound guest code. If that 11 fps is CPU-bound, the browser lands near 6,
and no amount of WASM work fixes it — GXRuntime has to reach roughly **2x
realtime natively** before the browser is playable. That is the project's
actual gate, and it is not in this repo's control.

## What is missing, honestly

gxcore keeps its own gap inventory (`GapCounters` in `gxcore.hpp`), which is the
best available list of what a real game will hit. Unimplemented or stubbed today:

| Gap | Counter | Matters because |
|---|---|---|
| CI/paletted textures (C4/C8/C14X2) | `tlut_texture` | common in GC titles |
| Lighting from channel control | `lighting_ignored` | any lit scene |
| EFB copy-to-texture | `efb_copy_ignored` | reflections, post, many effects |
| TEV stages beyond the slice | `tev_stages_over`, `tev_multi_texmap` | most real materials |
| Alpha compare without TEV | `alpha_compare_ignored` | cutouts, foliage |
| Fog on non-TEV draws | `fog_ignored` | outdoor scenes |
| Indirect texture stages | `indirect_ignored` | marked out of scope |
| Line/point primitives | skip reason | UI, particles, debug |
| Per-vertex texture matrices | `per_vertex_tex_mtx` | skinned/animated UVs |
| Emboss / SRTG texgen | `unsupported_texgen` | bump mapping |
| Logic op | `logic_op_ignored` | rare but real |

**Do not estimate this list from the outside.** Milestone 2 exists to turn it
into a per-title count, because "12 unimplemented features" and "12 features
that account for 0.3% of Strikers' draws" are different projects.

Outside gxcore: DSP audio is streamed rather than LLE, there is no browser disc
import, no touch input, and no WASM target in DolRecomp's LLVM backend.

---

## Milestone 0: Know where GXRuntime's 11 fps goes

Before any browser work. If the answer is "CPU", this plan's value depends
entirely on GXRuntime's own optimisation work and should be sequenced behind it.
If it is "renderer", WebGPU may behave differently enough that the number does
not transfer at all.

**Work.** Build `StrikersRecomp` natively with the Aurora backend, get to the
in-match scene, and profile it. Separate guest-CPU time from gxcore draw-plan
building from GPU submission from present/vsync.

**Exit criteria.**

- A breakdown of a frame in that scene into at least those four buckets.
- A statement of what realtime would require, as a multiple of today.
- A go/no-go recorded here: if in-match is CPU-bound and more than ~4x off
  realtime, stop and revisit, because the browser multiplies the gap.

## Milestone 1: The web backend

Implement `DolPlatformOps` against the browser, as `backends/web/`, alongside
`backends/aurora/`.

**Work.**

- `present`, `should_quit`, `configure_vi` — canvas sizing and the frame loop.
- `mark_gx_begin` / `gx_write` / `call_display_list` / `set_array*` — these
  already land in gxcore; the backend only forwards.
- The submission half: take gxcore's `DrawPlan` (WGSL string, decoded vertices
  in the fixed 120-byte layout, POD uniform blocks, viewport, pipeline key) and
  turn it into WebGPU pipelines, buffers and draws, with a pipeline cache keyed
  on `PipelineKey`. `spike/web/index.html` is the throwaway version of exactly
  this; the real one lives in C++ behind `emscripten/html5_webgpu.h` or in JS
  with a thin C shim, whichever keeps the per-draw crossing cheapest.
- `load_texture*` / `load_tlut*` — gxcore's `texture_decode.cpp` already decodes
  GX formats; the backend uploads and caches on `object_id` + `data_version`.
- `pad_*` and `audio_*` are stubs at this milestone.

**Design note.** Keep the JS/WASM boundary at *frame* granularity, not draw
granularity, if measurement says draws are expensive to cross. The draw plan is
already a flat POD; batching a frame's plans into one buffer and crossing once
is the fallback if per-draw `writeBuffer` dominates.

**Exit criteria.**

- `StrikersRecomp` built with `emcc` and `GXRuntime::web` renders its first
  frame in desktop Safari and Chrome.
- The frame is verified by GPU readback, not by eye — the spike's
  `copyTextureToBuffer` pattern, because sampling a WebGPU canvas races the
  compositor and will lie to you.
- A `control.html` equivalent is kept in tree so a black screen can always be
  bisected into "WebGPU is broken" vs "we are broken".

## Milestone 2: Replay a real title's GX trace, and count the gaps

The cheapest possible truth about coverage, and it needs no booting game.

**Work.** `dolgx_replay` and `trace_io` already build for wasm. Record a FIFO
trace from a title running natively, replay it through the web backend, and dump
`GapCounters` per frame.

**Exit criteria.**

- Per-title, per-scene table: draws planned, draws skipped, and the skip reason
  histogram.
- The gap table above re-sorted by **how many draws each gap actually costs**,
  which becomes the work queue for Milestone 3.
- Frames rendered from the trace compared against the native Aurora backend's
  output for the same trace — not pixel-exact necessarily, but close enough that
  divergence is a bug report rather than a shrug.

## Milestone 3: Close the gaps the trace says matter

Driven entirely by Milestone 2's histogram, in descending order. Expect CI
textures, lighting, EFB copies and TEV coverage to dominate; do not pre-commit
to that ordering before the measurement exists.

**Exit criteria.** Skipped draws under 1% of planned draws for the target
title's boot-to-gameplay path, and every remaining skip named and understood.

## Milestone 4: Boot a game in the browser

Everything before this replays or renders. This makes it a game.

**Work.**

- **Disc import and storage.** A file picker into OPFS, extraction with
  DolRecomp's `disc_extract_gamecube()` (already portable C), and `dvd.c` /
  `di.c` reading from OPFS. Watch iOS storage quotas and eviction — a 1.4 GB
  extracted disc is not something Safari guarantees to keep.
- **Input.** Touch controls into `DolPadState`, plus the Gamepad API for real
  pads. The existing on-screen pad in `ios/App/DBTouchPadView.mm` is the design
  reference, not the code.
- **Audio.** `audio_push` into an AudioWorklet ring buffer, with the
  user-gesture unlock that iOS requires before any audio starts.
- **The frame loop.** GXRuntime's `Run()` blocks; the browser cannot. Either
  drive it from `requestAnimationFrame` with a bounded slice per callback, or
  run it on a worker with `-pthread` and `SharedArrayBuffer`. **Start
  single-threaded** — threads need cross-origin isolation, which a WKWebView
  custom scheme silently does not provide.

**Exit criteria.** Target title boots from a user-supplied disc to a playable
scene in desktop Safari, with sound and working input, guest code still
interpreted.

## Milestone 5: The guest code as WebAssembly

Only now does the recompiler change, because until Milestone 4 there is nothing
to run it in.

**Work.**

- Register the WebAssembly target in DolRecomp's LLVM backend and add `wasm32`
  to `supportedTarget()` (`src/backend/llvm/llvm_backend.cpp`); link with
  `wasm-ld`. The guest load path is already a range check plus a byteswap
  against a base pointer, which is the shape WASM linear memory wants.
- Reconcile the module ABI. GXRuntime declares `GXRUNTIME_CPU_ABI_VERSION 3`
  and ModernGekko's descriptor is at version 4 — confirm which surface a
  browser client attaches to before assuming either.
- Skip Binaryen's `wasm-opt` at link. It cost 373 s on a 29 MB module and LLVM
  has already optimised each translation unit.

**Exit criteria.**

- Target title runs on recompiled WASM rather than the interpreter, with
  identical guest checksums against a native arm64 build of the same module.
- Measured speedup over the interpreted path recorded here.

## Milestone 6: iOS device bring-up

**This is where the plan's two unmeasured risks land**, and both are cheap to
retire early — consider pulling them forward if either looks likely to be fatal.

- **WebGPU on a real iPhone is unverified.** The iOS Simulator exposes
  `navigator.gpu` but `requestAdapter()` returns null, so it cannot answer this.
  A phone can, in ten minutes, with `serve.py spike --lan`. **Do this first.**
- **Memory.** A 29 MB module cost ~460 MB of engine metadata — a ~16x blowup —
  and that sits against the WebContent process's jetsam limit alongside guest
  RAM and textures. Load progressively larger modules in Safari on a device
  until it dies, and compare a Safari tab against a home-screen PWA, which may
  have a different limit.

**Work.** Beyond measurement: touch input tuning, thermal behaviour over a
sustained session, and deciding tab vs PWA vs WKWebView shell.

**Exit criteria.** Target title playable on an iPhone 15 Pro Max in Safari, with
a recorded speed figure and a recorded peak memory figure.

## Milestone 7: Recompile on the device

The milestone that makes this an emulator rather than a per-game binary, and
the only reason the whole approach beats the native AOT path on merit.

**Work.** Emit WASM bytes **directly from DolIR** — no clang, no LLVM, neither
of which fits in a browser. DolIR is 2,007 lines and
`DolRecomp/src/backend/vm/dolvm_emit.c` (DolIR → bytecode) is 2,815, so a
DolIR → WASM emitter is the same shape and roughly the same size. Call it ~3k
lines targeting a stack machine simpler than the bytecode it replaces.

**Exit criteria.** A disc the build has never seen, imported on the phone,
recompiled on the phone, and played — with no rebuild and no re-signing.

## Milestone 8: Make it fast enough

Deliberately last, and deliberately not optional.

**Start by finding out where the gap is, because two obvious answers are wrong.**
Measured against the `mem` kernel: deleting the byteswap entirely moves the
wasm/native ratio 0.62 → 0.68, inside the noise; replacing the branchy
`resolve_addr` with one unchecked path nearly doubles native (1295 → 2408 guest
MIPS) and leaves wasm flat (805 → 817). Neither the endianness swap nor the
range checks are the bottleneck — the wasm memory path sits at a ~800 MIPS floor
that neither change moves, and the cause is unidentified. **Attribute it before
optimising anything**, ideally against the LLVM backend's inlined memory path
rather than the C backend's helper calls.

- **Build the whole thing with `-msimd128`.** It is one flag, Safari supports
  it, and it takes bulk byteswap loops from 9.4 to 49.3 GB/s — native parity
  (48.8). It does nothing for scalar guest loads (818 → 818 MIPS on the `mem`
  kernel), so it is a texture-decode and vertex-loader win, not a CPU win. Do
  not hand-write intrinsics: LLVM's auto-vectoriser beat a hand-written
  `i8x16.shuffle` version (49.3 vs 44.2 GB/s).
- **Paired-single work maps onto `f64x2`** if the FP path needs it.
- **Floating point was the worst kernel (0.42x)** in the C backend's helper-call
  shape. The LLVM backend inlines the common FP path; confirm that survives the
  WASM retarget rather than assuming it.
- **Expect the ratio to worsen as the native side improves.** The `resolve_addr`
  result above shows native has headroom wasm cannot reach, so the 0.5–0.6x
  figure measured against the C backend is likely optimistic for the shipping
  LLVM backend.

**Exit criteria.** A speed figure on device for the target title, and an honest
statement of what is left.

---

## Risks

| Risk | Kills the plan? | Retire it by |
|---|---|---|
| GXRuntime's 11 fps is CPU-bound and far off realtime | Yes | Milestone 0, before anything else |
| WebGPU absent or broken on real iOS devices | Yes | Ten minutes with a phone; do it in week one |
| WebContent memory ceiling below a real game's needs | Yes | Progressive module loading on a device |
| gxcore coverage gaps are pervasive rather than long-tail | No, but re-scopes | Milestone 2's histogram |
| iOS evicts a 1.4 GB extracted disc from OPFS | No, annoying | Measure quota behaviour in Milestone 4 |
| Apple rejects a WKWebView-hosted emulator | No — browser build is unaffected | Only matters if an App Store build is wanted |

## What this plan does not do

- **It does not touch the native arm64 path.** That path is faster, it works,
  and Disney skate and Metroid Prime 2 hold 100% on it today. This is a
  distribution strategy, not a performance one, and if it stalls nothing is lost.
- **It does not port Dolphin.** ModernGekko's runtime is Dolphin Core at 480,659
  lines with no WASM port and no WebGPU backend. Choosing GXRuntime is the
  decision that makes this tractable, and it is a real trade: a
  mature-but-unportable emulator for a portable-but-immature one.
- **It ships no game data**, in the browser or anywhere else. The user supplies
  the disc; nothing game-derived enters this repository.
