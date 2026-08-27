# DolBundler for iOS

A GameCube disc is recompiled to native arm64 on a Mac and linked into the app
before it is signed. On the phone the disc goes in, gets extracted, and shows up
in a library you tap to play against the module already inside the binary.

```
   on a Mac:   main.dol ──▶ DolRecomp --backend llvm ──▶ .o ──▶ link ──▶ sign
   on the phone:  .iso ──▶ extract ──▶ game root ──▶ ▶ play
                          (DolRecomp)                (ModernGekko + Metal)
```

Nothing is generated or mapped executable at runtime: the guest code is part of
the signed binary, and the phone only ever reads the disc's data. That is also
why a disc this build was not compiled for can be imported and stored but not
played -- the library says so rather than booting into something unusably slow.

## Building

```sh
export DOLBUNDLER_TEAM=<your Apple Developer team ID>
./ios/build.sh --install
```

That configures, builds, and installs to a connected device. Without
`--install` it just builds; without `DOLBUNDLER_TEAM` it builds unsigned.
With more than one iPhone paired, say which with `DOLBUNDLER_DEVICE` -- a
`devicectl` identifier or any part of the phone's name (`DOLBUNDLER_DEVICE="15
pro max"`); the script lists the candidates rather than guess.

On first launch, trust the certificate under **Settings → General → VPN &
Device Management**.

Requirements: Xcode 15+, an iOS 17+ device with an A-series chip. Built and
tested against an iPhone 15 Pro Max.

### Building a game into the app

**The short way is the Mac app.** DolBundler's **iPhone** button does every
step below — recompile, link, sign, install — and copies the extracted disc
onto the phone afterwards, so the game is in the phone's library rather than
merely linked into the binary. See
[DolBundler/README.md](../DolBundler/README.md#on-an-iphone). From a shell:

```sh
~/Applications/DolBundler.app/Contents/Resources/recompios send
```

The rest of this section is the same thing by hand, which is what you want when
you are changing the recompiler, tuning a target, or building with PGO. A
module built that way is brought into DolBundler's store with

```sh
recompios adopt --disc-id GEXE52 --generated /tmp/gexe52-llvm-ios/generated
```

so the next `send` links it in and leaves it alone; without that step the next
send would produce an app missing that game.

Build the host recompiler with the pinned LLVM 20 wrapper, emit iPhoneOS objects
on the Mac, and embed them before Xcode signs the app:

```sh
./ios/build-dolrecomp-llvm20.sh

export DOLRECOMP_LLVM_TARGET=arm64-apple-ios17.0
export DOLRECOMP_LLVM_CPU=apple-a16
export DOLRECOMP_LLVM_CACHE=/tmp/dolbundler-gexe52-llvm-cache

build-dolrecomp-llvm20/dolrecomp -j"$(sysctl -n hw.ncpu)" \
  --gamecube --backend llvm \
  /path/to/GEXE52/sys/main.dol /tmp/gexe52-llvm-ios
cp /path/to/GEXE52/sys/main.dol /tmp/gexe52-llvm-ios/generated/main.dol

export DOLBUNDLER_NATIVE_GAME_ID=GEXE52
export DOLBUNDLER_NATIVE_GENERATED_DIR=/tmp/gexe52-llvm-ios/generated
export DOLBUNDLER_TEAM=<your Apple Developer team ID>
./ios/build.sh --install
```

Several titles can coexist in one signed app. Give each generated module a
unique C-identifier prefix while recompiling, then pass all `GAME_ID=directory`
pairs to the iOS build:

```sh
export DOLRECOMP_LLVM_SYMBOL_PREFIX=gG4QE01_
build-dolrecomp-llvm20/dolrecomp -j"$(sysctl -n hw.ncpu)" \
  --gamecube --backend llvm \
  /path/to/G4QE01/sys/main.dol /tmp/g4qe01-llvm-ios
cp /path/to/G4QE01/sys/main.dol /tmp/g4qe01-llvm-ios/generated/main.dol

export DOLBUNDLER_NATIVE_MODULES='GEXE52=/tmp/gexe52-llvm-ios/generated;G4QE01=/tmp/g4qe01-llvm-ios/generated'
./ios/build.sh --install
```

The app selects the embedded descriptor whose disc ID matches the library
entry. The older `DOLBUNDLER_NATIVE_GAME_ID` and
`DOLBUNDLER_NATIVE_GENERATED_DIR` variables remain available for a one-title
build.

`-j` is worth passing every time. dolrecomp emits one object per guest-code
chunk and a GameCube game has thousands of them, but the default is `-j1`, not
a core count — the flag reads as being about the split C output and is not.

The wrapper rejects an unversioned or non-20 LLVM and keeps the host tool out
of `build-ios`. The module configure step validates its target, minimum OS,
CPUState layout, and complete undefined-symbol inventory; the final app link
uses `-undefined error` and writes a native link map. Generated game code stays
outside the repository. No compiler is linked into the phone app, and no
object is added or changed after signing.

## Getting a disc onto the device

The Mac app does this for you: a `send` copies each game's already-extracted
disc into `Documents/games/<DISC_ID>` over `devicectl`, which skips the minutes
the phone would otherwise spend extracting it. The phone builds its library by
scanning that folder, so a game appears there the next time the app is opened.

By hand, either tap **+** in the app and pick an `.iso`, or drop one into the
DolBundler folder in the Files app. `UIFileSharingEnabled` and
`LSSupportsOpeningDocumentsInPlace` are both set, so the app's Documents folder
is reachable from Files, iCloud Drive, or a Mac over USB.

GameCube `.iso`, `.ciso`, and NKit `.nkit.iso` all work. CISO support was added
to DolRecomp's native extractor for this (it detects the format by magic, not
by extension, so a mislabelled file still opens), and NKit images extract
because they keep the disc header and FST intact.

