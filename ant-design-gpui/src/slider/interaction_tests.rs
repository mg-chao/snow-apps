// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;
use crate::{ConfigPatch, ConfigScope, Direction, UiRoot};
use gpui::{
    AppContext, Context, Entity, IntoElement, KeyDownEvent, KeyUpEvent, Keystroke, MouseButton,
    MouseDownEvent, MouseMoveEvent, MouseUpEvent, Pixels, PlatformInput, Point, Render,
    Subscription, TestAppContext, Window, WindowHandle, div, point, prelude::*, px,
};
use std::{cell::RefCell, rc::Rc, time::Duration};

struct Consumer {
    state: Entity<SliderState>,
    scope: ConfigScope,
    controlled: Option<SliderValue>,
    accept: bool,
    shown: bool,
    disabled: Option<SliderDisabled>,
    tooltip: SliderTooltip,
    styles: SliderStyles,
    _subscription: Subscription,
}
impl Render for Consumer {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let mut root = div().w(px(320.)).h(px(240.)).p(px(10.));
        if self.shown {
            let mut slider = Slider::new("slider", &self.state)
                .scope(self.scope.clone())
                .tooltip(self.tooltip.clone())
                .styles(self.styles.clone())
                .accessibility_label("Value")
                .automation_id("test-slider")
                .on_change(cx.listener(|this, event: &SliderChange, _, cx| {
                    if this.accept && this.controlled.is_some() {
                        this.controlled = Some(event.value.clone());
                        cx.notify();
                    }
                }));
            if let Some(value) = &self.controlled {
                slider = slider.value(value.clone());
            }
            if let Some(disabled) = &self.disabled {
                slider = slider.disabled(disabled.clone());
            }
            root = root.child(slider);
        }
        root
    }
}
struct Fixture {
    cx: TestAppContext,
    window: WindowHandle<UiRoot>,
    consumer: Entity<Consumer>,
    state: Entity<SliderState>,
    events: Rc<RefCell<Vec<SliderEvent>>>,
}
impl Fixture {
    fn new(options: SliderOptions, value: impl Into<SliderValue>) -> Self {
        let value = value.into();
        let mut cx = TestAppContext::single();
        let mut result = None;
        let events = Rc::new(RefCell::new(Vec::new()));
        let output = events.clone();
        let window = cx.add_window(|window, cx| {
            window.activate_window();
            let scope = ConfigScope::new_window(cx);
            scope.update_patch(cx, |p| p.motion = Some(false)).unwrap();
            let state = cx.new(|cx| SliderState::with_options(options, window, cx).unwrap());
            state.update(cx, |state, cx| state.set_value(value, window, cx).unwrap());
            let consumer = cx.new(|cx| Consumer {
                _subscription: cx.subscribe(&state, move |_, _, event, _| {
                    output.borrow_mut().push(event.clone())
                }),
                state: state.clone(),
                scope: scope.clone(),
                controlled: None,
                accept: false,
                shown: true,
                disabled: None,
                tooltip: SliderTooltip {
                    enabled: false,
                    ..Default::default()
                },
                styles: SliderStyles::default(),
            });
            result = Some((consumer.clone(), state));
            UiRoot::new(scope, consumer, window, cx)
        });
        let (consumer, state) = result.unwrap();
        let mut fixture = Self {
            cx,
            window,
            consumer,
            state,
            events,
        };
        fixture.draw();
        fixture
    }
    fn draw(&mut self) {
        self.cx.run_until_parked();
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                window.draw(cx).clear(cx)
            })
            .unwrap();
        self.cx.run_until_parked();
    }
    fn next_frame(&mut self) {
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                window.simulate_next_frame(cx);
            })
            .unwrap();
        self.draw();
    }
    fn event(&mut self, event: PlatformInput) {
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                window.dispatch_event(event, cx);
            })
            .unwrap();
        self.draw();
    }
    fn point(&self, value: f64) -> Point<Pixels> {
        self.state.read_with(&self.cx, |s, cx| {
            let bounds = s.bounds.expect("painted rail");
            let mut f = s.model.scale.fraction(value) as f32;
            if s.options().inverted(s.direction(cx)) {
                f = 1. - f;
            }
            if s.options().orientation == SliderOrientation::Vertical {
                point(bounds.center().x, bounds.top() + bounds.size.height * f)
            } else {
                point(bounds.left() + bounds.size.width * f, bounds.center().y)
            }
        })
    }
    fn down(&mut self, position: Point<Pixels>) {
        self.event(PlatformInput::MouseDown(MouseDownEvent {
            position,
            button: MouseButton::Left,
            click_count: 1,
            ..Default::default()
        }));
    }
    fn move_to(&mut self, position: Point<Pixels>) {
        self.event(PlatformInput::MouseMove(MouseMoveEvent {
            position,
            pressed_button: Some(MouseButton::Left),
            ..Default::default()
        }));
    }
    fn hover(&mut self, position: Point<Pixels>) {
        self.event(PlatformInput::MouseMove(MouseMoveEvent {
            position,
            ..Default::default()
        }));
    }
    fn up(&mut self, position: Point<Pixels>) {
        self.event(PlatformInput::MouseUp(MouseUpEvent {
            position,
            button: MouseButton::Left,
            ..Default::default()
        }));
        self.next_frame();
    }
    fn key(&mut self, key: &'static str) {
        self.event(PlatformInput::KeyDown(KeyDownEvent {
            keystroke: Keystroke::parse(key).unwrap(),
            is_held: false,
            prefer_character_input: false,
        }));
    }
    fn key_up(&mut self, key: &'static str) {
        self.event(PlatformInput::KeyUp(KeyUpEvent {
            keystroke: Keystroke::parse(key).unwrap(),
        }));
    }
    fn focus(&mut self, index: usize) {
        let state = self.state.clone();
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                state.update(cx, |state, cx| state.focus(index, window, cx))
            })
            .unwrap();
        self.draw();
    }
    fn value(&self) -> SliderValue {
        self.state.read_with(&self.cx, |s, _| s.value())
    }
    fn tooltip_visible(&mut self) -> bool {
        let background = self.consumer.read_with(&self.cx, |c, cx| {
            c.scope
                .snapshot(cx)
                .theme
                .tokens
                .color_bg_spotlight
                .to_hsla()
        });
        self.cx
            .update_window(self.window.into(), |_, window, _| {
                window
                    .rendered_scene()
                    .quads
                    .iter()
                    .any(|quad| quad.background.as_solid() == Some(background))
            })
            .unwrap()
    }
    fn edit(&mut self, edit: impl FnOnce(&mut Consumer)) {
        self.consumer.update(&mut self.cx, |c, cx| {
            edit(c);
            cx.notify();
        });
        self.draw();
    }
    fn changes(&self) -> Vec<SliderValue> {
        self.events
            .borrow()
            .iter()
            .filter_map(|e| match e {
                SliderEvent::Change(c) => Some(c.value.clone()),
                _ => None,
            })
            .collect()
    }
    fn completions(&self) -> usize {
        self.events
            .borrow()
            .iter()
            .filter(|e| matches!(e, SliderEvent::ChangeComplete(_)))
            .count()
    }
}

