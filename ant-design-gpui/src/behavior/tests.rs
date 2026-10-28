// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::{cell::RefCell, rc::Rc, time::Duration};

use gpui::{
    AnimationExt, Context, Element, FocusHandle, IntoElement, KeyDownEvent, KeyUpEvent,
    KeyboardButton, Keystroke, MouseButton, MouseDownEvent, MouseUpEvent, PlatformInput, Render,
    Role, StatefulInteractiveElement, Subscription, TestAppContext, Window, WindowHandle,
    accesskit, div, point, prelude::*, px,
};

use super::*;
use crate::theme::CubicBezier;

fn linear() -> CubicBezier {
    CubicBezier {
        x1: 0.,
        y1: 0.,
        x2: 1.,
        y2: 1.,
    }
}

#[test]
fn semantic_adapter_preserves_identity_and_native_properties() {
    let element = Accessibility {
        role: Some(Role::Button),
        disabled: true,
        busy: true,
        invalid: true,
        read_only: true,
        current: Some(accesskit::AriaCurrent::Page),
        ..Accessibility::default()
    }
    .label("Save")
    .description("Save this document")
    .automation_id("save-document")
    .apply(div().id("save").aria_expanded(true).on_click(|_, _, _| {}));
    assert_eq!(element.id(), Some("save".into()));
    assert_eq!(element.a11y_role(), Some(Role::Button));
    let mut node = accesskit::Node::new(element.a11y_role().unwrap());
    element.write_a11y_info(&mut node);
    assert_eq!(node.label(), Some("Save"));
    assert_eq!(node.description(), Some("Save this document"));
    assert_eq!(node.author_id(), Some("save-document"));
    assert_eq!(node.is_expanded(), Some(true));
    assert!(node.is_disabled());
    assert!(node.is_busy());
    assert!(node.is_read_only());
    assert_eq!(node.aria_current(), Some(accesskit::AriaCurrent::Page));
    assert_eq!(node.invalid(), Some(accesskit::Invalid::True));
    assert!(!node.supports_action(accesskit::Action::Click));
    assert!(!node.supports_action(accesskit::Action::Focus));
}

#[test]
fn busy_semantics_keep_focus_action_but_hide_activation() {
    let element = Accessibility {
        busy: true,
        ..Accessibility::new(Role::Button)
    }
    .apply(div().id("busy").focusable().on_click(|_, _, _| {}));
    let mut node = accesskit::Node::new(Role::Button);
    element.write_a11y_info(&mut node);
    assert!(node.supports_action(accesskit::Action::Focus));
    assert!(!node.supports_action(accesskit::Action::Click));
    assert!(!node.is_disabled());
}

#[test]
fn cubic_bezier_solves_time_axis_and_preserves_overshoot() {
    let ease = CubicBezier {
        x1: 0.42,
        y1: 0.,
        x2: 0.58,
        y2: 1.,
    };
    assert_eq!(evaluate_cubic_bezier(ease, 0.), 0.);
    assert_eq!(evaluate_cubic_bezier(ease, 1.), 1.);
    assert!((evaluate_cubic_bezier(ease, 0.5) - 0.5).abs() < 1e-8);
    assert!(evaluate_cubic_bezier(ease, 0.25) < 0.25);
    assert!(evaluate_cubic_bezier(ease, 0.75) > 0.75);
    let overshoot = CubicBezier {
        x1: 0.2,
        y1: 1.8,
        x2: 0.8,
        y2: 1.8,
    };
    assert!(evaluate_cubic_bezier(overshoot, 0.5) > 1.);
    let invalid = CubicBezier {
        x1: f64::NAN,
        ..linear()
    };
    assert_eq!(evaluate_cubic_bezier(invalid, 0.25), 0.25);
}

