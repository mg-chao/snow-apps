// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! A scrollbar for an already scrollable native element. The supplied handle
//! remains authoritative; this element never creates a second scroll viewport.

use super::geometry::BarGeometry;
use crate::{
    ConfigScope, Direction,
    behavior::{Accessibility, Activation, ActivationGuard, Availability, FocusRing, focus_handle},
    locale::MessageKey,
};
use gpui::{
    AnyElement, App, AvailableSpace, Bounds, ContentMask, Context, DispatchPhase, Element,
    ElementId, Entity, FocusHandle, GlobalElementId, InspectorElementId, IntoElement, LayoutId,
    MouseButton, MouseMoveEvent, MouseUpEvent, PathBuilder, Pixels, RenderOnce, Role, ScrollHandle,
    Subscription, WeakEntity, Window, accesskit, canvas, div, point, prelude::*, px, size,
};
use std::rc::Rc;

const WIDTH: f32 = 10.;
const ARROW_RESERVE: f32 = 12.;
const THUMB_WIDTH: f32 = 6.;
const MINIMUM_THUMB: f32 = 11.;
const LINE_STEP: f32 = 40.;

type ScrollCallback = Rc<dyn Fn(&mut Window, &mut App)>;
type GeometryCallback = Rc<dyn Fn(&ExternalScrollbarGeometry, &mut App)>;

/// Painted scrollbar geometry, measured against the supplied native scroll handle.
#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct ExternalScrollbarGeometry {
    pub rail: Bounds<Pixels>,
    pub track: Bounds<Pixels>,
    pub thumb: Bounds<Pixels>,
    pub decrement: Bounds<Pixels>,
    pub increment: Bounds<Pixels>,
    pub viewport_bounds: Bounds<Pixels>,
    pub viewport: Pixels,
    pub content: Pixels,
    pub maximum: Pixels,
    /// GPUI's native negative vertical offset.
    pub offset: Pixels,
    pub visible: bool,
}

/// An absolute physical-edge overlay. Keep the handle stable for each mounted ID.
#[derive(IntoElement)]
pub(crate) struct ExternalScrollbar {
    id: ElementId,
    handle: ScrollHandle,
    scope: Option<ConfigScope>,
    disabled: Option<bool>,
    metrics: Option<(f32, f32)>,
    on_scroll: Option<ScrollCallback>,
    on_geometry: Option<GeometryCallback>,
}

impl ExternalScrollbar {
    pub fn new(id: impl Into<ElementId>, handle: ScrollHandle) -> Self {
        Self {
            id: id.into(),
            handle,
            scope: None,
            disabled: None,
            metrics: None,
            on_scroll: None,
            on_geometry: None,
        }
    }
    pub fn scope(mut self, scope: ConfigScope) -> Self {
        self.scope = Some(scope);
        self
    }
    pub fn disabled(mut self, disabled: bool) -> Self {
        self.disabled = Some(disabled);
        self
    }
    /// Optional exact CSS-equivalent range, bounded by the native handle's measured range.
    pub fn metrics(mut self, viewport: f32, content: f32) -> Self {
        if viewport.is_finite() && viewport > 0. && content.is_finite() && content >= 0. {
            self.metrics = Some((viewport, content));
        }
        self
    }
    pub fn on_scroll(mut self, callback: impl Fn(&mut Window, &mut App) + 'static) -> Self {
        self.on_scroll = Some(Rc::new(callback));
        self
    }
    pub fn on_geometry(
        mut self,
        callback: impl Fn(&ExternalScrollbarGeometry, &mut App) + 'static,
    ) -> Self {
        self.on_geometry = Some(Rc::new(callback));
        self
    }
}

struct ExternalState {
    scope: ConfigScope,
    handle: ScrollHandle,
    disabled: Option<bool>,
    focus: FocusHandle,
    guard: ActivationGuard,
    arrow_guards: [ActivationGuard; 2],
    bar: Option<BarGeometry>,
    grab: Option<Pixels>,
    on_scroll: Option<ScrollCallback>,
    _scope: Subscription,
    _subscriptions: Vec<Subscription>,
}

impl ExternalState {
    fn new(
        scope: ConfigScope,
        handle: ScrollHandle,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        let focus = focus_handle(cx);
        let subscriptions = vec![
            cx.on_focus(&focus, window, |_, _, cx| cx.notify()),
            cx.on_blur(&focus, window, |state, _, cx| {
                state.cancel();
                cx.notify();
            }),
            cx.observe_window_activation(window, |state, window, cx| {
                if !window.is_window_active() {
                    state.cancel();
                    cx.notify();
                }
            }),
            cx.observe_window_visibility(window, |state, visibility, _, cx| {
                if !visibility.is_visible() {
                    state.cancel();
                    cx.notify();
                }
            }),
        ];
        Self {
            _scope: Self::observe_scope(&scope, window, cx),
            scope,
            handle,
            disabled: None,
            focus,
            guard: ActivationGuard::default(),
            arrow_guards: std::array::from_fn(|_| ActivationGuard::default()),
            bar: None,
            grab: None,
            on_scroll: None,
            _subscriptions: subscriptions,
        }
    }

