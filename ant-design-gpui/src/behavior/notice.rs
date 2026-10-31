// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use gpui::{
    A11ySubtreeBuilder, AnyElement, App, AvailableSpace, Bounds, ContentMask, Element, ElementId,
    GlobalElementId, InspectorElementId, IntoElement, KeyDownEvent, KeyUpEvent, LayoutId,
    MouseDownEvent, MouseUpEvent, Pixels, Point, Role, Size, Window, accesskit, size,
};
use gpui::{FocusHandle, Global, Subscription, WeakFocusHandle, WindowId};
use std::{cell::Cell, rc::Rc};

#[derive(Clone, Copy)]
pub(crate) struct SurfaceGeometry {
    pub logical: Size<Pixels>,
    pub scale: f32,
}

/// Retains a child's native measurement while applying the fractional CSS inset
/// discarded by GPUI's per-padding device-pixel snapping. The child is built once.
pub(crate) struct ContentProbe {
    pub child: AnyElement,
    pub layout: Rc<Cell<Option<LayoutId>>>,
    pub offset: Rc<Cell<Point<Pixels>>>,
}

impl IntoElement for ContentProbe {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}
impl Element for ContentProbe {
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
        let layout = self.child.request_layout(window, cx);
        self.layout.set(Some(layout));
        (layout, ())
    }
    fn prepaint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        _: Bounds<Pixels>,
        _: &mut (),
        window: &mut Window,
        cx: &mut App,
    ) {
        window.with_element_offset(self.offset.get(), |window| self.child.prepaint(window, cx));
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

/// Delegation preserves the actual notice's native identity and live-region boundary.
pub(crate) struct LiveSurface<E> {
    pub element: E,
    pub closing: bool,
    pub boundary: Option<FocusBoundary>,
    pub geometry: Option<Rc<Cell<Option<SurfaceGeometry>>>>,
}

impl<E: Element> IntoElement for LiveSurface<E> {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}

impl<E: Element> Element for LiveSurface<E> {
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
        // The retained handle groups descendant focus; a passive live region
        // itself is not a control that assistive technology should focus.
        if self.boundary.is_some()
            && matches!(self.element.a11y_role(), Some(Role::Alert | Role::Status))
        {
            node.remove_action(accesskit::Action::Focus);
        }
        if let Some(geometry) = self.geometry.as_ref().and_then(|geometry| geometry.get())
            && let Some(bounds) = node.bounds()
        {
            node.set_bounds(accesskit::Rect {
                x1: bounds.x0 + f64::from(f32::from(geometry.logical.width) * geometry.scale),
                y1: bounds.y0 + f64::from(f32::from(geometry.logical.height) * geometry.scale),
                ..bounds
            });
        }
        if self.closing {
            node.set_hidden();
        }
        node.set_live(if self.closing {
            accesskit::Live::Off
        } else {
            match self.element.a11y_role() {
                Some(Role::Alert) => accesskit::Live::Assertive,
                Some(Role::Status) => accesskit::Live::Polite,
                _ => accesskit::Live::Off,
            }
        });
        node.set_live_atomic();
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
        mut bounds: Bounds<Pixels>,
        state: &mut Self::RequestLayoutState,
        window: &mut Window,
        cx: &mut App,
    ) -> Self::PrepaintState {
        if let Some(geometry) = self.geometry.as_ref().and_then(|geometry| geometry.get()) {
            bounds.size = geometry.logical;
        }
        if self.closing {
            window.with_content_mask(
                Some(ContentMask {
                    bounds: Bounds::default(),
                }),
                |window| {
                    self.element
                        .prepaint(id, inspector, bounds, state, window, cx)
                },
            )
        } else {
            self.element
                .prepaint(id, inspector, bounds, state, window, cx)
        }
    }
    fn paint(
        &mut self,
        id: Option<&GlobalElementId>,
        inspector: Option<&InspectorElementId>,
        mut bounds: Bounds<Pixels>,
        state: &mut Self::RequestLayoutState,
        prepaint: &mut Self::PrepaintState,
        window: &mut Window,
        cx: &mut App,
    ) {
        if let Some(geometry) = self.geometry.as_ref().and_then(|geometry| geometry.get()) {
            bounds.size = geometry.logical;
        }
        if let Some(boundary) = &self.boundary {
            if !self.closing {
                // Only the previous open frame still owns live pointer hitboxes.
                // A newly painted exit already has an empty pointer mask and
                // must leave controls underneath its visual fully interactive.
                let down = boundary.clone();
                window.on_mouse_event(move |event: &MouseDownEvent, phase, _, cx| {
                    if phase.capture() && down.is_closed() && bounds.contains(&event.position) {
                        cx.stop_propagation();
                    }
                });
                let up = boundary.clone();
                window.on_mouse_event(move |event: &MouseUpEvent, phase, _, cx| {
                    if phase.capture() && up.is_closed() && bounds.contains(&event.position) {
                        cx.stop_propagation();
                    }
                });
            }
            let down = boundary.clone();
            window.on_key_event(move |_: &KeyDownEvent, phase, window, cx| {
                if phase.capture() && down.is_closed() {
                    down.skip(window, cx);
                    cx.stop_propagation();
                }
            });
            let up = boundary.clone();
            window.on_key_event(move |_: &KeyUpEvent, phase, window, cx| {
                if phase.capture() && up.is_closed() {
                    up.skip(window, cx);
                    cx.stop_propagation();
                }
            });
        }
        self.element
            .paint(id, inspector, bounds, state, prepaint, window, cx);
    }
}

