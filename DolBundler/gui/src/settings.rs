// SPDX-License-Identifier: GPL-3.0-or-later
//! Per-game runtime settings, and the two files the runtime reads them from.
//!
//! DolBundler keeps its own store — global defaults plus per-game overrides —
//! in `settings.json` beside `library.json`. That store is the source of truth
//! the window edits. Immediately before a game starts, the resolved settings
//! are rendered into the two files ModernGekko actually reads:
//!
//!   * `<user-dir>/config.ini`                — internal resolution, fullscreen,
//!                                              FPS in the title, controller ports
//!   * `<user-dir>/Config/GCPadNew.ini`       — the button mapping, GameCube
//!     or `<user-dir>/Config/WiimoteNew.ini`  — or Wii, per the game's platform
//!
//! Rendering at launch rather than on save is what makes per-game settings
//! possible at all: both files are global to the ModernGekko user directory, so
//! the only way one game can differ from another is to write them each time.
//!
//! The graphics and audio backends are the exception. They are not in
//! `config.ini` in any usable form — its `backend` key only accepts Vulkan or
//! OpenGL, neither of which exists on macOS — so they ride on `recompgc`'s
//! command line instead.

use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::fmt::Write as _;
use std::path::{Path, PathBuf};

use crate::library;

/// Sentinel for "inherit the global default" in the settings form. No real
/// value can collide with it: resolutions are digits, backends are known
/// words, and controller devices always start with `SDL/`.
pub const INHERIT: &str = "~default";

/// Dolphin's integer EFB scales, labelled by the output resolution they give.
/// Mirrors `SupportedResolutions()` in ModernGekko's `frontend_config.cpp`;
/// `config.ini` is rejected outright if it names anything else.
pub const RESOLUTIONS: [&str; 7] = [
    "640x528",
    "1280x720",
    "1920x1080",
    "2560x1440",
    "3840x2160",
    "5120x2880",
    "7680x4320",
];

/// (stored value, label). The stored value is passed straight to
/// `recompgc --graphics`, which forwards it to Dolphin's `MAIN_GFX_BACKEND`.
pub const GRAPHICS: [(&str, &str); 3] = [
    ("Metal", "Metal"),
    ("Vulkan", "Vulkan (MoltenVK)"),
    ("OGL", "OpenGL"),
];

/// (stored value, label). Empty means "let the runtime choose", which is what
/// it does anyway for a name it does not recognise.
pub const AUDIO: [(&str, &str); 3] = [
    ("", "Automatic"),
    ("Cubeb", "Cubeb"),
    ("No Audio Output", "Muted"),
];

/// (stored value, label) for what a disc is recompiled to, default first. `vm`
/// lowers the recompilation to DolVM bytecode, which takes seconds because
/// nothing is compiled -- the runtime interprets it instead. It is the arm an
/// iOS build has to use, because an App Store binary may not generate or load
/// executable code. `c` is the native path: PowerPC to C to native arm64, a few
/// minutes of compiling per game, and still the faster of the two to play.
pub const BACKENDS: [(&str, &str); 2] = [
    ("vm", "Bytecode (interpreted)"),
    ("c", "Native code"),
];

pub fn default_backend() -> String {
    "vm".into()
}

/// A gamepad `recompgc list-controllers` reported.
#[derive(Clone, Debug, PartialEq)]
pub struct Controller {
    /// `SDL/<index>/<name>`, exactly as a `Device` line needs it.
    pub device: String,
    pub label: String,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Defaults {
    pub resolution: String,
    pub fullscreen: bool,
    pub show_fps: bool,
    pub graphics: String,
    pub audio: String,
    /// One entry per port. Empty means the port is left unbound.
    pub controllers: [String; 4],
}

impl Default for Defaults {
    fn default() -> Self {
        Self {
            resolution: "1920x1080".into(),
            fullscreen: false,
            show_fps: true,
            graphics: "Metal".into(),
            audio: String::new(),
            controllers: Default::default(),
        }
    }
}

/// What one game overrides. `None` anywhere means "take the global default".
#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
pub struct Overrides {
    #[serde(default)]
    pub resolution: Option<String>,
    #[serde(default)]
    pub fullscreen: Option<bool>,
    #[serde(default)]
    pub show_fps: Option<bool>,
    #[serde(default)]
    pub graphics: Option<String>,
    #[serde(default)]
    pub audio: Option<String>,
    #[serde(default)]
    pub controllers: [Option<String>; 4],
}

impl Overrides {
    /// How many settings this game does not take from the defaults.
    pub fn count(&self) -> usize {
        self.controllers.iter().filter(|port| port.is_some()).count()
            + [
                self.resolution.is_some(),
                self.fullscreen.is_some(),
                self.show_fps.is_some(),
                self.graphics.is_some(),
                self.audio.is_some(),
            ]
            .iter()
            .filter(|overridden| **overridden)
            .count()
    }

