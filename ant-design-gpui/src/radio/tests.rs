// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::{cell::RefCell, rc::Rc, time::Duration};

use gpui::{
    AppContext, Context, Element, Entity, FocusHandle, IntoElement, KeyDownEvent, KeyUpEvent,
    KeyboardButton, Keystroke, MouseButton, MouseDownEvent, MouseUpEvent, PlatformInput, Render,
    RenderOnce, Role, StyleRefinement, Styled, TestAppContext, Window, WindowHandle, accesskit,
    div, point, px,
};

use super::*;
use crate::{ConfigPatch, UiRoot};

struct Consumer {
    scope: ConfigScope,
    focus: FocusHandle,
    second_focus: FocusHandle,
    changes: Rc<RefCell<Vec<RadioChange>>>,
    semantics: Rc<RefCell<accesskit::Node>>,
    checked: Option<bool>,
    default_checked: bool,
    disabled: Option<bool>,
    visible: bool,
    label: &'static str,
    external_focus: bool,
    style: StyleRefinement,
    icon_style: Option<StyleRefinement>,
    button: bool,
}

impl Consumer {
    fn new(scope: ConfigScope, cx: &mut Context<Self>) -> Self {
        Self {
            scope,
            focus: focus_handle(cx),
            second_focus: focus_handle(cx),
            changes: Rc::default(),
            semantics: Rc::new(RefCell::new(accesskit::Node::new(Role::RadioButton))),
            checked: None,
            default_checked: false,
            disabled: None,
            visible: true,
            label: "Accept terms",
            external_focus: true,
            style: StyleRefinement::default(),
            icon_style: None,
            button: false,
        }
    }
}

impl Render for Consumer {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let mut row = div()
            .flex()
            .items_start()
            .p(px(12.))
            .gap(px(20.))
            .w(px(320.));
        if self.visible {
            let changes = self.changes.clone();
            let mut radio = Radio::new("radio-test-first")
                .scope(self.scope.clone())
                .label(self.label)
                .default_checked(self.default_checked)
                .automation_id("radio-test.first")
                .accessibility_description("Agree to the terms")
                .on_change(move |change, _, _| changes.borrow_mut().push(*change));
            if self.button {
                radio = radio.option_type(RadioOptionType::Button);
            }
            if let Some(checked) = self.checked {
                radio = radio.checked(checked);
            }
            if let Some(disabled) = self.disabled {
                radio = radio.disabled(disabled);
            }
            if self.external_focus {
                radio = radio.focus_handle(&self.focus);
            }
            radio.style().refine(&self.style);
            if let Some(refinement) = &self.icon_style {
                radio = radio.icon_style(refinement.clone());
            }
            let element = radio.render(window, cx).into_element();
            let mut semantics = accesskit::Node::new(element.a11y_role().unwrap());
            element.write_a11y_info(&mut semantics);
            *self.semantics.borrow_mut() = semantics;
            row = row.child(element);
        }
        row.child(
            Radio::new("radio-test-second")
                .label("Next")
                .scope(self.scope.clone())
                .focus_handle(&self.second_focus),
        )
    }
}

struct CachedPanel(Entity<Consumer>);
impl Render for CachedPanel {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        div().size_full().child(
            self.0
                .clone()
                .cached(StyleRefinement::default().w(px(320.)).h(px(180.))),
        )
    }
}

struct Fixture {
    cx: TestAppContext,
    window: WindowHandle<UiRoot>,
    consumer: Entity<Consumer>,
    scope: ConfigScope,
}

impl Fixture {
    fn new(configure: impl FnOnce(&mut Consumer)) -> Self {
        Self::with_motion(false, configure)
    }

    fn with_motion(motion: bool, configure: impl FnOnce(&mut Consumer)) -> Self {
        let mut cx = TestAppContext::single();
        let mut consumer = None;
        let mut scope = None;
        let window = cx.add_window(|window, cx| {
            let root_scope = ConfigScope::new_window(cx);
            let radio_scope = root_scope
                .child(
                    ConfigPatch {
                        motion: Some(motion),
                        ..Default::default()
                    },
                    cx,
                )
                .unwrap();
            let content = cx.new(|cx| {
                let mut consumer = Consumer::new(radio_scope.clone(), cx);
                configure(&mut consumer);
                consumer
            });
            let panel = cx.new(|_| CachedPanel(content.clone()));
            consumer = Some(content);
            scope = Some(radio_scope);
            UiRoot::new(root_scope, panel, window, cx)
        });
        let mut fixture = Self {
            cx,
            window,
            consumer: consumer.unwrap(),
            scope: scope.unwrap(),
        };
        fixture.draw();
        fixture
    }

