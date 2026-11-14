// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::{geometry::*, *};
use crate::{
    ConfigScope, Direction, UiRoot,
    theme::{ComponentTheme, ComponentThemeConfig, ThemeConfig},
};
use gpui::{
    AppContext, Context, Entity, FocusHandle, IntoElement, KeyDownEvent, Keystroke, Modifiers,
    MouseButton, MouseDownEvent, MouseMoveEvent, MouseUpEvent, Pixels, PlatformInput, Point,
    Render, ScrollDelta, ScrollEvent, Subscription, TestAppContext, Window, WindowHandle, div,
    point, prelude::*, px, size,
};
use std::{cell::RefCell, rc::Rc};

#[test]
fn proportional_thumb_and_drag_reach_both_endpoints() {
    let track = gpui::Bounds::new(point(px(10.), px(20.)), size(px(12.), px(100.)));
    let bar = BarGeometry::new(track, true, px(100.), px(300.), px(-150.), px(24.)).unwrap();
    assert_eq!(bar.thumb.size.height, px(25.));
    assert_eq!(bar.thumb.top(), px(57.5));
    assert_eq!(
        bar.offset_for_pointer(point(px(10.), px(-100.)), px(10.)),
        px(0.)
    );
    assert_eq!(
        bar.offset_for_pointer(point(px(10.), px(1000.)), px(10.)),
        px(-300.)
    );
    assert_eq!(
        bar.offset_for_pointer(point(px(10.), px(67.5)), px(10.)),
        px(-150.)
    );
}

#[test]
fn tiny_tracks_bound_minimum_thumb_and_do_not_divide_by_zero() {
    let track = gpui::Bounds::new(point(px(0.), px(0.)), size(px(12.), px(8.)));
    let bar = BarGeometry::new(track, true, px(10.), px(1000.), px(-500.), px(24.)).unwrap();
    assert_eq!(bar.thumb.size.height, px(8.));
    assert_eq!(
        bar.offset_for_pointer(point(px(0.), px(7.)), px(2.)),
        px(0.)
    );
    assert!(BarGeometry::new(track, true, px(0.), px(1000.), px(0.), px(24.)).is_none());
    assert!(BarGeometry::new(track, true, px(100.), px(0.), px(0.), px(24.)).is_none());
}

#[test]
fn custom_visual_thickness_stays_inside_tiny_track_bounds() {
    let track = gpui::Bounds::new(point(px(20.), px(30.)), size(px(3.), px(8.)));
    let bar = BarGeometry::new(track, true, px(8.), px(1000.), px(-500.), px(24.)).unwrap();
    let visual = bar.visual_thumb(px(40.), Direction::Ltr);
    assert_eq!(visual.size.width, px(3.));
    assert_eq!(visual, bar.thumb);
    let track = gpui::Bounds::new(point(px(20.), px(30.)), size(px(8.), px(2.)));
    let bar = BarGeometry::new(track, false, px(8.), px(1000.), px(-500.), px(24.)).unwrap();
    let visual = bar.visual_thumb(px(40.), Direction::Ltr);
    assert_eq!(visual.size.height, px(2.));
    assert_eq!(visual, bar.thumb);
}

#[test]
fn thumb_expansion_keeps_the_qt_overlay_outer_edge() {
    let vertical = BarGeometry::new(
        gpui::Bounds::new(point(px(20.), px(30.)), size(px(12.), px(100.))),
        true,
        px(100.),
        px(300.),
        px(-150.),
        px(24.),
    )
    .unwrap();
    let horizontal = BarGeometry::new(
        gpui::Bounds::new(point(px(20.), px(30.)), size(px(100.), px(12.))),
        false,
        px(100.),
        px(300.),
        px(-150.),
        px(24.),
    )
    .unwrap();
    for thickness in [3., 5.5, 8., 40.] {
        for direction in [Direction::Ltr, Direction::Rtl] {
            let visual = vertical.visual_thumb(px(thickness), direction);
            if direction == Direction::Ltr {
                assert_eq!(visual.right(), vertical.track.right());
            } else {
                assert_eq!(visual.left(), vertical.track.left());
            }
            assert_eq!(visual.top(), vertical.thumb.top());
            assert_eq!(visual.size.width, px(thickness.min(12.)));
            let visual = horizontal.visual_thumb(px(thickness), direction);
            assert_eq!(visual.bottom(), horizontal.track.bottom());
            assert_eq!(visual.left(), horizontal.thumb.left());
            assert_eq!(visual.size.height, px(thickness.min(12.)));
        }
    }
}

