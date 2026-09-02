//! The canonical controller state, and the GameCube view of it.

/// Bit positions in the report's 32-bit button field, GameCube layout.
///
/// The right side carries both `R` (the click at the bottom of the analog pull)
/// and `Z` (the digital shoulder above it). The left side mirrors those bits,
/// but real GameCube hardware has no `ZL`, so bit 23 never fires here.
pub mod bit {
    pub const Y: u32 = 0;
    pub const X: u32 = 1;
    pub const B: u32 = 2;
    pub const A: u32 = 3;
    pub const R_CLICK: u32 = 6;
    pub const Z: u32 = 7;
    pub const START: u32 = 9;
    pub const HOME: u32 = 12;
    pub const CAPTURE: u32 = 13;
    pub const C: u32 = 14;
    pub const DPAD_DOWN: u32 = 16;
    pub const DPAD_UP: u32 = 17;
    pub const DPAD_RIGHT: u32 = 18;
    pub const DPAD_LEFT: u32 = 19;
    pub const L_CLICK: u32 = 22;
    /// Present in the shared Switch 2 button field but unreachable on GameCube
    /// hardware, which has no ZL. Recorded so the layout stays complete.
    #[allow(dead_code)]
    pub const ZL: u32 = 23;
}

/// One decoded input report, still in the controller's own units.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct RawState {
    /// The 32-bit button field; index it with [`bit`].
    pub buttons: u32,
    /// Left ("main") stick, 12-bit unsigned, as reported.
    pub left_stick: (u16, u16),
    /// Right ("C") stick, 12-bit unsigned, as reported.
    pub right_stick: (u16, u16),
    /// Analog trigger travel before the zero point is subtracted.
    pub trigger_l: u8,
    pub trigger_r: u8,
}

impl RawState {
    pub fn pressed(&self, bit: u32) -> bool {
        self.buttons & (1 << bit) != 0
    }
}

/// Controller state after calibration, in the units a consumer wants.
///
/// Sticks are `-1.0..=1.0` with up and right positive; triggers are `0.0..=1.0`.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct GcState {
    pub a: bool,
    pub b: bool,
    pub x: bool,
    pub y: bool,
    pub z: bool,
    pub start: bool,
    /// The click at the bottom of the L trigger's travel.
    pub l: bool,
    /// The click at the bottom of the R trigger's travel.
    pub r: bool,
    pub d_up: bool,
    pub d_down: bool,
    pub d_left: bool,
    pub d_right: bool,
    /// Not a GameCube button — surfaced so callers can bind it (Dolphin's menu, etc).
    pub home: bool,
    pub capture: bool,
    pub c_button: bool,
    pub main: (f32, f32),
    pub c_stick: (f32, f32),
    pub trigger_l: f32,
    pub trigger_r: f32,
}