#[test]
fn native_pointer_click_drag_and_outside_release_emit_one_completion() {
    let mut f = Fixture::new(SliderOptions::default(), 0.);
    f.down(f.point(25.));
    assert_eq!(f.value(), 25.0.into());
    f.move_to(f.point(75.));
    assert_eq!(f.value(), 75.0.into());
    f.up(point(px(900.), px(900.)));
    assert_eq!(f.completions(), 1);
    f.move_to(f.point(10.));
    assert_eq!(f.value(), 75.0.into());
    assert_eq!(f.changes(), vec![25.0.into(), 75.0.into()]);
}
#[test]
fn native_keyboard_uses_reference_steps_and_completes_on_key_release() {
    let mut f = Fixture::new(
        SliderOptions {
            step: Some(0.1),
            max: 1.,
            ..Default::default()
        },
        0.3,
    );
    f.focus(0);
    f.key("pageup");
    assert_eq!(f.value(), 0.5.into());
    assert_eq!(f.completions(), 0);
    f.key_up("pageup");
    assert_eq!(f.completions(), 1);
    f.key("end");
    f.key_up("end");
    assert_eq!(f.value(), 1.0.into());
    f.key("home");
    f.key_up("home");
    assert_eq!(f.value(), 0.0.into());
}
#[test]
fn native_controlled_values_are_proposals_until_the_owner_accepts() {
    let mut f = Fixture::new(SliderOptions::default(), 20.);
    f.edit(|c| c.controlled = Some(20.0.into()));
    f.down(f.point(50.));
    f.up(f.point(50.));
    assert_eq!(f.value(), 20.0.into());
    assert_eq!(f.changes(), vec![50.0.into()]);
    f.edit(|c| c.accept = true);
    f.down(f.point(60.));
    f.up(f.point(60.));
    assert_eq!(f.value(), 60.0.into());
}
#[test]
fn native_range_crossing_retains_the_dragged_handle_and_focus() {
    let mut f = Fixture::new(
        SliderOptions {
            range: Some(SliderRange::default()),
            ..Default::default()
        },
        [20., 80.],
    );
    let id = f.state.read_with(&f.cx, |s, _| s.model.handles[0].id);
    f.down(f.point(20.));
    f.move_to(f.point(90.));
    f.up(f.point(90.));
    assert_eq!(f.value(), [80., 90.].into());
    assert_eq!(f.state.read_with(&f.cx, |s, _| s.model.handles[1].id), id);
    f.key("left");
    f.key_up("left");
    assert_eq!(f.value(), [80., 89.].into());
}
#[test]
fn native_disabled_handles_block_pointer_keyboard_and_crossing() {
    let mut f = Fixture::new(
        SliderOptions {
            range: Some(SliderRange::default()),
            disabled: [true, false, true].into(),
            ..Default::default()
        },
        [20., 50., 80.],
    );
    f.down(f.point(20.));
    f.move_to(f.point(30.));
    f.up(f.point(30.));
    assert!(f.changes().is_empty());
    f.down(f.point(50.));
    f.move_to(f.point(100.));
    f.up(f.point(100.));
    assert_eq!(f.value(), [20., 80., 80.].into());
    f.edit(|c| c.disabled = Some(true.into()));
    f.key("home");
    assert_eq!(f.value(), [20., 80., 80.].into());
}
#[test]
fn native_track_drag_preserves_range_span() {
    let mut f = Fixture::new(
        SliderOptions {
            range: Some(SliderRange {
                draggable_track: true,
                ..Default::default()
            }),
            ..Default::default()
        },
        [20., 80.],
    );
    f.down(f.point(50.));
    f.move_to(f.point(60.));
    f.up(f.point(60.));
    assert_eq!(f.value(), [30., 90.].into());
}
#[test]
fn native_editable_drag_removes_and_restores_before_release() {
    let mut f = Fixture::new(
        SliderOptions {
            range: Some(SliderRange {
                editable: true,
                min_count: 1,
                max_count: Some(5),
                ..Default::default()
            }),
            ..Default::default()
        },
        [20., 80.],
    );
    let point = f.point(20.);
    f.down(point);
    f.move_to(point + gpui::point(px(0.), px(140.)));
    assert_eq!(f.value(), vec![80.].into());
    f.move_to(point);
    assert_eq!(f.value(), [20., 80.].into());
    f.up(point);
    f.focus(0);
    f.key("delete");
    assert_eq!(f.value(), vec![80.].into());
}
#[test]
fn native_rtl_and_vertical_pointer_mapping_match_logical_values() {
    for orientation in [SliderOrientation::Horizontal, SliderOrientation::Vertical] {
        let mut f = Fixture::new(
            SliderOptions {
                orientation,
                ..Default::default()
            },
            0.,
        );
        let scope = f.consumer.read_with(&f.cx, |c, _| c.scope.clone());
        f.cx.update(|cx| {
            scope
                .set_patch(
                    ConfigPatch {
                        direction: Some(Direction::Rtl),
                        motion: Some(false),
                        ..Default::default()
                    },
                    cx,
                )
                .unwrap()
        });
        f.draw();
        f.down(f.point(75.));
        f.up(f.point(75.));
        assert_eq!(f.value(), 75.0.into());
    }
}
#[test]
fn native_unmount_cancels_drag_and_settled_controls_schedule_no_frames() {
    let mut f = Fixture::new(SliderOptions::default(), 20.);
    f.down(f.point(20.));
    f.edit(|c| c.shown = false);
    assert!(f.state.read_with(&f.cx, |s, _| s.drag.is_none()));
    f.edit(|c| c.shown = true);
    f.move_to(f.point(80.));
    assert_eq!(f.value(), 20.0.into());
    f.cx.executor().advance_clock(Duration::from_millis(500));
    f.draw();
    f.cx.update_window(f.window.into(), |_, window, cx| {
        window.simulate_next_frame(cx);
    })
    .unwrap();
    f.draw();
    let pending =
        f.cx.update_window(f.window.into(), |_, window, cx| {
            window.simulate_next_frame(cx)
        })
        .unwrap();
    assert_eq!(pending, 0);
}

