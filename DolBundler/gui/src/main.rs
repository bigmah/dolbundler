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
        pipeline::run(recompgc.peek().clone(), image, tx);

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
                            on_play: move |target: Game| {
                                // With a bundle, launch it so the game gets its
                                // own Dock icon; without one, run the same
                                // command the bundle would have run.
                                let launched = if target.has_app {
                                    std::process::Command::new("open")
                                        .arg(&target.app)
                                        .spawn()
                                        .map(|_| ())
                                } else {
                                    let tool = recompgc.peek().clone();
                                    std::process::Command::new(tool)
                                        .args([
                                            "play",
                                            "--disc-id", &target.disc_id,
                                            "--game-root", &target.game_root,
                                            "--module", &target.module,
                                            "--title", &target.name,
                                        ])
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
                            },
                            on_make_app: move |target: Game| {
                                if making_app.peek().is_some() {
                                    return;
                                }
                                making_app.set(Some(target.disc_id.clone()));
                                let tool = recompgc.peek().clone();
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
    on_play: EventHandler<Game>,
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
                div { class: "card-meta", "{game.disc_id} · {game.platform}" }
                if !game.ready {
                    div { class: "warn", "Recompiled module or extracted disc is missing. Add the disc image again." }
                }
            }
            div { class: "card-actions",
                button {
                    class: "play",
                    disabled: !game.ready || busy,
                    onclick: {
                        let game = game.clone();
                        move |_| on_play.call(game.clone())
                    },
                    "Play"
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
