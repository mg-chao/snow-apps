// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;
use crate::{ConfigPatch, ConfigScope, Direction, behavior::Accessibility};
use gpui::{
    AppContext, Context as ViewContext, Element, IntoElement, MouseMoveEvent, PlatformInput,
    Render, Role, TestAppContext, WindowHandle, accesskit, div, point, prelude::*, px, size,
};
use std::cell::Cell;

struct Owner {
    state: Entity<MessageState>,
    mounted: bool,
    duplicate: bool,
    page_clicks: Option<Rc<Cell<usize>>>,
}
impl Render for Owner {
    fn render(&mut self, _: &mut Window, _: &mut ViewContext<Self>) -> impl IntoElement {
        let content = div().relative().size_full().bg(gpui::white()).when_some(
            self.page_clicks.clone(),
            |content, clicks| {
                content.child(
                    div()
                        .absolute()
                        .left(px(8.))
                        .bottom(px(8.))
                        .w(px(140.))
                        .debug_selector(|| "pointer-page-action".into())
                        .child(
                            crate::Button::new("page-pointer-action")
                                .label("Page action")
                                .w_full()
                                .wave(false)
                                .on_click(move |_, _, _| clicks.set(clicks.get() + 1)),
                        ),
                )
            },
        );
        if !self.mounted {
            return content.into_any_element();
        }
        let content = if self.duplicate {
            MessageHost::new("duplicate", &self.state, content).into_any_element()
        } else {
            content.into_any_element()
        };
        MessageHost::new("primary", &self.state, content).into_any_element()
    }
}

struct Fixture {
    cx: TestAppContext,
    window: WindowHandle<Owner>,
    messages: Message,
    scope: ConfigScope,
    state: Entity<MessageState>,
}
impl Fixture {
    fn new(motion: bool) -> Self {
        let mut cx = TestAppContext::single();
        let mut retained = None;
        let window = cx.open_window(size(px(420.), px(360.)), |window, cx| {
            let scope = ConfigScope::new_window(cx);
            scope
                .update_patch(cx, |patch| patch.motion = Some(motion))
                .unwrap();
            let state = cx.new(|cx| MessageState::new(scope.clone(), window, cx));
            retained = Some((Message::new(&state), scope, state.clone()));
            Owner {
                state,
                mounted: true,
                duplicate: false,
                page_clicks: None,
            }
        });
        let (messages, scope, state) = retained.unwrap();
        let mut result = Self {
            cx,
            window,
            messages,
            scope,
            state,
        };
        result.draw();
        result
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
    fn open(&mut self, options: MessageOptions) -> MessageHandle {
        let service = self.messages.clone();
        let result = self
            .cx
            .update_window(self.window.into(), |_, window, cx| {
                service.open(options, window, cx)
            })
            .unwrap();
        self.draw();
        result
    }
    fn layouts(&mut self) -> Vec<MessageLayout> {
        let service = self.messages.clone();
        self.cx.update(|cx| service.layouts(cx))
    }
    fn advance(&mut self, millis: u64) {
        self.cx
            .executor()
            .advance_clock(Duration::from_millis(millis));
        self.cx.run_until_parked();
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                window.simulate_next_frame(cx)
            })
            .unwrap();
        self.draw();
    }
}

#[test]
fn message_surface_preserves_identity_and_role_specific_live_policy() {
    for (role, expected) in [
        (Role::Alert, accesskit::Live::Assertive),
        (Role::Status, accesskit::Live::Polite),
    ] {
        for closing in [false, true] {
            let surface = element::LiveSurface {
                element: Accessibility::new(role)
                    .label("Saved")
                    .automation_id("saved")
                    .apply(div().id("notice")),
                closing,
                boundary: None,
                geometry: None,
            };
            assert_eq!(surface.id(), Some("notice".into()));
            assert_eq!(surface.a11y_role(), Some(role));
            let mut node = accesskit::Node::new(role);
            surface.write_a11y_info(&mut node);
            assert_eq!(
                node.live(),
                Some(if closing {
                    accesskit::Live::Off
                } else {
                    expected
                })
            );
            assert!(node.is_live_atomic());
            assert_eq!(node.label(), Some("Saved"));
            assert_eq!(node.author_id(), Some("saved"));
        }
    }
}

#[test]
fn message_native_sentences_wrap_and_remeasure_on_resize_in_both_directions() {
    for direction in [Direction::Ltr, Direction::Rtl] {
        let mut f = Fixture::new(false);
        f.cx.update(|cx| {
            f.scope
                .update_patch(cx, |patch| patch.direction = Some(direction))
                .unwrap()
        });
        f.open(MessageOptions::new("This message contains several words that wrap across multiple lines inside a narrow floating card.").kind(MessageType::Info).duration(Duration::ZERO));
        let wide = f.layouts()[0].bounds;
        assert!(wide.size.width <= px(372.1));
        f.cx.simulate_window_resize(f.window.into(), size(px(220.), px(360.)));
        f.draw();
        let narrow = f.layouts()[0].bounds;
        assert!(narrow.size.width <= px(172.1));
        assert!(
            narrow.size.height > wide.size.height,
            "wide={wide:?}, narrow={narrow:?}"
        );
        assert!((f32::from(narrow.center().x) - 110.).abs() < 0.5);
        assert!((f32::from(narrow.top()) - 8.).abs() < 0.5);
    }
}

