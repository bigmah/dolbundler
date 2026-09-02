//! Dolphin "Pipe" input backend.
//!
//! Dolphin scans `~/Library/Application Support/Dolphin/Pipes/` at startup and
//! exposes every FIFO it can open as an input device named `Pipe/0/<filename>`.
//! It speaks a line-oriented text protocol:
//!
//! ```text
//! PRESS A          RELEASE A
//! SET MAIN 0.5 0.5     (x, y in 0..1, 0.5 centred)
//! SET L 0.75           (analog trigger, 0..1)
//! ```
//!
//! Two rules keep this path fast. We only ever write what changed, and we never
//! block: the FIFO is opened non-blocking and a full pipe drops the update
//! rather than stalling the transport thread. Dropped updates are self-healing
//! because the committed state only advances on a successful write, so the next
//! report re-sends whatever did not land.

use std::ffi::CString;
use std::fmt::Write as _;
use std::io;
use std::path::{Path, PathBuf};

use crate::protocol::state::GcState;

/// macOS `PIPE_BUF`. Writes up to this size are atomic on a non-blocking FIFO:
/// they either land whole or fail with `EAGAIN`, never tearing a line in half.
const PIPE_BUF: usize = 512;

/// One of Dolphin's button tokens and the field it reflects.
type Button = (&'static str, fn(&GcState) -> bool);

/// One stick: its token, the value now, and the value last sent.
type StickDelta<'a> = (&'a str, (f32, f32), Option<(f32, f32)>);

/// Buttons Dolphin's pipe device understands, paired with their accessor.
const BUTTONS: &[Button] = &[
    ("A", |s| s.a),
    ("B", |s| s.b),
    ("X", |s| s.x),
    ("Y", |s| s.y),
    ("Z", |s| s.z),
    ("START", |s| s.start),
    ("L", |s| s.l),
    ("R", |s| s.r),
    ("D_UP", |s| s.d_up),
    ("D_DOWN", |s| s.d_down),
    ("D_LEFT", |s| s.d_left),
    ("D_RIGHT", |s| s.d_right),
];

/// Dolphin's default pipe directory.
pub fn default_dir() -> Option<PathBuf> {
    dirs::home_dir().map(|h| h.join("Library/Application Support/Dolphin/Pipes"))
}

/// A named FIFO that Dolphin reads as a controller.
pub struct DolphinPipe {
    path: PathBuf,
    fd: Option<i32>,
    /// State Dolphin has actually acknowledged receipt of, by way of a
    /// successful `write`. Only advances when bytes leave.
    committed: Option<GcState>,
    buf: String,
    dropped: u64,
}

impl DolphinPipe {
    /// Creates the FIFO if absent. Does not wait for Dolphin to open it.
    pub fn create(dir: &Path, name: &str) -> anyhow::Result<Self> {
        std::fs::create_dir_all(dir)?;
        let path = dir.join(name);

        // A stale regular file here would silently swallow every write, so
        // insist on an actual FIFO.
        match std::fs::metadata(&path) {
            Ok(m) => {
                let is_fifo = {
                    use std::os::unix::fs::FileTypeExt;
                    m.file_type().is_fifo()
                };
                if !is_fifo {
                    anyhow::bail!("{} exists and is not a FIFO; remove it", path.display());
                }
            }
            Err(e) if e.kind() == io::ErrorKind::NotFound => {
                let c = CString::new(path.as_os_str().as_encoded_bytes())?;
                // SAFETY: `c` is a valid NUL-terminated path for the duration.
                if unsafe { libc::mkfifo(c.as_ptr(), 0o644) } != 0 {
                    return Err(io::Error::last_os_error().into());
                }
            }
            Err(e) => return Err(e.into()),
        }

        Ok(Self {
            path,
            fd: None,
            committed: None,
            buf: String::with_capacity(PIPE_BUF),
            dropped: 0,
        })
    }

    pub fn path(&self) -> &Path {
        &self.path
    }

    /// Whether Dolphin currently has the read end open.
    pub fn is_connected(&self) -> bool {
        self.fd.is_some()
    }

    /// Number of updates dropped because Dolphin was not draining the pipe.
    pub fn dropped(&self) -> u64 {
        self.dropped
    }