#[test]
fn native_handle_circles_fit_their_quad_bounds_and_explicit_width_preserves_rail_width() {
    let mut f = Fixture::new(SliderOptions::default(), 30.);
    f.edit(|c| c.styles.root = gpui::StyleRefinement::default().w(px(280.)));
    let bounds = f.state.read_with(&f.cx, |s, _| s.bounds.unwrap());
    assert_eq!(
        bounds.size.width,
        px(280.),
        "public width sizes the rail, with margins outside it"
    );
    let center = f.point(30.);
    f.cx.update_window(f.window.into(), |_, w, _| {
        let scale = w.scale_factor();
        let circles: Vec<_> = w
            .rendered_scene()
            .quads
            .iter()
            .filter(|q| {
                (q.bounds.size.width.0 - q.bounds.size.height.0).abs() < 0.01
                    && q.bounds.size.width.0 <= 20. * scale
                    && (q.bounds.center().x.0 - f32::from(center.x) * scale).abs() < 1.
            })
            .collect();
        assert_eq!(
            circles.len(),
            2,
            "handle ring and fill must both be painted"
        );
        for q in circles {
            assert!(
                q.corner_radii.top_left.0 <= q.bounds.size.width.0 / 2. + 0.001,
                "GPUI canvas radii are not automatically clamped and oversized circles disappear"
            );
        }
    })
    .unwrap();
}