`.rvz` and Wii discs do not work: both need the `wit` bridge, which cannot run
on iOS (see *No subprocesses* below).

The disc is probed before it is extracted — a 0x440-byte read — so a file that
is not a GameCube image is rejected in a moment rather than after a minute of
pointless work, and the progress card can name the game rather than the file.
Import runs beside the library instead of behind a modal: it takes minutes, and
the one thing there is to do while waiting is look at what is already there.

There is no percentage from the extractor, so the card polls the growing game
root once a second and shows the bytes written, which is exact. The bar beside
it is an estimate — a plain `.iso` is a byte-for-byte image so its own size is
the denominator, a CISO is compressed and its size says nothing so a full
GameCube disc is used as a ceiling instead — and it is clamped short of the end
rather than allowed to sit at 100% while the extractor is still working.

## In a game

The corner button opens a panel that pauses the emulated machine rather than
leaving it running behind a menu, using `Runtime::Pause()`. It carries an
overlay-opacity slider, a haptics switch, the fps/speed readout's switch, and
Quit. It exists because the alternative was a bare ✕ that quit without asking,
on a screen whose entire surface is a controller — a mis-tap in the corner
ended the session.

The button and the readout sit at the top *centre*. The corners are where L and
R are, and those belong under the index fingers and cannot move.

Backgrounding the app pauses the game and returning resumes it, unless the
panel is open — someone who opened it before switching away still has it open,
and resuming underneath it is what the panel exists to prevent.

There is no "Starting <game>…" placeholder. The one that used to be here was a
label on an eighteen-second timer, sized for the twelve seconds the removed
bytecode path spent verifying a module against the DOL. Native AOT boots
straight into the game, and a fixed timer that outlives what it was measuring
is worse than nothing: it covers the first frames of a game that has already
started.

## Why this is allowed on the App Store

Two separate rules matter, and the architecture answers both.

**Emulators are permitted.** App Store Review Guideline 4.7 has allowed retro
game console emulators, including ones that run user-provided games, since
April 2024.