#[test]
fn motion_policy_uses_static_states_for_zero_or_disabled_motion() {
    assert!(
        MotionPolicy::new(true, false)
            .animation(Duration::ZERO, linear())
            .is_none()
    );
    assert!(
        MotionPolicy::new(false, false)
            .animation(Duration::from_secs(1), linear())
            .is_none()
    );
    assert!(
        MotionPolicy::new(true, true)
            .animation(Duration::from_secs(1), linear())
            .is_none()
    );
    assert!(
        MotionPolicy::new(true, false)
            .animation(Duration::from_secs(1), linear())
            .is_some()
    );
}

#[test]
fn interrupted_transition_is_continuous_and_reduced_motion_finishes_immediately() {
    let mut transition = ScalarTransition::new(0., linear());
    let motion = MotionPolicy::new(true, false);
    transition.retarget(100., Duration::ZERO, Duration::from_secs(1), motion);
    let halfway = Duration::from_millis(500);
    assert!((transition.sample(halfway).value - 50.).abs() < 1e-6);
    transition.retarget(0., halfway, Duration::from_secs(1), motion);
    assert!((transition.sample(halfway).value - 50.).abs() < 1e-6);
    let later = Duration::from_millis(1_000);
    assert!((transition.sample(later).value - 25.).abs() < 1e-6);
    transition.retarget(
        0.,
        later,
        Duration::from_secs(1),
        MotionPolicy::new(true, true),
    );
    assert_eq!(
        transition.sample(later),
        TransitionSample {
            value: 0.,
            finished: true
        }
    );
}

#[test]
fn an_unchanged_target_can_finish_immediately_without_restarting_ordinary_motion() {
    let enabled = MotionPolicy::new(true, false);
    let mut transition = ScalarTransition::new(0., linear());
    transition.retarget(100., Duration::ZERO, Duration::from_secs(1), enabled);
    transition.retarget(
        100.,
        Duration::from_millis(250),
        Duration::from_secs(1),
        enabled,
    );
    assert_eq!(transition.sample(Duration::from_millis(500)).value, 50.);
    transition.retarget(100., Duration::from_millis(500), Duration::ZERO, enabled);
    assert_eq!(
        transition.sample(Duration::from_millis(500)),
        TransitionSample {
            value: 100.,
            finished: true
        }
    );
    transition.retarget(0., Duration::from_secs(1), Duration::from_secs(1), enabled);
    transition.retarget(
        0.,
        Duration::from_millis(1_250),
        Duration::from_secs(1),
        MotionPolicy::new(true, true),
    );
    assert_eq!(
        transition.sample(Duration::from_millis(1_250)),
        TransitionSample {
            value: 0.,
            finished: true
        }
    );
}

#[test]
fn extreme_unvalidated_curves_cannot_produce_nonfinite_animation_values() {
    let overflowing = CubicBezier::new(0.2, f64::MAX, 0.8, f64::MAX);
    assert_eq!(evaluate_cubic_bezier(overflowing, 0.5), 0.5);
    let finite_but_unrepresentable = CubicBezier::new(0.2, 1e100, 0.8, 1e100);
    let animation = MotionPolicy::new(true, false)
        .animation(Duration::from_millis(100), finite_but_unrepresentable)
        .unwrap();
    assert_eq!((animation.easing)(0.5), 0.5);
    assert_eq!((animation.easing)(0.), 0.);
    assert_eq!((animation.easing)(1.), 1.);
}

#[test]
fn gpui_reduced_motion_renders_static_loop_without_scheduling_frames() {
    struct AnimatedFixture(Rc<RefCell<Vec<f32>>>);
    impl Render for AnimatedFixture {
        fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
            let values = self.0.clone();
            let animation = MotionPolicy::new(true, false)
                .animation(Duration::from_secs(1), linear())
                .unwrap()
                .repeat();
            div()
                .id("animated")
                .with_animation("loop", animation, move |element, value| {
                    values.borrow_mut().push(value);
                    element
                })
        }
    }
    let mut cx = TestAppContext::single();
    cx.update(|cx| cx.set_reduce_motion(true));
    let values: Rc<RefCell<Vec<f32>>> = Rc::default();
    let recorded = values.clone();
    let window = cx.add_window(move |_, _| AnimatedFixture(recorded));
    cx.update_window(window.into(), |_, window, cx| {
        window.draw(cx).clear(cx);
        assert_eq!(window.simulate_next_frame(cx), 0);
    })
    .unwrap();
    assert!(!values.borrow().is_empty());
    assert!(values.borrow().iter().all(|value| *value == 0.));
}

