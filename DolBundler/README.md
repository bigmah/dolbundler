# DolBundler

A macOS app for recompiling GameCube and Wii disc images into native code.
Add a `.iso`, watch it recompile, and play it from the library. The disc's
PowerPC code is statically recompiled to native machine code — the game runs
as compiled code, not as interpreted instructions.

```
mario_party_4.iso  ──▶  DolBundler  ──▶  ~/Applications/Mario Party 4.app
```

## What it is made of

| Piece | Role |
|---|---|
| `DolRecomp` | reads the disc's `main.dol`, decodes PowerPC, emits C |
| `ModernGekko` | the runtime: GX/Metal video, audio, input, memory, disc I/O |
| `DolBundler/gui` | the Dioxus window — library, live console, one-click play |
| `DolBundler/src` | the pipeline the window drives, usable on its own |

`DolRecomp` and `ModernGekko` are upstream projects. `DolBundler` is the glue
that turns their CLI tools into something you can double-click.

`ModernGekko` is a pinned submodule at the repo root. `DolRecomp` is not a
direct dependency — it is built from `ModernGekko/vendor/dolphin/DolRecomp`
through ModernGekko's own submodule chain, so that is the tree to edit if you
need to hack on the recompiler. See the [root README](../README.md) for the
full wiring and for how to bump the pin.

## Install

```sh
git clone --recursive https://github.com/<you>/recomp_gc.git
cd recomp_gc
./DolBundler/build.sh
```

If you forgot `--recursive`, `build.sh` initialises the ModernGekko submodule
for you.

The first run fetches ModernGekko's vendored RecompCore/Dolphin tree (a few
hundred MB), builds it, builds the window, and installs `DolBundler.app` to
`~/Applications`. A cold build compiles all of Dolphin, so expect it to take a
while; later runs reuse the build directory.

Requirements: Xcode command line tools, `cmake`, `ninja`, `python3`, `cargo`.
Apple Silicon and Intel are both fine — the recompiled module is built for
whichever architecture you build on.

Options:

```sh
./build.sh --rebuild      # wipe ModernGekko/build and start over
./build.sh --no-install   # leave DolBundler.app in this directory
```

## Use

Open DolBundler. Three ways to add a game:

- **Add disc image…** in the top right
- **Drop** a disc image anywhere in the window
- **Drop it on the app icon**, or `open -a DolBundler game.iso` from a shell

Four steps run, with live output in the console pane:

1. **Extract** the disc into `~/.local/share/moderngekko/games/<DISC_ID>`
2. **Read** the disc header for the game's name, ID, and platform
3. **Recompile** the DOL to a native `.dylib`, cached under
   `~/.cache/moderngekko/modules/<DISC_ID>/`
4. **Add** it to the library, with its banner art

That is all it takes to play. Building `~/Applications/<Game Name>.app` is a
separate step you ask for per game — **Create App** on the card, or `--app` on
the command line. Without it nothing is written to `~/Applications`, so a disc
you only wanted to try does not leave an icon behind.

Step 3 is the slow one — a few minutes for a GameCube game. It is keyed on the
DOL hash, the compiler, and the recompiler revision, so adding the same disc
again is instant and reuses the cached module.

The game then sits in the library with its banner art, decoded from the disc's
own `opening.bnr`. Per game:

| Button | What it does |
|---|---|
| **Play** | runs the game |
| **Settings** | resolution, backends, and controllers for this game alone |
| **Create App** | builds `~/Applications/<Game>.app`; disappears once one exists |
| **Log** | loads that game's last 200 runtime log lines into the console |
| **Reveal** | shows the app in Finder, or the extracted disc if there is none |
| **Remove** | drops the library entry and its settings; nothing on disk is deleted |

The generated apps are ordinary macOS apps. They hold no game data — each one
points at the extracted disc, the cached module, and this ModernGekko build —
so you can launch them straight from Launchpad or the Dock without DolBundler
running. That independence is the reason to make one; if you are happy
launching from the library, you never need to.

### Settings

**Settings** in the top right holds the defaults every game starts from.
**Settings** on a card overrides any of them for that game alone; each control
there reads `Default (…)` until you change it, and the card is tagged *custom
settings* once something differs. **Use defaults for everything** puts a game
back on the defaults.

| Setting | |
|---|---|
| **Internal resolution** | what the game renders at, not the window size — `640x528` (native) through `7680x4320` |
| **Graphics backend** | Metal, Vulkan through MoltenVK, or OpenGL |
| **Audio backend** | Automatic, Cubeb, or muted |
| **Fullscreen**, **Show FPS in the title bar** | on or off |
| **Ports 1–4** | which gamepad drives each controller port |

Only gamepads SDL recognises are offered; **Rescan** picks up one plugged in
after the window opened. Choosing one writes a full button profile for it —
`Config/GCPadNew.ini` for a GameCube game, `Config/WiimoteNew.ini` for a Wii
one — using ModernGekko's own standard mapping. Leave every port on *None* and
nothing is bound, which is what you want if you keep a hand-written profile:
DolBundler only overwrites a profile it wrote itself, marked by a
`# Written by DolBundler.` first line.

Settings live in `settings.json` beside `library.json`. Nothing is stored in
the runtime's own configuration, because it has no per-game layer: the
resolved settings are written into `config.ini` and the controller profile in
the moment before a game starts, and rewritten for the next game.