#[test]
fn wheel_axis_contract_preserves_real_horizontal_and_shift_fallback() {
    let maximum = point(px(300.), px(400.));
    let down = point(px(0.), px(-20.));
    let right = point(px(-20.), px(0.));
    assert_eq!(wheel_delta(down, true, ScrollAxis::Both, maximum), right);
    assert_eq!(wheel_delta(right, true, ScrollAxis::Both, maximum), right);
    assert_eq!(wheel_delta(right, false, ScrollAxis::Both, maximum), right);
    assert_eq!(wheel_delta(down, true, ScrollAxis::Vertical, maximum), down);
    assert_eq!(
        wheel_delta(down, true, ScrollAxis::Both, point(px(0.), px(400.))),
        down
    );
    assert_eq!(
        wheel_delta(right, false, ScrollAxis::Vertical, maximum),
        point(px(0.), px(0.))
    );
}

struct Consumer {
    state: Entity<ScrollAreaState>,
    outer: Entity<ScrollAreaState>,
    scope: ConfigScope,
    axis: ScrollAxis,
    visibility: ScrollbarVisibility,
    nested: bool,
    shown: bool,
    clipped: bool,
    content_width: f32,
    content_height: f32,
    viewport_width: f32,
    viewport_height: f32,
    wrapping: bool,
    child_wheel: Rc<RefCell<usize>>,
    child_focus: FocusHandle,
    _subscription: Subscription,
}

impl Render for Consumer {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        let mut root = div().w(px(700.)).h(px(600.)).p(px(10.));
        if self.shown {
            let mut area = ScrollArea::new("area", &self.state)
                .scope(self.scope.clone())
                .axis(self.axis)
                .scrollbar_visibility(self.visibility)
                .w(px(self.viewport_width))
                .h(px(self.viewport_height))
                .line_height(px(37.))
                .accessibility_label("Documents")
                .automation_id("documents");
            area = if self.wrapping {
                area.child(div().w_full().text_size(px(14.)).line_height(px(20.)).child("A paragraph of text which wraps naturally to the measured viewport width. ".repeat(30)))
            } else {
                area.child(
                    div()
                        .w(px(self.content_width))
                        .h(px(self.content_height))
                        .flex_none()
                        .child(
                            div()
                                .id("wheel-child")
                                .w(px(60.))
                                .h(px(30.))
                                .track_focus(&self.child_focus)
                                .on_scroll({
                                    let count = self.child_wheel.clone();
                                    move |_, _, cx| {
                                        *count.borrow_mut() += 1;
                                        cx.stop_propagation();
                                    }
                                }),
                        ),
                )
            };
            if self.nested {
                root = root.child(
                    ScrollArea::new("outer", &self.outer)
                        .scope(self.scope.clone())
                        .w(px(400.))
                        .h(px(220.))
                        .child(area)
                        .child(div().h(px(600.)).flex_none()),
                );
            } else if self.clipped {
                root = root.child(
                    div()
                        .h(px(40.))
                        .overflow_hidden()
                        .child(div().mt(px(100.)).child(area)),
                );
            } else {
                root = root.child(area);
            }
        }
        root
    }
}

struct Fixture {
    cx: TestAppContext,
    window: WindowHandle<UiRoot>,
    consumer: Entity<Consumer>,
    state: Entity<ScrollAreaState>,
    outer: Entity<ScrollAreaState>,
    events: Rc<RefCell<Vec<ScrollAreaEvent>>>,
}

impl Fixture {
    fn new() -> Self {
        Self::with_initial(None)
    }

