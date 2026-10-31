// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Collect layout geometry without adding a layout, semantic, or event node.

use std::{cell::Cell, rc::Rc};

use gpui::{
    A11ySubtreeBuilder, App, Bounds, Element, ElementId, GlobalElementId, InspectorElementId,
    IntoElement, LayoutId, Pixels, Role, Window, accesskit,
};

pub(super) fn measure<E: IntoElement>(
    element: E,
    bounds: Rc<Cell<Option<Bounds<Pixels>>>>,
) -> Measured<E::Element> {
    Measured {
        element: element.into_element(),
        bounds,
    }
}

/// Keep the wrapped element's native identity, focus, hitbox, and accessibility.
pub(super) struct Measured<E> {
    element: E,
    bounds: Rc<Cell<Option<Bounds<Pixels>>>>,
}

impl<E: Element> IntoElement for Measured<E> {
    type Element = Self;

    fn into_element(self) -> Self {
        self
    }
}

impl<E: Element> Element for Measured<E> {
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
        prepaint: &mut Self::PrepaintState,
        builder: &mut A11ySubtreeBuilder,
    ) {
        self.element.a11y_synthetic_children(prepaint, builder);
    }

    fn request_layout(
        &mut self,
        id: Option<&GlobalElementId>,
        inspector_id: Option<&InspectorElementId>,
        window: &mut Window,
        cx: &mut App,
    ) -> (LayoutId, Self::RequestLayoutState) {
        self.element.request_layout(id, inspector_id, window, cx)
    }

    fn prepaint(
        &mut self,
        id: Option<&GlobalElementId>,
        inspector_id: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        state: &mut Self::RequestLayoutState,
        window: &mut Window,
        cx: &mut App,
    ) -> Self::PrepaintState {
        self.bounds.set(Some(bounds));
        self.element
            .prepaint(id, inspector_id, bounds, state, window, cx)
    }

    fn paint(
        &mut self,
        id: Option<&GlobalElementId>,
        inspector_id: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        state: &mut Self::RequestLayoutState,
        prepaint: &mut Self::PrepaintState,
        window: &mut Window,
        cx: &mut App,
    ) {
        self.element
            .paint(id, inspector_id, bounds, state, prepaint, window, cx);
    }
}
