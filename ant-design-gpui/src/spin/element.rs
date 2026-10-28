// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::state::SpinState;
use gpui::{
    A11ySubtreeBuilder, AnyElement, App, AvailableSpace, Bounds, ContentMask, Element, ElementId,
    Entity, GlobalElementId, InspectorElementId, IntoElement, LayoutId, Pixels, Point, Role, Style,
    Window, accesskit, px, size,
};
use std::{cell::Cell, rc::Rc};

pub(super) struct Activity<E> {
    pub element: E,
    pub state: Entity<SpinState>,
    pub animation: bool,
    pub pointer_events: bool,
    pub indicator_visible: Rc<Cell<bool>>,
    pub surface_animation: bool,
    pub probe: bool,
}
impl<E: Element> IntoElement for Activity<E> {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}
impl<E: Element> Element for Activity<E> {
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
        if !self.probe {
            node.set_live(accesskit::Live::Polite);
        }
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
        if self.pointer_events {
            self.element
                .prepaint(id, inspector, bounds, state, window, cx)
        } else {
            // Register focus/keyboard paths normally, but clip pointer hitboxes
            // to empty. Painting has its own normal mask, so the leaving layer
            // remains visible without intercepting the page underneath it.
            window.with_content_mask(
                Some(ContentMask {
                    bounds: Bounds::default(),
                }),
                |window| {
                    self.element
                        .prepaint(id, inspector, bounds, state, window, cx)
                },
            )
        }
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
        // Hidden native capture windows still paint on demand. The clipping check
        // prevents offscreen rows from sustaining an animation render loop.
        let visible =
            window.is_visible() && !bounds.intersect(&window.content_mask().bounds).is_empty();
        if self.probe {
            self.indicator_visible.set(visible);
            return;
        }
        let visible = visible && (self.surface_animation || self.indicator_visible.get());
        let animation = self.surface_animation || (self.indicator_visible.get() && self.animation);
        self.state.update(cx, |state, cx| {
            state.schedule(visible, animation, window, cx)
        });
    }
}

/// Fixed viewport layer with no effect on its owner's normal layout.
pub(super) struct Fullscreen {
    pub id: ElementId,
    pub child: AnyElement,
    pub priority: usize,
}
impl IntoElement for Fullscreen {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}
impl Element for Fullscreen {
    type RequestLayoutState = ();
    type PrepaintState = ();
    fn id(&self) -> Option<ElementId> {
        Some(self.id.clone())
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
        let viewport = window.viewport_size();
        self.child.layout_as_root(
            size(
                AvailableSpace::Definite(viewport.width),
                AvailableSpace::Definite(viewport.height),
            ),
            window,
            cx,
        );
        (
            window.request_layout(
                Style {
                    size: size(px(0.).into(), px(0.).into()),
                    ..Default::default()
                },
                [],
                cx,
            ),
            (),
        )
    }
    fn prepaint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        _: Bounds<Pixels>,
        _: &mut (),
        window: &mut Window,
        _: &mut App,
    ) {
        let child = std::mem::replace(&mut self.child, gpui::div().into_any_element());
        window.defer_draw(child, Point::default(), self.priority, None);
    }
    fn paint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        _: Bounds<Pixels>,
        _: &mut (),
        _: &mut (),
        _: &mut Window,
        _: &mut App,
    ) {
    }
}