    /// Whether this game differs from the defaults at all, which is all the
    /// card needs to know to mark its Settings button.
    pub fn is_empty(&self) -> bool {
        self.count() == 0
    }
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Store {
    #[serde(default)]
    pub defaults: Defaults,
    /// What the next disc added is recompiled to. Not a per-game launch
    /// setting: it decides what the recompiler produces, so it only applies
    /// when a game is built, and a game already in the library keeps whatever
    /// it was built with until it is recompiled.
    #[serde(default = "default_backend")]
    pub backend: String,
    /// Keyed by disc ID. Games with nothing overridden are dropped on save.
    #[serde(default)]
    pub games: BTreeMap<String, Overrides>,
}

impl Default for Store {
    fn default() -> Self {
        Self {
            defaults: Defaults::default(),
            backend: default_backend(),
            games: BTreeMap::new(),
        }
    }
}

impl Store {
    pub fn overrides(&self, disc_id: &str) -> Overrides {
        self.games.get(disc_id).cloned().unwrap_or_default()
    }

    /// The settings a given game will actually run with.
    pub fn resolve(&self, disc_id: &str) -> Resolved {
        let game = self.overrides(disc_id);
        let default = &self.defaults;
        let mut controllers: [String; 4] = Default::default();
        for (port, slot) in controllers.iter_mut().enumerate() {
            *slot = game.controllers[port]
                .clone()
                .unwrap_or_else(|| default.controllers[port].clone());
        }
        Resolved {
            resolution: game.resolution.unwrap_or_else(|| default.resolution.clone()),
            fullscreen: game.fullscreen.unwrap_or(default.fullscreen),
            show_fps: game.show_fps.unwrap_or(default.show_fps),
            graphics: game.graphics.unwrap_or_else(|| default.graphics.clone()),
            audio: game.audio.unwrap_or_else(|| default.audio.clone()),
            controllers,
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct Resolved {
    pub resolution: String,
    pub fullscreen: bool,
    pub show_fps: bool,
    pub graphics: String,
    pub audio: String,
    pub controllers: [String; 4],
}

impl Resolved {
    /// The pipe a port is driven from, if any — the name after `Pipe/<index>/`,
    /// which is the FIFO's filename and what the driver has to be told to
    /// create. Only the first is returned: one driver, one controller.
    pub fn pipe_name(&self) -> Option<String> {
        self.controllers
            .iter()
            .find_map(|device| device.rsplit_once('/').filter(|_| device.starts_with("Pipe/")))
            .map(|(_, name)| name.to_string())
    }
}

pub fn store_path() -> PathBuf {
    library::support_dir().join("settings.json")
}

pub fn load() -> Store {
    std::fs::read_to_string(store_path())
        .ok()
        .and_then(|text| serde_json::from_str(&text).ok())
        .unwrap_or_default()
}

pub fn save(store: &Store) -> std::io::Result<()> {
    let mut store = store.clone();
    store.games.retain(|_, game| !game.is_empty());
    let path = store_path();
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)?;
    }
    let text = serde_json::to_string_pretty(&store).unwrap_or_else(|_| "{}".into());
    std::fs::write(path, text + "\n")
}

/// The gamepads the runtime can see, via `recompgc list-controllers`.
///
/// A ModernGekko build without the `--list-controllers` patch exits non-zero
/// here, which is reported the same way as having nothing plugged in: the
/// picker is simply empty.
pub fn list_controllers(recompgc: &Path) -> Vec<Controller> {
    let output = std::process::Command::new(recompgc)
        .arg("list-controllers")
        .stdin(std::process::Stdio::null())
        .output();
    let Ok(output) = output else {
        return Vec::new();
    };
    if !output.status.success() {
        return Vec::new();
    }
    String::from_utf8_lossy(&output.stdout)
        .lines()
        .filter_map(|line| line.split_once('\t'))
        .filter(|(device, _)| !device.is_empty())
        .map(|(device, label)| Controller {
            device: device.to_string(),
            label: label.to_string(),
        })
        .collect()
}

// --- writing what the runtime reads ------------------------------------------

/// Marks the files this module owns. A pad profile without it was written by
/// hand or by ModernGekko itself, and is never removed on our account.
const MARKER: &str = "# Written by DolBundler.";

/// Render `resolved` into the runtime's `config.ini` and controller profile.
///
/// `platform` is the library's platform string ("GameCube (Gekko)" or
/// "Wii (Broadway)"); it decides which of the two profile files a controller
/// selection lands in. Returns the notes worth showing in the console.
pub fn apply(resolved: &Resolved, platform: &str) -> Result<Vec<String>, String> {
    let user_dir = library::user_dir();
    std::fs::create_dir_all(&user_dir)
        .map_err(|err| format!("could not create {}: {err}", user_dir.display()))?;

    let config = user_dir.join("config.ini");
    let netplay = read_netplay(&config);
    std::fs::write(&config, render_config(resolved, &netplay))
        .map_err(|err| format!("could not write {}: {err}", config.display()))?;

    let mut notes = Vec::new();
    let wii = platform.contains("Wii");
    let profile = user_dir
        .join("Config")
        .join(if wii { "WiimoteNew.ini" } else { "GCPadNew.ini" });
    let assigned = resolved.controllers.iter().filter(|d| !d.is_empty()).count();
    // Nothing selected and no profile of ours to update: leave whatever is
    // there, so a build that has never been near this window keeps Dolphin's
    // own defaults and a hand-written profile is never clobbered.
    if assigned == 0 && !ours(&profile) {
        return Ok(notes);
    }
    if let Some(parent) = profile.parent() {
        std::fs::create_dir_all(parent)
            .map_err(|err| format!("could not create {}: {err}", parent.display()))?;
    }
    std::fs::write(&profile, render_profile(&resolved.controllers, wii))
        .map_err(|err| format!("could not write {}: {err}", profile.display()))?;
    notes.push(match assigned {
        0 => "No controller assigned; the ports are left unbound.".to_string(),
        1 => format!("1 controller mapped in {}", profile.display()),
        count => format!("{count} controllers mapped in {}", profile.display()),
    });
    Ok(notes)
}

/// Push the backends a game resolved to into the `game.conf` of the `.app`
/// DolBundler generated for it, so a bundle launched from Finder or the Dock
/// runs with the same ones as Play does. Only the two keys are touched, and
/// only in a file that already has them; anything else is left alone.
///
/// The rest of the settings need no sync: the bundle's launcher reads the same
/// `config.ini` this module writes.
pub fn sync_bundle(app: &str, resolved: &Resolved) -> Result<(), String> {
    if app.is_empty() {
        return Ok(());
    }
    let conf = PathBuf::from(app).join("Contents/Resources/game.conf");
    let Ok(text) = std::fs::read_to_string(&conf) else {
        return Ok(());
    };
    let mut changed = false;
    let updated: Vec<String> = text
        .lines()
        .map(|line| {
            let replacement = match line.split_once('=').map(|(key, _)| key.trim()) {
                Some("GRAPHICS_BACKEND") => Some(&resolved.graphics),
                Some("AUDIO_BACKEND") => Some(&resolved.audio),
                _ => None,
            };
            match replacement {
                Some(value) => {
                    let line_out = format!(
                        "{}={}",
                        line.split_once('=').map(|(key, _)| key).unwrap_or(line),
                        shell_quote(value)
                    );
                    changed |= line_out != line;
                    line_out
                }
                None => line.to_string(),
            }
        })
        .collect();
    if !changed {
        return Ok(());
    }
    std::fs::write(&conf, updated.join("\n") + "\n")
        .map_err(|err| format!("could not update {}: {err}", conf.display()))
}

/// Single-quoted for the bundle's `source game.conf`, the same way
/// `make_game_app.py` writes it.
fn shell_quote(value: &str) -> String {
    format!("'{}'", value.replace('\'', "'\\''"))
}

fn ours(path: &Path) -> bool {
    std::fs::read_to_string(path)
        .map(|text| text.starts_with(MARKER))
        .unwrap_or(false)
}

/// The four `[Netplay]` values, which this module does not own. ModernGekko
/// validates them on every load and refuses to boot if they are missing or
/// out of range, so a rewrite has to carry forward what is already there.
struct Netplay {
    nickname: String,
    address: String,
    port: String,
    buffer: String,
}

impl Default for Netplay {
    fn default() -> Self {
        Self {
            nickname: "Player".into(),
            address: "127.0.0.1".into(),
            port: "2626".into(),
            buffer: "auto".into(),
        }
    }
}

fn read_netplay(config: &Path) -> Netplay {
    let mut netplay = Netplay::default();
    let Ok(text) = std::fs::read_to_string(config) else {
        return netplay;
    };
    for line in text.lines() {
        let line = line.trim();
        if line.starts_with('#') || line.starts_with(';') || line.starts_with('[') {
            continue;
        }
        let Some((key, value)) = line.split_once('=') else {
            continue;
        };
        let value = value.trim().to_string();
        if value.is_empty() {
            continue;
        }
        match key.trim().to_ascii_lowercase().as_str() {
            "nickname" => netplay.nickname = value,
            "address" => netplay.address = value,
            "port" => netplay.port = value,
            "buffer" => netplay.buffer = value,
            _ => {}
        }
    }
    netplay
}

fn render_config(resolved: &Resolved, netplay: &Netplay) -> String {
    let mut out = String::new();
    out.push_str(MARKER);
    out.push_str(
        "\n# Rewritten from DolBundler's settings.json every time a game starts,\n\
         # because the settings are per-game and this file is not. Edit them in\n\
         # the window instead; changes made here are replaced on the next launch.\n\
         [Video]\n",
    );
    let _ = writeln!(out, "resolution={}", resolved.resolution);
    // The key only accepts Vulkan or OpenGL. What actually runs is whatever
    // `recompgc --graphics` passes, which is why Metal can be the real answer
    // while this says Vulkan.
    let _ = writeln!(
        out,
        "backend={}",
        if resolved.graphics == "OGL" { "OGL" } else { "Vulkan" }
    );
    let _ = writeln!(out, "fullscreen={}", resolved.fullscreen);
    let _ = writeln!(out, "show_fps_in_title={}", resolved.show_fps);
    out.push_str("[Input]\n");
    for (port, device) in resolved.controllers.iter().enumerate() {
        if !device.is_empty() {
            let _ = writeln!(out, "controller{}={device}", port + 1);
        }
    }
    out.push_str("[Netplay]\n");
    let _ = writeln!(out, "nickname={}", netplay.nickname);
    let _ = writeln!(out, "address={}", netplay.address);
    let _ = writeln!(out, "port={}", netplay.port);
    let _ = writeln!(out, "buffer={}", netplay.buffer);
    out
}

/// One `[GCPad<n>]` or `[Wiimote<n>]` section per port, with a `Device` line
/// only where a port has a controller. The mappings are ModernGekko's own, from
/// `GenerateControllerConfig()` in `tools/frontend_config.cpp`; kept in step
/// with it by hand, and duplicated here because that generator is compiled to
/// one platform at a time while DolBundler has to serve both from one build.
fn render_profile(controllers: &[String; 4], wii: bool) -> String {
    let mut out = String::from(MARKER);
    out.push_str("\n# Generated from the controller ports set in DolBundler.\n");
    for (port, device) in controllers.iter().enumerate() {
        let _ = writeln!(
            out,
            "[{}{}]",
            if wii { "Wiimote" } else { "GCPad" },
            port + 1
        );
        if device.is_empty() {
            continue;
        }
        let _ = writeln!(out, "Device = {device}");
        // A pipe device's inputs are named nothing like a gamepad's, so the
        // mapping is chosen by what the port is driven from, not just by the
        // game's platform.
        let pipe = device.starts_with("Pipe/");
        out.push_str(match (pipe, wii) {
            (false, false) => GCPAD_MAPPING,
            (false, true) => WIIMOTE_MAPPING,
            (true, false) => PIPE_GCPAD_MAPPING,
            (true, true) => PIPE_WIIMOTE_MAPPING,
        });
    }
    if wii {
        out.push_str("[BalanceBoard]\n");
    }
    out
}

const GCPAD_MAPPING: &str = "\
Buttons/A = `Button A`
Buttons/B = `Button B`
Buttons/X = `Button X`
Buttons/Y = `Button Y`
Buttons/Z = `Shoulder R`
Buttons/Start = Start
Main Stick/Up = `Left Y+`
Main Stick/Down = `Left Y-`
Main Stick/Left = `Left X-`
Main Stick/Right = `Left X+`
Main Stick/Calibration = 100.00
C-Stick/Up = `Right Y+`
C-Stick/Down = `Right Y-`
C-Stick/Left = `Right X-`
C-Stick/Right = `Right X+`
C-Stick/Calibration = 100.00
Triggers/L = `Trigger L`
Triggers/R = `Trigger R`
Triggers/L-Analog = `Trigger L`
Triggers/R-Analog = `Trigger R`
D-Pad/Up = `Pad N`
D-Pad/Down = `Pad S`
D-Pad/Left = `Pad W`
D-Pad/Right = `Pad E`
Rumble/Motor = `Motor L` | `Motor R`
";

/// Dolphin's pipe device exposes `Button <TOKEN>` for each of the twelve tokens
/// its protocol accepts and a split `Axis <NAME> +`/`-` pair per stick axis and
/// trigger. The tokens are a GameCube pad's, so this is a straight one-to-one
/// map — the only one in this file that loses nothing in translation.
const PIPE_GCPAD_MAPPING: &str = "\
Buttons/A = `Button A`
Buttons/B = `Button B`
Buttons/X = `Button X`
Buttons/Y = `Button Y`
Buttons/Z = `Button Z`
Buttons/Start = `Button START`
Main Stick/Up = `Axis MAIN Y +`
Main Stick/Down = `Axis MAIN Y -`
Main Stick/Left = `Axis MAIN X -`
Main Stick/Right = `Axis MAIN X +`
Main Stick/Calibration = 100.00
C-Stick/Up = `Axis C Y +`
C-Stick/Down = `Axis C Y -`
C-Stick/Left = `Axis C X -`
C-Stick/Right = `Axis C X +`
C-Stick/Calibration = 100.00
Triggers/L = `Button L`
Triggers/R = `Button R`
Triggers/L-Analog = `Axis L +`
Triggers/R-Analog = `Axis R +`
D-Pad/Up = `Button D_UP`
D-Pad/Down = `Button D_DOWN`
D-Pad/Left = `Button D_LEFT`
D-Pad/Right = `Button D_RIGHT`
";

/// A GameCube pad driving a Wii Remote, which is a genuine translation rather
/// than a relabelling: the pipe protocol has no pointer, no motion, and no Home,
/// so the C stick stands in for IR and the triggers for shakes. Playable, not
/// faithful — a Wii game that wants real pointing wants a real Wii Remote.
const PIPE_WIIMOTE_MAPPING: &str = "\
Buttons/A = `Button A`
Buttons/B = `Button B`
Buttons/1 = `Button X`
Buttons/2 = `Button Y`
Buttons/- = `Button Z`
Buttons/+ = `Button START`
D-Pad/Up = `Button D_UP` | `Axis MAIN Y +`
D-Pad/Down = `Button D_DOWN` | `Axis MAIN Y -`
D-Pad/Left = `Button D_LEFT` | `Axis MAIN X -`
D-Pad/Right = `Button D_RIGHT` | `Axis MAIN X +`
IR/Up = `Axis C Y +`
IR/Down = `Axis C Y -`
IR/Left = `Axis C X -`
IR/Right = `Axis C X +`
Shake/X = `Axis L +`
Shake/Y = `Axis R +`
Shake/Z = `Axis L +`
Extension = None
Options/Sideways Wiimote = True
";

const WIIMOTE_MAPPING: &str = "\
Buttons/A = `Shoulder L`
Buttons/B = `Shoulder R`
Buttons/1 = `Button W`
Buttons/2 = `Button S`
Buttons/- = Back
Buttons/+ = Start
Buttons/Home = Guide
D-Pad/Up = `Pad N` | `Left Y+`
D-Pad/Down = `Pad S` | `Left Y-`
D-Pad/Left = `Pad W` | `Left X-`
D-Pad/Right = `Pad E` | `Left X+`
IR/Up = `Cursor Y-`
IR/Down = `Cursor Y+`
IR/Left = `Cursor X-`
IR/Right = `Cursor X+`
Shake/X = `Trigger L`
Shake/Y = `Trigger R`
Shake/Z = `Trigger L`
IRPassthrough/Object 1 X = `IR Object 1 X`
IRPassthrough/Object 1 Y = `IR Object 1 Y`
IRPassthrough/Object 1 Size = `IR Object 1 Size`
IRPassthrough/Object 2 X = `IR Object 2 X`
IRPassthrough/Object 2 Y = `IR Object 2 Y`
IRPassthrough/Object 2 Size = `IR Object 2 Size`
IRPassthrough/Object 3 X = `IR Object 3 X`
IRPassthrough/Object 3 Y = `IR Object 3 Y`
IRPassthrough/Object 3 Size = `IR Object 3 Size`
IRPassthrough/Object 4 X = `IR Object 4 X`
IRPassthrough/Object 4 Y = `IR Object 4 Y`
IRPassthrough/Object 4 Size = `IR Object 4 Size`
IMUAccelerometer/Up = `Accel Up`
IMUAccelerometer/Down = `Accel Down`
IMUAccelerometer/Left = `Accel Left`
IMUAccelerometer/Right = `Accel Right`
IMUAccelerometer/Forward = `Accel Forward`
IMUAccelerometer/Backward = `Accel Backward`
IMUGyroscope/Pitch Up = `Gyro Pitch Up`
IMUGyroscope/Pitch Down = `Gyro Pitch Down`
IMUGyroscope/Roll Left = `Gyro Roll Left`
IMUGyroscope/Roll Right = `Gyro Roll Right`
IMUGyroscope/Yaw Left = `Gyro Yaw Left`
IMUGyroscope/Yaw Right = `Gyro Yaw Right`
Rumble/Motor = Motor
Extension = None
Options/Sideways Wiimote = True
";

#[cfg(test)]
mod tests {
    use super::*;

