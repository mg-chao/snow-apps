// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;
use crate::{
    Button, ConfigPatch, UiRoot,
    behavior::focus_handle,
    icons::IconName,
    theme::{Color, ThemeConfig},
};
use gpui::{
    AppContext, Bounds, Context, Entity, FocusHandle, InteractiveElement, IntoElement,
    KeyDownEvent, KeyUpEvent, Keystroke, MouseButton, MouseDownEvent, MouseUpEvent, Pixels,
    PlatformInput, Render, StyleRefinement, TestAppContext, Window, WindowHandle, div, point, px,
    size,
};
use std::{
    cell::{Cell, RefCell},
    rc::Rc,
    time::Duration,
};

const DOT: Color = Color::rgb(0x8b19d3);
const CHILD: Color = Color::rgb(0x32ba71);

struct Consumer {
    scope: ConfigScope,
    spinning: bool,
    mounted: bool,
    delay: Duration,
    percent: Option<SpinPercent>,
    nested: bool,
    fullscreen: bool,
    custom: bool,
    custom_spin: bool,
    custom_content: bool,
    offset: f32,
    generation: usize,
    observed: Rc<RefCell<Option<SpinStyleContext>>>,
    activations: Rc<Cell<usize>>,
    underlying_activations: Rc<Cell<usize>>,
    focus: FocusHandle,
    styles: SpinStyles,
    child_height: f32,
    description_color: Option<Rc<Cell<Option<gpui::Hsla>>>>,
}

impl Render for Consumer {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        let mut root = div()
            .size_full()
            .flex()
            .flex_col()
            .items_start()
            .overflow_hidden();
        if self.fullscreen {
            let activations = self.underlying_activations.clone();
            root = root.child(
                Button::new("underlying-page-action")
                    .scope(self.scope.clone())
                    .wave(false)
                    .label("Page action")
                    .on_click(move |_, _, _| activations.set(activations.get() + 1)),
            );
        }
        root = root.child(div().h(px(self.offset)).flex_shrink_0());
        if self.mounted {
            let observed = self.observed.clone();
            let mut spin = Spin::new(("spin-test", self.generation))
                .scope(self.scope.clone())
                .spinning(self.spinning)
                .delay(self.delay)
                .fullscreen(self.fullscreen)
                .description("Loading...")
                .accessibility_label("Loading results")
                .automation_id("spin.test")
                .styles(self.styles.clone())
                .styles_fn(move |context, _, _| {
                    *observed.borrow_mut() = Some(*context);
                    SpinStyles::default()
                });
            if let Some(percent) = self.percent {
                spin = spin.percent(percent);
            }
            if let Some(observed) = self.description_color.clone() {
                spin = spin.description_content(
                    gpui::canvas(
                        |_, _, _| (),
                        move |_, (), window, _| observed.set(Some(window.text_style().color)),
                    )
                    .w(px(40.))
                    .h(px(14.)),
                );
            }
            if self.custom {
                spin = spin
                    .indicator(IconName::LoadingOutlined)
                    .indicator_spin(self.custom_spin);
            }
            if self.custom_content {
                spin = spin
                    .indicator_content(div().size(px(20.)))
                    .indicator_spin(self.custom_spin);
            }
            if self.nested {
                let activations = self.activations.clone();
                spin = spin.child(
                    div().w(px(220.)).h(px(self.child_height)).bg(CHILD).child(
                        Button::new("spin-child")
                            .scope(self.scope.clone())
                            .label("Action")
                            .focus_handle(&self.focus)
                            .on_click(move |_, _, _| activations.set(activations.get() + 1)),
                    ),
                );
            }
            root = root.child(spin);
        }
        root
    }
}

