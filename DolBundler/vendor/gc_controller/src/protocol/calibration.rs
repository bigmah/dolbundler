//! Factory and user calibration, read out of the controller's SPI flash.
//!
//! Without this the sticks sit off-centre and never reach the corners, and the
//! triggers report a non-zero resting value. Dolphin notices both immediately.

use super::state::{GcState, RawState, bit};

/// Flash blocks worth reading. Each read returns 0x40 bytes.
pub mod addr {
    /// Serial number, 17 bytes at +2.
    pub const SERIAL: u32 = 0x0001_3000;
    /// Factory left-stick calibration at +0x28.
    pub const LEFT_STICK: u32 = 0x0001_3080;
    /// Factory right-stick calibration at +0x28.
    pub const RIGHT_STICK: u32 = 0x0001_30C0;
    /// GameCube trigger zero points: left at +0, right at +1.
    pub const TRIGGER_ZEROS: u32 = 0x0001_3140;
    /// User left-stick calibration, guarded by a magic prefix.
    pub const USER_LEFT_STICK: u32 = 0x001F_C040;
    /// User right-stick calibration, guarded by a magic prefix.
    pub const USER_RIGHT_STICK: u32 = 0x001F_C080;
}

/// Offset of the factory stick block inside a flash read.
const FACTORY_STICK_OFFSET: usize = 0x28;
/// User calibration blocks start with this magic, then the same 9-byte layout.
const USER_MAGIC: [u8; 2] = [0xB2, 0xA1];

/// A trigger reads 0xE8 at full pull regardless of where its zero sits.
const TRIGGER_FULL: f32 = 232.0;

/// Per-axis calibration: a centre and an asymmetric range either side of it.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct AxisCal {
    pub neutral: u16,
    pub max: u16,
    pub min: u16,
}

impl AxisCal {
    /// Calibration is only usable if every field was populated.
    fn is_valid(&self) -> bool {
        self.neutral != 0 && self.min != 0 && self.max != 0
    }

    /// Maps a raw axis reading to `-1.0..=1.0`.
    fn apply(&self, raw: u16) -> f32 {
        if !self.is_valid() {
            // Uncalibrated fallback: assume a symmetric 12-bit range centred on
            // 2048. Dividing by 2047 rather than 2048 keeps the centre exact
            // while still letting the top of the range reach full scale.
            return ((f32::from(raw) - 2048.0) / 2047.0).clamp(-1.0, 1.0);
        }
        let v = f32::from(raw) - f32::from(self.neutral);
        let scaled = if v < 0.0 {
            v / f32::from(self.min)
        } else {
            v / f32::from(self.max)
        };
        scaled.clamp(-1.0, 1.0)
    }
}

/// Both axes of one stick.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct StickCal {
    pub x: AxisCal,
    pub y: AxisCal,
}

impl StickCal {
    /// Parses the packed 9-byte calibration blob: three 12-bit pairs, in the
    /// order neutral, max, min.
    pub fn parse(d: &[u8]) -> Option<Self> {
        let d = d.get(..9)?;
        let pair = |a: u8, b: u8, c: u8| {
            (
                u16::from(a) | (u16::from(b & 0x0F) << 8),
                u16::from(b >> 4) | (u16::from(c) << 4),
            )
        };
        let (nx, ny) = pair(d[0], d[1], d[2]);
        let (mx, my) = pair(d[3], d[4], d[5]);
        let (ix, iy) = pair(d[6], d[7], d[8]);
        Some(Self {
            x: AxisCal { neutral: nx, max: mx, min: ix },
            y: AxisCal { neutral: ny, max: my, min: iy },
        })
    }
}

/// Everything we read out of flash for one controller.
#[derive(Clone, Debug, Default)]
pub struct Calibration {
    pub serial: Option<String>,
    pub left: StickCal,
    pub right: StickCal,
    pub trigger_l_zero: u8,
    pub trigger_r_zero: u8,
    /// Inverts each stick's Y axis so up is positive.
    pub invert_left_y: bool,
    pub invert_right_y: bool,
    /// Travel below this fraction reads as fully released.
    ///
    /// A trigger rests a count or two above its stored zero point, so without
    /// this the controller reports a permanent ~1% pull and Dolphin sees a
    /// trigger that is never quite let go.
    pub trigger_deadzone: f32,
}

