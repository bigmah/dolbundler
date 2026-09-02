//! Wireless transport over Bluetooth LE.
//!
//! The controller does not expose HID over GATT and it refuses standard SMP
//! pairing from a host with no IO capability. Instead it carries the same SW2
//! command frames over two custom services, so once the characteristics are
//! found the protocol is identical to USB apart from the interface byte.
//!
//! Channel choice is not obvious and matters: commands must go to the *shared*
//! command characteristic and replies come back on the *shared* response
//! characteristic. The per-controller "vibration + command" characteristic
//! accepts writes but answers with bare acks carrying no payload.
//!
//! Note the ceiling: the controller generates input at roughly 1 kHz, but the
//! BLE link delivers around 70 Hz. Wired USB is materially lower latency.

use std::str::FromStr;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Duration;

use anyhow::{Context, Result, anyhow, bail};
use btleplug::api::{Central, Manager as _, Peripheral as _, ScanFilter, WriteType};
use btleplug::platform::{Manager, Peripheral};
use futures::StreamExt;
use uuid::Uuid;

use crate::protocol::calibration::Calibration;
use crate::protocol::command::{self, Iface};
use crate::protocol::report;
use crate::protocol::state::RawState;

/// Characteristic UUIDs. The command and response channels are shared across
/// the whole Switch 2 family; the input report characteristic is too.
mod uuids {
    pub const SERVICE_ENABLE: &str = "00c5af5d-1964-4e30-8f51-1956f96bd282";
    pub const INPUT_REPORT: &str = "ab7de9be-89fe-49ad-828f-118f09df7fd2";
    pub const COMMAND_WRITE: &str = "649d4ac9-8eb7-4e6c-af44-1ea54fe5f005";
    pub const COMMAND_RESPONSE: &str = "c765a961-d9d8-4d36-a20a-5315b111836a";
}

/// Written to the service-enable characteristic to switch the proprietary
/// protocol on. Without it every later command is silently dropped.
const SERVICE_ENABLE_VALUE: [u8; 2] = [0x01, 0x00];

/// Per-command reply deadline. A vibration ack can take over 300 ms, so this is
/// deliberately generous; replies are matched by cmd + subcmd so a late one
/// cannot be mistaken for the next command's.
const CMD_TIMEOUT: Duration = Duration::from_millis(500);

/// Deadline for connecting and for service discovery. Generous — a cold
/// controller is not quick — but finite, because neither call is interruptible
/// and an unbounded one wedges the process against Ctrl-C.
const CONNECT_TIMEOUT: Duration = Duration::from_secs(10);
const FLASH_PAYLOAD_OFFSET: usize = 0x10;

/// Nintendo's Bluetooth SIG company identifier, as it appears in the
/// advertisement's manufacturer data. This is what actually identifies the
/// controller — see `find_controller`.
const NINTENDO_COMPANY_ID: u16 = 0x0553;

/// Names the controller may advertise itself under. Only a fallback: in sync
/// mode it advertises unnamed.
const NAME_HINTS: &[&str] = &["GameCube", "NSO", "Nintendo"];

pub struct BleTransport {
    peripheral: Peripheral,
    input: btleplug::api::Characteristic,
    command: btleplug::api::Characteristic,
    response: btleplug::api::Characteristic,
    enable: Option<btleplug::api::Characteristic>,
}

impl BleTransport {
    /// Scans until a controller appears, then connects and resolves characteristics.
    ///
    /// The controller only advertises while in sync mode, so put it there first
    /// (hold the sync button until the player LEDs chase).
    ///
    /// `running` is polled between scan rounds so a signal lands promptly.
    /// Without it the scan would ignore Ctrl-C for up to `timeout`, which is
    /// precisely the state you are in when you decide to give up on syncing.
    pub async fn discover(timeout: Duration, running: &AtomicBool) -> Result<Self> {
        let manager = Manager::new().await.context("starting CoreBluetooth")?;
        let central = manager
            .adapters()
            .await
            .context("listing Bluetooth adapters")?
            .into_iter()
            .next()
            .ok_or_else(|| anyhow!("no Bluetooth adapter available"))?;

        central
            .start_scan(ScanFilter::default())
            .await
            .context("starting BLE scan (grant Bluetooth permission if macOS prompted)")?;

        let deadline = tokio::time::Instant::now() + timeout;
        let peripheral = loop {
            if let Some(p) = find_controller(&central).await {
                break p;
            }
            if !running.load(Ordering::Relaxed) {
                let _ = central.stop_scan().await;
                bail!("cancelled while scanning");
            }
            if tokio::time::Instant::now() >= deadline {
                let _ = central.stop_scan().await;
                bail!(
                    "no controller found in {timeout:?}; hold the sync button until the player LEDs chase, then retry"
                );
            }
            tokio::time::sleep(Duration::from_millis(200)).await;
        };
        let _ = central.stop_scan().await;

        // Both of these can hang indefinitely against a controller that
        // advertises but will not finish the link — CoreBluetooth simply never
        // calls back. Unbounded, they swallow Ctrl-C and the process needs
        // SIGKILL, so each gets a deadline of its own.
        tokio::time::timeout(CONNECT_TIMEOUT, peripheral.connect())
            .await
            .map_err(|_| anyhow!("timed out connecting after {CONNECT_TIMEOUT:?}"))?
            .context("connecting")?;
        tokio::time::timeout(CONNECT_TIMEOUT, peripheral.discover_services())
            .await
            .map_err(|_| anyhow!("timed out discovering services after {CONNECT_TIMEOUT:?}"))?
            .context("discovering GATT services")?;

        let find = |raw: &str| -> Option<btleplug::api::Characteristic> {
            let want = Uuid::from_str(raw).ok()?;
            peripheral.characteristics().into_iter().find(|c| c.uuid == want)
        };

        let input = find(uuids::INPUT_REPORT).ok_or_else(|| {
            anyhow!("input report characteristic missing; not a Switch 2 controller?")
        })?;
        let command =
            find(uuids::COMMAND_WRITE).ok_or_else(|| anyhow!("command characteristic missing"))?;
        let response = find(uuids::COMMAND_RESPONSE)
            .ok_or_else(|| anyhow!("command response characteristic missing"))?;
        let enable = find(uuids::SERVICE_ENABLE);

        Ok(Self { peripheral, input, command, response, enable })
    }

