# What is left, and how to do it

*Written 2026-08-28, rewritten 2026-08-29 after a header turned out to be
corrupting the disc. The objective is unchanged: **Disney skate playing
correctly at full speed in Safari on the iPhone, with nothing installed**, on
code statically recompiled on a Mac. `README.md` is how to build and measure;
`DOLPHIN-ROUTE.md` is why the route exists. This is the part that is not done.*

## 0. Every number below 2026-08-29 was taken through a broken transport

`serve.py` sent `Accept-Ranges: bytes` twice on a Range HEAD. HTTP joins
repeated headers with a comma, emscripten's WASMFS fetch backend tests
`headers.get('Accept-Ranges') == 'bytes'`, and `"bytes, bytes"` is not `"bytes"`
-- so **every game file took the download-the-whole-thing path**: 1.2 GB of disc
into JS memory instead of 61 MB of ranges.

That is not merely slow. It left the apploader's copy of `main.dol` wrong: 411
chunks failed their SMC hash, 420 KB of guest code fell to the interpreter,
`native_dispatches=2` against 1.9 billion interpreted steps, 0% speed, and the
PC walking the exception vectors. Every symptom pointed at the recompiler. The
same binary in the node harness (NODERAWFS) ran at 229% with `smc_failed=0`, and
that contrast is what localises this class of defect in one run.

Fixed in `23f63d5`. **The device figures in the next section, and the whole of
the black-ground investigation, were made against that build** -- the header
went in on 2026-08-28 in `76b9ef5`, before either. Nothing here is trustworthy
until it is retaken.

## 1. The renderer on the device -- the speed, unmeasured again

This Mac, on the fixed build, matched guest window 45-90 s, throttle off
(`?ab=45&auto=1&report=1`), measured 2026-08-29:

| | |
|---|---|
| Null (CPU only) | **199%** (median 201, 194-208) |
| OpenGL | **215%** (median 217, 207-224) |
| heap at steady state | **614 MB**, on top of an 86.5 MB module |

So the renderer is free on the Mac, and that half of the old reading survives
the transport fix.

### The phone, measured

The old device row here read 157% Null / 40% OpenGL. Both were taken while the
phone was pulling 1.2 GB of disc over Wi-Fi on every boot, and a WASMFS fetch
blocks the emulation thread, so both were measuring the download. **They are
withdrawn.** Replaced 2026-08-29 by a run on the real phone -- iPhone 15 Pro
Max, iOS 18.7, Safari 26.6.1, ranged fetches, the `acts=` timeline driving
itself into Andy's House, window `?ab=30&abfrom=125`:

| | |
|---|---|
| iPhone 15 Pro Max, **Null**, in the level | **95%** (median 102, 70-134) |
| iPhone 15 Pro Max, **OpenGL**, in the level | *pending* |

The phone's CPU half is **not** the problem: 95% against the simulator's 85%,
on a core that ought to be slower than the M4's. The gap is the other way round
because the simulator shares the Mac's GPU and its Safari is not the phone's
driver stack -- so **treat the simulator as a correctness tool and a rough CPU
proxy, and take performance numbers from the phone.**

**The heap, finally measured on hardware.** 512 MB with the Null backend,
**614 MB with OpenGL** -- the same figure Chrome reports, so it is the
emulator's own footprint and not a desktop-only artifact. It survives on the
device, but not with much room: during the OpenGL pass the tab **reloaded
itself once** at guest ~40 s and started over. That is what an iOS jetsam kill
looks like from the outside. It bounds anything memory-hungry -- see the LLVM
module's 213.8 MB below, which would land on top of this.

### Safari is not Chrome, and the A/B window has been measuring the menus

`sim-run.sh` puts the page in **simulator Safari** -- the same WebKit as the
phone, on the same Mac GPU as the Chrome numbers above, so the only variable is
the engine. In the level, throttled:

Measured on an idle machine, `?ab=30&abfrom=125` with the `acts=` timeline, so
the window is **inside Andy's House** rather than in the attract loop:

| | |
|---|---|
| simulator Safari, **Null**, in the level | **85%** (median 87, 64-103) |
| simulator Safari, **OpenGL**, in the level | **51%** (median 51, 31-82) |
| simulator Safari, Null / OpenGL, menus | 166% / 156% |
| Chrome, Null / OpenGL, menus | 199% / 215% |