#[test]
fn message_native_unbroken_tokens_keep_reference_single_line_overflow() {
    for direction in [Direction::Ltr, Direction::Rtl] {
        let mut f = Fixture::new(false);
        f.cx.update(|cx| {
            f.scope
                .update_patch(cx, |patch| patch.direction = Some(direction))
                .unwrap()
        });
        f.cx.simulate_window_resize(f.window.into(), size(px(220.), px(360.)));
        f.open(MessageOptions::new("ThisVeryLongUnbrokenIdentifierKeepsItsMinimumContentWidthAndOverflowsTheNoticeCard").kind(MessageType::Info).duration(Duration::ZERO));
        let bounds = f.layouts()[0].bounds;
        assert!(bounds.size.width <= px(172.1));
        assert!((f32::from(bounds.size.height) - 40.).abs() < 0.5);
    }
}

#[test]
fn message_aggregate_padding_keeps_reference_height_and_spacing_at_each_scale() {
    for compact in [false, true] {
        for scale in [1., 1.5, 2.] {
            let mut f = Fixture::new(false);
            f.cx.simulate_window_scale_factor_change(f.window.into(), scale);
            if compact {
                f.cx.update(|cx| {
                    f.scope
                        .update_patch(cx, |patch| {
                            patch.theme = Some(crate::theme::ThemeConfig {
                                algorithms: Some(crate::theme::AlgorithmChain::compact()),
                                ..Default::default()
                            });
                        })
                        .unwrap()
                });
            }
            for index in 0..5 {
                f.open(
                    MessageOptions::new(format!("Notice {index}"))
                        .kind(MessageType::Info)
                        .duration(Duration::ZERO),
                );
            }
            let height = if compact { 35. } else { 40. };
            let gap = if compact { 8. } else { 16. };
            for (index, layout) in f.layouts().iter().enumerate() {
                assert!(
                    (f32::from(layout.bounds.size.height) - height).abs() < 0.01,
                    "compact={compact}, scale={scale}, {layout:?}"
                );
                assert!(
                    (f32::from(layout.bounds.top()) - (8. + index as f32 * (height + gap))).abs()
                        < 0.01,
                    "native row positions must not accumulate individually rounded insets: compact={compact}, scale={scale}, {layout:?}"
                );
            }
        }
    }
}

#[test]
fn message_padding_correction_preserves_empty_factory_and_explicit_or_constrained_height() {
    let mut f = Fixture::new(false);
    f.cx.update(|cx| {
        f.scope
            .update_patch(cx, |patch| {
                patch.theme = Some(crate::theme::ThemeConfig {
                    algorithms: Some(crate::theme::AlgorithmChain::compact()),
                    ..Default::default()
                });
            })
            .unwrap()
    });
    f.open(
        MessageOptions::new("")
            .key("empty")
            .duration(Duration::ZERO),
    );
    f.open(
        MessageOptions::content(|_, _, _| div().w(px(40.)).h(px(32.)))
            .key("factory")
            .duration(Duration::ZERO),
    );
    f.open(
        MessageOptions::new("Fixed")
            .key("fixed")
            .styles(MessageStyles {
                root: gpui::StyleRefinement::default().h(px(60.)),
                ..Default::default()
            })
            .duration(Duration::ZERO),
    );
    f.open(
        MessageOptions::new("Minimum")
            .key("min")
            .styles(MessageStyles {
                root: gpui::StyleRefinement::default().min_h(px(60.)),
                ..Default::default()
            })
            .duration(Duration::ZERO),
    );
    f.open(
        MessageOptions::new("Maximum")
            .key("max")
            .styles(MessageStyles {
                root: gpui::StyleRefinement::default().max_h(px(30.)),
                ..Default::default()
            })
            .duration(Duration::ZERO),
    );
    let layouts = f.layouts();
    for (key, height) in [
        ("empty", 15.),
        ("factory", 47.),
        ("fixed", 60.),
        ("min", 60.),
        ("max", 30.),
    ] {
        let actual = layouts
            .iter()
            .find(|layout| layout.key.as_ref() == key)
            .unwrap();
        assert!(
            (f32::from(actual.bounds.size.height) - height).abs() < 0.01,
            "{key}: {actual:?}"
        );
    }
}

#[test]
fn message_fractional_surface_height_uses_css_offset_height_for_row_placement() {
    let mut f = Fixture::new(false);
    f.open(
        MessageOptions::new("Older")
            .key("older")
            .duration(Duration::ZERO),
    );
    f.open(
        MessageOptions::new("Fractional")
            .styles(MessageStyles {
                root: gpui::StyleRefinement::default().h(px(35.5)),
                ..Default::default()
            })
            .duration(Duration::ZERO),
    );
    let layouts = f.layouts();
    assert_eq!(layouts[0].bounds.size.height, px(35.5));
    assert_eq!(layouts[1].bounds.top(), px(60.));
}

