// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::{Direction, ProgressAlign};
use gpui::{
    AnyElement, App, AvailableSpace, Bounds, Element, ElementId, GlobalElementId,
    InspectorElementId, IntoElement, LayoutId, Pixels, Refineable, Size, Style, StyleRefinement,
    Window, point, size,
};

/// Intrinsic content participates in the track's minimum width, including native
/// icons and custom layouts. Each child is constructed and measured only once.
pub(super) struct InnerTrack {
    pub label: AnyElement,
    pub surface: AnyElement,
    pub style: StyleRefinement,
    pub align: ProgressAlign,
    pub direction: Direction,
}
impl IntoElement for InnerTrack {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}
impl Element for InnerTrack {
    type RequestLayoutState = Size<Pixels>;
    type PrepaintState = ();
    fn id(&self) -> Option<ElementId> {
        Some("inner-track".into())
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
    ) -> (LayoutId, Size<Pixels>) {
        let measured = self.label.layout_as_root(
            size(AvailableSpace::MaxContent, AvailableSpace::MaxContent),
            window,
            cx,
        );
        let mut style = Style::default();
        style.refine(&self.style);
        style.min_size.width = measured.width.into();
        (window.request_layout(style, [], cx), measured)
    }
    fn prepaint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        measured: &mut Size<Pixels>,
        window: &mut Window,
        cx: &mut App,
    ) {
        self.surface.prepaint_as_root(
            bounds.origin,
            size(
                AvailableSpace::Definite(bounds.size.width),
                AvailableSpace::Definite(bounds.size.height),
            ),
            window,
            cx,
        );
        let remainder = bounds.size.width - measured.width;
        let offset = match (self.align, self.direction) {
            (ProgressAlign::Center, _) => remainder / 2.,
            (ProgressAlign::Start, Direction::Rtl) | (ProgressAlign::End, Direction::Ltr) => {
                remainder
            }
            _ => gpui::px(0.),
        };
        self.label.prepaint_at(
            point(
                bounds.left() + offset,
                bounds.top() + (bounds.size.height - measured.height) / 2.,
            ),
            window,
            cx,
        );
    }
    fn paint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        _: Bounds<Pixels>,
        _: &mut Size<Pixels>,
        _: &mut (),
        window: &mut Window,
        cx: &mut App,
    ) {
        self.surface.paint(window, cx);
        self.label.paint(window, cx);
    }
}