    fn with_initial(initial: Option<Point<Pixels>>) -> Self {
        let mut cx = TestAppContext::single();
        let mut result = None;
        let events = Rc::new(RefCell::new(Vec::new()));
        let output = events.clone();
        let window = cx.add_window(|window, cx| {
            window.activate_window();
            let scope = ConfigScope::new_window(cx);
            scope.update_patch(cx, |p| p.motion = Some(false)).unwrap();
            let state = cx.new(|cx| ScrollAreaState::new(window, cx));
            if let Some(offset) = initial {
                state.update(cx, |s, cx| s.scroll_to(offset, cx));
            }
            let outer = cx.new(|cx| ScrollAreaState::new(window, cx));
            let consumer = cx.new(|cx| Consumer {
                _subscription: cx.subscribe(&state, move |_, _, event, _| {
                    output.borrow_mut().push(*event)
                }),
                state: state.clone(),
                outer: outer.clone(),
                scope: scope.clone(),
                axis: ScrollAxis::Both,
                visibility: ScrollbarVisibility::Always,
                nested: false,
                shown: true,
                clipped: false,
                content_width: 560.,
                content_height: 360.,
                viewport_width: 180.,
                viewport_height: 120.,
                wrapping: false,
                child_wheel: Rc::default(),
                child_focus: cx.focus_handle().tab_stop(true),
            });
            result = Some((consumer.clone(), state, outer));
            UiRoot::new(scope, consumer, window, cx)
        });
        let (consumer, state, outer) = result.unwrap();
        let mut this = Self {
            cx,
            window,
            consumer,
            state,
            outer,
            events,
        };
        this.draw();
        this
    }

    fn draw(&mut self) {
        self.cx.run_until_parked();
        self.cx
            .update_window(self.window.into(), |_, w, cx| w.draw(cx).clear(cx))
            .unwrap();
        self.cx.run_until_parked();
    }
    fn event(&mut self, event: PlatformInput) {
        self.cx
            .update_window(self.window.into(), |_, w, cx| {
                w.dispatch_event(event, cx);
            })
            .unwrap();
        self.draw();
    }
    fn frame(&mut self) -> usize {
        let callbacks = self
            .cx
            .update_window(self.window.into(), |_, window, cx| {
                window.simulate_next_frame(cx)
            })
            .unwrap();
        self.draw();
        callbacks
    }
    fn move_mouse(&mut self, position: Point<Pixels>, dragging: bool) {
        self.event(PlatformInput::MouseMove(MouseMoveEvent {
            position,
            pressed_button: dragging.then_some(MouseButton::Left),
            ..Default::default()
        }));
    }
    fn wheel_at(&mut self, position: Point<Pixels>, delta: Point<Pixels>, shift: bool) {
        self.move_mouse(position, false);
        self.event(PlatformInput::Scroll(ScrollEvent {
            position,
            delta: ScrollDelta::Pixels(delta),
            modifiers: Modifiers {
                shift,
                ..Default::default()
            },
            ..Default::default()
        }));
    }
    fn wheel(&mut self, delta: Point<Pixels>, shift: bool) {
        self.wheel_at(point(px(90.), px(75.)), delta, shift);
    }
    fn offset(&self) -> Point<Pixels> {
        self.state.read_with(&self.cx, |s, _| s.offset())
    }
    fn maximum(&self) -> Point<Pixels> {
        self.state.read_with(&self.cx, |s, _| s.max_offset())
    }
    fn scroll(&mut self, offset: Point<Pixels>) {
        self.state
            .update(&mut self.cx, |s, cx| s.scroll_to(offset, cx));
        self.draw();
    }
    fn edit(&mut self, edit: impl FnOnce(&mut Consumer)) {
        self.consumer.update(&mut self.cx, |c, cx| {
            edit(c);
            cx.notify();
        });
        self.draw();
    }
    fn bar(&self, vertical: bool) -> BarGeometry {
        self.state
            .read_with(&self.cx, |s, _| s.bars[usize::from(vertical)].unwrap())
    }
    fn down(&mut self, position: Point<Pixels>) {
        self.move_mouse(position, false);
        self.event(PlatformInput::MouseDown(MouseDownEvent {
            position,
            button: MouseButton::Left,
            click_count: 1,
            ..Default::default()
        }));
    }
    fn up(&mut self, position: Point<Pixels>) {
        self.event(PlatformInput::MouseUp(MouseUpEvent {
            position,
            button: MouseButton::Left,
            ..Default::default()
        }));
    }
    fn focus(&mut self) {
        let state = self.state.clone();
        self.cx
            .update_window(self.window.into(), |_, w, cx| {
                state.update(cx, |s, cx| s.focus(w, cx))
            })
            .unwrap();
        self.draw();
    }
    fn key(&mut self, key: &'static str) {
        self.event(PlatformInput::KeyDown(KeyDownEvent {
            keystroke: Keystroke::parse(key).unwrap(),
            is_held: false,
            prefer_character_input: false,
        }));
    }
}