#[test]
fn compact_focused_handle_rings_do_not_accumulate_layout_rounding() {
    for orientation in [SliderOrientation::Horizontal, SliderOrientation::Vertical] {
        let mut f = Fixture::new(
            SliderOptions {
                orientation,
                ..Default::default()
            },
            30.,
        );
        let scope = f
            .consumer
            .read_with(&f.cx, |consumer, _| consumer.scope.clone());
        f.cx.update_window(f.window.into(), |_, window, cx| {
            window.set_scale_factor(1.);
            scope
                .update_patch(cx, |patch| {
                    patch.theme.get_or_insert_default().algorithms =
                        Some(crate::theme::AlgorithmChain::compact())
                })
                .unwrap();
        })
        .unwrap();
        f.focus(0);
        f.cx.update_window(f.window.into(), |_, window, cx| {
            let tokens = scope.snapshot(cx).theme.component::<SliderTheme>();
            let diameter = tokens.handle_size_hover + 2. * tokens.handle_line_width_hover;
            let expected_cross =
                10. + tokens.margin_cross() + tokens.rail_size * 1.5 - diameter / 2.;
            let ring = window
                .rendered_scene()
                .quads
                .iter()
                .find(|quad| {
                    quad.background.as_solid() == Some(tokens.handle_active_color.to_hsla())
                })
                .unwrap();
            let actual = if orientation == SliderOrientation::Horizontal {
                ring.bounds.top().0
            } else {
                ring.bounds.left().0
            };
            assert!(
                (f64::from(actual) - expected_cross).abs() <= 1.,
                "{orientation:?}: {actual} versus {expected_cross}"
            );
        })
        .unwrap();
    }
}