#[test]
fn message_percentage_card_dimensions_constrain_content_before_native_measurement() {
    let mut f = Fixture::new(false);
    let half = MessageStyles {
        root: gpui::StyleRefinement::default().w(gpui::relative(0.5)),
        ..Default::default()
    };
    f.open(
        MessageOptions::new(
            "A message with enough words to wrap into several lines in its half-width card.",
        )
        .styles(half.clone())
        .duration(Duration::ZERO),
    );
    let bounds = f.layouts()[0].bounds;
    assert_eq!(bounds.size.width, px(186.));
    assert!(bounds.size.height > px(40.));
    f.open(
        MessageOptions::content(|_, _, _| div().w_full().h(px(32.)).child("Factory"))
            .styles(half)
            .duration(Duration::ZERO),
    );
    let bounds = f.layouts()[0].bounds;
    assert_eq!(bounds.size.width, px(186.));
    assert_eq!(bounds.size.height, px(50.));
}

#[test]
fn message_fractional_padding_preserves_centered_rich_content_in_fixed_height_card() {
    for column in [false, true] {
        let mut f = Fixture::new(false);
        f.cx.update(|cx| {
            f.scope
                .update_patch(cx, |patch| {
                    patch.theme = Some(crate::theme::ThemeConfig {
                        algorithms: Some(crate::theme::AlgorithmChain::compact()),
                        ..Default::default()
                    });
                })
                .unwrap()
        });
        let root = if column {
            gpui::StyleRefinement::default()
                .flex()
                .flex_col()
                .justify_center()
                .h(px(60.))
        } else {
            gpui::StyleRefinement::default()
                .flex()
                .items_center()
                .h(px(60.))
        };
        f.open(
            MessageOptions::content(|_, _, _| {
                div()
                    .w(px(20.))
                    .h(px(20.))
                    .debug_selector(|| "centered-content".into())
            })
            .styles(MessageStyles {
                root,
                ..Default::default()
            })
            .duration(Duration::ZERO),
        );
        let mut visual = gpui::VisualTestContext::from_window(f.window.into(), &f.cx);
        let content = visual.debug_bounds("centered-content").unwrap();
        assert!((f32::from(content.top()) - 28.).abs() < 0.01, "{content:?}");
        assert_eq!(content.size.height, px(20.));
    }
}

fn painted_notice_opacity(f: &mut Fixture) -> f32 {
    f.cx.update_window(f.window.into(), |_, window, _| {
        window
            .rendered_scene()
            .quads
            .iter()
            .filter_map(|quad| {
                let color = quad.background.as_solid()?.to_rgb();
                let expected = gpui::rgb(0x7162ab);
                ((color.r - expected.r).abs() < 0.001
                    && (color.g - expected.g).abs() < 0.001
                    && (color.b - expected.b).abs() < 0.001)
                    .then_some(color.a)
            })
            .fold(0., f32::max)
    })
    .unwrap()
}

fn animated_notice() -> MessageOptions {
    MessageOptions::new("Interrupted motion")
        .styles(MessageStyles {
            root: gpui::StyleRefinement::default().bg(gpui::rgb(0x7162ab)),
            ..Default::default()
        })
        .duration(Duration::ZERO)
}

#[test]
fn message_closed_before_first_paint_never_flashes_during_exit() {
    let mut f = Fixture::new(true);
    f.cx.update(|cx| {
        f.scope
            .update_patch(cx, |patch| patch.message.top = Some(90.))
            .unwrap()
    });
    let messages = f.messages.clone();
    f.cx.update_window(f.window.into(), |_, window, cx| {
        let handle = messages.open(animated_notice(), window, cx);
        handle.close(window, cx);
    })
    .unwrap();
    f.draw();
    let mut previous = 0;
    for millis in [0, 50, 100, 150] {
        f.advance(millis - previous);
        previous = millis;
        assert_eq!(
            painted_notice_opacity(&mut f),
            0.,
            "unseen notice must remain invisible at t={millis}"
        );
    }
}

#[test]
fn message_unpresented_close_releases_factory_without_building_or_scheduling_frames() {
    let mut f = Fixture::new(true);
    let builds = Rc::new(Cell::new(0));
    let observed = builds.clone();
    let retained = Rc::new(());
    let weak = Rc::downgrade(&retained);
    let messages = f.messages.clone();
    f.cx.update_window(f.window.into(), |_, window, cx| {
        let handle = messages.open(
            MessageOptions::content(move |_, _, _| {
                let _anchor = retained.clone();
                observed.set(observed.get() + 1);
                div().child("Never presented")
            })
            .duration(Duration::ZERO),
            window,
            cx,
        );
        handle.close(window, cx);
        assert!(handle.is_dismissed());
    })
    .unwrap();
    f.draw();
    assert_eq!(builds.get(), 0);
    assert!(weak.upgrade().is_none());
    assert!(f.layouts().is_empty());
    let frames =
        f.cx.update_window(f.window.into(), |_, window, cx| {
            window.simulate_next_frame(cx)
        })
        .unwrap();
    assert_eq!(frames, 0);
}

