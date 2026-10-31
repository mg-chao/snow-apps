// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::{cell::RefCell, rc::Rc, sync::Arc, time::Duration};

use gpui::{
    AppContext, Context, Element, Entity, FocusHandle, IntoElement, KeyDownEvent, KeyUpEvent,
    KeyboardButton, Keystroke, MouseButton, MouseDownEvent, MouseUpEvent, PlatformInput, Render,
    RenderOnce, Role, StyleRefinement, TestAppContext, Window, WindowHandle, accesskit, div, point,
    px,
};

use super::*;
use crate::{
    ConfigPatch, Direction, UiRoot,
    theme::{AlgorithmChain, Color, ComponentThemeConfig, ThemeConfig, TokenPatch},
};

struct Consumer {
    scope: ConfigScope,
    focus: FocusHandle,
    second_focus: FocusHandle,
    changes: Rc<RefCell<Vec<SwitchChange>>>,
    clicks: Rc<RefCell<Vec<SwitchChange>>>,
    semantics: Rc<RefCell<accesskit::Node>>,
    checked: Option<bool>,
    default_checked: bool,
    disabled: Option<bool>,
    loading: bool,
    visible: bool,
    external_focus: bool,
    size: Option<Size>,
    checked_label: &'static str,
    unchecked_label: &'static str,
    custom_content: bool,
    style: StyleRefinement,
    indicator_style: Option<StyleRefinement>,
    content_style: Option<StyleRefinement>,
}

