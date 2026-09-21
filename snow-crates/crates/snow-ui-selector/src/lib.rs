//! Native window and accessibility selection. Provider objects never cross worker threads.
mod query;
pub use query::{QueryControl, QueryResult, StopReason};
#[cfg(windows)]
mod windows;
#[cfg(windows)]
pub use windows::{ElementRegionService, enable_high_dpi_support};
#[cfg(target_os = "macos")]
mod macos;
#[cfg(target_os = "macos")]
pub use macos::{ElementRegionService, accessibility_permission};

pub type SelectorResult<T> = Result<T, Box<dyn std::error::Error + Send + Sync>>;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Point {
    pub x: i32,
    pub y: i32,
    pub display_id: u32,
}
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PixelRect {
    pub left: i32,
    pub top: i32,
    pub right: i32,
    pub bottom: i32,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ElementRect {
    rect: PixelRect,
}
impl ElementRect {
    pub(crate) fn new(rect: impl Into<PixelRect>) -> Self {
        Self { rect: rect.into() }
    }
    pub fn left(&self) -> i32 {
        self.rect.left
    }
    pub fn top(&self) -> i32 {
        self.rect.top
    }
    pub fn right(&self) -> i32 {
        self.rect.right
    }
    pub fn bottom(&self) -> i32 {
        self.rect.bottom
    }
    pub fn width(&self) -> i32 {
        self.rect.right - self.rect.left
    }
    pub fn height(&self) -> i32 {
        self.rect.bottom - self.rect.top
    }
}
#[cfg(windows)]
impl From<::windows::Win32::Foundation::RECT> for PixelRect {
    fn from(r: ::windows::Win32::Foundation::RECT) -> Self {
        Self {
            left: r.left,
            top: r.top,
            right: r.right,
            bottom: r.bottom,
        }
    }
}
#[cfg(windows)]
impl From<PixelRect> for ::windows::Win32::Foundation::RECT {
    fn from(r: PixelRect) -> Self {
        Self {
            left: r.left,
            top: r.top,
            right: r.right,
            bottom: r.bottom,
        }
    }
}
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum HitTestMode {
    #[default]
    UiElement,
    Window,
}
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum AccessibilityBackend {
    #[cfg_attr(windows, default)]
    Uia,
    Msaa,
    #[cfg_attr(not(windows), default)]
    Accessibility,
}
/// Transferable geometry and session metadata; native handles remain worker-local.
#[derive(Clone, Debug, Default)]
pub struct WindowSnapshot {
    #[cfg(windows)]
    pub(crate) windows: Vec<(usize, PixelRect)>,
    #[cfg(target_os = "macos")]
    pub(crate) windows: Vec<macos::WindowInfo>,
    #[cfg(target_os = "macos")]
    pub(crate) displays: Vec<macos::DisplayInfo>,
    #[cfg(target_os = "macos")]
    pub(crate) activation: std::sync::Arc<macos::activation::Session>,
}