    fn resolved() -> Resolved {
        Resolved {
            resolution: "1280x720".into(),
            fullscreen: true,
            show_fps: false,
            graphics: "Metal".into(),
            audio: String::new(),
            controllers: [
                "SDL/0/Xbox Wireless Controller".into(),
                String::new(),
                String::new(),
                String::new(),
            ],
        }
    }

    #[test]
    fn overrides_fall_back_to_the_defaults() {
        let mut store = Store::default();
        store.defaults.resolution = "1920x1080".into();
        store.defaults.controllers[0] = "SDL/0/Pad".into();
        store.games.insert(
            "GMPE01".into(),
            Overrides {
                resolution: Some("640x528".into()),
                ..Default::default()
            },
        );

        let game = store.resolve("GMPE01");
        assert_eq!(game.resolution, "640x528");
        assert_eq!(game.controllers[0], "SDL/0/Pad");
        assert_eq!(store.resolve("GGVE78").resolution, "1920x1080");
    }

    #[test]
    fn config_keeps_the_netplay_block_and_a_loadable_backend() {
        let netplay = Netplay {
            nickname: "tony".into(),
            address: "10.0.0.4".into(),
            port: "2727".into(),
            buffer: "4".into(),
        };
        let text = render_config(&resolved(), &netplay);
        assert!(text.contains("resolution=1280x720\n"));
        assert!(text.contains("fullscreen=true\n"));
        assert!(text.contains("show_fps_in_title=false\n"));
        // Metal is not a value config.ini accepts; it travels on the command
        // line, and this key has to stay loadable.
        assert!(text.contains("backend=Vulkan\n"));
        assert!(text.contains("controller1=SDL/0/Xbox Wireless Controller\n"));
        assert!(!text.contains("controller2="));
        assert!(text.contains("nickname=tony\n"));
        assert!(text.contains("buffer=4\n"));
    }