impl Calibration {
    /// Feeds one flash block in. `block` is the 0x40 bytes of payload.
    ///
    /// Unreadable or absent blocks are simply skipped — the driver still runs,
    /// just on the uncalibrated fallback path.
    pub fn absorb(&mut self, address: u32, block: &[u8]) {
        match address {
            addr::SERIAL => {
                if let Some(raw) = block.get(2..0x13) {
                    let end = raw.iter().position(|&b| b == 0).unwrap_or(raw.len());
                    let s = String::from_utf8_lossy(&raw[..end]).trim().to_string();
                    if !s.is_empty() {
                        self.serial = Some(s);
                    }
                }
            }
            addr::LEFT_STICK => {
                if let Some(c) = block.get(FACTORY_STICK_OFFSET..).and_then(StickCal::parse) {
                    self.left = c;
                }
            }
            addr::RIGHT_STICK => {
                if let Some(c) = block.get(FACTORY_STICK_OFFSET..).and_then(StickCal::parse) {
                    self.right = c;
                }
            }
            addr::TRIGGER_ZEROS => {
                if block.len() >= 2 {
                    self.trigger_l_zero = block[0];
                    self.trigger_r_zero = block[1];
                }
            }
            // User calibration overrides the factory values, but only when the
            // magic is present — an unwritten block is all zeroes or all 0xFF.
            addr::USER_LEFT_STICK => {
                if block.starts_with(&USER_MAGIC)
                    && let Some(c) = block.get(2..).and_then(StickCal::parse)
                {
                    self.left = c;
                }
            }
            addr::USER_RIGHT_STICK => {
                if block.starts_with(&USER_MAGIC)
                    && let Some(c) = block.get(2..).and_then(StickCal::parse)
                {
                    self.right = c;
                }
            }
            _ => {}
        }
    }