    fn draw(&mut self) {
        self.cx.run_until_parked();
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                window.draw(cx).clear(cx);
            })
            .unwrap();
        self.cx.run_until_parked();
    }

    fn edit(&mut self, edit: impl FnOnce(&mut Consumer)) {
        self.consumer.update(&mut self.cx, |consumer, cx| {
            edit(consumer);
            cx.notify();
        });
        self.draw();
    }

    fn key_event(&mut self, stroke: &'static str, down: bool, held: bool) {
        let keystroke = Keystroke::parse(stroke).unwrap();
        let event = if down {
            PlatformInput::KeyDown(KeyDownEvent {
                keystroke,
                is_held: held,
                prefer_character_input: false,
            })
        } else {
            PlatformInput::KeyUp(KeyUpEvent { keystroke })
        };
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                window.dispatch_event(event, cx)
            })
            .unwrap();
    }

    fn key(&mut self, stroke: &'static str) {
        self.key_event(stroke, true, false);
        self.key_event(stroke, false, false);
        self.draw();
    }

    fn pointer(&mut self, down: bool, x: f32) {
        let position = point(px(x), px(23.));
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
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                window.dispatch_event(event, cx)
            })
            .unwrap();
    }

    fn click_at(&mut self, x: f32) {
        self.pointer(true, x);
        self.pointer(false, x);
        self.draw();
    }

    fn node(&self) -> accesskit::Node {
        self.consumer
            .read_with(&self.cx, |c, _| c.semantics.borrow().clone())
    }
    fn changes(&self) -> Vec<RadioChange> {
        self.consumer
            .read_with(&self.cx, |c, _| c.changes.borrow().clone())
    }
    fn checked(&self) -> bool {
        self.node().toggled() == Some(Toggled::True)
    }

    fn box_bounds(&mut self) -> gpui::Bounds<gpui::Pixels> {
        self.cx
            .update_window(self.window.into(), |_, window, _| {
                let scale = window.scale_factor();
                let quad = window
                    .rendered_scene()
                    .quads
                    .iter()
                    .filter(|quad| {
                        (quad.bounds.size.width.0 / scale - 16.).abs() < 0.01
                            && quad.border_color.a > 0.
                    })
                    .min_by(|left, right| {
                        left.bounds.origin.x.0.total_cmp(&right.bounds.origin.x.0)
                    })
                    .expect("radio box quad");
                gpui::Bounds::new(
                    point(
                        px(quad.bounds.origin.x.0 / scale),
                        px(quad.bounds.origin.y.0 / scale),
                    ),
                    gpui::size(
                        px(quad.bounds.size.width.0 / scale),
                        px(quad.bounds.size.height.0 / scale),
                    ),
                )
            })
            .unwrap()
    }

    fn focused(&mut self, second: bool) -> bool {
        let focus = self.consumer.read_with(&self.cx, |c, _| {
            if second {
                c.second_focus.clone()
            } else {
                c.focus.clone()
            }
        });
        self.cx
            .update_window(self.window.into(), |_, window, _| focus.is_focused(window))
            .unwrap()
    }

    fn frame(&mut self, millis: u64) -> usize {
        self.cx
            .executor()
            .advance_clock(Duration::from_millis(millis));
        let count = self
            .cx
            .update_window(self.window.into(), |_, window, cx| {
                window.simulate_next_frame(cx)
            })
            .unwrap();
        self.draw();
        count
    }
}