**Nothing here generates executable code.** Guideline 2.5.2 forbids an app from
generating or running code, or loading code it did not ship with. DolBundler
does neither: every instruction the guest runs was compiled on a Mac and signed
as part of the binary, and the only thing the phone produces is the extracted
disc's *data*. There is no JIT, no `dlopen`, and no executable memory allocated
at runtime. Verified against the built binary:

```sh
APP=build-ios/Release-iphoneos/DolBundler.app/DolBundler
nm "$APP" | grep -c JitArm64                 # 0 -- no ARM JIT backend
nm "$APP" | grep -c Jit64                    # 0 -- no x86 JIT backend
nm "$APP" | grep AllocateExecutableMemory    # absent -- linker dropped it
```

`AllocateExecutableMemory()` in `Common/MemoryUtil.cpp` is the only place
Dolphin asks for `PROT_EXEC` or `MAP_JIT`, and with no JIT to call it the
linker drops it entirely. The binary does import `mprotect`, but the only
reachable callers are `WriteProtectMemory(..., allowExecute=false)` and
`ReadProtectMemory()` on the emulated DSP's instruction RAM — `PROT_READ` and
`PROT_NONE`, guest data protection with no executable mapping involved.

`JitInterface` symbols are present: that is Dolphin's CPU-core abstraction,
which dispatches to the interpreter here, not a JIT.

The load-bearing build flag is `ENABLE_GENERIC=ON`. Dolphin picks a JIT backend
from the target architecture, and iOS is arm64 — so a default build would
compile `JitArm64` straight into the binary. `ENABLE_GENERIC` is Dolphin's own
supported switch for a JIT-less build, and `ios/CMakeLists.txt` refuses to
configure without it rather than let a rejectable binary get produced by
accident.

## How it differs from the macOS build

The macOS pipeline is four binaries wired together by a shell script
(`DolBundler/src/recompgc`), and each game becomes a `.app` whose launcher is a
bash script that `exec`s `moderngekko-run`. None of that survives on iOS.

**No subprocesses.** iOS marks `system()` unavailable at compile time and has no
`fork`/`exec` at all. What remains of the pipeline on the phone is linked into
the app and called in-process instead:

| macOS | iOS |
|---|---|
| `ModernGekko --extract` | `disc_extract_gamecube()` |
| `moderngekko-port` → `popen(dolrecomp)` | done on the Mac, linked in before signing |
| launcher script → `exec moderngekko-run` | `Runtime::Create()` / `Run()` |

`ios/bridge/dolbundler_core.h` is the import half, `dolbundler_run.h` the play
half. Two DolRecomp functions were added for this — `disc_extract_gamecube()`
and `disc_probe_gamecube()` — because the only entry point was previously
`disc_extract_main(argc, argv)`, and there is no command line to synthesise.

**The app owns the window.** On macOS `PlatformMacos.mm` creates an `NSWindow`,
runs an `NSApplication` loop, and hands its content view to the video backend.
On iOS that is inverted: `DBMetalView` is backed by a `CAMetalLayer`, the app
hands the layer over with `Platform::SetIOSRenderLayer()`, and `PlatformIOS.mm`
only reports it and pumps Dolphin's host job queue. UIKit owns the run loop, and
`Runtime::Run()` blocks, so it runs on its own `QOS_CLASS_USER_INTERACTIVE`
thread.

**Input.** Physical controllers work through Dolphin's existing SDL backend —
SDL3 builds for iOS and handles MFi and Bluetooth pads. The on-screen pad feeds
`ciface::Touch`, the same input overrider the Android overlay uses, which
carries no Android dependencies. The overlay hides itself whenever a real
controller is connected, and drops whatever it was holding as it goes.

Every control the hardware has is on screen: both analog sticks, the D-pad, A,
B, X, Y, Z, L, R and Start. They are laid out and coloured the way a GameCube
pad is, because the point of colouring them at all is that someone who has held
one can find A without reading the letter.

Three things about it are less obvious than they look:

