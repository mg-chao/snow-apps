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

/// Owned display geometry supplied by a capture session. No native objects cross threads.
#[derive(Clone, Debug, PartialEq)]
pub struct DisplayGeometry {
    pub display_id: u32,
    pub x: f64,
    pub y: f64,
    pub width: f64,
    pub height: f64,
    pub pixel_width: u32,
    pub pixel_height: u32,
}
impl DisplayGeometry {
    pub fn valid(&self) -> bool {
        [
            self.x,
            self.y,
            self.width,
            self.height,
            self.x + self.width,
            self.y + self.height,
        ]
        .iter()
        .all(|n| n.is_finite() && *n >= f64::from(i32::MIN) && *n <= f64::from(i32::MAX))
            && self.width > 0.0
            && self.height > 0.0
            && self.pixel_width > 0
            && self.pixel_height > 0
            && self.pixel_width <= i32::MAX as u32
            && self.pixel_height <= i32::MAX as u32
            && self.x + f64::from(self.pixel_width) <= f64::from(i32::MAX)
            && self.y + f64::from(self.pixel_height) <= f64::from(i32::MAX)
            && (!cfg!(target_os = "macos") || self.display_id != 0)
    }
}

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