#[test]
fn radio_selects_once_from_label_and_space_and_never_toggles_off() {
    for button in [false, true] {
        let mut f = Fixture::new(|c| c.button = button);
        f.key("tab");
        assert!(f.focused(false));
        f.key("enter");
        f.key("ctrl-space");
        assert!(f.changes().is_empty());
        f.key_event("space", true, false);
        f.key_event("space", true, true);
        f.key_event("space", false, false);
        f.draw();
        assert!(f.checked());
        assert_eq!(f.changes().len(), 1);
        f.click_at(20.);
        f.click_at(70.);
        f.key("space");
        assert!(f.checked());
        assert_eq!(
            f.changes(),
            [RadioChange {
                checked: true,
                activation: Activation::Keyboard(KeyboardButton::Space)
            }]
        );
    }
}

#[test]
fn radio_controlled_owner_can_refuse_and_defaults_only_initialize_on_mount() {
    let mut f = Fixture::new(|c| {
        c.checked = Some(false);
        c.default_checked = true;
    });
    f.click_at(70.);
    f.click_at(20.);
    assert!(!f.checked());
    assert_eq!(f.changes().len(), 2);
    f.edit(|c| c.checked = Some(true));
    f.click_at(20.);
    assert!(f.checked());
    assert_eq!(f.changes().len(), 2);
    f.edit(|c| {
        c.checked = None;
        c.default_checked = false;
    });
    assert!(f.checked());
    f.edit(|c| c.visible = false);
    f.edit(|c| c.visible = true);
    assert!(!f.checked());
}

#[test]
fn radio_disabled_skips_traversal_and_explicit_false_overrides_provider() {
    let mut f = Fixture::new(|c| c.disabled = Some(true));
    assert!(f.node().is_disabled());
    assert!(!f.node().supports_action(accesskit::Action::Click));
    f.click_at(70.);
    f.key("tab");
    assert!(f.focused(true));
    assert!(f.changes().is_empty());
    let scope = f.scope.clone();
    f.cx.update(|cx| scope.update_patch(cx, |p| p.disabled = Some(true)).unwrap());
    f.edit(|c| c.disabled = Some(false));
    f.key("shift-tab");
    assert!(f.focused(false));
    f.key("space");
    assert!(f.checked());
}

#[test]
fn radio_cancelled_and_disable_roundtrip_presses_never_select() {
    let mut f = Fixture::new(|_| {});
    f.pointer(true, 20.);
    f.pointer(false, 310.);
    f.key("tab");
    f.key_event("space", true, false);
    let scope = f.scope.clone();
    for disabled in [true, false] {
        f.cx.update(|cx| {
            scope
                .update_patch(cx, |p| p.disabled = Some(disabled))
                .unwrap()
        });
        f.cx.run_until_parked();
    }
    f.key_event("space", false, false);
    f.draw();
    assert!(f.changes().is_empty());
    assert!(!f.focused(false));
}

#[test]
fn radio_scope_updates_repaint_cached_parent_and_internal_focus_survives() {
    let mut f = Fixture::new(|c| c.external_focus = false);
    f.key("tab");
    let focus =
        f.cx.update_window(f.window.into(), |_, w, cx| w.focused(cx).unwrap())
            .unwrap();
    f.edit(|c| c.label = "Allow notifications");
    f.cx.update_window(f.window.into(), |_, w, _| assert!(focus.is_focused(w)))
        .unwrap();
    assert_eq!(f.node().role(), Role::RadioButton);
    assert_eq!(f.node().label(), Some("Allow notifications"));
    assert_eq!(f.node().description(), Some("Agree to the terms"));
    let scope = f.scope.clone();
    f.cx.update(|cx| scope.update_patch(cx, |p| p.disabled = Some(true)).unwrap());
    f.draw();
    assert!(f.node().is_disabled());
    f.key("space");
    assert!(f.changes().is_empty());
}

#[test]
fn radio_idle_mounted_selected_and_reduced_motion_schedule_no_frames() {
    for button in [false, true] {
        let mut f = Fixture::with_motion(true, |c| {
            c.default_checked = true;
            c.button = button;
        });
        assert_eq!(f.frame(1000), 0);
        let mut f = Fixture::new(|c| c.button = button);
        f.click_at(20.);
        assert!(f.checked());
        assert_eq!(f.frame(1000), 0);
    }
}

