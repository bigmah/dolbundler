// SPDX-License-Identifier: GPL-3.0-or-later
//! The iPhone half of the library: which games have been recompiled for a
//! phone, and which are on one.
//!
//! `recompios` does all of the work and records what it did as small files
//! under `<support>/iphone`, so this module only reads a directory. That is
//! deliberate, and it is the same choice the phone itself makes: a game is on
//! the phone because its extracted disc is, not because a manifest says so.
//! A stamp that outlived what it described would offer a Play button for a
//! game the app cannot boot.
//!
//! ```text
//! <support>/iphone/
//!   modules/<disc-id>/generated/     arm64 iPhone objects, linked in at build
//!   modules/<disc-id>/dol.sha256     the DOL, triple and CPU they came from
//!   devices/<device>/name            what that phone is called
//!   devices/<device>/installed       when the app was last installed on it
//!   devices/<device>/games/<disc-id> size of the disc copied onto it
//! ```

use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

use crate::library;

/// A paired iPhone, as `recompios devices` reports it.
#[derive(Clone, Debug, PartialEq)]
pub struct Device {
    /// The devicectl identifier. Stable, and what every command is given.
    pub id: String,
    /// What the phone is called in Settings.
    pub name: String,
    /// "iPhone 15 Pro Max".
    pub model: String,
}

impl Device {
    pub fn label(&self) -> String {
        if self.model.is_empty() {
            self.name.clone()
        } else {
            format!("{} ({})", self.name, self.model)
        }
    }
}

/// An Apple Developer team that could sign the app, with what stands between
/// it and this phone. `recompios teams` orders these best-first.
#[derive(Clone, Debug, PartialEq)]
pub struct Team {
    pub id: String,
    pub label: String,
}

/// What one library game's iPhone state is.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum State {
    /// Never recompiled for a phone.
    Absent,
    /// Recompiled and ready to be linked into the next build, but the disc has
    /// not been copied to the selected phone.
    Built,
    /// Recompiled, installed, and its disc is on the selected phone.
    OnPhone,
}

pub fn dir() -> PathBuf {
    library::support_dir().join("iphone")
}

fn modules_dir() -> PathBuf {
    dir().join("modules")
}

fn device_dir(device_id: &str) -> PathBuf {
    dir().join("devices").join(device_id)
}

/// Whether a complete iPhone module exists for this game. Both halves are
/// checked because `recompios` writes the stamp last: a build interrupted part
/// way leaves objects that must not be linked into an app.
fn is_built(disc_id: &str) -> bool {
    let module = modules_dir().join(disc_id);
    module.join("generated/generated.h").is_file() && module.join("dol.sha256").is_file()
}

fn is_on_phone(device_id: &str, disc_id: &str) -> bool {
    !device_id.is_empty() && device_dir(device_id).join("games").join(disc_id).is_file()
}

pub fn state(disc_id: &str, device_id: &str) -> State {
    if is_on_phone(device_id, disc_id) {
        State::OnPhone
    } else if is_built(disc_id) {
        State::Built
    } else {
        State::Absent
    }
}

impl State {
    /// The words on a game card. Empty when there is nothing worth saying.
    pub fn chip(self) -> &'static str {
        match self {
            State::OnPhone => "on iPhone",
            State::Built => "built for iPhone",
            State::Absent => "",
        }
    }

    pub fn detail(self) -> &'static str {
        match self {
            State::OnPhone => "Recompiled, installed, and its disc is on the phone.",
            State::Built => "Recompiled for iPhone. Send it to put it on the phone.",
            State::Absent => "Not sent to an iPhone yet.",
        }
    }
}

/// When the app was last installed on this phone, as `recompios` recorded it.
pub fn last_install(device_id: &str) -> Option<String> {
    std::fs::read_to_string(device_dir(device_id).join("installed"))
        .ok()
        .map(|stamp| stamp.trim().to_string())
        .filter(|stamp| !stamp.is_empty())
}

/// The paired iPhones. An empty list means none is reachable, which is the
/// same answer as none being paired as far as anything here can act on it.
pub fn list_devices(recompios: &Path) -> Vec<Device> {
    lines(recompios, &["devices"])
        .into_iter()
        .filter_map(|line| {
            let mut fields = line.split('\t');
            let id = fields.next()?.to_string();
            let name = fields.next().unwrap_or_default().to_string();
            let model = fields.next().unwrap_or_default().to_string();
            (!id.is_empty()).then_some(Device { id, name, model })
        })
        .collect()
}

/// The teams that could sign the app. `device_id` may be empty; given one, the
/// labels say whether a team's provisioning profile actually covers that phone,
/// which is the difference between an install and a twenty-minute build that
/// fails at the last step.
pub fn list_teams(recompios: &Path, device_id: &str) -> Vec<Team> {
    let args: Vec<&str> = if device_id.is_empty() {
        vec!["teams"]
    } else {
        vec!["teams", "--device", device_id]
    };
    lines(recompios, &args)
        .into_iter()
        .filter_map(|line| {
            let (id, label) = line.split_once('\t')?;
            (!id.is_empty()).then(|| Team {
                id: id.to_string(),
                label: label.to_string(),
            })
        })
        .collect()
}

fn lines(recompios: &Path, args: &[&str]) -> Vec<String> {
    let output = Command::new(recompios)
        .args(args)
        .stdin(Stdio::null())
        .output();
    let Ok(output) = output else {
        return Vec::new();
    };
    if !output.status.success() {
        return Vec::new();
    }
    String::from_utf8_lossy(&output.stdout)
        .lines()
        .filter(|line| !line.trim().is_empty())
        .map(|line| line.to_string())
        .collect()
}