#[test]
fn native_tooltip_closes_after_drag_release_and_pointer_leave_without_blurring() {
    for range in [false, true] {
        for release_outside in [false, true] {
            let mut f = Fixture::new(
                SliderOptions {
                    range: range.then(SliderRange::default),
                    ..Default::default()
                },
                if range {
                    [20., 80.].into()
                } else {
                    SliderValue::Single(20.)
                },
            );
            f.edit(|c| c.tooltip = SliderTooltip::default());
            assert!(!f.tooltip_visible());
            f.hover(f.point(20.));
            assert!(f.tooltip_visible(), "hover opens the tooltip");
            f.down(f.point(20.));
            f.move_to(f.point(60.));
            let outside = f.point(60.) + point(px(0.), px(80.));
            f.move_to(outside);
            assert!(
                f.tooltip_visible(),
                "dragging outside keeps the tooltip open"
            );
            if release_outside {
                f.up(outside);
            } else {
                f.move_to(f.point(60.));
                f.up(f.point(60.));
                assert!(
                    f.tooltip_visible(),
                    "hover keeps the tooltip open after release"
                );
                f.hover(outside);
            }
            assert!(
                !f.tooltip_visible(),
                "release and pointer leave close the tooltip"
            );
            let focus = f.state.read_with(&f.cx, |s, _| {
                s.handles[&s.model.handles[0].id].focus.clone()
            });
            assert!(
                f.cx.update_window(f.window.into(), |_, window, _| { focus.is_focused(window) })
                    .unwrap(),
                "pointer release preserves native focus"
            );
            f.key("right");
            f.key_up("right");
            assert_eq!(
                f.value(),
                if range {
                    [61., 80.].into()
                } else {
                    61.0.into()
                }
            );
            f.hover(f.point(61.));
            assert!(f.tooltip_visible(), "hover can reopen the tooltip");
            f.hover(outside);
            assert!(!f.tooltip_visible());
            f.down(f.point(61.));
            f.move_to(outside);
            assert!(
                f.tooltip_visible(),
                "pressing the focused thumb reopens its tooltip"
            );
            f.up(outside);
            assert!(!f.tooltip_visible());
        }
    }
}

#[test]
fn native_tooltip_release_before_focus_notification_does_not_reopen_it() {
    let mut f = Fixture::new(SliderOptions::default(), 30.);
    f.edit(|c| c.tooltip = SliderTooltip::default());
    let position = f.point(30.);
    f.cx.update_window(f.window.into(), |_, window, cx| {
        window.dispatch_event(
            PlatformInput::MouseDown(MouseDownEvent {
                position,
                button: MouseButton::Left,
                click_count: 1,
                ..Default::default()
            }),
            cx,
        );
        window.dispatch_event(
            PlatformInput::MouseUp(MouseUpEvent {
                position,
                button: MouseButton::Left,
                ..Default::default()
            }),
            cx,
        );
    })
    .unwrap();
    f.draw();
    f.next_frame();
    f.hover(position + point(px(0.), px(80.)));
    assert!(
        !f.tooltip_visible(),
        "a late focus event must not reopen the released tooltip"
    );
}

