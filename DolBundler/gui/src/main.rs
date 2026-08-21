// SPDX-License-Identifier: GPL-3.0-or-later
//! DolBundler - a window over the DolRecomp / ModernGekko pipeline.
//!
//! Pick or drop a GameCube or Wii disc image; the app extracts it, statically
//! recompiles its PowerPC code to native machine code, and adds it to the
//! library. Building a per-game .app is opt-in, per game. All of the actual work
//! happens in `recompgc`, which lives beside this binary in Contents/Resources.

#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod library;
mod pipeline;
mod settings;

use dioxus::desktop::tao::event::Event;
use dioxus::desktop::{Config, LogicalSize, WindowBuilder};
use dioxus::html::HasFileData;
use dioxus::prelude::*;
use futures_util::StreamExt;
use library::Game;
use pipeline::Msg;
use futures_channel::mpsc::{UnboundedReceiver, UnboundedSender};
use std::path::{Path, PathBuf};
use std::sync::{Mutex, OnceLock};

const STYLE: &str = include_str!("../assets/style.css");
const MAX_LOG_LINES: usize = 4000;
const DISC_EXTENSIONS: [&str; 6] = ["iso", "gcm", "wbfs", "rvz", "ciso", "gcz"];

/// Discs handed to us by Finder. macOS delivers those as `application:openURLs:`
/// rather than as arguments, so they arrive on the tao event loop before the
/// component tree exists and have to be queued.
static OPENED_TX: OnceLock<UnboundedSender<PathBuf>> = OnceLock::new();
static OPENED_RX: Mutex<Option<UnboundedReceiver<PathBuf>>> = Mutex::new(None);

fn main() {
    let (tx, rx) = futures_channel::mpsc::unbounded::<PathBuf>();
    let _ = OPENED_TX.set(tx);
    *OPENED_RX.lock().unwrap() = Some(rx);

    // `DolBundler game.iso` from a shell starts a job too.
    for argument in std::env::args().skip(1) {
        let path = PathBuf::from(argument);
        if path.is_file() {
            queue_opened(path);
        }
    }

    let window = WindowBuilder::new()
        .with_title("DolBundler")
        .with_inner_size(LogicalSize::new(1140.0, 760.0))
        .with_min_inner_size(LogicalSize::new(900.0, 560.0));

    let config = Config::new()
        .with_window(window)
        .with_background_color((0x14, 0x14, 0x17, 0xff))
        .with_disable_context_menu(true)
        .with_custom_event_handler(|event, _| {
            if let Event::Opened { urls } = event {
                for url in urls {
                    if let Ok(path) = url.to_file_path() {
                        queue_opened(path);
                    }
                }
            }
        });

    dioxus::LaunchBuilder::desktop().with_cfg(config).launch(app);
}

fn queue_opened(path: PathBuf) {
    if let Some(tx) = OPENED_TX.get() {
        let _ = tx.unbounded_send(path);
    }
}

/// Absolute path to Contents/Resources, where `recompgc` and the toolchain
/// configuration live. Falls back to the source tree for `cargo run`.
fn resources_dir() -> Option<PathBuf> {
    let bundled = std::env::current_exe()
        .ok()
        .and_then(|exe| Some(exe.parent()?.parent()?.join("Resources")));
    let dev = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .map(|root| root.join("DolBundler.app/Contents/Resources"));

    [bundled, dev]
        .into_iter()
        .flatten()
        .find(|dir| dir.join("recompgc").is_file() && dir.join("toolchain.conf").is_file())
}

fn is_disc_image(path: &Path) -> bool {
    path.extension()
        .and_then(|ext| ext.to_str())
        .map(|ext| DISC_EXTENSIONS.contains(&ext.to_ascii_lowercase().as_str()))
        .unwrap_or(false)
}

#[derive(Clone, PartialEq)]
struct Job {
    image: String,
    step: u32,
    total: u32,
    title: String,
    running: bool,
    error: Option<String>,
}

/// What the settings panel is open on. The panel is modal, so at most one.
#[derive(Clone, PartialEq)]
enum Editing {
    Global,
    Game(Game),
}

/// The settings form. Every field is a string so one panel can serve both the
/// global defaults and a single game's overrides; in the latter, `INHERIT`
/// stands for "whatever the defaults say".
#[derive(Clone, PartialEq)]
struct Draft {
    resolution: String,
    fullscreen: String,
    show_fps: String,
    graphics: String,
    audio: String,
    controllers: [String; 4],
}

