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

**The device reading, in a level:**

    [gl] 58.5 ms/frame  glBufferData 3.1ms/1350  glDrawElements 0.3ms/493
    [gl] 54.1 ms/frame  glBufferData 2.4ms/1128  glDrawElements 0.3ms/409

**About 3 ms of a 55 ms frame -- five percent.** The round-trip GL calls are
*not* the cost on the device, which rules out the thing this section was
written to chase. (Mac, same instrument: 17 ms/frame, 0.4 ms.) So the
157% -> 40% gap is one of two other things, which need different fixes:

- **the unwrapped per-draw calls** -- state, uniforms, bindings. In WebGL every
  one crosses into JS and is validated, and a tile-based GPU charges for
  framebuffer switches that no single call attributes.
- **Dolphin's own CPU-side renderer work** -- the texture cache, vertex loading,
  shader generation. The Null backend skips much of this too, so it is inside
  the Null-versus-OpenGL difference and is not GL at all.

**The instrument now covers the state calls and the texture uploads too** --
and Dolphin's textures are 2D *arrays*, so uploads go through
`glTexSubImage3D` and none of the 2D entry points the shim originally wrapped
are ever called. Texture uploads were invisible in every reading before that
was fixed.

With all of it wrapped, the total is **~4% of the frame on both machines** --
and the Mac pays nothing for the renderer while the phone pays 117 points. So
the cost is not in any GL call the shim can see, and the two candidates above
are down to one plus a third that was never on the list:

- **Dolphin's CPU-side renderer work** (VideoCommon: texture decode, vertex
  loading, EFB copy processing) -- the Null backend skips much of it, so it sits
  inside the same Null-versus-OpenGL difference.
- **Waiting for the GPU.** Time spent blocked in the present is charged to no
  CPU-side call at all. `DOLWEB_TIME_SWAP=1` measures exactly that and **has
  never been run on the device**. Run it before anything else: if the frame is
  sitting in the swap, this is GPU-bound and the levers are resolution, EFB
  copies and overdraw; if it is not, the work is in VideoCommon and the backend
  is the wrong place to look entirely.

      ...&env=DOLWEB_TIME_GL%3D1&env=DOLWEB_TIME_SWAP%3D1

**The old note said the state calls** -- glUseProgram,
glBindTexture, glBindFramebuffer, glBindBufferRange, glBindSampler,
glUniform4fv -- so the next device run separates them directly. On the Mac they
are 0.1 ms of 17; if they are tens of milliseconds on the phone it is chattiness
and the fix is batching and redundant-state elimination. If they are not, what
is left is Dolphin's own CPU-side renderer work, and the fix is somewhere in
VideoCommon rather than in the backend at all.

Where it will otherwise probably point, in order of prior:

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