/// A host delegates its owner's layout unchanged; its overlay has zero layout extent.
pub(crate) struct HostElement {
    pub id: ElementId,
    pub child: AnyElement,
    pub layer: AnyElement,
}

impl IntoElement for HostElement {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}

impl Element for HostElement {
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
        self.layer.layout_as_root(
            size(AvailableSpace::MaxContent, AvailableSpace::MaxContent),
            window,
            cx,
        );
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
        self.child.prepaint(window, cx);
        self.layer.prepaint_at(bounds.origin, window, cx);
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
        self.layer.paint(window, cx);
    }
}

pub(crate) struct BoundaryState {
    pub(crate) root: FocusHandle,
    window: WindowId,
    return_to: Option<WeakFocusHandle>,
    pub(crate) closed: Cell<bool>,
    _focus: Subscription,
}

#[derive(Default)]
struct FocusBoundaries(Vec<std::rc::Weak<BoundaryState>>);
impl Global for FocusBoundaries {}

/// An outgoing rich-content tree keeps its native paint while losing interaction.
#[derive(Clone)]
pub(crate) struct FocusBoundary(pub(crate) Rc<BoundaryState>);

impl FocusBoundary {
    pub(crate) fn new(window: &mut Window, cx: &mut App) -> Self {
        let root = cx.focus_handle();
        let window_id = window.window_handle().window_id();
        let return_to = window.focused(cx).map(|focus| focus.downgrade());
        let state = Rc::new_cyclic(|weak: &std::rc::Weak<BoundaryState>| {
            let observed = weak.clone();
            let subscription = window.on_focus_in(&root, cx, move |window, cx| {
                if let Some(state) = observed.upgrade()
                    && state.closed.get()
                {
                    FocusBoundary(state).skip(window, cx);
                }
            });
            BoundaryState {
                root,
                window: window_id,
                return_to,
                closed: Cell::new(false),
                _focus: subscription,
            }
        });
        if !cx.has_global::<FocusBoundaries>() {
            cx.set_global(FocusBoundaries::default());
        }
        let registry = cx.global_mut::<FocusBoundaries>();
        registry.0.retain(|weak| weak.strong_count() != 0);
        registry.0.push(Rc::downgrade(&state));
        Self(state)
    }

    fn closed_roots(&self, cx: &App) -> Vec<FocusHandle> {
        cx.try_global::<FocusBoundaries>()
            .map(|registry| {
                registry
                    .0
                    .iter()
                    .filter_map(std::rc::Weak::upgrade)
                    .filter(|boundary| boundary.window == self.0.window && boundary.closed.get())
                    .map(|boundary| boundary.root.clone())
                    .collect()
            })
            .unwrap_or_else(|| vec![self.0.root.clone()])
    }

    fn restore(&self, roots: &[FocusHandle], window: &mut Window, cx: &mut App) {
        if let Some(focus) = self.0.return_to.as_ref().and_then(WeakFocusHandle::upgrade)
            && !roots.iter().any(|root| root.contains(&focus, window))
        {
            window.focus(&focus, cx);
        } else {
            window.blur(cx);
        }
    }

    pub(crate) fn dismiss(&self, window: &mut Window, cx: &mut App) {
        if self.0.root.contains_focused(window, cx) {
            self.restore(&self.closed_roots(cx), window, cx);
        }
    }

    pub(crate) fn is_closed(&self) -> bool {
        self.0.closed.get()
    }

    pub(crate) fn skip(&self, window: &mut Window, cx: &mut App) {
        let mut visited = Vec::new();
        let roots = self.closed_roots(cx);
        while roots.iter().any(|root| root.contains_focused(window, cx)) {
            let current = window.focused(cx);
            if current.is_none() || visited.contains(&current) {
                self.restore(&roots, window, cx);
                break;
            }
            visited.push(current);
            if window.modifiers().shift {
                window.focus_prev(cx);
            } else {
                window.focus_next(cx);
            }
        }
    }
}