impl Draft {
    fn from_defaults(defaults: &settings::Defaults) -> Self {
        Self {
            resolution: defaults.resolution.clone(),
            fullscreen: defaults.fullscreen.to_string(),
            show_fps: defaults.show_fps.to_string(),
            graphics: defaults.graphics.clone(),
            audio: defaults.audio.clone(),
            controllers: defaults.controllers.clone(),
        }
    }

    fn from_overrides(game: &settings::Overrides) -> Self {
        let inherit = || settings::INHERIT.to_string();
        Self {
            resolution: game.resolution.clone().unwrap_or_else(inherit),
            fullscreen: game.fullscreen.map(|on| on.to_string()).unwrap_or_else(inherit),
            show_fps: game.show_fps.map(|on| on.to_string()).unwrap_or_else(inherit),
            graphics: game.graphics.clone().unwrap_or_else(inherit),
            audio: game.audio.clone().unwrap_or_else(inherit),
            controllers: std::array::from_fn(|port| {
                game.controllers[port].clone().unwrap_or_else(inherit)
            }),
        }
    }

    fn into_defaults(self) -> settings::Defaults {
        settings::Defaults {
            resolution: self.resolution,
            fullscreen: self.fullscreen == "true",
            show_fps: self.show_fps == "true",
            graphics: self.graphics,
            audio: self.audio,
            controllers: self.controllers,
        }
    }

    fn into_overrides(self) -> settings::Overrides {
        let kept = |value: String| (value != settings::INHERIT).then_some(value);
        settings::Overrides {
            resolution: kept(self.resolution),
            fullscreen: kept(self.fullscreen).map(|value| value == "true"),
            show_fps: kept(self.show_fps).map(|value| value == "true"),
            graphics: kept(self.graphics),
            audio: kept(self.audio),
            controllers: self.controllers.map(kept),
        }
    }
}

#[derive(Clone, PartialEq)]
struct Line {
    text: String,
    kind: &'static str,
}

impl Line {
    fn from_output(text: String) -> Self {
        let kind = if text.starts_with("==>") {
            "head"
        } else if text.starts_with("error:") || text.contains("FAILED") {
            "bad"
        } else {
            ""
        };
        Self { text, kind }
    }
}