    /// Exercises the real write path, user directory and all. `apply` resolves
    /// that directory from the environment, so this test owns `XDG_DATA_HOME`
    /// and has to be the only one that touches it.
    #[test]
    fn apply_writes_both_files_and_respects_a_foreign_profile() {
        let root = std::env::temp_dir().join(format!(
            "dolbundler-settings-test-{}",
            std::process::id()
        ));
        let _ = std::fs::remove_dir_all(&root);
        std::env::set_var("XDG_DATA_HOME", &root);
        let user_dir = root.join("moderngekko");
        let profile = user_dir.join("Config/GCPadNew.ini");

        // Nothing selected, no profile of ours: config.ini only.
        let mut bare = resolved();
        bare.controllers = Default::default();
        assert_eq!(apply(&bare, "GameCube (Gekko)").unwrap().len(), 0);
        assert!(user_dir.join("config.ini").is_file());
        assert!(!profile.exists());

        // A profile someone else wrote stays that way.
        std::fs::create_dir_all(profile.parent().unwrap()).unwrap();
        std::fs::write(&profile, "[GCPad1]\nDevice = mine\n").unwrap();
        apply(&bare, "GameCube (Gekko)").unwrap();
        assert_eq!(
            std::fs::read_to_string(&profile).unwrap(),
            "[GCPad1]\nDevice = mine\n"
        );

        // Assigning a pad takes the file over, and the platform picks the name.
        let notes = apply(&resolved(), "GameCube (Gekko)").unwrap();
        assert_eq!(notes.len(), 1);
        assert!(std::fs::read_to_string(&profile).unwrap().starts_with(MARKER));
        apply(&resolved(), "Wii (Broadway)").unwrap();
        assert!(user_dir.join("Config/WiimoteNew.ini").is_file());

        // Now that a profile of ours exists, clearing every port empties it
        // rather than leaving the last mapping in place.
        apply(&bare, "GameCube (Gekko)").unwrap();
        let emptied = std::fs::read_to_string(&profile).unwrap();
        assert!(!emptied.contains("Device ="));

        std::env::remove_var("XDG_DATA_HOME");
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn a_pipe_port_gets_pipe_input_names() {
        let mut piped = resolved();
        piped.controllers[0] = "Pipe/0/gcc1".into();
        piped.controllers[1] = "SDL/0/Some Pad".into();
        let text = render_profile(&piped.controllers, false);
        assert!(text.contains("[GCPad1]\nDevice = Pipe/0/gcc1\n"));
        // The pipe protocol's own token names, not a gamepad's.
        assert!(text.contains("Buttons/Start = `Button START`\n"));
        assert!(text.contains("Main Stick/Up = `Axis MAIN Y +`\n"));
        assert!(text.contains("Triggers/L-Analog = `Axis L +`\n"));
        // Port 2 is an SDL pad and keeps the gamepad mapping in the same file.
        assert!(text.contains("[GCPad2]\nDevice = SDL/0/Some Pad\n"));
        assert!(text.contains("Main Stick/Up = `Left Y+`\n"));
    }

    #[test]
    fn the_pipe_name_is_the_fifos_filename() {
        assert_eq!(resolved().pipe_name(), None);
        let mut piped = resolved();
        piped.controllers[1] = "Pipe/0/gcc1".into();
        assert_eq!(piped.pipe_name().as_deref(), Some("gcc1"));
    }

    #[test]
    fn bundle_conf_quoting_matches_the_generator() {
        assert_eq!(shell_quote("Metal"), "'Metal'");
        assert_eq!(shell_quote(""), "''");
        assert_eq!(shell_quote("it's"), "'it'\\''s'");
    }

    #[test]
    fn a_port_without_a_controller_gets_a_section_and_no_device() {
        let text = render_profile(&resolved().controllers, false);
        assert!(text.starts_with(MARKER));
        assert!(text.contains("[GCPad1]\nDevice = SDL/0/Xbox Wireless Controller\n"));
        assert!(text.contains("Buttons/Z = `Shoulder R`\n"));
        assert!(text.contains("[GCPad2]\n[GCPad3]\n[GCPad4]\n"));
        assert!(!text.contains("Wiimote"));

        let wii = render_profile(&resolved().controllers, true);
        assert!(wii.contains("[Wiimote1]\nDevice = SDL/0/Xbox Wireless Controller\n"));
        assert!(wii.trim_end().ends_with("[BalanceBoard]"));
    }
}