    fn observe_scope(
        scope: &ConfigScope,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Subscription {
        scope.observe_in(window, cx, |state, window, cx| {
            state.sync(window, cx);
            cx.notify();
        })
    }

    fn cancel(&mut self) {
        self.grab = None;
        self.guard.cancel();
        for guard in &self.arrow_guards {
            guard.cancel();
        }
    }

    fn sync(&mut self, window: &mut Window, cx: &mut App) {
        let mut availability = self.scope.snapshot(cx).availability(self.disabled, false);
        availability.disabled |= self.bar.is_none();
        self.guard.update(availability, &self.focus, window, cx);
        if availability.disabled {
            self.cancel();
        }
        let offset = self.handle.offset().y;
        let maximum = self.bar.map_or(px(0.), |bar| bar.maximum);
        for (index, guard) in self.arrow_guards.iter().enumerate() {
            guard.set_availability(Availability {
                disabled: availability.disabled
                    || if index == 0 {
                        offset >= px(0.)
                    } else {
                        offset <= -maximum
                    },
                busy: false,
            });
        }
    }

    fn scroll_to(&mut self, offset: Pixels, cx: &mut Context<Self>) -> Option<ScrollCallback> {
        if !self.guard.availability().can_activate() || !f32::from(offset).is_finite() {
            return None;
        }
        let maximum = self.bar.map_or(px(0.), |bar| bar.maximum);
        let offset = offset.clamp(-maximum, px(0.));
        let previous = self.handle.offset();
        if previous.y == offset {
            return None;
        }
        self.handle.set_offset(point(previous.x, offset));
        cx.notify();
        self.on_scroll.clone()
    }

    fn scroll_by(&mut self, delta: Pixels, cx: &mut Context<Self>) -> Option<ScrollCallback> {
        self.scroll_to(self.handle.offset().y + delta, cx)
    }
}

impl Drop for ExternalState {
    fn drop(&mut self) {
        self.cancel();
        let unavailable = Availability {
            disabled: true,
            busy: false,
        };
        self.guard.set_availability(unavailable);
        for guard in &self.arrow_guards {
            guard.set_availability(unavailable);
        }
    }
}

impl RenderOnce for ExternalScrollbar {
    fn render(self, window: &mut Window, cx: &mut App) -> impl IntoElement {
        let scope = self.scope.unwrap_or_else(|| ConfigScope::application(cx));
        let state =
            window.use_keyed_state((self.id.clone(), "external-scrollbar"), cx, |window, cx| {
                ExternalState::new(scope.clone(), self.handle.clone(), window, cx)
            });
        state.update(cx, |state, cx| {
            if state.scope != scope {
                state.cancel();
                state._scope = ExternalState::observe_scope(&scope, window, cx);
                state.scope = scope.clone();
            }
            state.handle = self.handle;
            state.disabled = self.disabled;
            state.on_scroll = self.on_scroll;
            state.sync(window, cx);
        });
        let direction = scope.snapshot(cx).direction;
        let inner = div()
            .id("external-scrollbar-slot")
            .absolute()
            .top_0()
            .bottom_0()
            .w(px(WIDTH))
            .when(direction == Direction::Ltr, |slot| slot.right_0())
            .when(direction == Direction::Rtl, |slot| slot.left_0())
            .into_any_element();
        ExternalElement {
            id: self.id,
            state,
            inner,
            scope,
            metrics: self.metrics,
            on_geometry: self.on_geometry,
        }
    }
}

struct ExternalElement {
    id: ElementId,
    state: Entity<ExternalState>,
    inner: AnyElement,
    scope: ConfigScope,
    metrics: Option<(f32, f32)>,
    on_geometry: Option<GeometryCallback>,
}

impl IntoElement for ExternalElement {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}

fn geometry(
    rail: Bounds<Pixels>,
    viewport_bounds: Bounds<Pixels>,
    viewport: Pixels,
    maximum: Pixels,
    offset: Pixels,
) -> (ExternalScrollbarGeometry, Option<BarGeometry>) {
    let arrow = px(ARROW_RESERVE).min(rail.size.height.max(px(0.)) / 2.);
    let track = Bounds::new(
        rail.origin + point(px(0.), arrow),
        size(rail.size.width, (rail.size.height - 2. * arrow).max(px(0.))),
    );
    let bar = BarGeometry::new(track, true, viewport, maximum, offset, px(MINIMUM_THUMB)).map(
        |mut bar| {
            // Native thin-scrollbar painting quantizes thumb size and position in logical pixels.
            let height = px(f32::from(bar.thumb.size.height).round()).min(track.size.height);
            let travel = track.size.height - height;
            let position = px(f32::from(travel * (-offset / maximum).clamp(0., 1.)).floor());
            bar.thumb = Bounds::new(
                track.origin + point(px(0.), position),
                size(track.size.width, height),
            );
            bar
        },
    );
    let thumb = bar.map_or(Bounds::new(track.origin, size(px(0.), px(0.))), |bar| {
        let width = px(THUMB_WIDTH).min(rail.size.width.max(px(0.)));
        Bounds::new(
            point(
                rail.left() + (rail.size.width - width) / 2.,
                bar.thumb.top(),
            ),
            size(width, bar.thumb.size.height),
        )
    });
    (
        ExternalScrollbarGeometry {
            rail,
            track,
            thumb,
            decrement: Bounds::new(rail.origin, size(rail.size.width, arrow)),
            increment: Bounds::new(
                point(rail.left(), rail.bottom() - arrow),
                size(rail.size.width, arrow),
            ),
            viewport_bounds,
            viewport,
            content: viewport + maximum,
            maximum,
            offset,
            visible: bar.is_some(),
        },
        bar,
    )
}

impl Element for ExternalElement {
    type RequestLayoutState = ();
    type PrepaintState = Option<AnyElement>;
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
        (self.inner.request_layout(window, cx), ())
    }
    fn prepaint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        _: &mut (),
        window: &mut Window,
        cx: &mut App,
    ) -> Option<AnyElement> {
        self.inner.prepaint(window, cx);
        let handle = self.state.read(cx).handle.clone();
        let native_bounds = handle.bounds();
        let viewport = px(self
            .metrics
            .map_or(f32::from(bounds.size.height), |metrics| metrics.0));
        let native_maximum = handle.max_offset().y.max(px(0.));
        let mut maximum = self.metrics.map_or(native_maximum, |(_, content)| {
            (px(content) - viewport).max(px(0.))
        });
        if native_bounds.size.height > px(0.) {
            maximum = maximum.min(native_maximum);
        }
        let offset = if f32::from(handle.offset().y).is_finite() {
            handle.offset().y.clamp(-maximum, px(0.))
        } else {
            px(0.)
        };
        // Derive proportional thumb geometry from the authored viewport. Its
        // native layout box can differ by a fraction of a device pixel; using
        // that rounded extent before quantizing the thumb's position can move
        // it by an entire logical pixel.
        let geometry_bounds = if self.metrics.is_some() && bounds.size.height > px(0.) {
            Bounds::new(bounds.origin, size(bounds.size.width, viewport))
        } else {
            bounds
        };
        let (mut report, mut bar) =
            geometry(geometry_bounds, native_bounds, viewport, maximum, offset);
        report.rail = bounds;
        // Quad painting snaps global edges. Keep the hit target and drag
        // geometry at those same edges rather than snapping an offset again.
        report.thumb = Bounds::from_corners(
            window.pixel_snap_point(report.thumb.origin),
            window.pixel_snap_point(report.thumb.bottom_right()),
        );
        if let Some(bar) = &mut bar {
            bar.thumb.origin.y = report.thumb.top();
            bar.thumb.size.height = report.thumb.size.height;
        }
        self.state.update(cx, |state, cx| {
            if state
                .bar
                .is_some_and(|previous| previous.maximum != maximum)
            {
                state.cancel();
            }
            state.bar = bar;
            if state.handle.offset().y != offset {
                state
                    .handle
                    .set_offset(point(state.handle.offset().x, offset));
                cx.notify();
            }
            state.sync(window, cx);
        });
        if let Some(callback) = &self.on_geometry {
            callback(&report, cx);
        }
        if !report.visible {
            return None;
        }
        let mut rail = render_rail(&self.state, report, &self.scope, cx);
        rail.layout_as_root(
            size(
                AvailableSpace::Definite(bounds.size.width),
                AvailableSpace::Definite(bounds.size.height),
            ),
            window,
            cx,
        );
        window.with_content_mask(Some(ContentMask { bounds }), |window| {
            rail.prepaint_at(bounds.origin, window, cx)
        });
        Some(rail)
    }
    fn paint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        _: &mut (),
        rail: &mut Option<AnyElement>,
        window: &mut Window,
        cx: &mut App,
    ) {
        self.inner.paint(window, cx);
        if let Some(rail) = rail {
            window.with_content_mask(Some(ContentMask { bounds }), |window| {
                rail.paint(window, cx)
            });
        }
        let target = self.state.downgrade();
        window.on_mouse_event(move |event: &MouseMoveEvent, phase, window, cx| {
            if phase != DispatchPhase::Capture {
                return;
            }
            let callback = target
                .update(cx, |state, cx| {
                    state.sync(window, cx);
                    let grab = state.grab?;
                    if event.pressed_button != Some(MouseButton::Left) || !window.is_window_active()
                    {
                        state.cancel();
                        return None;
                    }
                    let bar = state.bar?;
                    state.scroll_to(bar.offset_for_pointer(event.position, grab), cx)
                })
                .ok()
                .flatten();
            if let Some(callback) = callback {
                callback(window, cx);
            }
        });
        let target = self.state.downgrade();
        window.on_mouse_event(move |event: &MouseUpEvent, phase, _, cx| {
            if phase == DispatchPhase::Capture && event.button == MouseButton::Left {
                let _ = target.update(cx, |state, _| state.grab = None);
            }
        });
    }
}