That last part is the one limitation worth knowing. A generated `.app`
launched from Finder or the Dock reads whatever those two files hold from the
last game DolBundler started. Its graphics and audio backends are its own —
they are baked into `Contents/Resources/game.conf` and kept in step whenever
that game's settings change — but its resolution, fullscreen, and controllers
are not. Start it from the library and it always gets its own.

### Without the window

The same pipeline runs headless, which is handy for scripting or debugging:

```sh
R=~/Applications/DolBundler.app/Contents/Resources
$R/recompgc game.iso           # recompile and add to the library
$R/recompgc --app game.iso     # also build ~/Applications/<Game>.app
```

Two subcommands act on a game that is already recompiled — they are what the
window calls, and they need neither the disc image nor a compiler:

```sh
$R/recompgc play     --disc-id GGVE78 --game-root <dir> --module <lib> --title <name>
$R/recompgc make-app --disc-id GGVE78 --game-root <dir> --module <lib> --title <name>
```

Both take `--graphics <backend>` and `--audio <backend>`, which is how the
window hands a game its own; without them the build-wide default from
`toolchain.conf` applies. A third subcommand lists the gamepads the runtime can
see, one `SDL/<index>/<name><TAB><label>` per line — that first field is what a
controller port has to be set to:

```sh
$R/recompgc list-controllers
```

Add `--porcelain` for the machine-readable event stream the window consumes;
the protocol is documented at the top of the script.

## Notes and limitations

- **The checkout has to stay put.** `moderngekko-port` has the ModernGekko
  source path compiled into it and reads its module template, GX runtime, and
  per-game Dolphin settings from there. Generated apps also reference
  `ModernGekko/build` by absolute path. If you move or delete either, re-run
  `build.sh` and re-add your discs.
- **Metal, not Vulkan.** ModernGekko's `config.ini` only accepts Vulkan and
  OpenGL, neither of which exists on macOS, so the backend travels on the
  command line as `--graphics Metal` instead and that key is left at a value
  the config parser will accept. Pick a different one per game in Settings, or
  change `GRAPHICS_BACKEND` in
  `DolBundler.app/Contents/Resources/toolchain.conf` for the build-wide default.
- **Three upstream patches**, in `patches/`, applied by `build.sh` (idempotently
  — a patch that reverse-applies cleanly is treated as already in the tree):
  - `0001-accept-unpinned-discs` — an unbranded ModernGekko build pins no disc
    ID and ships no disc preparer, so `PrepareDisc()` falls into a branch that
    rejects every image. Without this, no disc can be added at all.
  - `0002-dol-patch-widths-and-conditionals` — `moderngekko-port` bakes
    Dolphin's enabled `[OnFrame]` patches into the DOL before recompiling, but
    only parsed the 32-bit `dword` form. Dolphin also emits `byte` and `word`
    widths and a four-field conditional form, and the recompiler refused to
    build any game whose INI used one. That is 21 of the 127 games that ship
    enabled patches, The SpongeBob SquarePants Movie among them.

    A conditional patch means "write only if memory already holds the
    comparand". Dolphin re-tests that against live memory every frame; against
    a static DOL the only thing to test is what the image ships with, so a
    condition that does not hold is simply not applied.

  Run `./src/check_game_patches.py` after bumping the vendored Dolphin tree to
  see whether any newly shipped INI uses a form the patcher still rejects.
  - `0003-list-controllers` — adds `moderngekko-run --list-controllers`, which
    prints the SDL gamepads Dolphin's input backend will see. The device string
    a controller profile needs is SDL's own name for the pad, so guessing it
    from outside is not an option; the runner already links SDL, and this is
    the same enumeration the ModernGekko launcher does for its own picker.
- **Neither app is signed or notarized.** They are built locally and run
  locally. Gatekeeper may want a right-click → Open the first time.
- **Not every game will work.** DolRecomp covers 236 opcodes and does not
  handle self-modifying code; ModernGekko's GX and hardware coverage is
  a work in progress. A game that boots is not a game that finishes.
- Bring your own disc images. Nothing here ships game data.

## Where things live

```
~/Applications/DolBundler.app                       the window
~/Applications/<Game>.app                           one per recompiled game
~/Library/Application Support/DolBundler/
    library.json                                    the library index
    settings.json                                   defaults and per-game overrides
    covers/<DISC_ID>.png                            banner art for the list
~/.local/share/moderngekko/games/<DISC_ID>/         extracted disc
~/.local/share/moderngekko/Logs/<DISC_ID>.log       runtime log
~/.local/share/moderngekko/config.ini               written per launch
~/.local/share/moderngekko/Config/GCPadNew.ini      controller profile, GameCube
~/.local/share/moderngekko/Config/WiimoteNew.ini    controller profile, Wii
~/.cache/moderngekko/modules/<DISC_ID>/             recompiled modules
```

## Layout

```
DolBundler/
  build.sh                 builds everything and installs the app
  gui/                     the Dioxus window (Rust)
    src/main.rs            UI, job state, drop and open handling
    src/pipeline.rs        runs recompgc, parses its porcelain output
    src/library.rs         library.json, cover art, log tailing
    src/settings.rs        settings.json, and the config.ini and pad profile
                           it renders into before each launch
    assets/style.css
  patches/                 the ModernGekko fixes build.sh applies
  src/
    recompgc               the four-step pipeline
    make_game_app.py       library entry and cover art; .app only with --app
    make_app_icon.py       DolBundler's own icon
    check_game_patches.py  audits Dolphin's GameSettings against the patcher
```

## License

GPL-3.0-or-later, same as the upstreams it builds on. See
[`LICENSE`](../LICENSE) at the repo root.
