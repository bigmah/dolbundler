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
the transport fix. The device half does not:

| | |
|---|---|
| iPhone 15 Pro Max, Null | 157% |
| iPhone 15 Pro Max, OpenGL | 40% |

**Read those as suspect, not as facts.** Both were taken while the phone was
pulling 1.2 GB of disc over Wi-Fi, and a WASMFS fetch blocks the emulation
thread.

**And the heap is a device number nobody has taken.** 614 MB resident plus the
module is close to what a WebContent process gets before iOS kills it, and a
process that crosses its jetsam limit just disappears -- which looks exactly
like a crash in the emulator. Measure it on the phone before optimising
anything else about memory.

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

## 2. The black ground -- a texture that decodes to nothing

**It is not a renderer defect.** Whole floor surfaces render black while the
walls, ramps, props and skater in the same frame are correct, and the cause is a
texture that decodes to nothing but zeros. `DOLWEB_PAINT_ZERO_TEXTURES=1`
uploads magenta wherever the decoded bytes are all zero, and over the window
where the base build has seven black frames out of seven, the painted build has
**none**.

**Which** empty texture is not pinned yet. A run reports five distinct ones, and
they fall into two classes that have nothing to do with each other:

1. **An empty palette over real indices.** A 256x256 **C8** texture -- 8-bit
   paletted, IA8 palette -- whose own 64 KB of indices are present and non-zero:

    [tex] decoded to zero: 256x256 fmt=9 tlut=0 addr=0x001faca0 src=65536 bytes,
          source itself is NOT zero, tlut 512 bytes is ZERO

   Its 512-byte palette in TMEM is entirely zero, so `TexDecoder_Decode` maps
   every index to black and is right to.

2. **Source bytes that were already zero** -- three textures (a 256x256 C4, a
   64x64 C4, two 640x480 RGBA8) whose data in guest RAM is all zeros, which for
   a texture the game has not loaded yet is not a defect at all.

`DOLWEB_PAINT_ZERO_TEXTURES=palette` and `=source` paint one class at a time and
are how to settle it. A first `=source` run came back not-black in the frames
the base run had black, which would put the floor in class 2 -- but the floor in
those frames is a plain red carpet with no magenta in it, so the run is more
likely to have been a different moment of the level than a painted floor. **Run
both halves and compare against the same guest ticks before believing either.**

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
