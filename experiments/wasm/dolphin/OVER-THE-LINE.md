# What is left, and how to do it

*Written 2026-08-28, rewritten 2026-08-29 after a header turned out to be
corrupting the disc. The objective is unchanged: **Disney skate playing
correctly at full speed in Safari on the iPhone, with nothing installed**, on
code statically recompiled on a Mac. `README.md` is how to build and measure;
`DOLPHIN-ROUTE.md` is why the route exists. This is the part that is not done.*

## 0. Every browser number before 2026-08-31 was taken with Dolphin's log on

`?report=1` appended `DOLWEB_DEBUG_LOG=1` to the module's argv, on the reasoning
that reporting is a diagnostic mode and the run that needs the boot narrative has
usually already happened. So the flag *every* measurement here sets --
`ab-safari.sh`, `sim-run.sh`, `drive-dolphin.mjs`, and every device URL in this
file -- turned on every Dolphin log type with the console listener. The node
harness never set it.

In a pthread build a `printf` from the emulation thread is a postMessage to the
browser's main thread, which is the same thread WASMFS proxies every disc read
through, and Dolphin logs hardest exactly where the game loads most. Desktop
Safari, Null backend, 150 s wall, guest-anchored, 2026-08-31:

| | log ON | log OFF |
|---|---|---|
| cold wasm cache | 45.7% (guest 69 s reached) | 75.2% (guest 101 s) |
| warm wasm cache | **stalled at guest 1.1 s** | **105.9%** (guest 150 s) |

The warm log-ON run is the shape of it: the page stopped posting its own reports
at wall second 5 and never resumed, because its one-per-second timer was queued
behind the log. **Saturation is not a fixed multiplier**, which is why two runs
of the same configuration differ by sixty times -- so do not read "2.3x" as the
correction to apply to an old number. There is no such factor; the old numbers
have to be retaken.

Fixed on 2026-08-31: `?log=N` asks for the log, `?report=1` only reports.

**What this invalidates -- and what it does not.** The window measured above is
boot and menus, which is where the game logs. **Gameplay is barely affected**:
desktop Safari, Null, guest 125-155 s reads **84.1%** with the log off against
the 85.2% this file records from the simulator with it on. So the log is not
what made the CPU look short in the level; **section 0a is** -- the same window
on the module this build should have been linking reads 203-209%.

What it does invalidate is everything that crosses a load: boot time, level-load
time, the "hitching" census, and any comparison against the node harness, which
never set the flag. It is the second time this file has had a browser-only
harness defect masquerade as an emulator one (see section 0b). **When node and
the browser disagree by more than two engines can, suspect the harness first.**

## 0a. The browser build was linking the module README.md says not to use

`build.sh`'s `MODULES` default was `gexe52-c/generated` -- the C backend's 4096
instructions per chunk -- while `README.md` has documented the 256-chunk recipe
since the sweep that found it. Nothing passed `DOLWEB_MODULES`, so
`build-wasm/web/build.ninja` carried 118 chunk compile edges and **every device
and Safari figure in this file was taken on the wrong module.**

It matters more in Safari than the node sweep implies, though not nearly as much
as the first readings here claimed — **a withdrawn 2.3x, then 1.27x, then 1.9x,
all of them scene errors; see the table at the end of this section.** With the
renderer on, the `acts=` timeline extended to keep driving the skater through
the window, two runs per build, every run's scene checked by distinct-PC count:

| chunks | wall to guest 200 | guest speed | samples under 70% | module |
|---|---|---|---|---|
| 4096 | 245 s, 240 s | **92%, 99%** | 3/45, 0/42 | 91.6 MB |
| 256 | 245 s, **205 s** | 100%, 100% | 0/40, 0/40 | 86.7 MB |
| **128** | **205 s, 205 s** | **100%, 100%** | **0/40, 0/40** | **86.2 MB** |

**128 is the default as of 2026-08-31.** It is the only size that both
reproduces and holds 100% with no hitching, and its module is the smallest. The
4096 build does not keep up. 256 straddles -- 245 s then 205 s -- so it is not
the pick despite being what `README.md` recommended.

Node (V8) ranks the same four 4096 < **128** < 512 < 256, putting 128 next to
worst. A 4096-instruction chunk is one `switch (ctx->pc)` in one enormous wasm
function, and JSC is much less willing than V8 to optimise those. **Anything
tuned in the node harness that changes the shape of the generated code has to be
re-measured here** -- which still includes `DOLRECOMP_C_MAX_CALL_DEPTH` and
`DOLRECOMP_C_LOOP_CYCLE_BUDGET`, both currently set from node numbers.

### Frame rate is not the metric; "did it keep up" is

fps over the same guest window, across those six runs: **32.8, 43.0, 37.3, 33.1,
33.6, 47.1** -- with the *slowest* build posting the highest number. It does not
rank the builds and it does not reproduce within a build.

Guest speed does, because the throttle caps it: a build that keeps up reads 100%
with no samples under 70%, and one that does not reads 92% with dips. That is
the number to quote, together with **wall seconds to reach a fixed guest
second**.

### The timeline has to keep driving through the window

The original `acts=` string stops at guest 160. After that the skater goes
wherever physics takes him, so by guest 200 two runs are in different parts of
Andy's Room looking at different amounts of geometry -- and the same 128 build
measured 52.4 fps and then 34.4 over the identical window. Extending the
timeline to hold the stick through guest 300 fixed the scene enough to compare:
distinct-PC counts went from 5-97 to 42-470.

Anchoring to the guest clock fixes the *time*, not the *scene*. This file said
"anchor to the game, not the machine" before any of this and it was still got
wrong three times in one day:

| claimed | from | why it was wrong |
|---|---|---|
| 2.3x | wall to guest 125, Null backend | no picture; scene never verified |
| 1.27x, +24% fps | screenshots, windows 125-178 vs 125-183 | windows of different lengths |
| 1.9x | screenshots, matched window 125-175 | window straddled the level-load boundary, and the loading screen renders a flat 60 fps |

**And check the scene with distinct guest PCs, which is free.** The `[perf]`
line carries `pc=`; count how many distinct values a session visits. Calibrated
against screenshots on 2026-08-31: **42-470 distinct PCs** on every run that
reached the level, **5-7** on runs that came back black -- and note from the
table that a black run posts a *plausible* ~59 fps. What gives it away is that
the value never moves: range 59-59 against 23-60 for a real one.
`phone-window.py` prints the count and a verdict.

### The black screen is intermittent, and it is a shipping problem

Of the simulator runs on 2026-08-31, **three came back with a black canvas** --
all three on the 256 build, which also played Andy's Room on other runs. Nothing distinguished them but luck -- same build, same URL, same
simulator, minutes apart. This is the WebKit canvas defect this file has chased
twice before under other names; what is new is a way to *tell*, from the report
alone, that a run was one of the bad ones -- the distinct-PC count above.

It also means **a black run still posts plausible-looking speed numbers**
(58.7 fps, guest 125 at 130 s, 100% speed) and cost most of an afternoon here by
being read as a regression in the module. Check the PC count before believing a
renderer number, and take at least two runs.

**The phone is measured now -- see section 0c.** It runs the level at ~30%,
which the simulator's 100% on the same build gave no hint of.

### Measure builds by the wall time to a guest second, not by `?ab=`'s speed

The same 4096 build over the same guest 125-155 window came back at **84.1%**
and then **38.3%** (sample ranges 64-106 and 7-103) while its wall time to guest
125 was 150 s both times. The browser's disc fetches freeze the guest clock for
a wall time that varies run to run, so a windowed speed measures whichever
stalls happened to fall inside the window. Equal guest ranges are equal scenes
and equal work; that is the comparison to make. `reach.sh` and
`phone-window.py --marks` do it, and `ab-safari.sh` now prints which of the
`acts=` presses actually fired -- a guest-anchored window says *when* it opened,
never that the game is where the timeline meant to put it.

## 0c. The device, measured properly at last: it runs the level at ~30%

2026-08-31, iPhone 15 Pro Max, Safari, throttled, on the 128-chunk build with
the logging defect fixed and the `acts=` timeline driving the skater through the
window. **100 distinct guest PCs and the owner watching it play**, so the scene
is not in doubt for once:

| in Andy's Room | |
|---|---|
| guest speed, median | **32%** (range 4-65) |
| fps, median | **19.4** |
| samples under 70% | **70 of 70** |
| heap | 614 MB |

The shape matters as much as the median. Menus and the attract loop run at
**100-108%**; from the moment the level loads it drops to 23-54% and stays
there. "Plays but is slow" was the owner's description before the data existed,
and it is exactly right.

**This retires every device row below**, including "95% Null" and the throttled
"100% median" -- those were taken on the 4096 module, with `?report=1` silently
logging, over windows whose scene nothing verified.

**The Mac cannot see this.** The same build in simulator Safari holds 100% with
zero dips. The phone is roughly 3x short of realtime and the simulator says the
opposite, which is the third time this file has recorded a desktop reading that
did not survive contact with the device. Take performance numbers from the
phone; use the Mac for correctness and for A/B of *code shape*, nothing else.

**What is not yet known is which half.** The renderer-off run on the device is
still the missing measurement -- `?backend=Null` with the driven timeline. Near
100% means the renderer is the entire wall; near 30% means the recompiled code
is. Every remaining decision depends on which.

## 0d. It is the renderer, and it is the *number* of GL calls -- not the present

Two device measurements on 2026-08-31, 128-chunk build, driven timeline, scene
confirmed by distinct-PC count and by the owner watching it play.

**Which half.** Guest 110-160, in Andy's Room:

| | speed | fps |
|---|---|---|
| Null (nothing drawn) | **87.4%** | -- |
| OpenGL | **30.2%** | 19.0 |

The recompiled CPU is nearly there on its own -- 87%, about 1.15x short. The
renderer costs **2.9x**. So the CPU work queued in this file (LTO, the
recompiler knobs, the float helpers) is chasing the 13% that is not the problem.

