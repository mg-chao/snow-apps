// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::{cell::RefCell, rc::Rc, time::Duration};

use gpui::{
    AppContext, Bounds, Context, Element, Entity, FocusHandle, IntoElement, KeyDownEvent,
    KeyUpEvent, KeyboardButton, Keystroke, MouseButton, MouseDownEvent, MouseUpEvent, Pixels,
    PlatformInput, Point, Render, StyleRefinement, TestAppContext, Window, WindowHandle, accesskit,
    point, prelude::*, px, size,
};

use super::*;
use crate::{
    Button, ConfigPatch, UiRoot,
    behavior::{Accessibility, focus_handle},
    icons::IconName,
    theme::Color,
};

const ROOT: Color = Color::rgb(0x13579b);
const CLOSE: Color = Color::rgb(0xfedcba);
const TITLE: Color = Color::rgb(0x123456);
const DESCRIPTION: Color = Color::rgb(0x654321);
const ICON: Color = Color::rgb(0xabcdef);
const ACTION: Color = Color::rgb(0xface12);

struct Consumer {
    scope: ConfigScope,
    close_focus: FocusHandle,
    action_focus: FocusHandle,
    next_focus: FocusHandle,
    events: Rc<RefCell<Vec<&'static str>>>,
    activations: Rc<RefCell<Vec<Activation>>>,
    observed: Rc<RefCell<Option<AlertStyleContext>>>,
    visible: bool,
    generation: usize,
    width: f32,
    title: &'static str,
    description: &'static str,
    banner: bool,
    show_icon: Option<bool>,
    custom_icon: bool,
    kind: Option<AlertType>,
    variant: Option<AlertVariant>,
    closable: Option<bool>,
    close_disabled: bool,
    custom_close: bool,
    custom_content: bool,
    action: bool,
    remove_on_close: bool,
    focus_next_on_close: bool,
    style: StyleRefinement,
}

impl Consumer {
    fn new(scope: ConfigScope, cx: &mut Context<Self>) -> Self {
        Self {
            scope,
            close_focus: focus_handle(cx),
            action_focus: focus_handle(cx),
            next_focus: focus_handle(cx),
            events: Rc::default(),
            activations: Rc::default(),
            observed: Rc::default(),
            visible: true,
            generation: 0,
            width: 400.,
            title: "Alert message",
            description: "",
            banner: false,
            show_icon: None,
            custom_icon: false,
            kind: None,
            variant: None,
            closable: Some(true),
            close_disabled: false,
            custom_close: false,
            custom_content: false,
            action: false,
            remove_on_close: false,
            focus_next_on_close: false,
            style: StyleRefinement::default(),
        }
    }
}