fn run_scroll(
    state: &WeakEntity<ExternalState>,
    value: impl FnOnce(&ExternalState) -> Pixels,
    window: &mut Window,
    cx: &mut App,
) {
    let callback = state
        .update(cx, |state, cx| {
            state.sync(window, cx);
            let offset = value(state);
            state.scroll_to(offset, cx)
        })
        .ok()
        .flatten();
    if let Some(callback) = callback {
        callback(window, cx);
    }
}

fn render_rail(
    state: &Entity<ExternalState>,
    geometry: ExternalScrollbarGeometry,
    scope: &ConfigScope,
    cx: &App,
) -> AnyElement {
    let config = scope.snapshot(cx);
    let g = &config.theme.tokens;
    let retained = state.read(cx);
    let focus = retained.focus.clone();
    let disabled = !retained.guard.availability().can_activate();
    let arrow_guards = retained.arrow_guards.clone();
    let target = state.downgrade();
    let key_target = state.downgrade();
    let wheel_target = state.downgrade();
    let mut rail = div()
        .id("rail")
        .relative()
        .w(geometry.rail.size.width)
        .h(geometry.rail.size.height)
        .cursor_default()
        .aria_orientation(accesskit::Orientation::Vertical)
        .aria_numeric_value(f64::from(f32::from(-geometry.offset)))
        .aria_min_numeric_value(0.)
        .aria_max_numeric_value(f64::from(f32::from(geometry.maximum)))
        .when(!disabled, |rail| rail.track_focus(&focus))
        .on_mouse_down(MouseButton::Left, move |event, window, cx| {
            let callback = target
                .update(cx, |state, cx| {
                    state.sync(window, cx);
                    if !state.guard.availability().can_activate() {
                        return None;
                    }
                    state.focus.focus(window, cx);
                    let bar = state.bar?;
                    if bar.thumb.contains(&event.position) {
                        state.grab = Some(bar.coordinate(event.position) - bar.thumb_start());
                        None
                    } else {
                        let before = event.position.y < bar.thumb.top();
                        state.scroll_by(geometry.viewport * if before { 0.9 } else { -0.9 }, cx)
                    }
                })
                .ok()
                .flatten();
            if let Some(callback) = callback {
                callback(window, cx);
            }
            window.prevent_default();
            cx.stop_propagation();
        })
        .on_key_down(move |event, window, cx| {
            let key = event.keystroke.key.as_str();
            if event.keystroke.modifiers.modified()
                || !matches!(
                    key,
                    "up" | "down" | "pageup" | "pagedown" | "home" | "end" | "space"
                )
            {
                return;
            }
            run_scroll(
                &key_target,
                |state| match key {
                    "up" => state.handle.offset().y + px(LINE_STEP),
                    "down" => state.handle.offset().y - px(LINE_STEP),
                    "pageup" => state.handle.offset().y + geometry.viewport * 0.9,
                    "pagedown" | "space" => state.handle.offset().y - geometry.viewport * 0.9,
                    "home" => px(0.),
                    _ => -geometry.maximum,
                },
                window,
                cx,
            );
            window.prevent_default();
            cx.stop_propagation();
        })
        .on_scroll(move |event, window, cx| {
            let delta = event.delta.pixel_delta(px(20.)).y;
            run_scroll(
                &wheel_target,
                |state| state.handle.offset().y + delta,
                window,
                cx,
            );
            window.prevent_default();
            cx.stop_propagation();
        });
    let thumb_color = g.color_text_tertiary;
    let thumb = canvas(
        |_, _, _| (),
        move |_, _, window, _| {
            // Geometry is already in window coordinates. Laying out another
            // positioned box would snap its relative offset a second time at
            // fractional scales and move the thumb away from its hit target.
            let mut quad = gpui::fill(geometry.thumb, thumb_color.to_hsla());
            quad.corner_radii = px(THUMB_WIDTH / 2.).into();
            window.paint_quad(quad);
        },
    )
    .absolute()
    .inset_0();
    rail = rail.child(thumb);
    for (index, part) in [geometry.decrement, geometry.increment]
        .into_iter()
        .enumerate()
    {
        let guard = arrow_guards[index].clone();
        let unavailable = !guard.availability().can_activate();
        let down_target = state.downgrade();
        let click_target = state.downgrade();
        let delta = px(if index == 0 { LINE_STEP } else { -LINE_STEP });
        let color = g.color_text_tertiary;
        let arrow = div()
            .id(("arrow", index))
            .absolute()
            .top(part.top() - geometry.rail.top())
            .left_0()
            .w(part.size.width)
            .h(part.size.height)
            .cursor_default()
            .on_mouse_down(MouseButton::Left, move |_, window, cx| {
                if !unavailable {
                    if let Ok(focus) = down_target.read_with(cx, |state, _| state.focus.clone()) {
                        focus.focus(window, cx);
                    }
                    run_scroll(
                        &down_target,
                        |state| state.handle.offset().y + delta,
                        window,
                        cx,
                    );
                }
                window.prevent_default();
                cx.stop_propagation();
            })
            .child(
                canvas(
                    |_, _, _| (),
                    move |_, _, window, _| {
                        let mut path = PathBuilder::fill();
                        let center_x = part.center().x;
                        let (base, tip) = if index == 0 {
                            (part.top() + px(7.), part.top() + px(2.))
                        } else {
                            (part.bottom() - px(7.), part.bottom() - px(2.))
                        };
                        path.move_to(point(center_x - px(3.), base));
                        path.line_to(point(center_x + px(3.), base));
                        path.line_to(point(center_x, tip));
                        path.close();
                        if let Ok(path) = path.build() {
                            window.paint_path(path, color.to_hsla());
                        }
                    },
                )
                .size_full(),
            );
        let arrow = guard.install(arrow, move |activation, window, cx| {
            if activation != Activation::Pointer {
                run_scroll(
                    &click_target,
                    |state| state.handle.offset().y + delta,
                    window,
                    cx,
                );
            }
            cx.stop_propagation();
        });
        rail = rail.child(
            Accessibility {
                disabled: unavailable,
                ..Accessibility::new(Role::Button)
            }
            .label(if index == 0 {
                config.locale.common.decrease_value.clone()
            } else {
                config.locale.common.increase_value.clone()
            })
            .apply(arrow),
        );
    }
    for (action, delta) in [
        (accesskit::Action::Increment, -LINE_STEP),
        (accesskit::Action::Decrement, LINE_STEP),
        (accesskit::Action::ScrollDown, -LINE_STEP),
        (accesskit::Action::ScrollUp, LINE_STEP),
    ] {
        let target = state.downgrade();
        rail = rail.on_a11y_action(action, move |_, window, cx| {
            run_scroll(
                &target,
                |state| state.handle.offset().y + px(delta),
                window,
                cx,
            );
        });
    }
    let target = state.downgrade();
    rail = rail.on_a11y_action(accesskit::Action::SetValue, move |data, window, cx| {
        if let Some(accesskit::ActionData::NumericValue(value)) = data
            && value.is_finite()
        {
            run_scroll(
                &target,
                |_| px(-value.max(0.).min(f32::MAX as f64) as f32),
                window,
                cx,
            );
        }
    });
    let target = state.downgrade();
    rail = rail.on_a11y_action(
        accesskit::Action::SetScrollOffset,
        move |data, window, cx| {
            if let Some(accesskit::ActionData::SetScrollOffset(value)) = data
                && value.y.is_finite()
            {
                run_scroll(
                    &target,
                    |_| px(-value.y.max(0.).min(f32::MAX as f64) as f32),
                    window,
                    cx,
                );
            }
        },
    );
    let semantics = Accessibility {
        disabled,
        ..Accessibility::new(Role::ScrollBar)
    }
    .label(config.locale.text(MessageKey::VerticalScrollBar).clone());
    FocusRing::new(g.color_primary_border, px(g.line_width_focus as f32))
        .apply(semantics.apply(rail), &focus)
        .into_any_element()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::scroll_area::{ExternalScrollbar, ExternalScrollbarGeometry};
    use gpui::{
        KeyDownEvent, Keystroke, Modifiers, MouseDownEvent, PlatformInput, Render, ScrollDelta,
        ScrollEvent, ScrollSource, TestAppContext, TouchPhase, VisualTestContext, WindowHandle,
        WindowVisibility,
    };
    use std::cell::{Cell, RefCell};

    #[test]
    fn external_scrollbar_matches_independently_measured_chrome_thin_geometry() {
        let rail = Bounds::new(point(px(78.), px(76.)), size(px(10.), px(216.)));
        let viewport = Bounds::new(point(px(32.), px(76.)), size(px(46.), px(216.)));
        for (content, offset, top, height) in [
            (860., 364., 169., 48.),
            (1868., 672., 157., 22.),
            (1868., 980., 188., 22.),
            (244., 28., 110., 170.),
            (28188., 0., 88., 11.),
        ] {
            let (geometry, bar) =
                geometry(rail, viewport, px(216.), px(content - 216.), px(-offset));
            assert_eq!(
                geometry.thumb,
                Bounds::new(point(px(80.), px(top)), size(px(6.), px(height)))
            );
            assert_eq!(geometry.decrement.size.height, px(12.));
            assert_eq!(geometry.increment.size.height, px(12.));
            assert_eq!(geometry.track.size.height, px(192.));
            assert_eq!(geometry.content, px(content));
            let bar = bar.unwrap();
            assert_eq!(
                bar.offset_for_pointer(point(px(80.), px(-100.)), px(3.)),
                px(0.)
            );
            assert_eq!(
                bar.offset_for_pointer(point(px(80.), px(1000.)), px(3.)),
                px(216. - content)
            );
        }
    }

    #[test]
    fn external_scrollbar_bounds_tiny_viewports_and_zero_ranges() {
        for height in [0., 8., 20., 24.] {
            let bounds = Bounds::new(point(px(0.), px(0.)), size(px(10.), px(height)));
            let (geometry, bar) = geometry(bounds, bounds, px(height), px(1000.), px(-10.));
            assert!(!geometry.visible);
            assert!(bar.is_none());
            assert!(geometry.decrement.bottom() <= geometry.increment.top());
        }
        let bounds = Bounds::new(point(px(0.), px(0.)), size(px(10.), px(100.)));
        assert!(!geometry(bounds, bounds, px(100.), px(0.), px(0.)).0.visible);
    }

    struct Consumer {
        scope: ConfigScope,
        root_focus: FocusHandle,
        handle: ScrollHandle,
        report: Rc<RefCell<Option<ExternalScrollbarGeometry>>>,
        changes: Rc<Cell<usize>>,
        visible: bool,
        disabled: Option<bool>,
        canonical_content: f32,
        native_content: f32,
        viewport: f32,
    }

    impl Consumer {
        fn new(window: &mut Window, cx: &mut Context<Self>) -> Self {
            window.activate_window();
            let root_focus = cx.focus_handle().tab_stop(false);
            window.focus(&root_focus, cx);
            Self {
                scope: ConfigScope::new_window(cx),
                root_focus,
                handle: ScrollHandle::new(),
                report: Rc::default(),
                changes: Rc::default(),
                visible: true,
                disabled: None,
                canonical_content: 400.,
                native_content: 400.,
                viewport: 100.,
            }
        }
    }

    impl Render for Consumer {
        fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
            let mut root = crate::behavior::traversal(
                div()
                    .id("external-test-root")
                    .track_focus(&self.root_focus)
                    .relative()
                    .w(px(100.))
                    .h(px(self.viewport)),
            )
            .child(
                div()
                    .id("native-viewport")
                    .w_full()
                    .h_full()
                    .overflow_y_scroll()
                    .track_scroll(&self.handle)
                    .child(div().flex_none().w_full().h(px(self.native_content))),
            );
            if self.visible {
                let report = self.report.clone();
                let changes = self.changes.clone();
                let mut scrollbar = ExternalScrollbar::new("external-test", self.handle.clone())
                    .scope(self.scope.clone())
                    .metrics(self.viewport, self.canonical_content)
                    .on_geometry(move |geometry, _| *report.borrow_mut() = Some(*geometry))
                    .on_scroll(move |_, _| changes.set(changes.get() + 1));
                if let Some(disabled) = self.disabled {
                    scrollbar = scrollbar.disabled(disabled);
                }
                root = root.child(scrollbar);
            }
            root
        }
    }

    struct Fixture {
        cx: VisualTestContext,
        consumer: Entity<Consumer>,
        _window: WindowHandle<Consumer>,
    }

    impl Fixture {
        fn new() -> Self {
            let mut cx = TestAppContext::single();
            let window = cx.add_window(Consumer::new);
            let consumer = window.root(&mut cx).unwrap();
            let mut fixture = Self {
                cx: VisualTestContext::from_window(window.into(), &cx),
                consumer,
                _window: window,
            };
            fixture.cx.simulate_resize(size(px(320.), px(180.)));
            fixture.draw();
            fixture
        }
        fn draw(&mut self) {
            self.cx.run_until_parked();
            self.cx.update(|window, cx| window.draw(cx).clear(cx));
            self.cx.run_until_parked();
        }
        fn geometry(&self) -> ExternalScrollbarGeometry {
            self.consumer
                .read_with(&self.cx, |state, _| state.report.borrow().unwrap())
        }
        fn offset(&self) -> Pixels {
            self.consumer
                .read_with(&self.cx, |state, _| state.handle.offset().y)
        }
        fn down(&mut self, position: gpui::Point<Pixels>) {
            self.cx.simulate_event(MouseDownEvent {
                button: MouseButton::Left,
                position,
                ..Default::default()
            });
            self.draw();
        }
        fn drag(&mut self, position: gpui::Point<Pixels>) {
            self.cx.simulate_event(MouseMoveEvent {
                pressed_button: Some(MouseButton::Left),
                position,
                ..Default::default()
            });
            self.draw();
        }
        fn up(&mut self, position: gpui::Point<Pixels>) {
            self.cx.simulate_event(MouseUpEvent {
                button: MouseButton::Left,
                position,
                ..Default::default()
            });
            self.draw();
        }
        fn key(&mut self, stroke: &'static str) {
            self.cx.update(|window, cx| {
                window.dispatch_event(
                    PlatformInput::KeyDown(KeyDownEvent {
                        keystroke: Keystroke::parse(stroke).unwrap(),
                        is_held: false,
                        prefer_character_input: false,
                    }),
                    cx,
                )
            });
            self.draw();
        }
    }

    #[test]
    fn external_scrollbar_uses_authored_viewport_before_quantizing_thumb_position() {
        let mut fixture = Fixture::new();
        fixture.cx.simulate_resize(size(px(320.), px(400.)));
        fixture.cx.simulate_scale_factor_change(1.5);
        fixture.consumer.update(&mut fixture.cx, |state, cx| {
            state.viewport = 231.;
            state.native_content = 1883.;
            state.canonical_content = 1883.;
            cx.notify();
        });
        fixture.draw();
        fixture.consumer.update(&mut fixture.cx, |state, cx| {
            state.handle.set_offset(point(px(0.), px(-672.)));
            cx.notify();
        });
        fixture.draw();
        let geometry = fixture.geometry();
        assert_eq!(geometry.viewport, px(231.));
        assert!(geometry.rail.size.height < geometry.viewport);
        // Independent browser probe: the 231px/1883px viewport at offset672
        // puts the thumb 74px into its track, after the 12px arrow reserve.
        assert_eq!(geometry.track.size.height, px(207.));
        assert_eq!(geometry.thumb.top(), px(86.));
    }

    #[test]
    fn external_scrollbar_fractional_scale_thumb_paint_stays_inside_its_hit_geometry() {
        let mut fixture = Fixture::new();
        fixture.consumer.update(&mut fixture.cx, |state, cx| {
            state.handle.set_offset(point(px(0.), px(-40.)));
            cx.notify();
        });
        for scale in [1., 1.25, 1.5, 2.] {
            fixture.cx.simulate_scale_factor_change(scale);
            fixture.draw();
            let expected = fixture.geometry().thumb;
            fixture.cx.update(|window, _| {
                let error = window
                    .rendered_scene()
                    .quads
                    .iter()
                    .map(|quad| {
                        (quad.bounds.origin.x.0 / scale - f32::from(expected.left())).abs()
                            + (quad.bounds.origin.y.0 / scale - f32::from(expected.top())).abs()
                            + (quad.bounds.size.width.0 / scale - f32::from(expected.size.width))
                                .abs()
                            + (quad.bounds.size.height.0 / scale - f32::from(expected.size.height))
                                .abs()
                    })
                    .min_by(f32::total_cmp)
                    .expect("painted scrollbar thumb");
                assert!(
                    error < 0.03,
                    "thumb paint diverged from its hit geometry at {scale}x by {error}px"
                );
            });
        }
    }

    #[test]
    fn external_scrollbar_shares_native_offsets_for_arrows_drag_and_keyboard() {
        let mut fixture = Fixture::new();
        let geometry = fixture.geometry();
        assert!(geometry.visible);
        assert_eq!(geometry.rail.left(), px(90.));
        assert_eq!(geometry.maximum, px(300.));
        assert_eq!(geometry.viewport_bounds.size, size(px(100.), px(100.)));
        fixture
            .cx
            .simulate_click(geometry.increment.center(), Modifiers::none());
        fixture.draw();
        assert_eq!(fixture.offset(), px(-40.), "arrow click scrolls once");
        let focus = fixture.cx.update(|window, cx| window.focused(cx).unwrap());
        fixture
            .consumer
            .update(&mut fixture.cx, |_, cx| cx.notify());
        fixture.draw();
        fixture.cx.update(|window, cx| {
            assert_eq!(window.focused(cx), Some(focus));
        });
        fixture.key("home");
        let track = fixture.geometry().track;
        fixture.cx.simulate_click(
            point(track.center().x, track.bottom() - px(1.)),
            Modifiers::none(),
        );
        fixture.draw();
        assert_eq!(fixture.offset(), px(-90.), "track click pages once");
        fixture.cx.simulate_event(ScrollEvent {
            position: track.center(),
            delta: ScrollDelta::Pixels(point(px(0.), px(-17.))),
            modifiers: Modifiers::none(),
            touch_phase: TouchPhase::Moved,
            source: ScrollSource::Pointer,
        });
        fixture.draw();
        assert_eq!(
            fixture.offset(),
            px(-107.),
            "rail wheel uses the same handle"
        );
        let geometry = fixture.geometry();
        fixture.down(geometry.thumb.center());
        fixture.drag(point(geometry.thumb.center().x, px(300.)));
        fixture.up(point(geometry.thumb.center().x, px(300.)));
        assert_eq!(fixture.offset(), px(-300.));
        fixture.key("home");
        assert_eq!(fixture.offset(), px(0.));
        fixture.key("pagedown");
        assert_eq!(fixture.offset(), px(-90.));
        fixture.key("end");
        assert_eq!(fixture.offset(), px(-300.));
        assert!(
            fixture
                .consumer
                .read_with(&fixture.cx, |state, _| state.changes.get())
                >= 4
        );
        fixture
            .cx
            .update(|window, cx| assert_eq!(window.simulate_next_frame(cx), 0));
    }

    #[test]
    fn external_scrollbar_honors_canonical_tail_and_rtl_physical_edge() {
        let mut fixture = Fixture::new();
        fixture.consumer.update(&mut fixture.cx, |state, cx| {
            state.canonical_content = 300.;
            state
                .scope
                .update_patch(cx, |patch| patch.direction = Some(Direction::Rtl))
                .unwrap();
            cx.notify();
        });
        fixture.draw();
        let geometry = fixture.geometry();
        assert_eq!(geometry.rail.left(), px(0.));
        assert_eq!(geometry.maximum, px(200.));
        fixture.down(geometry.thumb.center());
        fixture.drag(point(geometry.thumb.center().x, px(300.)));
        fixture.up(point(geometry.thumb.center().x, px(300.)));
        assert_eq!(fixture.offset(), px(-200.));
        fixture.consumer.read_with(&fixture.cx, |state, _| {
            assert_eq!(
                state.handle.max_offset().y,
                px(300.),
                "native range remains untouched"
            );
        });
    }

    #[test]
    fn external_scrollbar_cancels_drag_when_disabled_or_unmounted() {
        let mut fixture = Fixture::new();
        let geometry = fixture.geometry();
        fixture.down(geometry.thumb.center());
        fixture.consumer.update(&mut fixture.cx, |state, cx| {
            state.disabled = Some(true);
            cx.notify();
        });
        fixture.draw();
        fixture.consumer.update(&mut fixture.cx, |state, cx| {
            state.disabled = Some(false);
            cx.notify();
        });
        fixture.draw();
        fixture.drag(point(geometry.thumb.center().x, px(300.)));
        fixture.up(point(geometry.thumb.center().x, px(300.)));
        assert_eq!(fixture.offset(), px(0.));
        fixture.down(fixture.geometry().thumb.center());
        fixture.consumer.update(&mut fixture.cx, |state, cx| {
            state.visible = false;
            cx.notify();
        });
        fixture.draw();
        fixture.drag(point(geometry.thumb.center().x, px(300.)));
        fixture.up(point(geometry.thumb.center().x, px(300.)));
        assert_eq!(fixture.offset(), px(0.));
    }

    #[test]
    fn external_scrollbar_live_scope_focus_and_window_changes_cancel_drag() {
        let mut fixture = Fixture::new();
        let scope = fixture
            .consumer
            .read_with(&fixture.cx, |state, _| state.scope.clone());
        let thumb = fixture.geometry().thumb;
        fixture.down(thumb.center());
        fixture.cx.update(|_, cx| {
            scope
                .update_patch(cx, |patch| patch.disabled = Some(true))
                .unwrap();
        });
        fixture.cx.run_until_parked();
        fixture.cx.update(|_, cx| {
            scope
                .update_patch(cx, |patch| patch.disabled = Some(false))
                .unwrap();
        });
        fixture.draw();
        fixture.drag(point(thumb.center().x, px(300.)));
        fixture.up(point(thumb.center().x, px(300.)));
        assert_eq!(fixture.offset(), px(0.), "live scope edges cancel the grab");

        fixture.down(thumb.center());
        let root_focus = fixture
            .consumer
            .read_with(&fixture.cx, |state, _| state.root_focus.clone());
        fixture.cx.update(|window, cx| root_focus.focus(window, cx));
        fixture.draw();
        fixture.drag(point(thumb.center().x, px(300.)));
        fixture.up(point(thumb.center().x, px(300.)));
        assert_eq!(fixture.offset(), px(0.), "focus departure cancels the grab");

        fixture.down(thumb.center());
        let other = fixture.cx.add_window(|window, _| {
            window.activate_window();
            gpui::EmptyView
        });
        fixture.draw();
        fixture.cx.update(|window, _| {
            assert!(!window.is_window_active());
            window.activate_window();
        });
        fixture.draw();
        fixture.drag(point(thumb.center().x, px(300.)));
        fixture.up(point(thumb.center().x, px(300.)));
        assert_eq!(
            fixture.offset(),
            px(0.),
            "reactivation cannot resume a grab"
        );
        other
            .update(&mut fixture.cx, |_, window, _| window.remove_window())
            .unwrap();

        fixture.down(thumb.center());
        fixture
            .cx
            .simulate_visibility_change(WindowVisibility::Hidden);
        fixture.cx.run_until_parked();
        fixture
            .cx
            .simulate_visibility_change(WindowVisibility::Visible);
        fixture.draw();
        fixture.drag(point(thumb.center().x, px(300.)));
        fixture.up(point(thumb.center().x, px(300.)));
        assert_eq!(fixture.offset(), px(0.), "visibility edges cancel the grab");

        fixture.down(thumb.center());
        fixture.drag(point(thumb.center().x, px(300.)));
        fixture.up(point(thumb.center().x, px(300.)));
        assert_eq!(fixture.offset(), px(-300.), "a fresh drag still works");
    }
}
