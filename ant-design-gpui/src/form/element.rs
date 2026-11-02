// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::{
    cell::{Cell, RefCell},
    rc::Rc,
};

use gpui::{
    A11ySubtreeBuilder, AnyElement, App, Bounds, Element, ElementId, GlobalElementId,
    InspectorElementId, IntoElement, LayoutId, Pixels, Refineable, RenderOnce, Role, ScrollHandle,
    SharedString, Styled, Window, accesskit, point, px,
};

use super::FieldHandle;
use crate::{Direction, behavior::text::measured_paragraph_text};

/// Compose nowrap label fragments using native fractional glyph advances.
/// The shared paragraph still paints bidi text and native accessibility data.
pub(crate) struct FractionalText {
    pub text: SharedString,
    pub direction: Direction,
}
impl IntoElement for FractionalText {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}
impl Element for FractionalText {
    type RequestLayoutState = AnyElement;
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
    ) -> (LayoutId, AnyElement) {
        let metrics = Rc::new(Cell::new(None));
        let mut child =
            measured_paragraph_text(self.text.clone(), self.direction, false, metrics.clone());
        let child_layout = child.request_layout(window, cx);
        let preferred = metrics.get().map_or(px(0.), |metrics| metrics.preferred);
        let mut style = gpui::Style::default();
        style.size.width = preferred.into();
        (window.request_layout(style, [child_layout], cx), child)
    }
    fn prepaint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        _: Bounds<Pixels>,
        child: &mut AnyElement,
        window: &mut Window,
        cx: &mut App,
    ) {
        child.prepaint(window, cx);
    }
    fn paint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        _: Bounds<Pixels>,
        child: &mut AnyElement,
        _: &mut (),
        window: &mut Window,
        cx: &mut App,
    ) {
        child.paint(window, cx);
    }
}

/// Native layout rounds authored lengths to device pixels. Budget a literal
/// label as a whole so its text, mark and punctuation do not each contribute
/// a rounding error to every following field.
#[derive(IntoElement)]
pub(crate) struct IntrinsicLabel {
    pub element: gpui::Stateful<gpui::Div>,
    pub text: SharedString,
    pub punctuation: &'static str,
    pub margin: f32,
    pub mark: Option<IntrinsicLabelMark>,
}
pub(crate) struct IntrinsicLabelMark {
    pub text: SharedString,
    pub style: gpui::TextStyleRefinement,
    pub margin: f32,
}
impl RenderOnce for IntrinsicLabel {
    fn render(mut self, window: &mut Window, _: &mut App) -> impl IntoElement {
        let refinement = self.element.style();
        if refinement.size.width.is_none()
            && refinement.padding.left.is_none()
            && refinement.padding.right.is_none()
            && refinement.border_widths.left.is_none()
            && refinement.border_widths.right.is_none()
            && refinement.gap.width.is_none()
        {
            let mut style = window.text_style();
            style.refine(&self.element.style().text);
            let advance = |text: SharedString, style: &gpui::TextStyle| {
                window
                    .text_system()
                    .shape_line(
                        text.clone(),
                        style.font_size.to_pixels(window.rem_size()),
                        &[style.to_run(text.len())],
                        None,
                    )
                    .width()
            };
            let mut preferred = advance(
                crate::behavior::text::collapse_css_whitespace(&self.text),
                &style,
            ) + advance(self.punctuation.into(), &style)
                + px(self.margin);
            if let Some(mark) = self.mark {
                let mut mark_style = style.clone();
                mark_style.refine(&mark.style);
                preferred += advance(mark.text, &mark_style) + px(mark.margin);
            }
            self.element = self.element.w(preferred);
        }
        self.element
    }
}

/// A transparent adapter retains the actual semantic/layout identity while
/// publishing field geometry for application-owned scroll containers.
pub(super) type LayoutCallback = Rc<dyn Fn(Bounds<Pixels>, &mut Window, &mut App)>;

pub(crate) struct FormElement<E> {
    pub element: E,
    pub field: Option<FieldHandle>,
    pub required: bool,
    pub live: bool,
    pub on_layout: Option<LayoutCallback>,
    pub scroll: FormScrollTarget,
}