#[test]
fn native_tooltip_focus_opens_until_blur_or_mouse_release() {
    let mut f = Fixture::new(SliderOptions::default(), 30.);
    f.edit(|c| c.tooltip = SliderTooltip::default());
    f.focus(0);
    f.draw();
    assert!(f.tooltip_visible(), "native focus opens the tooltip");
    f.key("right");
    f.key_up("right");
    assert!(
        f.tooltip_visible(),
        "keyboard release keeps the focused tooltip open"
    );
    f.up(point(px(900.), px(900.)));
    assert!(
        !f.tooltip_visible(),
        "mouse release clears tooltip focus even without a drag"
    );
    let state = f.state.clone();
    f.cx.update_window(f.window.into(), |_, window, cx| {
        state.update(cx, |s, cx| s.blur(window, cx));
    })
    .unwrap();
    f.draw();
    f.focus(0);
    f.draw();
    assert!(f.tooltip_visible(), "new focus can reopen the tooltip");
    f.cx.update_window(f.window.into(), |_, window, cx| {
        state.update(cx, |s, cx| s.blur(window, cx));
    })
    .unwrap();
    f.draw();
    f.draw();
    assert!(!f.tooltip_visible(), "blur closes the focused tooltip");
}

#[test]
fn native_tooltip_explicit_open_overrides_pointer_and_focus() {
    for open in [false, true] {
        let mut f = Fixture::new(SliderOptions::default(), 30.);
        f.edit(|c| {
            c.tooltip = SliderTooltip {
                open: Some(open),
                ..Default::default()
            }
        });
        f.focus(0);
        f.draw();
        assert_eq!(f.tooltip_visible(), open);
        f.down(f.point(30.));
        f.move_to(f.point(60.));
        assert_eq!(f.tooltip_visible(), open);
        let outside = f.point(60.) + point(px(0.), px(80.));
        f.move_to(outside);
        f.up(outside);
        assert_eq!(f.tooltip_visible(), open);
    }
}

#[test]
fn native_tooltip_formatter_is_lazy_and_runs_once_per_visible_handle_render() {
    let mut f = Fixture::new(SliderOptions::default(), 30.);
    let calls = Rc::new(std::cell::Cell::new(0));
    let counter = calls.clone();
    f.edit(|c| {
        c.tooltip = SliderTooltip {
            open: Some(false),
            ..Default::default()
        }
        .formatter(move |value, _, _| {
            counter.set(counter.get() + 1);
            Some(div().child(value.to_string()))
        })
    });
    assert_eq!(
        calls.get(),
        0,
        "closed tooltips do not construct their content"
    );
    f.edit(|c| c.tooltip.open = Some(true));
    let first = calls.get();
    assert!(first > 0);
    calls.set(0);
    f.cx.update_window(f.window.into(), |_, w, c| {
        w.refresh();
        w.draw(c).clear(c);
    })
    .unwrap();
    assert_eq!(
        calls.get(),
        1,
        "presence and painting share the same element"
    );
    calls.set(0);
    f.edit(|c| c.disabled = Some(true.into()));
    assert_eq!(
        calls.get(),
        0,
        "disabled handles never build forced tooltips"
    );
}

#[test]
fn native_disabling_one_handle_cancels_its_gesture_without_late_completion() {
    let mut f = Fixture::new(
        SliderOptions {
            range: Some(SliderRange::default()),
            ..Default::default()
        },
        [20., 80.],
    );
    f.down(f.point(20.));
    f.edit(|c| c.disabled = Some([true, false].into()));
    f.move_to(f.point(40.));
    f.up(f.point(40.));
    assert_eq!(f.value(), [20., 80.].into());
    assert_eq!(f.completions(), 0);
    f.focus(1);
    f.key("left");
    assert_eq!(f.value(), [20., 79.].into());
    f.key_up("left");
    assert_eq!(f.completions(), 1, "the available handle remains operable");
}

