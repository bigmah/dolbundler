//! Nintendo "SW2" protocol shared by the Switch 2 controller family.
//!
//! The NSO GameCube controller (VID 0x057E, PID 0x2073) does not speak plain
//! HID for its useful data. Both transports carry the same command frames and
//! the same input report, so everything that is not framing lives here.
//!
//! None of this is documented by Nintendo. The framing, flash addresses and
//! report layout come from prior open-source reverse-engineering of the Switch 2
//! controller family, confirmed against this unit.

pub mod calibration;
pub mod command;
pub mod report;
pub mod state;

/// USB vendor ID for all Nintendo controllers.
pub const VID_NINTENDO: u16 = 0x057E;
/// USB product ID for the NSO GameCube controller.
pub const PID_NSO_GAMECUBE: u16 = 0x2073;
