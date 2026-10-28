// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! A transparent surface adapter: explicit native live semantics and passive geometry capture.

use std::{cell::Cell, rc::Rc};

use gpui::{
    A11ySubtreeBuilder, App, Bounds, DefiniteLength, Element, ElementId, GlobalElementId,
    InspectorElementId, IntoElement, LayoutId, Length, Pixels, Role, StyleRefinement, Window,
    accesskit, px,
};

use super::state::OpenGeometry;

pub(super) struct Surface<E> {
    element: E,
    geometry: Option<Rc<Cell<Option<OpenGeometry>>>>,
    style: StyleRefinement,
    live: accesskit::Live,
}

pub(super) fn surface<E: IntoElement>(
    element: E,
    geometry: Option<Rc<Cell<Option<OpenGeometry>>>>,
    style: StyleRefinement,
    live: accesskit::Live,
) -> Surface<E::Element> {
    Surface {
        element: element.into_element(),
        geometry,
        style,
        live,
    }
}

impl<E: Element> IntoElement for Surface<E> {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}

impl<E: Element> Element for Surface<E> {
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
        node.set_live(self.live);
        node.set_live_atomic();
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
        if let Some(geometry) = &self.geometry {
            let length = |value: Option<DefiniteLength>| {
                value
                    .unwrap_or_default()
                    .to_pixels(bounds.size.width.into(), window.rem_size())
            };
            let margin = match self.style.margin.bottom {
                Some(Length::Definite(value)) => length(Some(value)),
                _ => px(0.),
            };
            geometry.set(Some(OpenGeometry {
                bounds,
                padding_top: length(self.style.padding.top),
                padding_bottom: length(self.style.padding.bottom),
                margin_bottom: margin,
                opacity: self.style.opacity.unwrap_or(1.),
            }));
        }
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
    }
}