struct ControlFixture {
    root_focus: FocusHandle,
    first: FocusHandle,
    second: FocusHandle,
    guard: ActivationGuard,
    activations: Rc<RefCell<Vec<Activation>>>,
    _blur: Subscription,
    _focus_lost: Subscription,
}

impl ControlFixture {
    fn new(window: &mut Window, cx: &mut Context<Self>) -> Self {
        initialize(cx);
        let root_focus = cx.focus_handle();
        window.focus(&root_focus, cx);
        let restore_root = root_focus.clone();
        let focus_lost = cx.on_focus_lost(window, move |_, window, cx| {
            window.focus(&restore_root, cx);
        });
        let first = focus_handle(cx);
        let second = focus_handle(cx);
        let guard = ActivationGuard::default();
        let blur = guard.observe_blur(&first, window, cx);
        Self {
            root_focus,
            first,
            second,
            guard,
            activations: Rc::default(),
            _blur: blur,
            _focus_lost: focus_lost,
        }
    }
}

impl Render for ControlFixture {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        let availability = self.guard.availability();
        let mut first = div().id("first").w(px(80.)).h(px(40.));
        if availability.can_focus() {
            first = first.track_focus(&self.first);
        }
        let events = self.activations.clone();
        let first = self.guard.install(first, move |activation, _, _| {
            events.borrow_mut().push(activation);
        });
        let first = FocusRing::new(gpui::rgb(0x1677ff), px(2.)).apply(first, &self.first);
        let first = Accessibility {
            disabled: availability.disabled,
            busy: availability.busy,
            ..Accessibility::new(Role::Button)
        }
        .label("First")
        .apply(first);
        traversal(
            div()
                .id("root")
                .track_focus(&self.root_focus)
                .flex()
                .size_full(),
        )
        .child(first)
        .child(
            div()
                .id("second")
                .w(px(80.))
                .h(px(40.))
                .track_focus(&self.second)
                .role(Role::Button),
        )
    }
}

fn fixture() -> (TestAppContext, WindowHandle<ControlFixture>) {
    let mut cx = TestAppContext::single();
    let window = cx.add_window(ControlFixture::new);
    draw(&mut cx, window);
    (cx, window)
}

fn draw(cx: &mut TestAppContext, window: WindowHandle<ControlFixture>) {
    cx.run_until_parked();
    cx.update_window(window.into(), |_, window, cx| window.draw(cx).clear(cx))
        .unwrap();
}

fn key(
    cx: &mut TestAppContext,
    window: WindowHandle<ControlFixture>,
    stroke: &'static str,
    down: bool,
    held: bool,
) {
    let stroke = Keystroke::parse(stroke).unwrap();
    let event = if down {
        PlatformInput::KeyDown(KeyDownEvent {
            keystroke: stroke,
            is_held: held,
            prefer_character_input: false,
        })
    } else {
        PlatformInput::KeyUp(KeyUpEvent { keystroke: stroke })
    };
    cx.update_window(window.into(), |_, window, cx| {
        window.dispatch_event(event, cx)
    })
    .unwrap();
}

fn focus_first(cx: &mut TestAppContext, window: WindowHandle<ControlFixture>) {
    cx.update_window(window.into(), |view, window, cx| {
        let focus = view
            .downcast::<ControlFixture>()
            .unwrap()
            .read(cx)
            .first
            .clone();
        window.focus(&focus, cx);
    })
    .unwrap();
    draw(cx, window);
}

