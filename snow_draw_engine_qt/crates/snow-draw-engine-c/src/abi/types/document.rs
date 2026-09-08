use super::*;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct SnowArrowTextLayoutRequest {
    pub info: SnowTextElementInfo,
    pub style: SnowTextStyle,
    pub key: u64,
    pub max_width: f64,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct SnowArrowTextLayoutResult {
    pub text_id: SnowElementId,
    pub key: u64,
    pub size: SnowTextLayoutSize,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SnowElementId {
    pub index: u32,
    pub generation: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct SnowTextLayoutSize {
    /// Exact host-renderer measured text width.
    pub width: f64,
    /// Exact host-renderer measured text height.
    pub height: f64,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct SnowTextLayoutOverride {
    /// Text element that this exact host-renderer layout applies to.
    pub id: SnowElementId,
    pub size: SnowTextLayoutSize,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SnowTextCommitDraft {
    pub element_id: SnowElementId,
    pub arrow_id: SnowElementId,
    pub has_existing_element: u8,
    pub auto_resize: u8,
    pub update_default_style: u8,
    pub reserved0: [u8; 5],
    pub center_x: f64,
    pub center_y: f64,
    pub text_utf8: *const std::ffi::c_char,
    pub text_utf8_len: u32,
    pub reserved1: u32,
    pub measured_layout: SnowTextLayoutSize,
    pub style: SnowTextStyle,
}

impl Default for SnowTextCommitDraft {
    fn default() -> Self {
        Self {
            element_id: SnowElementId::default(),
            arrow_id: SnowElementId::default(),
            has_existing_element: 0,
            auto_resize: 0,
            update_default_style: 0,
            reserved0: [0; 5],
            center_x: 0.0,
            center_y: 0.0,
            text_utf8: std::ptr::null(),
            text_utf8_len: 0,
            reserved1: 0,
            measured_layout: SnowTextLayoutSize::default(),
            style: SnowTextStyle::default(),
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SnowActiveTextDraftPresentation {
    pub element_id: SnowElementId,
    pub arrow_id: SnowElementId,
    pub has_existing_element: u8,
    pub auto_resize: u8,
    pub reserved0: [u8; 6],
    pub center_x: f64,
    pub center_y: f64,
    pub width: f64,
    pub height: f64,
    pub rotation: f64,
    pub text_utf8: *const std::ffi::c_char,
    pub text_utf8_len: u32,
    pub reserved1: u32,
    pub style: SnowTextStyle,
}

impl Default for SnowActiveTextDraftPresentation {
    fn default() -> Self {
        Self {
            element_id: SnowElementId::default(),
            arrow_id: SnowElementId::default(),
            has_existing_element: 0,
            auto_resize: 0,
            reserved0: [0; 6],
            center_x: 0.0,
            center_y: 0.0,
            width: 0.0,
            height: 0.0,
            rotation: 0.0,
            text_utf8: std::ptr::null(),
            text_utf8_len: 0,
            reserved1: 0,
            style: SnowTextStyle::default(),
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SnowTextElementInfo {
    pub id: SnowElementId,
    pub arrow_id: SnowElementId,
    pub arrow_width: f64,
    pub center_x: f64,
    pub center_y: f64,
    pub width: f64,
    pub height: f64,
    pub rotation: f64,
    pub font_size: f64,
    pub text_utf8_len: u32,
    pub text_truncated: u8,
    pub auto_resize: u8,
    /// Nonzero asks the host to measure natural unwrapped width; zero asks for
    /// wrapped height using `width`.
    pub measure_natural_width: u8,
    pub reserved0: [u8; 1],
    pub text_utf8: [std::ffi::c_char; SNOW_TEXT_UTF8_CAPACITY],
    pub font_family_utf8_len: u32,
    pub font_family_truncated: u8,
    pub reserved1: [u8; 3],
    pub font_family_utf8: [std::ffi::c_char; SNOW_FONT_FAMILY_UTF8_CAPACITY],
}

impl Default for SnowTextElementInfo {
    fn default() -> Self {
        Self {
            id: SnowElementId::default(),
            arrow_id: SnowElementId::default(),
            arrow_width: 0.0,
            center_x: 0.0,
            center_y: 0.0,
            width: 0.0,
            height: 0.0,
            rotation: 0.0,
            font_size: 0.0,
            text_utf8_len: 0,
            text_truncated: 0,
            auto_resize: 0,
            measure_natural_width: 0,
            reserved0: [0; 1],
            text_utf8: [0; SNOW_TEXT_UTF8_CAPACITY],
            font_family_utf8_len: 0,
            font_family_truncated: 0,
            reserved1: [0; 3],
            font_family_utf8: [0; SNOW_FONT_FAMILY_UTF8_CAPACITY],
        }
    }
}