#[test]
fn message_display_none_keeps_no_geometry_hover_or_loading_frames() {
    let mut f = Fixture::new(true);
    let builds = Rc::new(Cell::new(0));
    let observed = builds.clone();
    f.open(
        MessageOptions::content(move |_, _, _| {
            observed.set(observed.get() + 1);
            div().child("Hidden factory")
        })
        .kind(MessageType::Loading)
        .styles(MessageStyles {
            root: gpui::StyleRefinement::default().hidden(),
            ..Default::default()
        })
        .duration(Duration::ZERO),
    );
    assert_eq!(builds.get(), 0);
    assert!(f.layouts().is_empty());
    let frames =
        f.cx.update_window(f.window.into(), |_, window, cx| {
            window.simulate_next_frame(cx)
        })
        .unwrap();
    assert_eq!(frames, 0);
}

#[test]
fn message_semantic_hidden_loading_indicators_stop_requesting_frames() {
    for icon_style in [
        gpui::StyleRefinement::default().hidden(),
        gpui::StyleRefinement::default().opacity(0.),
    ] {
        let mut f = Fixture::new(true);
        f.open(
            MessageOptions::new("Hidden spinner")
                .kind(MessageType::Loading)
                .styles(MessageStyles {
                    icon: icon_style,
                    ..Default::default()
                })
                .duration(Duration::ZERO),
        );
        f.advance(220);
        let frames =
            f.cx.update_window(f.window.into(), |_, window, cx| {
                window.simulate_next_frame(cx)
            })
            .unwrap();
        assert_eq!(
            frames, 0,
            "an invisible semantic icon must not sustain a render loop"
        );
        assert_eq!(f.layouts().len(), 1);
    }
}

#[test]
fn message_transparent_root_suppresses_motion_until_revealed_without_restarting_entry() {
    let mut f = Fixture::new(true);
    f.open(
        MessageOptions::new("Transparent loading")
            .key("loading")
            .kind(MessageType::Loading)
            .styles(MessageStyles {
                root: gpui::StyleRefinement::default().opacity(0.),
                ..Default::default()
            })
            .duration(Duration::ZERO),
    );
    let frames =
        f.cx.update_window(f.window.into(), |_, window, cx| {
            window.simulate_next_frame(cx)
        })
        .unwrap();
    assert_eq!(
        frames, 0,
        "a transparent root must not animate entry, list movement or indicator"
    );
    f.advance(1000);
    f.open(
        MessageOptions::new("Visible loading")
            .key("loading")
            .kind(MessageType::Loading)
            .duration(Duration::ZERO),
    );
    assert_eq!(
        f.layouts()[0].bounds.top(),
        px(8.),
        "reveal samples the original clock rather than restarting entry"
    );
    let frames =
        f.cx.update_window(f.window.into(), |_, window, cx| {
            window.simulate_next_frame(cx)
        })
        .unwrap();
    assert!(frames > 0, "visible loading resumes indicator frames");
}

#[test]
fn message_exit_from_mid_entry_fades_monotonically_from_its_current_transform() {
    let mut f = Fixture::new(true);
    f.cx.update(|cx| {
        f.scope
            .update_patch(cx, |patch| patch.message.top = Some(90.))
            .unwrap()
    });
    let handle = f.open(animated_notice());
    f.advance(50);
    let mut alpha = painted_notice_opacity(&mut f);
    let mut y = f.layouts()[0].bounds.top();
    assert!(alpha > 0. && alpha < 1.);
    f.cx.update_window(f.window.into(), |_, window, cx| handle.close(window, cx))
        .unwrap();
    f.draw();
    assert!((painted_notice_opacity(&mut f) - alpha).abs() < 1e-6);
    assert!((f32::from(f.layouts()[0].bounds.top() - y)).abs() < 1e-5);
    for _ in 0..8 {
        f.advance(2);
        let next_alpha = painted_notice_opacity(&mut f);
        let next_y = f.layouts()[0].bounds.top();
        assert!(
            next_alpha <= alpha + 1e-6,
            "exit cannot brighten a partially entered notice"
        );
        assert!(
            next_y <= y + px(1e-5),
            "exit cannot move back toward the entry destination"
        );
        assert!(
            next_y >= px(26. - 1e-5),
            "exit cannot overshoot its -64px target"
        );
        alpha = next_alpha;
        y = next_y;
    }
    f.advance(2);
    assert!(
        f.layouts().is_empty(),
        "CSS reversal shortening completes this 50ms interrupted entry by 18ms"
    );
    assert_eq!(painted_notice_opacity(&mut f), 0.);
}