impl Render for Consumer {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let mut column = gpui::div()
            .flex()
            .flex_col()
            .items_start()
            .gap(px(20.))
            .p(px(12.))
            .w(px(self.width));
        if self.visible {
            let events = self.events.clone();
            let after = self.events.clone();
            let click = self.events.clone();
            let actions = self.events.clone();
            let activations = self.activations.clone();
            let observed = self.observed.clone();
            let owner = cx.weak_entity();
            let next_focus = self.next_focus.clone();
            let remove = self.remove_on_close;
            let move_focus = self.focus_next_on_close;
            let mut alert = Alert::new(("test-alert", self.generation))
                .scope(self.scope.clone())
                .title(self.title)
                .description(self.description)
                .banner(self.banner)
                .close_disabled(self.close_disabled)
                .close_focus_handle(&self.close_focus)
                .automation_id("alert-test.root")
                .close_automation_id("alert-test.close")
                .styles(AlertStyles {
                    root: StyleRefinement::default().bg(ROOT),
                    icon: StyleRefinement::default().bg(ICON),
                    title: StyleRefinement::default().bg(TITLE),
                    description: StyleRefinement::default().bg(DESCRIPTION),
                    close: StyleRefinement::default().bg(CLOSE),
                    actions: StyleRefinement::default().bg(ACTION),
                    ..Default::default()
                })
                .styles_fn(move |context, _, _| {
                    *observed.borrow_mut() = Some(*context);
                    AlertStyles::default()
                })
                .on_close(move |activation, window, cx| {
                    events.borrow_mut().push("close");
                    activations.borrow_mut().push(*activation);
                    if move_focus {
                        window.focus(&next_focus, cx);
                    }
                    if remove {
                        let _ = owner.update(cx, |s, cx| {
                            s.visible = false;
                            cx.notify();
                        });
                    }
                })
                .after_close(move |_, _| after.borrow_mut().push("after"))
                .on_click(move |_, _, _| click.borrow_mut().push("root-click"));
            if let Some(show) = self.show_icon {
                alert = alert.show_icon(show);
            }
            if self.custom_icon {
                alert = alert.icon(IconName::SmileOutlined);
            }
            if let Some(kind) = self.kind {
                alert = alert.alert_type(kind);
            }
            if let Some(variant) = self.variant {
                alert = alert.variant(variant);
            }
            if let Some(closable) = self.closable {
                alert = alert.closable(closable);
            }
            if self.custom_close {
                alert = alert.close_content("Dismiss");
            }
            if self.custom_content {
                alert = alert
                    .title_content(gpui::div().w(px(90.)).h(px(28.)))
                    .description_content(gpui::div().w(px(140.)).h(px(45.)));
            }
            if self.action {
                alert = alert.action(
                    Button::new("alert-action")
                        .scope(self.scope.clone())
                        .label("Action")
                        .focus_handle(&self.action_focus)
                        .on_click(move |_, _, _| actions.borrow_mut().push("action")),
                );
            }
            alert.style().refine(&self.style);
            column = column.child(alert.render(window, cx));
        }
        column.child(
            Button::new("after-alert")
                .scope(self.scope.clone())
                .label("Next")
                .focus_handle(&self.next_focus),
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
    fn new(edit: impl FnOnce(&mut Consumer)) -> Self {
        Self::motion(false, edit)
    }
    fn motion(motion: bool, edit: impl FnOnce(&mut Consumer)) -> Self {
        let mut cx = TestAppContext::single();
        let mut consumer = None;
        let mut scope = None;
        let window = cx.add_window(|window, cx| {
            let config = ConfigScope::new_window(cx);
            config
                .update_patch(cx, |p| p.motion = Some(motion))
                .unwrap();
            let entity = cx.new(|cx| {
                let mut c = Consumer::new(config.clone(), cx);
                edit(&mut c);
                c
            });
            consumer = Some(entity.clone());
            scope = Some(config.clone());
            UiRoot::new(config, entity, window, cx)
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
        self.consumer.update(&mut self.cx, |s, cx| {
            edit(s);
            cx.notify();
        });
        self.draw();
    }
    fn patch(&mut self, edit: impl FnOnce(&mut ConfigPatch)) {
        self.cx
            .update(|cx| self.scope.update_patch(cx, edit).unwrap());
        self.draw();
    }
    fn bounds(&mut self, color: Color) -> Option<Bounds<Pixels>> {
        self.cx
            .update_window(self.window.into(), |_, w, _| {
                let scale = w.scale_factor();
                w.rendered_scene()
                    .quads
                    .iter()
                    .find(|q| {
                        q.background.as_solid().is_some_and(|c| {
                            let c = c.to_rgb();
                            let expected = color.to_rgba();
                            (c.r - expected.r).abs() < 0.001
                                && (c.g - expected.g).abs() < 0.001
                                && (c.b - expected.b).abs() < 0.001
                        })
                    })
                    .map(|q| {
                        Bounds::new(
                            point(
                                px(q.bounds.origin.x.0 / scale),
                                px(q.bounds.origin.y.0 / scale),
                            ),
                            size(
                                px(q.bounds.size.width.0 / scale),
                                px(q.bounds.size.height.0 / scale),
                            ),
                        )
                    })
            })
            .unwrap()
    }
    fn events(&self) -> Vec<&'static str> {
        self.consumer
            .read_with(&self.cx, |s, _| s.events.borrow().clone())
    }
    fn activations(&self) -> Vec<Activation> {
        self.consumer
            .read_with(&self.cx, |s, _| s.activations.borrow().clone())
    }
    fn context(&self) -> AlertStyleContext {
        self.consumer
            .read_with(&self.cx, |s, _| s.observed.borrow().unwrap())
    }
    fn pointer(&mut self, down: bool, position: Point<Pixels>) {
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
            .update_window(self.window.into(), |_, w, cx| w.dispatch_event(event, cx))
            .unwrap();
    }
    fn click(&mut self, marker: Color) {
        let position = self.bounds(marker).expect("visible slot").center();
        self.pointer(true, position);
        self.pointer(false, position);
        self.draw();
    }
    fn focus(&mut self, close: bool) {
        let focus = self.consumer.read_with(&self.cx, |s, _| {
            if close {
                s.close_focus.clone()
            } else {
                s.next_focus.clone()
            }
        });
        self.cx
            .update_window(self.window.into(), |_, w, cx| w.focus(&focus, cx))
            .unwrap();
        self.draw();
    }
    fn next_focused(&mut self) -> bool {
        let focus = self
            .consumer
            .read_with(&self.cx, |s, _| s.next_focus.clone());
        self.cx
            .update_window(self.window.into(), |_, w, _| focus.is_focused(w))
            .unwrap()
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
            .update_window(self.window.into(), |_, w, cx| w.dispatch_event(event, cx))
            .unwrap();
    }
    fn key(&mut self, key: &'static str) {
        self.key_event(key, true, false);
        self.key_event(key, false, false);
        self.draw();
    }
    fn frame(&mut self, millis: u64) -> usize {
        self.cx
            .executor()
            .advance_clock(Duration::from_millis(millis));
        let count = self
            .cx
            .update_window(self.window.into(), |_, w, cx| w.simulate_next_frame(cx))
            .unwrap();
        self.draw();
        count
    }
}

#[test]
fn alert_defaults_banner_overrides_and_slot_presence() {
    let mut f = Fixture::new(|c| {
        c.closable = None;
        c.custom_icon = true;
    });
    assert_eq!(f.context().kind, AlertType::Info);
    assert_eq!(f.context().variant, AlertVariant::Outlined);
    assert!(!f.context().show_icon);
    assert!(f.bounds(ICON).is_none());
    assert!(!f.context().closable);
    assert!(f.bounds(CLOSE).is_none());
    f.edit(|c| c.banner = true);
    assert_eq!(f.context().kind, AlertType::Warning);
    assert!(f.context().show_icon);
    assert!(f.bounds(ICON).is_some());
    f.edit(|c| {
        c.show_icon = Some(false);
        c.title = "";
        c.description = "0";
    });
    assert!(f.context().has_description);
    assert!(f.bounds(TITLE).is_none());
    assert!(f.bounds(ICON).is_none());
    assert!(f.bounds(DESCRIPTION).is_some());
    f.edit(|c| {
        c.kind = Some(AlertType::Error);
        c.variant = Some(AlertVariant::Filled);
    });
    assert_eq!(f.context().kind, AlertType::Error);
    assert_eq!(f.context().variant, AlertVariant::Filled);
}

#[test]
fn alert_native_geometry_matches_reference_spacing_and_rtl() {
    let mut f = Fixture::new(|c| c.show_icon = Some(true));
    let root = f.bounds(ROOT).unwrap();
    let title = f.bounds(TITLE).unwrap();
    let icon = f.bounds(ICON).unwrap();
    let close = f.bounds(CLOSE).unwrap();
    assert_eq!(root.size.height, px(40.));
    assert_eq!(icon.size, size(px(14.), px(14.)));
    assert_eq!(close.size, size(px(12.), px(12.)));
    assert_eq!(title.left() - icon.right(), px(8.));
    assert_eq!(close.left() - title.right(), px(8.));
    assert_eq!(icon.center().y, root.center().y);
    assert_eq!(close.center().y, root.center().y);
    f.patch(|p| p.direction = Some(Direction::Rtl));
    assert!(f.bounds(CLOSE).unwrap().right() < f.bounds(ICON).unwrap().left());
    assert_eq!(f.bounds(ROOT).unwrap().size, root.size);
}

#[test]
fn alert_description_has_enlarged_filled_icon_and_no_empty_title_gap() {
    let mut f = Fixture::new(|c| {
        c.title = "";
        c.description = "Description";
        c.show_icon = Some(true);
    });
    let root = f.bounds(ROOT).unwrap();
    let icon = f.bounds(ICON).unwrap();
    let body = f.bounds(DESCRIPTION).unwrap();
    assert_eq!(icon.size, size(px(24.), px(24.)));
    assert_eq!(icon.top() - root.top(), px(21.));
    assert_eq!(body.top(), icon.top());
    assert_eq!(body.left() - icon.right(), px(12.));
    assert!(f.bounds(TITLE).is_none());
    f.edit(|c| c.title = "Title");
    let title = f.bounds(TITLE).unwrap();
    // The pinned GPUI TextStyle rounds line-height to a logical pixel before shaping.
    assert_eq!(title.size.height, px(25.));
    assert!((f32::from(f.bounds(DESCRIPTION).unwrap().top() - title.bottom()) - 8.).abs() < 0.02);
}

#[test]
fn alert_wrapping_and_custom_content_use_actual_native_layout() {
    let mut f = Fixture::new(|c| {
        c.width = 220.;
        c.title = "Long alert title wraps to multiple lines in its narrow container.";
        c.show_icon = Some(true);
    });
    let root = f.bounds(ROOT).unwrap();
    assert!(root.size.height > px(60.));
    assert_eq!(f.bounds(CLOSE).unwrap().center().y, root.center().y);
    f.edit(|c| c.title = "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX");
    assert!(f.bounds(ROOT).unwrap().size.height > px(40.));
    f.edit(|c| {
        c.custom_content = true;
        c.width = 400.;
    });
    assert_eq!(f.bounds(DESCRIPTION).unwrap().size.height, px(45.));
    assert_eq!(f.bounds(TITLE).unwrap().size.height, px(28.));
}

#[test]
fn alert_close_pointer_orders_callbacks_and_preserves_root_bubbling() {
    let mut f = Fixture::new(|_| {});
    f.click(CLOSE);
    assert_eq!(f.activations(), [Activation::Pointer]);
    let events = f.events();
    assert_eq!(events.iter().filter(|e| **e == "close").count(), 1);
    assert_eq!(events.iter().filter(|e| **e == "after").count(), 1);
    assert!(events.contains(&"root-click"));
    assert!(events.iter().position(|e| *e == "close") < events.iter().position(|e| *e == "after"));
    assert!(f.bounds(ROOT).is_none());
    f.edit(|c| c.title = "Owner rerender");
    assert!(f.bounds(ROOT).is_none());
    assert_eq!(f.frame(1000), 0);
    f.edit(|c| c.generation += 1);
    assert!(f.bounds(ROOT).is_some());
    f.click(CLOSE);
    f.edit(|c| c.visible = false);
    f.edit(|c| c.visible = true);
    assert!(
        f.bounds(ROOT).is_some(),
        "unmount/remount resets the same stable ID"
    );
}

#[test]
fn alert_close_enter_and_space_share_native_activation_and_focus_recovery() {
    for (key, button) in [
        ("enter", KeyboardButton::Enter),
        ("space", KeyboardButton::Space),
    ] {
        let mut f = Fixture::new(|_| {});
        f.focus(true);
        f.key(key);
        assert_eq!(f.activations(), [Activation::Keyboard(button)]);
        assert_eq!(f.events().iter().filter(|e| **e == "after").count(), 1);
        assert!(f.next_focused());
    }
}

#[test]
fn alert_disabled_close_stale_keys_and_explicit_closability() {
    let mut f = Fixture::new(|c| c.close_disabled = true);
    f.click(CLOSE);
    assert!(f.activations().is_empty());
    assert!(f.bounds(ROOT).is_some());
    f.edit(|c| c.close_disabled = false);
    f.focus(true);
    f.key_event("space", true, false);
    f.edit(|c| c.close_disabled = true);
    f.edit(|c| c.close_disabled = false);
    f.focus(true);
    f.key_event("space", false, false);
    f.draw();
    assert!(f.activations().is_empty());
    f.edit(|c| {
        c.custom_close = true;
        c.closable = Some(false);
    });
    assert!(f.bounds(CLOSE).is_none());
    f.edit(|c| c.closable = None);
    assert!(f.bounds(CLOSE).unwrap().size.width > px(12.));
}

#[test]
fn alert_scope_updates_closability_without_disabling_static_content() {
    let mut f = Fixture::new(|c| c.closable = None);
    f.patch(|p| {
        p.alert.closable = Some(true);
        p.disabled = Some(true);
    });
    assert!(f.bounds(CLOSE).is_some());
    f.click(CLOSE);
    assert_eq!(f.activations(), [Activation::Pointer]);
}

#[test]
fn alert_actions_do_not_dismiss_and_static_root_is_not_a_tab_stop() {
    let mut f = Fixture::new(|c| c.action = true);
    f.click(ACTION);
    assert!(f.events().contains(&"action"));
    assert!(f.activations().is_empty());
    assert!(f.bounds(ROOT).is_some());
    f.focus(false);
    f.key("shift-tab");
    let focus = f.consumer.read_with(&f.cx, |s, _| s.close_focus.clone());
    assert!(
        f.cx.update_window(f.window.into(), |_, w, _| focus.is_focused(w))
            .unwrap()
    );
}

#[test]
fn alert_measured_leave_collapses_styled_padding_margin_and_completes_once() {
    let mut f = Fixture::motion(true, |c| {
        c.description = "Description";
        c.style = StyleRefinement::default()
            .pt(px(31.))
            .pb(px(29.))
            .mb(px(19.));
    });
    let open = f.bounds(ROOT).unwrap();
    f.focus(true);
    f.key("enter");
    assert_eq!(f.activations().len(), 1);
    assert!(!f.events().contains(&"after"));
    f.key("enter");
    assert_eq!(f.activations().len(), 1);
    f.frame(150);
    let middle = f.bounds(ROOT).unwrap();
    assert!(middle.size.height < open.size.height);
    assert!(middle.size.height > px(2.));
    f.frame(151);
    assert!(f.bounds(ROOT).is_none());
    assert!(f.next_focused());
    assert_eq!(f.events().iter().filter(|e| **e == "after").count(), 1);
    f.frame(1000);
    assert_eq!(f.frame(1), 0);
}

#[test]
fn alert_leave_matches_upstream_description_and_inline_padding_precedence() {
    let mut description = Fixture::motion(true, |c| {
        c.description = "Description";
    });
    description.focus(true);
    description.key("enter");
    description.frame(250);
    let root = description.bounds(ROOT).unwrap();
    assert_eq!(
        root.size.height,
        px(42.),
        "description padding remains until unmount, matching the actual upstream CSS"
    );
    assert_eq!(
        description.bounds(TITLE).unwrap().top() - root.top(),
        px(21.)
    );
    description.frame(51);
    assert!(description.bounds(ROOT).is_none());
    let mut title = Fixture::motion(true, |_| {});
    title.focus(true);
    title.key("enter");
    title.frame(150);
    let root = title.bounds(ROOT).unwrap();
    let ratio = 1.
        - crate::behavior::evaluate_cubic_bezier(
            crate::theme::CubicBezier::new(0.78, 0.14, 0.15, 0.86),
            0.5,
        );
    assert!((f64::from(f32::from(root.size.height)) - 40. * ratio).abs() <= 0.5);
    let mut styled = Fixture::motion(true, |c| c.style = StyleRefinement::default().py(px(14.)));
    styled.focus(true);
    styled.key("enter");
    styled.frame(250);
    assert_eq!(styled.bounds(ROOT).unwrap().size.height, px(30.));
}

#[test]
fn alert_owner_unmount_cancels_after_close_and_pending_animation_work() {
    let mut f = Fixture::motion(true, |c| c.remove_on_close = true);
    f.click(CLOSE);
    assert_eq!(f.activations().len(), 1);
    assert!(!f.events().contains(&"after"));
    f.frame(1000);
    assert!(!f.events().contains(&"after"));
    assert_eq!(f.frame(1000), 0);
}

#[test]
fn alert_reduced_motion_and_runtime_policy_change_finish_without_idle_frames() {
    let mut f = Fixture::motion(true, |_| {});
    f.focus(true);
    f.key("enter");
    f.patch(|p| p.motion = Some(false));
    assert!(f.bounds(ROOT).is_none());
    assert!(f.events().contains(&"after"));
    f.frame(1000);
    assert_eq!(f.frame(1), 0);
    let mut f = Fixture::motion(true, |_| {});
    f.cx.update(|cx| cx.set_reduce_motion(true));
    f.click(CLOSE);
    assert!(f.bounds(ROOT).is_none());
    assert!(f.events().contains(&"after"));
    assert_eq!(f.frame(1), 0);
}

#[test]
fn alert_close_callback_can_move_focus_and_mutate_owner_without_borrow_conflicts() {
    let mut f = Fixture::new(|c| c.focus_next_on_close = true);
    f.focus(true);
    f.key("space");
    assert!(f.next_focused());
    assert!(f.events().contains(&"after"));
}

#[test]
fn alert_surface_preserves_identity_and_explicit_native_live_semantics() {
    for (role, live) in [
        (gpui::Role::Alert, accesskit::Live::Assertive),
        (gpui::Role::Status, accesskit::Live::Polite),
    ] {
        let surface = element::surface(
            Accessibility::new(role)
                .label("Network unavailable")
                .automation_id("alert-network")
                .apply(gpui::div().id("network")),
            None,
            StyleRefinement::default(),
            live,
        );
        assert_eq!(surface.id(), Some("network".into()));
        assert_eq!(surface.a11y_role(), Some(role));
        let mut node = accesskit::Node::new(role);
        surface.write_a11y_info(&mut node);
        assert_eq!(node.live(), Some(live));
        assert!(node.is_live_atomic());
        assert_eq!(node.label(), Some("Network unavailable"));
        assert_eq!(node.author_id(), Some("alert-network"));
    }
}

#[test]
fn alert_builder_resolves_role_announcement_overrides_and_quiets_leaving_content() {
    assert_eq!(
        Alert::new("default").effective_live(false),
        accesskit::Live::Assertive
    );
    assert_eq!(
        Alert::new("status")
            .accessibility_role(gpui::Role::Status)
            .effective_live(false),
        accesskit::Live::Polite
    );
    assert_eq!(
        Alert::new("group")
            .accessibility_role(gpui::Role::Group)
            .effective_live(false),
        accesskit::Live::Off
    );
    let explicit = Alert::new("quiet").accessibility_live(accesskit::Live::Off);
    assert_eq!(explicit.effective_live(false), accesskit::Live::Off);
    assert_eq!(
        Alert::new("leaving").effective_live(true),
        accesskit::Live::Off
    );
}

#[test]
fn alert_scoped_content_factories_are_lazy_and_instance_slots_take_precedence() {
    let calls = std::sync::Arc::new(std::sync::Mutex::new(Vec::new()));
    let mut f = Fixture::new(|c| {
        c.closable = None;
    });
    let icon_calls = calls.clone();
    let close_calls = calls.clone();
    f.patch(|p| {
        p.alert.info_icon = Some(AlertIcon::from_content(move |context, _, _| {
            icon_calls.lock().unwrap().push(("icon", context.kind));
            gpui::div().w(px(17.)).h(px(15.))
        }));
        p.alert.close_icon = Some(AlertIcon::from_content(move |context, _, _| {
            close_calls.lock().unwrap().push(("close", context.kind));
            gpui::div().w(px(37.)).h(px(13.))
        }));
    });
    assert!(
        calls.lock().unwrap().is_empty(),
        "hidden slots do not execute factories"
    );
    f.edit(|c| {
        c.show_icon = Some(true);
        c.closable = Some(true);
    });
    assert!(calls.lock().unwrap().contains(&("icon", AlertType::Info)));
    assert!(calls.lock().unwrap().contains(&("close", AlertType::Info)));
    assert_eq!(f.bounds(ICON).unwrap().size, size(px(17.), px(15.)));
    assert_eq!(f.bounds(CLOSE).unwrap().size, size(px(37.), px(13.)));
    calls.lock().unwrap().clear();
    f.edit(|c| {
        c.custom_icon = true;
        c.custom_close = true;
    });
    assert!(
        calls.lock().unwrap().is_empty(),
        "instance content overrides configured factories"
    );
}

#[test]
fn alert_semantic_precedence_keeps_instance_root_above_resolved_layers() {
    let mut f = Fixture::new(|c| c.style = StyleRefinement::default().pt(px(32.)));
    f.patch(|p| {
        p.alert.styles.root = StyleRefinement::default().pt(px(50.));
        p.alert.styles_fn = Some(AlertStyleResolver::new(|context, _, _| {
            assert_eq!(context.kind, AlertType::Info);
            AlertStyles {
                root: StyleRefinement::default().pt(px(60.)),
                ..Default::default()
            }
        }));
    });
    assert_eq!(
        f.bounds(TITLE).unwrap().top() - f.bounds(ROOT).unwrap().top(),
        px(33.)
    );
}

#[test]
fn alert_static_and_settled_controls_schedule_zero_frames() {
    let mut f = Fixture::motion(true, |c| {
        c.action = true;
        c.show_icon = Some(true);
    });
    assert_eq!(f.frame(1000), 0);
    f.edit(|c| c.title = "A different title");
    assert_eq!(f.frame(1), 0);
}

#[test]
fn alert_catalog_artwork_uses_the_reference_frame_and_host_assets_keep_their_view_box() {
    for name in [
        IconName::CheckCircleFilled,
        IconName::InfoCircleFilled,
        IconName::ExclamationCircleFilled,
        IconName::CloseCircleFilled,
        IconName::CloseOutlined,
        IconName::SmileOutlined,
    ] {
        assert_eq!(name.icon().ant_design_viewbox_scale(), 8. / 7.);
    }
    assert_eq!(
        crate::icons::IconRef::asset("host/icon.svg", crate::icons::IconColorModel::Monochrome)
            .ant_design_viewbox_scale(),
        1.
    );
    assert_eq!(
        crate::icons::IconRef::asset(
            "antd/outlined/close",
            crate::icons::IconColorModel::Monochrome
        )
        .ant_design_viewbox_scale(),
        1.
    );
}
