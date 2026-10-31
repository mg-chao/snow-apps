// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use gpui::{
    A11ySubtreeBuilder, App, Bounds, Element, ElementId, FocusHandle, Global, GlobalElementId,
    Hsla, InspectorElementId, InteractiveElement, IntoElement, KeyBinding, LayoutId, Pixels, Role,
    Window, accesskit, px,
};

gpui::actions!(ant_design, [FocusNext, FocusPrevious]);

struct Initialized;
impl Global for Initialized {}

/// Install the library's context-scoped traversal keys once per application.
///
/// Application bindings registered afterward may override these defaults.
pub fn initialize(cx: &mut App) {
    if cx.has_global::<Initialized>() {
        return;
    }
    cx.set_global(Initialized);
    cx.bind_keys([
        KeyBinding::new("tab", FocusNext, Some("AntDesign")),
        KeyBinding::new("shift-tab", FocusPrevious, Some("AntDesign")),
    ]);
}

/// Attach traversal handling to the window's library root.
///
/// Controls may handle Tab themselves (for example, an editor inserting indentation).
/// Actions reaching this root follow GPUI's existing tab groups and traversal order.
pub fn traversal<E: InteractiveElement>(element: E) -> E {
    element
        .key_context("AntDesign")
        .on_action(|_: &FocusNext, window, cx| window.focus_next(cx))
        .on_action(|_: &FocusPrevious, window, cx| window.focus_prev(cx))
}

/// Create a retained handle participating in normal source-order keyboard traversal.
///
/// Set traversal properties on the handle itself when using `track_focus`: div properties
/// only configure the handles that GPUI creates automatically.
pub fn focus_handle(cx: &mut App) -> FocusHandle {
    cx.focus_handle().tab_index(0).tab_stop(true)
}

/// A token-colored focus indicator which does not alter a control's layout.
#[derive(Clone, Copy, Debug)]
pub struct FocusRing {
    pub color: Hsla,
    pub width: Pixels,
    pub offset: Pixels,
    pub radius: Pixels,
}

impl FocusRing {
    pub fn new(color: impl Into<Hsla>, width: impl Into<Pixels>) -> Self {
        Self {
            color: color.into(),
            width: width.into(),
            offset: px(1.),
            radius: px(0.),
        }
    }

    pub fn with_offset(mut self, offset: impl Into<Pixels>) -> Self {
        self.offset = offset.into();
        self
    }

    pub fn radius(mut self, radius: impl Into<Pixels>) -> Self {
        self.radius = radius.into();
        self
    }

    /// Use GPUI's input-modality tracking, including its programmatic-focus semantics.
    ///
    /// Programmatic focus preserves the preceding input modality. Mouse focus remains
    /// quiet; keyboard navigation reveals the ring without changing the layout bounds.
    pub fn apply<E: IntoElement>(
        self,
        element: E,
        focus: &FocusHandle,
    ) -> FocusRingElement<E::Element> {
        FocusRingElement {
            element: element.into_element(),
            focus: focus.clone(),
            ring: self,
            corners: None,
        }
    }
}

/// A focus outline painted outside the control with a transparent gap.
///
/// The adapter preserves the control's ID, semantics, dimensions and existing shadows.
/// Ancestor clipping remains in force, as it does for other GPUI drawing.
pub struct FocusRingElement<E> {
    element: E,
    focus: FocusHandle,
    ring: FocusRing,
    corners: Option<gpui::Corners<Pixels>>,
}

impl<E> FocusRingElement<E> {
    /// Follow a styled control's individual corners without changing its layout.
    pub fn corners(mut self, corners: gpui::Corners<Pixels>) -> Self {
        self.corners = Some(corners);
        self
    }
}

impl<E: Element> IntoElement for FocusRingElement<E> {
    type Element = Self;

    fn into_element(self) -> Self {
        self
    }
}

impl<E: Element> Element for FocusRingElement<E> {
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
        if self.focus.is_focused(window)
            && window.last_input_was_keyboard()
            && f32::from(self.ring.width).is_finite()
            && self.ring.width > px(0.)
        {
            let offset = finite_nonnegative(self.ring.offset);
            let extent = offset + self.ring.width;
            // CSS resolves oversized radii against the painted box. GPUI's
            // quad shader accepts raw radii, so clamp before expanding an outline
            // (notably pill buttons and controls narrower than their height).
            let corners = self
                .corners
                .unwrap_or_else(|| gpui::Corners::all(self.ring.radius))
                .map(|radius| finite_nonnegative(*radius))
                .clamp_radii_for_quad_size(bounds.size)
                .map(|radius| *radius + extent);
            window.paint_quad(
                gpui::outline(bounds.dilate(extent), self.ring.color, Default::default())
                    .border_widths(self.ring.width)
                    .corner_radii(corners),
            );
        }
    }
}

fn finite_nonnegative(value: Pixels) -> Pixels {
    if f32::from(value).is_finite() && value > px(0.) {
        value
    } else {
        px(0.)
    }
}
