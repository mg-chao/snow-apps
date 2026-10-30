// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use gpui::{
    AnyElement, App, AvailableSpace, Bounds, Element, ElementId, Entity, GlobalElementId, Hitbox,
    HitboxBehavior, InspectorElementId, IntoElement, KeyDownEvent, KeyUpEvent, LayoutId,
    MouseButton, MouseDownEvent, MouseMoveEvent, MouseUpEvent, Pixels, Point, Refineable, Window,
    div, prelude::*, px, relative, size,
};

use super::{
    PopupGeometry, PopupOpenReason, PopupTriggers,
    host::{self, Entry},
    runtime::{PopupState, finite_nonnegative, update_interest},
    shadow,
    shape::ArrowShape,
};
use crate::{Direction, behavior::Accessibility};

/// A layout-transparent adapter. Returning the trigger's own layout ID preserves
/// block sizing, flex growth, margins and native hit bounds without a wrapper div.
pub(super) struct PopupElement {
    pub id: ElementId,
    pub child: AnyElement,
    pub state: Entity<PopupState>,
}

impl IntoElement for PopupElement {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}

impl Element for PopupElement {
    type RequestLayoutState = ();
    type PrepaintState = Hitbox;
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
        let owner = crate::modal::host::owner(window, cx);
        self.state.update(cx, |s, _| s.modal_owner = owner);
        let state = self.state.downgrade();
        (
            super::trigger::with_trigger(&state, cx, |cx| self.child.request_layout(window, cx)),
            (),
        )
    }
    fn prepaint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        _: &mut (),
        window: &mut Window,
        cx: &mut App,
    ) -> Hitbox {
        let focus = self.state.read(cx).trigger_focus.clone();
        window.set_focus_handle(&focus, cx);
        let hitbox = window.insert_hitbox(bounds, HitboxBehavior::Normal);
        self.child.prepaint(window, cx);
        let clipped = bounds.intersect(&window.content_mask().bounds);
        let visible = !clipped.is_empty()
            && self.state.read(cx).presented()
            && crate::modal::host::popup_allowed(self.state.read(cx).modal_owner, window, cx);
        let opacity = self.state.update(cx, |state, cx| {
            state.anchor = bounds;
            state.sample_opacity(visible, window, cx)
        });
        let exiting = self.state.read(cx).exiting;
        if visible || exiting {
            let (host, content, handle, options, scope, surface_focus) = {
                let state = self.state.read(cx);
                (
                    state.host.clone(),
                    state.content.clone(),
                    state.handle.clone(),
                    state.options.clone(),
                    state.scope.clone(),
                    state.surface_focus.clone(),
                )
            };
            if let Some(content) = content {
                let config = scope.snapshot(cx);
                let tokens = &config.theme.tokens;
                let background = options
                    .background
                    .unwrap_or(tokens.color_bg_elevated.into());
                let radius = finite_nonnegative(
                    options.radius.unwrap_or(px(tokens.border_radius_lg as f32)),
                );
                let weak = self.state.downgrade();
                let content = host::with_parent(&host, self.state.entity_id(), || {
                    content(&handle, window, cx)
                });
                let mut body = div()
                    .id("surface")
                    .track_focus(&surface_focus)
                    .tab_group()
                    .occlude()
                    .bg(background)
                    .rounded(radius)
                    .p(px(12.))
                    .max_w(window.viewport_size().width)
                    .max_h(window.viewport_size().height)
                    .overflow_y_scroll()
                    .font(config.font.clone())
                    .text_size(px(tokens.font_size as f32))
                    .line_height(relative(tokens.line_height as f32))
                    .text_color(tokens.color_text)
                    .when_some(options.on_popup_click.clone(), |body, callback| {
                        body.on_click(move |event, window, cx| callback(event, window, cx))
                    })
                    .when(config.direction == Direction::Rtl, |element| {
                        element.text_right()
                    })
                    .on_hover(move |hovered, window, cx| {
                        update_interest(&weak, window, cx, |state| {
                            if state.options.triggers.contains(PopupTriggers::HOVER) {
                                state.lifecycle.hover(
                                    true,
                                    *hovered,
                                    state.options.enter_delay,
                                    state.options.leave_delay,
                                );
                            }
                        });
                    })
                    .child(content);
                body.style().refine(&options.surface_style);
                if exiting {
                    // Retain native pixels during the fade, while a top hitbox
                    // prevents the closing subtree from accepting another gesture.
                    body = body.relative().child(
                        div()
                            .absolute()
                            .top(px(0.))
                            .left(px(0.))
                            .right(px(0.))
                            .bottom(px(0.))
                            .occlude()
                            .on_mouse_down(MouseButton::Left, |_, _, cx| cx.stop_propagation())
                            .on_mouse_up(MouseButton::Left, |_, _, cx| cx.stop_propagation()),
                    );
                    let state = self.state.read(cx);
                    if state.surface_focus.contains_focused(window, cx) {
                        let focus = state
                            .trigger_metadata
                            .borrow()
                            .focus
                            .clone()
                            .unwrap_or_else(|| state.trigger_focus.clone());
                        window.focus(&focus, cx);
                    }
                }
                if let Some(margin) = options.viewport_margin {
                    body = body
                        .max_w((window.viewport_size().width - 2. * margin).max(px(0.)))
                        .max_h((window.viewport_size().height - 2. * margin).max(px(0.)));
                }
                body = match options.width {
                    super::PopupWidth::Content => body,
                    super::PopupWidth::Trigger => body.w(bounds.size.width),
                    super::PopupWidth::AtLeastTrigger => body.min_w(bounds.size.width),
                    super::PopupWidth::TriggerMinimum(width) => {
                        body.w(bounds.size.width.max(finite_nonnegative(width)))
                    }
                    super::PopupWidth::Fixed(width) => body.w(finite_nonnegative(width)),
                };
                if options.unique_group.is_some() {
                    // The shared background moves independently; new content
                    // retains its final layout and never re-registers old input.
                    body = body.bg(gpui::rgba(0)).opacity(opacity.1);
                }
                let mut semantics = Accessibility::new(options.role);
                semantics.label = options.label.clone();
                semantics.description = options.description.clone();
                semantics.busy = options.busy;
                semantics.automation_id = options.automation_id.clone();
                if exiting {
                    semantics.role = Some(gpui::Role::Group);
                    semantics.hidden = true;
                }
                let surface = PopupSurface {
                    child: semantics.apply(body).into_any_element(),
                    state: self.state.clone(),
                    background,
                    radius,
                    clipped_anchor: clipped,
                }
                .into_any_element();
                // Opacity on a native ancestor applies to the shadow, vector
                // arrow and complete arbitrary content subtree in one pass.
                let mut surface = div().opacity(opacity.0).child(surface).into_any_element();
                host::with_parent(&host, self.state.entity_id(), || {
                    surface.layout_as_root(
                        size(AvailableSpace::MaxContent, AvailableSpace::MaxContent),
                        window,
                        cx,
                    );
                });
                let priority = {
                    let state = self.state.read(cx);
                    if exiting {
                        options.z_index.saturating_mul(1 << 20)
                    } else {
                        host.borrow_mut().register(
                            state.id,
                            Entry {
                                state: self.state.downgrade(),
                                parent: state.parent,
                                trigger: clipped,
                                surface: state
                                    .layout
                                    .map_or(Bounds::default(), |layout| layout.bounds),
                                layer: options.z_index,
                                order: 0,
                            },
                        )
                    }
                };
                let priority = crate::modal::host::popup_priority(
                    self.state.read(cx).modal_owner,
                    priority,
                    window,
                    cx,
                );
                window.defer_draw(surface, Point::default(), priority, None);
            }
        } else {
            self.state.update(cx, |state, _| {
                state.layout = None;
                state.visual_layout = None;
                state.host.borrow_mut().entries.remove(&state.id);
            });
        }
        hitbox
    }
    fn paint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        _: Bounds<Pixels>,
        _: &mut (),
        hitbox: &mut Hitbox,
        window: &mut Window,
        cx: &mut App,
    ) {
        let weak = self.state.downgrade();
        let hover_hitbox = hitbox.clone();
        window.on_mouse_event(move |_: &MouseMoveEvent, phase, window, cx| {
            if phase.capture() {
                let hovered = hover_hitbox.is_hovered(window);
                update_interest(&weak, window, cx, |state| {
                    if state.options.triggers.contains(PopupTriggers::HOVER) {
                        state.lifecycle.hover(
                            false,
                            hovered,
                            state.options.enter_delay,
                            state.options.leave_delay,
                        );
                    }
                });
            }
        });
        let weak = self.state.downgrade();
        let down_hitbox = hitbox.clone();
        window.on_mouse_event(move |event: &MouseDownEvent, phase, window, cx| {
            if phase.capture() {
                let inside = down_hitbox.is_hovered_at(event.position, window);
                update_interest(&weak, window, cx, |state| {
                    state.pointer_down = inside && event.button == MouseButton::Left;
                    if inside
                        && event.button == MouseButton::Right
                        && state.trigger_metadata.borrow().can_activate()
                        && state.options.triggers.contains(PopupTriggers::CONTEXT_MENU)
                    {
                        state.context_position = Some(event.position);
                        state.lifecycle.request(true, PopupOpenReason::ContextMenu);
                    }
                });
            }
        });
        let weak = self.state.downgrade();
        window.on_key_event(move |event: &KeyDownEvent, phase, window, cx| {
            if phase.capture() {
                let _ = weak.update(cx, |state, cx| {
                    if !state.surface_focus.contains_focused(window, cx)
                        && !event.is_held
                        && !event.keystroke.modifiers.modified()
                        && matches!(event.keystroke.key.as_str(), "enter" | "space")
                    {
                        state.keyboard_down = Some(event.keystroke.key.clone().into());
                    }
                });
            }
        });
        let weak = self.state.downgrade();
        window.on_key_event(move |event: &KeyUpEvent, phase, window, cx| {
            if phase.capture() {
                update_interest(&weak, window, cx, |state| {
                    let armed = state.keyboard_down.take();
                    if armed.as_deref() == Some(event.keystroke.key.as_str())
                        && !event.keystroke.modifiers.modified()
                        && state.options.triggers.contains(PopupTriggers::CLICK)
                        && state.trigger_metadata.borrow().can_activate()
                    {
                        state.lifecycle.toggle();
                    }
                });
            }
        });
        self.child.paint(window, cx);
        let weak = self.state.downgrade();
        let up_hitbox = hitbox.clone();
        window.on_mouse_event(move |event: &MouseUpEvent, phase, window, cx| {
            if phase.bubble() && event.button == MouseButton::Left {
                let inside = up_hitbox.is_hovered_at(event.position, window);
                update_interest(&weak, window, cx, |state| {
                    let pressed = std::mem::take(&mut state.pointer_down);
                    if pressed
                        && inside
                        && state.options.triggers.contains(PopupTriggers::CLICK)
                        && state.trigger_metadata.borrow().can_activate()
                    {
                        state.lifecycle.toggle();
                    }
                });
            }
        });
    }
}