#[test]
fn radio_dot_animates_without_assets_and_quiesces() {
    let mut f = Fixture::with_motion(true, |_| {});
    f.click_at(20.);
    assert!(f.checked());
    assert!(f.frame(100) > 0);
    assert!(f.frame(300) > 0);
    assert_eq!(f.frame(1000), 0);
    let scope = f.scope.clone();
    f.cx.update(|cx| scope.update_patch(cx, |p| p.motion = Some(false)).unwrap());
    f.edit(|c| c.checked = Some(false));
    assert_eq!(f.frame(1000), 0);
}

#[test]
fn radio_icon_geometry_focus_outline_and_rtl_follow_native_layout() {
    let mut f = Fixture::new(|_| {});
    let bounds = f.box_bounds();
    assert_eq!(bounds.size, gpui::size(px(16.), px(16.)));
    f.key("tab");
    f.cx.update_window(f.window.into(), |_, w, _| {
        let scale = w.scale_factor();
        let ring = w
            .rendered_scene()
            .quads
            .iter()
            .find(|q| q.border_widths.top.0 / scale == 3.)
            .expect("focus ring");
        assert_eq!(ring.bounds.size.width.0 / scale, 24.);
        assert_eq!(ring.corner_radii.top_left.0 / scale, 12.);
    })
    .unwrap();
    let scope = f.scope.clone();
    f.cx.update(|cx| {
        scope
            .update_patch(cx, |p| p.direction = Some(Direction::Rtl))
            .unwrap()
    });
    f.draw();
    assert!(f.box_bounds().origin.x > bounds.origin.x);
}

#[test]
fn radio_dot_is_centered_and_uses_one_native_circle_path() {
    let mut f = Fixture::new(|c| c.default_checked = true);
    let circle = f.box_bounds();
    f.cx.update_window(f.window.into(), |_, window, _| {
        let scale = window.scale_factor();
        let dots = &window.rendered_scene().paths;
        assert_eq!(dots.len(), 1);
        let dot = &dots[0];
        assert_eq!(
            dot.bounds.center().x.0 / scale,
            f32::from(circle.center().x)
        );
        assert_eq!(
            dot.bounds.center().y.0 / scale,
            f32::from(circle.center().y)
        );
        assert!((dot.bounds.size.width.0 / scale - 6.).abs() < 0.05);
        assert!((dot.bounds.size.height.0 / scale - 6.).abs() < 0.05);
        assert_eq!(
            dot.color.as_solid(),
            Some(crate::theme::Color::WHITE.to_hsla())
        );
    })
    .unwrap();
}

#[test]
fn radio_dot_stays_centered_at_fractional_scale_through_selection() {
    let mut f = Fixture::with_motion(true, |_| {});
    f.cx.update_window(f.window.into(), |_, window, _| window.set_scale_factor(1.5))
        .unwrap();
    f.draw();
    let circle = f.box_bounds();
    f.edit(|c| c.checked = Some(true));
    assert!(f.checked());
    let curve = crate::theme::ThemeConfig::default()
        .resolve()
        .tokens
        .motion_ease_in_out_circ;
    let mut elapsed = 0;
    for delay in [100, 50, 50, 100] {
        f.frame(delay);
        elapsed += delay;
        f.cx.update_window(f.window.into(), |_, window, _| {
            let scale = window.scale_factor();
            let dot = window
                .rendered_scene()
                .paths
                .iter()
                .find(|path| path.color.as_solid().is_some_and(|color| color.l > 0.99))
                .expect("animated dot path");
            let dot_center = dot.bounds.center();
            assert!(
                (dot_center.x.0 / scale - f32::from(circle.center().x)).abs() < 0.01,
                "dot center x drifts at {elapsed} ms"
            );
            assert!(
                (dot_center.y.0 / scale - f32::from(circle.center().y)).abs() < 0.01,
                "dot center y drifts at {elapsed} ms"
            );
            let expected =
                crate::behavior::evaluate_cubic_bezier(curve, elapsed as f64 / 300.) as f32;
            assert!(
                (dot.bounds.size.width.0 / scale - 6. * expected).abs() < 0.05,
                "dot width departs from the reference scale at {elapsed} ms"
            );
            assert!(
                (dot.color.as_solid().unwrap().a - expected).abs() < 0.01,
                "dot opacity departs from the reference curve at {elapsed} ms"
            );
        })
        .unwrap();
    }
    f.edit(|c| c.checked = Some(false));
    f.frame(150);
    f.cx.update_window(f.window.into(), |_, window, _| {
        let scale = window.scale_factor();
        let dot = &window.rendered_scene().paths[0];
        let reverse = 1. - crate::behavior::evaluate_cubic_bezier(curve, 0.5) as f32;
        assert!(
            (dot.bounds.size.width.0 / scale - 6. * reverse).abs() < 0.05,
            "dot width departs from the reference curve on deselection"
        );
        assert!(
            (dot.color.as_solid().unwrap().a - reverse).abs() < 0.01,
            "dot opacity departs from the reference curve on deselection"
        );
    })
    .unwrap();
    f.frame(150);
    f.cx.update_window(f.window.into(), |_, window, _| {
        assert!(window.rendered_scene().paths.is_empty());
    })
    .unwrap();
}