struct Fixture {
    cx: TestAppContext,
    window: WindowHandle<UiRoot>,
    view: Entity<Consumer>,
    scope: ConfigScope,
}
impl Fixture {
    fn new(motion: bool, edit: impl FnOnce(&mut Consumer)) -> Self {
        let mut cx = TestAppContext::single();
        let mut view = None;
        let mut retained_scope = None;
        let window = cx.open_window(size(px(420.), px(360.)), |window, cx| {
            let scope = ConfigScope::new_window(cx);
            scope
                .update_patch(cx, |patch| patch.motion = Some(motion))
                .unwrap();
            let content = cx.new(|cx| {
                let mut consumer = Consumer {
                    scope: scope.clone(),
                    spinning: true,
                    mounted: true,
                    delay: Duration::ZERO,
                    percent: None,
                    nested: false,
                    fullscreen: false,
                    custom: false,
                    custom_spin: false,
                    custom_content: false,
                    offset: 0.,
                    generation: 0,
                    observed: Rc::default(),
                    activations: Rc::new(Cell::new(0)),
                    underlying_activations: Rc::new(Cell::new(0)),
                    focus: focus_handle(cx),
                    styles: SpinStyles {
                        indicator: StyleRefinement::default().text_color(DOT),
                        ..Default::default()
                    },
                    child_height: 100.,
                    description_color: None,
                };
                edit(&mut consumer);
                consumer
            });
            view = Some(content.clone());
            retained_scope = Some(scope.clone());
            UiRoot::new(scope, content, window, cx)
        });
        let mut fixture = Self {
            cx,
            window,
            view: view.unwrap(),
            scope: retained_scope.unwrap(),
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
    fn edit(&mut self, edit: impl FnOnce(&mut Consumer)) {
        self.view.update(&mut self.cx, |state, cx| {
            edit(state);
            cx.notify();
        });
        self.draw();
    }
    fn patch(&mut self, edit: impl FnOnce(&mut ConfigPatch)) {
        self.cx
            .update(|cx| self.scope.update_patch(cx, edit).unwrap());
        self.draw();
    }
    fn advance(&mut self, millis: u64) -> usize {
        self.cx
            .executor()
            .advance_clock(Duration::from_millis(millis));
        self.cx.run_until_parked();
        let count = self
            .cx
            .update_window(self.window.into(), |_, window, cx| {
                window.simulate_next_frame(cx)
            })
            .unwrap();
        self.draw();
        count
    }
    fn frames(&mut self) -> usize {
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                window.simulate_next_frame(cx)
            })
            .unwrap()
    }
    fn context(&self) -> SpinStyleContext {
        self.view
            .read_with(&self.cx, |view, _| view.observed.borrow().unwrap())
    }
    fn dots(&mut self) -> Vec<(Bounds<Pixels>, f32)> {
        self.cx
            .update_window(self.window.into(), |_, window, _| {
                let scale = window.scale_factor();
                window
                    .rendered_scene()
                    .paths
                    .iter()
                    .filter_map(|quad| {
                        let color = quad.color.as_solid()?.to_rgb();
                        let expected = DOT.to_rgba();
                        ((color.r - expected.r).abs() < 0.001
                            && (color.g - expected.g).abs() < 0.001
                            && (color.b - expected.b).abs() < 0.001)
                            .then(|| {
                                (
                                    Bounds::new(
                                        point(
                                            px(quad.bounds.origin.x.0 / scale),
                                            px(quad.bounds.origin.y.0 / scale),
                                        ),
                                        size(
                                            px(quad.bounds.size.width.0 / scale),
                                            px(quad.bounds.size.height.0 / scale),
                                        ),
                                    ),
                                    color.a,
                                )
                            })
                    })
                    .collect()
            })
            .unwrap()
    }
    fn child_bounds(&mut self) -> Bounds<Pixels> {
        self.cx
            .update_window(self.window.into(), |_, window, _| {
                let scale = window.scale_factor();
                let quad = window
                    .rendered_scene()
                    .quads
                    .iter()
                    .find(|quad| {
                        quad.background.as_solid().is_some_and(|color| {
                            let color = color.to_rgb();
                            let expected = CHILD.to_rgba();
                            (color.r - expected.r).abs() < 0.001
                                && (color.g - expected.g).abs() < 0.001
                        })
                    })
                    .expect("retained child");
                Bounds::new(
                    point(
                        px(quad.bounds.origin.x.0 / scale),
                        px(quad.bounds.origin.y.0 / scale),
                    ),
                    size(
                        px(quad.bounds.size.width.0 / scale),
                        px(quad.bounds.size.height.0 / scale),
                    ),
                )
            })
            .unwrap()
    }
    fn click_child(&mut self) {
        let position = self.child_bounds().origin + point(px(20.), px(15.));
        self.click_at(position);
    }
    fn click_at(&mut self, position: gpui::Point<Pixels>) {
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                window.dispatch_event(
                    PlatformInput::MouseDown(MouseDownEvent {
                        button: MouseButton::Left,
                        position,
                        ..Default::default()
                    }),
                    cx,
                );
                window.dispatch_event(
                    PlatformInput::MouseUp(MouseUpEvent {
                        button: MouseButton::Left,
                        position,
                        ..Default::default()
                    }),
                    cx,
                );
            })
            .unwrap();
        self.draw();
    }
    fn key_child(&mut self) {
        let focus = self.view.read_with(&self.cx, |view, _| view.focus.clone());
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                window.focus(&focus, cx);
                let keystroke = Keystroke::parse("enter").unwrap();
                window.dispatch_event(
                    PlatformInput::KeyDown(KeyDownEvent {
                        keystroke: keystroke.clone(),
                        is_held: false,
                        prefer_character_input: false,
                    }),
                    cx,
                );
                window.dispatch_event(PlatformInput::KeyUp(KeyUpEvent { keystroke }), cx);
            })
            .unwrap();
        self.draw();
    }
}

