// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use gpui::{
    AnyElement, App, AvailableSpace, Bounds, Element, ElementId, GlobalElementId,
    InspectorElementId, IntoElement, LayoutId, Length, Pixels, Style, Window, px, size,
};

/// A popup anchor with the child's intrinsic width in both block and flex flow.
/// The real child keeps its focus, accessibility and popup-trigger identities.
pub(super) struct IntrinsicAnchor {
    child: AnyElement,
    preferred_width: Option<Pixels>,
    width_correction: Pixels,
    widths: [Option<Length>; 3],
}
impl IntrinsicAnchor {
    pub fn new(
        child: impl IntoElement,
        preferred_width: Option<Pixels>,
        width_correction: Pixels,
    ) -> Self {
        Self {
            child: child.into_any_element(),
            preferred_width,
            width_correction,
            widths: [None; 3],
        }
    }
    /// Keep authored widths in the parent's layout context. In particular,
    /// percentages cannot resolve during the child's intrinsic measurement.
    pub fn with_widths(
        mut self,
        width: Option<Length>,
        min: Option<Length>,
        max: Option<Length>,
    ) -> Self {
        self.widths = [width, min, max];
        self
    }
}
impl IntoElement for IntrinsicAnchor {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}
impl Element for IntrinsicAnchor {
    type RequestLayoutState = ();
    type PrepaintState = ();
    fn id(&self) -> Option<ElementId> {
        None
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
        // RenderOnce children are built here, while Popup's trigger context is
        // installed. Subsequent layout calls reuse that same native element.
        let measured = self.child.layout_as_root(
            size(AvailableSpace::MaxContent, AvailableSpace::MaxContent),
            window,
            cx,
        );
        let width = if measured.width <= px(0.) || measured.height <= px(0.) {
            px(0.)
        } else if let Some(preferred) = self.preferred_width {
            measured.width.max(preferred)
        } else {
            (measured.width + self.width_correction).max(px(0.))
        };
        let native = if width != measured.width && width > px(0.) {
            self.child.layout_as_root(
                size(AvailableSpace::Definite(width), AvailableSpace::MaxContent),
                window,
                cx,
            )
        } else {
            measured
        };
        let mut style = Style::default();
        // A definite content width also prevents cross-axis stretching in flex
        // columns. An aspect ratio with auto width still stretches there.
        style.size.width = width.into();
        style.size.height = native.height.into();
        style.flex_shrink = 0.;
        if let Some(width) = self.widths[0] {
            style.size.width = width;
        }
        if let Some(min) = self.widths[1] {
            style.min_size.width = min;
        }
        if let Some(max) = self.widths[2] {
            style.max_size.width = max;
        }
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
        self.child.layout_as_root(
            size(
                AvailableSpace::Definite(bounds.size.width),
                AvailableSpace::MaxContent,
            ),
            window,
            cx,
        );
        self.child.prepaint_at(bounds.origin, window, cx);
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
        self.child.paint(window, cx);
    }
}

#[cfg(test)]
#[path = "anchor_tests.rs"]
mod tests;