    /// The peripheral's advertised name, for logging.
    pub async fn name(&self) -> String {
        self.peripheral
            .properties()
            .await
            .ok()
            .flatten()
            .and_then(|p| p.local_name)
            .unwrap_or_else(|| "NSO GameCube Controller".into())
    }

    /// Writes one command frame.
    ///
    /// The characteristic advertises both write modes; only write-without-response
    /// works on hardware.
    async fn send(&self, frame: &[u8]) -> Result<()> {
        let len = command::frame_len(frame);
        self.peripheral
            .write(&self.command, &frame[..len], WriteType::WithoutResponse)
            .await
            .context("writing command")
    }

    /// Sends a command and waits for the reply that matches its cmd + subcmd.
    async fn request(&self, frame: &[u8]) -> Result<Vec<u8>> {
        let mut notifications = self.peripheral.notifications().await?;
        self.send(frame).await?;

        let wait = async {
            while let Some(n) = notifications.next().await {
                if n.uuid == self.response.uuid && command::response_matches(frame, &n.value) {
                    return Ok(n.value);
                }
            }
            Err(anyhow!("notification stream ended"))
        };

        tokio::time::timeout(CMD_TIMEOUT, wait)
            .await
            .map_err(|_| anyhow!("no reply to command {:#04x}/{:#04x}", frame[0], frame[3]))?
    }

    async fn read_flash(&self, address: u32) -> Result<[u8; 0x40]> {
        let reply = self.request(&command::flash_read(address, Iface::Ble)).await?;
        let payload = reply
            .get(FLASH_PAYLOAD_OFFSET..FLASH_PAYLOAD_OFFSET + 0x40)
            .ok_or_else(|| anyhow!("short flash reply for {address:#x}: {} bytes", reply.len()))?;
        let mut out = [0u8; 0x40];
        out.copy_from_slice(payload);
        Ok(out)
    }

    /// Enables the proprietary service, reads calibration, and runs init.
    pub async fn initialise(&self) -> Result<Calibration> {
        if let Some(enable) = &self.enable {
            // Best effort: some firmware revisions have this on already.
            let _ = self
                .peripheral
                .write(enable, &SERVICE_ENABLE_VALUE, WriteType::WithoutResponse)
                .await;
        }

        // Replies and input reports share a notification stream, so take
        // calibration before the controller starts streaming.
        self.peripheral
            .subscribe(&self.response)
            .await
            .context("subscribing to command responses")?;

        let mut cal = Calibration::default();
        for &address in Calibration::BLOCKS {
            match self.read_flash(address).await {
                Ok(block) => cal.absorb(address, &block),
                Err(e) => eprintln!("warning: flash block {address:#x} unreadable: {e:#}"),
            }
        }

        for frame in command::INIT_SEQUENCE {
            let frame = command::with_iface(frame, Iface::Ble);
            // Init acks are advisory; a missing one is not fatal, and stopping
            // here would leave the controller half-configured.
            if let Err(e) = self.request(&frame).await {
                eprintln!("warning: init command {:#04x}/{:#04x}: {e:#}", frame[0], frame[3]);
            }
        }

        let led = command::set_player_leds(0x01, Iface::Ble);
        let _ = self.send(&led).await;

        // Stop response notifications so they cannot interleave with input.
        let _ = self.peripheral.unsubscribe(&self.response).await;
        Ok(cal)
    }

    /// Subscribes to input reports and yields decoded state.
    pub async fn reports(&self) -> Result<impl futures::Stream<Item = RawState> + use<>> {
        self.peripheral
            .subscribe(&self.input)
            .await
            .context("subscribing to input reports")?;
        let input_uuid = self.input.uuid;
        let stream = self.peripheral.notifications().await?;
        Ok(stream.filter_map(move |n| async move {
            if n.uuid != input_uuid {
                return None;
            }
            report::decode(&n.value, report::BLE_BODY_OFFSET)
        }))
    }

    pub async fn is_connected(&self) -> bool {
        self.peripheral.is_connected().await.unwrap_or(false)
    }

    pub async fn disconnect(&self) {
        let _ = self.peripheral.disconnect().await;
    }
}

/// Picks the first advertising peripheral that looks like a Switch 2 controller.
///
/// The manufacturer ID is the reliable signal. In sync mode the controller
/// advertises with **no local name at all** on macOS — CoreBluetooth surfaces a
/// name only once one arrives in a scan response, which this controller does not
/// send — so a name-only filter never matches it.
async fn find_controller(central: &impl Central<Peripheral = Peripheral>) -> Option<Peripheral> {
    for p in central.peripherals().await.ok()? {
        let Ok(Some(props)) = p.properties().await else {
            continue;
        };
        if props.manufacturer_data.contains_key(&NINTENDO_COMPANY_ID) {
            return Some(p);
        }
        // Keep the name check as a fallback: a controller already known to
        // macOS can come back with a name and no manufacturer data.
        let name = props.local_name.unwrap_or_default();
        if NAME_HINTS.iter().any(|h| name.contains(h)) {
            return Some(p);
        }
    }
    None
}
