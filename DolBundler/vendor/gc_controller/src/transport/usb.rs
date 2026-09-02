//! Wired transport. Uses both of the controller's USB interfaces.
//!
//! The controller exposes two interfaces and they are not interchangeable:
//!
//! * **Interface 0**, HID class. macOS binds its own driver here on plug-in.
//!   This is where input reports stream from, so we read it through IOKit's HID
//!   stack rather than trying to claim it.
//! * **Interface 1**, vendor class (0xFF), bulk endpoints, no driver attached.
//!   This is the SW2 command channel: flash reads, the init sequence, LEDs.
//!
//! Sending init over the vendor interface and then reading input from the same
//! endpoint yields nothing at all — the split is what the console does, and it is
//! load-bearing.
//!
//! The read loop is a blocking HID read on its own thread: no runtime, no
//! channel, no buffering between the wire and the output sink.

use std::time::Duration;

use anyhow::{Context, Result, anyhow};
use rusb::{Direction, GlobalContext, TransferType};

use crate::protocol::calibration::Calibration;
use crate::protocol::command::{self, Iface};
use crate::protocol::report;
use crate::protocol::state::RawState;
use crate::protocol::{PID_NSO_GAMECUBE, VID_NINTENDO};

/// The vendor-specific interface carrying the SW2 command protocol.
const VENDOR_INTERFACE: u8 = 1;
/// Largest packet either interface uses.
const PACKET: usize = 64;
/// A flash read replies with a header plus 0x40 bytes of payload.
const FLASH_REPLY: usize = 0x50;
const FLASH_PAYLOAD_OFFSET: usize = 0x10;

/// The report ID the init sequence selects. `hidapi` hands back numbered
/// reports with this byte still in front, so the body starts one later.
const REPORT_ID_INPUT: u8 = 0x05;

/// How long to wait for a command acknowledgement during setup.
const CMD_TIMEOUT: Duration = Duration::from_millis(250);
/// Short timeout used when clearing stale packets out of the bulk pipe.
const DRAIN_TIMEOUT: Duration = Duration::from_millis(20);
/// Blocking read timeout while streaming. Long enough that a quiet controller
/// does not look like an error, short enough to notice an unplug promptly.
const READ_TIMEOUT_MS: i32 = 1000;

/// What one poll produced.
pub enum Poll {
    /// The read timed out. Not an error — just nothing new.
    Idle,
    /// A report arrived that we do not decode, e.g. a different report ID.
    /// Carries its length so `--raw` can show it.
    Unknown(usize),
    Report(RawState),
}

pub struct UsbTransport {
    /// Vendor interface, for commands.
    handle: rusb::DeviceHandle<GlobalContext>,
    ep_in: u8,
    ep_out: u8,
    /// HID interface, for input reports.
    hid: hidapi::HidDevice,
    buf: [u8; PACKET],
    last_len: usize,
}

/// Whether the controller is plugged in.
///
/// Deliberately separate from [`UsbTransport::open`]: `auto` needs to choose a
/// transport, and a plain "is it there" answer is easier to trust than picking
/// apart the failure of an open that was never going to succeed. A `false` here
/// means absent, not busy — a controller held by another process still reports
/// present, and `open` is what surfaces that.
pub fn is_present() -> bool {
    rusb::devices().is_ok_and(|list| {
        list.iter().any(|d| {
            d.device_descriptor().is_ok_and(|desc| {
                desc.vendor_id() == VID_NINTENDO && desc.product_id() == PID_NSO_GAMECUBE
            })
        })
    })
}

impl UsbTransport {
    /// Finds the controller and opens both interfaces.
    pub fn open() -> Result<Self> {
        let device = rusb::devices()
            .context("enumerating USB devices")?
            .iter()
            .find(|d| {
                d.device_descriptor().is_ok_and(|desc| {
                    desc.vendor_id() == VID_NINTENDO && desc.product_id() == PID_NSO_GAMECUBE
                })
            })
            .ok_or_else(|| {
                anyhow!(
                    "no NSO GameCube controller found (looking for {VID_NINTENDO:04x}:{PID_NSO_GAMECUBE:04x})"
                )
            })?;

        let (ep_in, ep_out) = find_bulk_endpoints(&device)?;

        let handle = device.open().context(
            "opening the controller (another process may already hold the vendor interface)",
        )?;

        // Linux-only convenience; macOS reports it unsupported and needs nothing.
        let _ = handle.set_auto_detach_kernel_driver(true);

        handle
            .claim_interface(VENDOR_INTERFACE)
            .with_context(|| format!("claiming vendor interface {VENDOR_INTERFACE}"))?;

        let api = hidapi::HidApi::new().context("initialising the HID backend")?;
        let hid = api.open(VID_NINTENDO, PID_NSO_GAMECUBE).context(
            "opening the HID interface — if this fails, grant Input Monitoring \
             in System Settings → Privacy & Security",
        )?;
        hid.set_blocking_mode(true)
            .context("setting HID blocking mode")?;

        Ok(Self { handle, ep_in, ep_out, hid, buf: [0; PACKET], last_len: 0 })
    }

    fn send(&self, frame: &[u8]) -> Result<()> {
        let len = command::frame_len(frame);
        self.handle
            .write_bulk(self.ep_out, &frame[..len], CMD_TIMEOUT)
            .context("writing command")?;
        Ok(())
    }