#[test]
fn message_stack_collapses_to_one_notice_and_expands_on_native_pointer_hover() {
    let mut f = Fixture::new(false);
    let state = f.state.clone();
    f.cx.update_window(f.window.into(), |_, window, cx| {
        state.update(cx, |state, cx| {
            state.set_config(
                MessageConfigPatch {
                    stack: Some(MessageStack::enabled()),
                    ..Default::default()
                },
                window,
                cx,
            );
        })
    })
    .unwrap();
    for index in 0..4 {
        f.open(
            MessageOptions::new(format!("Notice {index}"))
                .key(format!("{index}"))
                .duration(Duration::ZERO),
        );
    }
    let collapsed = f.layouts();
    assert_eq!(collapsed.len(), 1);
    assert_eq!(collapsed[0].key.as_ref(), "3");
    f.cx.update_window(f.window.into(), |_, window, cx| {
        window.dispatch_event(
            PlatformInput::MouseMove(MouseMoveEvent {
                position: collapsed[0].bounds.center(),
                ..Default::default()
            }),
            cx,
        );
    })
    .unwrap();
    f.draw();
    let expanded = f.layouts();
    assert_eq!(expanded.len(), 4);
    for cards in expanded.windows(2) {
        assert!((f32::from(cards[1].bounds.top() - cards[0].bounds.bottom()) - 16.).abs() < 0.5);
    }
    f.cx.update_window(f.window.into(), |_, window, cx| {
        window.dispatch_event(
            PlatformInput::MouseMove(MouseMoveEvent {
                position: point(px(4.), px(300.)),
                ..Default::default()
            }),
            cx,
        );
    })
    .unwrap();
    f.draw();
    assert_eq!(f.layouts().len(), 1);
}

#[test]
fn message_custom_factories_run_outside_controller_borrows() {
    let mut f = Fixture::new(false);
    let messages = f.messages.clone();
    let ran = Rc::new(Cell::new(false));
    let observed = ran.clone();
    f.open(
        MessageOptions::content(move |_, window, cx| {
            if !observed.replace(true) {
                messages.info("Opened from native content factory", window, cx);
            }
            div().child("Factory content")
        })
        .duration(Duration::ZERO),
    );
    assert!(ran.get());
    f.draw();
    assert_eq!(f.layouts().len(), 2);
}

#[test]
fn message_factory_that_dismisses_itself_never_paints_a_stale_snapshot() {
    let mut f = Fixture::new(false);
    let paints = Rc::new(Cell::new(0));
    let observed = paints.clone();
    let handle = f.open(
        MessageOptions::content(move |handle, window, cx| {
            handle.close(window, cx);
            let observed = observed.clone();
            gpui::canvas(
                |_, _, _| (),
                move |_, (), _, _| observed.set(observed.get() + 1),
            )
            .w(px(20.))
            .h(px(20.))
        })
        .duration(Duration::ZERO),
    );
    assert!(handle.is_dismissed());
    assert_eq!(paints.get(), 0);
    assert!(f.layouts().is_empty());
}

#[test]
fn message_host_unmount_releases_content_and_can_mount_the_controller_again() {
    let mut f = Fixture::new(false);
    let retained = Rc::new(());
    let weak_content = Rc::downgrade(&retained);
    let handle = f.open(
        MessageOptions::content(move |_, _, _| {
            let _retained = retained.clone();
            div().child("Owned content")
        })
        .duration(Duration::ZERO),
    );
    f.window
        .update(&mut f.cx, |owner, _, cx| {
            owner.mounted = false;
            cx.notify();
        })
        .unwrap();
    f.draw();
    // Keyed native state eviction happens after the replacing frame releases it.
    f.draw();
    assert!(handle.is_dismissed());
    assert!(weak_content.upgrade().is_none());
    assert!(f.layouts().is_empty());
    f.window
        .update(&mut f.cx, |owner, _, cx| {
            owner.mounted = true;
            cx.notify();
        })
        .unwrap();
    f.draw();
    let second = f.open(MessageOptions::new("Mounted again").duration(Duration::ZERO));
    assert!(!second.is_dismissed());
    assert_eq!(f.layouts().len(), 1);
}

#[test]
fn message_duplicate_hosts_render_a_controller_once() {
    let mut f = Fixture::new(false);
    let runs = Rc::new(Cell::new(0));
    let observed = runs.clone();
    f.open(
        MessageOptions::content(move |_, _, _| {
            observed.set(observed.get() + 1);
            div().child("One host")
        })
        .duration(Duration::ZERO),
    );
    let before =
        f.cx.update_window(f.window.into(), |_, window, _| {
            let scene = window.rendered_scene();
            (
                scene.shadows.len(),
                scene.quads.len(),
                scene.monochrome_sprites.len() + scene.subpixel_sprites.len(),
            )
        })
        .unwrap();
    runs.set(0);
    f.window
        .update(&mut f.cx, |owner, _, cx| {
            owner.duplicate = true;
            cx.notify();
        })
        .unwrap();
    f.draw();
    let after =
        f.cx.update_window(f.window.into(), |_, window, _| {
            let scene = window.rendered_scene();
            (
                scene.shadows.len(),
                scene.quads.len(),
                scene.monochrome_sprites.len() + scene.subpixel_sprites.len(),
            )
        })
        .unwrap();
    assert!(runs.get() > 0);
    assert_eq!(
        after, before,
        "a duplicate host must not paint duplicate shadows, cards or text"
    );
    assert_eq!(f.layouts().len(), 1);
}