**Where inside the renderer.** `DOLWEB_TIME_SWAP` and `DOLWEB_TIME_GL` on the
device, ~55 ms per frame in the level:

| | per frame |
|---|---|
| the present (`emscripten_webgl_commit_frame`) | **0.15-0.21 ms** |
| all wrapped GL calls | ~11 ms (20%) |
| -- `glBufferData` | **6.2 ms over 1600 calls** |
| -- `glBindTexture` | 0.8 ms over **647 calls** |
| -- `glDrawElements` | 0.8 ms over **535 calls** |
| everything the shim cannot see | ~44 ms (80%) |

**The present is free, and that retires this file's leading renderer
hypothesis.** Simulator Safari measured it at 5.0 ms and ~19% of the frame, with
a note to treat that as a lead rather than a device fact. The note was right:
the device says 0.2 ms, 25x cheaper. `-sOFFSCREENCANVAS_SUPPORT`, the canvas
transfer, and how the frame is committed are all answers to a question that is
not being asked.

**What is left is call count.** The shim wraps 28 entry points; the untimed 80%
is VideoCommon plus the *unwrapped* state calls -- `glEnable`, `glScissor`,
`glVertexAttribPointer`, `glActiveTexture`, `glTexParameteri` -- of which a frame
with 535 draws issues thousands. In WebGL every call crosses into JS and is
validated, so the cost is per call, not per byte.

**And call counts are machine-independent**, which is the first thing here that
can be worked on without a device run per iteration: cut the count on the Mac,
confirm the count dropped, then take one device run to confirm the speed.

## 0e. The video thread, profiled: the cost is diffuse, and ~11% is proxy tax

Chrome, dual core, in the level, ranked by *total* time on the stack rather than
self time -- self time is useless here because -O3 inlines the whole video loop
into its thread entry lambda and 22% of the thread reads as
`GetInitializedVideoGuard::$_3::__invoke`.

| video thread | |
|---|---|
| waiting on a condition variable | **43.5%** |
| `CommandProcessor::SetCPStatusFromGPU` | 9.7% |
| `OpcodeDecoder::RunFifo` | 9.5% |
| `VertexManagerBase::Flush` | 8.6% |
| **emscripten proxy** (`em_task_queue_send`, `do_dispatch_to_thread`, `mailbox_send`) | **~11%** |
| `VertexLoaderManager::RunVertices` + `VertexLoader::RunVertices` | ~7% |
| `AsyncRequests::PullEvents` | 5.2% |
| `Presenter::ViSwap` + `Present` | 5.0% |
| `FramebufferManager::RefreshPeekCache` | 2.6% |
| `glBufferData` | 2.0% |

**There is no dominant cost.** FIFO decoding, vertex loading, the flush and the
present are each 5-10%, which is why no single flag closes the device's 2.9x.
Anyone hoping for one hot function should read this table first.

**~11% is pure proxy tax** -- the price of shipping each GL call to the browser's
main thread, paid only because WebKit will not let the video thread own the
canvas. It is the same ~14% section 1 measured when it transferred the canvas and
got a black screen. Reducing the *number* of GL calls reduces this as well as the
per-call cost, which is the argument for `DOLWEB_GL_DEDUP`.

**And on this Mac the video thread waits 43.5% of the time**, which is why the
Mac cannot rank renderer changes at all: it has slack where the phone has none.
Throttled it is capped at 100%; unthrottled the guest clock races to 1884%. There
is no third mode. Renderer work has to be measured on the device.

### Ruled out by measurement, not by opinion

- **baseVertex batching.** Safari reports 26 WebGL extensions and Chrome 36;
  **neither includes baseVertex** (only `WEBGL_multi_draw`). Dolphin's
  "upload everything to offset zero" fallback is forced by WebGL2, not chosen,
  and that is why a frame issues 1197 `glBufferData` for 466 draws. Replacing
  each upload with per-draw `glVertexAttribPointer` calls costs more at the
  device's measured per-call rate than it saves.
- **Canvas resolution.** Already 640x528 in `index.html`, EFB scale 1.
- **The present.** 0.15-0.21 ms on the device (section 0d).

### Implemented and awaiting a device number

`DOLWEB_GL_DEDUP=1` drops GL calls whose arguments equal the ones already passed
through -- program, VAO, active unit, array-target texture, sampler, and UBO
range. Measured drop: **~54% of the calls it covers** (727 of 1348 per hundred
frames), about 700 of a frame's 3795. Verified by screenshot not to change what
is drawn. Its value on the device is unmeasured.

## 0f. LTO/IPO is +14% on the CPU, and it had never been measured

`build.sh` carried `--lto` and `--ipo` as opt-in flags with a comment saying to
iterate first and measure later; `README.md` called the combination "the slow,
fully optimised link (unmeasured)". Measured 2026-08-31, simulator Safari, Null
backend (no renderer, so nothing is capped and nothing races), throttle off,
guest 200-260 with input driven through the window, every run scene-checked:

| | run 1 | run 2 |
|---|---|---|
| off | 127.8% | 123.8% |
| **on** | **144.7%** | **143.4%** |

**+14%, reproduced.** Both are now the default.

The profile says why. On the emulator thread `ps_madd_fast` is **6.3%**, and
`ppc_fmuls`, `ppc_fma`, `ppc_fadds`, `ppc_psq_load_inline`, `psq_store_value` and
`dolrecomp_f32_from_bits` are another **~7%** between them. Every one is an
out-of-line call into GXRuntime -- *one per guest floating-point op*, taking
register indices and going through `cpu->fpr[]` in memory rather than keeping
values in locals. LTO lets them inline into the chunk that calls them.

**It costs 11 MB of wasm** (86.2 -> 97.5), which matters on a phone whose heap
already reaches 614 MB and which has jetsam-killed a tab. Worth it at +14%, but
it is the reason to keep an eye on `-Os` for the module.

**And it is the first change today that made the emulator faster rather than the
measurements truer.** Everything before it -- the logging defect, the wrong
module, the scene checks -- was repairing what the numbers meant.

## 0g. The recompiler knobs are unresolved, and the machine drifts +/-15%

`DOLRECOMP_C_MAX_CALL_DEPTH` (64) and `DOLRECOMP_C_LOOP_CYCLE_BUDGET` (1024) were
tuned in the node harness and, like chunk size, needed re-checking in Safari.
Raised to 128/2048 and A/B'd against the LTO build, Null, throttle off, guest
200-260, alternating:

| | run 1 | run 2 |
|---|---|---|
| 64 / 1024 | 129.9% | 114.7% |
| 128 / 2048 | 117.4% | 142.8% |

**Indistinguishable** by that method. Both span 115-143%.

**Resolved 2026-09-01 from the savestate** (section 0h), which is what the state
was built for. Null, throttle off, guest 300-360, three interleaved pairs:

| pair | 64/1024 | 128/2048 | difference |
|---|---|---|---|
| 1 | 138.4% | 142.9% | +3.3% |
| 2 | 124.9% | 125.9% | +0.8% |
| 3 | 130.0% | 130.7% | +0.5% |

**Raised knobs are ahead in all three pairs, by ~1.5% on average.** So the
regression this section nearly recorded was noise, and so was the improvement --
the real effect is small and positive.

**The default stays at 64/1024**: 1.5% does not pay for bigger wasm functions on
an engine that demonstrably punishes them (section 0a), and the module is already
11 MB larger since LTO on a phone that has jetsam-killed a tab.

**And note the shape of the data**, because it is the lesson: the *level* drifts
between pairs (138 -> 125 -> 130) while the *within-pair* gap stays under 5
points. Interleaving separates the two; measuring one arm then the other does
not, which is how this looked like -12% and then +25% on the same evening.

**The number that matters more is the spread.** The *same* build measured 144.7%
and 143.4% at 19:5x and then 129.9% and 114.7% at 20:4x -- a 20% decline in one
build over an hour on a Mac that had been running simulators and 14-core builds
all evening. Almost certainly thermal.

The renderer A/B has the same problem and the same verdict. Single core with the
throttle off is the one mode where the Mac *can* rank a renderer change -- dual
core lets the CPU race ahead so the guest clock is meaningless, and throttled it
caps at 100% -- and it reads 56.4% baseline. But over two interleaved pairs:

Three interleaved pairs on a *cooled, idle* machine:

| | runs | mean |
|---|---|---|
| baseline | 51.2, 66.0, 53.4 | 56.9% |
| `DOLWEB_GL_DEDUP` + `SkipEFBCopyToRam,NoEFBAccess` | 66.1, 59.6, 54.3 | 60.0% |

**No detectable effect.** Both arms span 51-66%; the means differ by 3 against a
within-arm spread of 15, and dropping each arm's first run reverses the sign.
Cooling the machine did not reduce the spread, so it is not thermal.

**The harness cannot resolve anything below about 15%**, and that is the finding
to carry forward. Four separate "results" were reported and withdrawn on
2026-08-31 -- +15%, +29%, a knob regression, and a knob improvement -- every one
of them a first-pair reading inside that spread. The LTO result in section 0f
survives only because it reproduced in *both* directions across four runs in a
ten-minute window.

**Before quoting any renderer or knob number, measure the same build three times
and look at the spread first.** If the effect is smaller than that, the harness
has not measured it, whatever the two runs in front of you say.

**And one real bug came out of it.** `DOLWEB_GL_DEDUP` installs the same shims as
`DOLWEB_TIME_GL` -- that is how it intercepts a call at all -- and every shim
opened with a `GLProfileScope` that called `emscripten_get_now()` twice
unconditionally. Two clock reads on each of a frame's 3795 GL calls cost more
than the dedup saved: 62.2% -> 56.2%, a change that looks exactly like a failed
optimisation. The timer is conditional now. **An instrument that is not free is
part of what you are measuring.**