**Read the first two rows twice.** With the renderer switched off entirely,
WebKit runs the recompiled code at **85% of realtime in gameplay** -- the CPU is
already short before the renderer is asked for anything, on an M4, whose core is
faster than the phone's. Turning the renderer on takes it to **51%**. The work
splits almost exactly in half: the CPU cannot keep up alone, and the renderer
costs about as much again.

Both halves need roughly a 2x, and neither one alone is enough. That is the
opposite of what the menu-window numbers have been saying, and it is why the
menu window mattered so much: over 45-90 s the same build reads 166/156, which
would have said the CPU was done and the renderer nearly free.

For the CPU half that is now an argument from measurement rather than from a
prior -- see `README.md`, where the LLVM backend already recompiles the whole
disc for wasm32 and needs only the CPUState tail. For the renderer half,
`DOLWEB_TIME_SWAP=1` still has never been run anywhere, and it is the one
measurement that separates "blocked waiting for the GPU" from "busy in
VideoCommon".

**And a measurement is worth exactly what the machine's idleness was.** The first
Safari level readings here -- 43-51% throttled, 18-23% unthrottled -- were taken
while a `dolrecomp` compile was running eight jobs on the same Mac. They measured
the contention. Check `uptime` before believing an emulator number.

**But the `?ab=45` window does not see it.** Anchored at guest second 45, it
lands in the attract loop and the title, not in gameplay -- Andy's House starts
around guest 110. Over that window Safari reads Null 166% / OpenGL 156% against
Chrome's 199% / 215%, a 20% gap rather than a 2x one. **Every figure in this
file taken with `abfrom=45`, including the device ones, is a menu figure.** Use
`?ab=30&abfrom=125` together with the `acts=` timeline that reaches the level.

**To retake them takes one tap**, and nothing on this machine substitutes for
it:

    python3 experiments/wasm/serve.py dolphin --lan

then on the phone, in Safari (it warns once about the self-signed certificate --
Show Details, visit this website):

    https://<printed LAN address>:8712/index.html?auto=1&report=1\
      &ab=30&abfrom=125\
      &acts=g25:5,g40:5,g52:5,g70:0,g76:0,g82:0,g110:15:9000,g135:15:9000,g160:15:9000

The page drives itself into Andy's House and posts Null then OpenGL back to
`reports.jsonl` with no further input. **Use `abfrom=125`, not the `abfrom=45`
every earlier device figure used** -- that window is the attract loop.

Retake them before doing anything else with the renderer. What the earlier work
did establish, and what survives:

- `DOLWEB_TIME_GL=1` on the device: the round-trip GL calls are ~3 ms of a 55 ms
  frame. With the state calls and the texture uploads wrapped as well, the total
  is ~4% of the frame on both machines. **Whatever the gap is, it is not in a GL
  call the shim can see.**
- `DOLWEB_TIME_SWAP=1` **has been run now, in gameplay, and the two engines
  answer differently** -- which is the single most useful thing in this file:

  | | swap (the present) | frame | swap share |
  |---|---|---|---|
  | Chrome | 0.30 ms | 16.0 ms | **2%** |
  | simulator Safari | **5.0 ms** (up to 10.3) | 26.5 ms | **~19%** |

  Same Mac, same GPU. Safari blocks in the present about 17x longer than Chrome
  does, and none of that is visible on Chrome, which is where every previous
  renderer measurement was taken.

  **Treat the Safari row as a lead, not a device fact.** It is the *simulator*,
  whose GL reaches the Mac's Metal through extra layers, and this file has
  already been wrong twice by reading one machine's renderer as another's. What
  it does establish is that the present is worth timing at all -- on Chrome it
  is 2% and could be dismissed. It is not the proxied-canvas fallback:
  `Core::Init` logs a warning when the canvas cannot be handed to the rendering
  thread, and neither engine logged one.

  On Chrome the rest is not GL either: every wrapped call sums to ~1.0-1.4 ms of
  a 16-23 ms frame (`glBufferData` 0.3-0.7 ms over ~270-615 calls,
  `glDrawElements` under 0.1 ms, the state calls a tenth each). **About 92% of a
  Chrome frame is in code the shim cannot see**, which is VideoCommon: texture
  cache, vertex loading, EFB copy processing.

  So the renderer half splits: **a present cost that only WebKit pays**, and a
  VideoCommon cost both pay. `-sOFFSCREENCANVAS_SUPPORT` and how the frame is
  committed are the levers for the first; the second is a profile away.