#[test]
fn message_idle_notices_and_disabled_motion_schedule_no_frames() {
    let mut f = Fixture::new(false);
    f.open(
        MessageOptions::new("Static loading")
            .kind(MessageType::Loading)
            .duration(Duration::ZERO),
    );
    let frames =
        f.cx.update_window(f.window.into(), |_, window, cx| {
            window.simulate_next_frame(cx)
        })
        .unwrap();
    assert_eq!(frames, 0);
    let mut animated = Fixture::new(true);
    animated.open(MessageOptions::new("Settled").duration(Duration::ZERO));
    animated.advance(220);
    let frames = animated
        .cx
        .update_window(animated.window.into(), |_, window, cx| {
            window.simulate_next_frame(cx)
        })
        .unwrap();
    assert_eq!(frames, 0);
}

#[test]
fn message_scoped_semantic_styles_apply_to_native_notice_geometry() {
    let mut f = Fixture::new(false);
    f.cx.update(|cx| {
        f.scope
            .update_patch(cx, |patch: &mut ConfigPatch| {
                patch.message.styles.root = gpui::StyleRefinement::default().w(px(180.)).h(px(60.));
            })
            .unwrap()
    });
    f.open(MessageOptions::new("Styled").duration(Duration::ZERO));
    let bounds = f.layouts()[0].bounds;
    assert_eq!(bounds.size, size(px(180.), px(60.)));
}

#[test]
fn message_holder_semantic_padding_and_content_gap_match_reference() {
    let mut f = Fixture::new(false);
    f.cx.update(|cx| {
        f.scope
            .update_patch(cx, |patch| {
                patch.message.styles.list = gpui::StyleRefinement::default().p(px(31.));
                patch.message.styles.list_content = gpui::StyleRefinement::default().gap(px(37.));
            })
            .unwrap()
    });
    f.open(MessageOptions::new("Older").duration(Duration::ZERO));
    f.open(MessageOptions::new("Latest").duration(Duration::ZERO));
    let layouts = f.layouts();
    assert!((f32::from(layouts[0].bounds.top()) - 15.).abs() < 0.5);
    assert!((f32::from(layouts[1].bounds.top() - layouts[0].bounds.bottom()) - 37.).abs() < 0.5);
}

#[test]
fn message_dismissal_restores_focus_and_blocks_outgoing_rich_button_keyboard_activation() {
    let mut f = Fixture::new(true);
    let focus = f.cx.update(crate::behavior::focus_handle);
    let button_focus = focus.clone();
    let clicks = Rc::new(Cell::new(0));
    let observed = clicks.clone();
    let handle = f.open(
        MessageOptions::content(move |_, _, _| {
            let observed = observed.clone();
            crate::Button::new("outgoing-rich-button")
                .label("Rich action")
                .focus_handle(&button_focus)
                .wave(false)
                .on_click(move |_, _, _| observed.set(observed.get() + 1))
        })
        .duration(Duration::ZERO),
    );
    f.advance(220);
    f.cx.update_window(f.window.into(), |_, window, cx| window.focus(&focus, cx))
        .unwrap();
    f.draw();
    for key in ["enter", "space"] {
        f.cx.update_window(f.window.into(), |_, window, cx| {
            let keystroke = gpui::Keystroke::parse(key).unwrap();
            window.dispatch_event(
                PlatformInput::KeyDown(gpui::KeyDownEvent {
                    keystroke: keystroke.clone(),
                    is_held: false,
                    prefer_character_input: false,
                }),
                cx,
            );
            window.dispatch_event(PlatformInput::KeyUp(gpui::KeyUpEvent { keystroke }), cx);
        })
        .unwrap();
        f.cx.run_until_parked();
    }
    assert_eq!(
        clicks.get(),
        2,
        "presented rich buttons support Enter and Space before dismissal"
    );
    clicks.set(0);
    f.cx.update_window(f.window.into(), |_, window, cx| {
        assert!(focus.is_focused(window));
        handle.close(window, cx);
        assert!(
            !focus.is_focused(window),
            "logical dismissal relinquishes focus immediately"
        );
    })
    .unwrap();
    f.draw();
    assert!(f.layouts()[0].closing);
    f.cx.update_window(f.window.into(), |_, window, cx| {
        // GPUI's Tab action delegates to focus_next. Attempt activation before
        // deferred focus listeners run, then again after normal event delivery.
        window.focus_next(cx);
        let keystroke = gpui::Keystroke::parse("enter").unwrap();
        window.dispatch_event(
            PlatformInput::KeyDown(gpui::KeyDownEvent {
                keystroke: keystroke.clone(),
                is_held: false,
                prefer_character_input: false,
            }),
            cx,
        );
        window.dispatch_event(PlatformInput::KeyUp(gpui::KeyUpEvent { keystroke }), cx);
    })
    .unwrap();
    f.cx.run_until_parked();
    assert_eq!(clicks.get(), 0);
    f.cx.update_window(f.window.into(), |_, window, _| {
        assert!(!focus.is_focused(window))
    })
    .unwrap();
}