#[test]
fn estimated_progress_matches_reference_buckets_and_saturates() {
    assert_eq!(super::geometry::auto_percent(0), 0.);
    assert_eq!(super::geometry::auto_percent(1), 5.);
    assert!((super::geometry::auto_percent(235) - 96.0123896369).abs() < 1e-9);
    assert_eq!(
        super::geometry::auto_percent(u64::MAX),
        super::geometry::auto_percent(235)
    );
}

#[test]
fn dots_rotate_and_fade_without_owner_input_and_queue_one_frame() {
    let mut f = Fixture::new(true, |_| {});
    let initial = f.dots();
    assert_eq!(initial.len(), 4);
    assert!(initial.iter().all(|(_, alpha)| (*alpha - 0.3).abs() < 1e-6));
    for millis in [200, 200, 400, 400, 400, 400] {
        let before = f.dots();
        assert_eq!(f.advance(millis), 1);
        assert_ne!(
            f.dots(),
            before,
            "the retained element drives its own motion"
        );
        f.draw();
        f.draw(); // owner rerenders do not register duplicate callbacks
    }
    f.edit(|view| view.spinning = false);
    assert!(f.dots().is_empty());
    f.advance(400);
    assert_eq!(f.frames(), 0);
}

#[test]
fn delay_is_cancellable_and_unchanged_renders_do_not_restart_it() {
    let mut f = Fixture::new(false, |view| view.delay = Duration::from_millis(500));
    assert!(!f.context().spinning);
    assert!(f.dots().is_empty());
    assert_eq!(f.frames(), 0);
    f.advance(250);
    f.edit(|_| {});
    f.advance(249);
    assert!(!f.context().spinning);
    f.advance(1);
    assert!(f.context().spinning);
    assert_eq!(f.dots().len(), 4);
    f.edit(|view| view.spinning = false);
    assert!(!f.context().spinning);
    assert!(f.dots().is_empty());
    f.edit(|view| view.spinning = true);
    f.advance(499);
    f.edit(|view| view.spinning = false);
    f.advance(1000);
    assert!(f.dots().is_empty());
}

