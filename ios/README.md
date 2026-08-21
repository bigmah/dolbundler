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

On first launch, trust the certificate under **Settings → General → VPN &
Device Management**.

Requirements: Xcode 15+, an iOS 17+ device with an A-series chip. Built and
tested against an iPhone 15 Pro Max.

## Getting a disc onto the device

Either tap **+** in the app and pick an `.iso`, or drop one into the DolBundler
folder in the Files app. `UIFileSharingEnabled` and
`LSSupportsOpeningDocumentsInPlace` are both set, so the app's Documents folder
is reachable from Files, iCloud Drive, or a Mac over USB.

Only uncompressed GameCube `.iso` works. `.ciso`, `.rvz`, and `.gcm` do not —
DolRecomp's native extractor reads `.iso` and `.wbfs`, and the `wit` bridge it
would otherwise fall back to cannot run on iOS (see *No subprocesses* below).

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

## What is disabled, and why

| Dropped | Reason |
|---|---|
| All JIT backends | `ENABLE_GENERIC`; the whole point of DolVM |
| cubeb (audio) | The vendored copy declares macOS-only CoreAudio globals at file scope with no iOS guard. **There is currently no audio.** |
| libusb, hidapi | Both need IOKit. The devices they drive can't attach to an iPhone anyway |
| Quartz input | Carbon key codes and CoreGraphics mouse events |
| FSEvents watcher | No FSEvents on iOS; asset hot-reload is a no-op |
| AGL / OpenGL | Metal only |
| NoGUI / Qt frontends, updater | Desktop-only; the app links the libraries directly |

## Known limitations

**No audio.** cubeb is disabled, so `AudioCommon` falls back to its null
stream. This is the largest gap and the next thing to fix — either by guarding
the vendored cubeb's macOS-only globals, or by adding an `AVAudioEngine`
backend.

**Speed is the real constraint, not the port.** Measured on an M-series Mac,
DolVM runs Mario Party 4 at 1.72× realtime, the SpongeBob movie game at 1.23×,
and Melee at 0.93× — Melee is already below full speed on a desktop. A phone
has less sustained throughput and throttles hard under the kind of load an
emulator generates, so expect lighter titles to play and Melee-class ones not
to. Nothing about the iOS port changes that; it is a property of the
interpreter.

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
