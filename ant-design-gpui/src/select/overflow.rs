// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Single-pass native responsive tags. Measure only tags that could fit; hidden
//! tags never enter the element tree or accessibility tree. Resize needs no
//! observer, state update, asynchronous measurement or follow-up layout frame.

use crate::Direction;
use gpui::{
    AnyElement, App, AvailableSpace, Bounds, ContentMask, Element, ElementId, GlobalElementId,
    InspectorElementId, IntoElement, LayoutId, Pixels, Style, Window, point, px, relative, size,
};
use std::rc::Rc;

pub(super) type Factory = Rc<dyn Fn(usize, &mut Window, &mut App) -> AnyElement>;
pub(super) struct ResponsiveTags {
    pub count: usize,
    pub tag: Factory,
    /// Argument is the first omitted index, not the omitted count.
    pub overflow: Factory,
    pub editor: AnyElement,
    pub height: Pixels,
    pub direction: Direction,
}
impl IntoElement for ResponsiveTags {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}
impl Element for ResponsiveTags {
    type RequestLayoutState = ();
    type PrepaintState = Vec<AnyElement>;
    fn id(&self) -> Option<ElementId> {
        Some("responsive-tags".into())
    }
    fn source_location(&self) -> Option<&'static std::panic::Location<'static>> {
        None
    }
    fn request_layout(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        window: &mut Window,
        cx: &mut App,
    ) -> (LayoutId, ()) {
        let mut style = Style::default();
        style.size.width = relative(1.).into();
        style.size.height = self.height.into();
        style.min_size.width = px(0.).into();
        style.flex_grow = 1.;
        style.flex_shrink = 1.;
        (window.request_layout(style, [], cx), ())
    }
    fn prepaint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        _: &mut (),
        window: &mut Window,
        cx: &mut App,
    ) -> Vec<AnyElement> {
        let measure = size(
            AvailableSpace::MaxContent,
            AvailableSpace::Definite(bounds.size.height),
        );
        let mut editor_size = self.editor.layout_as_root(measure, window, cx);
        if editor_size.width > bounds.size.width {
            editor_size = self.editor.layout_as_root(
                size(AvailableSpace::Definite(bounds.size.width), measure.height),
                window,
                cx,
            );
        }
        let available = (bounds.size.width - editor_size.width).max(px(0.));
        let mut children = Vec::new();
        let mut widths = Vec::new();
        let mut used = px(0.);
        let mut omitted = self.count;
        let mut overflow = if self.count == 0 {
            None
        } else {
            let mut child = (self.overflow)(0, window, cx);
            let measured = child.layout_as_root(measure, window, cx);
            Some((child, measured.width))
        };
        for index in 0..self.count {
            let mut child = (self.tag)(index, window, cx);
            let measured = child.layout_as_root(measure, window, cx);
            let next_overflow = if index + 1 < self.count {
                let mut overflow = (self.overflow)(index + 1, window, cx);
                let size = overflow.layout_as_root(measure, window, cx);
                Some((overflow, size.width))
            } else {
                None
            };
            let needed = used + measured.width + next_overflow.as_ref().map_or(px(0.), |(_, w)| *w);
            if needed > available {
                break;
            }
            used += measured.width;
            widths.push(measured.width);
            children.push(child);
            overflow = next_overflow;
            omitted = self.count - index - 1;
        }
        if omitted > 0
            && let Some((mut child, mut width)) = overflow
        {
            let remaining = (available - used).max(px(0.));
            if width > remaining {
                // The summary has lower priority than the editing viewport.
                // Constrain its native layout before positioning the editor;
                // clipping the whole row alone would leave the editor outside.
                width = child
                    .layout_as_root(
                        size(AvailableSpace::Definite(remaining), measure.height),
                        window,
                        cx,
                    )
                    .width
                    .min(remaining);
            }
            children.push(child);
            widths.push(width);
        }
        let mut offset = px(0.);
        window.with_content_mask(Some(ContentMask { bounds }), |window| {
            for (child, width) in children.iter_mut().zip(widths) {
                let x = match self.direction {
                    Direction::Ltr => bounds.left() + offset,
                    Direction::Rtl => bounds.right() - offset - width,
                };
                child.prepaint_at(point(x, bounds.top()), window, cx);
                offset += width;
            }
            let x = match self.direction {
                Direction::Ltr => bounds.left() + offset,
                Direction::Rtl => bounds.right() - offset - editor_size.width,
            };
            self.editor.prepaint_at(point(x, bounds.top()), window, cx);
        });
        children
    }
    fn paint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        _: &mut (),
        children: &mut Vec<AnyElement>,
        window: &mut Window,
        cx: &mut App,
    ) {
        window.with_content_mask(Some(ContentMask { bounds }), |window| {
            for child in children {
                child.paint(window, cx);
            }
            self.editor.paint(window, cx);
        });
    }
}