#[test]
fn changing_pending_delay_replaces_its_deadline_and_visible_delay_does_not_hide() {
    let mut f = Fixture::new(false, |view| view.delay = Duration::from_millis(500));
    f.advance(300);
    f.edit(|view| view.delay = Duration::from_millis(100));
    f.advance(99);
    assert!(!f.context().spinning);
    f.advance(1);
    assert!(f.context().spinning);
    f.edit(|view| view.delay = Duration::from_secs(10));
    assert!(f.context().spinning);
    assert_eq!(f.dots().len(), 4);
}

#[test]
fn unmount_cancels_delayed_show_and_remount_starts_fresh() {
    let mut f = Fixture::new(false, |view| view.delay = Duration::from_millis(500));
    f.advance(300);
    f.edit(|view| view.mounted = false);
    f.advance(1000);
    assert!(f.dots().is_empty());
    assert_eq!(f.frames(), 0);
    f.edit(|view| view.mounted = true);
    assert!(!f.context().spinning);
    f.advance(499);
    assert!(!f.context().spinning);
    f.advance(1);
    assert!(f.context().spinning);
}

#[test]
fn static_hidden_and_reduced_motion_indicators_do_not_schedule_frames() {
    for (spinning, percent, custom) in [
        (false, None, false),
        (true, Some(SpinPercent::Value(50.)), false),
        (true, None, true),
    ] {
        let mut f = Fixture::new(true, |view| {
            view.spinning = spinning;
            view.percent = percent;
            view.custom = custom;
        });
        assert_eq!(f.frames(), 0);
    }
    let mut f = Fixture::new(false, |_| {});
    assert_eq!(f.dots().len(), 4);
    assert_eq!(f.frames(), 0);
    let mut clipped = Fixture::new(true, |view| view.offset = 1000.);
    assert_eq!(clipped.frames(), 0);
    clipped.edit(|view| view.offset = 0.);
    assert_eq!(clipped.frames(), 1);
    clipped.edit(|view| view.offset = 1000.);
    clipped.advance(1);
    assert_eq!(clipped.frames(), 0);
}

#[test]
fn disabling_motion_settles_current_progress_and_turning_it_back_on_resumes_dots() {
    let mut f = Fixture::new(true, |_| {});
    assert_eq!(f.advance(200), 1);
    f.patch(|patch| patch.motion = Some(false));
    f.advance(1);
    assert_eq!(f.frames(), 0);
    f.patch(|patch| patch.motion = Some(true));
    assert_eq!(f.frames(), 1);
    f.edit(|view| view.percent = Some(SpinPercent::Value(50.)));
    f.advance(150);
    assert!(!f.dots().is_empty());
    f.advance(200);
    assert!(f.dots().is_empty());
    assert_eq!(f.frames(), 0);
    f.edit(|view| view.percent = Some(SpinPercent::Value(0.)));
    f.advance(300);
    assert_eq!(f.dots().len(), 4);
}

#[test]
fn automatic_progress_resets_on_restart_changes_on_ticks_and_stops_at_saturation() {
    let mut f = Fixture::new(false, |view| view.percent = Some(SpinPercent::Auto));
    assert_eq!(f.context().percent, Some(0.));
    f.advance(199);
    assert_eq!(f.context().percent, Some(0.));
    f.advance(1);
    assert_eq!(f.context().percent, Some(5.));
    f.advance(46_800);
    assert!((f.context().percent.unwrap() - 96.0123896369).abs() < 1e-9);
    assert_eq!(f.frames(), 0);
    f.advance(20_000);
    assert_eq!(f.frames(), 0);
    f.edit(|view| view.spinning = false);
    f.edit(|view| view.spinning = true);
    assert_eq!(f.context().percent, Some(0.));
    f.advance(200);
    assert_eq!(f.context().percent, Some(5.));
    f.edit(|view| view.percent = Some(SpinPercent::Value(32.)));
    f.advance(1000);
    assert_eq!(f.context().percent, Some(32.));
    assert_eq!(f.frames(), 0);
}

