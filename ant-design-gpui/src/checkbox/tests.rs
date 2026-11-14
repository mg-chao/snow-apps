// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::{cell::RefCell, rc::Rc, time::Duration};

use gpui::{
    AppContext, Context, Element, Entity, FocusHandle, IntoElement, KeyDownEvent, KeyUpEvent,
    KeyboardButton, Keystroke, MouseButton, MouseDownEvent, MouseUpEvent, PlatformInput, Render,
    RenderOnce, Role, StyleRefinement, TestAppContext, Window, WindowHandle, accesskit, div, point,
    px,
};

use super::*;
use crate::{ConfigPatch, UiRoot};

struct Consumer {
    scope: ConfigScope,
    focus: FocusHandle,
    second_focus: FocusHandle,
    changes: Rc<RefCell<Vec<CheckboxChange>>>,
    semantics: Rc<RefCell<accesskit::Node>>,
    checked: Option<bool>,
    default_checked: bool,
    indeterminate: bool,
    disabled: Option<bool>,
    visible: bool,
    label: &'static str,
    external_focus: bool,
    style: StyleRefinement,
    icon_style: Option<StyleRefinement>,
}

impl Consumer {
    fn new(scope: ConfigScope, cx: &mut Context<Self>) -> Self {
        Self {
            scope,
            focus: focus_handle(cx),
            second_focus: focus_handle(cx),
            changes: Rc::default(),
            semantics: Rc::new(RefCell::new(accesskit::Node::new(Role::CheckBox))),
            checked: None,
            default_checked: false,
            indeterminate: false,
            disabled: None,
            visible: true,
            label: "Accept terms",
            external_focus: true,
            style: StyleRefinement::default(),
            icon_style: None,
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
            let mut checkbox = Checkbox::new("checkbox-test-first")
                .scope(self.scope.clone())
                .label(self.label)
                .default_checked(self.default_checked)
                .indeterminate(self.indeterminate)
                .automation_id("checkbox-test.first")
                .accessibility_description("Agree to the terms")
                .on_change(move |change, _, _| changes.borrow_mut().push(*change));
            if let Some(checked) = self.checked {
                checkbox = checkbox.checked(checked);
            }
            if let Some(disabled) = self.disabled {
                checkbox = checkbox.disabled(disabled);
            }
            if self.external_focus {
                checkbox = checkbox.focus_handle(&self.focus);
            }
            checkbox.style().refine(&self.style);
            if let Some(refinement) = &self.icon_style {
                checkbox = checkbox.icon_style(refinement.clone());
            }
            let element = checkbox.render(window, cx).into_element();
            let mut semantics = accesskit::Node::new(element.a11y_role().unwrap());
            element.write_a11y_info(&mut semantics);
            *self.semantics.borrow_mut() = semantics;
            row = row.child(element);
        }
        row.child(
            Checkbox::new("checkbox-test-second")
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
            let checkbox_scope = root_scope
                .child(
                    ConfigPatch {
                        motion: Some(motion),
                        ..Default::default()
                    },
                    cx,
                )
                .unwrap();
            let content = cx.new(|cx| {
                let mut consumer = Consumer::new(checkbox_scope.clone(), cx);
                configure(&mut consumer);
                consumer
            });
            let panel = cx.new(|_| CachedPanel(content.clone()));
            consumer = Some(content);
            scope = Some(checkbox_scope);
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
    fn changes(&self) -> Vec<CheckboxChange> {
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
                    .find(|quad| {
                        (quad.bounds.size.width.0 / scale - 16.).abs() < 0.01
                            && quad.border_color.a > 0.
                    })
                    .expect("checkbox box quad");
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
fn checkbox_pointer_label_and_space_toggle_once_while_enter_and_repeats_do_not() {
    let mut f = Fixture::new(|_| {});
    f.key("tab");
    assert!(f.focused(false));
    f.key_event("space", true, false);
    f.key_event("space", true, true);
    f.key_event("space", false, false);
    f.draw();
    assert!(f.checked());
    f.key("enter");
    f.key("ctrl-space");
    assert_eq!(f.changes().len(), 1);
    f.click_at(20.);
    assert!(!f.checked());
    f.click_at(70.);
    assert!(f.checked());
    assert_eq!(
        f.changes().iter().map(|c| c.activation).collect::<Vec<_>>(),
        [
            Activation::Keyboard(KeyboardButton::Space),
            Activation::Pointer,
            Activation::Pointer,
        ]
    );
}

#[test]
fn checkbox_controlled_changes_are_proposals_and_default_is_only_read_on_mount() {
    let mut f = Fixture::new(|c| {
        c.checked = Some(false);
        c.default_checked = true;
    });
    assert!(!f.checked());
    f.click_at(20.);
    f.click_at(20.);
    assert!(!f.checked());
    assert!(f.changes().iter().all(|c| c.checked));
    f.edit(|c| c.checked = Some(true));
    assert!(f.checked());
    f.click_at(20.);
    assert!(!f.changes().last().unwrap().checked);
    f.edit(|c| c.checked = None);
    f.click_at(20.);
    assert!(!f.checked());
    f.edit(|c| c.default_checked = true);
    assert!(!f.checked());
    f.edit(|c| c.visible = false);
    f.edit(|c| c.visible = true);
    assert!(f.checked());
}

#[test]
fn checkbox_indeterminate_semantics_do_not_replace_the_underlying_boolean() {
    let mut f = Fixture::new(|c| c.indeterminate = true);
    assert_eq!(f.node().toggled(), Some(Toggled::Mixed));
    f.click_at(20.);
    assert_eq!(f.node().toggled(), Some(Toggled::Mixed));
    assert!(f.changes()[0].checked);
    f.edit(|c| c.indeterminate = false);
    assert!(f.checked());
    f.edit(|c| c.indeterminate = true);
    f.click_at(20.);
    assert!(!f.changes()[1].checked);
}

#[test]
fn checkbox_disabled_skips_focus_and_explicit_false_overrides_provider() {
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
    assert!(!f.node().is_disabled());
    f.key("shift-tab");
    assert!(f.focused(false));
    f.key("space");
    assert!(f.checked());
}

#[test]
fn checkbox_cancelled_pointer_and_keyboard_presses_never_toggle() {
    let mut f = Fixture::new(|_| {});
    f.pointer(true, 20.);
    f.pointer(false, 310.);
    f.key("tab");
    f.key_event("space", true, false);
    f.key("tab");
    f.key_event("space", false, false);
    f.draw();
    assert!(f.changes().is_empty());
}

#[test]
fn checkbox_inherited_disable_roundtrip_cancels_without_drawing() {
    let mut f = Fixture::new(|_| {});
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
fn checkbox_internal_focus_and_name_survive_rerenders() {
    let mut f = Fixture::new(|c| c.external_focus = false);
    f.key("tab");
    let focus =
        f.cx.update_window(f.window.into(), |_, window, cx| window.focused(cx).unwrap())
            .unwrap();
    f.edit(|c| c.label = "Allow notifications");
    f.cx.update_window(f.window.into(), |_, window, _| {
        assert!(focus.is_focused(window))
    })
    .unwrap();
    assert_eq!(f.node().role(), Role::CheckBox);
    assert_eq!(f.node().label(), Some("Allow notifications"));
    assert_eq!(f.node().description(), Some("Agree to the terms"));
    assert_eq!(f.node().author_id(), Some("checkbox-test.first"));
    f.edit(|c| c.visible = false);
    f.key("tab");
    assert!(f.focused(true));
}

#[test]
fn checkbox_idle_and_reduced_motion_never_schedule_continuous_frames() {
    let mut f = Fixture::new(|_| {});
    assert_eq!(f.frame(16), 0);
    f.click_at(20.);
    assert!(f.checked());
    assert_eq!(f.frame(16), 0);
    let mut animated = Fixture::with_motion(true, |_| {});
    assert_eq!(animated.frame(16), 0);
    animated.click_at(20.);
    assert_eq!(animated.frame(16), 1);
    animated.frame(400);
    assert_eq!(animated.frame(16), 0);
    animated.cx.update(|cx| cx.set_reduce_motion(true));
    animated.click_at(20.);
    assert_eq!(animated.frame(16), 0);
}

#[test]
fn checkbox_mounted_checked_state_is_static_and_does_not_animate_in() {
    let mut f = Fixture::with_motion(true, |c| c.default_checked = true);
    assert!(f.checked());
    assert_eq!(f.frame(16), 0);
}

#[test]
fn checkbox_icon_only_keeps_reference_line_box_and_multiline_labels_center_the_box() {
    let mut f = Fixture::new(|c| c.label = "");
    let bounds = f.box_bounds();
    assert_eq!(bounds.origin, point(px(12.), px(15.)));
    assert_eq!(bounds.size, gpui::size(px(16.), px(16.)));
    f.edit(|c| {
        c.label = "Accept terms";
        c.style = StyleRefinement::default().w(px(80.));
    });
    let bounds = f.box_bounds();
    assert!(
        bounds.top() > px(15.),
        "a wrapped label centers the icon across all lines"
    );
}

#[test]
fn checkbox_keyboard_focus_outline_surrounds_only_the_box() {
    let mut f = Fixture::new(|_| {});
    f.key("tab");
    f.cx.update_window(f.window.into(), |_, window, _| {
        let scale = window.scale_factor();
        let outline = window
            .rendered_scene()
            .quads
            .iter()
            .find(|quad| {
                (quad.bounds.size.width.0 / scale - 24.).abs() < 0.01
                    && (quad.border_widths.left.0 / scale - 3.).abs() < 0.01
            })
            .expect("keyboard focus outline around 16px icon plus 1px gap and 3px border");
        assert!((outline.bounds.origin.x.0 / scale - 8.).abs() < 0.01);
        assert!((outline.bounds.origin.y.0 / scale - 11.).abs() < 0.01);
    })
    .unwrap();
    f.click_at(70.);
    f.cx.update_window(f.window.into(), |_, window, _| {
        let scale = window.scale_factor();
        assert!(!window.rendered_scene().quads.iter().any(|quad| {
            (quad.bounds.size.width.0 / scale - 24.).abs() < 0.01
                && (quad.border_widths.left.0 / scale - 3.).abs() < 0.01
        }));
    })
    .unwrap();
}

#[test]
fn checkbox_rtl_reverses_label_order_without_changing_box_geometry() {
    let mut f = Fixture::new(|_| {});
    let ltr = f.box_bounds();
    let scope = f.scope.clone();
    f.cx.update(|cx| {
        scope
            .update_patch(cx, |p| p.direction = Some(Direction::Rtl))
            .unwrap()
    });
    f.draw();
    let rtl = f.box_bounds();
    assert!(rtl.left() > ltr.left());
    assert_eq!(rtl.size, ltr.size);
    assert_eq!(rtl.top(), ltr.top());
}

#[test]
fn checkbox_focus_outline_respects_root_and_icon_opacity_once_each() {
    let mut f = Fixture::new(|c| {
        c.style = StyleRefinement::default().opacity(0.5);
        c.icon_style = Some(StyleRefinement::default().opacity(0.5));
    });
    f.key("tab");
    f.cx.update_window(f.window.into(), |_, window, _| {
        let scale = window.scale_factor();
        let outline = window
            .rendered_scene()
            .quads
            .iter()
            .find(|quad| (quad.border_widths.left.0 / scale - 3.).abs() < 0.01)
            .expect("checkbox focus outline");
        assert!((outline.border_color.a - 0.25).abs() < 1e-6);
    })
    .unwrap();
}

#[test]
fn checkbox_visual_retargeting_preserves_continuity_and_reduced_motion_snaps() {
    let theme = crate::theme::ResolvedTheme::default();
    let style = CheckboxStyle::resolve(&theme, true, false, false);
    let motion = MotionPolicy::new(true, false);
    let curve = theme.tokens.motion_ease_in_out;
    let mut visual = VisualTransition::default();
    visual.resolve(
        style.rest,
        Duration::ZERO,
        Duration::from_millis(200),
        curve,
        motion,
    );
    visual.resolve(
        style.hover,
        Duration::ZERO,
        Duration::from_millis(200),
        curve,
        motion,
    );
    let halfway = visual.sample(Duration::from_millis(100));
    let (retargeted, pending) = visual.resolve(
        style.rest,
        Duration::from_millis(100),
        Duration::from_millis(200),
        curve,
        motion,
    );
    assert_eq!(retargeted, halfway);
    assert!(pending);
    let (snapped, pending) = visual.resolve(
        style.rest,
        Duration::from_millis(110),
        Duration::from_millis(200),
        curve,
        MotionPolicy::new(true, true),
    );
    assert_eq!(snapped, style.rest);
    assert!(!pending);
}

#[test]
fn checkbox_hover_border_fades_without_darkening_its_color() {
    let style = CheckboxStyle::resolve(&crate::theme::ResolvedTheme::default(), true, false, false);
    let halfway = interpolate_style(style.rest, style.hover, 0.5)
        .border
        .to_rgb();
    let primary = style.rest.border.to_rgb();
    assert!((halfway.a - 0.5).abs() < 1e-6);
    assert!((halfway.r - primary.r).abs() < 1e-6);
    assert!((halfway.g - primary.g).abs() < 1e-6);
    assert!((halfway.b - primary.b).abs() < 1e-6);
}

#[test]
fn checkbox_tick_delays_appearance_and_fades_independently_when_hiding() {
    let global = &crate::theme::ResolvedTheme::default().tokens;
    let motion = MotionPolicy::new(true, false);
    let mut mark = MarkTransition::new(0., global);
    assert_eq!(
        mark.resolve(1., Duration::ZERO, global, motion),
        (0., 0., true)
    );
    assert_eq!(
        mark.resolve(1., global.motion_duration_fast / 2, global, motion),
        (0., 0., true)
    );
    let shown_at = global.motion_duration_fast + global.motion_duration_mid;
    assert_eq!(mark.resolve(1., shown_at, global, motion), (1., 1., false));
    mark.resolve(0., shown_at, global, motion);
    let (scale, opacity, pending) = mark.resolve(
        0.,
        shown_at + global.motion_duration_fast / 4,
        global,
        motion,
    );
    assert!(scale > 1., "back easing briefly overshoots while hiding");
    assert!(
        opacity < 1. && opacity > 0.,
        "opacity starts fading immediately"
    );
    assert!(pending);
    assert_eq!(
        mark.resolve(0., shown_at + global.motion_duration_fast, global, motion),
        (0., 0., false)
    );
}

#[test]
fn checkbox_controlled_owner_refusal_does_not_start_a_wave_for_an_unchecked_control() {
    let mut f = Fixture::with_motion(true, |c| c.checked = Some(false));
    f.key("tab");
    f.key("space");
    assert_eq!(f.changes().len(), 1);
    assert!(!f.checked());
    assert_eq!(f.frame(16), 0);
}

#[test]
fn checkbox_disabled_label_changes_immediately_while_the_box_animates() {
    let theme = crate::theme::ResolvedTheme::default();
    let enabled = CheckboxStyle::resolve(&theme, true, false, false).rest;
    let disabled = CheckboxStyle::resolve(&theme, true, false, true).rest;
    let mut visual = VisualTransition::default();
    let motion = MotionPolicy::new(true, false);
    let duration = theme.tokens.motion_duration_slow;
    visual.resolve(enabled, Duration::ZERO, duration, CSS_EASE, motion);
    let (first, animating) = visual.resolve(disabled, Duration::ZERO, duration, CSS_EASE, motion);
    assert!(animating);
    assert_eq!(
        first.label, disabled.label,
        "the wrapper has no CSS transition"
    );
    assert_eq!(first.border, enabled.border);
    let (middle, _) = visual.resolve(
        disabled,
        Duration::from_millis(100),
        duration,
        CSS_EASE,
        motion,
    );
    assert_eq!(middle.label, disabled.label);
    assert_ne!(middle.border, disabled.border);
    let label_only = CheckboxStateStyle {
        label: enabled.label,
        ..disabled
    };
    let (settled, animating) = visual.resolve(
        label_only,
        Duration::from_secs(1),
        duration,
        CSS_EASE,
        motion,
    );
    assert_eq!(settled, label_only);
    assert!(
        !animating,
        "a label-only change does not schedule box animation frames"
    );
}

#[test]
fn checkbox_unchecking_preserves_the_existing_wave_without_creating_another() {
    let mut f = Fixture::with_motion(true, |_| {});
    let waves = |f: &mut Fixture| {
        f.cx.update_window(f.window.into(), |_, window, _| {
            let mut waves: Vec<_> = window
                .rendered_scene()
                .quads
                .iter()
                .filter(|quad| {
                    quad.border_color.a > 0.
                        && quad.border_color.a < 0.2
                        && quad.background.as_solid().is_none_or(|color| color.a == 0.)
                })
                .map(|quad| (quad.border_widths.top.0, quad.border_color))
                .collect();
            // Count logical outlines, not GPUI's clipped border strips.
            waves.dedup();
            waves.len()
        })
        .unwrap()
    };
    f.click_at(20.);
    f.frame(100);
    assert_eq!(waves(&mut f), 1);
    f.click_at(20.);
    assert!(!f.checked());
    f.frame(50);
    assert_eq!(waves(&mut f), 1);
    f.frame(150);
    assert_eq!(waves(&mut f), 0);
}