    /// Every block the driver reads at startup, in order.
    pub const BLOCKS: &'static [u32] = &[
        addr::SERIAL,
        addr::LEFT_STICK,
        addr::RIGHT_STICK,
        addr::TRIGGER_ZEROS,
        addr::USER_LEFT_STICK,
        addr::USER_RIGHT_STICK,
    ];

    /// Maps an analog trigger byte to `0.0..=1.0`, taking out its resting offset.
    ///
    /// The deadzone is rescaled rather than subtracted, so a fully pulled
    /// trigger still reaches exactly 1.0.
    fn trigger(zero: u8, deadzone: f32, raw: u8) -> f32 {
        let zero = f32::from(zero);
        let span = TRIGGER_FULL - zero;
        if span <= 0.0 {
            return 0.0;
        }
        let v = ((f32::from(raw) - zero) / span).clamp(0.0, 1.0);
        let dz = deadzone.clamp(0.0, 0.9);
        ((v - dz) / (1.0 - dz)).clamp(0.0, 1.0)
    }

    /// Turns a raw report into the calibrated GameCube view.
    pub fn apply(&self, raw: &RawState) -> GcState {
        let ly = self.left.y.apply(raw.left_stick.1);
        let ry = self.right.y.apply(raw.right_stick.1);
        GcState {
            a: raw.pressed(bit::A),
            b: raw.pressed(bit::B),
            x: raw.pressed(bit::X),
            y: raw.pressed(bit::Y),
            z: raw.pressed(bit::Z),
            start: raw.pressed(bit::START),
            l: raw.pressed(bit::L_CLICK),
            r: raw.pressed(bit::R_CLICK),
            d_up: raw.pressed(bit::DPAD_UP),
            d_down: raw.pressed(bit::DPAD_DOWN),
            d_left: raw.pressed(bit::DPAD_LEFT),
            d_right: raw.pressed(bit::DPAD_RIGHT),
            home: raw.pressed(bit::HOME),
            capture: raw.pressed(bit::CAPTURE),
            c_button: raw.pressed(bit::C),
            main: (
                self.left.x.apply(raw.left_stick.0),
                if self.invert_left_y { -ly } else { ly },
            ),
            c_stick: (
                self.right.x.apply(raw.right_stick.0),
                if self.invert_right_y { -ry } else { ry },
            ),
            trigger_l: Self::trigger(self.trigger_l_zero, self.trigger_deadzone, raw.trigger_l),
            trigger_r: Self::trigger(self.trigger_r_zero, self.trigger_deadzone, raw.trigger_r),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn cal() -> AxisCal {
        AxisCal { neutral: 2000, max: 1000, min: 800 }
    }

    #[test]
    fn axis_maps_neutral_to_zero_and_extremes_to_full_scale() {
        let c = cal();
        assert_eq!(c.apply(2000), 0.0);
        assert_eq!(c.apply(3000), 1.0);
        assert_eq!(c.apply(1200), -1.0);
    }

    #[test]
    fn axis_respects_asymmetric_ranges() {
        let c = cal();
        // Half of the positive range, and half of the (different) negative one.
        assert!((c.apply(2500) - 0.5).abs() < 1e-6);
        assert!((c.apply(1600) + 0.5).abs() < 1e-6);
    }

    #[test]
    fn axis_clamps_beyond_the_calibrated_range() {
        let c = cal();
        assert_eq!(c.apply(4095), 1.0);
        assert_eq!(c.apply(0), -1.0);
    }

    #[test]
    fn uncalibrated_axis_falls_back_to_the_symmetric_range() {
        let c = AxisCal::default();
        assert_eq!(c.apply(2048), 0.0);
        assert_eq!(c.apply(0), -1.0);
        assert_eq!(c.apply(4095), 1.0);
    }

    #[test]
    fn trigger_subtracts_its_resting_offset() {
        // A trigger resting at 0x20 must read 0.0 there, not 0x20/255.
        assert_eq!(Calibration::trigger(0x20, 0.0, 0x20), 0.0);
        assert_eq!(Calibration::trigger(0x20, 0.0, 0xE8), 1.0);
        assert_eq!(Calibration::trigger(0x20, 0.0, 0x10), 0.0, "below zero clamps");
        assert_eq!(Calibration::trigger(0x20, 0.0, 0xFF), 1.0, "above full clamps");
        let mid = Calibration::trigger(0x20, 0.0, 0x84);
        assert!((mid - 0.5).abs() < 0.01, "midpoint was {mid}");
    }

    #[test]
    fn trigger_deadzone_silences_rest_noise_but_keeps_full_travel() {
        // Two raw counts above the stored zero: real hardware at rest.
        let resting = Calibration::trigger(0x1F, 0.0, 0x21);
        assert!(resting > 0.0, "precondition: rest reads {resting}, not 0");
        assert_eq!(Calibration::trigger(0x1F, 0.02, 0x21), 0.0);
        // A full pull must still reach the top of the range.
        assert_eq!(Calibration::trigger(0x1F, 0.02, 0xE8), 1.0);
    }

    #[test]
    fn trigger_deadzone_rescales_rather_than_shifting() {
        // Half travel stays near half, not half-minus-deadzone.
        let v = Calibration::trigger(0x00, 0.10, 116);
        assert!((v - 0.444).abs() < 0.01, "got {v}");
    }

    #[test]
    fn stick_cal_unpacks_three_12_bit_pairs() {
        // neutral (0x801, 0x802), max (0x101, 0x102), min (0x201, 0x202)
        let blob = [
            0x01, 0x28, 0x80, //
            0x01, 0x21, 0x10, //
            0x01, 0x22, 0x20,
        ];
        let c = StickCal::parse(&blob).unwrap();
        assert_eq!(c.x, AxisCal { neutral: 0x801, max: 0x101, min: 0x201 });
        assert_eq!(c.y, AxisCal { neutral: 0x802, max: 0x102, min: 0x202 });
    }

    #[test]
    fn user_calibration_overrides_factory_only_with_the_magic() {
        let mut c = Calibration::default();
        let mut factory = vec![0u8; 0x40];
        factory[FACTORY_STICK_OFFSET..FACTORY_STICK_OFFSET + 9]
            .copy_from_slice(&[0x01, 0x28, 0x80, 0x01, 0x21, 0x10, 0x01, 0x22, 0x20]);
        c.absorb(addr::LEFT_STICK, &factory);
        assert_eq!(c.left.x.neutral, 0x801);

        // No magic: ignored.
        let mut blank = vec![0u8; 0x40];
        blank[2..11].copy_from_slice(&[0x05, 0x28, 0x80, 0x01, 0x21, 0x10, 0x01, 0x22, 0x20]);
        c.absorb(addr::USER_LEFT_STICK, &blank);
        assert_eq!(c.left.x.neutral, 0x801, "unwritten block must not override");

        // With magic: applied.
        blank[0..2].copy_from_slice(&USER_MAGIC);
        c.absorb(addr::USER_LEFT_STICK, &blank);
        assert_eq!(c.left.x.neutral, 0x805);
    }

    #[test]
    fn serial_is_trimmed_at_the_nul() {
        let mut c = Calibration::default();
        let mut blk = vec![0u8; 0x40];
        blk[2..8].copy_from_slice(b"ABC123");
        c.absorb(addr::SERIAL, &blk);
        assert_eq!(c.serial.as_deref(), Some("ABC123"));
    }

    #[test]
    fn y_inversion_flips_only_the_requested_stick() {
        let raw = RawState {
            left_stick: (2048, 4095),
            right_stick: (2048, 4095),
            ..Default::default()
        };
        let mut c = Calibration::default();
        assert_eq!(c.apply(&raw).main.1, 1.0);
        c.invert_left_y = true;
        let s = c.apply(&raw);
        assert_eq!(s.main.1, -1.0);
        assert_eq!(s.c_stick.1, 1.0);
    }
}