#[test]
fn custom_indicator_receives_raw_finite_percent_and_owns_animation() {
    let mut f = Fixture::new(true, |view| {
        view.custom = true;
        view.percent = Some(SpinPercent::Value(140.));
    });
    assert_eq!(f.context().percent, Some(140.));
    assert_eq!(f.frames(), 0);
    f.edit(|view| view.percent = Some(SpinPercent::Value(f64::NAN)));
    assert_eq!(f.context().percent, Some(0.));
    assert_eq!(f.frames(), 0);
    f.edit(|view| view.custom_spin = true);
    assert_eq!(f.frames(), 1);
    f.advance(125);
    assert!(f.context().rotation > 0.);
    f.edit(|view| view.custom_spin = false);
    f.advance(1);
    assert_eq!(f.frames(), 0);
}

#[test]
fn nested_loading_keeps_children_and_blocks_pointer_but_preserves_keyboard() {
    let mut f = Fixture::new(false, |view| view.nested = true);
    let bounds = f.child_bounds();
    f.click_child();
    assert_eq!(f.view.read_with(&f.cx, |view, _| view.activations.get()), 0);
    f.key_child();
    assert_eq!(f.view.read_with(&f.cx, |view, _| view.activations.get()), 1);
    f.edit(|view| view.spinning = false);
    assert_eq!(f.child_bounds(), bounds);
    f.click_child();
    assert_eq!(f.view.read_with(&f.cx, |view, _| view.activations.get()), 2);
}

#[test]
fn fullscreen_is_viewport_sized_outside_parent_clipping_and_stops_blocking_immediately() {
    let mut f = Fixture::new(false, |view| {
        view.nested = true;
        view.fullscreen = true;
        view.offset = 1000.;
    });
    assert!(f.context().spinning);
    assert_eq!(f.dots().len(), 4, "fixed content escapes local clip");
    f.click_child();
    assert_eq!(f.view.read_with(&f.cx, |view, _| view.activations.get()), 0);
    f.key_child();
    assert_eq!(f.view.read_with(&f.cx, |view, _| view.activations.get()), 1);
    f.edit(|view| view.spinning = false);
    f.click_at(point(px(20.), px(15.)));
    assert_eq!(
        f.view.read_with(&f.cx, |view, _| view.activations.get()),
        1,
        "invisible fullscreen children do not receive pointer input"
    );
    assert_eq!(
        f.view
            .read_with(&f.cx, |view, _| view.underlying_activations.get()),
        1,
        "the inactive overlay allows the page underneath to receive input"
    );
    f.key_child();
    assert_eq!(
        f.view.read_with(&f.cx, |view, _| view.activations.get()),
        2,
        "keyboard focus remains reference-compatible"
    );
}

#[test]
fn fullscreen_loading_blocks_modal_actions_until_loading_finishes() {
    let mut f = Fixture::new(false, |view| view.fullscreen = true);
    let clicks = Rc::new(Cell::new(0));
    let observed = clicks.clone();
    let scope = f.scope.clone();
    let modal =
        f.cx.update_window(f.window.into(), |_, window, cx| {
            crate::ModalManager::new(scope, window).confirm(
                crate::modal::ModalOptions {
                    content: Some(crate::modal::ModalContent::render(move |_, _, _| {
                        let observed = observed.clone();
                        div().debug_selector(|| "spin-modal-action".into()).child(
                            Button::new("modal-action")
                                .label("Apply")
                                .on_click(move |_, _, _| observed.set(observed.get() + 1)),
                        )
                    })),
                    ..Default::default()
                },
                window,
                cx,
            )
        })
        .unwrap();
    f.draw();
    let mut visual = gpui::VisualTestContext::from_window(f.window.into(), &f.cx);
    let position =
        visual.debug_bounds("spin-modal-action").unwrap().origin + point(px(20.), px(15.));
    f.click_at(position);
    assert_eq!(
        clicks.get(),
        0,
        "fullscreen loading must cover dialog actions"
    );
    f.edit(|view| view.spinning = false);
    f.click_at(position);
    assert_eq!(
        clicks.get(),
        1,
        "finishing loading releases the modal action"
    );
    f.cx.update(|cx| assert!(modal.is_open(cx)));
}