impl Consumer {
    fn new(scope: ConfigScope, cx: &mut Context<Self>) -> Self {
        Self {
            scope,
            focus: focus_handle(cx),
            second_focus: focus_handle(cx),
            changes: Rc::default(),
            clicks: Rc::default(),
            semantics: Rc::new(RefCell::new(accesskit::Node::new(Role::Switch))),
            checked: None,
            default_checked: false,
            disabled: None,
            loading: false,
            visible: true,
            external_focus: true,
            size: None,
            checked_label: "",
            unchecked_label: "",
            custom_content: false,
            style: StyleRefinement::default(),
            indicator_style: None,
            content_style: None,
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
            .w(px(400.));
        if self.visible {
            let changes = self.changes.clone();
            let clicks = self.clicks.clone();
            let mut control = Switch::new("switch-test-first")
                .scope(self.scope.clone())
                .default_checked(self.default_checked)
                .loading(self.loading)
                .checked_label(self.checked_label)
                .unchecked_label(self.unchecked_label)
                .accessibility_label("Notifications")
                .accessibility_description("Receive updates")
                .automation_id("switch-test.first")
                .on_change(move |change, _, _| changes.borrow_mut().push(*change))
                .on_click(move |change, _, _| clicks.borrow_mut().push(*change));
            if let Some(value) = self.checked {
                control = control.checked(value);
            }
            if let Some(value) = self.disabled {
                control = control.disabled(value);
            }
            if let Some(value) = self.size {
                control = control.size(value);
            }
            if self.external_focus {
                control = control.focus_handle(&self.focus);
            }
            control.style().refine(&self.style);
            if let Some(style) = &self.indicator_style {
                control = control.indicator_style(style.clone());
            }
            if let Some(style) = &self.content_style {
                control = control.content_style(style.clone());
            }
            if self.custom_content {
                control = control
                    .checked_content(div().w(px(30.)).h(px(12.)).bg(Color::rgb(0xff0000)))
                    .unchecked_content(div().w(px(12.)).h(px(12.)).bg(Color::rgb(0x00ff00)));
            }
            let element = control.render(window, cx).into_element();
            let mut node = accesskit::Node::new(element.a11y_role().unwrap());
            element.write_a11y_info(&mut node);
            *self.semantics.borrow_mut() = node;
            row = row.child(
                div()
                    .id("first-wrapper")
                    .debug_selector(|| "switch-test.first".into())
                    .child(element),
            );
        }
        row.child(
            Switch::new("switch-test-second")
                .scope(self.scope.clone())
                .accessibility_label("Next")
                .disabled(false)
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
                .cached(StyleRefinement::default().w(px(400.)).h(px(180.))),
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
            let local_scope = root_scope
                .child(
                    ConfigPatch {
                        motion: Some(motion),
                        ..Default::default()
                    },
                    cx,
                )
                .unwrap();
            let content = cx.new(|cx| {
                let mut c = Consumer::new(local_scope.clone(), cx);
                configure(&mut c);
                c
            });
            let panel = cx.new(|_| CachedPanel(content.clone()));
            consumer = Some(content);
            scope = Some(local_scope);
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
                window.draw(cx).clear(cx)
            })
            .unwrap();
        self.cx.run_until_parked();
    }
    fn edit(&mut self, edit: impl FnOnce(&mut Consumer)) {
        self.consumer.update(&mut self.cx, |c, cx| {
            edit(c);
            cx.notify();
        });
        self.draw();
    }
    fn patch(&mut self, patch: impl FnOnce(&mut ConfigPatch)) {
        let scope = self.scope.clone();
        self.cx.update(|cx| scope.update_patch(cx, patch).unwrap());
        self.draw();
    }
    fn key_event(&mut self, key: &'static str, down: bool, held: bool) {
        let keystroke = Keystroke::parse(key).unwrap();
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
    fn key(&mut self, key: &'static str) {
        self.key_event(key, true, false);
        self.key_event(key, false, false);
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
    fn click(&mut self) {
        self.pointer(true, 20.);
        self.pointer(false, 20.);
        self.draw();
    }
    fn node(&self) -> accesskit::Node {
        self.consumer
            .read_with(&self.cx, |c, _| c.semantics.borrow().clone())
    }
    fn checked(&self) -> bool {
        self.node().toggled() == Some(Toggled::True)
    }
    fn changes(&self) -> Vec<SwitchChange> {
        self.consumer
            .read_with(&self.cx, |c, _| c.changes.borrow().clone())
    }
    fn clicks(&self) -> Vec<SwitchChange> {
        self.consumer
            .read_with(&self.cx, |c, _| c.clicks.borrow().clone())
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
    fn quads(&mut self) -> Vec<(gpui::Bounds<gpui::Pixels>, gpui::Hsla)> {
        self.cx
            .update_window(self.window.into(), |_, window, _| {
                let scale = window.scale_factor();
                window
                    .rendered_scene()
                    .quads
                    .iter()
                    .map(|q| {
                        (
                            gpui::Bounds::new(
                                point(
                                    px(q.bounds.origin.x.0 / scale),
                                    px(q.bounds.origin.y.0 / scale),
                                ),
                                size(
                                    px(q.bounds.size.width.0 / scale),
                                    px(q.bounds.size.height.0 / scale),
                                ),
                            ),
                            q.background.as_solid().unwrap_or_default(),
                        )
                    })
                    .collect()
            })
            .unwrap()
    }
    fn track(&mut self) -> gpui::Bounds<gpui::Pixels> {
        self.quads()
            .into_iter()
            .find(|(b, _)| b.left() == px(12.) && b.top() == px(12.) && b.size.height >= px(14.))
            .expect("track quad")
            .0
    }
    fn handle(&mut self) -> gpui::Bounds<gpui::Pixels> {
        self.quads()
            .into_iter()
            .find(|(b, c)| *c == Color::WHITE.to_hsla() && b.left() < px(120.))
            .expect("handle quad")
            .0
    }
}

#[test]
fn switch_pointer_space_enter_and_accessibility_metadata() {
    let mut f = Fixture::new(|_| {});
    assert_eq!(f.node().role(), Role::Switch);
    assert_eq!(f.node().label(), Some("Notifications"));
    assert_eq!(f.node().description(), Some("Receive updates"));
    assert_eq!(f.node().author_id(), Some("switch-test.first"));
    assert_eq!(f.node().toggled(), Some(Toggled::False));
    f.key("tab");
    assert!(f.focused(false));
    f.key_event("space", true, false);
    f.key_event("space", true, true);
    f.key_event("space", false, false);
    f.draw();
    assert!(f.checked());
    f.key("enter");
    assert!(!f.checked());
    f.key("ctrl-space");
    assert_eq!(f.changes().len(), 2);
    f.click();
    assert!(f.checked());
    assert_eq!(
        f.changes().iter().map(|e| e.activation).collect::<Vec<_>>(),
        [
            Activation::Keyboard(KeyboardButton::Space),
            Activation::Keyboard(KeyboardButton::Enter),
            Activation::Pointer
        ]
    );
    assert_eq!(f.changes(), f.clicks());
}

#[test]
fn switch_arrows_set_values_without_click_callbacks_and_keep_physical_rtl_mapping() {
    let mut f = Fixture::new(|_| {});
    f.key("tab");
    f.key("left");
    assert!(f.changes().is_empty());
    f.key("right");
    assert!(f.checked());
    f.key("right");
    assert_eq!(f.changes().len(), 1);
    f.key("left");
    assert!(!f.checked());
    f.patch(|p| p.direction = Some(Direction::Rtl));
    f.key("right");
    assert!(f.checked());
    f.key("left");
    assert!(!f.checked());
    assert_eq!(f.changes().len(), 4);
    assert!(f.clicks().is_empty());
    assert!(
        f.changes()
            .iter()
            .all(|e| e.activation == Activation::KeyboardNavigation)
    );
}

#[test]
fn switch_controlled_state_is_a_proposal_and_defaults_are_mount_only() {
    let mut f = Fixture::new(|c| {
        c.checked = Some(true);
        c.default_checked = false;
    });
    f.click();
    assert!(f.checked());
    assert!(!f.changes()[0].checked);
    f.edit(|c| c.checked = Some(false));
    assert!(!f.checked());
    f.edit(|c| {
        c.checked = None;
        c.default_checked = true;
    });
    assert!(!f.checked());
    f.click();
    assert!(f.checked());
    f.edit(|c| c.default_checked = false);
    assert!(f.checked());
    f.edit(|c| c.visible = false);
    f.edit(|c| c.visible = true);
    assert!(!f.checked());
}

#[test]
fn switch_consecutive_clicks_before_render_read_live_uncontrolled_value() {
    let mut f = Fixture::new(|_| {});
    f.pointer(true, 20.);
    f.pointer(false, 20.);
    f.pointer(true, 20.);
    f.pointer(false, 20.);
    f.draw();
    assert!(!f.checked());
    assert_eq!(
        f.changes().iter().map(|e| e.checked).collect::<Vec<_>>(),
        [true, false]
    );
}

#[test]
fn switch_disabled_and_loading_skip_focus_and_local_false_overrides_provider() {
    let mut f = Fixture::new(|c| c.disabled = Some(true));
    f.click();
    f.key("tab");
    assert!(f.focused(true));
    assert!(f.node().is_disabled());
    assert!(!f.node().supports_action(accesskit::Action::Click));
    assert!(f.changes().is_empty());
    f.patch(|p| p.disabled = Some(true));
    f.edit(|c| c.disabled = Some(false));
    f.key("shift-tab");
    assert!(f.focused(false));
    f.edit(|c| c.loading = true);
    assert!(!f.focused(false));
    assert!(f.node().is_disabled());
    assert!(f.node().is_busy());
    f.click();
    assert!(f.changes().is_empty());
    f.key("tab");
    assert!(f.focused(true));
    f.edit(|c| c.loading = false);
    f.key("shift-tab");
    f.key("space");
    assert!(f.checked());
}

#[test]
fn switch_cancelled_pointer_keyboard_and_inherited_availability_roundtrip_do_not_toggle() {
    let mut f = Fixture::new(|_| {});
    f.pointer(true, 20.);
    f.pointer(false, 390.);
    f.key("tab");
    f.key_event("space", true, false);
    f.key("tab");
    f.key_event("space", false, false);
    f.draw();
    assert!(f.changes().is_empty());
    f.key("shift-tab");
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
fn switch_internal_focus_survives_rerender_and_unmount_recovers_traversal() {
    let mut f = Fixture::new(|c| c.external_focus = false);
    f.key("tab");
    let focus =
        f.cx.update_window(f.window.into(), |_, window, cx| window.focused(cx).unwrap())
            .unwrap();
    f.edit(|c| c.checked_label = "On");
    f.cx.update_window(f.window.into(), |_, window, _| {
        assert!(focus.is_focused(window))
    })
    .unwrap();
    f.key("space");
    assert!(f.checked());
    f.edit(|c| c.visible = false);
    f.key("tab");
    assert!(f.focused(true));
}

#[test]
fn switch_native_dimensions_handle_travel_and_rtl_match_reference() {
    let mut f = Fixture::new(|_| {});
    assert_eq!(f.track().size, size(px(44.), px(22.)));
    assert_eq!(
        f.handle(),
        gpui::Bounds::new(point(px(14.), px(14.)), size(px(18.), px(18.)))
    );
    f.click();
    assert_eq!(f.handle().left(), px(36.));
    f.patch(|p| p.direction = Some(Direction::Rtl));
    assert_eq!(f.handle().left(), px(14.));
    f.click();
    assert_eq!(f.handle().left(), px(36.));
    f.edit(|c| c.size = Some(Size::Small));
    assert_eq!(f.track().size, size(px(28.), px(16.)));
    assert_eq!(f.handle().size, size(px(12.), px(12.)));
}

#[test]
fn switch_asymmetric_content_measures_both_states_without_width_jumps() {
    let mut f = Fixture::new(|c| {
        c.checked_label = "Connected";
        c.unchecked_label = "Off";
    });
    let width = f.track().size.width;
    assert!(width > px(80.));
    f.click();
    assert_eq!(f.track().size.width, width);
    f.patch(|p| p.direction = Some(Direction::Rtl));
    assert_eq!(f.track().size.width, width);
    f.click();
    assert_eq!(f.track().size.width, width);
    f.edit(|c| c.style = StyleRefinement::default().w(px(180.)));
    assert_eq!(f.track().size.width, px(180.));
}

#[test]
fn switch_press_stretches_inward_and_release_restores_handle() {
    let mut f = Fixture::new(|_| {});
    f.pointer(true, 20.);
    f.draw();
    assert!(
        (f32::from(f.handle().size.width) - 23.4).abs() < 0.26,
        "{:?}",
        f.quads()
    );
    assert_eq!(f.handle().left(), px(14.));
    f.pointer(false, 20.);
    f.draw();
    let resting = f.handle();
    assert_eq!(resting.size.width, px(18.));
    f.pointer(true, 40.);
    f.draw();
    let pressed = f.handle();
    assert!((f32::from(pressed.size.width) - 23.4).abs() < 0.26);
    assert_eq!(pressed.right(), resting.right());
}

#[test]
fn switch_idle_mount_loading_and_reduced_motion_request_only_needed_frames() {
    let mut f = Fixture::with_motion(true, |c| c.default_checked = true);
    assert_eq!(f.frame(16), 0);
    f.click();
    assert_eq!(f.frame(16), 1);
    f.frame(2100);
    assert_eq!(f.frame(16), 0);
    f.edit(|c| c.loading = true);
    assert_eq!(f.frame(16), 1);
    f.cx.update(|cx| cx.set_reduce_motion(true));
    f.draw();
    f.frame(16);
    assert_eq!(f.frame(16), 0);
    f.edit(|c| c.loading = false);
    f.click();
    assert_eq!(f.frame(16), 0);
    let mut static_loading = Fixture::new(|c| c.loading = true);
    assert_eq!(static_loading.frame(16), 0);
}

#[test]
fn switch_interrupted_handle_motion_is_continuous_and_scope_motion_snaps() {
    let mut f = Fixture::with_motion(true, |_| {});
    f.click();
    f.frame(100);
    let middle = f.handle().left();
    assert!(middle > px(14.) && middle < px(36.));
    f.click();
    assert_eq!(f.handle().left(), middle);
    f.frame(50);
    assert!(f.handle().left() < middle);
    f.patch(|p| p.motion = Some(false));
    assert_eq!(f.handle().left(), px(14.));
    f.frame(16);
    assert_eq!(f.frame(16), 0);
}

#[test]
fn switch_token_defaults_component_overrides_and_theme_cache() {
    let theme = ThemeConfig::default().resolve();
    let tokens = theme.component::<SwitchTheme>();
    assert!(Arc::ptr_eq(&tokens, &theme.component::<SwitchTheme>()));
    let normal = SwitchStyle::from_tokens(&tokens, Size::Medium, false, false, false);
    assert_eq!(
        (
            normal.track_height,
            normal.track_min_width,
            normal.handle_size
        ),
        (22., 44., 18.)
    );
    let small = SwitchStyle::resolve(&theme, Size::Small, true, false, true);
    assert_eq!(
        (
            small.track_height,
            small.track_min_width,
            small.handle_size,
            small.loading_icon_size
        ),
        (16., 28., 12., 9.)
    );
    assert_eq!(small.opacity, 0.65);
    assert_eq!(small.background, small.hover_background);
    let mut config = ThemeConfig::default();
    config.components.set_theme(
        ComponentThemeConfig::<SwitchTheme>::new(SwitchTokenPatch {
            track_padding: Some(-3.),
            handle_size: Some(28.),
            ..Default::default()
        })
        .with_algorithms(AlgorithmChain::compact())
        .with_tokens(TokenPatch {
            color_primary: Some(Color::rgb(0x722ed1)),
            ..Default::default()
        }),
    );
    let custom = SwitchStyle::resolve(&config.resolve(), Size::Medium, true, false, false);
    assert_eq!(custom.track_padding, -3.);
    assert_eq!(custom.handle_size, 28.);
    assert_eq!(custom.background, Color::rgb(0x722ed1).to_hsla());
    let compact = ThemeConfig {
        algorithms: Some(AlgorithmChain::compact()),
        ..Default::default()
    }
    .resolve();
    assert_eq!(
        SwitchStyle::resolve(&compact, Size::Medium, false, false, false).track_height,
        20.
    );
}

#[test]
fn switch_component_patch_merges_each_field_and_rejects_nonfinite_lengths() {
    use crate::theme::ComponentTheme;
    let parent = SwitchTokenPatch {
        track_height: Some(30.),
        handle_bg: Some(Color::rgb(0xff0000)),
        track_padding: Some(-3.),
        ..Default::default()
    };
    let local = SwitchTokenPatch {
        handle_size: Some(26.),
        track_height: Some(f64::NAN),
        ..Default::default()
    };
    let patch = SwitchTheme::merge(&parent, &local);
    let mut tokens = SwitchTheme::default_tokens(&ThemeConfig::default().resolve().tokens);
    SwitchTheme::apply(&mut tokens, &patch);
    assert_eq!(tokens.track_height, 22.);
    assert_eq!(tokens.track_padding, -3.);
    assert_eq!(tokens.handle_size, 26.);
    assert_eq!(tokens.handle_bg, Color::rgb(0xff0000));
}

#[test]
fn switch_native_custom_content_uses_both_slots_and_clips_the_inactive_state() {
    let mut f = Fixture::new(|c| c.custom_content = true);
    assert_eq!(f.track().size.width, px(63.));
    let red = Color::rgb(0xff0000).to_hsla();
    let green = Color::rgb(0x00ff00).to_hsla();
    assert!(f.quads().iter().any(|(_, color)| *color == green));
    assert!(!f.quads().iter().any(|(_, color)| *color == red));
    f.click();
    assert_eq!(f.track().size.width, px(63.));
    assert!(f.quads().iter().any(|(_, color)| *color == red));
    assert!(!f.quads().iter().any(|(_, color)| *color == green));
    f.patch(|p| p.direction = Some(Direction::Rtl));
    assert_eq!(f.track().size.width, px(63.));
    f.edit(|c| c.content_style = Some(StyleRefinement::default().pl(px(10.)).pr(px(30.))));
    assert_eq!(f.track().size.width, px(70.));
}

#[test]
fn switch_styled_root_keeps_its_layout_hitbox_and_focus_contour_together() {
    let mut f = Fixture::new(|c| {
        c.style = StyleRefinement::default()
            .w(px(120.))
            .rounded(px(5.))
            .opacity(0.5);
    });
    assert_eq!(f.track().size.width, px(120.));
    f.key("tab");
    f.cx.update_window(f.window.into(), |_, window, _| {
        let scale = window.scale_factor();
        let outline = window
            .rendered_scene()
            .quads
            .iter()
            .find(|q| (q.border_widths.left.0 / scale - 3.).abs() < 0.01)
            .expect("keyboard outline");
        assert!((outline.bounds.size.width.0 / scale - 128.).abs() < 0.01);
        assert!((outline.border_color.a - 0.5).abs() < 1e-6);
        assert!((outline.corner_radii.top_left.0 / scale - 9.).abs() < 0.01);
    })
    .unwrap();
    // The far end of a styled track belongs to the same activation hitbox.
    f.pointer(true, 120.);
    f.pointer(false, 120.);
    f.draw();
    assert!(f.checked());
}
