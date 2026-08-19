// SPDX-License-Identifier: GPL-3.0-or-later
//! Runs `recompgc --porcelain` and turns its output into UI messages.
//!
//! The porcelain protocol is documented at the top of `recompgc`: `@@`-prefixed
//! lines carry structured events, everything else is log text.

use futures_channel::mpsc::UnboundedSender;
use std::io::{BufRead, BufReader};
use std::path::PathBuf;
use std::process::{Command, Stdio};

#[derive(Clone, Debug)]
pub enum Msg {
    Line(String),
    Step { index: u32, total: u32, title: String },
    Info { key: String, value: String },
    Failed(String),
    Finished(bool),
}

/// The four-step disc pipeline.
pub fn run(recompgc: PathBuf, image: PathBuf, tx: UnboundedSender<Msg>) {
    run_args(
        recompgc,
        vec![
            "--porcelain".into(),
            image.to_string_lossy().into_owned(),
        ],
        tx,
    );
}

/// Any `recompgc` invocation, streamed as porcelain. Used by `run` and by the
/// `make-app` subcommand the library's Create App button calls.
pub fn run_args(recompgc: PathBuf, args: Vec<String>, tx: UnboundedSender<Msg>) {
    std::thread::spawn(move || {
        let spawned = Command::new(&recompgc)
            .args(&args)
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn();

        let mut child = match spawned {
            Ok(child) => child,
            Err(err) => {
                let _ = tx.unbounded_send(Msg::Failed(format!(
                    "could not start {}: {err}",
                    recompgc.display()
                )));
                let _ = tx.unbounded_send(Msg::Finished(false));
                return;
            }
        };

        let stdout = child.stdout.take();
        let stderr = child.stderr.take();

        // stderr only ever carries the failure line, but it is read on its own
        // thread so a full pipe buffer can never wedge the child.
        let err_tx = tx.clone();
        let err_thread = std::thread::spawn(move || {
            if let Some(stderr) = stderr {
                for line in BufReader::new(stderr).lines().map_while(Result::ok) {
                    let _ = err_tx.unbounded_send(Msg::Line(line));
                }
            }
        });

        if let Some(stdout) = stdout {
            for line in BufReader::new(stdout).lines().map_while(Result::ok) {
                let _ = tx.unbounded_send(parse(line));
            }
        }
        let _ = err_thread.join();

        let success = child.wait().map(|status| status.success()).unwrap_or(false);
        let _ = tx.unbounded_send(Msg::Finished(success));
    });
}

fn parse(line: String) -> Msg {
    let Some(event) = line.strip_prefix("@@") else {
        return Msg::Line(line);
    };
    if let Some(rest) = event.strip_prefix("step ") {
        let mut parts = rest.splitn(3, ' ');
        let index = parts.next().and_then(|v| v.parse().ok()).unwrap_or(0);
        let total = parts.next().and_then(|v| v.parse().ok()).unwrap_or(0);
        let title = parts.next().unwrap_or_default().to_string();
        return Msg::Step { index, total, title };
    }
    if let Some(rest) = event.strip_prefix("info ") {
        if let Some((key, value)) = rest.split_once('=') {
            return Msg::Info {
                key: key.to_string(),
                value: value.to_string(),
            };
        }
    }
    if let Some(rest) = event.strip_prefix("fail ") {
        return Msg::Failed(rest.to_string());
    }
    if event == "ok" {
        // Success is confirmed by the exit status; this only marks the point
        // where the pipeline stopped producing steps.
        return Msg::Line(String::new());
    }
    Msg::Line(line)
}
