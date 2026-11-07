// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use crate::theme::Color;
use gpui::{
    A11ySubtreeBuilder, App, Bounds, Element, ElementId, GlobalElementId, InspectorElementId,
    IntoElement, LayoutId, Pixels, Role, Window, accesskit, fill, px,
};

/// Resolve a group over its explicitly known uniform backdrop, after all native
/// content has painted. Delegation preserves its layout, interaction and a11y.
pub(super) struct OpacityGroup<E> {
    pub element: E,
    pub backdrop: Color,
    pub opacity: f32,
    pub outset: f32,
}

impl<E: Element> IntoElement for OpacityGroup<E> {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}

impl<E: Element> Element for OpacityGroup<E> {
    type RequestLayoutState = E::RequestLayoutState;
    type PrepaintState = E::PrepaintState;

    fn id(&self) -> Option<ElementId> {
        self.element.id()
    }
    fn source_location(&self) -> Option<&'static std::panic::Location<'static>> {
        self.element.source_location()
    }
    fn a11y_role(&self) -> Option<Role> {
        self.element.a11y_role()
    }
    fn write_a11y_info(&self, node: &mut accesskit::Node) {
        self.element.write_a11y_info(node);
    }
    fn a11y_synthetic_children(
        &mut self,
        state: &mut Self::PrepaintState,
        builder: &mut A11ySubtreeBuilder,
    ) {
        self.element.a11y_synthetic_children(state, builder);
    }
    fn request_layout(
        &mut self,
        id: Option<&GlobalElementId>,
        inspector: Option<&InspectorElementId>,
        window: &mut Window,
        cx: &mut App,
    ) -> (LayoutId, Self::RequestLayoutState) {
        self.element.request_layout(id, inspector, window, cx)
    }
    fn prepaint(
        &mut self,
        id: Option<&GlobalElementId>,
        inspector: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        state: &mut Self::RequestLayoutState,
        window: &mut Window,
        cx: &mut App,
    ) -> Self::PrepaintState {
        self.element
            .prepaint(id, inspector, bounds, state, window, cx)
    }
    fn paint(
        &mut self,
        id: Option<&GlobalElementId>,
        inspector: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        state: &mut Self::RequestLayoutState,
        prepaint: &mut Self::PrepaintState,
        window: &mut Window,
        cx: &mut App,
    ) {
        self.element
            .paint(id, inspector, bounds, state, prepaint, window, cx);
        if self.opacity < 1. {
            // B + opacity * (painted - B), including overlapping native children.
            // This is exact for the declared uniform backdrop. The ancestor
            // content mask clips the wash, including shadow and focus overflow.
            window.paint_quad(fill(
                bounds.dilate(px(self.outset)),
                self.backdrop
                    .with_alpha(f64::from(1. - self.opacity))
                    .to_rgba(),
            ));
        }
    }
}