    /// Reads one reply, tolerating the timeout a fire-and-forget command produces.
    fn recv(&self, out: &mut [u8]) -> Result<usize> {
        match self.handle.read_bulk(self.ep_in, out, CMD_TIMEOUT) {
            Ok(n) => Ok(n),
            Err(rusb::Error::Timeout) => Ok(0),
            Err(e) => Err(e).context("reading command reply"),
        }
    }

    /// Discards anything already queued on the bulk IN endpoint.
    ///
    /// A previous run that exited mid-stream can leave replies sitting in the
    /// pipe. The first flash read would then pair its request with a stale
    /// packet and come back short, which is exactly what a restart used to look
    /// like: calibration silently missing.
    fn drain(&self) {
        let mut scratch = [0u8; PACKET];
        // Bounded so a controller that streams continuously cannot trap us.
        for _ in 0..16 {
            match self.handle.read_bulk(self.ep_in, &mut scratch, DRAIN_TIMEOUT) {
                Ok(n) if n > 0 => continue,
                _ => break,
            }
        }
    }

    /// Reads 0x40 bytes from SPI flash at `address`, retrying once.
    fn read_flash(&self, address: u32) -> Result<[u8; 0x40]> {
        match self.read_flash_once(address) {
            Ok(block) => Ok(block),
            Err(_) => {
                // One short reply usually means the pipe was out of step.
                // Resynchronise and try again before giving up.
                self.drain();
                self.read_flash_once(address)
            }
        }
    }

    fn read_flash_once(&self, address: u32) -> Result<[u8; 0x40]> {
        self.send(&command::flash_read(address, Iface::Usb))?;

        let mut reply = [0u8; FLASH_REPLY];
        let mut got = 0;
        // The reply spans more than one bulk packet.
        while got < reply.len() {
            let end = (got + PACKET).min(reply.len());
            let n = self.recv(&mut reply[got..end])?;
            if n == 0 {
                break;
            }
            got += n;
        }
        if got < FLASH_PAYLOAD_OFFSET + 0x40 {
            anyhow::bail!("short flash reply for {address:#x}: {got} bytes");
        }

        let mut out = [0u8; 0x40];
        out.copy_from_slice(&reply[FLASH_PAYLOAD_OFFSET..FLASH_PAYLOAD_OFFSET + 0x40]);
        Ok(out)
    }

    /// Reads calibration, then runs the init sequence that starts the stream.
    ///
    /// Calibration comes first, while the controller is still quiet.
    pub fn initialise(&mut self) -> Result<Calibration> {
        // Clear anything a previous run left behind before the first request.
        self.drain();

        let mut cal = Calibration::default();
        for &address in Calibration::BLOCKS {
            match self.read_flash(address) {
                Ok(block) => cal.absorb(address, &block),
                // A controller with no user calibration written yet fails those
                // reads; the factory blocks are what matter.
                Err(e) => eprintln!("warning: flash block {address:#x} unreadable: {e:#}"),
            }
        }

        for frame in command::INIT_SEQUENCE {
            self.send(frame).context("running init sequence")?;
            let mut scratch = [0u8; PACKET];
            let _ = self.recv(&mut scratch)?;
        }

        Ok(cal)
    }

    /// Lights the player-indicator LEDs. Cosmetic; failure is not fatal.
    pub fn set_player_led(&self, mask: u8) -> Result<()> {
        self.send(&command::set_player_leds(mask, Iface::Usb))
    }

    /// Blocks until the next HID input report arrives, or the read times out.
    ///
    /// An error means the controller is gone.
    pub fn poll(&mut self) -> Result<Poll> {
        let n = self
            .hid
            .read_timeout(&mut self.buf, READ_TIMEOUT_MS)
            .map_err(|e| anyhow!("controller disconnected: {e}"))?;
        self.last_len = n;

        if n == 0 {
            return Ok(Poll::Idle);
        }
        if self.buf[0] != REPORT_ID_INPUT {
            return Ok(Poll::Unknown(n));
        }
        match report::decode(&self.buf[..n], report::USB_BODY_OFFSET) {
            Some(state) => Ok(Poll::Report(state)),
            None => Ok(Poll::Unknown(n)),
        }
    }

    /// The bytes of the most recent read, for `--raw`.
    pub fn last_frame(&self) -> &[u8] {
        &self.buf[..self.last_len]
    }
}

/// Locates the bulk IN and OUT endpoints on the vendor interface.
fn find_bulk_endpoints(device: &rusb::Device<GlobalContext>) -> Result<(u8, u8)> {
    let config = device
        .active_config_descriptor()
        .context("reading USB config descriptor")?;

    let (mut ep_in, mut ep_out) = (None, None);
    for iface in config.interfaces() {
        for desc in iface.descriptors() {
            if desc.interface_number() != VENDOR_INTERFACE {
                continue;
            }
            for ep in desc.endpoint_descriptors() {
                if ep.transfer_type() != TransferType::Bulk {
                    continue;
                }
                match ep.direction() {
                    Direction::In => ep_in = Some(ep.address()),
                    Direction::Out => ep_out = Some(ep.address()),
                }
            }
        }
    }

    match (ep_in, ep_out) {
        (Some(i), Some(o)) => Ok((i, o)),
        _ => Err(anyhow!(
            "no bulk endpoint pair on interface {VENDOR_INTERFACE}; is this really an NSO GameCube controller?"
        )),
    }
}