#[test]
fn native_geometry_shares_handle_and_clamps_programmatic_updates() {
    let mut f = Fixture::new();
    assert_eq!(f.maximum(), point(px(380.), px(240.)));
    f.scroll(point(px(-1000.), px(-1000.)));
    assert_eq!(f.offset(), point(px(-380.), px(-240.)));
    let event = *f.events.borrow().last().unwrap();
    assert_eq!(event.viewport_size, size(px(180.), px(120.)));
    assert_eq!(event.content_size, size(px(560.), px(360.)));
    assert_eq!(f.bar(true).maximum, px(240.));
    f.edit(|c| {
        c.content_height = 160.;
        c.content_width = 200.;
    });
    assert_eq!(f.offset(), point(px(-20.), px(-40.)));
    f.scroll(point(px(f32::NAN), px(f32::INFINITY)));
    assert_eq!(f.offset(), point(px(0.), px(0.)));
}

#[test]
fn initial_offsets_and_follow_bottom_resolve_against_native_layout() {
    let mut f = Fixture::with_initial(Some(point(px(-100.), px(-50.))));
    assert_eq!(f.offset(), point(px(-100.), px(-50.)));
    f.consumer.update(&mut f.cx, |c, cx| {
        c.content_height = 600.;
        cx.notify();
    });
    f.state.update(&mut f.cx, |s, cx| s.scroll_to_bottom(cx));
    f.draw();
    assert_eq!(f.offset().y, px(-480.));
    f.state.update(&mut f.cx, |s, cx| {
        s.scroll_to_bottom(cx);
        s.scroll_to_top(cx);
    });
    f.draw();
    assert_eq!(f.offset().y, px(0.));
    f.edit(|c| c.axis = ScrollAxis::Horizontal);
    f.state.update(&mut f.cx, |s, cx| s.scroll_to_bottom(cx));
    f.draw();
    assert_eq!(f.offset().y, px(0.));
}

#[test]
fn repeated_wheel_events_without_a_frame_stay_clamped_and_do_not_lose_deltas() {
    let mut f = Fixture::new();
    let position = point(px(90.), px(75.));
    f.move_mouse(position, false);
    for delta in [-100., -100., -100., 50.] {
        f.cx.update_window(f.window.into(), |_, window, cx| {
            window.dispatch_event(
                PlatformInput::Scroll(ScrollEvent {
                    position,
                    delta: ScrollDelta::Pixels(point(px(0.), px(delta))),
                    ..Default::default()
                }),
                cx,
            );
        })
        .unwrap();
    }
    assert_eq!(f.offset().y, px(-190.));
    f.draw();
    assert_eq!(f.offset().y, px(-190.));
}

#[test]
fn native_wheel_shift_and_horizontal_deltas_follow_reference() {
    let mut f = Fixture::new();
    f.wheel(point(px(0.), px(-30.)), true);
    assert_eq!(f.offset(), point(px(-30.), px(0.)));
    f.wheel(point(px(-20.), px(0.)), true);
    assert_eq!(f.offset(), point(px(-50.), px(0.)));
    f.wheel(point(px(0.), px(-40.)), false);
    assert_eq!(f.offset(), point(px(-50.), px(-40.)));
    f.edit(|c| {
        c.axis = ScrollAxis::Vertical;
        c.content_width = 180.;
    });
    f.scroll(point(px(0.), px(0.)));
    f.wheel(point(px(0.), px(-30.)), true);
    assert_eq!(f.offset(), point(px(0.), px(-30.)));
}