- **The main stick floats.** A touch anywhere in the lower left that misses
  every button picks the stick up and re-centres it under the thumb. Finding an
  exact circle without looking is the thing people get wrong, and a stick that
  has to be found is a stick that is not being used. The C stick does not float
  — the right side is crowded with buttons and a floating stick there would fire
  on stray taps — so it stays put with a generous hit radius instead.
- **The triggers report twice.** Pressing L sets both `GCPAD_L_ANALOG` and
  `GCPAD_L_DIGITAL`. A game may read either the analog depth or the click at
  the bottom of the throw, and which one it uses is not knowable from here.
- **The outlines are light, not dark.** A dark outline disappears into the dark
  scenes games spend most of their time in, and a translucent fill is too faint
  to carry a control on its own there. Against a bright scene the fill is what
  separates the shape, so the pale line can afford to lose that fight.

Nothing is drawn with `-drawRect:`. Each control owns a `CAShapeLayer`, so
pressing a button repaints that button rather than rasterising the whole
overlay — which sits on top of a Metal layer that is the thing being measured.
Overlay opacity is applied per layer for the same reason: a translucent *view*
with this many sublayers forces Core Animation into an offscreen group-opacity
pass every frame.

**Dolphin's `Sys` folder rides inside the app.** The desktop build copies
`Data/Sys` next to `moderngekko-run`; `ios/CMakeLists.txt` copies the same tree
to `DolBundler.app/Sys`. Flat, not `Contents/Resources/Sys`: an iOS bundle has
no `Contents`, and ModernGekko forces `LINUX_LOCAL_DEV`, which is what makes
Dolphin's `SYSDATA_DIR` a plain `Sys` — so `GetSysDirectory()` comes out as
`GetBundleDirectory() + "/Sys/"`, which is exactly where an iOS resource
belongs.

It is load-bearing, and the way it fails is worth knowing. `Sys/GC/
font_western.bin` is the GameCube's ROM font: a game asks the IPL for it and
draws its system text with it, so with the file absent `CEXIIPL` hands back a
page of zeroes and the game draws every glyph as nothing. Star Fox Assault's
menus are drawn that way, which is how the missing folder was found — the
memory card prompt came up as an empty box with a highlighted button and no
words anywhere. Dolphin says so, but only through a `PanicAlert`, which on a
phone goes nowhere anyone will look.

## Audio

`AudioCommon/IOSSoundStream.mm` outputs through a RemoteIO audio unit at 48 kHz
stereo, which is the format `Mixer` already produces. It registers as the
`AudioUnit` backend and is the default on iOS.

It exists because the vendored cubeb cannot build for iOS: its AudioUnit
backend declares macOS-only CoreAudio device-enumeration types and constants at
file scope, and the errors run the length of the file. None of that machinery
is needed here -- iOS has one output device, the system owns it, and the app
never enumerates or switches it -- so this talks to RemoteIO directly, which is
the same thing cubeb would do underneath.

The session category is `Playback`, so a game keeps its sound with the ringer
switch off, and the session is deactivated with
`NotifyOthersOnDeactivation` when the stream is torn down so other audio is not
left ducked.

## What is disabled, and why

| Dropped | Reason |
|---|---|
| All JIT backends | `ENABLE_GENERIC`; guest code is AOT-compiled and signed, so nothing needs to be generated |
| cubeb (audio) | The vendored copy declares macOS-only CoreAudio globals at file scope with no iOS guard -- 164 compile errors spread through the file, which is a fork of a third-party library rather than a patch. Replaced by `IOSSoundStream`, a RemoteIO audio unit. |
| libusb, hidapi | Both need IOKit. The devices they drive can't attach to an iPhone anyway |
| Quartz input | Carbon key codes and CoreGraphics mouse events |
| FSEvents watcher | No FSEvents on iOS; asset hot-reload is a no-op |
| AGL / OpenGL | Metal only |
| NoGUI / Qt frontends, updater | Desktop-only; the app links the libraries directly |

## Debugging a failed boot

A boot failure on iOS is a SIGKILL: no crash report, no stdout, and Dolphin's
own log stops part way through. Three things exist because of that.

