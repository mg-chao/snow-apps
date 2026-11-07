// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::CarouselState;
use gpui::{
    AnyElement, App, Bounds, Element, ElementId, Entity, GlobalElementId, InspectorElementId,
    IntoElement, LayoutId, Pixels, Window, point, px,
};
use std::{cell::Cell, rc::Rc};

pub(super) type ViewportBounds = Rc<Cell<Option<Bounds<Pixels>>>>;

/// A native layout participant whose position is sampled in viewport units after layout.
/// Measuring hidden content never registers its focus, hitboxes or accessibility tree.
pub(super) struct SlideFrame {
    pub child: AnyElement,
    pub viewport: ViewportBounds,
    pub displacement: f64,
    pub vertical: bool,
    pub visible: bool,
    pub measured: Option<Rc<Cell<Option<f32>>>>,
}
impl IntoElement for SlideFrame {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}
impl Element for SlideFrame {
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
        (self.child.request_layout(window, cx), ())
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
        if let Some(measured) = &self.measured {
            measured.set(Some(bounds.size.height.into()));
        }
        if !self.visible {
            return;
        }
        let viewport = self.viewport.get().unwrap_or(bounds);
        let offset = if self.vertical {
            point(px(0.), viewport.size.height * self.displacement as f32)
        } else {
            point(viewport.size.width * self.displacement as f32, px(0.))
        };
        window.with_element_offset(viewport.origin + offset - bounds.origin, |window| {
            self.child.prepaint(window, cx)
        });
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
        if self.visible {
            self.child.paint(window, cx);
        }
    }
}

/// Runs visibility/scheduling only after the viewport's actual native bounds exist.
pub(super) struct Viewport<E> {
    pub child: E,
    pub state: Entity<CarouselState>,
    pub bounds: ViewportBounds,
    pub controls_animating: bool,
    pub repair_layout: bool,
}
impl<E: Element> IntoElement for Viewport<E> {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}
impl<E: Element> Element for Viewport<E> {
    type RequestLayoutState = E::RequestLayoutState;
    type PrepaintState = E::PrepaintState;
    fn id(&self) -> Option<ElementId> {
        self.child.id()
    }
    fn source_location(&self) -> Option<&'static std::panic::Location<'static>> {
        self.child.source_location()
    }
    fn a11y_role(&self) -> Option<gpui::Role> {
        self.child.a11y_role()
    }
    fn write_a11y_info(&self, node: &mut gpui::accesskit::Node) {
        self.child.write_a11y_info(node);
    }
    fn a11y_synthetic_children(
        &mut self,
        state: &mut Self::PrepaintState,
        builder: &mut gpui::A11ySubtreeBuilder,
    ) {
        self.child.a11y_synthetic_children(state, builder);
    }
    fn request_layout(
        &mut self,
        id: Option<&GlobalElementId>,
        inspector: Option<&InspectorElementId>,
        window: &mut Window,
        cx: &mut App,
    ) -> (LayoutId, Self::RequestLayoutState) {
        self.child.request_layout(id, inspector, window, cx)
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
        self.bounds.set(Some(bounds));
        self.state.update(cx, |s, cx| {
            if s.bounds
                .is_some_and(|old| old.size.width != bounds.size.width)
            {
                s.invalidate_measurements(cx);
                self.repair_layout = true;
            }
            s.bounds = Some(bounds);
        });
        self.child
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
        self.child
            .paint(id, inspector, bounds, state, prepaint, window, cx);
        let movement = self.state.downgrade();
        window.on_mouse_event(move |event: &gpui::MouseMoveEvent, phase, window, cx| {
            if phase.capture()
                && movement
                    .update(cx, |s, cx| s.pointer_move(event.position, window, cx))
                    .unwrap_or(false)
            {
                window.prevent_default();
                cx.stop_propagation();
            }
        });
        let release = self.state.downgrade();
        window.on_mouse_event(move |event: &gpui::MouseUpEvent, phase, window, cx| {
            if phase.capture()
                && event.button == gpui::MouseButton::Left
                && release
                    .update(cx, |s, cx| s.pointer_end(window, cx))
                    .unwrap_or(false)
            {
                window.prevent_default();
                cx.stop_propagation();
            }
        });
        let visible =
            window.is_visible() && !bounds.intersect(&window.content_mask().bounds).is_empty();
        self.state.update(cx, |s, cx| {
            s.schedule(visible, window, cx);
            if visible && (self.controls_animating || self.repair_layout) {
                s.request_frame(window, cx);
            }
        });
    }
}

pub(super) enum ControlGeometry {
    Dot(usize, f32),
    Previous,
    Next,
}
pub(super) fn record_control<E: IntoElement>(
    child: E,
    state: Entity<CarouselState>,
    control: ControlGeometry,
) -> impl IntoElement {
    ControlMeasure {
        child: child.into_element(),
        state,
        control,
    }
}
struct ControlMeasure<E> {
    child: E,
    state: Entity<CarouselState>,
    control: ControlGeometry,
}
impl<E: Element> IntoElement for ControlMeasure<E> {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}
impl<E: Element> Element for ControlMeasure<E> {
    type RequestLayoutState = E::RequestLayoutState;
    type PrepaintState = E::PrepaintState;
    fn id(&self) -> Option<ElementId> {
        self.child.id()
    }
    fn source_location(&self) -> Option<&'static std::panic::Location<'static>> {
        self.child.source_location()
    }
    fn a11y_role(&self) -> Option<gpui::Role> {
        self.child.a11y_role()
    }
    fn write_a11y_info(&self, node: &mut gpui::accesskit::Node) {
        self.child.write_a11y_info(node);
    }
    fn a11y_synthetic_children(
        &mut self,
        state: &mut Self::PrepaintState,
        builder: &mut gpui::A11ySubtreeBuilder,
    ) {
        self.child.a11y_synthetic_children(state, builder);
    }
    fn request_layout(
        &mut self,
        id: Option<&GlobalElementId>,
        inspector: Option<&InspectorElementId>,
        window: &mut Window,
        cx: &mut App,
    ) -> (LayoutId, Self::RequestLayoutState) {
        self.child.request_layout(id, inspector, window, cx)
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
        self.state.update(cx, |s, _| match self.control {
            ControlGeometry::Dot(index, inset) => {
                s.dot_bounds.insert(
                    index,
                    Bounds::new(
                        bounds.origin + point(px(inset), px(inset)),
                        gpui::size(
                            (bounds.size.width - px(2. * inset)).max(px(0.)),
                            (bounds.size.height - px(2. * inset)).max(px(0.)),
                        ),
                    ),
                );
            }
            ControlGeometry::Previous => s.previous_arrow_bounds = Some(bounds),
            ControlGeometry::Next => s.next_arrow_bounds = Some(bounds),
        });
        self.child
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
        self.child
            .paint(id, inspector, bounds, state, prepaint, window, cx);
    }
}
