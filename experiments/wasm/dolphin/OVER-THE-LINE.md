# What is left, and how to do it

*Written 2026-08-28, at the end of the day that made this route real. The
objective is unchanged: **Disney skate playing correctly at full speed in Safari
on the iPhone, with nothing installed**, on code statically recompiled on a Mac.
`README.md` is how to build and measure; `DOLPHIN-ROUTE.md` is why the route
exists. This is the part that is not done.*

## Where it stands

Measured on device (iPhone 15 Pro Max, Safari), warm, matched guest window
45-90 s, throttle off — `?ab=45&auto=1&report=1`:

| | |
|---|---|
| CPU only (Null backend) | **157%** |
| with rendering (OpenGL) | **40%** |
| this Mac, same window | Null 192% / OpenGL 212% |
| boot traffic | 63 MB first load, ~0 on a reload |
| module | 86.5 MB, 256-instruction chunks |

**The CPU half is done.** Recompiled ahead of time on a Mac, linked into the
binary, 157% of realtime on the phone with room to spare. Nothing on the
critical path needs the interpreter (`fallback_steps` is in the tens over a
ten-minute run).

**The renderer is the whole remaining gap**, and it is a device-only problem:
this Mac measures Null and OpenGL the same, the phone measures a 4x difference.
That asymmetry is the single most important fact in this file, and it is why
`ios/PERFORMANCE.md`-style reasoning from desktop numbers went wrong twice
today. Apple's GPU is tile-based; Dolphin's frame is full of the things a tiler
hates.

Two caveats on the 40%: it is *worse* than the 92% measured earlier the same
day, and that is expected — the depth fix below means geometry that used to
silently vanish now actually draws. And it has not been re-measured since the
last few commits.

## 1. The renderer on the device — the speed

**Do this first, and do it on the phone.** `DOLWEB_TIME_GL=1` wraps the GL calls
that force a round trip and prints `[gl]` lines once per hundred presents. It
has never been run on the device. It is what found `glBufferSubData` at 6 ms a
call, and no WebGL backend's *source* looks expensive, so reading the code
instead of running this is a way to lose a day.

    https://<host>:8712/index.html?auto=1&report=1&env=DOLWEB_TIME_GL%3D1

Run it on the Mac too, for a baseline: the interesting entries are the ones the
phone weights very differently.

Where it will probably point, in order of prior:

- **EFB copies.** A tile-based GPU resolves the whole tile buffer on every
  render-target change. `DOLWEB_LOG_EFB_COPY` already prints them; Disney skate
  does a 640x448, a 256x256 and a 64x64 per frame. The known levers are
  `GFX_HACK_SKIP_EFB_COPY_TO_RAM`, `GFX_HACK_DISABLE_COPY_TO_VRAM` and the copy
  filter, all reachable from `DOLWEB_GFX_HACK=`.
- **Internal resolution.** `DOLWEB_EFB_SCALE=1` is the floor and is what a phone
  should be running; check what it is actually defaulting to.
- **Shader compilation hitches.** Compilation is synchronous on this target
  (WebGL has no shared contexts, so the async compiler cannot work), so a new
  pipeline stalls the frame. It will show as spikes rather than a lower median,
  and the A/B's guest-anchored window is the honest way to see past it.

## 2. The black ground — the correctness

Olliewood's ground renders black; the buildings, sky and skater are correct.
Reproduces headless:

    node drive-dolphin.mjs --backend OGL --seconds 260 \
      --acts "g25:5,g40:5,g52:5,g60:8,g63:8,g66:8,g70:0,g76:0,g82:0,\
              g110:15:9000,g130:15:9000"

**Ruled out, measured, do not re-check:** the depth test
(`MODERNGEKKO_DEPTH=always` and `=noflip` are both still black), the scissor and
viewport, the XFB, the EFB copy, the EFB copy *paths*
(`DOLWEB_GFX_HACK=SkipEFBCopyToRam,DisableCopyToVRAM`), mipmap completeness
(`MODERNGEKKO_NO_MIPMAP`), shader compilation (`DOLWEB_DEBUG_LOG=5` reports no
failures) and the geometry.

**What is known:** `MODERNGEKKO_FLAT_SHADE=1` paints every fragment magenta and
the black region goes magenta, so it *is* rasterising. `=tex` and `=ras` emit
the TEV's texture and raster inputs and both are correct there. So the inputs
are right and the TEV combination produces black, which points at the constants.

**The suspect and the trap.** `StreamBuffer::Create` gives every buffer on this
target the offset-zero streaming that the *vertex* path needs for want of base
vertex, and `glBindBufferRange` names a buffer *and a range* — so a later upload
can land on an earlier draw's constants. A ring of moving offsets **does** fix
the ground and costs **OpenGL 212% -> 32%** on a matched window, because
`glBufferSubData` at a non-zero offset stalls. Three cheaper shapes do not fix
it at all: a pool of orphaned buffers, a pool of never-resized ones written at
offset zero, and uploading the constants every batch instead of only when dirty.

**So stop trying buffer shapes.** The next honest step is to read back the
pixel-shader constant block the GPU actually sees for a black draw, against the
same draw with the ring in place, and find what is reading stale data.

## 3. Not yet swept

Menus, one level's lobby and Olliewood are all that have been looked at. Both
defect classes found today were found by *someone opening a screen nobody had
opened*. A pass over every level and menu, headless, comparing against
`moderngekko-run` with the same input script, would be worth more than another
round of guessing.

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