`Documents/moderngekko/dolbundler-run.log` gets a line per boot step with the
process footprint at that moment, written straight through and flushed, so it
survives the kill. `Core.cpp` traces the core boot into the same file through a
function pointer that is null everywhere except this app. And
`DOLBUNDLER_DEBUG_LOG=1` turns Dolphin's own log on at LDEBUG -- useful, but it
writes a line per DVD read, so leave it off unless you are chasing something.

**The simulator is the better loop.** It produces a real crash report with a
stack trace, which the device does not:

```sh
cmake -S ios -B build-sim -G Xcode -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=iphonesimulator -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 -DENABLE_GENERIC=ON
xcodebuild -project build-sim/DolBundlerIOS.xcodeproj -target DolBundler \
  -configuration Release -sdk iphonesimulator -arch arm64 CODE_SIGNING_ALLOWED=NO build
```

A simulator build is silent: it would otherwise play through the Mac's
speakers, and the loop here is launch-run-relaunch. `DOLBUNDLER_SIM_AUDIO=1`
turns the sound back on when audio is the thing being worked on. Device builds
are unaffected -- the switch is `TARGET_OS_SIMULATOR`, a compile-time property
of a separate binary.

There is no way to script a tap, so `DOLBUNDLER_AUTOPLAY=<disc id>` starts a
game directly. It works on both:

```sh
SIMCTL_CHILD_DOLBUNDLER_AUTOPLAY=GLME01 xcrun simctl launch --console-pty <udid> com.bigmah.dolbundler
DEVICECTL_CHILD_DOLBUNDLER_AUTOPLAY=GLME01 xcrun devicectl device process launch --device <id> com.bigmah.dolbundler
```

Crash reports land in `~/Library/Logs/DiagnosticReports/DolBundler-*.ips`.

One more loop worth knowing: a macOS build with `ENABLE_GENERIC=ON` runs the
same JIT-less StaticRecomp path against a module recompiled for the Mac, with a
debugger. It will not reproduce anything iOS-specific, but it tells you straight
away whether the chassis or the generated code is at fault.

## Known limitations

**Only games built into this app can be played.** A module is generated on a
Mac and linked in before signing, so an imported disc the build does not cover
shows in the library as "not in this build" and refuses to boot. That is not a
policy choice: iOS will not map a page executable unless a valid code signature
backs it, which needs the `dynamic-codesigning` entitlement (WebKit's), and a
dylib produced on the phone could not be signed by anything on it -- so `dlopen`
refuses it too. "Just AOT-compile any ISO on the phone" cannot be made to work.

**Speed.** [`PERFORMANCE.md`](PERFORMANCE.md) is the device-side record: how to
measure emulation speed on a phone without a console attached, and what a
week's worth of measuring found -- including which plausible-sounding fixes
(dual core, the DSP, graphics settings) are already done or worth nothing. Dual
core in particular is worth **-15% on the phone** and +19% on a desktop, so the
desktop does not predict the phone here.

Two things in the build matter and are easy to lose: GXRuntime's guest CPU
helpers are compiled with `-ffp-contract=off -fno-fast-math` so a lane computed
in a helper matches one the generated code inlined, and with `-mcpu=apple-a16`
rather than `native`, which on a cross build would name whatever Mac did the
compiling. Both live in `ModernGekko/CMakeLists.txt` under `moderngekko_gxcpu`,
and module-template applies the same two flags to the generated objects.

**Memory during recompilation.** Peak RSS scales at roughly 1.5 KB per guest
instruction: 423 MB for Mario Party 4, 796 MB for Luigi's Mansion, 1.44 GB for
Melee, because the whole DolIR is held in memory at once. This is now the Mac's
problem rather than the phone's.

**Storage.** A disc costs its ISO plus roughly the same again extracted —
about 2.7 GB for Melee. The importer refuses to start with under 3 GB free.
Deleting the `.iso` after import is safe; the extracted game root is what the
app plays from.

**GameCube only.** Wii discs need the `wit` bridge to extract, which cannot run
here.