/// The mount owns the latest native scroll handle. Cached field elements share
/// this slot, so replacing the owner never leaves a stale handle in a scene.
#[derive(Clone, Default)]
pub(crate) struct FormScrollTarget(Option<Rc<RefCell<Option<ScrollHandle>>>>);
impl FormScrollTarget {
    pub fn new() -> Self {
        Self(Some(Rc::default()))
    }
    pub fn set(&self, handle: Option<ScrollHandle>) {
        if let Some(target) = &self.0 {
            *target.borrow_mut() = handle;
        }
    }
    pub fn same_source(&self, other: &Self) -> bool {
        match (&self.0, &other.0) {
            (Some(a), Some(b)) => Rc::ptr_eq(a, b),
            (None, None) => true,
            _ => false,
        }
    }
    fn reveal(&self, field: &FieldHandle, bounds: Bounds<Pixels>, window: &mut Window) -> bool {
        if self
            .0
            .as_ref()
            .is_none_or(|target| target.borrow().is_none())
        {
            return false;
        }
        let target = self.clone();
        let field = field.clone();
        window.on_next_frame(move |window, cx| {
            target.apply(field.bounds(cx).unwrap_or(bounds), window);
            window.refresh();
            // A field can gain help in the same frame as submit requests its
            // reveal. Cached intrinsic-height repair then moves later fields
            // and changes the owner's range. Recheck once after that layout.
            window.on_next_frame(move |window, cx| {
                if let Some(bounds) = field.bounds(cx) {
                    target.apply(bounds, window);
                }
            });
        });
        true
    }
    fn apply(&self, bounds: Bounds<Pixels>, window: &mut Window) {
        let Some(handle) = self.0.as_ref().and_then(|target| target.borrow().clone()) else {
            return;
        };
        let viewport = handle.bounds();
        let offset = handle.offset();
        let maximum = handle.max_offset();
        let reveal_axis =
            |start: Pixels, end: Pixels, near: Pixels, far: Pixels, old: Pixels, max: Pixels| {
                let delta = if end - start > far - near || start < near {
                    near - start
                } else if end > far {
                    far - end
                } else {
                    px(0.)
                };
                (old + delta).clamp(-max.max(px(0.)), px(0.))
            };
        let next = point(
            reveal_axis(
                bounds.left(),
                bounds.right(),
                viewport.left(),
                viewport.right(),
                offset.x,
                maximum.x,
            ),
            reveal_axis(
                bounds.top(),
                bounds.bottom(),
                viewport.top(),
                viewport.bottom(),
                offset.y,
                maximum.y,
            ),
        );
        if next != offset {
            handle.set_offset(next);
            window.refresh();
        }
    }
}

/// The shared additional area spends unused help margin *after* extra content.
/// Retain normal native sizing, but move the extra subtree into that unused
/// portion using its measured predecessor. Hitboxes and semantics move with it.
pub(crate) struct ExtraAfterHelp {
    pub child: AnyElement,
    pub help_height: Rc<Cell<Option<Pixels>>>,
    pub margin: Pixels,
}
impl IntoElement for ExtraAfterHelp {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}
impl Element for ExtraAfterHelp {
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
        _: Bounds<Pixels>,
        _: &mut (),
        window: &mut Window,
        cx: &mut App,
    ) {
        let remaining = (self.margin - self.help_height.get().unwrap_or(self.margin)).max(px(0.));
        window.with_element_offset(point(px(0.), -remaining), |window| {
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
        self.child.paint(window, cx);
    }
}
impl<E: Element> IntoElement for FormElement<E> {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}
impl<E: Element> Element for FormElement<E> {
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
        if self.required {
            node.set_required();
        }
        if self.live {
            node.set_live(accesskit::Live::Polite);
            node.set_live_atomic();
        }
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
        bounds: Bounds<Pixels>,
        state: &mut Self::RequestLayoutState,
        window: &mut Window,
        cx: &mut App,
    ) -> Self::PrepaintState {
        if let Some(field) = &self.field {
            field.set_bounds(bounds, cx);
        }
        if let Some(on_layout) = &self.on_layout {
            on_layout(bounds, window, cx);
        }
        let prepaint = self
            .element
            .prepaint(id, inspector, bounds, state, window, cx);
        if let Some(field) = &self.field
            && field.take_scroll_request(cx)
            && !self.scroll.reveal(field, bounds, window)
        {
            window.request_autoscroll(bounds);
        }
        prepaint
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
