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

Where it stood before the header was found, on an iPhone 15 Pro Max in Safari,
warm, matched guest window 45-90 s, throttle off (`?ab=45&auto=1&report=1`):

| | |
|---|---|
| CPU only (Null backend) | 157% |
| with rendering (OpenGL) | 40% |
| this Mac, same window | Null 192% / OpenGL 212% |

**Read those as suspect, not as facts.** Both were taken while the phone was
pulling 1.2 GB of disc over Wi-Fi, and a WASMFS fetch blocks the emulation
thread. The Mac reads 92-100% with OpenGL on the fixed build.

Retake them before doing anything else with the renderer. What the earlier work
did establish, and what survives:

- `DOLWEB_TIME_GL=1` on the device: the round-trip GL calls are ~3 ms of a 55 ms
  frame. With the state calls and the texture uploads wrapped as well, the total
  is ~4% of the frame on both machines. **Whatever the gap is, it is not in a GL
  call the shim can see.**
- `DOLWEB_TIME_SWAP=1` **has still never been run on the device.** It is the one
  measurement that separates "blocked waiting for the GPU" from "busy in
  VideoCommon", and they have opposite fixes. Run it first.
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

## 2. The black ground -- the correctness

Whole floor surfaces render black while the walls, ramps, props and skater in
the same frame are correct. It is **not Olliewood-only**: Andy's House -- the
first career level, zero LEFT presses on the skater carousel -- shows it on
roughly half of a run's frames, so the repro no longer needs a specific level.
Three minutes:

    node drive-dolphin.mjs --backend OGL --seconds 190 --shotEvery 10 \
      --acts "g25:5,g40:5,g52:5,g70:0,g76:0,g82:0,\
              g110:15:9000,g135:15:9000,g160:15:9000"

Frames 9 onward show it. Judge them by the fraction of near-black pixels in the
lower half of the canvas rect (192,18)-(768,458) rather than by eye: ~0.9 is the
defect, ~0.05 is a good frame, and a screenshot of a fade looks like both.

**The earlier narrowing was wrong, and it was the instrument's fault.**
`MODERNGEKKO_FLAT_SHADE=tex` wrote `textemp.rgb` into `main()`, where the TEV
temporaries do not exist -- they live inside
`dolphin_process_emulated_fragment` and only `frag_output` crosses back. 114
shaders failed to compile in a three-minute run and the screen went flat grey.
So "the inputs are right and the TEV combination produces black", and the
`StreamBuffer` ring and pool experiments that followed from it, rest on a shader
that never ran. Fixed in `fd230c5`; the inputs travel out through
`DolphinFragmentOutput` now, and a `tex` run reports zero compile failures.

**What the fixed instrument says: the texture input is black.** With
`=tex`, the whole scene shows its texture samples -- sky, fence, boxes,
characters all textured -- and the ground is black, in exactly the frames where
the finished image is black. So the defect is upstream of the TEV combination,
not in it. (Caveat worth keeping: `textemp` holds the *last* stage's sample, so
this says a texture used by a late stage on floor materials samples zero.)

**Ruled out, each measured, do not re-check:** the depth test
(`MODERNGEKKO_DEPTH=always` and `=noflip`), the scissor and viewport, the XFB,
the EFB copy and its paths (`DOLWEB_GFX_HACK=SkipEFBCopyToRam,DisableCopyToVRAM`),
**mipmap completeness** (`MODERNGEKKO_NO_MIPMAP=1`, re-run 2026-08-29 on the
fixed build: identical black frames in the same places), shader compilation (the
base build logs zero failures), and the disc corruption of section 0 -- the
defect survives that fix.

**And now, with the texture path instrumented, ruled out as well:**

- **GL is rejecting nothing.** `DOLWEB_GL_ERRORS=1` drains `glGetError` once a
  frame; a full run through the black region reports none. This matters because
  WebGL drops a call it rejects and reports nothing to the console, so a failed
  upload is a black surface with no other trace.
- **No upload fails, and no texture arrives empty.** `DOLWEB_LOG_TEXTURE=1`
  reports the shape of every distinct texture, a GL error after each upload, and
  whether the bytes handed to GL were already all zero. Over a full run the only
  all-zero textures are a 1x1, a 64x64 and two full-screen ones; nothing
  floor-shaped.
- **The texture cache always binds something.** The same flag logs every stage
  where `GetTexture` returns null, which would leave the unit holding whatever
  was there before. Zero in a full run.
- **`bSupportsTextureStorage` is true here** after all -- every texture logs
  `storage=1` -- so the mutable `glTexImage3D` path with its `depth=1` bug is
  not in play. (`MODERNGEKKO_GL_ENABLE=` exists now for the general case: some
  capability flags test for a *desktop* extension name that GLES cannot report
  even when GLES core has the feature.)

**So the data is right, the upload is right, and the binding happens.** What is
left is the *coordinate* or the sampler: `MODERNGEKKO_FLAT_SHADE=uv` emits the
last stage's `tevcoord` as a ramp, which separates "the texture is black" from
"we are sampling somewhere black" in one run. That is the next step.

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
