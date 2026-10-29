// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Native images and retained, window-local image previews.

mod config;
pub(crate) mod picture;
mod preview;
mod progress;
mod style;
mod thumbnail;

pub use config::{ImageConfig, ImageConfigPatch, ImageStyleResolver};
pub use picture::{PictureEvent, PictureState, PictureStatus};
pub use preview::{PreviewGroup, PreviewState};
pub use style::{ImageTheme, ImageTokenPatch, ImageTokens};
pub use thumbnail::{Image, ImageEvent, ImageState};

use crate::icons::{IconName, IconRef};
use gpui::{AnyElement, App, Entity, ImageSource, SharedString, StyleRefinement, Window};
use std::rc::Rc;

/// One previewable item. Identity is independent of its source and translated alt text.
#[derive(Clone)]
pub struct PreviewItem {
    pub id: SharedString,
    pub source: ImageSource,
    pub alt: SharedString,
    pub fallback: Option<ImageSource>,
}
impl PreviewItem {
    pub fn new(id: impl Into<SharedString>, source: impl Into<ImageSource>) -> Self {
        Self {
            id: id.into(),
            source: source.into(),
            alt: SharedString::default(),
            fallback: None,
        }
    }
    pub fn alt(mut self, alt: impl Into<SharedString>) -> Self {
        self.alt = alt.into();
        self
    }
    pub fn fallback(mut self, source: impl Into<ImageSource>) -> Self {
        self.fallback = Some(source.into());
        self
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct PreviewTransform {
    pub x: f32,
    pub y: f32,
    pub rotate: f32,
    pub scale: f32,
    pub flip_x: bool,
    pub flip_y: bool,
}
impl Default for PreviewTransform {
    fn default() -> Self {
        Self {
            x: 0.,
            y: 0.,
            rotate: 0.,
            scale: 1.,
            flip_x: false,
            flip_y: false,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PreviewAction {
    FlipY,
    FlipX,
    RotateLeft,
    RotateRight,
    ZoomOut,
    ZoomIn,
    Reset,
    Close,
    Previous,
    Next,
    Wheel,
    Pinch,
    DoubleClick,
    Move,
    DragRebound,
}
#[derive(Clone, Debug, PartialEq)]
pub enum PreviewEvent {
    OpenChange {
        open: bool,
        current: usize,
    },
    Change {
        current: usize,
        previous: usize,
    },
    Transform {
        transform: PreviewTransform,
        action: PreviewAction,
    },
}
pub type PreviewEventHandler = Rc<dyn Fn(&PreviewEvent, &mut Window, &mut App)>;

/// Metadata of the selected preview image. Dimensions are logical pixels and
/// remain zero while the selected source is loading or has failed.
#[derive(Clone)]
pub struct ImageInfo {
    pub id: SharedString,
    pub source: ImageSource,
    pub alt: SharedString,
    pub width: f32,
    pub height: f32,
}

#[derive(Clone)]
pub struct PreviewRenderInfo {
    pub state: Entity<PreviewState>,
    pub transform: PreviewTransform,
    pub current: usize,
    pub total: usize,
    pub image: Option<ImageInfo>,
}
pub type PreviewRenderer =
    Rc<dyn Fn(AnyElement, &PreviewRenderInfo, &mut Window, &mut App) -> AnyElement>;
pub type PreviewCountRenderer = Rc<dyn Fn(usize, usize, &mut Window, &mut App) -> AnyElement>;

/// Fresh native content factory. Retain the factory when its identity matters.
pub type ImageContent = Rc<dyn Fn(&mut Window, &mut App) -> AnyElement>;
pub type ImageProgressRenderer = Rc<dyn Fn(AnyElement, u8, &mut Window, &mut App) -> AnyElement>;

#[derive(Clone, Default)]
pub struct ImageProgress {
    /// `None` shows indeterminate ink. Values are rounded and clamped to 0–100.
    pub percent: Option<f32>,
    pub render: Option<ImageProgressRenderer>,
}
#[derive(Clone)]
pub enum ImagePlaceholder {
    Default,
    Content(ImageContent),
    Progress(ImageProgress),
}
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum ImageCoverPlacement {
    Top,
    Bottom,
    #[default]
    Center,
}

/// Native semantic slots. Unset style fields inherit the preceding layer.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ImageStyles {
    pub root: StyleRefinement,
    pub image: StyleRefinement,
    pub cover: StyleRefinement,
    pub placeholder: StyleRefinement,
    pub progress_root: StyleRefinement,
    pub progress_content: StyleRefinement,
    pub progress_rail: StyleRefinement,
    pub progress_indicator: StyleRefinement,
    pub popup_root: StyleRefinement,
    pub popup_mask: StyleRefinement,
    pub popup_body: StyleRefinement,
    pub popup_footer: StyleRefinement,
    pub popup_actions: StyleRefinement,
    pub popup_close: StyleRefinement,
}
impl ImageStyles {
    pub(crate) fn refine(&mut self, other: &Self) {
        use gpui::Refineable;
        macro_rules! merge { ($($field:ident),+) => { $(self.$field.refine(&other.$field);)+ }; }
        merge!(
            root,
            image,
            cover,
            placeholder,
            progress_root,
            progress_content,
            progress_rail,
            progress_indicator,
            popup_root,
            popup_mask,
            popup_body,
            popup_footer,
            popup_actions,
            popup_close
        );
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ImageStyleContext {
    pub preview: bool,
    pub loading: bool,
    pub failed: bool,
    pub direction: crate::Direction,
}

#[derive(Clone)]
pub struct PreviewOptions {
    pub enabled: bool,
    pub min_scale: f32,
    pub max_scale: f32,
    pub scale_step: f32,
    pub wheel: bool,
    pub movable: bool,
    pub mask: bool,
    /// Accepted for API parity; currently rendered as a dimmed mask by pinned GPUI.
    pub blur: bool,
    pub mask_closable: bool,
    pub focus_trap: bool,
    pub close_icon: IconRef,
    pub controlled_open: Option<bool>,
    pub controlled_current: Option<usize>,
    pub styles: ImageStyles,
    pub actions_render: Option<PreviewRenderer>,
    pub image_render: Option<PreviewRenderer>,
    pub count_render: Option<PreviewCountRenderer>,
    /// Groups show their one-based count by default; standalone Image hides it.
    pub show_count: Option<bool>,
    pub on_event: Option<PreviewEventHandler>,
}
impl Default for PreviewOptions {
    fn default() -> Self {
        Self {
            enabled: true,
            min_scale: 1.,
            max_scale: 50.,
            scale_step: 0.5,
            wheel: true,
            movable: true,
            mask: true,
            blur: false,
            mask_closable: true,
            focus_trap: true,
            close_icon: IconName::CloseOutlined.into(),
            controlled_open: None,
            controlled_current: None,
            styles: ImageStyles::default(),
            actions_render: None,
            image_render: None,
            count_render: None,
            show_count: None,
            on_event: None,
        }
    }
}
impl PartialEq for PreviewOptions {
    fn eq(&self, other: &Self) -> bool {
        fn callback_eq<T: ?Sized>(a: &Option<Rc<T>>, b: &Option<Rc<T>>) -> bool {
            match (a, b) {
                (None, None) => true,
                (Some(a), Some(b)) => Rc::ptr_eq(a, b),
                _ => false,
            }
        }
        self.enabled == other.enabled
            && self.min_scale == other.min_scale
            && self.max_scale == other.max_scale
            && self.scale_step == other.scale_step
            && self.wheel == other.wheel
            && self.movable == other.movable
            && self.mask == other.mask
            && self.blur == other.blur
            && self.mask_closable == other.mask_closable
            && self.focus_trap == other.focus_trap
            && self.close_icon == other.close_icon
            && self.controlled_open == other.controlled_open
            && self.controlled_current == other.controlled_current
            && self.show_count == other.show_count
            && self.styles == other.styles
            && callback_eq(&self.actions_render, &other.actions_render)
            && callback_eq(&self.image_render, &other.image_render)
            && callback_eq(&self.count_render, &other.count_render)
            && callback_eq(&self.on_event, &other.on_event)
    }
}

#[cfg(test)]
mod configuration_tests;
#[cfg(test)]
mod performance_tests;
#[cfg(test)]
mod registration_tests;
#[cfg(test)]
mod tests;
#[cfg(test)]
mod thumbnail_tests;