#[test]
fn scoped_defaults_inherit_without_executing_factories_and_local_styles_win() {
    fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<SpinConfig>();
    assert_send_sync::<crate::ResolvedConfig>();
    let indicator = SpinIndicator::new(IconName::LoadingOutlined);
    assert_eq!(indicator, indicator.clone());
    assert_ne!(indicator, SpinIndicator::new(IconName::LoadingOutlined));
    let mut f = Fixture::new(false, |_| {});
    f.patch(|patch| {
        patch.size = Some(Size::Small);
        patch.spin.indicator = Some(indicator.clone());
    });
    assert_eq!(f.context().size, Size::Small);
    assert!(f.dots().is_empty());
    f.patch(|patch| patch.spin = SpinConfigPatch::default());
    assert_eq!(f.dots().len(), 4);
    let mut theme = ThemeConfig::default();
    theme.components.set::<SpinTheme>(SpinTokenPatch {
        dot_size_sm: Some(28.),
        ..Default::default()
    });
    f.patch(|patch| patch.theme = Some(theme));
    let dots = f.dots();
    assert!((f32::from(dots[0].0.size.width) - 9.75).abs() < 1.);
}

#[test]
fn a_visible_tall_loading_region_does_not_animate_an_offscreen_indicator() {
    for percent in [None, Some(SpinPercent::Auto)] {
        let mut f = Fixture::new(true, |view| {
            view.nested = true;
            view.child_height = 2000.;
            view.percent = percent;
        });
        assert!(f.child_bounds().size.height > px(1000.));
        assert_eq!(
            f.frames(),
            0,
            "only the centered indicator's visibility can sustain motion"
        );
        f.advance(400);
        assert_eq!(f.frames(), 0);
        f.edit(|view| view.child_height = 100.);
        assert!(f.context().spinning);
        if percent.is_none() {
            assert_eq!(f.frames(), 1);
        }
    }
}

#[test]
fn a_delay_beyond_the_native_clock_range_is_cancellable_without_panicking() {
    let mut f = Fixture::new(false, |view| view.delay = Duration::MAX);
    assert!(!f.context().spinning);
    assert_eq!(f.frames(), 0);
    f.edit(|view| view.delay = Duration::ZERO);
    assert!(f.context().spinning);
}

#[test]
fn section_colors_inherit_into_default_and_custom_indicators_and_descriptions() {
    let red = Color::rgb(0xda2148);
    for custom in [false, true] {
        for nested in [false, true] {
            let observed = Rc::new(Cell::new(None));
            let mut f = Fixture::new(false, |view| {
                view.custom = custom;
                view.nested = nested;
                view.description_color = Some(observed.clone());
                view.styles = SpinStyles {
                    section: StyleRefinement::default().text_color(red),
                    ..Default::default()
                };
            });
            assert_eq!(
                observed.get(),
                Some(red.to_hsla()),
                "the native description inherits section currentColor"
            );
            f.edit(|view| {
                view.styles.description =
                    StyleRefinement::default().text_color(Color::rgb(0x123456))
            });
            assert_eq!(
                observed.get(),
                Some(Color::rgb(0x123456).to_hsla()),
                "a local description color overrides inheritance"
            );
        }
    }
}

