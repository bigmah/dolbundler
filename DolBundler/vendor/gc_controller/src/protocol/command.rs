//! SW2 command frames.
//!
//! Every command — over USB bulk or over the BLE command characteristic — uses
//! the same 8-byte header:
//!
//! ```text
//! 0  cmd        command code
//! 1  type       0x91 request, 0x01 response, 0x00 error
//! 2  interface  0x00 USB, 0x01 BLE
//! 3  subcmd     subcommand code
//! 4  ..
//! 5  len        payload length; total frame is len + 8
//! 6  ..
//! 7  ..
//! 8+ payload
//! ```
//!
//! The frames below are what a real Switch 2 was captured sending. Several have
//! no known purpose; they are sent anyway, because the controller does not start
//! streaming without the full sequence.

/// Transport identifier that goes in header byte 2.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Iface {
    Usb = 0x00,
    Ble = 0x01,
}

/// Rewrites the interface byte of a frame so one table serves both transports.
pub fn with_iface(frame: &[u8], iface: Iface) -> Vec<u8> {
    let mut out = frame.to_vec();
    if out.len() > 2 {
        out[2] = iface as u8;
    }
    out
}

/// A frame's declared total length: payload length (byte 5) plus the 8-byte header.
///
/// The static tables below are already exactly this long, but the controller is
/// strict about it, so we compute rather than trust `slice::len`.
pub fn frame_len(frame: &[u8]) -> usize {
    if frame.len() < 6 {
        return frame.len();
    }
    (frame[5] as usize + 8).min(frame.len())
}

/// The initialisation sequence, in order. Nothing streams until all of it lands.
///
/// Interface bytes are written as USB (`0x00`); call [`with_iface`] for BLE.
pub const INIT_SEQUENCE: &[&[u8]] = &[
    // Unknown purpose.
    &[0x07, 0x91, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00],
    // Set feature output bit mask.
    &[0x0c, 0x91, 0x00, 0x02, 0x00, 0x04, 0x00, 0x00, 0x27, 0x00, 0x00, 0x00],
    // Unknown purpose.
    &[0x11, 0x91, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00],
    // Set rumble data. Purpose unconfirmed — it appears in every capture of the
    // console's own init, so we mirror it without claiming to know what it does.
    &[
        0x0a, 0x91, 0x00, 0x08, 0x00, 0x14, 0x00, 0x00, //
        0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, //
        0xff, 0x35, 0x00, 0x46, 0x00, 0x00, 0x00, 0x00, //
        0x00, 0x00, 0x00, 0x00,
    ],
    // Enable feature output bits.
    &[0x0c, 0x91, 0x00, 0x04, 0x00, 0x04, 0x00, 0x00, 0x27, 0x00, 0x00, 0x00],
    // Unknown purpose.
    &[0x01, 0x91, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00],
    // Enable rumble.
    &[0x01, 0x91, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00],
    // Enable grip buttons on the charging grip.
    &[0x08, 0x91, 0x00, 0x02, 0x00, 0x04, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00],
    // Set report format to report 0x05.
    &[0x03, 0x91, 0x00, 0x0a, 0x00, 0x04, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00],
    // Start output.
    &[
        0x03, 0x91, 0x00, 0x0d, 0x00, 0x08, 0x00, 0x00, //
        0x01, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    ],
];

/// Builds an SPI flash read for `address`. The reply carries 0x40 bytes of data
/// starting at offset 0x10 of the response frame.
pub fn flash_read(address: u32, iface: Iface) -> [u8; 16] {
    let a = address.to_le_bytes();
    [
        0x02, 0x91, iface as u8, 0x01, 0x00, 0x08, 0x00, 0x00, //
        0x00, 0x00, 0x00, 0x00, a[0], a[1], a[2], a[3],
    ]
}

/// Sets the player-indicator LEDs. `mask` is one bit per LED, so player 1 is `0x01`.
///
/// Sent as the bitmask subcommand (`0x09/0x07`) rather than the single-byte
/// `setPlayer1` variant, which is documented as equivalent but is not honoured
/// on a cold controller.
pub fn set_player_leds(mask: u8, iface: Iface) -> [u8; 16] {
    [
        0x09, 0x91, iface as u8, 0x07, 0x00, 0x08, 0x00, 0x00, //
        mask, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    ]
}

/// Byte 1 of a response frame when the controller accepted the command.
pub const RSP_TYPE_OK: u8 = 0x01;

/// True if `rsp` looks like a successful reply to `req`.
///
/// Matching on cmd + subcmd matters: a vibration ack can arrive more than 300 ms
/// late, and without matching it would satisfy the *next* command's wait.
pub fn response_matches(req: &[u8], rsp: &[u8]) -> bool {
    rsp.len() >= 4 && req.len() >= 4 && rsp[0] == req[0] && rsp[3] == req[3] && rsp[1] == RSP_TYPE_OK
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn init_frames_are_exactly_their_declared_length() {
        for frame in INIT_SEQUENCE {
            assert_eq!(frame_len(frame), frame.len(), "bad frame {frame:02x?}");
        }
    }

    #[test]
    fn flash_read_encodes_address_little_endian() {
        let f = flash_read(0x0001_3140, Iface::Usb);
        assert_eq!(&f[12..16], &[0x40, 0x31, 0x01, 0x00]);
    }

    #[test]
    fn with_iface_rewrites_only_the_interface_byte() {
        let ble = with_iface(INIT_SEQUENCE[0], Iface::Ble);
        assert_eq!(ble[2], 0x01);
        assert_eq!(ble[0], INIT_SEQUENCE[0][0]);
        assert_eq!(ble[3], INIT_SEQUENCE[0][3]);
    }

    #[test]
    fn response_must_match_command_and_subcommand() {
        let req = [0x03, 0x91, 0x00, 0x0d];
        assert!(response_matches(&req, &[0x03, 0x01, 0x01, 0x0d, 0x10, 0x78]));
        // Right command, wrong subcommand — a stale ack from an earlier write.
        assert!(!response_matches(&req, &[0x03, 0x01, 0x01, 0x0a, 0x10, 0x78]));
        // Error type.
        assert!(!response_matches(&req, &[0x03, 0x00, 0x01, 0x0d]));
    }
}