    /// Tries to open the write end. `ENXIO` means Dolphin is not reading yet,
    /// which is the normal state before it launches — not an error.
    fn try_open(&mut self) -> bool {
        if self.fd.is_some() {
            return true;
        }
        let Ok(c) = CString::new(self.path.as_os_str().as_encoded_bytes()) else {
            return false;
        };
        // SAFETY: `c` is a valid NUL-terminated path for the duration of the call.
        let fd = unsafe { libc::open(c.as_ptr(), libc::O_WRONLY | libc::O_NONBLOCK) };
        if fd < 0 {
            return false;
        }
        self.fd = Some(fd);
        // A fresh reader knows nothing about us, so replay the whole state.
        self.committed = None;
        true
    }

    fn close(&mut self) {
        if let Some(fd) = self.fd.take() {
            // SAFETY: `fd` came from `open` above and is closed exactly once.
            unsafe { libc::close(fd) };
        }
        self.committed = None;
    }

    /// Writes one chunk. `Ok(true)` if it landed, `Ok(false)` if the pipe was
    /// full, `Err` if the reader went away.
    fn write_chunk(&mut self, s: &str) -> io::Result<bool> {
        let Some(fd) = self.fd else {
            return Ok(false);
        };
        // SAFETY: writing `s.len()` bytes from a live `&str` to an owned fd.
        let n = unsafe { libc::write(fd, s.as_ptr().cast(), s.len()) };
        if n >= 0 {
            return Ok(n as usize == s.len());
        }
        let err = io::Error::last_os_error();
        match err.raw_os_error() {
            Some(libc::EAGAIN) => Ok(false),
            _ => Err(err),
        }
    }

    /// Appends the commands needed to move Dolphin from `prev` to `next`.
    ///
    /// `prev` of `None` emits a full state sync, which a newly-attached reader
    /// needs since it starts from Dolphin's own defaults.
    fn encode_delta(buf: &mut String, prev: Option<&GcState>, next: &GcState) {
        buf.clear();
        for (name, get) in BUTTONS {
            let now = get(next);
            if prev.is_none_or(|p| get(p) != now) {
                let verb = if now { "PRESS" } else { "RELEASE" };
                let _ = writeln!(buf, "{verb} {name}");
            }
        }

        // Sticks arrive as -1..1 and Dolphin wants 0..1 with 0.5 centred.
        // Positive Y is up, matching the usual Dolphin pipe convention.
        let axes: [StickDelta<'_>; 2] = [
            ("MAIN", next.main, prev.map(|p| p.main)),
            ("C", next.c_stick, prev.map(|p| p.c_stick)),
        ];
        for (name, (x, y), was) in axes {
            let (px, py) = (to_unit(x), to_unit(y));
            // Compare what would be printed, so we stay quiet under sensor noise
            // but never skip a change Dolphin could have seen.
            let changed = was.is_none_or(|(ox, oy)| {
                !q4_eq(to_unit(ox), px) || !q4_eq(to_unit(oy), py)
            });
            if changed {
                let _ = writeln!(buf, "SET {name} {px:.4} {py:.4}");
            }
        }

        let triggers: [(&str, f32, Option<f32>); 2] = [
            ("L", next.trigger_l, prev.map(|p| p.trigger_l)),
            ("R", next.trigger_r, prev.map(|p| p.trigger_r)),
        ];
        for (name, v, was) in triggers {
            if was.is_none_or(|o| !q4_eq(o, v)) {
                let _ = writeln!(buf, "SET {name} {v:.4}");
            }
        }
    }

    /// Pushes `state` to Dolphin. Cheap and non-blocking; safe to call at the
    /// controller's full report rate.
    pub fn update(&mut self, state: &GcState) -> io::Result<()> {
        if !self.try_open() {
            return Ok(());
        }

        // `encode_delta` needs `&mut self.buf` while reading `self.committed`,
        // so take the buffer for the duration.
        let mut buf = std::mem::take(&mut self.buf);
        Self::encode_delta(&mut buf, self.committed.as_ref(), state);
        let result = self.flush_delta(&buf, state);
        self.buf = buf;
        result
    }

    fn flush_delta(&mut self, buf: &str, state: &GcState) -> io::Result<()> {
        if buf.is_empty() {
            return Ok(());
        }

        // Split on line boundaries so every chunk stays within PIPE_BUF and is
        // therefore written atomically.
        let mut start = 0;
        while start < buf.len() {
            let mut end = (start + PIPE_BUF).min(buf.len());
            if end < buf.len() {
                match buf[start..end].rfind('\n') {
                    Some(nl) => end = start + nl + 1,
                    // A single line longer than PIPE_BUF cannot happen with the
                    // fixed command set above, but refuse to tear one if it did.
                    None => break,
                }
            }
            match self.write_chunk(&buf[start..end]) {
                Ok(true) => start = end,
                Ok(false) => {
                    // Pipe full. Leave `committed` where it is so the unsent
                    // remainder is recomputed and retried on the next report.
                    self.dropped += 1;
                    return Ok(());
                }
                Err(e) => {
                    // EPIPE: Dolphin quit. Drop the fd and wait for it to return.
                    self.close();
                    return if e.raw_os_error() == Some(libc::EPIPE) {
                        Ok(())
                    } else {
                        Err(e)
                    };
                }
            }
        }

        self.committed = Some(*state);
        Ok(())
    }
}