#[test]
fn configured_factories_are_lazy_and_explicit_instance_indicators_take_precedence() {
    use std::sync::{
        Arc,
        atomic::{AtomicUsize, Ordering},
    };
    let mut f = Fixture::new(false, |_| {});
    let calls = Arc::new(AtomicUsize::new(0));
    let count = calls.clone();
    let indicator = SpinIndicator::from_content(move |_, _, _| {
        count.fetch_add(1, Ordering::Relaxed);
        div().size(px(20.))
    });
    f.cx.update(|cx| {
        let parent = ConfigScope::new_window(cx);
        parent
            .update_patch(cx, |patch| patch.spin.indicator = Some(indicator.clone()))
            .unwrap();
        let child = parent.child(ConfigPatch::default(), cx).unwrap();
        assert_eq!(child.snapshot(cx).spin.indicator.as_ref(), Some(&indicator));
        assert!(!child.set_patch(child.patch(cx), cx).unwrap());
        assert_eq!(
            calls.load(Ordering::Relaxed),
            0,
            "merging configuration never executes factories"
        );
    });
    f.patch(|patch| patch.spin.indicator = Some(indicator));
    assert!(calls.load(Ordering::Relaxed) > 0);
    let before = calls.load(Ordering::Relaxed);
    f.edit(|view| view.custom = true);
    assert_eq!(calls.load(Ordering::Relaxed), before);
    f.edit(|view| view.custom = false);
    assert!(calls.load(Ordering::Relaxed) > before);
}

#[test]
fn local_static_content_does_not_inherit_an_unused_scoped_indicators_animation() {
    let mut f = Fixture::new(true, |view| view.custom_content = true);
    f.patch(|patch| {
        patch.spin.indicator = Some(SpinIndicator::new(IconName::LoadingOutlined).animated(true))
    });
    assert_eq!(f.frames(), 0);
    f.edit(|view| view.custom_spin = true);
    assert_eq!(f.frames(), 1);
    f.edit(|view| view.custom_spin = false);
    f.advance(1);
    assert_eq!(f.frames(), 0);
}

#[test]
fn changing_indicator_source_remounts_dot_motion_without_resetting_auto_progress() {
    let mut f = Fixture::new(true, |_| {});
    let initial = f.dots();
    f.advance(800);
    assert_ne!(f.dots(), initial);
    f.edit(|view| view.custom = true);
    f.advance(1000);
    f.edit(|view| view.custom = false);
    assert_eq!(
        f.dots(),
        initial,
        "a fresh default indicator restarts at45 degrees and low alpha"
    );
    let mut auto = Fixture::new(false, |view| view.percent = Some(SpinPercent::Auto));
    auto.advance(800);
    let before = auto.context().percent.unwrap();
    auto.edit(|view| view.custom = true);
    auto.advance(1000);
    auto.edit(|view| view.custom = false);
    assert!(
        auto.context().percent.unwrap() > before,
        "estimated progress belongs to the loading request rather than its indicator"
    );
}

#[test]
fn numeric_progress_latches_hidden_rings_and_remounts_them_after_custom_content() {
    let mut f = Fixture::new(true, |view| view.percent = Some(SpinPercent::Value(-20.)));
    assert_eq!(f.dots().len(), 4);
    f.edit(|view| view.percent = Some(SpinPercent::Value(60.)));
    f.advance(150);
    assert!(
        !f.dots().is_empty(),
        "previous hidden ring and dots crossfade"
    );
    f.advance(200);
    assert!(f.dots().is_empty());
    assert_eq!(f.frames(), 0);
    f.edit(|view| view.custom = true);
    f.edit(|view| view.custom = false);
    assert!(f.dots().is_empty());
    assert_eq!(f.frames(), 0, "fresh positive ring starts settled");
}

#[test]
fn moving_dots_preserve_fractional_geometry_in_the_native_scene() {
    let mut f = Fixture::new(true, |_| {});
    for _ in 0..8 {
        for (bounds, _) in f.dots() {
            assert!((f32::from(bounds.size.width) - 6.75).abs() < 0.0001);
            assert!((f32::from(bounds.size.height) - 6.75).abs() < 0.0001);
        }
        f.advance(125);
    }
}