#[test]
fn message_outgoing_notice_does_not_block_active_rich_sibling_keyboard_actions() {
    let mut f = Fixture::new(true);
    let focus = f.cx.update(crate::behavior::focus_handle);
    let active_focus = focus.clone();
    let active_clicks = Rc::new(Cell::new(0));
    let observed = active_clicks.clone();
    f.open(
        MessageOptions::content(move |_, _, _| {
            let observed = observed.clone();
            crate::Button::new("active-sibling")
                .label("Active sibling")
                .focus_handle(&active_focus)
                .wave(false)
                .on_click(move |_, _, _| observed.set(observed.get() + 1))
        })
        .duration(Duration::ZERO),
    );
    f.advance(220);
    let closing_clicks = Rc::new(Cell::new(0));
    let observed = closing_clicks.clone();
    let outgoing = f.open(
        MessageOptions::content(move |_, _, _| {
            let observed = observed.clone();
            crate::Button::new("closing-sibling")
                .label("Closing sibling")
                .wave(false)
                .on_click(move |_, _, _| observed.set(observed.get() + 1))
        })
        .duration(Duration::ZERO),
    );
    f.advance(220);
    f.cx.update_window(f.window.into(), |_, window, cx| {
        window.focus(&focus, cx);
        outgoing.close(window, cx);
        assert!(focus.is_focused(window));
    })
    .unwrap();
    f.draw();
    assert_eq!(
        f.layouts().len(),
        2,
        "outgoing notice still paints its exit"
    );
    assert_eq!(
        f.layouts().iter().filter(|layout| layout.closing).count(),
        1
    );
    for key in ["enter", "space"] {
        f.cx.update_window(f.window.into(), |_, window, cx| {
            assert!(focus.is_focused(window));
            let keystroke = gpui::Keystroke::parse(key).unwrap();
            window.dispatch_event(
                PlatformInput::KeyDown(gpui::KeyDownEvent {
                    keystroke: keystroke.clone(),
                    is_held: false,
                    prefer_character_input: false,
                }),
                cx,
            );
            window.dispatch_event(PlatformInput::KeyUp(gpui::KeyUpEvent { keystroke }), cx);
        })
        .unwrap();
        f.cx.run_until_parked();
    }
    assert_eq!(
        active_clicks.get(),
        2,
        "outgoing guards must not consume a live sibling's Enter or Space"
    );
    assert_eq!(closing_clicks.get(), 0);
}

#[test]
fn message_rich_pointer_actions_end_at_dismissal_without_blocking_active_siblings() {
    fn click(position: gpui::Point<Pixels>, window: &mut Window, cx: &mut App) {
        window.dispatch_event(
            PlatformInput::MouseMove(MouseMoveEvent {
                position,
                ..Default::default()
            }),
            cx,
        );
        window.dispatch_event(
            PlatformInput::MouseDown(gpui::MouseDownEvent {
                button: gpui::MouseButton::Left,
                position,
                ..Default::default()
            }),
            cx,
        );
        window.dispatch_event(
            PlatformInput::MouseUp(gpui::MouseUpEvent {
                button: gpui::MouseButton::Left,
                position,
                ..Default::default()
            }),
            cx,
        );
    }
    let mut f = Fixture::new(true);
    let page_clicks = Rc::new(Cell::new(0));
    f.window
        .update(&mut f.cx, |owner, _, cx| {
            owner.page_clicks = Some(page_clicks.clone());
            cx.notify();
        })
        .unwrap();
    f.draw();
    let closing_clicks = Rc::new(Cell::new(0));
    let observed = closing_clicks.clone();
    let outgoing = f.open(
        MessageOptions::content(move |_, _, _| {
            let observed = observed.clone();
            div()
                .w(px(140.))
                .debug_selector(|| "stale-rich-button".into())
                .child(
                    crate::Button::new("closing-pointer-action")
                        .label("Closing action")
                        .w_full()
                        .wave(false)
                        .on_click(move |_, _, _| observed.set(observed.get() + 1)),
                )
        })
        .duration(Duration::ZERO),
    );
    f.advance(220);
    let active_clicks = Rc::new(Cell::new(0));
    let observed = active_clicks.clone();
    f.open(
        MessageOptions::content(move |_, _, _| {
            let observed = observed.clone();
            div()
                .w(px(140.))
                .debug_selector(|| "live-rich-button".into())
                .child(
                    crate::Button::new("active-pointer-action")
                        .label("Active action")
                        .w_full()
                        .wave(false)
                        .on_click(move |_, _, _| observed.set(observed.get() + 1)),
                )
        })
        .duration(Duration::ZERO),
    );
    f.advance(220);
    let mut visual = gpui::VisualTestContext::from_window(f.window.into(), &f.cx);
    let closing_position = visual.debug_bounds("stale-rich-button").unwrap().center();
    let active_position = visual.debug_bounds("live-rich-button").unwrap().center();
    let page_position = visual.debug_bounds("pointer-page-action").unwrap().center();
    f.cx.update_window(f.window.into(), |_, window, cx| {
        click(closing_position, window, cx);
        click(active_position, window, cx);
        click(page_position, window, cx);
    })
    .unwrap();
    f.draw();
    assert_eq!(
        closing_clicks.get(),
        1,
        "outgoing candidate works before dismissal"
    );
    assert_eq!(
        active_clicks.get(),
        1,
        "active sibling works before dismissal"
    );
    closing_clicks.set(0);
    active_clicks.set(0);
    assert_eq!(
        page_clicks.get(),
        1,
        "outside page action works before dismissal"
    );
    page_clicks.set(0);
    f.cx.update_window(f.window.into(), |_, window, cx| {
        outgoing.close(window, cx);
        assert!(outgoing.is_dismissed());
        // The old hitboxes and callbacks are still the last rendered frame:
        // neither draw() nor executor/event-loop delivery occurs before these clicks.
        click(closing_position, window, cx);
        click(active_position, window, cx);
        click(page_position, window, cx);
    })
    .unwrap();
    f.cx.run_until_parked();
    assert_eq!(
        closing_clicks.get(),
        0,
        "a logically dismissed rich child must not invoke its stale pointer callback"
    );
    assert_eq!(
        active_clicks.get(),
        1,
        "closing bounds must not block a live sibling"
    );
    assert_eq!(
        page_clicks.get(),
        1,
        "stale capture must not block outside page actions"
    );
    f.draw();
    f.advance(150);
    let exiting = f
        .layouts()
        .into_iter()
        .find(|layout| layout.closing)
        .unwrap();
    assert!(
        exiting.bounds.contains(&active_position),
        "exit visual overlaps the live button for the click-through proof"
    );
    f.cx.update_window(f.window.into(), |_, window, cx| {
        click(active_position, window, cx);
        click(page_position, window, cx);
    })
    .unwrap();
    f.cx.run_until_parked();
    assert_eq!(closing_clicks.get(), 0);
    assert_eq!(
        active_clicks.get(),
        2,
        "a freshly rendered exit must remain click-through"
    );
    assert_eq!(page_clicks.get(), 2);
}