fn app() -> Element {
    let resources = use_hook(resources_dir);
    // Held in a signal so the closures below stay Copy and can be shared
    // between the button, the file dialog, and the drop handler.
    let recompgc = use_signal(|| {
        resources
            .clone()
            .map(|dir| dir.join("recompgc"))
            .unwrap_or_default()
    });
    let mut games = use_signal(library::load);
    let mut log = use_signal(Vec::<Line>::new);
    let mut job = use_signal(|| None::<Job>);
    // Disc ID of the game currently having a bundle built, so Create App
    // cannot be double-clicked into two concurrent builders.
    let mut making_app = use_signal(|| None::<String>);
    let mut dragging = use_signal(|| false);
    let mut store = use_signal(settings::load);
    // The gamepads the runtime reported, refreshed whenever the panel opens so
    // a pad plugged in while the window was already up shows up in the picker.
    let mut pads = use_signal(Vec::<settings::Controller>::new);
    let mut editing = use_signal(|| None::<Editing>);
    // Disc ID of a game waiting on its controller driver, so Play cannot be
    // pressed twice into two driver starts.
    let mut launching = use_signal(|| None::<String>);

    // Keep the console pinned to the newest line.
    use_effect(move || {
        let _ = log.read().len();
        spawn(async move {
            let _ = document::eval(
                "const el = document.getElementById('console-body');
                 if (el) el.scrollTop = el.scrollHeight;",
            )
            .await;
        });
    });

    if resources.is_none() {
        return rsx! {
            style { dangerous_inner_html: STYLE }
            div { class: "setup",
                h2 { "DolBundler is not set up yet" }
                p { "The recompiler, the runtime, and the disc tools have not been built.
                     Run the build script once and then reopen this app." }
                code { "cd DolBundler && ./build.sh" }
            }
        };
    }

    // Actually starting a game, with whatever settings it resolved to. Split
    // out of the Play handler because a port driven by a pipe has to wait for
    // its driver, so the launch can happen a moment later and asynchronously.
    let mut launch = move |target: Game| {
        let resolved = store.read().resolve(&target.disc_id);
        // With a bundle, launch it so the game gets its own Dock icon; without
        // one, run the same command the bundle would have run.
        let launched = if target.has_app {
            std::process::Command::new("open")
                .arg(&target.app)
                .spawn()
                .map(|_| ())
        } else {
            let tool = recompgc.peek().clone();
            let mut args = vec![
                "play".to_string(),
                "--disc-id".into(), target.disc_id.clone(),
                "--game-root".into(), target.game_root.clone(),
                "--module".into(), target.module.clone(),
                "--title".into(), target.name.clone(),
                "--graphics".into(), resolved.graphics.clone(),
            ];
            if !resolved.audio.is_empty() {
                args.push("--audio".into());
                args.push(resolved.audio.clone());
            }
            std::process::Command::new(tool)
                .args(args)
                .stdin(std::process::Stdio::null())
                .spawn()
                .map(|_| ())
        };
        match launched {
            Ok(()) => log.write().push(Line {
                text: format!("Launched {}.", target.name),
                kind: "good",
            }),
            Err(err) => log.write().push(Line {
                text: format!("Could not launch {}: {err}", target.name),
                kind: "bad",
            }),
        }
    };

    // Enumerating gamepads spins up SDL in a child process, so it happens off
    // the UI thread the same way the pipeline does.
    let rescan = move || {
        let tool = recompgc.peek().clone();
        let (tx, mut rx) = futures_channel::mpsc::unbounded::<Vec<settings::Controller>>();
        std::thread::spawn(move || {
            let _ = tx.unbounded_send(settings::list_controllers(&tool));
        });
        spawn(async move {
            if let Some(found) = rx.next().await {
                pads.set(found);
            }
        });
    };

    let mut start = move |image: PathBuf| {
        if job.read().as_ref().is_some_and(|current| current.running) {
            return;
        }
        let name = image
            .file_name()
            .map(|value| value.to_string_lossy().into_owned())
            .unwrap_or_default();

        log.write().push(Line {
            text: format!("$ recompgc {}", image.display()),
            kind: "dim",
        });
        job.set(Some(Job {
            image: name,
            step: 0,
            total: 4,
            title: "Starting".into(),
            running: true,
            error: None,
        }));

        let (tx, mut rx) = futures_channel::mpsc::unbounded::<Msg>();
        let backend = store.peek().backend.clone();
        pipeline::run(recompgc.peek().clone(), image, backend, tx);

        spawn(async move {
            while let Some(message) = rx.next().await {
                match message {
                    Msg::Line(text) => {
                        if text.trim().is_empty() {
                            continue;
                        }
                        let mut lines = log.write();
                        lines.push(Line::from_output(text));
                        let overflow = lines.len().saturating_sub(MAX_LOG_LINES);
                        if overflow > 0 {
                            lines.drain(..overflow);
                        }
                    }
                    Msg::Step { index, total, title } => {
                        log.write().push(Line {
                            text: format!("==> {index}/{total}  {title}"),
                            kind: "head",
                        });
                        if let Some(current) = job.write().as_mut() {
                            current.step = index;
                            current.total = total;
                            current.title = title;
                        }
                    }
                    Msg::Info { key, value } => {
                        if key == "name" {
                            if let Some(current) = job.write().as_mut() {
                                current.image = value;
                            }
                        }
                    }
                    Msg::Failed(reason) => {
                        if let Some(current) = job.write().as_mut() {
                            current.error = Some(reason);
                        }
                    }
                    Msg::Finished(success) => {
                        if let Some(current) = job.write().as_mut() {
                            current.running = false;
                            if !success && current.error.is_none() {
                                current.error = Some("the pipeline exited early".into());
                            }
                            if success {
                                current.step = current.total;
                                current.title = "Done".into();
                            }
                        }
                        if success {
                            games.set(library::load());
                        }
                        break;
                    }
                }
            }
        });
    };

    // Discs opened through Finder or passed on the command line.
    use_future(move || async move {
        let queued = OPENED_RX.lock().ok().and_then(|mut slot| slot.take());
        let Some(mut queued) = queued else { return };
        while let Some(path) = queued.next().await {
            if is_disc_image(&path) {
                start(path);
            } else {
                log.write().push(Line {
                    text: format!("Ignored {}: not a disc image.", path.display()),
                    kind: "bad",
                });
            }
        }
    });

    let busy = job.read().as_ref().is_some_and(|current| current.running);

    let pick = move || {
        spawn(async move {
            let chosen = rfd::AsyncFileDialog::new()
                .set_title("Choose a GameCube or Wii disc image")
                .add_filter("Disc image", &DISC_EXTENSIONS)
                .pick_file()
                .await;
            if let Some(file) = chosen {
                start(file.path().to_path_buf());
            }
        });
    };

    rsx! {
        style { dangerous_inner_html: STYLE }
        div {
            class: "shell",
            ondragover: move |event| {
                event.prevent_default();
                if !dragging() { dragging.set(true); }
            },
            ondragleave: move |_| dragging.set(false),
            ondrop: move |event| {
                event.prevent_default();
                dragging.set(false);
                let images: Vec<PathBuf> = event
                    .files()
                    .iter()
                    .map(|file| file.path())
                    .filter(|path| is_disc_image(path))
                    .collect();
                match images.into_iter().next() {
                    Some(image) => start(image),
                    None => log.write().push(Line {
                        text: "That is not a disc image. Expected .iso, .gcm, .wbfs, .rvz, .ciso, or .gcz.".into(),
                        kind: "bad",
                    }),
                }
            },

            header { class: "header",
                div {
                    div { class: "wordmark", "DolBundler" }
                    div { class: "tagline", "GameCube and Wii discs, recompiled to native macOS apps" }
                }
                div { class: "spacer" }
                button {
                    title: "Defaults every game starts from",
                    onclick: move |_| {
                        rescan();
                        editing.set(Some(Editing::Global));
                    },
                    "Settings"
                }
                button {
                    class: "primary",
                    disabled: busy,
                    onclick: move |_| pick(),
                    if busy { "Working…" } else { "Add disc image…" }
                }
            }

            if let Some(current) = job.read().clone() {
                JobStrip { job: current }
            }

            div { class: "body",
                div { class: "library",
                    p { class: "section-label", "Library" }
                    if games.read().is_empty() {
                        div { class: "empty",
                            strong { "No games yet" }
                            "Drop a disc image anywhere in this window, or use Add disc image…"
                            br {}
                            "The first recompile of a game takes a few minutes. After that it is cached."
                        }
                    }
                    for game in games.read().iter().cloned() {
                        GameCard {
                            key: "{game.disc_id}",
                            game: game.clone(),
                            busy,
                            making_app: making_app.read().as_deref() == Some(game.disc_id.as_str()),
                            launching: launching.read().as_deref() == Some(game.disc_id.as_str()),
                            customised: !store.read().overrides(&game.disc_id).is_empty(),
                            on_settings: move |target: Game| {
                                rescan();
                                editing.set(Some(Editing::Game(target)));
                            },
                            on_play: move |target: Game| {
                                // config.ini and the controller profile are
                                // global to the runtime, so this game's
                                // settings are written into them here, one
                                // moment before it starts.
                                let resolved = store.read().resolve(&target.disc_id);
                                match settings::apply(&resolved, &target.platform) {
                                    Ok(notes) => for note in notes {
                                        log.write().push(Line { text: note, kind: "dim" });
                                    },
                                    Err(err) => log.write().push(Line {
                                        text: format!("Could not apply settings: {err}"),
                                        kind: "bad",
                                    }),
                                }
                                if let Err(err) = settings::sync_bundle(&target.app, &resolved) {
                                    log.write().push(Line { text: err, kind: "bad" });
                                }
                                // A pipe device is fed by a driver outside the
                                // runtime, and Dolphin only scans for pipes
                                // once at startup, so that driver has to be up
                                // first. recompgc does this for itself too;
                                // running it here is what puts its output — the
                                // "plug the controller in" case above all — in
                                // front of whoever pressed Play.
                                if let Some(pipe) = resolved.pipe_name() {
                                    launching.set(Some(target.disc_id.clone()));
                                    let (tx, mut rx) =
                                        futures_channel::mpsc::unbounded::<Msg>();
                                    pipeline::run_args(
                                        recompgc.peek().clone(),
                                        vec![
                                            "driver".into(),
                                            "--ensure".into(),
                                            "--pipe".into(), pipe,
                                        ],
                                        tx,
                                    );
                                    spawn(async move {
                                        let mut ready = false;
                                        while let Some(message) = rx.next().await {
                                            match message {
                                                Msg::Line(text) if !text.trim().is_empty() => {
                                                    log.write().push(Line::from_output(text));
                                                }
                                                Msg::Failed(reason) => log.write().push(Line {
                                                    text: reason,
                                                    kind: "bad",
                                                }),
                                                Msg::Finished(success) => {
                                                    ready = success;
                                                    break;
                                                }
                                                _ => {}
                                            }
                                        }
                                        launching.set(None);
                                        if ready {
                                            launch(target.clone());
                                        } else {
                                            log.write().push(Line {
                                                text: format!(
                                                    "{} was not started: its controller is not ready.",
                                                    target.name,
                                                ),
                                                kind: "bad",
                                            });
                                        }
                                    });
                                    return;
                                }
                                launch(target);
                            },
                            on_make_app: move |target: Game| {
                                if making_app.peek().is_some() {
                                    return;
                                }
                                making_app.set(Some(target.disc_id.clone()));
                                let tool = recompgc.peek().clone();
                                let resolved = store.read().resolve(&target.disc_id);
                                log.write().push(Line {
                                    text: format!("==> Building {}.app", target.name),
                                    kind: "head",
                                });
                                let (tx, mut rx) = futures_channel::mpsc::unbounded::<Msg>();
                                pipeline::run_args(
                                    tool,
                                    vec![
                                        "make-app".into(),
                                        "--porcelain".into(),
                                        "--disc-id".into(), target.disc_id.clone(),
                                        "--game-root".into(), target.game_root.clone(),
                                        "--module".into(), target.module.clone(),
                                        "--title".into(), target.name.clone(),
                                        "--platform".into(), target.platform.clone(),
                                        "--graphics".into(), resolved.graphics.clone(),
                                        "--audio".into(), resolved.audio.clone(),
                                    ],
                                    tx,
                                );
                                let name = target.name.clone();
                                spawn(async move {
                                    while let Some(message) = rx.next().await {
                                        match message {
                                            Msg::Line(text) if !text.trim().is_empty() => {
                                                log.write().push(Line::from_output(text));
                                            }
                                            Msg::Failed(reason) => log.write().push(Line {
                                                text: format!("Could not build {name}.app: {reason}"),
                                                kind: "bad",
                                            }),
                                            Msg::Finished(success) => {
                                                if success {
                                                    games.set(library::load());
                                                    log.write().push(Line {
                                                        text: format!("{name}.app is in Applications."),
                                                        kind: "good",
                                                    });
                                                }
                                                making_app.set(None);
                                                break;
                                            }
                                            _ => {}
                                        }
                                    }
                                });
                            },
                            on_log: move |target: Game| {
                                let mut lines = log.write();
                                lines.push(Line {
                                    text: format!("==> Runtime log for {}", target.name),
                                    kind: "head",
                                });
                                for text in library::tail_log(&target.disc_id, 200) {
                                    lines.push(Line::from_output(text));
                                }
                            },
                            on_reveal: move |target: Game| {
                                let target_path = if target.has_app {
                                    &target.app
                                } else {
                                    &target.game_root
                                };
                                let _ = std::process::Command::new("open")
                                    .arg("-R")
                                    .arg(target_path)
                                    .spawn();
                            },
                            on_forget: move |target: Game| {
                                let kept: Vec<Game> = games
                                    .read()
                                    .iter()
                                    .filter(|entry| entry.disc_id != target.disc_id)
                                    .cloned()
                                    .collect();
                                let _ = library::save(&kept);
                                games.set(library::load());
                                {
                                    let mut current = store.write();
                                    current.games.remove(&target.disc_id);
                                }
                                let _ = settings::save(&store.read());
                                log.write().push(Line {
                                    text: if target.has_app {
                                        format!(
                                            "Removed {} from the library. {}.app is still in Applications.",
                                            target.name, target.name
                                        )
                                    } else {
                                        format!(
                                            "Removed {} from the library. Nothing on disk was deleted.",
                                            target.name
                                        )
                                    },
                                    kind: "dim",
                                });
                            },
                        }
                    }
                }

                div { class: "console",
                    div { class: "console-head",
                        span { class: "section-label", style: "margin: 0;", "Console" }
                        div { class: "spacer" }
                        button {
                            disabled: log.read().is_empty(),
                            onclick: move |_| log.write().clear(),
                            "Clear"
                        }
                    }
                    div { class: "console-body", id: "console-body",
                        if log.read().is_empty() {
                            div { class: "console-empty",
                                "Output from the extractor, the recompiler, and the games shows up here."
                            }
                        }
                        for (index, line) in log.read().iter().enumerate() {
                            div { key: "{index}", class: "line {line.kind}", "{line.text}" }
                        }
                    }
                }
            }

            if let Some(target) = editing.read().clone() {
                SettingsPanel {
                    title: match &target {
                        Editing::Global => "Default settings".to_string(),
                        Editing::Game(game) => game.name.clone(),
                    },
                    subtitle: match &target {
                        Editing::Global =>
                            "What every game starts from. A game can override any of it."
                                .to_string(),
                        Editing::Game(game) => format!(
                            "{} · {} — only this game", game.disc_id, game.platform,
                        ),
                    },
                    per_game: matches!(target, Editing::Game(_)),
                    defaults: store.read().defaults.clone(),
                    backend: store.read().backend.clone(),
                    on_backend: move |value: String| {
                        store.write().backend = value;
                        if let Err(err) = settings::save(&store.read()) {
                            log.write().push(Line {
                                text: format!("Could not save settings: {err}"),
                                kind: "bad",
                            });
                        }
                    },
                    initial: match &target {
                        Editing::Global => Draft::from_defaults(&store.read().defaults),
                        Editing::Game(game) =>
                            Draft::from_overrides(&store.read().overrides(&game.disc_id)),
                    },
                    pads: pads.read().clone(),
                    on_rescan: move |_| rescan(),
                    on_close: move |_| editing.set(None),
                    on_save: move |draft: Draft| {
                        let saved = match &target {
                            Editing::Global => {
                                store.write().defaults = draft.into_defaults();
                                "Default settings saved.".to_string()
                            }
                            Editing::Game(game) => {
                                let overrides = draft.into_overrides();
                                let count = overrides.count();
                                {
                                    let mut current = store.write();
                                    if overrides.is_empty() {
                                        current.games.remove(&game.disc_id);
                                    } else {
                                        current.games
                                            .insert(game.disc_id.clone(), overrides);
                                    }
                                }
                                // A bundle keeps its own copy of the backends,
                                // so it is brought in line as soon as they
                                // change rather than at the next Play.
                                let resolved = store.read().resolve(&game.disc_id);
                                let _ = settings::sync_bundle(&game.app, &resolved);
                                match count {
                                    0 => format!("{} is back on the defaults.", game.name),
                                    1 => format!("{}: 1 setting overridden.", game.name),
                                    n => format!("{}: {n} settings overridden.", game.name),
                                }
                            }
                        };
                        match settings::save(&store.read()) {
                            Ok(()) => log.write().push(Line { text: saved, kind: "good" }),
                            Err(err) => log.write().push(Line {
                                text: format!("Could not save settings: {err}"),
                                kind: "bad",
                            }),
                        }
                        editing.set(None);
                    },
                }
            }

            if dragging() {
                div { class: "drop-veil", "Drop a disc image to recompile it" }
            }
        }
    }
}

#[component]
fn JobStrip(job: Job) -> Element {
    let failed = job.error.is_some();
    let done = !job.running && !failed;
    let percent = if job.total == 0 {
        0.0
    } else {
        (job.step as f32 / job.total as f32) * 100.0
    };
    let fill_class = if failed {
        "fill failed"
    } else if done {
        "fill done"
    } else {
        "fill"
    };

    rsx! {
        div { class: if failed { "job failed" } else { "job" },
            div { class: "job-row",
                if job.running {
                    div { class: "spinner" }
                }
                div { class: "job-title",
                    if let Some(reason) = job.error.clone() {
                        "Failed: {reason}"
                    } else if done {
                        "{job.image} is ready"
                    } else {
                        "{job.title}"
                    }
                }
                div { class: "spacer" }
                div { class: "job-sub",
                    if job.running { "{job.image} · step {job.step} of {job.total}" } else { "{job.image}" }
                }
            }
            div { class: "track",
                div { class: fill_class, style: "width: {percent}%;" }
            }
        }
    }
}

#[component]
fn GameCard(
    game: Game,
    busy: bool,
    making_app: bool,
    launching: bool,
    customised: bool,
    on_play: EventHandler<Game>,
    on_settings: EventHandler<Game>,
    on_make_app: EventHandler<Game>,
    on_log: EventHandler<Game>,
    on_reveal: EventHandler<Game>,
    on_forget: EventHandler<Game>,
) -> Element {
    rsx! {
        div { class: "card",
            if game.cover_uri.is_empty() {
                div { class: "cover blank", "{game.disc_id}" }
            } else {
                img { class: "cover", src: "{game.cover_uri}", alt: "{game.name}" }
            }
            div { class: "card-text",
                div { class: "card-name", "{game.name}" }
                div { class: "card-meta",
                    "{game.disc_id} · {game.platform}"
                    if customised {
                        span { class: "tag", "custom settings" }
                    }
                }
                if !game.ready {
                    div { class: "warn", "Recompiled module or extracted disc is missing. Add the disc image again." }
                }
            }
            div { class: "card-actions",
                button {
                    class: "play",
                    disabled: !game.ready || busy || launching,
                    onclick: {
                        let game = game.clone();
                        move |_| on_play.call(game.clone())
                    },
                    if launching { "Waiting…" } else { "Play" }
                }
                button {
                    title: "Resolution, backends, and controllers for this game",
                    onclick: {
                        let game = game.clone();
                        move |_| on_settings.call(game.clone())
                    },
                    "Settings"
                }
                if !game.has_app {
                    button {
                        disabled: !game.ready || busy || making_app,
                        title: "Build a double-clickable .app in ~/Applications",
                        onclick: {
                            let game = game.clone();
                            move |_| on_make_app.call(game.clone())
                        },
                        if making_app { "Building…" } else { "Create App" }
                    }
                }
                button {
                    onclick: {
                        let game = game.clone();
                        move |_| on_log.call(game.clone())
                    },
                    "Log"
                }
                button {
                    onclick: {
                        let game = game.clone();
                        move |_| on_reveal.call(game.clone())
                    },
                    "Reveal"
                }
                button {
                    onclick: {
                        let game = game.clone();
                        move |_| on_forget.call(game.clone())
                    },
                    "Remove"
                }
            }
        }
    }
}

/// One `<select>`, rendered from `(value, label)` pairs.
#[component]
fn Choice(value: String, options: Vec<(String, String)>, on_pick: EventHandler<String>) -> Element {
    rsx! {
        select {
            // Set on the element as well as on the options: an `option`'s
            // selected attribute only carries the initial choice, so resetting
            // the form from a button would otherwise leave the control showing
            // the old one.
            value: "{value}",
            onchange: move |event| on_pick.call(event.value()),
            for (candidate, label) in options.iter() {
                option {
                    key: "{candidate}",
                    value: "{candidate}",
                    selected: *candidate == value,
                    "{label}"
                }
            }
        }
    }
}

#[component]
fn SettingRow(label: String, hint: String, children: Element) -> Element {
    rsx! {
        div { class: "setting",
            div { class: "setting-label",
                div { "{label}" }
                if !hint.is_empty() {
                    div { class: "setting-hint", "{hint}" }
                }
            }
            div { class: "setting-control", {children} }
        }
    }
}

/// The settings form, modal over the window.
///
/// `per_game` decides whether every control carries a leading "Default (…)"
/// entry: the global panel edits concrete values, a game's panel edits which of
/// them it departs from.
#[component]
fn SettingsPanel(
    title: String,
    subtitle: String,
    per_game: bool,
    defaults: settings::Defaults,
    backend: String,
    initial: Draft,
    pads: Vec<settings::Controller>,
    on_save: EventHandler<Draft>,
    on_backend: EventHandler<String>,
    on_close: EventHandler<()>,
    on_rescan: EventHandler<()>,
) -> Element {
    let mut draft = use_signal(|| initial.clone());

    // "Default (1920x1080)" ahead of the real choices, in a game's panel only.
    let lead = |shown: String| -> Vec<(String, String)> {
        if per_game {
            vec![(settings::INHERIT.to_string(), format!("Default ({shown})"))]
        } else {
            Vec::new()
        }
    };
    let named = |table: &[(&str, &str)], value: &str| -> String {
        table
            .iter()
            .find(|(candidate, _)| *candidate == value)
            .map(|(_, label)| (*label).to_string())
            .unwrap_or_else(|| value.to_string())
    };
    let switch = |on: bool| if on { "On" } else { "Off" }.to_string();

    let resolutions: Vec<(String, String)> = lead(defaults.resolution.clone())
        .into_iter()
        .chain(
            settings::RESOLUTIONS
                .iter()
                .map(|value| (value.to_string(), value.to_string())),
        )
        .collect();
    let graphics: Vec<(String, String)> = lead(named(&settings::GRAPHICS, &defaults.graphics))
        .into_iter()
        .chain(
            settings::GRAPHICS
                .iter()
                .map(|(value, label)| (value.to_string(), label.to_string())),
        )
        .collect();
    let audio: Vec<(String, String)> = lead(named(&settings::AUDIO, &defaults.audio))
        .into_iter()
        .chain(
            settings::AUDIO
                .iter()
                .map(|(value, label)| (value.to_string(), label.to_string())),
        )
        .collect();
    let fullscreen: Vec<(String, String)> = lead(switch(defaults.fullscreen))
        .into_iter()
        .chain([("true".into(), "On".into()), ("false".into(), "Off".into())])
        .collect();
    let show_fps: Vec<(String, String)> = lead(switch(defaults.show_fps))
        .into_iter()
        .chain([("true".into(), "On".into()), ("false".into(), "Off".into())])
        .collect();

    // A pad that is saved but unplugged still has to be offered, or opening
    // the panel with it disconnected would quietly drop the assignment.
    let device_label = |device: &str| -> String {
        pads.iter()
            .find(|pad| pad.device == device)
            .map(|pad| pad.label.clone())
            .unwrap_or_else(|| format!("{device} (not connected)"))
    };
    let ports: Vec<Vec<(String, String)>> = (0..4)
        .map(|port| {
            let mut options = lead(match defaults.controllers[port].as_str() {
                "" => "None".to_string(),
                device => device_label(device),
            });
            options.push((String::new(), "None".to_string()));
            options.extend(
                pads.iter()
                    .map(|pad| (pad.device.clone(), pad.label.clone())),
            );
            let chosen = draft.read().controllers[port].clone();
            if !chosen.is_empty()
                && chosen != settings::INHERIT
                && !options.iter().any(|(value, _)| *value == chosen)
            {
                options.push((chosen.clone(), device_label(&chosen)));
            }
            options
        })
        .collect();

    rsx! {
        div { class: "modal-veil", onclick: move |_| on_close.call(()),
            div {
                class: "modal",
                onclick: move |event| event.stop_propagation(),

                div { class: "modal-head",
                    div { class: "modal-title", "{title}" }
                    div { class: "modal-sub", "{subtitle}" }
                }

                div { class: "modal-body",
                    if !per_game {
                        p { class: "section-label", "Recompiler" }
                        SettingRow {
                            label: "Recompile discs to",
                            hint: "Native code is faster to play. Bytecode builds in seconds instead of minutes and never generates machine code, which is what an iOS build needs. Applies to the next disc you add.",
                            Choice {
                                value: backend.clone(),
                                options: settings::BACKENDS
                                    .iter()
                                    .map(|(value, label)| (value.to_string(), label.to_string()))
                                    .collect::<Vec<(String, String)>>(),
                                on_pick: move |value| on_backend.call(value),
                            }
                        }
                    }
                    p { class: "section-label", "Video and audio" }
                    SettingRow {
                        label: "Internal resolution",
                        hint: "What the game renders at, not the window size.",
                        Choice {
                            value: draft.read().resolution.clone(),
                            options: resolutions,
                            on_pick: move |value| draft.write().resolution = value,
                        }
                    }
                    SettingRow {
                        label: "Graphics backend",
                        hint: "Metal is the native one on macOS.",
                        Choice {
                            value: draft.read().graphics.clone(),
                            options: graphics,
                            on_pick: move |value| draft.write().graphics = value,
                        }
                    }
                    SettingRow {
                        label: "Audio backend",
                        hint: "",
                        Choice {
                            value: draft.read().audio.clone(),
                            options: audio,
                            on_pick: move |value| draft.write().audio = value,
                        }
                    }
                    SettingRow {
                        label: "Fullscreen",
                        hint: "",
                        Choice {
                            value: draft.read().fullscreen.clone(),
                            options: fullscreen,
                            on_pick: move |value| draft.write().fullscreen = value,
                        }
                    }
                    SettingRow {
                        label: "Show FPS in the title bar",
                        hint: "",
                        Choice {
                            value: draft.read().show_fps.clone(),
                            options: show_fps,
                            on_pick: move |value| draft.write().show_fps = value,
                        }
                    }

                    div { class: "modal-section",
                        span { class: "section-label", style: "margin: 0;", "Controllers" }
                        div { class: "spacer" }
                        button { onclick: move |_| on_rescan.call(()), "Rescan" }
                    }
                    if pads.is_empty() {
                        div { class: "notice",
                            "No gamepads detected. Plug one in and press Rescan. If nothing
                             ever shows up, the ModernGekko build predates the
                             --list-controllers patch — re-run build.sh."
                        }
                    }
                    for (port, options) in ports.into_iter().enumerate() {
                        SettingRow {
                            key: "{port}",
                            label: "Port {port + 1}",
                            hint: if port == 0 { "The mapping is ModernGekko's standard one." } else { "" },
                            Choice {
                                value: draft.read().controllers[port].clone(),
                                options,
                                on_pick: move |value| draft.write().controllers[port] = value,
                            }
                        }
                    }
                }

                div { class: "modal-foot",
                    if per_game {
                        button {
                            onclick: move |_| draft.set(Draft::from_overrides(
                                &settings::Overrides::default(),
                            )),
                            "Use defaults for everything"
                        }
                    }
                    div { class: "spacer" }
                    button { onclick: move |_| on_close.call(()), "Cancel" }
                    button {
                        class: "primary",
                        onclick: move |_| on_save.call(draft.read().clone()),
                        "Save"
                    }
                }
            }
        }
    }
}
