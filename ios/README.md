# DolBundler for iOS

A GameCube disc goes in, the app recompiles it to DolVM bytecode on the device,
and it shows up in a library you tap to play. No Mac in the loop, and nothing
the app produces is ever mapped executable.

```
   .iso  ──▶  extract  ──▶  recompile  ──▶  .dvm  ──▶  ▶ play
              (DolRecomp)    (DolVM)                    (ModernGekko + Metal)
```

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

## Getting a disc onto the device

Either tap **+** in the app and pick an `.iso`, or drop one into the DolBundler
folder in the Files app. `UIFileSharingEnabled` and
`LSSupportsOpeningDocumentsInPlace` are both set, so the app's Documents folder
is reachable from Files, iCloud Drive, or a Mac over USB.

GameCube `.iso`, `.ciso`, and NKit `.nkit.iso` all work. CISO support was added
to DolRecomp's native extractor for this (it detects the format by magic, not
by extension, so a mislabelled file still opens), and NKit images extract
because they keep the disc header and FST intact.

`.rvz` and Wii discs do not work: both need the `wit` bridge, which cannot run
on iOS (see *No subprocesses* below).

## Why this is allowed on the App Store

Two separate rules matter, and the architecture answers both.

**Emulators are permitted.** App Store Review Guideline 4.7 has allowed retro
game console emulators, including ones that run user-provided games, since
April 2024.

**Nothing here generates executable code.** Guideline 2.5.2 forbids an app from
generating or running code. DolBundler never does: `dolrecomp` lowers the
disc's PowerPC to *bytecode*, and `dolvm_interp.c` reads that bytecode as data.
There is no JIT and no executable memory. Verified against the built binary:

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
`fork`/`exec` at all. Both halves of the pipeline are linked into the app and
called in-process instead:

| macOS | iOS |
|---|---|
| `ModernGekko --extract` | `disc_extract_gamecube()` |
| `moderngekko-port` → `popen(dolrecomp)` | `emit_dol_split(..., BACKEND_VM, ...)` |
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
controller is connected.

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
| All JIT backends | `ENABLE_GENERIC`; the whole point of DolVM |
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

There is no way to script a tap, so `DOLBUNDLER_AUTOPLAY=<disc id>` starts a
game directly. It works on both:

```sh
SIMCTL_CHILD_DOLBUNDLER_AUTOPLAY=GLME01 xcrun simctl launch --console-pty <udid> com.bigmah.dolbundler
DEVICECTL_CHILD_DOLBUNDLER_AUTOPLAY=GLME01 xcrun devicectl device process launch --device <id> com.bigmah.dolbundler
```

Crash reports land in `~/Library/Logs/DiagnosticReports/DolBundler-*.ips`.

One more loop worth knowing: a macOS build with `ENABLE_GENERIC=ON` runs the
same JIT-less StaticRecomp/DolVM path against the same `.dvm`, in seconds, with
a debugger. It will not reproduce anything iOS-specific, but it tells you
straight away whether the interpreter or the module is at fault.

## Known limitations

**Speed is the real constraint, not the port.** Measured on an M4 Pro (cpu
time over each title's first six billion guest cycles), DolVM runs Mario Party
4 at 2.94× realtime, the SpongeBob movie game at 1.81×, and Melee at 1.39×.
An A17 Pro core has roughly three quarters of that single-thread throughput
and throttles under sustained load, so expect Melee-class titles to sit near
full speed on a 15 Pro and lighter ones comfortably above it. Nothing about the
iOS port changes that; it is a property of the interpreter, and
`ModernGekko/vendor/dolphin/DolRecomp/src/vm/README.md` says where the time
goes and what each change bought.

Three things in the build matter for it and are easy to lose: the interpreter
is compiled with one indirect branch per handler (`-mllvm -tail-dup-*-size`),
with LTO over the cpu helpers (and the same `-mllvm` flags handed to the
linker, or LTO undoes the first), and with `-mcpu=apple-a16` rather than
`native`, which on a cross build would name whatever Mac did the compiling.
All three live in `ModernGekko/CMakeLists.txt` under `moderngekko_dolvm`.

**A module from an older build is rebuilt, not refused.** The bytecode ABI
changes as the interpreter does, and the library checks each `.dvm`'s header
on load. A stale one shows as "needs a quick update" and is recompiled from
the extracted disc the next time it is played -- seconds, since nothing is
extracted again.

**Memory during import.** Peak RSS scales at roughly 1.5 KB per guest
instruction: 423 MB for Mario Party 4, 796 MB for Luigi's Mansion, 1.44 GB for
Melee. An 8 GB device handles all three; smaller devices will be tight on the
largest games, because the whole DolIR is held in memory at once.

**Storage.** A disc costs its ISO plus roughly the same again extracted —
about 2.7 GB for Melee. The importer refuses to start with under 3 GB free.
Deleting the `.iso` after import is safe; the game root and `.dvm` are what
the app plays from.

**GameCube only.** Wii discs need the `wit` bridge to extract, which cannot run
here.