struct PopupSurface {
    child: AnyElement,
    state: Entity<PopupState>,
    background: gpui::Hsla,
    radius: Pixels,
    clipped_anchor: Bounds<Pixels>,
}

impl IntoElement for PopupSurface {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}

impl Element for PopupSurface {
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
        // An earlier trigger in this frame may already have scheduled its draw
        // when a later unique tooltip replaces it. Skip both input and paint.
        if !self.state.read(cx).presented() && !self.state.read(cx).exiting {
            return;
        }
        let state = self.state.read(cx);
        let config = state.scope.snapshot(cx);
        let options = &state.options;
        let anchor = state.context_position.map_or(state.anchor, |position| {
            Bounds::new(position, size(px(0.), px(0.)))
        });
        let mut layout = PopupGeometry {
            anchor,
            popup_size: bounds.size,
            viewport: Bounds::new(Point::default(), window.viewport_size()),
            placement: options.placement,
            arrow: options.arrow,
            arrow_size: options
                .arrow_size
                .unwrap_or(px(config.theme.tokens.size_popup_arrow as f32)),
            gap: options
                .gap
                .unwrap_or(px(config.theme.tokens.margin_xxs as f32)),
            radius: options.alignment_radius.unwrap_or(self.radius),
            offset: options.offset,
            auto_adjust_overflow: options.auto_adjust_overflow,
        }
        .resolve();
        if let Some(flipped_gap) = options.flipped_gap
            && layout.placement.before() != options.placement.before()
        {
            let original_gap = options
                .gap
                .unwrap_or(px(config.theme.tokens.margin_xxs as f32));
            let offset =
                (flipped_gap - original_gap) * if layout.placement.before() { -1. } else { 1. };
            if layout.placement.is_vertical() {
                layout.bounds.origin.y += offset;
            } else {
                layout.bounds.origin.x += offset;
            }
        }
        if let Some(margin) = options.viewport_margin {
            let contain = |origin: Pixels, extent: Pixels, viewport: Pixels| {
                let spare = (viewport - extent).max(px(0.));
                let inset = margin.min(spare / 2.);
                origin.clamp(inset, spare - inset)
            };
            layout.bounds.origin.x = contain(
                layout.bounds.origin.x,
                layout.bounds.size.width,
                window.viewport_size().width,
            );
            layout.bounds.origin.y = contain(
                layout.bounds.origin.y,
                layout.bounds.size.height,
                window.viewport_size().height,
            );
        }
        if layout.arrow.is_some()
            && let Some(attachment) = &options.arrow_attachment
        {
            layout.arrow = attachment(&layout, cx)
                .filter(|point| f32::from(point.x).is_finite() && f32::from(point.y).is_finite());
        }
        let host = state.host.clone();
        let id = state.id;
        if let Some(entry) = host.borrow_mut().entries.get_mut(&id) {
            entry.surface = layout.bounds;
            entry.trigger = self.clipped_anchor;
        }
        self.state.update(cx, |state, cx| {
            state.layout = Some(layout);
            state.visual_layout = Some(state.sample_background(layout, window, cx));
        });
        host::with_parent(&host, id, || {
            self.child.prepaint_at(layout.bounds.origin, window, cx);
        });
        let focus = self.state.update(cx, |state, _| {
            if std::mem::take(&mut state.focus_pending) {
                state.options.initial_focus.clone()
            } else {
                None
            }
        });
        if let Some(focus) = focus {
            let weak = self.state.downgrade();
            window.defer(cx, move |window, cx| {
                if weak
                    .upgrade()
                    .is_some_and(|state| state.read(cx).lifecycle.open())
                {
                    window.focus(&focus, cx);
                }
            });
        }
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
        if !self.state.read(cx).presented() && !self.state.read(cx).exiting {
            return;
        }
        let state = self.state.read(cx);
        let host = state.host.clone();
        let id = state.id;
        let config = state.scope.snapshot(cx);
        let g = &config.theme.tokens;
        let arrow = ArrowShape::new(
            state
                .options
                .arrow_size
                .unwrap_or(px(g.size_popup_arrow as f32)),
            state
                .options
                .arrow_tip_radius
                .unwrap_or(px(g.border_radius_xs as f32)),
            state
                .options
                .arrow_outer_radius
                .unwrap_or(px(g.border_radius_outer as f32)),
        );
        let shadows = state
            .options
            .shadows
            .clone()
            .unwrap_or_else(|| g.drop_shadow_popover.clone());
        let unique = state.options.unique_group.is_some();
        if let Some(layout) = state.visual_layout {
            shadow::paint(
                layout,
                self.radius,
                arrow,
                self.background.a,
                shadows,
                window,
                cx,
            );
            if unique {
                window.paint_quad(
                    gpui::fill(layout.bounds, self.background).corner_radii(self.radius),
                );
            }
            if let Some(path) = arrow.path(layout) {
                window.paint_path(path, self.background);
            }
        }
        host::with_parent(&host, id, || self.child.paint(window, cx));
    }
}