impl Drop for DolphinPipe {
    fn drop(&mut self) {
        self.close();
    }
}

/// Maps `-1.0..=1.0` onto the `0.0..=1.0` range Dolphin expects, 0.5 centred.
fn to_unit(v: f32) -> f32 {
    ((v + 1.0) / 2.0).clamp(0.0, 1.0)
}

/// Equality at the 4 decimal places we actually print.
fn q4_eq(a: f32, b: f32) -> bool {
    (a * 10_000.0).round() == (b * 10_000.0).round()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn delta(prev: Option<&GcState>, next: &GcState) -> String {
        let mut s = String::new();
        DolphinPipe::encode_delta(&mut s, prev, next);
        s
    }

    #[test]
    fn first_update_syncs_every_control() {
        let out = delta(None, &GcState::default());
        for (name, _) in BUTTONS {
            assert!(out.contains(&format!("RELEASE {name}\n")), "missing {name}");
        }
        assert!(out.contains("SET MAIN 0.5000 0.5000\n"));
        assert!(out.contains("SET C 0.5000 0.5000\n"));
        assert!(out.contains("SET L 0.0000\n"));
        assert!(out.contains("SET R 0.0000\n"));
    }

    #[test]
    fn steady_state_emits_nothing() {
        let s = GcState { a: true, main: (0.25, -0.5), ..Default::default() };
        assert_eq!(delta(Some(&s), &s), "");
    }

    #[test]
    fn only_the_changed_control_is_sent() {
        let prev = GcState::default();
        let next = GcState { a: true, ..Default::default() };
        assert_eq!(delta(Some(&prev), &next), "PRESS A\n");

        let after = GcState::default();
        assert_eq!(delta(Some(&next), &after), "RELEASE A\n");
    }

    #[test]
    fn stick_range_maps_onto_dolphins_unit_axis() {
        let prev = GcState::default();
        let next = GcState { main: (-1.0, 1.0), c_stick: (1.0, -1.0), ..Default::default() };
        let out = delta(Some(&prev), &next);
        assert!(out.contains("SET MAIN 0.0000 1.0000\n"), "got {out}");
        assert!(out.contains("SET C 1.0000 0.0000\n"), "got {out}");
    }

    #[test]
    fn sub_quantum_jitter_does_not_produce_traffic() {
        let prev = GcState { main: (0.5, 0.0), ..Default::default() };
        // Well under the 4th decimal place of the 0..1 output range.
        let next = GcState { main: (0.500_01, 0.000_01), ..Default::default() };
        assert_eq!(delta(Some(&prev), &next), "");
    }

    #[test]
    fn a_change_at_the_printed_precision_is_sent() {
        let prev = GcState { main: (0.0, 0.0), ..Default::default() };
        let next = GcState { main: (0.001, 0.0), ..Default::default() };
        assert!(delta(Some(&prev), &next).starts_with("SET MAIN "));
    }

    #[test]
    fn triggers_are_sent_in_dolphins_zero_to_one_range() {
        let prev = GcState::default();
        let next = GcState { trigger_l: 1.0, trigger_r: 0.5, ..Default::default() };
        let out = delta(Some(&prev), &next);
        assert!(out.contains("SET L 1.0000\n"), "got {out}");
        assert!(out.contains("SET R 0.5000\n"), "got {out}");
    }

    #[test]
    fn a_full_sync_fits_in_one_atomic_write() {
        // Every control differing at once is the largest delta possible.
        let next = GcState {
            a: true, b: true, x: true, y: true, z: true, start: true,
            l: true, r: true, d_up: true, d_down: true, d_left: true, d_right: true,
            main: (-1.0, -1.0), c_stick: (-1.0, -1.0),
            trigger_l: 1.0, trigger_r: 1.0,
            ..Default::default()
        };
        let out = delta(None, &next);
        assert!(out.len() <= PIPE_BUF, "delta was {} bytes", out.len());
    }
}