#[test]
fn line_wheel_uses_viewport_typography_and_accepts_repeated_events() {
    let mut f = Fixture::new();
    let position = point(px(90.), px(75.));
    f.move_mouse(position, false);
    for _ in 0..2 {
        f.event(PlatformInput::Scroll(ScrollEvent {
            position,
            delta: ScrollDelta::Lines(point(0., -1.)),
            ..Default::default()
        }));
    }
    assert_eq!(f.offset().y, px(-74.));
    f.event(PlatformInput::Scroll(ScrollEvent {
        position,
        delta: ScrollDelta::Lines(point(0., -1.)),
        modifiers: Modifiers {
            shift: true,
            ..Default::default()
        },
        ..Default::default()
    }));
    assert_eq!(f.offset(), point(px(-37.), px(-74.)));
}

#[test]
fn clicking_content_focuses_viewport_without_stealing_descendant_focus() {
    let mut f = Fixture::new();
    let blank = point(px(90.), px(75.));
    f.down(blank);
    f.up(blank);
    f.key("down");
    assert_eq!(f.offset().y, px(-40.));
    f.scroll(point(px(0.), px(0.)));
    let child = point(px(30.), px(20.));
    f.down(child);
    f.up(child);
    let focused = f.consumer.read_with(&f.cx, |c, _| c.child_focus.clone());
    f.cx.update_window(f.window.into(), |_, window, _| {
        assert!(focused.is_focused(window))
    })
    .unwrap();
    f.key("down");
    assert_eq!(f.offset().y, px(0.));
}

#[test]
fn unmount_invalidates_pending_decoration_frames() {
    let mut f = Fixture::new();
    let observed = f.state.clone();
    let notifications = Rc::new(std::cell::Cell::new(0usize));
    let output = notifications.clone();
    let _subscription =
        f.cx.update(|cx| cx.observe(&observed, move |_, _| output.set(output.get() + 1)));
    let state = f.state.clone();
    f.cx.update_window(f.window.into(), |_, window, cx| {
        state.update(cx, |s, cx| s.request_frame(window, cx))
    })
    .unwrap();
    f.edit(|c| c.shown = false);
    let after_unmount = notifications.get();
    f.frame();
    assert_eq!(notifications.get(), after_unmount);
    assert!(
        f.state
            .read_with(&f.cx, |s, _| !s.mounted && !s.frame_pending)
    );
}

#[test]
fn replacing_controller_under_the_same_id_rebinds_lifetime_and_notifications() {
    let mut f = Fixture::new();
    let old = f.state.clone();
    f.down(f.bar(true).thumb.center());
    let replacement =
        f.cx.update_window(f.window.into(), |_, window, cx| {
            cx.new(|cx| ScrollAreaState::new(window, cx))
        })
        .unwrap();
    f.edit(|c| c.state = replacement.clone());
    f.state = replacement;
    assert!(old.read_with(&f.cx, |s, _| !s.mounted && s.drag.is_none()));
    assert!(f.state.read_with(&f.cx, |s, _| s.mounted));
    f.scroll(point(px(-60.), px(-80.)));
    assert_eq!(f.offset(), point(px(-60.), px(-80.)));
    f.edit(|c| c.shown = false);
    assert!(f.state.read_with(&f.cx, |s, _| !s.mounted));
}

#[test]
fn nested_wheel_moves_only_inner_until_its_edge_then_bubbles() {
    let mut f = Fixture::new();
    f.edit(|c| c.nested = true);
    f.wheel(point(px(0.), px(-30.)), false);
    assert_eq!(f.offset().y, px(-30.));
    assert_eq!(f.outer.read_with(&f.cx, |s, _| s.offset().y), px(0.));
    f.scroll(point(px(0.), -f.maximum().y));
    f.wheel(point(px(0.), px(-30.)), false);
    assert_eq!(f.offset().y, -f.maximum().y);
    assert_eq!(f.outer.read_with(&f.cx, |s, _| s.offset().y), px(-30.));
    let before = f.offset().y;
    f.scroll(point(-f.maximum().x, before));
    f.wheel(point(px(0.), px(-30.)), true);
    assert_eq!(f.offset().y, before);
}