#[test]
fn radio_dot_uses_painted_ring_center_with_custom_border_widths() {
    let mut f = Fixture::new(|c| {
        c.default_checked = true;
        c.icon_style = Some(StyleRefinement::default().border_l(px(3.)).border_t(px(2.)));
    });
    f.cx.update_window(f.window.into(), |_, window, _| window.set_scale_factor(1.5))
        .unwrap();
    f.draw();
    let circle = f.box_bounds();
    f.cx.update_window(f.window.into(), |_, window, _| {
        let scale = window.scale_factor();
        let dot = &window.rendered_scene().paths[0];
        assert!((dot.bounds.center().x.0 / scale - f32::from(circle.center().x)).abs() < 0.01);
        assert!((dot.bounds.center().y.0 / scale - f32::from(circle.center().y)).abs() < 0.01);
    })
    .unwrap();
}

#[test]
fn radio_dot_color_uses_its_own_slow_transition() {
    let mut f = Fixture::with_motion(true, |c| c.default_checked = true);
    let theme = crate::theme::ThemeConfig::default().resolve();
    let tokens = theme.component::<RadioTheme>();
    let from = RadioStyle::from_tokens(&tokens, true, false).rest.mark;
    let to = RadioStyle::from_tokens(&tokens, true, true).rest.mark;
    f.edit(|c| c.disabled = Some(true));
    f.frame(100);
    let actual =
        f.cx.update_window(f.window.into(), |_, window, _| {
            window.rendered_scene().paths[0].color.as_solid().unwrap()
        })
        .unwrap()
        .to_rgb();
    let progress = crate::behavior::evaluate_cubic_bezier(
        tokens.globals().motion_ease_in_out_circ,
        100. / tokens.globals().motion_duration_slow.as_millis() as f64,
    ) as f32;
    let expected = interpolate_color(from, to, progress).to_rgb();
    for (actual, expected) in [
        (actual.r, expected.r),
        (actual.g, expected.g),
        (actual.b, expected.b),
        (actual.a, expected.a),
    ] {
        assert!((actual - expected).abs() < 0.01);
    }
}

#[test]
fn radio_dot_size_token_changes_follow_the_slow_curve() {
    let mut f = Fixture::with_motion(true, |c| c.default_checked = true);
    let mut theme = crate::theme::ThemeConfig::default();
    theme.components.set::<RadioTheme>(RadioTokenPatch {
        dot_size: Some(10.),
        ..Default::default()
    });
    let scope = f.scope.clone();
    f.cx.update(|cx| {
        scope
            .update_patch(cx, |patch| patch.theme = Some(theme))
            .unwrap()
    });
    f.draw();
    let diameter = |f: &mut Fixture| {
        f.cx.update_window(f.window.into(), |_, window, _| {
            window.rendered_scene().paths[0].bounds.size.width.0 / window.scale_factor()
        })
        .unwrap()
    };
    assert!((diameter(&mut f) - 6.).abs() < 0.05);
    f.frame(150);
    let curve = crate::theme::ThemeConfig::default()
        .resolve()
        .tokens
        .motion_ease_in_out_circ;
    let expected = 6. + 4. * crate::behavior::evaluate_cubic_bezier(curve, 0.5) as f32;
    assert!((diameter(&mut f) - expected).abs() < 0.05);
    f.frame(150);
    assert!((diameter(&mut f) - 10.).abs() < 0.05);
    let mut reduced_theme = crate::theme::ThemeConfig::default();
    reduced_theme.components.set::<RadioTheme>(RadioTokenPatch {
        dot_size: Some(6.),
        ..Default::default()
    });
    f.cx.update(|cx| {
        scope
            .update_patch(cx, |patch| {
                patch.motion = Some(false);
                patch.theme = Some(reduced_theme);
            })
            .unwrap()
    });
    f.draw();
    assert!((diameter(&mut f) - 6.).abs() < 0.05);
    assert_eq!(f.frame(1000), 0);
}