#[test]
fn message_multiple_outgoing_rich_notices_do_not_cycle_focus_listeners() {
    let mut f = Fixture::new(true);
    let first_focus = f.cx.update(crate::behavior::focus_handle);
    let second_focus = f.cx.update(crate::behavior::focus_handle);
    let clicks = Rc::new(Cell::new(0));
    let first = first_focus.clone();
    let observed = clicks.clone();
    f.open(
        MessageOptions::content(move |_, _, _| {
            let observed = observed.clone();
            crate::Button::new("first-rich")
                .label("First")
                .focus_handle(&first)
                .on_click(move |_, _, _| observed.set(observed.get() + 1))
        })
        .duration(Duration::ZERO),
    );
    f.advance(220);
    f.cx.update_window(f.window.into(), |_, window, cx| {
        window.focus(&first_focus, cx)
    })
    .unwrap();
    f.draw();
    let second = second_focus.clone();
    let observed = clicks.clone();
    f.open(
        MessageOptions::content(move |_, _, _| {
            let observed = observed.clone();
            crate::Button::new("second-rich")
                .label("Second")
                .focus_handle(&second)
                .on_click(move |_, _, _| observed.set(observed.get() + 1))
        })
        .duration(Duration::ZERO),
    );
    f.advance(220);
    let messages = f.messages.clone();
    f.cx.update_window(f.window.into(), |_, window, cx| {
        window.focus(&second_focus, cx);
        messages.destroy_all(window, cx);
    })
    .unwrap();
    f.draw();
    assert_eq!(f.layouts().len(), 2);
    for reverse in [false, true, false, true] {
        f.cx.update_window(f.window.into(), |_, window, cx| {
            if reverse {
                window.focus_prev(cx);
            } else {
                window.focus_next(cx);
            }
            let keystroke = gpui::Keystroke::parse("enter").unwrap();
            window.dispatch_event(
                PlatformInput::KeyDown(gpui::KeyDownEvent {
                    keystroke: keystroke.clone(),
                    is_held: false,
                    prefer_character_input: false,
                }),
                cx,
            );
            window.dispatch_event(PlatformInput::KeyUp(gpui::KeyUpEvent { keystroke }), cx);
        })
        .unwrap();
        f.cx.run_until_parked();
        f.cx.update_window(f.window.into(), |_, window, _| {
            assert!(!first_focus.is_focused(window));
            assert!(!second_focus.is_focused(window));
        })
        .unwrap();
    }
    assert_eq!(clicks.get(), 0);
}

#[test]
fn message_native_content_height_shrink_matches_delayed_browser_motion() {
    let mut f = Fixture::new(true);
    f.open(MessageOptions::new("Older").duration(Duration::ZERO));
    let latest = f.open(MessageOptions::new("Latest").duration(Duration::ZERO));
    f.advance(220);
    f.cx.update_window(f.window.into(), |_, window, cx| latest.close(window, cx))
        .unwrap();
    f.draw();
    let mut previous = 0;
    for (millis, expected) in [
        (0, 96.),
        (200, 96.),
        (350, 91.109375),
        (500, 67.046875),
        (650, 43.84375),
        (800, 40.),
    ] {
        f.advance(millis - previous);
        previous = millis;
        let expected: f32 = expected;
        let mut visual = gpui::VisualTestContext::from_window(f.window.into(), &f.cx);
        let actual = visual
            .debug_bounds("message-list-content")
            .expect("native list content bounds");
        assert!(
            (f32::from(actual.size.height) - expected.round()).abs() < 0.04,
            "t={millis}ms: {actual:?}, expected native-pixel height={}",
            expected.round()
        );
    }
}
