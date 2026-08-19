// SPDX-License-Identifier: GPL-3.0-or-later
//! The library index that `recompgc` writes after each successful recompile.
//!
//! `make_game_app.py` owns the file; this module only reads it, plus removes
//! entries the user drops from the list. An entry is playable on its own — the
//! `app` field is empty unless the user opted into building a bundle.

use base64::Engine;
use serde::{Deserialize, Serialize};
use std::path::PathBuf;

#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
pub struct Game {
    #[serde(default)]
    pub disc_id: String,
    #[serde(default)]
    pub name: String,
    #[serde(default)]
    pub platform: String,
    #[serde(default)]
    pub game_root: String,
    #[serde(default)]
    pub module: String,
    #[serde(default)]
    pub app: String,
    #[serde(default)]
    pub cover: String,
    #[serde(default)]
    pub source_image: String,
    #[serde(default)]
    pub added: String,

    /// Cover art as a data URI. Filled in on load; never written back.
    #[serde(skip)]
    pub cover_uri: String,
    /// Whether the recompiled module and extracted disc are still on disk.
    /// The .app is deliberately not part of this: a game is playable from the
    /// library without one.
    #[serde(skip)]
    pub ready: bool,
    /// Whether this game also has a .app bundle built for it.
    #[serde(skip)]
    pub has_app: bool,
}

#[derive(Default, Serialize, Deserialize)]
struct Index {
    #[serde(default)]
    games: Vec<Game>,
}

pub fn support_dir() -> PathBuf {
    let home = std::env::var("HOME").unwrap_or_else(|_| "/tmp".into());
    PathBuf::from(home).join("Library/Application Support/DolBundler")
}

pub fn index_path() -> PathBuf {
    support_dir().join("library.json")
}

pub fn user_dir() -> PathBuf {
    match std::env::var("XDG_DATA_HOME") {
        Ok(value) if !value.is_empty() => PathBuf::from(value).join("moderngekko"),
        _ => PathBuf::from(std::env::var("HOME").unwrap_or_else(|_| "/tmp".into()))
            .join(".local/share/moderngekko"),
    }
}

pub fn load() -> Vec<Game> {
    let text = match std::fs::read_to_string(index_path()) {
        Ok(text) => text,
        Err(_) => return Vec::new(),
    };
    let index: Index = serde_json::from_str(&text).unwrap_or_default();
    index
        .games
        .into_iter()
        .map(|mut game| {
            game.cover_uri = read_cover(&game.cover);
            game.ready = !game.module.is_empty()
                && PathBuf::from(&game.module).is_file()
                && PathBuf::from(&game.game_root).is_dir();
            game.has_app = !game.app.is_empty() && PathBuf::from(&game.app).is_dir();
            game
        })
        .collect()
}

pub fn save(games: &[Game]) -> std::io::Result<()> {
    let path = index_path();
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)?;
    }
    let index = Index {
        games: games.to_vec(),
    };
    let text = serde_json::to_string_pretty(&index).unwrap_or_else(|_| "{}".into());
    std::fs::write(path, text + "\n")
}

/// The webview cannot read file:// paths from this page, so cover art rides
/// along as a data URI. The images are a few KB each.
fn read_cover(path: &str) -> String {
    if path.is_empty() {
        return String::new();
    }
    match std::fs::read(path) {
        Ok(bytes) => format!(
            "data:image/png;base64,{}",
            base64::engine::general_purpose::STANDARD.encode(bytes)
        ),
        Err(_) => String::new(),
    }
}

/// Last `lines` lines of a game's runtime log.
pub fn tail_log(disc_id: &str, lines: usize) -> Vec<String> {
    let path = user_dir().join("Logs").join(format!("{disc_id}.log"));
    match std::fs::read_to_string(&path) {
        Ok(text) => {
            let all: Vec<&str> = text.lines().collect();
            all[all.len().saturating_sub(lines)..]
                .iter()
                .map(|line| line.to_string())
                .collect()
        }
        Err(err) => vec![format!("no log at {}: {err}", path.display())],
    }
}
