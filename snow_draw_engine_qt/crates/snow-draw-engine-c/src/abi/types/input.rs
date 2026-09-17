use super::*;

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SnowPointerEventType {
    Down = 0,
    Move = 1,
    Up = 2,
    Cancel = 3,
    Enter = 4,
    Leave = 5,
    DoubleClick = 6,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SnowPointerDevice {
    Mouse = 0,
    Touch = 1,
    Pen = 2,
    Unknown = 3,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SnowPointerButton {
    None = 0,
    Primary = 1,
    Secondary = 2,
    Middle = 3,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SnowInputEventKind {
    Pointer = 0,
    Wheel = 1,
    Key = 2,
    FocusLost = 3,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SnowWheelDeltaKind {
    Pixel = 0,
    Angle = 1,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SnowKeyEventType {
    KeyDown = 0,
    KeyUp = 1,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SnowKeyCode {
    Unknown = 0,
    Space = 1,
    Escape = 2,
    ArrowUp = 3,
    ArrowDown = 4,
    ArrowLeft = 5,
    ArrowRight = 6,
    Character = 7,
    Backspace = 8,
    Delete = 9,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SnowPointerEvent {
    pub pointer_id: u32,
    pub event_type: SnowPointerEventType,
    pub device: SnowPointerDevice,
    pub position_x: f64,
    pub position_y: f64,
    pub button: SnowPointerButton,
    pub buttons: u8,
    pub reserved0: [u8; 3],
    pub modifiers: SnowModifiers,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SnowWheelEvent {
    pub position_x: f64,
    pub position_y: f64,
    pub delta_x: f64,
    pub delta_y: f64,
    pub delta_kind: SnowWheelDeltaKind,
    pub modifiers: SnowModifiers,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SnowKeyEvent {
    pub event_type: SnowKeyEventType,
    pub key_code: SnowKeyCode,
    pub codepoint: u32,
    pub modifiers: SnowModifiers,
    pub repeat: u8,
    pub reserved0: [u8; 3],
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SnowInputEvent {
    pub kind: SnowInputEventKind,
    pub pointer: SnowPointerEvent,
    pub wheel: SnowWheelEvent,
    pub key: SnowKeyEvent,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SnowPointerCaptureCommandKind {
    NoChange = 0,
    Capture = 1,
    Release = 2,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SnowCursorCommandKind {
    NoChange = 0,
    Set = 1,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SnowCursorStyle {
    Default = 0,
    Crosshair = 1,
    Grab = 2,
    Grabbing = 3,
    Move = 4,
    ResizeHorizontal = 5,
    ResizeVertical = 6,
    ResizeNwSe = 7,
    ResizeNeSw = 8,
    NotAllowed = 9,
    Text = 10,
    CornerRadius = 11,
    Hidden = 12,
    Stroke = 13,
    Eraser = 14,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SnowInteractionOutput {
    pub consumed: u8,
    pub reserved0: [u8; 3],
    pub capture_kind: SnowPointerCaptureCommandKind,
    pub capture_pointer_id: u32,
    pub cursor_kind: SnowCursorCommandKind,
    pub cursor_style: SnowCursorStyle,
}
