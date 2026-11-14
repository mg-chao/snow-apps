// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use gpui::{Bounds, HitboxId, Length, Pixels, Point, Size, Window, px};

/// Manual resizing of a multiline editor. Content-driven auto size disables it.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum TextAreaResize {
    None,
    #[default]
    Vertical,
    Horizontal,
    Both,
}
impl TextAreaResize {
    pub(crate) fn width(self) -> bool {
        matches!(self, Self::Horizontal | Self::Both)
    }
    pub(crate) fn height(self) -> bool {
        matches!(self, Self::Vertical | Self::Both)
    }
}

struct ResizeDrag {
    origin: Point<Pixels>,
    size: Size<Pixels>,
    mode: TextAreaResize,
    rtl: bool,
}

/// Presentation dimensions belong to a mounted editor, independently of its
/// text value, undo history, form binding or caret geometry.
#[derive(Default)]
pub(crate) struct ResizeState {
    pub width: Option<Pixels>,
    pub height: Option<Pixels>,
    pub bounds: Option<Bounds<Pixels>>,
    declared: Size<Option<Length>>,
    drag: Option<ResizeDrag>,
    capture: Option<HitboxId>,
}
impl ResizeState {
    pub fn configure(&mut self, declared: Size<Option<Length>>) -> bool {
        let changed = self.declared != declared;
        if self.declared.width != declared.width {
            self.width = None;
        }
        if self.declared.height != declared.height {
            self.height = None;
        }
        self.declared = declared;
        changed
    }
    pub fn dragging(&self) -> bool {
        self.drag.is_some()
    }
    pub fn compatible(&self, mode: TextAreaResize, rtl: bool) -> bool {
        self.drag
            .as_ref()
            .is_none_or(|drag| drag.mode == mode && drag.rtl == rtl)
    }
    pub fn begin(&mut self, position: Point<Pixels>, mode: TextAreaResize, rtl: bool) -> bool {
        let Some(bounds) = self.bounds else {
            return false;
        };
        self.drag = Some(ResizeDrag {
            origin: position,
            size: bounds.size,
            mode,
            rtl,
        });
        true
    }
    pub fn update(&mut self, position: Point<Pixels>) -> bool {
        let Some(drag) = &self.drag else {
            return false;
        };
        let delta = position - drag.origin;
        let width = if drag.mode.width() {
            Some((drag.size.width + if drag.rtl { -delta.x } else { delta.x }).max(px(1.)))
        } else {
            self.width
        };
        let height = if drag.mode.height() {
            Some((drag.size.height + delta.y).max(px(1.)))
        } else {
            self.height
        };
        let changed = self.width != width || self.height != height;
        self.width = width;
        self.height = height;
        changed
    }
    pub fn capture(&mut self, hitbox: HitboxId, window: &mut Window) {
        self.capture = Some(hitbox);
        window.capture_pointer(hitbox);
    }
    pub fn end(&mut self, window: &mut Window) {
        self.drag = None;
        if self
            .capture
            .take()
            .is_some_and(|capture| window.captured_hitbox() == Some(capture))
        {
            window.release_pointer();
        }
    }
    pub fn reset(&mut self, window: &mut Window) {
        self.end(window);
        self.width = None;
        self.height = None;
    }
}