- Internal resolution is **already at the floor**: `GFX_EFB_SCALE` defaults to 1
  (native 640x528) and the wasm build does not override it. That question is
  answered; `DOLWEB_EFB_SCALE` is only useful as an A/B.
- Shader compilation is synchronous here (WebGL has no shared contexts, so the
  async compiler cannot work), so a new pipeline stalls the frame. It shows as
  spikes, not a lower median, and the guest-anchored `?ab` window is the honest
  way to see past it.
- **`sim-run.sh` runs the page in simulator Safari** -- the same WebKit as the
  phone, on 127.0.0.1, which is a secure context so SharedArrayBuffer works
  without the LAN certificate. It is a correctness proxy, not a speed one: the
  GPU underneath is the Mac's.

### The CPU half, profiled at last

**The emulator runs on a worker**, so `Profiler.start` on the page session
samples an idle main thread -- the first profile came back 99.9% `(idle)`, a
perfectly accurate measurement of nothing. The driver attaches to worker targets
now (`--profile g130:25`), and the build keeps its wasm name section with
`DOLWEB_PROFILING_FUNCS=ON`; without it every frame reads `wasm-function[19893]`.

Ranking targets by time *not* spent in futex/condvar waits finds the emulator
thread. In gameplay, Null backend, throttle off:

| | |
|---|---|
| `fma` | **8.4%** -- the hottest single function |
| recompiled guest chunks (`func_*`) | ~30%+ spread over many |
| `PowerPC::RunLoop` + `chassis_dispatch` | 8.1% |
| `ppc_ps_madds0/1`, `ppc_fmuls`, `dolrecomp_f32_from_bits` | ~4.8% |
| `TexDecoder_DecodeXFB` | 2.4% |
| `VertexLoader::RunVertices` | 1.9% |

**`fma` being top is a wasm-specific cost.** PowerPC's `fmadd`/`ps_madd` round
once, so `cpu_interpreter_float.c` emulates them with C's `fma()` -- and
WebAssembly has no FMA instruction, so that is musl's software implementation on
every call. Roughly 13% of the thread is floating-point emulation once the
`ppc_*` helpers are counted.

**And it corrects a long-standing line in this file.** "About 92% of a Chrome
frame is in VideoCommon" was an OpenGL-path statement inferred from timing GL
calls. With the renderer off, VideoCommon is about **4%** of the CPU thread
(`TexDecoder_DecodeXFB` plus `VertexLoader::RunVertices`). The CPU half is guest
code and float emulation, not the video backend.

**The fused multiply-add is worth about 7%, and it is not free to remove.**
`DOLRECOMP_MEASURE_FAST_FMA` drops the fusion (incorrect: PowerPC rounds once,
this rounds twice) purely to price it. Like-for-like with the profiler running in
both: baseline **93.2%**, without fusion **100.0%**. `fma` leaves the profile
entirely, replaced by an inlined `ps_madd_fast` at 3.0%.

**A median of per-second samples cannot see a 7% change.** By that metric the
same pair read 99% and 93% -- the *wrong way round* -- with ranges of 76-133 and
63-141. Use guest seconds advanced per wall second over a long fixed span
instead; the `[act] guest N (wall M)` lines give it for free and it is a single
robust number per run.

**The exact fix is not the obvious one.** Computing in double and rounding to
single would be exact if the operands were singles, but `force_25_bit(c)` is a
Gekko quirk (c rounded to 25 bits, not 24) and `a` can be a full 53-bit double,
so the product is not exact in double -- which is why the code already carries a
halfway-case correction. A fast exact fma here needs a Dekker two-product
implementation, not a shortcut, and `GXRuntime/tests/paired_single_tests.c` is
what would have to pass.