#[test]
fn hidden_native_windows_suspend_animation_and_resume_when_presented() {
    let mut f = Fixture::new(true, |_| {});
    assert_eq!(f.frames(), 1);
    f.cx.simulate_window_visibility_change(f.window.into(), gpui::WindowVisibility::Hidden);
    f.draw();
    // A frame queued just before the presentation change may still drain once.
    f.frames();
    f.advance(1);
    assert_eq!(f.frames(), 0);
    f.cx.simulate_window_visibility_change(f.window.into(), gpui::WindowVisibility::Visible);
    f.draw();
    assert_eq!(f.frames(), 1);
}

#[test]
fn fullscreen_fades_for_200ms_and_releases_pointer_input_before_the_fade_finishes() {
    let mut f = Fixture::new(true, |view| {
        view.fullscreen = true;
        view.nested = true;
    });
    let mask_alpha = |f: &mut Fixture| {
        f.cx.update_window(f.window.into(), |_, window, _| {
            window.rendered_scene().quads.iter().find_map(|quad| {
                let color = quad.background.as_solid()?.to_rgb();
                ((quad.bounds.size.width.0
                    - f32::from(window.viewport_size().width) * window.scale_factor())
                .abs()
                    < 0.001
                    && (quad.bounds.size.height.0
                        - f32::from(window.viewport_size().height) * window.scale_factor())
                    .abs()
                        < 0.001
                    && color.r == 0.
                    && color.g == 0.
                    && color.b == 0.)
                    .then_some(color.a)
            })
        })
        .unwrap()
    };
    assert!((mask_alpha(&mut f).unwrap() - 0.45).abs() < 0.0001);
    f.edit(|view| view.spinning = false);
    assert!(f.dots().is_empty());
    f.click_at(point(px(20.), px(15.)));
    assert_eq!(
        f.view
            .read_with(&f.cx, |view, _| view.underlying_activations.get()),
        1
    );
    f.advance(100);
    let alpha = mask_alpha(&mut f).unwrap();
    assert!(alpha > 0. && alpha < 0.45);
    f.advance(100);
    assert!(
        mask_alpha(&mut f).unwrap_or(0.) < 0.000001,
        "fullscreen mask must be fully transparent after200ms"
    );
    assert_eq!(f.frames(), 0);
}

#[test]
fn automatic_progress_has_one_wakeup_owner_and_keeps_coarse_updates_for_static_content() {
    struct Watch {
        _subscription: gpui::Subscription,
    }
    impl Render for Watch {
        fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
            div()
        }
    }
    let mut cx = TestAppContext::single();
    let notifications = Rc::new(Cell::new(0));
    let observed = notifications.clone();
    let mut retained = None;
    let window = cx.open_window(size(px(300.), px(200.)), |window, cx| {
        let scope = ConfigScope::new_window(cx);
        let state = cx.new(|cx| super::state::SpinState::new(scope.clone(), window, cx));
        let subscription = cx.observe(&state, move |_, _, _| observed.set(observed.get() + 1));
        state.update(cx, |state, cx| {
            let input = Spin::new("wakeup-owner").percent(SpinPercent::Auto);
            state.update(&input, scope, window, cx);
            state.schedule(true, true, window, cx);
        });
        retained = Some(state);
        Watch {
            _subscription: subscription,
        }
    });
    let state = retained.unwrap();
    cx.run_until_parked();
    notifications.set(0);
    cx.executor().advance_clock(Duration::from_millis(200));
    cx.run_until_parked();
    assert_eq!(
        notifications.get(),
        0,
        "continuous frame ownership must suppress redundant200ms notifications"
    );
    cx.update_window(window.into(), |_, window, cx| {
        assert_eq!(window.simulate_next_frame(cx), 1);
    })
    .unwrap();
    cx.run_until_parked();
    assert_eq!(notifications.get(), 1);
    notifications.set(0);
    cx.update_window(window.into(), |_, window, cx| {
        state.update(cx, |state, cx| state.schedule(true, false, window, cx))
    })
    .unwrap();
    cx.executor().advance_clock(Duration::from_millis(200));
    cx.run_until_parked();
    assert_eq!(
        notifications.get(),
        1,
        "static or reduced-motion automatic indicators still receive coarse progress updates"
    );
}