fn set_availability(
    cx: &mut TestAppContext,
    window: WindowHandle<ControlFixture>,
    availability: Availability,
) {
    cx.update_window(window.into(), |view, window, cx| {
        view.downcast::<ControlFixture>()
            .unwrap()
            .update(cx, |view, cx| {
                view.guard.update(availability, &view.first, window, cx);
                cx.notify();
            });
    })
    .unwrap();
}

fn activations(cx: &mut TestAppContext, window: WindowHandle<ControlFixture>) -> Vec<Activation> {
    window
        .root(cx)
        .unwrap()
        .read_with(cx, |view, _| view.activations.borrow().clone())
}

#[test]
fn traversal_is_idempotent_and_focus_visible_tracks_window_modality() {
    let (mut cx, window) = fixture();
    cx.update(|cx| {
        let original = cx.key_bindings().borrow().bindings().len();
        initialize(cx);
        initialize(cx);
        assert_eq!(cx.key_bindings().borrow().bindings().len(), original);
    });
    key(&mut cx, window, "tab", true, false);
    draw(&mut cx, window);
    cx.update_window(window.into(), |view, window, cx| {
        let view = view.downcast::<ControlFixture>().unwrap();
        assert!(view.read(cx).first.is_focused(window));
        assert!(window.last_input_was_keyboard());
    })
    .unwrap();
    key(&mut cx, window, "tab", true, false);
    draw(&mut cx, window);
    cx.update_window(window.into(), |view, window, cx| {
        let view = view.downcast::<ControlFixture>().unwrap();
        assert!(view.read(cx).second.is_focused(window));
    })
    .unwrap();
    key(&mut cx, window, "shift-tab", true, false);
    draw(&mut cx, window);
    cx.update_window(window.into(), |view, window, cx| {
        assert!(
            view.downcast::<ControlFixture>()
                .unwrap()
                .read(cx)
                .first
                .is_focused(window)
        );
    })
    .unwrap();
}

#[test]
fn focus_outline_has_a_transparent_gap_and_preserves_programmatic_modality() {
    let (mut cx, window) = fixture();
    focus_first(&mut cx, window);
    cx.update_window(window.into(), |_, window, _| {
        assert!(!window.last_input_was_keyboard());
        assert!(window.rendered_scene().quads.is_empty());
    })
    .unwrap();
    key(&mut cx, window, "f10", true, false);
    draw(&mut cx, window);
    cx.update_window(window.into(), |_, window, _| {
        let ring = window
            .rendered_scene()
            .quads
            .first()
            .expect("keyboard focus outline");
        let scale = window.scale_factor();
        assert_eq!(ring.bounds.origin.x.0 / scale, -3.);
        assert_eq!(ring.bounds.origin.y.0 / scale, -3.);
        assert_eq!(ring.bounds.size.width.0 / scale, 86.);
        assert_eq!(ring.bounds.size.height.0 / scale, 46.);
        assert_eq!(ring.border_widths.left.0 / scale, 2.);
        assert_eq!(ring.border_widths.right.0 / scale, 2.);
        // The border's inner edge is one pixel beyond the original 0..80 control.
        assert_eq!(
            (ring.bounds.origin.x + ring.border_widths.left).0 / scale,
            -1.
        );
    })
    .unwrap();
    cx.update_window(window.into(), |_, window, cx| {
        window.dispatch_event(
            PlatformInput::MouseMove(gpui::MouseMoveEvent {
                position: point(px(200.), px(100.)),
                ..Default::default()
            }),
            cx,
        );
    })
    .unwrap();
    focus_first(&mut cx, window);
    cx.update_window(window.into(), |_, window, _| {
        assert!(!window.last_input_was_keyboard());
        assert!(window.rendered_scene().quads.is_empty());
    })
    .unwrap();
}

#[test]
fn keyboard_activation_fires_once_and_rejects_mismatched_or_modified_release() {
    let (mut cx, window) = fixture();
    focus_first(&mut cx, window);
    key(&mut cx, window, "space", true, false);
    key(&mut cx, window, "space", true, true);
    key(&mut cx, window, "space", false, false);
    assert_eq!(
        activations(&mut cx, window),
        vec![Activation::Keyboard(KeyboardButton::Space)]
    );
    key(&mut cx, window, "space", true, false);
    key(&mut cx, window, "enter", false, false);
    key(&mut cx, window, "enter", true, false);
    key(&mut cx, window, "shift-enter", false, false);
    assert_eq!(activations(&mut cx, window).len(), 1);
}