**So: interleave, and check the baseline twice.** An A/B whose two arms are ten
minutes apart on a hot machine measures the machine. This file already says
"check `uptime` before believing an emulator number"; the sharper rule is that a
baseline measured once is not a baseline. The LTO result in section 0f survives
because its four runs were tightly interleaved and the effect reproduced in both
directions inside a ten-minute window.

## 0h. A savestate harness -- and it works

Section 0g is the blocker for everything left: no remaining optimisation is
bigger than the +/-15% this harness spreads, because every measured run boots the
game and drives it with timed input, so by the window the skater is somewhere
different. **A savestate removes that: every run starts from one instant of one
scene.**

    # capture (once), at 235 s of run time, with the acts timeline driving:
    ?savestate=level:235&env=MODERNGEKKO_SAVE_STATE_AFTER=235:/user/level.sav&acts=...

    # then every measurement run starts there:
    ?env=DOLWEB_STATE=/game/level.sav

Verified 2026-09-01. An in-level state is captured and served
(`build-wasm/gexe52/level.sav`, 16.3 MB, listed in `.manifest`).

**And it does what it was built for.** Four *identical* runs from the state, Null,
throttle off: **103.8, 105.0, 104.9, 98.7** -- a spread of 6.3 points on ~103,
about **6% relative**, against the ~25% the same build spread when each run booted
and drove the game (51.2 to 66.0). Roughly four times the resolution. Three of the
four sit within 1.2 points; the fourth is an outlier, so averaging three runs and
discarding outliers should do better still.

That is the difference between being able to see a 10% change and not. Every
result withdrawn on 2026-08-31 was inside the old spread. Four pieces had to line up:

- `MODERNGEKKO_SAVE_STATE_AFTER=<seconds>:<path>` already existed in the shared
  runtime, so the browser build could always write one. Nothing could keep it.
- `/user` is a memory filesystem here, so the state dies with the page.
  `dolweb_read_file` / `dolweb_last_read_size` (main.cpp) hand it to JS -- the
  size comes back through a *second call*, because the build exports only
  `ccall`, `cwrap` and the heap views, not `getValue` or `_malloc`.
- `serve.py` writes the upload into the game tree **and appends it to
  `.manifest`**: a fetch directory only knows the children something inserted,
  so an unlisted file cannot be opened however well it is served.
- The page posts it as a **Blob copied out of the heap**. The heap is a
  SharedArrayBuffer and Safari will not accept one as a fetch body.

**Three false diagnoses on the way, each worth knowing:**

1. "The save is not firing." It fired every time. The *upload* failed, and the
   only evidence was `[state] upload failed: TypeError: Load failed`.
2. The page read the file on a fixed timeout that expired ~2 s *before* the
   emulator wrote it -- the emulator's clock starts at `Run()`, the page's at
   module init about ten seconds earlier. It polls now.
3. The upload kept failing after the code was right, because **`serve.py` was
   still the process started hours before the endpoint was added**. A long-lived
   server does not reload; restart it after editing it.

And the reason all three took so long: the boot narrative scrolls out of the
report `tail` long before a run ends, so the `[state]` lines were in the file the
whole time while every check looked at the last rows. **Grep the whole of
`reports.jsonl`, not the tail of a session.**

**Two things to know before using it:**

- `level.sav` resumes at **guest ~290 s**, so measure over guest 300-340, not the
  200-260 the boot-and-drive runs used. A window the state never reaches produces
  no samples at all and `phone-window.py` says "no perf samples in that session",
  which reads like a broken run.
- **A state captured under one backend does not load under another** -- confirmed
  2026-09-01, not a guess. `level.sav` was captured with `--backend Null`, and
  loading it into an OpenGL session **hangs the boot**: the page sits on
  "Booting the game / starting the console" and the emulator never starts, so the
  run posts no perf samples at all. Null -> Null is verified working.
- **The save under OpenGL crashes the worker that performs it.** I first wrote
  here that it "does not fire at all". That was wrong, and the way it was wrong
  is worth keeping: `[state] savestate written` **does** appear under OGL, so I
  read three runs as silent no-ops when they were nothing of the kind.
  `State::SaveAs` calls `RunOnCPUThread`, which **queues** the job and returns --
  so that line prints when the save is *queued*, not when it is *written*. The
  next line in the log is the real one:

      [state] savestate written to /user/oglvl.sav
      worker sent an error! RuntimeError: call_indirect to a signature that
      does not match (evaluating 'getWasmTableEntry(ptr)(arg)')

  Under Null the following line is instead `[state] uploaded 16346190 bytes`.
  **Do not trust `[state] savestate written` as evidence a state exists** -- check
  for the file, which is what the poller was right to do.

- **It is not a Dolphin bug.** The same code path on the desktop build, OGL,
  wrote a 9.4 MB state first try. The fault is wasm-specific.

- **FIXED.** The cause, from a symbolised stack:

      Fifo::FifoManager::RunGpuLoop
        AsyncRequests::PullEvents
          VideoBackendBase::DoState
            VideoCommon_DoState
              FramebufferManager::DoSaveState
                TextureCacheBase::SerializeTexture
                  AbstractStagingTexture::ReadTexels
                    OGL::OGLStagingTexture::Map      <- null function

  A savestate serialises the EFB and the texture cache, which means reading
  textures back off the GPU, which in Dolphin's GL backend means
  `glMapBufferRange`. **WebGL2 has no buffer mapping of any kind**, so that entry
  point is a null pointer and the GPU thread traps. Nothing but a state takes
  this path, which is why the emulator is otherwise perfectly healthy -- and why
  it took so long to find.

  Both readback sites (`FramebufferManager::DoState` and
  `TextureCacheBase::DoState`) are gated on one flag, so the fix is one line in
  `ModernGekko/src/runtime/dolphin_runtime.cpp`, under `#ifdef __EMSCRIPTEN__`:

      Config::SetBase(Config::GFX_SAVE_TEXTURE_CACHE_TO_STATE, false);

  Verified end to end under OGL: captured a 7.4 MB state, loaded it, and
  screenshotted the menu rendering correctly with all textures. The state is
  half the size of the Null-captured one because the texture cache is no longer
  in it; the textures re-upload on load, which is a frame of work, not a
  correctness problem.

- **Old states do not load under the fix, and that is expected.** On load,
  `FramebufferManager` reads `save_efb_state` **out of the state file**, not out
  of config, so a state captured before this change still tries the readback and
  still traps. Recapture rather than reuse; `level.sav` and `early.sav` are dead.

**How this was found, because the method was worth more than the bug.** Three
things, none of them clever:

1. **Run it on desktop.** OGL saved fine natively in one 75-second run, which
   converted "a savestate problem" into "a wasm problem" and deleted most of the
   search space.
2. **Get the crash off the phone.** Headless Chrome reproduced it in two minutes
   where the simulator took seven, and `--workerlog` on `drive-dolphin.mjs` was
   the thing that mattered: the page only ever sees a worker's crash as a
   forwarded `ErrorEvent` with no frames, and the wasm stack exists only on the
   worker's own CDP session.
3. **Get names.** The build has no name section, so the frames were `$func17915`.
   `ninja -t commands` prints the link line, and patching `LINK_FLAGS` in
   `build.ninja` with `--emit-symbol-map -sASSERTIONS=1` relinks **without
   recompiling anything** -- minutes, not the hours a reconfigure would have
   cost. `build-wasm/web128` is now that symbolised build; keep it.

Two guesses were killed cheaply on the way and neither cost more than a minute:
`Core::DisplayMessage` (it is a mutex and a queue, it touches no GL) and my own
GL instrumentation from 21:27 (`build-wasm/web128`, linked at 19:16, crashes
identically -- so the bug predates tonight).

**So the harness is Null-only, and that bounds what it is good for.** It makes
CPU work measurable -- it resolved the recompiler knobs in one pass after a whole
evening of noise -- and it does nothing for the renderer, because the renderer
cannot be measured without a renderer. **The renderer A/B therefore remains
unmeasurable**, and making `State::SaveAs` work under OGL is the prerequisite for
ever testing a renderer change on this project.
- **Close stale browser windows first.** Sessions left open for hours keep posting
  to `reports.jsonl`, and the analyser takes the *last* session in the file --
  which may be a seven-hour-old tab rather than the run just finished.

**Use this before measuring anything else.** Every result withdrawn on 2026-08-31
was withdrawn because the scene moved between runs.

## 0b. Every number below 2026-08-29 was taken through a broken transport

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

## 1. The renderer on the device (superseded by 0c; kept for the reasoning)

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

**2026-08-31, on the device, throttled (so 100% is a ceiling, not a headroom
reading):**

| iPhone 15 Pro Max | speed median | 2nd half | under 70% |
|---|---|---|---|
| defaults (dual core, canvas requested) | **100%** | 97% | 22% |
| `DOLWEB_SHADER_MODE=1` (ubershaders) | **56%** | 29% | 87% |

**Ubershaders are a regression here** -- they render, but at roughly half the
speed. The per-pixel cost is not worth the absence of compile stalls on this
GPU.

**The hitching is in gameplay, not loading.** Of 344 dips below 70% in a working
session, only **42 landed within three log lines of a file load**. An earlier
sample said the opposite and was taken over a boot-heavy window -- a reminder
that "correlated with loads" has to be measured in the level, not across a boot.
The owner of the device said "the gameplay is slow, I get that the first load
takes a while" before the data did, and was right.

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

> **Retired by section 0a on 2026-08-31.** Both rows were taken on the
> 4096-chunk module. On the 256-chunk one the same window reads **203-209%**
> with the renderer off, so the CPU half is not short on this machine and the
> "splits almost exactly in half" reading does not survive. The paragraphs
> below are kept because the *reasoning* about which window to measure in is
> still right, and because the device has not been retaken.

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

### The largest win measured, and why it is now the default