#[test]
fn radio_disabled_label_switches_immediately_while_the_ring_animates() {
    let theme = crate::theme::ResolvedTheme::default();
    let enabled = RadioStyle::resolve(&theme, true, false).rest;
    let disabled = RadioStyle::resolve(&theme, true, true).rest;
    let mut visual = VisualTransition::default();
    let motion = MotionPolicy::new(true, false);
    let duration = theme.tokens.motion_duration_mid;
    visual.resolve(
        AnimatedStyle::new(enabled, false),
        Duration::ZERO,
        duration,
        CSS_EASE,
        motion,
    );
    let (first, animating) = visual.resolve(
        AnimatedStyle::new(disabled, false),
        Duration::ZERO,
        duration,
        CSS_EASE,
        motion,
    );
    assert!(animating);
    assert_eq!(first.apply(disabled).label, disabled.label);
    assert_eq!(first.apply(disabled).border, enabled.border);
    let (middle, _) = visual.resolve(
        AnimatedStyle::new(disabled, false),
        Duration::from_millis(100),
        duration,
        CSS_EASE,
        motion,
    );
    assert_eq!(middle.apply(disabled).label, disabled.label);
    assert_ne!(middle.apply(disabled).border, disabled.border);
}

#[test]
fn radio_button_border_switches_immediately_on_disabled_state() {
    let mut f = Fixture::with_motion(true, |c| {
        c.button = true;
        c.default_checked = true;
    });
    let theme = crate::theme::ResolvedTheme::default();
    let enabled = RadioButtonVisualStyle::resolve(
        &theme,
        Size::Medium,
        RadioButtonStyle::Outline,
        true,
        false,
    )
    .rest;
    let disabled = RadioButtonVisualStyle::resolve(
        &theme,
        Size::Medium,
        RadioButtonStyle::Outline,
        true,
        true,
    )
    .rest;
    let mut visual = VisualTransition::default();
    let motion = MotionPolicy::new(true, false);
    visual.resolve(
        AnimatedStyle::new(enabled, true),
        Duration::ZERO,
        theme.tokens.motion_duration_mid,
        CSS_EASE,
        motion,
    );
    let (first, animating) = visual.resolve(
        AnimatedStyle::new(disabled, true),
        Duration::ZERO,
        theme.tokens.motion_duration_mid,
        CSS_EASE,
        motion,
    );
    assert!(animating);
    assert_eq!(first.apply(disabled).border, disabled.border);
    assert_eq!(first.apply(disabled).label, enabled.label);
    f.edit(|c| c.disabled = Some(true));
    f.cx.update_window(f.window.into(), |_, window, _| {
        let scale = window.scale_factor();
        let button = window
            .rendered_scene()
            .quads
            .iter()
            .find(|quad| {
                (quad.bounds.size.height.0 / scale - 32.).abs() < 0.01
                    && quad.border_widths.top.0 > 0.
            })
            .expect("standalone button border");
        assert_eq!(button.border_color, disabled.border);
    })
    .unwrap();
}

#[test]
fn radio_wave_uses_visible_background_when_checked_hover_border_is_transparent() {
    let primary = crate::theme::Color::rgb(0x1677ff).to_hsla();
    let hover = crate::theme::Color::rgb(0x4096ff).to_hsla();
    let mut style = StyleRefinement::default()
        .bg(hover)
        .border_color(crate::theme::Color::TRANSPARENT);
    assert_eq!(wave_color(&style, primary), hover);
    style.border_color = Some(primary);
    assert_eq!(wave_color(&style, hover), primary);
    assert_eq!(wave_color(&StyleRefinement::default(), primary), primary);
}

