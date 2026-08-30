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

**It is class 2 -- but "class 2" is a large and mostly innocent class.** See
below: all-zero textures are ordinary here, and the particular one this file
went on to name is bound on the desktop too, where the floor is fine. Painting
the two classes in *different colours* in one run --
magenta for the empty palette, green for the empty source -- settles it without
comparing two runs at two different moments of a level, which is what made the
first attempt at this useless. The floor comes back **green**: its texture data
in guest RAM is all zeros. The empty palette is real but is a different, and so
far harmless, defect.

So the question is why a texture the game is drawing with has no data behind it.
One thing is known about it: **`DOLWEB_FETCH_CHUNK=8192` does not fix it**, so
simply downloading files whole is not enough.

**The interpreter has never been tested against this texture.** This file said
it had -- "the interpreter shows the same, so the guest is not failing to write
it" -- and that is wrong, in a way worth spelling out because it sent the search
in the wrong direction for a day. The interpreter run
(`STATICRECOMP_FALLBACK_RANGES=80000000-90000000`) lasted three minutes at 6% of
realtime. It reproduced the *boot* decode at guest 1.2 s, which is the empty-
**palette** texture at `0x001faca0`. At 6% of realtime it cannot have reached
Andy's House, so it never saw the floor's C4 at `0x009a9e60`. **Whether the
recompiled code is responsible for the floor is still open**, and it is now
cheap to settle: an interpreter run needs ~35 wall minutes to reach guest 120 s,
and can simply be left going.

The open suspect is the transport again, but the *other* half of it.
emscripten's WASMFS fetch backend creates a file's range table after an awaited
HEAD:

    if (!(file in wasmFS$JSMemoryRanges)) {
      var fileInfo = await fetch(url, {method:'HEAD', ...});   // <-- await
      wasmFS$JSMemoryRanges[file] = { size, chunks: [], chunkSize };
    }

Two reads of one file that arrive before either HEAD resolves both pass the `in`
test, and the second **replaces the object**, discarding the chunks the first had
stored. The first then continues past its own await, finds `chunks[i]`
undefined, and throws a TypeError out of the async read -- which reaches C++ as a
read that moved no bytes, leaving a freshly zeroed buffer exactly as it was.
Dolphin reads the disc from more than one thread. `dolweb-fetch.js` is a
replacement backend that does each piece of work once (one in-flight promise per
file, one per chunk) and returns `EIO` rather than throwing when a chunk is
missing, so the next occurrence says so instead of going quiet.

**It is not that either.** With the replacement backend linked in, the floor is
still black in the same frames, and not one chunk-missing error fires -- so the
race is real but was not happening here. Keep the replacement: it removes a
genuine race, reads a comma-joined `Accept-Ranges` correctly, and turns a silent
short read into an error. It is not the cure.

**Nor is it TMEM preload.** Every empty texture logs `from RAM`, so none of them
is a preloaded texture read out of Dolphin's emulated TMEM.

**And `0x009a9e60` is not the floor.** This file named the 256x256 **C4** at
guest `0x009a9e60` on stage 0 as the floor's texture, on the grounds that its
32 KB of indices are entirely zero behind a perfectly good palette. It is bound
on the **native** build too, at the same guest second, where the floor renders
correctly -- so it cannot be what makes the floor black. A frame in Andy's House
binds **87 distinct textures, 3 of them all-zero**: an empty texture is ordinary
in this game and is not evidence of anything by itself. Whichever empty texture
the browser draws the floor with, it has to be identified by *diffing the two
builds*, not by being the only empty one in a log.

