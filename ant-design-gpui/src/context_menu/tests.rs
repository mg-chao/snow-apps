// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;
use crate::{ConfigScope, UiRoot};
use gpui::{
    AppContext, Context, Element, Entity, Focusable, IntoElement, KeyDownEvent, Keystroke,
    MouseButton, MouseDownEvent, MouseMoveEvent, MouseUpEvent, PlatformInput, Render, RenderOnce,
    Role, TestAppContext, Window, WindowHandle, accesskit, div, point, prelude::*, px,
};
use std::{
    cell::{Cell, RefCell},
    rc::Rc,
    time::Duration,
};

struct Consumer {
    state: Entity<ContextMenuState>,
    scope: ConfigScope,
    events: Rc<RefCell<Vec<MenuEvent>>>,
    disabled: bool,
    shown: bool,
    button_trigger: bool,
    focus_on_activate: Option<gpui::FocusHandle>,
    refresh_on_open: bool,
    button_semantics: Rc<RefCell<Option<accesskit::Node>>>,
}
#[derive(IntoElement)]
struct ButtonProbe(Rc<RefCell<Option<accesskit::Node>>>);
impl RenderOnce for ButtonProbe {
    fn render(self, window: &mut Window, cx: &mut gpui::App) -> impl IntoElement {
        let element = crate::Button::new("button-target")
            .label("Button target")
            .render(window, cx)
            .into_element();
        let mut node = accesskit::Node::new(element.a11y_role().unwrap());
        element.write_a11y_info(&mut node);
        *self.0.borrow_mut() = Some(node);
        element
    }
}
impl Render for Consumer {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        let events = self.events.clone();
        let focus_on_activate = self.focus_on_activate.clone();
        let refresh_on_open = self.refresh_on_open;
        let refreshed_state = self.state.clone();
        if !self.shown {
            return div().into_any_element();
        }
        let child = if self.button_trigger {
            ButtonProbe(self.button_semantics.clone()).into_any_element()
        } else {
            div()
                .id("target")
                .w(px(200.))
                .h(px(80.))
                .child("Target")
                .into_any_element()
        };
        div()
            .size_full()
            .p(px(40.))
            .child(
                ContextMenu::new("test-menu", &self.state)
                    .scope(self.scope.clone())
                    .automation_id("test-menu")
                    .disabled(self.disabled)
                    .on_event(move |event, window, cx| {
                        events.borrow_mut().push(event.clone());
                        if refresh_on_open
                            && matches!(event, MenuEvent::OpenChange { open: true, .. })
                        {
                            refreshed_state.update(cx, |s, cx| s.set_items(simple_items(), cx));
                        }
                        if matches!(event, MenuEvent::Activate(_))
                            && let Some(focus) = &focus_on_activate
                        {
                            window.focus(focus, cx);
                        }
                    })
                    .child(child),
            )
            .when_some(self.focus_on_activate.clone(), |root, focus| {
                root.child(
                    div()
                        .id("callback-target")
                        .track_focus(&focus)
                        .size(px(20.)),
                )
            })
            .into_any_element()
    }
}
struct Fixture {
    cx: TestAppContext,
    window: WindowHandle<UiRoot>,
    state: Entity<ContextMenuState>,
    consumer: Entity<Consumer>,
    events: Rc<RefCell<Vec<MenuEvent>>>,
}
impl Fixture {
    fn new(items: MenuItems) -> Self {
        let mut cx = TestAppContext::single();
        let mut output = None;
        let events = Rc::new(RefCell::new(Vec::new()));
        let window = cx.add_window(|window, cx| {
            crate::init(cx);
            window.activate_window();
            let scope = ConfigScope::new_window(cx);
            scope.update_patch(cx, |p| p.motion = Some(false)).unwrap();
            let state = cx.new(|cx| ContextMenuState::new(items, window, cx));
            let consumer = cx.new(|_| Consumer {
                state: state.clone(),
                scope: scope.clone(),
                events: events.clone(),
                disabled: false,
                shown: true,
                button_trigger: false,
                focus_on_activate: None,
                refresh_on_open: false,
                button_semantics: Rc::new(RefCell::new(None)),
            });
            output = Some((state, consumer.clone()));
            let root = UiRoot::new(scope, consumer, window, cx);
            window.focus(&root.focus_handle(cx), cx);
            root
        });
        let (state, consumer) = output.unwrap();
        let mut f = Self {
            cx,
            window,
            state,
            consumer,
            events,
        };
        f.draw();
        f
    }
    fn draw(&mut self) {
        self.cx.run_until_parked();
        self.cx
            .update_window(self.window.into(), |_, w, c| w.draw(c).clear(c))
            .unwrap();
        self.cx.run_until_parked();
    }
    fn event(&mut self, event: PlatformInput) {
        self.cx
            .update_window(self.window.into(), |_, w, c| {
                w.dispatch_event(event, c);
            })
            .unwrap();
        self.draw();
    }
    fn open(&mut self) {
        let state = self.state.clone();
        self.cx
            .update_window(self.window.into(), |_, w, c| {
                state.update(c, |s, c| s.open_at(point(px(50.), px(70.)), w, c))
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
    fn right_click(&mut self) {
        let position = point(px(80.), px(60.));
        self.event(PlatformInput::MouseDown(MouseDownEvent {
            position,
            button: MouseButton::Right,
            ..Default::default()
        }));
        self.event(PlatformInput::MouseUp(MouseUpEvent {
            position,
            button: MouseButton::Right,
            ..Default::default()
        }));
    }
    fn open_now(&self) -> bool {
        self.state.read_with(&self.cx, |s, _| s.is_open())
    }
    fn active(&self) -> Option<String> {
        self.state.read_with(&self.cx, |s, _| {
            s.active_item().map(|i| i.label.to_string())
        })
    }
    fn depth(&self) -> usize {
        self.state.read_with(&self.cx, |s, _| s.submenu_depth())
    }
    fn activations(&self) -> Vec<MenuActivation> {
        self.events
            .borrow()
            .iter()
            .filter_map(|e| {
                if let MenuEvent::Activate(a) = e {
                    Some(a.clone())
                } else {
                    None
                }
            })
            .collect()
    }
    fn highlight(&mut self) -> gpui::Bounds<gpui::ScaledPixels> {
        let color = self.consumer.read_with(&self.cx, |s, c| {
            s.scope
                .snapshot(c)
                .theme
                .component::<ContextMenuTheme>()
                .hover_background
                .to_hsla()
        });
        self.cx
            .update_window(self.window.into(), |_, w, _| {
                w.rendered_scene()
                    .quads
                    .iter()
                    .find(|q| q.background.as_solid() == Some(color))
                    .expect("painted active row")
                    .bounds
            })
            .unwrap()
    }
    fn edit(&mut self, edit: impl FnOnce(&mut Consumer)) {
        self.consumer.update(&mut self.cx, |s, c| {
            edit(s);
            c.notify()
        });
        self.draw();
    }
}

fn simple_items() -> MenuItems {
    MenuItems::new([
        MenuItem::new("disabled", "Disabled").disabled(true).into(),
        MenuItem::new("copy", "Copy")
            .shortcut("Ctrl+C")
            .badge("1/2")
            .into(),
        MenuItem::new("cut", "Cut").into(),
        MenuEntry::Separator,
        MenuItem::new("delete", "Delete").danger(true).into(),
    ])
    .unwrap()
}

#[test]
fn context_menu_right_click_keyboard_navigation_and_exactly_once_activation() {
    let mut f = Fixture::new(simple_items());
    f.right_click();
    assert!(f.open_now());
    assert_eq!(f.active().as_deref(), Some("Copy"));
    f.key("down");
    assert_eq!(f.active().as_deref(), Some("Cut"));
    f.key("end");
    assert_eq!(f.active().as_deref(), Some("Delete"));
    f.key("home");
    assert_eq!(f.active().as_deref(), Some("Copy"));
    f.key("enter");
    assert!(!f.open_now());
    assert_eq!(f.activations().len(), 1);
    assert_eq!(f.activations()[0].label, "Copy");
    f.key("enter");
    assert_eq!(f.activations().len(), 1);
    f.key("shift-f10");
    assert!(f.open_now());
    f.key("escape");
    assert!(!f.open_now());
    f.key("shift-f10");
    assert!(f.open_now());
    f.key("tab");
    assert!(!f.open_now());
}

#[test]
fn context_menu_lazy_submenu_is_fresh_and_escape_closes_one_level() {
    let builds = Rc::new(Cell::new(0));
    let counter = builds.clone();
    let items = MenuItems::new([MenuItem::new("more", "More")
        .lazy_submenu(move |_, _| {
            counter.set(counter.get() + 1);
            MenuItems::new([MenuItem::new("child", format!("Child {}", counter.get())).into()])
                .unwrap()
        })
        .into()])
    .unwrap();
    let mut f = Fixture::new(items);
    assert_eq!(builds.get(), 0);
    f.open();
    assert_eq!(builds.get(), 0);
    f.key("right");
    assert_eq!(builds.get(), 1);
    assert_eq!(f.depth(), 1);
    f.key("escape");
    assert_eq!(f.depth(), 0);
    assert!(f.open_now());
    f.key("right");
    assert_eq!(builds.get(), 2);
    f.key("enter");
    assert!(!f.open_now());
    assert_eq!(f.activations()[0].label, "Child 2");
}

#[test]
fn context_menu_typeahead_cycles_and_resets_after_timeout() {
    let mut f = Fixture::new(simple_items());
    f.open();
    f.key("c");
    assert_eq!(f.active().as_deref(), Some("Cut"));
    f.key("c");
    assert_eq!(f.active().as_deref(), Some("Copy"));
    f.cx.executor().advance_clock(Duration::from_millis(501));
    f.key("d");
    assert_eq!(f.active().as_deref(), Some("Delete"));
    f.key("enter");
    assert_eq!(f.activations()[0].label, "Delete");
}

#[test]
fn context_menu_empty_disabled_and_live_replacement_are_safe() {
    let mut f = Fixture::new(MenuItems::default());
    f.open();
    f.key("down");
    f.key("x");
    f.key("enter");
    assert!(f.open_now());
    assert!(f.activations().is_empty());
    f.state
        .update(&mut f.cx, |s, c| s.set_items(simple_items(), c));
    f.draw();
    f.key("end");
    assert_eq!(f.active().as_deref(), Some("Delete"));
    f.consumer.update(&mut f.cx, |s, c| {
        s.disabled = true;
        c.notify()
    });
    f.draw();
    assert!(!f.open_now());
    f.right_click();
    assert!(!f.open_now());
    assert!(f.activations().is_empty());
}

#[test]
fn context_menu_native_paint_refreshes_after_keyboard_and_virtual_jump() {
    let mut f = Fixture::new(simple_items());
    f.open();
    f.draw();
    let first = f.highlight();
    f.key("down");
    let second = f.highlight();
    assert!(second.origin.y > first.origin.y);
    let items = MenuItems::new(
        (0usize..1000).map(|i| MenuItem::new(("item", i), format!("Item {i}")).into()),
    )
    .unwrap();
    f.state.update(&mut f.cx, |s, c| s.set_items(items, c));
    f.draw();
    f.key("end");
    f.draw();
    assert_eq!(f.active().as_deref(), Some("Item 999"));
    assert!(f.state.read_with(&f.cx, |s, _| {
        s.levels[0]
            .native_list
            .as_ref()
            .unwrap()
            .bounds_for_item(999)
            .is_some()
    }));
    let _ = f.highlight();
    assert!(
        f.cx.update_window(f.window.into(), |_, w, _| w.rendered_scene().quads.len())
            .unwrap()
            < 80
    );
    f.key("home");
    f.draw();
    assert_eq!(f.active().as_deref(), Some("Item 0"));
    let _ = f.highlight();
}

#[test]
fn context_menu_unmount_cancels_lazy_open_and_retained_state() {
    let builds = Rc::new(Cell::new(0));
    let counter = builds.clone();
    let items = MenuItems::new([MenuItem::new("lazy", "Lazy")
        .lazy_submenu(move |_, _| {
            counter.set(counter.get() + 1);
            MenuItems::new([MenuItem::new("child", "Child").into()]).unwrap()
        })
        .into()])
    .unwrap();
    let mut f = Fixture::new(items);
    f.open();
    let state = f.state.clone();
    f.cx.update_window(f.window.into(), |_, w, c| {
        super::state::hover(&state, 0, 0, w, c)
    })
    .unwrap();
    f.consumer.update(&mut f.cx, |s, c| {
        s.shown = false;
        c.notify()
    });
    f.draw();
    f.draw();
    f.cx.executor().advance_clock(Duration::from_millis(200));
    f.draw();
    assert_eq!(builds.get(), 0);
    assert!(!f.open_now());
    assert_eq!(f.depth(), 0);
}

#[test]
fn context_menu_imperative_commands_in_one_cycle_follow_final_intent() {
    let mut f = Fixture::new(simple_items());
    let state = f.state.clone();
    f.cx.update_window(f.window.into(), |_, w, c| {
        state.update(c, |s, c| {
            s.open_at(point(px(80.), px(90.)), w, c);
            s.close(w, c);
        })
    })
    .unwrap();
    f.draw();
    assert!(!f.open_now());
    f.open();
    f.cx.update_window(f.window.into(), |_, w, c| {
        state.update(c, |s, c| {
            s.close(w, c);
            s.open_at(point(px(90.), px(100.)), w, c);
        })
    })
    .unwrap();
    f.draw();
    assert!(f.open_now());
}

#[test]
fn context_menu_generic_and_button_triggers_each_have_one_tab_stop() {
    let mut f = Fixture::new(simple_items());
    f.draw();
    f.key("tab");
    let trigger = f.state.read_with(&f.cx, |s, _| s.trigger.clone());
    assert!(
        f.cx.update_window(f.window.into(), |_, w, _| trigger.is_focused(w))
            .unwrap()
    );
    f.key("shift-f10");
    assert!(f.open_now());
    f.key("escape");
    f.edit(|s| s.button_trigger = true);
    f.draw();
    f.draw();
    assert!(!f.state.read_with(&f.cx, |s, _| s.trigger_tab_stop));
    f.key("tab");
    f.key("shift-f10");
    assert!(f.open_now());
    f.key("escape");
    assert!(!f.open_now());
}

#[test]
fn context_menu_activation_callback_keeps_its_chosen_focus() {
    let mut f = Fixture::new(simple_items());
    let focus = f.cx.update(|cx| cx.focus_handle());
    let target = focus.clone();
    f.edit(|s| s.focus_on_activate = Some(target));
    f.open();
    f.key("enter");
    assert!(
        f.cx.update_window(f.window.into(), |_, w, _| focus.is_focused(w))
            .unwrap()
    );
}

#[test]
fn context_menu_checks_persist_and_keyboard_reports_source() {
    let items = MenuItems::new([
        MenuItem::new("hidden", "Hidden").checked(false).into(),
        MenuItem::new("name", "Name").radio("sort", true).into(),
        MenuItem::new("date", "Date").radio("sort", false).into(),
    ])
    .unwrap();
    let mut f = Fixture::new(items);
    f.open();
    f.key("enter");
    assert_eq!(f.activations()[0].checked, Some(true));
    assert_eq!(f.activations()[0].source, MenuActivationSource::Keyboard);
    f.open();
    f.key("end");
    f.key("space");
    assert_eq!(f.activations()[1].checked, Some(true));
    f.state.read_with(&f.cx, |s, _| {
        assert_eq!(s.items().item(0).unwrap().check.checked(), Some(true));
        assert_eq!(s.items().item(1).unwrap().check.checked(), Some(false));
        assert_eq!(s.items().item(2).unwrap().check.checked(), Some(true));
    });
}

#[test]
fn context_menu_replacing_controller_under_same_id_releases_old_menu() {
    let mut f = Fixture::new(simple_items());
    f.open();
    let old = f.state.clone();
    let mut next = None;
    f.cx.update_window(f.window.into(), |_, w, c| {
        next = Some(c.new(|c| {
            ContextMenuState::new(
                MenuItems::new([MenuItem::new("new", "New item").into()]).unwrap(),
                w,
                c,
            )
        }));
    })
    .unwrap();
    let next = next.unwrap();
    f.edit(|s| s.state = next.clone());
    f.draw();
    assert!(!old.read_with(&f.cx, |s, _| s.is_open()));
    f.state = next;
    f.open();
    assert_eq!(f.active().as_deref(), Some("New item"));
}

#[test]
fn context_menu_pointer_hover_cancels_lazy_open_when_target_changes() {
    let calls = Rc::new(Cell::new(0));
    let count = calls.clone();
    let items = MenuItems::new([
        MenuItem::new("lazy", "Lazy")
            .lazy_submenu(move |_, _| {
                count.set(count.get() + 1);
                MenuItems::new([MenuItem::new("child", "Child").into()]).unwrap()
            })
            .into(),
        MenuItem::new("other", "Other").into(),
    ])
    .unwrap();
    let mut f = Fixture::new(items);
    f.open();
    let first = f.state.read_with(&f.cx, |s, _| {
        s.levels[0].scroll.bounds_for_item(0).unwrap().center()
    });
    let second = f.state.read_with(&f.cx, |s, _| {
        s.levels[0].scroll.bounds_for_item(1).unwrap().center()
    });
    f.event(PlatformInput::MouseMove(MouseMoveEvent {
        position: first,
        ..Default::default()
    }));
    f.cx.executor().advance_clock(Duration::from_millis(100));
    f.event(PlatformInput::MouseMove(MouseMoveEvent {
        position: second,
        ..Default::default()
    }));
    f.cx.executor().advance_clock(Duration::from_millis(200));
    f.draw();
    assert_eq!(calls.get(), 0);
    f.event(PlatformInput::MouseMove(MouseMoveEvent {
        position: first,
        ..Default::default()
    }));
    f.cx.executor().advance_clock(Duration::from_millis(150));
    f.draw();
    f.draw();
    assert_eq!(calls.get(), 1);
    assert_eq!(f.depth(), 1);
}

#[test]
fn context_menu_stale_painted_item_does_not_activate_replacement_at_same_index() {
    let mut f = Fixture::new(simple_items());
    f.open();
    let bounds = f.highlight();
    let position = point(px(bounds.center().x.0), px(bounds.center().y.0));
    let replacement = MenuItems::new([
        MenuItem::new("new-0", "New zero").into(),
        MenuItem::new("new-1", "New one").into(),
    ])
    .unwrap();
    f.state
        .update(&mut f.cx, |s, c| s.set_items(replacement, c));
    f.cx.update_window(f.window.into(), |_, w, c| {
        w.dispatch_event(
            PlatformInput::MouseDown(MouseDownEvent {
                position,
                button: MouseButton::Left,
                ..Default::default()
            }),
            c,
        );
        w.dispatch_event(
            PlatformInput::MouseUp(MouseUpEvent {
                position,
                button: MouseButton::Left,
                ..Default::default()
            }),
            c,
        );
    })
    .unwrap();
    f.draw();
    assert!(f.activations().is_empty());
    // Reflow may legitimately make the old position an outside press.
}

#[test]
fn menu_items_validate_identity_and_skip_unavailable_entries() {
    let items = MenuItems::new([
        MenuEntry::Separator,
        MenuItem::new("disabled", "Disabled").disabled(true).into(),
        MenuEntry::group("Files"),
        MenuItem::new("copy", "Copy").into(),
        MenuItem::new("cut", "Cut").into(),
    ])
    .unwrap();
    assert_eq!(items.next(None, 1), Some(3));
    assert_eq!(items.next(Some(4), 1), Some(3));
    assert_eq!(items.next(Some(3), -1), Some(4));
    assert!(
        MenuItems::new([
            MenuItem::new("same", "A").into(),
            MenuItem::new("same", "B").into()
        ])
        .is_err()
    );
    assert_eq!(MenuItems::default().next(None, 1), None);
}

#[test]
fn context_menu_refresh_on_open_keeps_initial_keyboard_focus() {
    let mut f = Fixture::new(simple_items());
    f.edit(|s| s.refresh_on_open = true);
    f.open();
    f.key("down");
    assert_eq!(f.active().as_deref(), Some("Cut"));
    f.key("enter");
    assert_eq!(f.activations()[0].label, "Cut");
}

#[test]
fn context_menu_button_keeps_native_identity_and_announces_menu() {
    let mut f = Fixture::new(simple_items());
    f.edit(|s| s.button_trigger = true);
    f.draw();
    f.consumer.read_with(&f.cx, |s, _| {
        let node = s.button_semantics.borrow();
        let node = node.as_ref().unwrap();
        assert_eq!(node.role(), Role::Button);
        assert_eq!(node.label(), Some("Button target"));
        assert_eq!(node.has_popup(), Some(accesskit::HasPopup::Menu));
    });
}

#[test]
fn context_menu_generic_region_has_context_action_and_menu_metadata() {
    let child = div()
        .id("region")
        .on_a11y_action(accesskit::Action::ShowContextMenu, |_, _, _| {});
    let region =
        super::trigger::semantics(child, "File actions".into(), false, true).into_element();
    assert_eq!(region.id(), Some("region".into()));
    assert_eq!(region.a11y_role(), Some(Role::Group));
    let mut node = accesskit::Node::new(region.a11y_role().unwrap());
    region.write_a11y_info(&mut node);
    assert_eq!(node.label(), Some("File actions"));
    assert_eq!(node.has_popup(), Some(accesskit::HasPopup::Menu));
    assert_eq!(node.is_expanded(), Some(true));
    assert!(node.supports_action(accesskit::Action::ShowContextMenu));
    assert!(!node.supports_action(accesskit::Action::Click));
    let disabled = super::trigger::semantics(
        div()
            .id("disabled")
            .on_a11y_action(accesskit::Action::ShowContextMenu, |_, _, _| {}),
        "File actions".into(),
        true,
        false,
    );
    let mut node = accesskit::Node::new(Role::Group);
    disabled.write_a11y_info(&mut node);
    assert!(node.is_disabled());
    assert!(!node.supports_action(accesskit::Action::ShowContextMenu));
}

#[test]
fn context_menu_accessibility_open_uses_shared_lifecycle_and_observable_menu_id() {
    let mut f = Fixture::new(simple_items());
    let state = f.state.clone();
    f.cx.update_window(f.window.into(), |_, w, c| {
        super::state::show_context_menu(&state, w, c)
    })
    .unwrap();
    f.draw();
    assert!(f.open_now());
    assert!(f.events.borrow().iter().any(|event| matches!(
        event,
        MenuEvent::OpenChange {
            open: true,
            reason: crate::popup::PopupOpenReason::Accessibility
        }
    )));
    f.cx.update(|cx| {
        let menu = super::view::menu_semantics(&state, 0, &"File actions".into(), cx)
            .apply(div().id("menu"));
        let mut node = accesskit::Node::new(menu.a11y_role().unwrap());
        menu.write_a11y_info(&mut node);
        assert_eq!(node.role(), Role::Menu);
        assert_eq!(node.author_id(), Some("test-menu"));
        assert_eq!(node.label(), Some("File actions"));
    });
    f.key("enter");
    assert_eq!(f.activations().len(), 1);
    f.edit(|s| s.disabled = true);
    f.cx.update_window(f.window.into(), |_, w, c| {
        super::state::show_context_menu(&state, w, c)
    })
    .unwrap();
    f.draw();
    assert!(!f.open_now());
}

#[test]
fn context_menu_scrolling_virtual_parent_retires_submenu_anchor() {
    let child = MenuItems::new([MenuItem::new("child", "Child").into()]).unwrap();
    let items = MenuItems::new(
        std::iter::once(MenuItem::new("more", "More").submenu(child).into())
            .chain((1usize..100).map(|i| MenuItem::new(("item", i), format!("Item {i}")).into())),
    )
    .unwrap();
    let mut f = Fixture::new(items);
    f.open();
    f.key("right");
    assert_eq!(f.depth(), 1);
    let bounds = f.state.read_with(&f.cx, |s, _| s.levels[0].scroll.bounds());
    f.event(PlatformInput::MouseMove(MouseMoveEvent {
        position: bounds.center(),
        ..Default::default()
    }));
    f.event(PlatformInput::Scroll(gpui::ScrollEvent {
        position: bounds.center(),
        delta: gpui::ScrollDelta::Pixels(point(px(0.), px(-600.))),
        ..Default::default()
    }));
    assert!(f.state.read_with(&f.cx, |s, _| {
        s.levels[0]
            .native_list
            .as_ref()
            .unwrap()
            .logical_scroll_top()
            .item_ix
            > 0
    }));
    assert_eq!(f.depth(), 0);
    assert!(f.open_now());
    f.key("end");
    assert_eq!(f.active().as_deref(), Some("Item 99"));
}

#[test]
fn context_menu_large_root_and_final_row_stay_inside_viewport() {
    let items = MenuItems::new(
        (0usize..1000).map(|i| MenuItem::new(("item", i), format!("Item {i}")).into()),
    )
    .unwrap();
    let mut f = Fixture::new(items);
    f.cx.simulate_window_resize(f.window.into(), gpui::size(px(880.), px(560.)));
    f.draw();
    let state = f.state.clone();
    f.cx.update_window(f.window.into(), |_, w, c| {
        state.update(c, |s, c| s.open_at(point(px(490.), px(186.)), w, c))
    })
    .unwrap();
    f.draw();
    f.key("end");
    f.draw();
    f.state.read_with(&f.cx, |s, c| {
        let layout = s.popup.layout(c).unwrap();
        assert!(layout.bounds.top() >= px(8.));
        assert!(layout.bounds.bottom() <= px(552.));
        assert!(layout.bounds.left() >= px(8.));
        assert!(layout.bounds.right() <= px(872.));
        let row = s.levels[0]
            .native_list
            .as_ref()
            .unwrap()
            .bounds_for_item(999)
            .unwrap();
        assert!(row.bottom() <= px(552.));
        assert!(row.top() >= px(8.));
    });
    let _ = f.highlight();
}

#[test]
fn menu_checks_toggle_without_changing_labels_and_radio_is_exclusive() {
    let mut items = MenuItems::new([
        MenuItem::new("hidden", "Hidden").checked(false).into(),
        MenuItem::new("name", "Name").radio("sort", true).into(),
        MenuItem::new("date", "Date").radio("sort", false).into(),
        MenuItem::new("other", "Other group")
            .radio("view", true)
            .into(),
    ])
    .unwrap();
    assert_eq!(items.toggle(0), Some(true));
    assert_eq!(items.toggle(2), Some(true));
    assert_eq!(items.item(1).unwrap().check.checked(), Some(false));
    assert_eq!(items.item(3).unwrap().check.checked(), Some(true));
    assert_eq!(items.item(0).unwrap().label, "Hidden");
}