#[test]
fn busy_round_trip_cancels_a_press_even_without_intermediate_render() {
    let (mut cx, window) = fixture();
    focus_first(&mut cx, window);
    key(&mut cx, window, "enter", true, false);
    set_availability(
        &mut cx,
        window,
        Availability {
            busy: true,
            disabled: false,
        },
    );
    set_availability(&mut cx, window, Availability::default());
    key(&mut cx, window, "enter", true, true);
    key(&mut cx, window, "enter", false, false);
    assert!(activations(&mut cx, window).is_empty());
    cx.update_window(window.into(), |view, window, cx| {
        assert!(
            view.downcast::<ControlFixture>()
                .unwrap()
                .read(cx)
                .first
                .is_focused(window)
        );
    })
    .unwrap();
    key(&mut cx, window, "enter", true, false);
    key(&mut cx, window, "enter", false, false);
    assert_eq!(activations(&mut cx, window).len(), 1);
}

#[test]
fn changing_focus_during_a_press_does_not_activate_either_control() {
    let (mut cx, window) = fixture();
    focus_first(&mut cx, window);
    key(&mut cx, window, "enter", true, false);
    key(&mut cx, window, "tab", true, false);
    draw(&mut cx, window);
    focus_first(&mut cx, window);
    key(&mut cx, window, "enter", false, false);
    assert!(activations(&mut cx, window).is_empty());
    key(&mut cx, window, "enter", true, false);
    key(&mut cx, window, "enter", false, false);
    assert_eq!(activations(&mut cx, window).len(), 1);
}

#[test]
fn disabled_controls_lose_focus_and_are_skipped_by_traversal() {
    let (mut cx, window) = fixture();
    focus_first(&mut cx, window);
    set_availability(
        &mut cx,
        window,
        Availability {
            disabled: true,
            busy: false,
        },
    );
    draw(&mut cx, window);
    key(&mut cx, window, "tab", true, false);
    draw(&mut cx, window);
    cx.update_window(window.into(), |view, window, cx| {
        let view = view.downcast::<ControlFixture>().unwrap();
        assert!(view.read(cx).second.is_focused(window));
    })
    .unwrap();
    assert!(activations(&mut cx, window).is_empty());
}

#[test]
fn pointer_release_outside_and_busy_round_trip_do_not_activate() {
    let (mut cx, window) = fixture();
    let inside = point(px(20.), px(20.));
    let outside = point(px(200.), px(100.));
    let pointer = |cx: &mut TestAppContext, down: bool, position| {
        cx.update_window(window.into(), |_, window, cx| {
            let event = if down {
                PlatformInput::MouseDown(MouseDownEvent {
                    button: MouseButton::Left,
                    position,
                    ..Default::default()
                })
            } else {
                PlatformInput::MouseUp(MouseUpEvent {
                    button: MouseButton::Left,
                    position,
                    ..Default::default()
                })
            };
            window.dispatch_event(event, cx);
        })
        .unwrap();
    };
    pointer(&mut cx, true, inside);
    pointer(&mut cx, false, outside);
    assert!(activations(&mut cx, window).is_empty());
    pointer(&mut cx, true, inside);
    set_availability(
        &mut cx,
        window,
        Availability {
            busy: true,
            disabled: false,
        },
    );
    set_availability(&mut cx, window, Availability::default());
    pointer(&mut cx, false, inside);
    assert!(activations(&mut cx, window).is_empty());
    draw(&mut cx, window);
    pointer(&mut cx, true, inside);
    pointer(&mut cx, false, inside);
    assert_eq!(activations(&mut cx, window), vec![Activation::Pointer]);
}