### The renderer's cost is cross-thread latency, not GPU or CPU work

Profiling the page session *and* every worker (the page must be included -- see
below) in gameplay with OpenGL:

| thread | work | what it is doing |
|---|---|---|
| emulator | **62%** (38% waiting) | `fma`, guest chunks, dispatch |
| **main thread** | **5%** (93% idle) | `bufferData`, `getParameter`, `bindTexture` |
| audio | 1% | the mixer |

**No thread is saturated.** The GL calls run on the *main thread* while the
emulator thread waits, and `em_task_queue_send` at 2.1% on the emulator side is
it paying to get them there. So the renderer's 24 points are largely a
cross-thread round trip per GL call -- latency, not throughput.

**`-sOFFSCREENCANVAS_SUPPORT=1` in `CMakeLists.txt` is inert.** In this
emscripten version `_emscripten_supports_offscreencanvas` is hardcoded to
`return 0`, with a comment proposing a future `OFFSCREENCANVAS_SUPPORT=2` build
mode that would return 1.

**Patching it to return 1 changes nothing, and not because the idea is wrong.**
Measured like-for-like (throttle off, no profiler): baseline **82.9%**, patched
**82.3%**. Reading the emscripten source afterwards says why -- that flag only
decides whether a proxied context also needs an offscreen FBO. The proxying is
decided one branch earlier:

    if (proxyContextToMainThread === 2 || (!canvas && proxyContextToMainThread === 1))
        return _emscripten_webgl_create_context_proxied(target, attributes);

`canvas` is null on the worker, so the context is proxied regardless. The
experiment was **inert, not a disproof.**

**And handing the canvas over is faster and renders nothing.** The build had
`DOLWEB_OFFSCREEN_CANVAS` pinned `OFF` in a stale CMake cache, so the flag never
reached the linker and no transfer code was emitted -- which is why the earlier
patch was inert. Built with it genuinely ON:

| | speed | picture |
|---|---|---|
| proxied GL (canvas on main thread) | 82.9% | **renders** |
| canvas on the render thread | **89.4%** | **pure black** |

Frames are produced (54-68 fps) and committed -- `GLContextEmscripten::Swap()`
does call `emscripten_webgl_commit_frame()` -- and never reach the screen.
Confirmed in **simulator Safari**, the same WebKit as the phone: the DOM
controls draw, the canvas stays black for the whole run. With it off, the same
build plays the intro movie normally.

**The default is now OFF**, and the trap is worth stating: Chrome's
`Page.captureScreenshot` cannot see a transferred canvas either, so a headless
run looks identical whether it works or not. That is how `ON` survived as a
default. **Check this one in the simulator, never in Chrome.**

So the ~14% is real and unclaimable until someone makes WebKit present a
transferred canvas. The 24 points remain the biggest single lever, and the route
to them is not this.

### Measuring speed at all requires turning the throttle off

`MODERNGEKKO_EMULATION_SPEED=0` is pushed by the page **only in `?ab=` mode**.
Any `drive-dolphin.mjs` run without it is throttled to 100%, every number
compresses toward it, and -- as the comment at that line already warns -- "a
throttled build with headroom is indistinguishable from one with none". Pass it
explicitly with `--env`.

Current numbers, Chrome, gameplay window (guest 125-175 s), throttle off:

| | |
|---|---|
| Null (CPU only) | **99%** |
| OpenGL | **75%** |

So the CPU is the wall and the renderer costs a further ~24 points. Both halves
still need work, but the CPU is the one that caps the result.

## 2. The black ground -- FIXED

**A paletted EFB copy was unusable without GPU palette conversion, and WebGL2
has no texture buffer objects.**