#[test]
fn native_multiple_handles_keep_independently_styled_track_segments() {
    let mut f = Fixture::new(
        SliderOptions {
            range: Some(SliderRange::default()),
            ..Default::default()
        },
        [20., 50., 80.],
    );
    f.edit(|c| {
        c.styles.root = gpui::StyleRefinement::default().w(px(280.));
        c.styles.track = gpui::StyleRefinement::default().bg(gpui::rgb(0xff0000));
    });
    f.cx.update_window(f.window.into(), |_, w, _| {
        let tracks: Vec<_> = w
            .rendered_scene()
            .quads
            .iter()
            .filter(|q| {
                q.background
                    .as_solid()
                    .is_some_and(|c| c.to_rgb() == gpui::rgb(0xff0000))
            })
            .collect();
        assert_eq!(tracks.len(), 2);
        for q in tracks {
            assert!((q.bounds.size.width.0 / w.scale_factor() - 84.).abs() < 0.01);
        }
    })
    .unwrap();
}

#[test]
fn native_dot_semantic_styles_preserve_size_corners_opacity_and_shadows() {
    let mut f = Fixture::new(
        SliderOptions {
            marks: vec![0., 25., 100.],
            ..Default::default()
        },
        30.,
    );
    f.edit(|c| {
        c.styles.dot = gpui::StyleRefinement::default()
            .w(px(12.))
            .h(px(10.))
            .rounded(px(2.))
            .bg(gpui::rgb(0xff11aa))
            .border(px(3.))
            .border_color(gpui::rgb(0x1133ff))
            .opacity(0.5)
            .shadow(vec![gpui::BoxShadow {
                color: gpui::rgb(0x11ff44).into(),
                offset: point(px(0.), px(2.)),
                blur_radius: px(5.),
                spread_radius: px(0.),
                inset: false,
            }]);
    });
    f.cx.update_window(f.window.into(), |_, window, _| {
        let scene = window.rendered_scene();
        let dots: Vec<_> = scene
            .quads
            .iter()
            .filter(|quad| {
                quad.background.as_solid().is_some_and(|color| {
                    let rgb = color.to_rgb();
                    rgb.r > 0.99 && rgb.g > 0.06 && rgb.g < 0.07 && rgb.b > 0.66 && rgb.b < 0.67
                })
            })
            .collect();
        assert_eq!(dots.len(), 3);
        for dot in dots {
            assert_eq!(dot.bounds.size.width.0 / window.scale_factor(), 12.);
            assert_eq!(dot.bounds.size.height.0 / window.scale_factor(), 10.);
            assert_eq!(dot.corner_radii.top_left.0 / window.scale_factor(), 2.);
            assert!((dot.background.as_solid().unwrap().a - 0.5).abs() < 0.01);
        }
        assert!(scene.shadows.len() >= 3);
    })
    .unwrap();
}

#[test]
fn dense_marks_bound_paint_work_and_preserve_individually_selected_dots() {
    let mut f = Fixture::new(
        SliderOptions {
            marks: (0..=100_000).map(|i| f64::from(i) / 1000.).collect(),
            step: None,
            included: false,
            ..Default::default()
        },
        37.123,
    );
    f.edit(|consumer| {
        consumer.styles.dot = gpui::StyleRefinement::default().bg(gpui::rgb(0xaa22bb))
    });
    let center = f.point(37.123);
    let scope = f
        .consumer
        .read_with(&f.cx, |consumer, _| consumer.scope.clone());
    f.cx.update_window(f.window.into(), |_, window, cx| {
        let expected = scope.snapshot(cx).theme.component::<SliderTheme>().dot_active_border_color.to_hsla();
        let dots: Vec<_> = window.rendered_scene().quads.iter().filter(|quad|
            quad.background.as_solid().is_some_and(|color| color.to_rgb() == gpui::rgb(0xaa22bb))).collect();
        let extent = f.state.read(cx).bounds.unwrap().size.width.to_f64() * f64::from(window.scale_factor());
        assert!(dots.len() <= extent.ceil() as usize + 3, "painted {} dots", dots.len());
        assert!(window.rendered_scene().quads.iter().any(|dot|
            // GPUI rounds painted quad origins to physical pixels.
            (dot.bounds.center().x.0 / window.scale_factor() - f32::from(center.x)).abs() <= 0.5 / window.scale_factor() + 0.001
            && dot.border_color == expected), "the selected mark must survive density reduction");
    }).unwrap();
}