#[test]
fn descendant_wheel_handlers_and_clipping_are_respected() {
    let mut f = Fixture::new();
    f.wheel_at(point(px(30.), px(20.)), point(px(0.), px(-30.)), false);
    assert_eq!(f.offset(), point(px(0.), px(0.)));
    assert_eq!(
        f.consumer.read_with(&f.cx, |c, _| *c.child_wheel.borrow()),
        1
    );
    f.edit(|c| c.clipped = true);
    f.wheel_at(point(px(90.), px(175.)), point(px(0.), px(-30.)), false);
    assert_eq!(f.offset(), point(px(0.), px(0.)));
}

#[test]
fn thumb_drag_tracks_outside_and_release_ends_capture() {
    let mut f = Fixture::new();
    let thumb = f.bar(true).thumb;
    f.down(thumb.center());
    f.move_mouse(point(thumb.center().x, px(800.)), true);
    assert_eq!(f.offset().y, -f.maximum().y);
    f.up(point(px(800.), px(800.)));
    assert!(f.state.read_with(&f.cx, |s, _| s.drag.is_none()));
    f.move_mouse(thumb.center(), true);
    assert_eq!(f.offset().y, -f.maximum().y);
    let horizontal = f.bar(false).thumb;
    f.down(horizontal.center());
    f.move_mouse(point(px(800.), horizontal.center().y), true);
    f.up(point(px(800.), horizontal.center().y));
    assert_eq!(f.offset().x, -f.maximum().x);
}

#[test]
fn track_pages_keyboard_navigates_and_axis_changes_cancel_drag() {
    let mut f = Fixture::new();
    let track = f.bar(true).track;
    f.down(point(track.center().x, track.bottom() - px(1.)));
    f.up(track.center());
    assert_eq!(f.offset().y, px(-108.));
    f.focus();
    f.key("end");
    assert_eq!(f.offset().y, px(-240.));
    f.key("home");
    f.key("down");
    assert_eq!(f.offset().y, px(-40.));
    f.key("right");
    assert_eq!(f.offset().x, px(-40.));
    f.down(f.bar(true).thumb.center());
    f.edit(|c| c.axis = ScrollAxis::Horizontal);
    assert!(f.state.read_with(&f.cx, |s, _| s.drag.is_none()));
    assert_eq!(f.offset().y, px(0.));
}

#[test]
fn unmount_cancels_drag_and_retains_scroll_position() {
    let mut f = Fixture::new();
    f.scroll(point(px(-20.), px(-70.)));
    f.down(f.bar(true).thumb.center());
    f.edit(|c| c.shown = false);
    assert!(f.state.read_with(&f.cx, |s, _| s.drag.is_none()));
    f.edit(|c| c.shown = true);
    assert_eq!(f.offset(), point(px(-20.), px(-70.)));
}

#[test]
fn rtl_places_vertical_overlay_at_logical_end_without_changing_offsets() {
    let mut f = Fixture::new();
    f.consumer.update(&mut f.cx, |c, cx| {
        c.scope
            .update_patch(cx, |p| p.direction = Some(Direction::Rtl))
            .unwrap();
        cx.notify();
    });
    f.draw();
    let vertical = f.bar(true);
    let horizontal = f.bar(false);
    assert!(vertical.track.left() < px(20.));
    assert!(horizontal.track.left() >= vertical.track.right());
    f.wheel(point(px(-30.), px(0.)), false);
    assert_eq!(f.offset().x, px(-30.));
}

#[test]
fn width_fitting_measures_wrapping_at_current_width_and_short_content_has_no_bar() {
    let mut f = Fixture::new();
    f.edit(|c| {
        c.axis = ScrollAxis::Vertical;
        c.wrapping = true;
    });
    let narrow = f.maximum().y;
    assert!(narrow > px(0.));
    f.edit(|c| c.viewport_width = 360.);
    assert!(f.maximum().y < narrow);
    assert_eq!(f.maximum().x, px(0.));
    f.edit(|c| {
        c.wrapping = false;
        c.content_width = 100.;
        c.content_height = 50.;
    });
    assert_eq!(f.maximum(), point(px(0.), px(0.)));
    assert!(
        f.state
            .read_with(&f.cx, |s, _| s.bars.iter().all(Option::is_none))
    );
}

