use snow_draw_engine_core::{Point, Vector2};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PointerEventType {
    Down,
    Move,
    Up,
    Cancel,
    Enter,
    Leave,
    DoubleClick,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PointerDevice {
    Mouse,
    Touch,
    Pen,
    Unknown,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PointerButton {
    Primary,
    Secondary,
    Middle,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PointerButtons(pub u8);

impl PointerButtons {
    pub const PRIMARY: u8 = 0b0000_0001;
    pub const SECONDARY: u8 = 0b0000_0010;

    pub fn contains(self, bit: u8) -> bool {
        self.0 & bit != 0
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Modifiers {
    pub ctrl: bool,
    pub shift: bool,
    pub alt: bool,
    pub meta: bool,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct PointerEvent {
    pub pointer_id: u32,
    pub event_type: PointerEventType,
    pub device: PointerDevice,
    pub position: Point<f64>,
    pub button: Option<PointerButton>,
    pub buttons: PointerButtons,
    pub modifiers: Modifiers,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct WheelEvent {
    pub position: Point<f64>,
    pub delta: Vector2<f64>,
    pub delta_kind: WheelDeltaKind,
    pub modifiers: Modifiers,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WheelDeltaKind {
    Pixel,
    Angle,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum KeyEventType {
    KeyDown,
    KeyUp,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum KeyCode {
    Space,
    Escape,
    Backspace,
    Delete,
    ArrowUp,
    ArrowDown,
    ArrowLeft,
    ArrowRight,
    Character(char),
    Unknown,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct KeyEvent {
    pub event_type: KeyEventType,
    pub key_code: KeyCode,
    pub modifiers: Modifiers,
    pub repeat: bool,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum InputEvent {
    Pointer(PointerEvent),
    Wheel(WheelEvent),
    Key(KeyEvent),
    FocusLost,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct InteractionOutput {
    pub consumed: bool,
    pub capture: PointerCaptureCommand,
    pub cursor: CursorCommand,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum PointerCaptureCommand {
    #[default]
    NoChange,
    Capture(u32),
    Release,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum CursorCommand {
    #[default]
    NoChange,
    Set(CursorStyle),
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum CursorStyle {
    #[default]
    Default,
    Crosshair,
    CornerRadius,
    Grab,
    Grabbing,
    Move,
    ResizeHorizontal,
    ResizeVertical,
    ResizeNwSe,
    ResizeNeSw,
    Text,
    NotAllowed,
    Hidden,
}
