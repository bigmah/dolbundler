//! Decoding of input report 0x05.
//!
//! The wire layout is identical on both transports; only the leading offset
//! differs. Over USB the report arrives as 64 bytes with a one-byte prefix
//! ahead of the body; over BLE the notification is the 63-byte body itself.
//! Everything below is expressed relative to the body, so each transport just
//! declares where its body starts.

use super::state::RawState;

/// Offset of the report body within a USB bulk read.
pub const USB_BODY_OFFSET: usize = 1;
/// Offset of the report body within a BLE notification.
pub const BLE_BODY_OFFSET: usize = 0;

/// Shortest body we can decode: through the trigger bytes at 60 and 61.
const MIN_BODY: usize = 62;

/// Body-relative field offsets.
const OFF_BUTTONS: usize = 4;
const OFF_LEFT_STICK: usize = 10;
const OFF_RIGHT_STICK: usize = 13;
const OFF_TRIGGER_L: usize = 60;
const OFF_TRIGGER_R: usize = 61;

/// Unpacks two 12-bit axes from three bytes.
fn unpack_stick(d: &[u8], at: usize) -> (u16, u16) {
    let x = u16::from(d[at]) | (u16::from(d[at + 1] & 0x0F) << 8);
    let y = u16::from(d[at + 1] >> 4) | (u16::from(d[at + 2]) << 4);
    (x, y)
}

/// Decodes a report body starting at `base`. Returns `None` if the frame is short.
pub fn decode(data: &[u8], base: usize) -> Option<RawState> {
    let body = data.get(base..)?;
    if body.len() < MIN_BODY {
        return None;
    }
    Some(RawState {
        buttons: u32::from_le_bytes([
            body[OFF_BUTTONS],
            body[OFF_BUTTONS + 1],
            body[OFF_BUTTONS + 2],
            body[OFF_BUTTONS + 3],
        ]),
        left_stick: unpack_stick(body, OFF_LEFT_STICK),
        right_stick: unpack_stick(body, OFF_RIGHT_STICK),
        trigger_l: body[OFF_TRIGGER_L],
        trigger_r: body[OFF_TRIGGER_R],
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::protocol::state::bit;

    /// Builds a synthetic 63-byte body with known field values.
    fn body(buttons: u32, l: (u16, u16), r: (u16, u16), tl: u8, tr: u8) -> Vec<u8> {
        let mut b = vec![0u8; 63];
        b[OFF_BUTTONS..OFF_BUTTONS + 4].copy_from_slice(&buttons.to_le_bytes());
        let pack = |(x, y): (u16, u16)| {
            [
                (x & 0xFF) as u8,
                (((y & 0x0F) << 4) | (x >> 8)) as u8,
                (y >> 4) as u8,
            ]
        };
        b[OFF_LEFT_STICK..OFF_LEFT_STICK + 3].copy_from_slice(&pack(l));
        b[OFF_RIGHT_STICK..OFF_RIGHT_STICK + 3].copy_from_slice(&pack(r));
        b[OFF_TRIGGER_L] = tl;
        b[OFF_TRIGGER_R] = tr;
        b
    }

    #[test]
    fn round_trips_packed_12_bit_sticks() {
        let b = body(0, (0x7FF, 0x123), (0xABC, 0x001), 0, 0);
        let s = decode(&b, BLE_BODY_OFFSET).unwrap();
        assert_eq!(s.left_stick, (0x7FF, 0x123));
        assert_eq!(s.right_stick, (0xABC, 0x001));
    }

    #[test]
    fn decodes_buttons_and_triggers() {
        let bits = (1 << bit::A) | (1 << bit::Z) | (1 << bit::DPAD_LEFT);
        let b = body(bits, (0, 0), (0, 0), 0x20, 0xE8);
        let s = decode(&b, BLE_BODY_OFFSET).unwrap();
        assert!(s.pressed(bit::A) && s.pressed(bit::Z) && s.pressed(bit::DPAD_LEFT));
        assert!(!s.pressed(bit::B) && !s.pressed(bit::START));
        assert_eq!((s.trigger_l, s.trigger_r), (0x20, 0xE8));
    }

    #[test]
    fn usb_offset_skips_the_leading_prefix_byte() {
        let mut usb = vec![0xAAu8]; // prefix
        usb.extend_from_slice(&body(1 << bit::START, (0x111, 0x222), (0, 0), 1, 2));
        let s = decode(&usb, USB_BODY_OFFSET).unwrap();
        assert!(s.pressed(bit::START));
        assert_eq!(s.left_stick, (0x111, 0x222));
    }

    #[test]
    fn rejects_short_frames() {
        assert!(decode(&[0u8; 40], BLE_BODY_OFFSET).is_none());
        assert!(decode(&[0u8; 62], USB_BODY_OFFSET).is_none());
        assert!(decode(&[0u8; 63], USB_BODY_OFFSET).is_some());
    }
}