#[test]
fn hidden_bars_keep_scrolling_and_reduced_motion_leaves_no_pending_frame() {
    let mut f = Fixture::new();
    f.edit(|c| c.visibility = ScrollbarVisibility::Hidden);
    f.wheel(point(px(0.), px(-30.)), false);
    assert_eq!(f.offset().y, px(-30.));
    assert!(
        f.state
            .read_with(&f.cx, |s, _| s.bars.iter().all(Option::is_none)
                && !s.frame_pending)
    );
    f.edit(|c| {
        c.visibility = ScrollbarVisibility::Hover;
        c.viewport_height = 8.;
        c.axis = ScrollAxis::Vertical;
    });
    let bar = f.bar(true);
    assert!(bar.thumb.size.height <= bar.track.size.height);
}

#[test]
fn hover_visibility_starts_hidden_and_settles_without_an_idle_frame_loop() {
    let mut f = Fixture::new();
    f.edit(|c| c.shown = false);
    f.consumer.update(&mut f.cx, |c, cx| {
        c.scope
            .update_patch(cx, |patch| patch.motion = Some(true))
            .unwrap();
    });
    f.edit(|c| {
        c.visibility = ScrollbarVisibility::Hover;
        c.shown = true;
    });
    assert!(f.state.read_with(&f.cx, |s, cx| {
        s.visibility
            .sample(
                cx.background_executor()
                    .now()
                    .saturating_duration_since(s.epoch),
            )
            .value
            == 0.
            && !s.frame_pending
    }));
    f.move_mouse(point(px(90.), px(75.)), false);
    assert!(f.state.read_with(&f.cx, |s, _| s.frame_pending));
    f.cx.executor()
        .advance_clock(std::time::Duration::from_secs(1));
    f.frame();
    assert!(f.state.read_with(&f.cx, |s, cx| {
        s.visibility
            .sample(
                cx.background_executor()
                    .now()
                    .saturating_duration_since(s.epoch),
            )
            .value
            == 1.
            && !s.frame_pending
    }));
    assert_eq!(f.frame(), 0);
    f.move_mouse(point(px(650.), px(500.)), false);
    f.cx.executor()
        .advance_clock(std::time::Duration::from_secs(1));
    f.frame();
    assert!(f.state.read_with(&f.cx, |s, cx| {
        s.visibility
            .sample(
                cx.background_executor()
                    .now()
                    .saturating_duration_since(s.epoch),
            )
            .value
            == 0.
            && !s.frame_pending
    }));
    assert_eq!(f.frame(), 0);
}

#[test]
fn token_patches_inherit_and_sanitize_invalid_lengths() {
    let config = ThemeConfig::default();
    let resolved = config.resolve();
    let mut tokens = ScrollAreaTheme::default_tokens(&resolved.tokens);
    let defaults = tokens.clone();
    ScrollAreaTheme::apply(
        &mut tokens,
        &ScrollAreaTokenPatch {
            thickness: Some(f64::NAN),
            inset: Some(-2.),
            collapsed_thickness: Some(100.),
            ..Default::default()
        },
    );
    assert_eq!(tokens.thickness, defaults.thickness);
    assert_eq!(tokens.inset, defaults.inset);
    assert_eq!(tokens.collapsed_thickness, tokens.thickness);
    let merged = ScrollAreaTheme::merge(
        &ScrollAreaTokenPatch {
            thickness: Some(10.),
            ..Default::default()
        },
        &ScrollAreaTokenPatch {
            inset: Some(4.),
            ..Default::default()
        },
    );
    assert_eq!(merged.thickness, Some(10.));
    assert_eq!(merged.inset, Some(4.));
    let _ = ComponentThemeConfig::<ScrollAreaTheme>::new(merged);
}
