//! Requesting a faster BLE connection interval from macOS.
//!
//! Thin wrapper over the Objective-C shim in `ble_latency.m`, which is where the
//! reasoning lives. The short version: the connection interval bounds wireless
//! input latency, macOS gives this controller 15 ms by default, and the only
//! knob an unentitled process can turn is a CoreBluetooth SPI that asks for a
//! latency *class* rather than a specific interval.
//!
//! Nothing here can promise an interval. macOS decides, and the answer is not
//! reported in documented units — so treat [`Histogram`](crate::latency::Histogram)
//! as the measurement and this module as the request.

use std::ffi::c_char;

use clap::ValueEnum;

/// How aggressively to ask macOS to schedule the link.
///
/// These mirror `CBPeripheralManagerConnectionLatency`, whose public
/// documentation describes `Low` as "prioritize rapid communication over
/// battery life". `bluetoothd` carries a fourth class beyond the public three.
#[derive(Copy, Clone, PartialEq, Eq, Debug, ValueEnum)]
pub enum Latency {
    /// Fastest scheduling. What you want for a controller.
    Low,
    Medium,
    High,
    VeryHigh,
    /// Ask for nothing and take whatever macOS picks. Use this as the baseline
    /// when comparing histograms.
    System,
}

impl Latency {
    fn level(self) -> i32 {
        match self {
            Self::Low => 0,
            Self::Medium => 1,
            Self::High => 2,
            Self::VeryHigh => 3,
            Self::System => -1,
        }
    }
}

unsafe extern "C" {
    fn gc_ble_latency_install(level: i32);
    fn gc_ble_latency_supported() -> i32;
    fn gc_ble_latency_applied() -> i32;
    fn gc_ble_latency_failed() -> i32;
    fn gc_ble_latency_hooks() -> i32;
    fn gc_ble_latency_params(out: *mut c_char, cap: i32) -> i32;
}

/// Whether the `CBCentralManager` hooks are installed. btleplug's delegate hook
/// is not counted here — it goes in on the first connect, since the class does
/// not exist until btleplug builds a manager.
pub fn hooked() -> bool {
    // SAFETY: reads two pointers.
    const CENTRAL_HOOKS: i32 = 0b011;
    unsafe { gc_ble_latency_hooks() & CENTRAL_HOOKS == CENTRAL_HOOKS }
}

/// Installs the hooks. Must run before anything connects, so call it before
/// building the `Manager`.
pub fn install(latency: Latency) {
    // SAFETY: the shim only reads `level` and swizzles two ObjC methods.
    unsafe { gc_ble_latency_install(latency.level()) }
}

/// Whether this macOS build exposes the SPI at all.
pub fn supported() -> bool {
    // SAFETY: pure lookup, no arguments.
    unsafe { gc_ble_latency_supported() != 0 }
}

/// How many times the request was made, and how many times it could not be.
pub fn attempts() -> (i32, i32) {
    // SAFETY: atomic loads.
    unsafe { (gc_ble_latency_applied(), gc_ble_latency_failed()) }
}

/// The last connection parameters CoreBluetooth reported, verbatim.
///
/// Returned as text on purpose: the units are undocumented, so this is a hint
/// worth showing rather than a number worth trusting.
pub fn reported_params() -> Option<String> {
    let mut buf = [0u8; 512];
    // SAFETY: `buf` is valid for `len` bytes and the shim NUL-terminates it.
    let have = unsafe { gc_ble_latency_params(buf.as_mut_ptr().cast(), buf.len() as i32) };
    if have == 0 {
        return None;
    }
    let end = buf.iter().position(|&b| b == 0).unwrap_or(buf.len());
    Some(String::from_utf8_lossy(&buf[..end]).into_owned())
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The SPI is undocumented, so its absence is a real possibility on a future
    /// macOS. This is the check that would catch that.
    #[test]
    fn the_connection_latency_spi_exists() {
        assert!(
            supported(),
            "-[CBCentralManager setDesiredConnectionLatency:forPeripheral:] is gone; \
             --ble-latency can no longer do anything"
        );
    }

    #[test]
    fn install_swizzles_corebluetooth() {
        assert!(!hooked(), "hooks should not be in place before install");
        install(Latency::Low);
        assert!(hooked(), "swizzling CBCentralManager failed");

        // Idempotent: installing twice must not chain the hook onto itself,
        // which would recurse forever on the first connect.
        install(Latency::Low);
        assert!(hooked());

        // Nothing has connected, so no request can have been made yet.
        assert_eq!(attempts(), (0, 0));
        assert_eq!(reported_params(), None);
    }

    #[test]
    fn system_asks_for_nothing() {
        assert_eq!(Latency::System.level(), -1);
        assert_eq!(Latency::Low.level(), 0);
    }
}