The floor is a 256x256 **C4 EFB copy** at `0x009a9e60`. Both builds create the
copy -- identical `[efbcopy]` lines. But `GetTexture` throws it away in the
browser:

    if ((base_hash == entry->hash &&
         (!texture_info.GetPaletteSize() || g_backend_info.bSupportsPaletteConversion)) || ...

For a *paletted* copy that second clause is `bSupportsPaletteConversion`: true on
the desktop, false on WebGL2. So the copy fails the test, is pruned, and Dolphin
decodes the texture out of guest RAM instead -- which is all zeros, because a
copy kept only in VRAM is never written back. Every C4 index is 0, and the floor
is black.

**The fix.** `bSupportsPaletteConversion` conflated two things: "can convert a
palette" and "has texture buffer objects". The palette is at most 512 bytes and
fits in the utility uniform block, so no capability is needed to deliver it. The
flag is split -- `bSupportsTexelBuffer` for the real thing, and palette
conversion is now always available on OGL -- and
`GeneratePaletteConversionShader` gained a third route beside Metal's SSBO and
the texel buffer: a `uint4[32]` in `PSBlock`.

| | guest 138.2 | 143.2 | 148.2 |
|---|---|---|---|
| browser before | 0.887 | ~0.90 | 0.960 |
| **browser after** | **0.015** | **0.016** | **0.062** |
| desktop reference | 0.002 | 0.127 | 0.032 |

Across the whole level window the browser now reads 0.015-0.123, no errors, and
a 76% median speed. The floor renders: water with reflections, the wooden deck,
the cart.

Validated on the desktop first with `MODERNGEKKO_NO_TEXEL_BUFFER=1`, which hides
texture buffer objects so the desktop takes the browser's route: 0.002 / 0.127 /
0.031 against an untouched 0.002 / 0.127 / 0.032.

---

### How it was found, and the six instruments that lied on the way

### The cheap fix works on the desktop and cannot be used here

`EFBToTextureEnable = False` sends copies to RAM as well as VRAM, so the CPU
palette path has real indices to decode. On the desktop with
`MODERNGEKKO_NO_PALETTE_CONVERSION=1` -- the configuration that reproduces the
defect exactly -- it is a **complete fix**:

| | guest 130 | 140 | 150 |
|---|---|---|---|
| CPU palette path, copies in VRAM (the bug) | 0.967 | 0.809 | 0.051 |
| CPU palette path, copies to RAM (the fix) | **0.002** | **0.121** | **0.032** |
| unmodified desktop build, for reference | 0.002 | 0.127 | 0.032 |

Identical to the untouched build.

**And it cannot be used in the browser.** An EFB copy to RAM has to read the
framebuffer back, and readback is impossible in this build -- `OGLStagingTexture`
maps a pixel-pack buffer and WebGL2 has no buffer mapping. Turning it on traps
the page with `Uncaught RuntimeError: null function` and the guest stops dead at
guest 53 s. Reverted, with the reason recorded at the call site.

### So the fix that can work

Make `bSupportsPaletteConversion` **true** for WebGL2. Dolphin's palette
conversion shader reads the TLUT from an SSBO or a `usamplerBuffer`
(`TextureConversionShader.cpp`, `FETCH_PALETTE`), and WebGL2 has neither -- but
the palette is at most 512 bytes and could be uploaded as a small 2D texture and
read with `texelFetch` on a `sampler2D`. Three pieces: a third `FETCH_PALETTE`
variant, a 2D texture to hold the TLUT, and flipping the capability once it
exists.

With that, the paletted EFB copy is usable directly, the floor renders, and
nothing needs reading back -- which is the constraint that rules out every other
route. The next question is concrete: for this draw, which TLUT does the
GPU path apply at draw time, and how does it differ from the one the CPU path
baked in? `ApplyPaletteToEntry` and `texTlut` at the moment of the draw are where
to look.

**Every visual cross-build comparison in this file predates that understanding
and should be read with it in mind**, including the tiling observation below.

### The tiling observation was wrong -- withdrawn

*Kept as a heading because the reasoning below is worth reading and because it
is the fifth lead in this file to dissolve under a proper control.*

`MODERNGEKKO_FLAT_SHADE=uv` appeared to show the floor's coordinates tiling on
the desktop and stretched into a single gradient in the browser, which would
have explained everything at once. **It does not survive measurement.** Sweeping
guest 115-200 every five seconds in both builds and counting sawtooth
discontinuities per scanline in the lower half -- high means the coordinate
wraps -- gives native 2.1-11.0 and Chrome 2.4-10.7. No systematic difference.
The original observation compared a desktop frame indoors against a browser
frame in the back yard.

**Also eliminated: dual-source blending.** WebGL2 has none, and Dolphin's
response is simply to switch the feature off ("force dual src off if we can't
support it"), so any material relying on a second output for destination alpha
composites differently in the browser and only in the browser -- a real
capability gap, and the browser is the only target that takes that path.
`MODERNGEKKO_NO_DUAL_SOURCE=1` forces the same path on the desktop: the floor
stays clean at 0.032 against a 0.031 baseline.

What the original section said, and what is still true of the *reasoning*:

### If the coordinates were wrong it would explain everything

`MODERNGEKKO_FLAT_SHADE=uv` in both builds, in the level:

- **Desktop**: the floor is covered in *tiled* ramps -- the coordinate wrapping
  many times across the surface, bands receding to the horizon.
- **Browser**: the floor is **one smooth gradient**, no repetition anywhere.

The floor is drawn, its coordinates vary across it (the frame is not black:
0.014-0.141 against 0.84-0.98 unpainted), and they simply do not wrap. One copy
of the texture is stretched over the whole surface instead of tiling.

**This is the first hypothesis that fits every observation at once.** If the
coordinate range collapses, every pixel of the floor samples nearly the same
texel; if that texel is dark, the floor is black. The source bytes are
identical, the decoded bytes are identical, the bindings are identical and the
shader is identical -- because none of those is wrong. And painting a texture
flat makes the floor render for the same reason it hid the answer for so long:
**a uniform colour is invariant to the texture coordinate**, so it looks like a
fix whatever the coordinates do.

Note this also retires the older "texture coordinates -- `=uv` shows a clean
tiled ramp on the black floor" elimination in this file. It shows a clean ramp;
it does not show a *tiled* one, and the tiling is the whole point.

The shader normalises by texture size --
`float size_s = float(texdim[texmap].x * 128)` -- and `texdims` is set from
`entry->native_width/height` in `BindTextures`. **That is not it**: the bind
census carries those dimensions now, and across all 82 textures bound in both
builds in the level, not one differs.

So the coordinate scale is right and the coordinates themselves are wrong.
**Texture coordinates are generated in the vertex shader**, and only the pixel
shader has been diffed so far. `MODERNGEKKO_DUMP_VS="<numTexGens>"` prints the
vertex shader for a given texgen count -- 2 for the floor -- so the other half
can be compared.

### A separate defect found on the way: GPU readback traps the wasm build

`OGLStagingTexture::CopyFromTexture` prefers `glGetTextureSubImage` when
`g_ogl_config.bSupportsTextureSubImage` is set. That function is GL 4.5 only and
does not exist in GLES or WebGL, where the loader leaves the entry point null,
so taking the branch traps the whole build with `Uncaught RuntimeError: null
function` -- the emulator never advances past 0 fps.

Two things were wrong. `bSupportsTextureSubImage` is assigned inside an
`if (!m_main_gl_context->IsGLES())` block, so the GLES path never sets it at
all, and the field had no initializer in `OGLConfig.h`. It now defaults to
false, and the call site is compiled out entirely under `__EMSCRIPTEN__`.

**This is not the black ground**, but it means every path that reads a texture
back -- texture dumping, depth readback -- would have killed the browser build,
and nothing would have said why.

**Cross-build savestates would settle the scene problem and do not work.**
`DOLWEB_STATE` exists in the browser build and the state can be reached by
dropping it in the served game tree, but loading the desktop's state wedges the
emulator: 0 fps, ticks frozen at 2390069, pc parked at the entry point, and the
module line comes back with an empty path. The recompiled module identity
differs between a host dylib and a wasm module. So the two builds can be brought
to the same guest *second* but not to the same guest *state*, and every picture
comparison has to allow for that.

**Every cross-build picture before this point was of a different scene**, and
each mismatch was caught only by looking at the image rather than the numbers --
a native flat-shade frame indoors read against a browser frame in the back
garden, a UID frame of Andy's House against one of the garden. `drive-dolphin.mjs`
takes `--shotAt g130,g140,g150` now, matching `MODERNGEKKO_SHOT_AT`, so the same
three numbers give the same three scenes in both builds. That is the third
instrument here to need the guest clock instead of the wall clock, and the rule
is worth stating once: **anything compared across builds must be anchored to the
game, not to the machine.**

### So where it actually stands

Ruled out, each by measurement: the renderer, mipmaps, texture coordinates, GL
errors, failed uploads, an unbound texture unit, shader compilation, the disc
transport (both the whole-file path and the race `dolweb-fetch.js` removes), and
TMEM preload. **Not** ruled out: the recompiled code -- see above, the
interpreter never reached the level.

What is established: **a texture the game draws the floor with has no data
behind it in guest RAM**, and the guest is executing correctly.

**The desktop does NOT reproduce it, and this file said the opposite.**

The claim was: run the same disc on native macOS OpenGL, get the same lines at
the same guest addresses, therefore the defect is Dolphin's rather than this
port's. The lines did match -- but they were the *boot* decode at
`0x001faca0`, the empty-**palette** texture, which is a different defect from
the floor and by all evidence a harmless one. The native run never reached a
level, because `MODERNGEKKO_ACTS` was anchored to **wall clock** while the
browser's `?acts=` is anchored to **guest** seconds, and this build boots from
local disk far faster than the browser streams the same files. At wall second
110 the native run was still in the menus. So the floor's texture was never
tested natively at all, and "the desktop reproduces it" was a conclusion about
the wrong texture.

`MODERNGEKKO_ACTS` is now anchored to guest seconds too (the same string drives
both builds; `MODERNGEKKO_ACTS_WALL=1` restores the old behaviour), and with it
the native build reaches Andy's House and **renders the floor correctly** --
wooden floorboards, correct textures, no black anywhere:

    MODERNGEKKO_ACTS="25:5,40:5,52:5,70:0,76:0,82:0,110:15:9000,135:15:9000,160:15:9000" \
    MODERNGEKKO_SHOT_AT="120:g120,130:g130" \
    MODERNGEKKO_SAVE_STATE_AT="145:/tmp/level.sav" MODERNGEKKO_QUIT_AT=170 \
    ./ModernGekko/build/moderngekko-run --game build-wasm/gexe52 \
      --graphics OGL --audio nullsound

**So the black ground is this port's defect after all.** Same disc, same
recompiled module, same guest moment, same VideoCommon -- and the difference is
the wasm build and its WebGL backend. That is a much better position than the
one this file recorded: the search belongs in the port, and it is now a
differential rather than a hunt.

Three instruments came out of establishing that, and they are the reason the
next attempt is minutes rather than a day:

- **`MODERNGEKKO_ACTS` on the guest clock.** One timeline string reaches the
  same moment of the game in the browser and on the desktop. Without it the two
  builds cannot be compared at all.
- **`MODERNGEKKO_SAVE_STATE_AT="<guest s>:<path>"`** writes a savestate at a
  point in the *game*, and `--load-state` brings it back. Chasing a defect that
  only appears in a level cost a two-minute boot and a scripted menu walk per
  attempt; it now costs seconds.
- **`MODERNGEKKO_SHOT_AT="<guest s>:<name>"`** writes the emulator's own
  framebuffer to `ScreenShots/`, and **`MODERNGEKKO_QUIT_AT=<guest s>`** ends a
  run at a fixed point in the game rather than after a fixed wall time, so two
  runs at different speeds capture the same frame.

**The palette is loaded correctly. One register names the wrong slot.** Over a
run, every TLUT load goes to TMEM 0x40000, 512 bytes, with data -- 15 692 of
them, not one empty. Every TLUT *select* the game issues names that same slot:
`0x000a00`, TMEM 0x40000, format RGB5A3, on units 0, 5 and 6.

The black texture reads its palette from **TMEM offset 0** with format **IA8** --
which is `texTlut` at its reset value, on a unit that never received a select.
And it is not a one-off at boot: over 4 000 decodes come out all-zero in a
three-minute run.

**It is not the recompiled code.**
`STATICRECOMP_FALLBACK_RANGES=80000000-90000000` forces the interpreter in the
same binary, and at 6% of realtime it produces the identical line -- same
texture, same empty palette at TMEM 0, same single select to unit 0. So the
guest is executing correctly and Dolphin's own hardware model is what disagrees
with it; the defect should reproduce on the desktop build too.

That test cost three minutes rather than the hour a full-level interpreter run
would have: **the first empty decode happens at guest 1.2 s, during boot**, long
before any menu. Any hypothesis about this defect can be tested against the boot
alone, which is the single most useful thing to know while chasing it.

What is left is a question inside VideoCommon: the decode runs between the TLUT
load and the TLUT select, so the texture is decoded while `bpmem.tex[].texTlut`
is still at its reset value. Either a draw really is issued before the select
(and the entry it caches is what persists), or the cache is handing that entry
back after the select arrives. `DOLWEB_LOG_TEXTURE=1` prints the whole sequence
in order, and the running "decodes to zero so far" total -- over 4 000 in a
three-minute run -- says it is not a one-off at boot.

Worth knowing while chasing it: **this target is the only one that takes
Dolphin's CPU palette path at all.** `bSupportsPaletteConversion` needs texture
buffer objects and WebGL2 has none, so paletted textures are decoded on the CPU
here where every other backend converts on the GPU. (The same flag also makes
Dolphin hand back a paletted *EFB copy* without applying its palette --
`TextureCacheBase.cpp:1470` -- which is a second, separate hazard on this
target.)

**The repro, three minutes:**

    node drive-dolphin.mjs --backend OGL --seconds 190 --shotEvery 10 \
      --acts "g25:5,g40:5,g52:5,g70:0,g76:0,g82:0,\
              g110:15:9000,g135:15:9000,g160:15:9000"

Frames 9 onward. Judge them by the fraction of near-black pixels in the lower
half of the canvas rect (192,18)-(768,458), not by eye: ~0.9 is the defect, ~0.05
is a good frame, and a screenshot of a fade looks like both.

**Ruled out, each measured, do not re-check:** the depth test, the scissor and
viewport, the XFB, EFB copies and their paths, mipmap completeness
(`MODERNGEKKO_NO_MIPMAP=1`), shader compilation, the texture coordinates
(`MODERNGEKKO_FLAT_SHADE=uv` draws a clean tiled ramp across the black floor),
GL errors (`DOLWEB_GL_ERRORS=1`: none in a full run), failed uploads (none), the
texture cache binding nothing (never), and the disc corruption of section 0.

**And the instrument that lied.** `MODERNGEKKO_FLAT_SHADE=tex` wrote
`textemp.rgb` into `main()`, where the TEV temporaries are out of scope: 114
shaders failed to compile in a three-minute run and the screen went flat grey.
"The inputs are right and the TEV combination produces black", and the
`StreamBuffer` ring and pool experiments that followed from it, came out of a
shader that never ran -- including the ring that "fixed" the ground at a cost of
6x, which was only changing when the texture cache re-decoded. Fixed in
`fd230c5`. **Check that the instrument compiled before believing what it shows.**

## 3. Not yet swept

Menus, Andy's House and Olliewood are all that have been looked at. Every defect
found so far was found by *someone opening a screen nobody had opened*. The
skater carousel is the cheap sweep: each skater starts career in a different
world, so `k` LEFT presses at guest 60 s reaches a different level per run, and
the frame triage above turns a run into one number per frame.

The comparison that has still never been made is **the same scene on the
desktop**. Everything above is Chrome-on-ANGLE, and the black ground could as
easily be VideoCommon as WebGL -- the only thing separating those is a native
run, and `moderngekko-run` has no scripted input to reach the level with.
Teaching it the same `?acts=` timeline (`ciface::Touch::SetControlState`, which
is what the wasm build already uses) is the missing piece.

## The one habit that made today work

**Reproduce it on the desktop.** `MODERNGEKKO_GL_DISABLE=BaseVertex,DepthClamp,…`
turns capabilities off on a Mac that has them; WebGL2 is missing thirteen at
once and the browser is the slowest place to find out which one matters. The
depth defect took one bisect step at ninety seconds each, after a day of
ten-minute guesses in the browser. The fallback paths WebGL forces are exactly
the ones nothing else exercises, which is where the bugs are.

`natrun.sh`-style driving is in the notes: named-pipe controller (synthetic
keystrokes are invisible to Dolphin's macOS keyboard device), `screencapture -R`
over the window rect, and **activate the window first** or the grab photographs
whatever is in front of it.
