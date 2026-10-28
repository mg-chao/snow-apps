// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Horizontal Menu layout measures native children and chooses its overflow range
//! in the same frame. Omitted entries never acquire hitboxes or accessible nodes.

use crate::Direction;
use gpui::prelude::*;
use gpui::{
    AnyElement, App, AvailableSpace, Bounds, ContentMask, Element, ElementId, GlobalElementId,
    InspectorElementId, IntoElement, LayoutId, Pixels, Style, Window, point, px, relative, size,
};
use std::rc::Rc;

pub(super) type ItemFactory = Rc<dyn Fn(usize, &mut Window, &mut App) -> AnyElement>;
pub(super) type RangeHandler = Rc<dyn Fn(usize, &mut Window, &mut App)>;
pub(super) type WidthFactory = Rc<dyn Fn(usize, &Window, &App) -> Option<Pixels>>;

pub(super) struct ResponsiveMenu {
    pub count: usize,
    pub item: ItemFactory,
    /// Receives the index of the first entry moved into the overflow submenu.
    pub overflow: ItemFactory,
    pub on_range: RangeHandler,
    pub overflow_width: Pixels,
    pub width: WidthFactory,
    pub height: Pixels,
    pub direction: Direction,
}

impl IntoElement for ResponsiveMenu {
    type Element = Self;

    fn into_element(self) -> Self {
        self
    }
}

impl Element for ResponsiveMenu {
    type RequestLayoutState = ();
    type PrepaintState = Vec<AnyElement>;

    fn id(&self) -> Option<ElementId> {
        Some("menu-responsive-overflow".into())
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
        let known: Option<Vec<Pixels>> = (0..self.count)
            .map(|index| (self.width)(index, window, cx))
            .collect();
        let (mut children, mut widths) = if let Some(widths) = known {
            let count = fit_count(&widths, bounds.size.width, self.overflow_width);
            let children = (0..count)
                .map(|index| {
                    let mut child = (self.item)(index, window, cx);
                    child.layout_as_root(measure, window, cx);
                    child
                })
                .collect();
            (children, widths)
        } else {
            let mut children = Vec::new();
            let mut widths = Vec::new();
            for index in 0..self.count {
                let mut child = (self.item)(index, window, cx);
                let measured = child.layout_as_root(measure, window, cx);
                widths.push(measured.width);
                children.push(child);
            }
            (children, widths)
        };
        let first_omitted = fit_count(&widths, bounds.size.width, self.overflow_width);
        children.truncate(first_omitted);
        widths.truncate(first_omitted);
        let used: Pixels = widths.iter().copied().sum();
        (self.on_range)(first_omitted, window, cx);
        if first_omitted < self.count {
            // Rebuild with the final range: its content closure must capture the
            // actual hidden suffix rather than the provisional measurement range.
            let mut child = (self.overflow)(first_omitted, window, cx);
            let remaining = (bounds.size.width - used).max(px(0.));
            let width = child.layout_as_root(measure, window, cx).width;
            let width = if width > remaining {
                child
                    .layout_as_root(
                        size(AvailableSpace::Definite(remaining), measure.height),
                        window,
                        cx,
                    )
                    .width
                    .min(remaining)
            } else {
                width
            };
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
        });
    }
}

fn fit_count(widths: &[Pixels], available: Pixels, overflow_width: Pixels) -> usize {
    if widths.iter().copied().sum::<Pixels>() <= available {
        return widths.len();
    }
    let available = (available - overflow_width).max(px(0.));
    let mut used = px(0.);
    widths
        .iter()
        .take_while(|width| {
            used += **width;
            used <= available
        })
        .count()
}

/// Preserve cumulative logical offsets across fractional display scales. Native
/// child painting still snaps its edges, while 35px rows do not each lose half
/// a device pixel before the following row is positioned.
pub(super) struct MenuStack {
    children: Vec<(AnyElement, Pixels, Pixels, Pixels)>,
}

impl MenuStack {
    pub fn with_insets(children: Vec<(AnyElement, Pixels, Pixels)>) -> Self {
        Self::with_spacing(
            children
                .into_iter()
                .map(|(child, height, inset)| (child, height, inset, px(0.)))
                .collect(),
        )
    }

    pub fn with_spacing(children: Vec<(AnyElement, Pixels, Pixels, Pixels)>) -> Self {
        Self {
            children: children
                .into_iter()
                .map(|(child, height, inset, before)| {
                    (
                        gpui::div()
                            .w_full()
                            .flex()
                            .flex_col()
                            .child(child)
                            .into_any_element(),
                        height,
                        inset,
                        before,
                    )
                })
                .collect(),
        }
    }
}

impl IntoElement for MenuStack {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}

impl Element for MenuStack {
    type RequestLayoutState = ();
    type PrepaintState = ();
    fn id(&self) -> Option<ElementId> {
        Some("menu-logical-stack".into())
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
        style.size.height = self
            .children
            .iter()
            .map(|(_, height, _, _)| *height)
            .sum::<Pixels>()
            .into();
        style.flex_shrink = 0.;
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
    ) {
        let mut offset = px(0.);
        for (child, height, inset, before) in &mut self.children {
            child.layout_as_root(
                size(
                    AvailableSpace::Definite(bounds.size.width),
                    AvailableSpace::MaxContent,
                ),
                window,
                cx,
            );
            let correction = *inset - window.pixel_snap(*inset);
            child.prepaint_at(
                point(bounds.left(), bounds.top() + offset + *before + correction),
                window,
                cx,
            );
            offset += *height;
        }
    }
    fn paint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        _: Bounds<Pixels>,
        _: &mut (),
        _: &mut (),
        window: &mut Window,
        cx: &mut App,
    ) {
        for (child, _, _, _) in &mut self.children {
            child.paint(window, cx);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn menu_overflow_reserves_indicator_only_when_required() {
        assert_eq!(fit_count(&[px(30.), px(30.), px(30.)], px(90.), px(40.)), 3);
        assert_eq!(fit_count(&[px(30.), px(30.), px(30.)], px(89.), px(40.)), 1);
        assert_eq!(fit_count(&[px(80.)], px(60.), px(40.)), 0);
        assert_eq!(fit_count(&[], px(0.), px(40.)), 0);
    }
}