**Not the CPU palette path either.** WebGL2 has no texture buffer objects, so
`bSupportsPaletteConversion` is false and the browser is the only target that
decodes a paletted texture on the CPU -- baking the palette in when the texture
is decoded -- instead of applying it on the GPU at draw time. A real and unique
difference, and the best remaining suspect.
`MODERNGEKKO_NO_PALETTE_CONVERSION=1` forces the same path on the desktop; the
floor stays clean (0.051 against the defect's 0.84-0.98). Eliminated.

**`DOLWEB_TEX_CENSUS=<guest s>`** is the instrument for the diff: it prints
every texture bound over the next half second -- stage, address, size, format,
source bytes, non-zero count, both hashes -- in both builds, reached in the
browser as `?env=DOLWEB_TEX_CENSUS=150`.

### The census answers it: guest RAM is identical

Run in both builds at guest 150 in Andy's House:

| | |
|---|---|
| distinct textures, native | 84 |
| distinct textures, Chrome | 77 |
| in both, native has data and Chrome is all zero | **0** |
| in both, non-zero counts differ at all | **0** |
| bound only in Chrome | **0** |
| bound only natively | 7 |

The 7 are one contiguous cluster of small CMPR textures,
`0x008e2b80`-`0x008e63a0`, 256 to 4096 bytes each -- HUD or font glyphs the
browser did not happen to bind inside the same half-second. Not missing data.

**Every texture the browser draws with has byte-identical source data to the
build that renders the floor correctly.** So the black ground is not a data
problem at any level: not the disc, not the recompiled code, not a DMA, not the
transport. Those are all now excluded by measurement rather than by argument.

It also retires the framing this file has used throughout. "A texture that
decodes to nothing but zeros" was the whole theory of the defect, and the
textures that decode to zero are the *same ones in both builds* -- so whatever
the browser draws the floor with, it has real bytes behind it and comes out
black anyway. The remaining ground is **decode, upload, or shading**, which is
a much smaller space than the one this file has been searching.

### And it is the decoded content, measured by painting over it

`DOLWEB_PAINT_BY_ADDRESS=1` replaces every decoded texture with a flat colour
derived from its guest address. In the browser, in the level, **the black ground
disappears completely**: 0.014-0.022 near-black on every frame of the window
against 0.84-0.98 on the same timeline unpainted. The floor comes out bright
pink, which is a texture identity colour and not a coincidence.

That places the defect exactly. The texture is found, bound, uploaded, sampled,
shaded and lit correctly -- every one of those steps is exercised by the painted
run and every one of them works. **What is wrong is the bytes the decoder
produces for one texture.**

Two eliminations on the way there, both taken in the level rather than in the
menus, which is where several earlier ones went wrong:

| | |
|---|---|
| `MODERNGEKKO_FLAT_SHADE=tex` | floor still black (0.55-0.97): not the shading |
| `MODERNGEKKO_NO_MIPMAP=1` | floor still black (0.91-0.98): not mipmap completeness, despite `SamplerCache.cpp` describing exactly this symptom |

**And the first decode census missed it**, which is worth recording as a
measurement error rather than quietly fixing. It reported only whether a decode
came out *entirely* zero, and by that test the browser had exactly one offender
-- the boot C8 at `0x001faca0` -- which pointed away from the truth. A texture
that decodes to (1,1,1) everywhere has every byte non-zero and still renders
black. The census carries the decoded mean and an FNV-1a checksum now.

The checksum is deliberately **not** Dolphin's `GetHash64`: that is hardware CRC
on ARM and xxhash on wasm, so it differs between the builds for identical bytes
and cannot be compared across them at all. Every `base_hash` in the texture
census differs between the two builds for exactly this reason, and it means
nothing.

### What is left, and what it is not

At the same guest moment, in both builds:

| | |
|---|---|
| textures bound | same set (77 of native's 84 in Chrome; **none** bound only in Chrome) |
| stages they are bound on | same, in the same proportions |
| source bytes | identical (non-zero counts match on all 77) |
| decoded bytes | identical (portable FNV checksum, 364 shared textures, 0 differ) |
| decoded bytes, level window only | identical (content-keyed, 0 disjoint) |
| the stage-7 mask bound in nearly every draw, `0x002d6a40` | identical |
| every step from lookup to lighting | works -- the painted run exercises all of it |

So the defect is not in the data, not in the binding, and not in the pipeline.
**What has not been compared is the generated shader**, and that is now the
whole of the remaining space. `MODERNGEKKO_DUMP_SHADER="<texgens>,<tevstages>,
<indstages>"` prints the fragment shader for one UID so the two builds' code can
be diffed; WebGL2 is GLSL ES 3.00 and the desktop is GLSL 3.30, and Dolphin's
generator branches on that.

The floor's UID natively at guest 150 is **texgens 2, tevstages 3, indstages 0**.

### The pixel shader is not it either

Dumped for that UID in both builds and diffed. The two are 93.5% identical and
the entire behavioural difference is **two lines**:

    -  return iround(255.0 * texture(tex, coords));
    +  float lod_bias = float(bitfieldExtract(int(texmode0), 8, 16)) / 256.0f;
    +  return iround(255.0 * texture(tex, coords, lod_bias));

    -  int zCoord = int(rawpos.z * 16777216.0);
    +  int zCoord = int((1.0 - rawpos.z) * 16777216.0);

Everything else is the expected GLSL ES 3.00 shape: a `bitfieldExtract`
polyfill, separate varyings instead of an interface block, and no `ocol1`
because there is no dual-source blending.

Neither line survives scrutiny as the cause:

- **The LOD bias** is not a difference in behaviour, only in where it is applied
  -- `bSupportsLodBiasInSampler` is false on GLES so the shader does it, and the
  desktop does the same thing through `glSamplerParameterf`. And it is already
  eliminated by experiment: `MODERNGEKKO_NO_MIPMAP=1` forces a non-mipmapped
  filter, which makes a LOD bias inert, and the floor stayed black.
- **The depth flip** is the symmetric handling of `backend_reversed_depth_range`
  and feeds only ztex and fog. Fog is applied *after* the texture, so a fogged
  floor would stay black no matter how bright its texture was -- and painting
  the textures makes the floor render. That rules fog out on the paint result
  alone.

**So the data, the decode, the bindings-as-looked-up and the pixel shader are
all identical, and the floor is still black in one build.** The one question not
yet asked is which texture object ends up on which *sampler unit* at draw time:
the census logs lookups, not binds, and those are different questions.
`DOLWEB_BIND_CENSUS=<guest s>` records the unit-to-address mapping for a short
window in both builds.

That is also the only story still consistent with the paint result. If the
browser's floor draw samples a texture that is legitimately black in *both*
builds -- and there is a good candidate, the stage-7 mask `0x002d6a40`, whose
mean of 63.75 is zero RGB behind opaque alpha -- then every census agrees, and
painting every texture bright makes the floor render because it makes even that
one bright.

**The binds are the same, and the mask is not it.** Over a run to guest 160,
**no stage is sampled with nothing bound in either build** -- a shader reading
an unbound unit gets (0,0,0,1) and nothing reports it, so that condition is now
counted always rather than behind `DOLWEB_LOG_TEXTURE`, and it is zero on both
sides. That also disposes of the one difference the bind census did show (native
binding a base texture together with the stage-7 mask where Chrome bound the
base texture alone): with no unbound sampling anywhere, that is `used_textures`
differing, which means different draws, which means the two runs were in
slightly different places. Scene divergence, not a defect.

And `DOLWEB_PAINT_ONLY=0x002d6a40` paints that mask and nothing else: the floor
stays black, 0.69-0.98. **The floor is showing its base texture on unit 0**, and
that texture's decoded bytes are identical to the desktop's.

### The two builds cannot be aligned by guest time at all

This matters more than any single result here, because it is the reason so many
comparisons in this file had to be withdrawn.

Anchoring the act timeline to the guest clock was necessary but is not
sufficient, and neither is anchoring the *holds*. Run both builds from boot with
identical guest-anchored inputs and screenshot both at guest 130, and they are
in **different parts of the game**: the desktop is indoors in Andy's House with
a wooden floor, the browser is outdoors in the back yard. Same timeline, same
guest second, different place.

The cause is not input timing. **The game streams from the disc and waits on it
in guest time**, and the browser's reads go over HTTP while the desktop's come
off local disk. The two builds therefore spend different amounts of *guest* time
loading, and everything after the first load is offset. No amount of input
anchoring fixes that.

Cross-build savestates would have fixed it and do not work (below). So the only
remaining way to compare a picture from each build is to anchor to an **event in
the game** rather than to a time.

**`DOLWEB_MARK_TEX=all` does that, and the two builds turn out to be offset by a
constant.** It records the guest time at which every texture is first bound. Run
in both builds over the same timeline:

| | |
|---|---|
| addresses bound, desktop | 367 |
| addresses bound, Chrome | 367 |
| shared | **367 -- every one** |
| offset (Chrome minus desktop), textures first bound after guest 100 | **+8.16 s**, min +8.15, max +8.16 over 29 textures |

The browser is a **fixed 8.16 seconds behind** in the level, with essentially no
spread. So a desktop shot at guest T and a browser shot at T+8.16 are the same
moment of the game, and `MODERNGEKKO_SHOT_AT` / `--shotAt` can finally produce a
matched pair. (Over the whole run the median is the same 8.16 s but the spread is
wide -- the menus drift about -- so take the offset from the level, which is
where the defect is.)

That both builds bind exactly the same 367 addresses is itself worth recording:
it is the strongest evidence yet that the guest is doing identical work in both.

**The first matched pair, and what it shows.** Desktop at guest 130 against
Chrome at 138.2: the same bedroom, the same two ramps, the skater a little
further along in the browser (the offset aligns texture *loads*, not the
skater's exact position, so this is a matched moment and not a matched frame).
In the browser **the blue oval rug renders correctly and the wooden floor
around it is black**. Both surfaces are in the same draw list, both textures
decode identically in both builds -- and one of them survives the trip and the
other does not.

**Identifying the floor's texture by its painted colour still does not work**,
even at a matched moment. The desktop floor reads (136,62,170) and the browser
floor (68,16,117) -- different hues, which would mean different textures -- but
the desktop value is brighter than any colour the paint can emit (the cap is
120), so the lighting multiplier there is above 1 and unknown, and the two
cameras are looking at different patches of floor. The nearest candidates are
4000 and 107 squared-distance away respectively. **Suggestive, not evidence.**

**Saturated hues were the obvious fix and they do not work either.** A *scalar*
multiply preserves hue, so `DOLWEB_PAINT_HUE=<seed>` paints 60 fully saturated
hues six degrees apart at fixed value, and the identity should survive being
lit. It does not: the browser's floor comes back at 108 degrees, a slot no
texture was painted with. **The lighting here is per-channel** -- coloured
vertex lighting and TEV konst -- so it shifts hue as well as brightness. No
colour-based identification can work through it.

**`DOLWEB_PAINT_RANGE=lo:hi` is the method that does**, because it asks a
different question. Paint only the textures in an address window and measure
whether the floor *stopped being black* -- a yes/no the near-black fraction
already answers, with no colour matching anywhere.

Bisecting the 98 addresses bound in the level, against a black baseline of
0.89-0.96:

| step | window | result | left |
|---|---|---|---|
| 1 | `0 .. 0x00b035a0` | 0.015 -- **renders** | 49 |
| 2 | `0 .. 0x009e1ee0` | 0.015 -- **renders** | 24 |
| 3 | `0 .. 0x008b8660` | 0.93 -- still black | 12 |
| 4 | `0x008b8660 .. 0x008e4ba0` | 0.88 -- still black | 6 |

The six survivors are `0x008e4ba0`, `0x008e5640`, `0x008e6100`, `0x008e63a0`,
`0x009a9e60`, `0x009b1e80`. The first four are 128x32 down to 32x16 -- too small
to be a floor. The last two are the **all-zero C4s**, and `0x009a9e60` is
256x256.

**Step 5 confirms it: painting `0x009a9e60` alone takes the floor from
0.89-0.96 black to 0.027-0.285.** One texture, and the floor comes back. That is
the floor's texture, definitively.

Which is the texture this file named as the floor's at the very beginning, and
which was dismissed on the grounds that the desktop binds it too while rendering
the floor correctly. That dismissal may have been right about the evidence and
wrong about the conclusion: **both builds can bind the same all-zero C4 and
still differ, because a C4 is an index into a palette.** All-zero indices mean
every texel is palette entry 0, so the floor's colour is *entirely* determined
by the TLUT -- and this is the only target that takes Dolphin's CPU palette
path, because WebGL2 has no texture buffer objects.

## The mechanism

The browser's own log names every piece of it:

    [tex] decoded to zero: stage 0, from RAM, 256x256 fmt=8 tlut=2
          addr=0x009a9e60 src=32768 bytes, source itself is zero too,
          tlut 32 bytes at tmem 0x40000 is not zero

The palette **has data**. The indices are all zero. The decode still comes out
zero -- so palette entry 0 was black *at the moment the palette was baked in*.

`TextureCacheBase::LoadImpl` has two shortcuts before the real lookup, and
**neither looks at the palette**:

    if (!force_reload && TMEM::IsValid(stage) && m_bound_textures[stage]) {
      if (TMEM::IsCached(stage)) return entry;              // no check at all
      if (!entry->invalidated && entry->base_hash == entry->CalculateHash())
        return entry;                                       // source bytes only
    }

`CalculateHash()` hashes `GetPointerForRange(addr, size_in_bytes)` -- the
texture's own bytes, nothing else. (`GetTexture`'s real path *does* fold the
TLUT into `full_hash`; the fast path bypasses it.)

With GPU palette conversion that is correct: the palette is applied at draw time
from the live TLUT, so a cached entry cannot go stale. **Without it, Dolphin
bakes the palette into the decoded texture**, and a cached entry carries the
palette that was current when it was *first* decoded.

For this texture the source bytes are all zero and never change, so the hash
matches for ever, the entry is never re-decoded, and the empty palette baked in
at the first decode survives the whole level. The floor is black in the browser
and correct on every backend that has texture buffer objects.

**And it is why the earlier palette test came back clean.**
`MODERNGEKKO_NO_PALETTE_CONVERSION=1` was run *from a savestate*, where the
texture was already cached with a correct palette -- the ordering that causes
the bug had happened long before the state was saved. Forcing a capability off
does not reproduce a defect that depends on when something was first decoded.

**The obvious fix does not work, and that is informative.** Taking the slow path
for paletted textures when `!g_backend_info.bSupportsPaletteConversion` -- so
every bind re-decodes rather than reusing a cached entry -- leaves the floor
black: **0.966** natively with the CPU path forced against 0.967 without it, and
0.65-0.86 in the browser against 0.89-0.96, which is scene variation rather than
a fix. Reverted.

So the cached entry is not stale. Re-decoding produces the same black, which
means **the palette the decode reads is the wrong one, not an old one.**

And the browser's log already says the palette it read is *not empty*:
`tlut 32 bytes at tmem 0x40000 is not zero`. A 32-byte C4 palette with data,
read from the slot the game actually loaded. With every index zero, the texel is
palette **entry 0** -- and that entry can perfectly well be transparent black
inside a palette that is not all zeros. So the CPU decode may be doing exactly
the right thing with the palette it is given, and the difference is that the GPU
path applies a *different* palette at draw time.

**Not the unconverted EFB copy either.** `GetTexture`'s EFB-copy branch returns
a paletted copy *raw* when the backend cannot convert palettes --
`if (!GetPaletteSize() || !bSupportsPaletteConversion) return entry;` -- which
would sample C4 indices as colour and come out black. It fires **zero times**
over a full run with the CPU path forced, and `0x009a9e60` never reaches it. The
floor is decoded from RAM, exactly as the census said. A tripwire is left there.

**Where to pick this up.** The defect is now reproducible on the desktop with one
environment variable, `MODERNGEKKO_NO_PALETTE_CONVERSION=1` **from boot**, which
makes it debuggable natively at desktop speed instead of through a 4.5-minute
browser run.

### The asymmetry, finally located: the desktop never decodes this texture

`GFX_ENABLE_GPU_TEXTURE_DECODING` defaults to **false**, so both builds use the
same CPU `TexDecoder_Decode` with the same `GetTlutAddress()`. Same code, same
pointer -- and yet:

| | `0x009a9e60` in the decode census |
|---|---|
| Chrome | present, twice, in two separate runs (`srcnz=0 dstnz=0`) |
| desktop | **absent -- it is never CPU-decoded at all** |

Confirmed twice: the ungated decode census over a whole run lists 365 textures
natively and `0x009a9e60` is not among them, and a run instrumented to print the
palette bytes for exactly that address printed nothing.

So the desktop's floor texture never enters `CreateTextureEntry`'s decode path.
It is being served from somewhere that path does not reach -- an EFB copy held in
VRAM is the obvious candidate, since those are registered without ever being
decoded. In the browser there is no such entry, so Dolphin falls back to
decoding guest RAM, which is all zeros, and the floor is black.

That also explains why the `[palcopy]` tripwire never fired: reaching it requires
*finding* an EFB copy at that address, and in the browser there is none to find.

### The line

Both builds make **identical** EFB copies -- three of them, including
`dst=009a9e60 256x256 stride=1024`, the floor. So the copy exists in the browser
too, and it is the *lookup* that throws it away. `GetTexture`:

    if ((base_hash == entry->hash &&
         (!texture_info.GetPaletteSize() || g_backend_info.bSupportsPaletteConversion)) ||
        IsPlayingBackFifologWithBrokenEFBCopies)

For a **paletted** EFB copy the second clause is `bSupportsPaletteConversion`.
On the desktop it is true and the copy is used. On WebGL2 it is false, so the
copy fails the test, is pruned as not useful, and Dolphin falls through to
decoding the texture out of guest RAM -- which is all zeros, because an EFB copy
kept in VRAM is never written back. **That is the black floor.**

Everything fits: the copy is made, guest RAM is legitimately empty, both builds
decode the same identical bytes when they decode at all, and the desktop never
CPU-decodes this texture because it never needs to.

(The `[palcopy]` tripwire sits in a different branch further down -- the
by-address loop -- which is why it fired zero times and briefly looked like an
acquittal.)

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