**Giving the GPU its own thread.** Gameplay, throttle off, frames actually
rendered:

| | fps median | range |
|---|---|---|
| single core | **45.8** | 36-65 |
| dual core | **67.3** | 59-72 |

That is ~47% more frames. It is the renderer's 24 points recovered: every GL call
is proxied to the browser's main thread, so on one thread the emulated CPU
blocks on a round trip per call -- the profile has it waiting 38% while the main
thread sits 93% idle. A second thread absorbs the latency.

It also retires "dual core is an 11% loss": that was measured in the **menus**,
where the renderer is nearly free and the extra thread only costs
synchronisation. In gameplay the renderer is the thing being waited on.

**It is the default as of 2026-08-31, and the reason it was not is retracted.**
The file used to record this:

| run | shots showing a picture |
|---|---|
| first | **0 of 10** -- 200 s of black, guest clock at 100%, 0.0 fps |
| second | 10 of 10 |

The first run took **no screenshots at all**. `simctl openurl` had timed out
while the page loaded and ran fine, `sim-run.sh` was under `set -e`, and the
detector scored zero screenshots as zero pictures. The page it supposedly killed
went on to run for sixteen hours at 99% speed. No dual-core session in
`reports.jsonl` has ever shown 0 fps at a full-speed guest clock. Re-run with
the timeout tolerated: 3 runs, 3 rendered.

**But the guess underneath it was right.** There *was* a canvas defect, in the
other engine:

| Chrome, gameplay | near-black @ g130 | fps | speed |
|---|---|---|---|
| dual core, before | **0.993** | 0 | 100% (throttled, so blind) |
| single core | 0.428 | 39.8 | 67% |
| dual core, after | **0.428** | **51.5** | **86%** |

`StartCanvasOwningThread` decided whether to request a canvas transfer from
`wsi.type == WindowSystemType::Emscripten` -- always -- without asking whether
the build could perform one. It cannot: `DOLWEB_OFFSCREEN_CANVAS` is OFF, so
nothing links `-sOFFSCREENCANVAS_SUPPORT`. Asking anyway still routes the spawn
through the browser's main thread by postMessage. The **emulation** thread
survives that, because it has a claim/fallback guard that starts a plain thread
when the canvas-carrying one never arrives; the **video** thread has no guard,
and it initialised video without error, logged no warning, and drew to something
the page never shows. Steady state after the fix is 60.0 fps at 100%.

So the prediction in this file -- "the same shape as the OffscreenCanvas failure
-- WebKit, threads and a canvas -- and understanding that one probably explains
both" -- was correct. One cause, two symptoms, two engines. What was wrong was
which engine had it.

**Use frames rendered, not guest time, to judge dual core.** Guest seconds per
wall second reads 82.9% -> 100-115%, but with a second thread the CPU runs ahead
of the GPU, so that metric flatters it. The fps figures above are the honest
ones.

**And change one thing at a time.** `-sGL_TRACK_ERRORS=0` (emscripten records a
GL error code per call; nothing here reads it) was committed together with the
dual-core default, and the combined build came back with the guest clock racing
at 3700%, 2.2 fps and a black screen. Reverting it and rebuilding with dual core
alone was fine -- so the GL flag was the fault -- but attributing it cost a run.

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

## 2b. Performance: what to do next (written 2026-08-31)

**Read sections 0 and 0a first.** Both of the day's wins were defects in what
this file was measuring *with*, not in the emulator: `?report=1` was switching
Dolphin's log on, and the browser build was linking the 4096-chunk module while
`README.md` documented the 256 one. Neither was on the list below. The list is
still worth having, but the habit that found these two is worth more than any
item on it -- **check what the harness is doing to the thing it measures, and
compare builds by the wall time to a guest second rather than by a windowed
speed.**

On the fixed harness and the 256-chunk module, desktop Safari in gameplay reads
**203-209% with the renderer off**, against the 85% this file recorded. The CPU
half is no longer the wall on that machine. The device is a different machine
and has not been retaken.

The rest of this section predates all of that. Treat its numbers as history.

> **Do not do this. Tried on the device 2026-08-31 and it is not a
> measurement.** Unthrottled, with dual core, the emulated CPU runs free while
> the phone's GPU lags, and the guest clock decouples from the game: it reached
> **guest 10 016 s in 295 wall seconds** (~3500%) while the frame counter
> reported only three distinct values all run (`0.0`, `43.1`, `39.7` — frozen).
>
> The damage is not just to that one number. **Every instrument here is
> anchored to the guest clock**, so all of them break together: the `acts=`
> timeline collapsed -- presses meant for guest 25/70/76/82/110/135 fired at
> guest 53, 124, 124 and 204 -- and the game was never navigated into a level
> at all. `?ab=`'s window and `reach.sh`'s marks have the same dependency.
>
> On this Mac the GPU keeps up well enough that none of this shows, which is
> why the advice below looked reasonable when it was written.
>
> **Run the device throttled** (just drop the `env=`). 100% is then a ceiling,
> and that is fine, because the ceiling is not what the device question is: the
> honest readings are **fps** and **the fraction of samples under 70%**, plus
> the wall time to reach guest 125 (≈125-135 s means it is keeping up). Those
> are comparable with the throttled device row in section 1.

**Start here: take one unthrottled reading on the device.**

    ...index.html?auto=1&report=1&env=MODERNGEKKO_EMULATION_SPEED=0

Every phone number in this file is **throttled**, so 100% is a ceiling and not a
headroom reading -- it cannot tell "comfortably fast" from "barely keeping up
and dropping frames", and the person holding the phone reports the second while
the log shows 100%. Unthrottled, the device renders normally and stays playable,
which is why this is a better instrument than `?ab=`, whose Null half is a black
screen that gets closed before it produces a result.

The answer forks the work, and until it exists both branches are guesswork:

- **above 100%** -- there is headroom, and the dips are spiky work. Go after
  texture uploads, EFB copies, and shader compilation, in that order. 88% of
  dips are *not* near a file load, so they are in the frame, not the disc.
- **well below 100%** -- the renderer is a wall and the fix is structural.
  Then the question is what the proxied GL call path costs on the device, which
  dual core reduced but did not remove.

**Ruled out, with evidence:**

| | why |
|---|---|
| LLVM backend | 213.8 MB vs 86.5 MB, overflows the stack at boot, on a build already at 614 MB on a device that has jetsam-killed the tab |
| Ubershaders | measured on the device: 56% against 100% |
| Internal resolution | already 1x native; `GFX_EFB_SCALE` defaults to 1 |
| The CPU | 95% with the Null backend on the phone |
| Disc read-ahead | written, then reverted: only 42 of 344 dips are near a load, so it targets the wrong thing. Its 22%-vs-97% "regression" was contention, not the change -- do not cite that number |

**Still untried, in order:** the recompiler knobs (`DOLRECOMP_C_MAX_CALL_DEPTH`,
`DOLRECOMP_C_LOOP_CYCLE_BUDGET`), tuned in the menus and never retuned for
gameplay -- the budget must stay well under 4096 or the ARAM-init livelock
returns; then texture upload batching; then EFB copy elimination.

**Two knobs exist so a device hypothesis costs a URL and not a rebuild:**
`DOLWEB_SHADER_MODE`, `DOLWEB_CANVAS_THREAD`. Add more of these rather than
rebuilding -- only the person holding the phone can run the test.

**Before believing any slowdown, check the machine is quiet.** Leftover
emulators made three separate changes look like 5x regressions in one session:
simulator tabs, simulator Safari, desktop Safari. `ps -Ao pcpu,comm -r | head`,
and ask whether `reports.jsonl` is still growing when nothing should be running.
`sim-run.sh` and `safari-run.sh` now both close by the real window title, before
as well as after.

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

## 0j. Real tail calls: chassis dispatches -90%, CPU +15.6% (2026-09-01, afternoon)

The dispatch-site histogram (`?env=STATICRECOMP_DISPATCH_SAMPLES=1`, the top 16
sites are in the shutdown log) said where the 9 dispatches per 1000 guest cycles
came from, and it was not calls:

- **7% of all dispatches were one site, `0x800038E0`**: the fallthrough off the end
  of `chunk_3707_data0_800036E0` into the next chunk. A paired-single loop
  straddles that boundary and crossed it twice per iteration, each crossing a
  `ctx->pc = ...; return;`. Fixed-stride chunks cut loops as readily as functions.
- Most of the rest were **return continuations** (`8004EDC0`, `8004EDC8`,
  `8004F488`...): a direct call's callee hit something it could not follow in place
  -- a `bctrl`, mostly -- and returned with a foreign pc, so every host frame above
  it unwound and every one of them became a chassis dispatch on the way back.

Three emitter changes (`backend/emitter.c`, `backend/dispatch.c`,
`app/pipeline.c`), all on when `DOLRECOMP_DIRECT_CALLS=1 DOLRECOMP_TAIL_CALLS=1`:

1. The chunk-end fallthrough is a gated transfer into the next chunk.
2. `bctrl`/`bctr`/`bclrl` resolve at run time through a generated
   `dolrecomp_chunk_index_of()` and `dolrecomp_chunk_table[]` (its one definition
   is `chunks/dolrecomp_chunk_table.c`, because the manifest is never compiled),
   gated like a direct call. A `blr` still returns: a host frame is waiting.