#[test]
fn radio_wave_duration_matches_the_reference_target() {
    for button in [false, true] {
        let mut f = Fixture::with_motion(true, |c| {
            c.button = button;
            c.default_checked = true;
        });
        f.click_at(20.);
        f.frame(350);
        let pending = f.frame(16);
        assert_eq!(
            pending > 0,
            button,
            "only button radios have a two-second wave"
        );
        f.frame(1700);
        assert_eq!(f.frame(16), 0, "completed waves stop requesting frames");
    }
}

#[test]
fn radio_wave_does_not_depend_on_a_controlled_owner_accepting_selection() {
    for button in [false, true] {
        let mut f = Fixture::with_motion(true, |c| {
            c.button = button;
            c.checked = Some(false);
        });
        f.key("tab");
        f.key("space");
        assert_eq!(f.changes().len(), 1);
        assert!(!f.checked());
        f.frame(30);
        f.cx.update_window(f.window.into(), |_, window, _| {
            assert!(
                window.rendered_scene().quads.iter().any(|quad| {
                    quad.border_color.a > 0.
                        && quad.border_color.a < 0.2
                        && quad.background.as_solid().is_none_or(|color| color.a == 0.)
                }),
                "an enabled radio paints its wave even when selection is refused"
            );
        })
        .unwrap();
    }
}

fn painted_waves(f: &mut Fixture) -> Vec<(f32, Hsla)> {
    f.cx.update_window(f.window.into(), |_, window, _| {
        let scale = window.scale_factor();
        let mut waves: Vec<_> = window
            .rendered_scene()
            .quads
            .iter()
            .filter(|quad| {
                quad.border_color.a > 0.
                    && quad.border_color.a < 0.2
                    && quad.background.as_solid().is_none_or(|color| color.a == 0.)
            })
            .map(|quad| (quad.border_widths.top.0 / scale, quad.border_color))
            .collect();
        // GPUI splits a border-only quad into strips with separate clip masks.
        waves.dedup();
        waves
    })
    .unwrap()
}

#[test]
fn radio_wave_paints_reference_spread_opacity_and_captures_color_per_click() {
    let purple = crate::theme::Color::rgb(0x722ed1).to_hsla();
    let orange = crate::theme::Color::rgb(0xfa8c16).to_hsla();
    for button in [false, true] {
        let refinement = |color| {
            StyleRefinement::default()
                .border_color(crate::theme::Color::WHITE)
                .bg(color)
        };
        let mut f = Fixture::with_motion(true, |c| {
            c.button = button;
            c.default_checked = true;
            if button {
                c.style = refinement(purple);
            } else {
                c.icon_style = Some(refinement(purple));
            }
        });
        f.click_at(20.);
        f.frame(if button { 400 } else { 150 });
        let waves = painted_waves(&mut f);
        assert_eq!(waves.len(), 1);
        let (spread, opacity) = if button {
            (6., 0.042_833_667)
        } else {
            (3.101_25, 0.096_625)
        };
        let scale =
            f.cx.update_window(f.window.into(), |_, window, _| window.scale_factor())
                .unwrap();
        let snapped_spread = (spread * scale).round().max(1.) / scale;
        assert!((waves[0].0 - snapped_spread).abs() < 0.001);
        assert!((waves[0].1.a - opacity).abs() < 0.000_01);
        assert_eq!(
            waves[0].1.h, purple.h,
            "white border falls through to background"
        );
        f.edit(|c| {
            if button {
                c.style = refinement(orange);
            } else {
                c.icon_style = Some(refinement(orange));
            }
        });
        assert_eq!(painted_waves(&mut f)[0].1.h, purple.h);
        f.click_at(20.);
        f.frame(50);
        let waves = painted_waves(&mut f);
        assert_eq!(
            waves.len(),
            2,
            "a repeated click does not remove the first wave"
        );
        assert!(waves.iter().any(|(_, color)| color.h == purple.h));
        assert!(waves.iter().any(|(_, color)| color.h == orange.h));
        f.cx.update(|cx| cx.set_reduce_motion(true));
        f.draw();
        assert!(painted_waves(&mut f).is_empty());
        f.cx.update(|cx| cx.set_reduce_motion(false));
        f.draw();
        assert!(
            painted_waves(&mut f).is_empty(),
            "cancelled waves stay cancelled"
        );
    }
}