3. Every transfer with nothing to resume into -- cross-chunk `b`/`bc`, `bctr`, the
   fallthrough -- is `DOLRECOMP_TAIL_CALL(f(ctx))`, which is
   `__attribute__((musttail)) return f(ctx)` when clang defines
   `__wasm_tail_call__` (`-mtail-call`, now in build.sh's default CFLAGS) and the
   old bounded host call elsewhere. wasm `return_call` has been in Safari since
   18.2; the Mac's `jsc` validated and ran one, and the phone is on iOS 26.

**And a bug**: `pipeline.c` handed the emitter a chunk table rebuilt per *section*
from the chunks emitted so far, but the index it yields is passed to the gate,
whose `chunk_open[]` follows the complete address-sorted list. This DOL's data0
sits below text1, so every text1 call asked about the chunk four below its target
(2708 for 2712). Harmless only while no chunk is closed. The table is now built
over every section before any chunk is written.

    module                dispatches / 1000 cycles   Null M ticks/sample   OGL
    gexe52-c128-tail             9.02                  1051.5 median        923.2
    gexe52-c128-tc               0.94                  1215.6 (+15.6%)      991.6 (+7.4%)

Null: four interleaved rounds, arms 1045-1085 against 1179-1237, **no overlap**.
OGL: three rounds, one pair overlapped (the user's Chrome was busy on the machine);
the CPU half is the number. `./ab-state.py A B --rounds N` is the harness. Renders,
`smc_failed=0`, 60 fps, module 95.0 -> 90.4 MB.

**Then the remaining 0.94 were loop-guard trips**, one per 1024 accumulated
cycles, each unwinding the host stack. An in-module dispatch loop
(`DOLRECOMP_MODULE_LOOP=1`, gate flag `STATICRECOMP_GATE_CONTINUE`) takes chassis
dispatches to one per slice (0.16/1000) -- and the first attempt was *slower*,
because the module never flushes its charge accumulator, so past 1024 every guard
in every chunk tripped on every iteration. The guard now flushes through a gate
callback (`gate->flush`, the same accounting a hook gets on entry, timebase
included) and reads the live slice instead of a fixed budget. Result against the
tc build: **-0.4%, arms overlapping -- neutral**. The chassis round trip was
already cheap once there were only 0.45 M of them a second. Left in, opt-in.

**Still a Mac number.** The phone's CPU thread read 87% with Null; +15% there is
the difference between the CPU thread keeping up and not. The renderer thread is
the other, larger half on the device and nothing above touches it.

### The same change on the phone's engine: +4-6%, and why

The iOS Simulator runs the phone's JavaScriptCore on this Mac, and from the
gameplay savestate it reads **within 1% of the device** (Null 842/843 M
ticks/sample here against the phone's 843.7 in the record). So it is the JSC
harness, and it says the tail-call build is **+4.2-5.7%** there (842.0/843.4 ->
890.4/878.4, two interleaved rounds), not V8's +15.6%. Two reasons:

- **JSC runs this module at 0.69x of V8 on the same machine** (842 against 1215).
  Whatever the dispatch change saves is a smaller fraction of a slower baseline,
  and the phone's CPU half is a B3-codegen question before it is a dispatch one.
- `jsc --options` lists the tiering knobs the phone runs with:
  `maximumOMGCandidateCost=100000` (a function over that never reaches the
  optimising tier -- a 4096-instruction chunk is ~1 MB of wasm, a 128 one is
  20-41 KB, which is the 2.3x cliff of 0a and why 256 and 128 tie),
  `thresholdForOMGOptimizeAfterWarmUp=50000` at 15 per entry,
  `useWasmTailCalls=true`, `useWasmFastMemory=true`.

Real tail calls against the host-call fallback for the same module, in the
simulator: +4.5% then a tie, with both arms dropping 15% between rounds --
the previous run's WebContent was still spinning at 100% CPU (sim-run.sh now
terminates Safari afterwards as well), and a Unity build was running on the
machine. Kept: never slower, and it removes the host-stack growth a loop across
a chunk boundary used to cost.

### 64-instruction chunks: +2% on both engines, and the floor is gone

The node sweep stopped at 128 because every crossing was a chassis dispatch,
and 128 was where the crossings cost more than the smaller switch saved. They
are tail calls now, so the trade-off moved. `DOLRECOMP_C_CHUNK_INSTRUCTIONS=64`
(the floor in `pipeline.c` was 128; it is 16), 7417 chunks, otherwise the same
module recipe:

    engine                        128-tc              64-tc
    headless Chrome (Null)        993.4 median        1013.3 (+2.0%, arms do not overlap)
    iOS Simulator JSC (Null)      762.3 / 746.7       772.1 / 771.9 (+1.3% / +3.4%)

Renders, `smc_failed=0`, 90.4 -> 90.0 MB. It is the default module now. 32 is
untried, and the noise on a machine running someone's Unity build is larger than
the +1% it might add; try it when the Mac is idle.

### The video thread, sized on the Mac, and the number to take to the phone

`gpu=NN%` in the perf line is new: the GPU thread's busy fraction over the
sample (one clock read per wakeup, only when it had work). Mac, dual core,
throttled, from the gameplay state, 60 fps: **21%**. Batching the FIFO reads to
1 KB per pass (the atomics per 32-byte block are all seq_cst on wasm) moved it to
20.6% -- neutral here, kept as a device hypothesis.

Do the arithmetic with the phone's known numbers and the video thread is the
whole problem: the phone's CPU thread is ~1.4x slower than this Mac's (843 vs
1215 M ticks/sample, Null), but at 19 fps and 30% speed its video thread is
spending ~50 ms a frame against this Mac's 3.5 ms -- **~15x**. Dolphin's own
video work does not scale that way between an M4 and an A17; the WebGL call
path does. A frame issues ~3800 GL calls, and on iOS every one crosses into
WebKit's GPU process. That is the lever, and it is only measurable on the
device.

**What to run on the phone** (serve with `python3 ../serve.py dolphin --lan`,
open the HTTPS address; every run is a URL, no rebuild):

    # 1. the CPU half, before/after today: renderer off, throttle off
    index.html?backend=Null&seconds=90&report=1&auto=1&env=DOLWEB_STATE=/game/oglplay.sav&env=DOLWEB_CPU_THREAD=0&env=MODERNGEKKO_EMULATION_SPEED=0
    #    ./use-build.sh web128 for the old module, snap-c64 for the new; then ./state-rate.py --ua phone

    # 2. the shipping configuration, read gpu= and fps
    index.html?backend=OGL&seconds=90&report=1&auto=1&env=DOLWEB_STATE=/game/oglplay.sav

    # 3. the same with redundant GL state calls dropped (neutral on the Mac; the phone is where a call costs)
    index.html?backend=OGL&seconds=90&report=1&auto=1&env=DOLWEB_STATE=/game/oglplay.sav&env=DOLWEB_GL_DEDUP=1

    # 4. per-call GL cost on the device, which has never been taken there
    index.html?backend=OGL&seconds=90&report=1&auto=1&env=DOLWEB_STATE=/game/oglplay.sav&env=DOLWEB_TIME_GL=1

If (2) reads gpu= near 100% and (4) shows the state calls costing microseconds
each, the next project is fewer GL calls per frame -- uniform-bind elision,
texture-bind dedup, and deferring draws so several uploads become one -- and it
should be sized from (4), not from anything this Mac says.

## 0i. The renderer became measurable, and it says "call-bound"

Fixing the OGL savestate above turned a harness that could not rank a renderer
change into one that resolves **half a percent**. Both numbers below are guest
ticks per perf sample, single core, throttle off, started from a state, headless
Chrome on the Mac.

**The harness.** Capture once, then every run starts in the identical scene:

    # capture (throttled, acts drive the skater into the level)
    ?savestate=oglplay:235&env=MODERNGEKKO_SAVE_STATE_AFTER=235:/user/oglplay.sav&acts=...
    # measure (unthrottled, single core, from the state)
    ?env=DOLWEB_STATE=/game/oglplay.sav&env=DOLWEB_CPU_THREAD=0&env=MODERNGEKKO_EMULATION_SPEED=0

This is strictly better than driving the skater with `acts=` and hoping two runs
look at the same geometry -- the problem section 0a spent a day on. A state fixes
the *scene*, not just the time, and Dolphin replays deterministically from it, so
the drift after the input stops is identical in every run.

    scene            spread over 3 runs
    boot, no state   +/- 15 %
    Null state       ~6 %
    menu state       1.9 %
    gameplay state   0.5 %      <- 884.8 / 882.8 / 882.8

**Measure in the level, not the menu.** The menu runs at 1815 M ticks/sample and
the level at 883 -- gameplay is twice the work -- and the two disagree about
results, so a menu state is not a proxy for anything.

**GL state dedup does nothing. Closed.** `DOLWEB_GL_DEDUP=1`, interleaved
A/B/A/B/A/B so drift hits both arms:

    baseline   884.8  882.8  882.8      median 882.8
    dedup      886.1  881.8  884.7      median 884.7

The arms overlap completely. (In the *menu* it read -1.4 % with no overlap, which
is exactly why the menu is not a proxy -- do not resurrect it on that number.)

**The renderer is call-bound, not fill-bound.** Internal resolution 1x against
2x, which is four times the fragment work:

    scale=1 (1x pixels)   883.7  884.4  879.1     median 883.7
    scale=2 (4x pixels)   881.5  877.0  874.6     median 877.0

**Four times the pixels costs 0.8 %.** Fragment work is free here; the cost is
per-call, crossing wasm -> JS -> WebGL, plus the buffer uploads. That is the
first hard evidence for the thesis section 0f only argued from a profile: the
lever is **fewer GL calls**, and nothing that reduces pixel work will move this
number. It also means internal resolution is free -- the page could render at 2x
for nothing, which is a user-visible win available today.

Caveat, and it matters: **this is V8 on a Mac GPU, not JSC on a phone.** The
direction should hold or strengthen on Safari, where the per-call tax is higher
and the GPU weaker, but "should" is how four results were withdrawn last night.

**The Safari cross-check was attempted and did not produce a number.** Recorded
because the next person will otherwise try it the same way and lose the same
hour. `safari-run.sh` reports *median speed %*, and with the throttle on that
caps at exactly 100 -- all four runs (1x and 2x pixels, twice each) returned an
identical `55.5 fps / 100% / 5 under 70%`, which is the metric refusing to
discriminate, not the two arms being equal. Its report parsing also double-counts:
the page posts its last three perf lines every time, so a 95 s run reported 182
"samples". Pulling ticks out of `reports.jsonl` instead disagreed with its own
speed figure -- 16 distinct tick values spanning 0.3 B ticks, which would be a
near-frozen guest, against a reported 100 %. One of those two readings is wrong
and I did not establish which.

### Null results, so nobody pays for them twice

**Audio output is free.** 889.2/888.2/883.1 against 886.5/885.1/883.7 with
`DOLWEB_NO_AUDIO=1` -- the arms overlap. Narrower than it sounds, though:
`"No Audio Output"` swaps the *output backend*, it does not switch off DSP
emulation, so this says nothing about what the DSP costs.

**`native_exc` is not a C++ throw.** The shutdown line reports ~2500 a second and
the build links with `-fexceptions` (the slow JS exception path, not
`-fwasm-exceptions`), which looks like a large, cheap win until you read
`StaticRecompCore_Run.cpp:368`: it counts *guest* PowerPC exceptions, and
handling one is a flag check in the dispatch loop. Nothing throws. Moving to
`-fwasm-exceptions` may still be worth measuring for codegen reasons -- landing
pads inhibit optimisation even where nothing throws -- but it is a whole-module
rebuild and there is no evidence for it yet.

### The loop budget is saturated at 1024. Closed.

`DOLRECOMP_C_LOOP_CYCLE_BUDGET` 1024 -> 2048 (still under the 4096 guard), on the
spare `web128` so the served build was never at risk, both arms measured after
the same reconfigure so the link flags match:

    budget 1024   899.0  893.7  890.9     median 893.7
    budget 2048   898.3  890.8  891.8     median 891.8

The arms overlap completely. **The knob is done** -- 256 -> 1024 was worth
+10.1 %, and the next doubling is worth nothing, so the default is in the right
place and nobody needs to sweep it again.

### The +14% LTO win is withdrawn

It was the only optimisation banked on 2026-08-31 and it was made the default
that night. Re-measured on 2026-09-01 from a savestate, three runs an arm,
interleaved, spreads 0.3-0.4%:

    backend   LTO+IPO off              LTO+IPO on
    OGL       883.1  891.2  890.2      879.7  874.3  878.4      -1.3%
    Null     1076.4 1075.6 1080.4     1063.7 1061.8 1064.5      -1.2%

**Consistently slower, in both backends, with no overlap between arms.** The
original was two runs an arm on the boot-anchored instrument, whose spread is
+/-15%; +14% sits comfortably inside that. It also costs 11 MB of wasm
(86.2 -> 97.5), which a phone pays on every cold load. The defaults in
`build.sh` are back to OFF and the served build is `web128`.

**One caveat, kept on purpose.** Both re-measurements are V8 and the withdrawn
+14% was simulator Safari, so JSC is unmeasured -- and this file's own rule is
that anything changing the shape of generated code must be re-measured on
WebKit. Desktop Safari was attempted and does not work at this run length:
`safari-run.sh` restarts the browser between runs, so each one re-downloads
~100 MB cold and yields about four samples in 95 s. It needs ~300 s runs. Until
someone does that, "LTO off" is the conservative setting rather than a proven
one -- the change was made on evidence that has since failed to replicate, so
the burden is on turning it back on.

**The lesson is the one this file keeps re-learning.** Every withdrawn result on
this project -- now five -- was a first-pair reading on an instrument whose
spread was never measured. The fix is not to be more careful when reading them;
it is to measure the spread first and refuse to quote anything smaller.

### wasm SIMD builds after all, and buys nothing

The build comment says `--simd` "does not currently work" because `-msimd128`
reaches `xxhash.h`, which then includes SIMDe's C++ headers from inside its own
`extern "C" {`. That is a **C++-only** collision, and `--simd` puts the flag in
both `CMAKE_C_FLAGS` and `CMAKE_CXX_FLAGS`. Passing it through `DOLWEB_CFLAGS`
alone reaches the generated chunks and GXRuntime -- the only code that could use
it -- and builds clean:

    DOLWEB_CFLAGS="-msimd128 -DDOLRECOMP_C_MAX_CALL_DEPTH=64 ..." ./build.sh ...

Interleaved against an identical non-SIMD build, gameplay, from the state:

    no simd   889.1  873.6  885.7     median 885.7
    simd128   886.8  874.4  877.6     median 877.6

**Nothing**, which confirms the node-kernel result in the real game on the sharp
instrument. The reason is structural, so it is not worth retrying: the generated
code is scalar per guest instruction and there is nothing to vectorise across
them, while a paired single is two f32 in a four-wide vector -- half the lanes
idle, plus the shuffles to get in and out. **SIMD is closed.**

### Call depth is saturated too, and so is chunk size in V8

Same harness, same spare build, same protocol:

    call depth  64    899.0  893.7  890.9     median 893.7
    call depth 128    885.9  891.2  889.6     median 889.6

**Both dispatch knobs are done.** 24 -> 64 was +6.9 % and 64 -> 128 is nothing;
256 -> 1024 was +10.1 % and 1024 -> 2048 is nothing. The dispatch overhead has
been wrung out, and **further CPU gains have to come from better generated code,
not from tuning what is there.** That is the useful half of two null results.

Chunk size 128 against 256, builds identical but for the module (`web128` and
`web256`, both LTO/IPO off, depth 64, budget 1024):

    chunk 128   891.8  889.9  881.4     median 889.9
    chunk 256   903.5  887.8  877.0     median 887.8

No difference -- and **the node sweep's claim that 256 is 12 % faster than 128
does not reproduce in a browser**, which is one more reason not to tune this
project in node.

**Watch for thermal drift on a long measuring session.** Both arms above fall
monotonically across the ten minutes they took -- 1.2 % and 2.9 % -- after about
seven hours of continuous runs. Earlier in the night the same harness drifted a
few tenths. Interleaving A/B/A/B keeps a comparison honest under drift, but when
the within-arm drift is bigger than the effect, the answer is to stop measuring
and let the machine cool, not to average harder.

**Two things about the build system, learned the hard way.**

*Change a compile flag through CMake, not `build.ninja`.* The flag appears in
4351 edges but only two distinct `FLAGS =` lines, so one `sed` looks like it
works. It does edit the file -- the edit was still there afterwards -- but
`ninja` schedules a `Re-running CMake` edge first, which regenerates
`build.ninja` from `CMakeCache.txt`, and creating any file in the build
directory (I left two `build.ninja` backups there) is enough to trip CMake's
globbed-directory check and schedule it. The first attempt rebuilt nothing and
still exited 0. Re-running `build.sh` with `DOLWEB_CFLAGS` is the supported
route and takes about four minutes for all 3711 chunk objects -- far cheaper
than it looks. Note a regeneration resets the link flags, so the
`--emit-symbol-map -sASSERTIONS=1` patch from 0h does not survive one: measure
both arms after the same reconfigure, or the baseline is an assertions build and
the test arm is not.

*`find -newermt "-30 minutes"` silently matches nothing on macOS.* BSD `find`
does not take a relative time there, so every "has it started rebuilding yet"
check returned 0 and made two builds look like no-ops. **Judge a rebuild by
object mtimes** (`stat -f %Sm`), or by whether `dolweb.wasm`'s timestamp moved.

### The shipped module has no direct cross-chunk calls at all

Found 2026-09-01 while looking for a way to cut the dispatch cost, and it
reframes several earlier results.

Every cross-chunk transfer in `gexe52-c128` returns to the chassis:

    chunks containing dolrecomp_call_enter :      0 / 3709
    direct func_XXXXXXXX(ctx) call sites   :      0
    "ctx->pc = 0x...; return;" sites       : 492,202

The recompiler *can* emit a direct call -- `emit_cross_chunk_call` in
`emitter.c` runs the target chunk as a host call and resumes at the
continuation label -- but it is behind `DOLRECOMP_DIRECT_CALLS=1` (gated, which
asks the chassis's dispatch gate and so keeps the SMC guard exact) or
`DOLRECOMP_UNSAFE_DIRECT_CALLS=1`. **Neither was used for this module**, and both
`dolrecomp` binaries on disk (2026-08-26 and 08-27) predate the feature entirely
-- the string is not even in them. Rebuild `dolrecomp` before assuming a
generation flag did anything.

**So `DOLRECOMP_C_MAX_CALL_DEPTH` is a no-op for this module.** Nothing calls
`dolrecomp_call_enter`, so the depth ceiling governs nothing. That kills the
+6.9% recorded for 24 -> 64 -- another first-pair reading on the +/-15%
instrument -- and it explains why 64 -> 128 measured as exactly nothing here.
Two knobs in `build.sh` are therefore decoration on this module; only the loop
cycle budget does anything.

**And it is why a chunk-lookup cache cannot work.** Consecutive dispatches land
in the same chunk **0.4%** of the time (1.64 M of 426 M, counted and then
removed). With no direct calls, essentially every dispatch is a cross-chunk
transition arriving through the chassis, so there is no locality to cache.

### Turning direct calls on: -38% dispatches, +1.6% (the first real win)

Both arms generated with the **same freshly built `dolrecomp`**, so the flag is
the only difference; both compiled with identical flags, LTO/IPO off, chunk 128.
Gameplay from `oglplay.sav`, OGL, single core, throttle off, interleaved:

    no direct calls   881.4  888.1  880.8     median 881.4
    direct (gated)    895.9  895.9  898.4     median 895.9      +1.6%

The arms do not overlap. **And the mechanism is confirmed by a count rather than
a timing**, which no amount of thermal drift can move -- two 45 s throttled runs
over the same guest work:

    shipped module   native=387,337,227 dispatches
    direct (gated)   native=238,560,381 dispatches      -38%

`smc_failed=0`, `verifications=1367`, and the scene renders correctly, so the
gated variant is keeping the SMC guard while removing two chassis round trips in
five. Module grows 86.2 -> 90.3 MB.

**Why only +1.6% for -38% of dispatches:** `RunLoop` is 8.9% of the thread, so
removing 38% of what it does is worth about 3.4% of CPU, diluted by the renderer's
share of the frame. The dispatch *count* was never the whole cost -- the work
inside the chunks is.

**The Null cross-check was inconclusive and is not quoted.** By the time it ran,
after hours of building, the same harness that had held 0.3-0.4% under Null was
spreading 4.9% and 6.6% with both arms declining monotonically. The OGL pair
above was taken while spreads were still tight. **Re-confirm on a cooled machine
before treating +1.6% as exact**; the -38% needs no such caveat.

### And native tail calls on top: -50% dispatches against stock

`emit_cross_chunk_call` only ever fired for `bl`, because it needs a local
continuation to `goto` after the call returns. A cross-chunk `b` -- a tail call --
has no continuation, so it kept doing the full chassis round trip even with
direct calls on. It does not need one: run the target as a host call and then
return, and whatever it left in `ctx->pc` is what the chassis dispatches next.
That still removes one dispatch, and `dolrecomp_call_enter` bounds the recursion
a chain of tail calls builds.

Added as `emit_cross_chunk_tail` behind **`DOLRECOMP_TAIL_CALLS=1`** (which needs
`DOLRECOMP_DIRECT_CALLS=1`, since it reuses the same chunk table and gate).
Native call sites 30,044 -> 38,199. Dispatches per 1000 guest cycles:

    stock module     18.2
    direct calls     11.35  11.34  11.31
    + tail calls      9.05   9.05   9.05      -20% again, -50% against stock

The tail arm is identical to two decimals across three runs, which is what a
deterministic instrument looks like. Verified over a 90 s run: renders correctly,
60 fps, `smc_failed=0`, no stack overflow.

**The timing for this half is directional only.** 896.3/882.4/869.3 against
897.7/890.3/887.2 is +0.9% at the median with the arms overlapping, on a machine
that by then was spreading 3% within an arm. The dispatch count is the result;
the timing needs a cooled machine to pin down.

**End to end, stock codegen against both changes** (same binary generated both
modules, identical build flags, interleaved):

    dispatches/1000 cycles   18.25 18.24 18.22   ->   9.05 9.05 9.05   -50.4%
    guest ticks per sample   881.1 879.5 871.6   -> 895.5 893.6 889.5   +1.6%

The arms do not overlap. **But direct calls *alone* also measured +1.6%**, so the
tail-call half halved the dispatch count again and bought no measurable
wall-clock time on this machine. Dispatch count and time are not linearly
related: the first half of the reduction was worth 1.6% and the second half was
worth nothing measurable, which says the dispatches tail calls remove were
already the cheap ones.

**So treat the two differently.** Direct calls are a pre-existing, previously
tested feature with a reproducible +1.6% (measured twice, in separate sessions,
non-overlapping both times). Tail calls are new code written 2026-09-01, verified
over one 90 s scene (`smc_failed=0`, no stack overflow, renders correctly) with
**no measurable wall-clock benefit here** -- kept as the default because they cost
nothing measurable and the phone's per-dispatch cost is higher than this Mac's,
so they may yet pay there. Regenerate without `DOLRECOMP_TAIL_CALLS=1` to drop
them; nothing else depends on them.

Note the ordering that made all of this findable: the -38% was visible in a
*count* before any timing was trustworthy, and the count is what carried both
changes through a machine too warm to time anything.

### The guest memory path checked Wii RAM first on a GameCube game: +2.7%

`get_ram_ptr` in `GXRuntime/include/core/cpu.h` runs on **every guest load and
store**, and it opened with:

    // Check MEM2 (EXRAM) first as it is much more common in Wii titles
    if (cpu->exram) { ... }

Dolphin only allocates EXRAM for Wii, so on a GameCube title `cpu->exram` is
NULL and that was a load and a branch on every memory access that could never
succeed. Nor is it a load the compiler can hoist out of a chunk: guest stores go
through a `u8*`, which may alias the CPUState fields, so `cpu->exram` has to be
re-read after every store.

Checking MEM1 first, interleaved, on a machine warm enough that this only just
clears the noise:

    MEM2 first (old)   900.4  888.9  888.5     median 888.9
    MEM1 first (new)   912.8  904.8  918.3     median 912.8      +2.7%

The arms do not overlap. **This is the largest single win of the session** --
bigger than direct calls -- and it is two branches swapped. Verified over 90 s:
renders correctly, 60 fps, `fallback=0`, `smc_failed=0`, and the run reaches the
same guest tick as the build before it.

A Wii title now pays one extra compare against `ram_size` before reaching its
MEM2 hit. That is the cheaper side of the trade here, but it is a real trade, so
a Wii-heavy target should measure it rather than inherit it.

**This is a GXRuntime change, so it is not wasm-specific** -- the native arm64
iOS build runs the same `get_ram_ptr` on the same GameCube titles.

### A +3.8% fma "win" that was me deleting safety guards. Withdrawn.

Recorded in full because the failure is more instructive than the result.

Seeing `fma()` in `ps_madd_fast` and knowing wasm has no FMA instruction, I
probed it (unconditional `a*c+b`, ~3%), then wrote a guarded fast path taking the
plain multiply-add when `(f64_bits(a) & 0x1FFFFFFF) == 0`, reasoning that `c` is
rounded to single by `ps_round_c_bits` so 24+24 bits is exact in a 53-bit
significand. It measured **+3.8%**, non-overlapping, the largest single win of
the day. It was wrong on every count:

- **`gekko_fma` already exists and already does this**, routed to every `fma()`
  in the file by a macro, and validated against `fma` over ~50M inputs. I had
  replaced a tested implementation with an untested one.
- **`c` is not 24 bits.** `force_25_bit` is a Gekko quirk -- 25 bits, not 24 --
  and this file already said so, two hundred lines further down.
- **`a` can be a full 53-bit double.** My guard only tested `a`'s low mantissa
  bits, admitting subnormals, infinities and NaNs, and I checked no product
  exponent range, so under- and overflowing products double-round and genuinely
  differ from `fma`.

`gekko_fma`'s guards -- both operands normal, `a` at most 27 bits, `c` at most
26, product exponent in range -- are exactly the analysis that makes it correct.
**The 3.8% was the price of those guards.** Reverted; the build uses `gekko_fma`.

**Why the verification did not catch it.** The per-guest-cycle fingerprint matched
to four significant figures, and the scene rendered correctly, because the inputs
my guard newly admitted are rare -- which is exactly the property that makes this
class of bug expensive later rather than immediately. **A fingerprint over one
scene cannot validate a floating-point change**; the input-space diff that
`moderngekko_gekko_fma_test` already does is what can.

**The general lesson, which cost real time twice today:** before optimising
something, grep for whether it has already been done. Direct calls were a flag
nobody had switched on; this was a function nobody had noticed was already
called.

### Two more nulls, and the lead worth taking next

**`convert_to_double` is not worth touching.** It implements the `lfs`
conversion in bit manipulation where wasm has `f64.promote_f32` as one
instruction, which looked like the fma finding all over again. Probed by
replacing it wholesale: 882.2/856.7 against 876.7/866.6 -- overlapping, no
effect. Its normal path is ~8 branch-free ops, not a libcall, and it is not hot
enough to matter. `convert_to_single_ftz` is the same shape and can be assumed
the same.

**Rounding modes are not a perf question here.** `ppc_arm_host_fp_mode` is
`#if defined(__aarch64__)`, so on wasm the guest's FPSCR rounding mode is never
applied to the host at all -- everything runs round-to-nearest. That is a
pre-existing accuracy gap on this target, worth knowing, but there is no time in
it.

**The lead worth taking next: the aliasing tax on guest registers.** The
generated code addresses guest state as `ctx->gpr[n]` on every instruction, and a
guest store goes through a `u8*` (`mem_write32` -> `read_be32`/`memcpy`), which
may alias anything -- including `gpr[]`, since guest memory holds the same types
as guest registers. So the compiler cannot keep guest registers in wasm locals
across a store; it must spill and reload. This is the same aliasing that made the
MEM1 fix worth 2.7%, but applied to every register access rather than every
memory access, so the ceiling is much higher.

Fixing it means giving chunk functions a way to promise that guest RAM does not
alias CPUState -- a `restrict` ram base passed in, or splitting the two into
provably distinct objects -- and then letting LLVM allocate guest registers to
locals within a chunk. That is an ABI change to the module and days of work, not
an evening's, but it is the largest remaining item on the CPU side and it is
worth sizing before anything else is attempted.

### Combined, the three changes that stand: about +6%

The +7.7% measured earlier included the fma change withdrawn above, so it is not
the number. Three separate stock-against-current A/Bs, all with **non-overlapping
arms**, put the effect at:

    stock 0.4% spread   803.1 -> 864.9    +6.0%    <- most trustworthy
    stock 3.1% spread   803.1 -> 864.9    +7.7%       (included the withdrawn fma)
    stock 3.0% spread   809.1 -> 899.0   +11.1%       (new arm spread 4.9%)

**Quote about +6%**, from the run where the baseline held 0.4%. The direction is
certain -- every pairing separates cleanly -- but the magnitude moves with the
machine's state, and the honest range across a day of measuring is roughly
+4% to +11%. About +7% with the LTO/IPO revert on top.

**Take the number that decides something on a rested machine, early.** That is
the third time today this file has had to say so.

Retried once the machine had ~10 minutes of light load, stock codegen against all
three changes, interleaved, same state and flags:

    session start   802.4  805.4  805.7     median 805.4   spread 0.4%
    session end     853.4  862.5  843.9     median 853.4

**+6.0% at the median, and the arms are 4.7% apart at their closest** -- no
overlap. The stock arm came back to a 0.4% spread, which is the harness working
again; the absolute numbers sit below the morning's because the machine is still
warmer, but both arms share that, and the ratio is the result.

That is more than the components measured separately (+1.6% and +2.7%, which
would compound to about +4.3%). Some of that is the two interacting -- the memory
fix makes each dispatch's work cheaper *and* there are half as many dispatches --
and some is the 2.2% spread in the new arm. **Quote it as "about 6%, at least
4.7%"** rather than a point estimate.

Separately, reverting LTO/IPO was worth another ~1.2% (measured earlier, cleanly),
so against the build that started the day the total is nearer +7%.

### An earlier attempt at this figure failed, and why

A final stock-against-everything A/B, run last, came back:

    session start   865.9  800.7  809.4      <- 8.1% spread within the arm
    session end     855.7  854.3  845.9

The first pair even reverses the direction. Everything is ~7% below readings
taken minutes earlier on the same builds. After a full day of building and
measuring, the harness that held **0.3%** in the morning was spreading **8%**, so
this says nothing and no combined number is quoted from it.

**The components stand on their own**, each measured while spreads were tight and
each with non-overlapping arms: direct calls **+1.6%** (measured twice, separate
sessions) and the MEM1-first memory path **+2.7%**. They should compound to
roughly +4.3%, and a single run of the final build did read 915.2 against a stock
879.5 -- consistent, but one run, so it is a sanity check and not a result.

**The lesson to carry:** this harness is only 0.5% on a rested machine. Take the
measurement that decides something *early*, and when within-arm spread exceeds
the effect, stop measuring rather than average harder. Everything deterministic --
the dispatch counts -- stayed perfectly stable throughout and is what carried the
codegen work.

### Where the CPU actually goes, on the current build

`--profile g255:25` from the gameplay state, single core, throttle off, symbol
map relinked so frames have names. Ignore `emscripten_futex_wait` (71.7%) and
`__timedwait_cp` (28.3%): those are ~20 idle worker threads waiting, not cost.
Percentages are summed across profiler sessions, so treat them as ranking rather
than as a budget:

    8.9  PowerPC::PowerPCManager::RunLoop()
    5.0  ps_madd_fast
    2.8  chassis_dispatch
    2.1  VertexLoader::RunVertices
    1.6  Common::GetMurmurHash3            <- texture hashing, every frame
    1.4  StaticRecompCore::AdvanceGuestTimebase
    2.5  ppc_fmuls + ppc_fma + ppc_fadds   (combined)
    0.6  Mixer::MixerFifo::Mix
    0.6  Color_ReadIndex_32b_8888          <- vertex loading

**The dispatch machinery is the largest identifiable block** -- `RunLoop` +
`chassis_dispatch` + `AdvanceGuestTimebase` is about 13% -- and its two tuning
knobs are already saturated (see above), so moving it means changing how dispatch
works, not what it is configured with.

**Do not spend a day inlining the float helpers.** `ps_madd_fast` plus the
`ppc_*` family is ~7.5% and every one is an out-of-line call into GXRuntime, which
makes "inline them" the obvious idea -- it is exactly what LTO does, and LTO
measured **1.2-1.3% slower** overall. That hypothesis is tested and failed.

The genuinely untried items are small and specific: texture hashing at 1.6%
(Dolphin has a texture-cache accuracy setting that trades hashing for correctness
and is not currently exposed through an env knob here), and the vertex-loading
path at ~2.7%.

### The batching project is not worth doing. Measured, not argued.

`DOLWEB_TIME_GL=1` from the gameplay state, headless Chrome. The headline is wall
ms per frame (`(now - last_report) / 100`), so the per-call figures are directly
comparable to it. A representative 22.7 ms frame:

    glBufferData        0.7 ms   983 calls
    glBindBufferRange   0.3 ms   835
    glActiveTexture     0.5 ms   292
    glBindTexture       0.1 ms   292
    glDrawElements      0.1 ms   359
    everything else    ~0.3 ms
    ------------------------------------
    all instrumented   ~2.0 ms   ~1780 calls   =  9% of the frame

**The GL calls are about 9 % of a frame and the buffer uploads about 3 %.** So
eliminating every vertex upload -- the baseVertex/index-rebasing restructuring
this file has called the renderer's architectural fix -- is worth **roughly 3 %**.
It is not a project worth a week. That also explains the dedup null result: there
is nothing to win by removing redundant state calls when all the state calls
together are a fraction of a small fraction.

**Where the renderer's 24 % actually is: Dolphin's CPU-side video emulation**, not
the GL API. The device profile already said so and it was read as a call-count
problem -- `SetCPStatusFromGPU` 9.7 %, `OpcodeDecoder::RunFifo` 9.5 %,
`VertexManagerBase::Flush` 8.6 %, vertex loading ~7 %. Those are CPU costs in the
video path. **The renderer problem and the recompiler problem are the same kind of
problem**, which is a much better description of this project than "the renderer
is the wall".

Three caveats, none of which rescue the batching idea. Only 24 entry points are
wrapped, and the frame issues more calls than that, so the true GL share is higher
than 9 % -- but `glBufferData` is wrapped, and it is the one batching removes.
The timer costs two clock reads per call, which *inflates* the per-call figures,
so the real share is lower still. And this is V8 on a Mac; the phone additionally
pays an ~11 % emscripten proxy tax on each call, which raises the GL share there
without changing the fact that uploads are a small part of it.

### The phone says the renderer costs 1.87x, not the Mac's 1.24x

Measured 2026-09-01 on the iPhone, savestate `oglplay.sav`, single core, throttle
off, build `gexe52-c128-tail`, read with `./state-rate.py --ua phone`:

    phone  OGL   450.8 M ticks/sample   (43 samples)
    phone  Null  843.7 M ticks/sample   (44 samples)
    renderer costs 1.87x

Against **1.24x** for the same measurement on the Mac. The renderer is ~47% of a
phone frame and ~24% of a Mac frame -- **roughly twice the relative cost**.

**This partly reverses the advice above.** "Do not fund the batching project" was
argued from the Mac: GL calls are ~9% of a Mac frame and buffer uploads ~3%. The
phone pays an ~11% emscripten proxy tax on every GL call on top of a weaker
driver, so that share is much larger there, and batching may pay on the device
even though it does not here. **The Mac systematically understates the renderer**;
size renderer work on the phone or not at all.

Two things this does *not* say. It is **not** comparable to the older 2.9x, which
was boot-driven, dual core, throttled and a different scene -- different
instrument, not an improvement. And it says nothing about whether the day's CPU
work helped the phone, which needs the same config measured on both builds.

**The next measurement to take is a phone before/after**: serve `web128p`
(session-start codegen, old memory path) and re-run the identical OGL link, then
compare with `state-rate.py`. Two runs, and it is the only thing that says what
today was worth on the target.

### Size the prize before funding the project

Same state, same scene, renderer on against renderer deleted:

    OGL  (renderer on)   888.2  891.9  887.1     median 888.2
    Null (no renderer)  1120.0 1100.6 1089.9     median 1100.6

**The entire renderer is worth 1.24x.** Not 2.9x -- that figure came from
comparing numbers taken in different scenes on different machines, and it is
withdrawn. On this machine the guest CPU is 76 % of the work and everything the
GL backend does is the other 24 %.

That is the number that should decide the restructuring project, and it argues
against starting it alone: a **perfect** renderer -- zero draw calls, zero
uploads, nothing -- moves this machine 1.24x, and the phone's own split was never
measured this way. Reaching native-class speed from 30 % needs about 3.3x. The
renderer cannot pay for that by itself, so a renderer project has to be sold as
one part of a plan that also moves the recompiled CPU code, not as the fix.

**Measure this on the phone before committing anyone's week.** It is now one
capture and six runs, and it is the difference between funding the right project
and the expensive one.

**Fixed: `state-rate.py` is the metric the Safari and phone paths were missing.**
It reads `reports.jsonl` and prints guest ticks per perf sample for each run,
plus the renderer-on/renderer-off ratio when it can see both. Validated against
the per-run logs it never reads -- OGL 871.6 against the log's 890.6, Null 1111.3
against 1120.2, ratio 1.27x against a directly measured 1.24x.

**That needed a change to the page, not cleverness in the reader.** Reports had
no run identity, and it cannot be reconstructed: `ms` is each page's own clock,
so two pages posting at once interleave two unrelated clocks; arrival time does
not separate runs either, because a stale tab re-posting its last perf lines
every five seconds keeps the stream gapless forever. Three heuristics were tried
and every one produced a confident, wrong answer -- 8126 bogus sessions, then a
per-sample step 100x too small, then **a renderer cost of 0.62x against the
measured 1.24x**. `dolweb-page.js` now mints a `run` id per page load and puts
it in every report. Reports from before 2026-09-01 have none and are skipped
rather than guessed at.

**So the phone measurement is now two runs**, and the analysis is
`./state-rate.py --ua phone`. That is the number that decides whether the
renderer restructuring is worth a week, and it is the one thing here that still
needs a device. Verified 2026-09-01 that the server actually serves what it
should -- the page carries `RUN_ID`, `dolweb.wasm` is the 86.2 MB
non-LTO build (with the savestate fix), and `game/oglplay.sav` returns 200 over
HTTPS, so the run will not die on a stale `.gzcache` entry:

    https://192.168.1.8:8712/index.html?backend=OGL&seconds=90&report=1&auto=1
      &env=DOLWEB_STATE=/game/oglplay.sav
      &env=DOLWEB_CPU_THREAD=0&env=MODERNGEKKO_EMULATION_SPEED=0

    ...the same URL again with backend=Null

Single core and throttle off on purpose: single core serialises CPU and GPU onto
one thread so the tick rate measures total work (and matches how the Mac number
was taken), and the throttle would otherwise cap the answer at 100 % and hide
the whole effect. HTTPS, not HTTP -- a LAN test over plain HTTP loses the secure
context, and the failure looks like something else entirely.

