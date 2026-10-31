// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::cell::RefCell;

use gpui::{
    AppContext, Element, Entity, KeyDownEvent, KeyUpEvent, Keystroke, PlatformInput, Render,
    TestAppContext, WindowHandle, accesskit,
};

use super::*;
use crate::{ConfigPatch, UiRoot, behavior::Activation};

#[test]
fn selection_retains_single_value_and_suppresses_reselection() {
    let mut selection = GroupSelection {
        uncontrolled: Some("pear"),
        controlled: None,
    };
    assert!(!selection.select(&"pear"));
    assert!(selection.select(&"apple"));
    assert_eq!(selection.selected(), Some(&"apple"));
    assert!(!selection.select(&"apple"));
}

#[test]
fn controlled_selection_only_proposes_changes_and_resumes_retained_value() {
    let mut selection = GroupSelection {
        uncontrolled: Some(1),
        controlled: Some(2),
    };
    assert!(!selection.select(&2));
    assert!(selection.select(&3));
    assert!(selection.select(&3));
    assert_eq!(selection.selected(), Some(&2));
    selection.controlled = None;
    assert_eq!(selection.selected(), Some(&1));
}

#[derive(Clone, Debug, PartialEq)]
struct Setting {
    threshold: f32,
}

#[test]
fn arbitrary_application_values_need_no_hash_or_eq() {
    let mut selection = GroupSelection {
        uncontrolled: None,
        controlled: None,
    };
    assert!(selection.select(&Setting { threshold: 0.5 }));
    assert!(!selection.select(&Setting { threshold: 0.5 }));
    assert!(selection.select(&Setting { threshold: 0.7 }));
}

struct Consumer {
    scope: ConfigScope,
    focus: [FocusHandle; 3],
    before: FocusHandle,
    after: FocusHandle,
    value: Option<u8>,
    default_value: Option<u8>,
    order: Vec<u8>,
    disabled: Option<bool>,
    option_disabled: [Option<bool>; 3],
    external_focus: bool,
    button: bool,
    block: bool,
    vertical: bool,
    changes: Rc<RefCell<Vec<u8>>>,
    events: Rc<RefCell<Vec<(&'static str, Activation)>>>,
    semantics: Rc<RefCell<accesskit::Node>>,
}

impl Consumer {
    fn new(scope: ConfigScope, cx: &mut Context<Self>) -> Self {
        Self {
            scope,
            focus: std::array::from_fn(|_| cx.focus_handle().tab_index(0)),
            before: crate::behavior::focus_handle(cx),
            after: crate::behavior::focus_handle(cx),
            value: None,
            default_value: Some(2),
            order: vec![1, 2, 3],
            disabled: None,
            option_disabled: [None; 3],
            external_focus: true,
            button: false,
            block: false,
            vertical: false,
            changes: Rc::default(),
            events: Rc::default(),
            semantics: Rc::new(RefCell::new(accesskit::Node::new(Role::RadioGroup))),
        }
    }
}

impl Render for Consumer {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let changes = self.changes.clone();
        let events = self.events.clone();
        let options = self.order.iter().map(|value| {
            let index = usize::from(*value - 1);
            let events = self.events.clone();
            let mut option = RadioOption::new(("option", index), *value, format!("Option {value}"))
                .on_change(move |change, _, _| {
                    assert!(change.checked);
                    events.borrow_mut().push(("option", change.activation));
                });
            if self.external_focus {
                option = option.focus_handle(&self.focus[index]);
            }
            if let Some(disabled) = self.option_disabled[index] {
                option = option.disabled(disabled);
            }
            option
        });
        let mut group = RadioGroup::new("radio-group-test")
            .scope(self.scope.clone())
            .options(options)
            .accessibility_label("Shipping method")
            .accessibility_description("Choose one shipping method")
            .automation_id("radio-group.test")
            .vertical(self.vertical)
            .block(self.block)
            .option_type(if self.button {
                RadioOptionType::Button
            } else {
                RadioOptionType::Default
            })
            .on_change(move |value, _, _| {
                assert_eq!(events.borrow().len(), changes.borrow().len() + 1);
                changes.borrow_mut().push(*value);
            });
        if let Some(value) = self.value {
            group = group.value(value);
        }
        if let Some(value) = self.default_value {
            group = group.default_value(value);
        }
        if let Some(disabled) = self.disabled {
            group = group.disabled(disabled);
        }
        let group = group.render(window, cx).into_element();
        let mut node = accesskit::Node::new(group.a11y_role().unwrap());
        group.write_a11y_info(&mut node);
        *self.semantics.borrow_mut() = node;
        div()
            .p(px(12.))
            .flex()
            .flex_col()
            .gap(px(12.))
            .w(px(400.))
            .child(
                Radio::new("before")
                    .label("Before")
                    .focus_handle(&self.before),
            )
            .child(group)
            .child(Radio::new("after").label("After").focus_handle(&self.after))
    }
}

struct CachedPanel(Entity<Consumer>);

impl Render for CachedPanel {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        div().size_full().child(
            self.0
                .clone()
                .cached(StyleRefinement::default().w(px(440.)).h(px(220.))),
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
        let mut cx = TestAppContext::single();
        let mut consumer = None;
        let mut scope = None;
        let window = cx.add_window(|window, cx| {
            let root_scope = ConfigScope::new_window(cx);
            let radio_scope = root_scope
                .child(
                    ConfigPatch {
                        motion: Some(false),
                        ..Default::default()
                    },
                    cx,
                )
                .unwrap();
            let content = cx.new(|cx| {
                let mut content = Consumer::new(radio_scope.clone(), cx);
                configure(&mut content);
                content
            });
            consumer = Some(content.clone());
            scope = Some(radio_scope);
            let panel = cx.new(|_| CachedPanel(content));
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

    fn focus(&mut self, value: u8) {
        self.focus_without_draw(value);
        self.draw();
    }

    fn focus_without_draw(&mut self, value: u8) {
        let focus = self.consumer.read_with(&self.cx, |consumer, _| {
            consumer.focus[usize::from(value - 1)].clone()
        });
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                focus.focus(window, cx);
            })
            .unwrap();
    }

    fn focused(&mut self, value: u8) -> bool {
        let focus = self.consumer.read_with(&self.cx, |consumer, _| {
            consumer.focus[usize::from(value - 1)].clone()
        });
        self.cx
            .update_window(self.window.into(), |_, window, _| focus.is_focused(window))
            .unwrap()
    }

    fn key_without_draw(&mut self, stroke: &'static str) {
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                for event in [
                    PlatformInput::KeyDown(KeyDownEvent {
                        keystroke: Keystroke::parse(stroke).unwrap(),
                        is_held: false,
                        prefer_character_input: false,
                    }),
                    PlatformInput::KeyUp(KeyUpEvent {
                        keystroke: Keystroke::parse(stroke).unwrap(),
                    }),
                ] {
                    window.dispatch_event(event, cx);
                }
            })
            .unwrap();
    }

    fn key(&mut self, stroke: &'static str) {
        self.key_without_draw(stroke);
        self.draw();
    }

    fn changes(&self) -> Vec<u8> {
        self.consumer
            .read_with(&self.cx, |consumer, _| consumer.changes.borrow().clone())
    }

    fn scope_patch(&mut self, patch: ConfigPatch) {
        let scope = self.scope.clone();
        self.cx.update(|cx| scope.set_patch(patch, cx).unwrap());
        self.draw();
    }
}

#[test]
fn native_group_exposes_named_radio_group_semantics() {
    let fixture = Fixture::new(|_| {});
    let node = fixture.consumer.read_with(&fixture.cx, |consumer, _| {
        consumer.semantics.borrow().clone()
    });
    assert_eq!(node.role(), Role::RadioGroup);
    assert_eq!(node.label(), Some("Shipping method"));
    assert_eq!(node.description(), Some("Choose one shipping method"));
    assert_eq!(node.author_id(), Some("radio-group.test"));
    assert_eq!(node.orientation(), Some(accesskit::Orientation::Horizontal));
}

#[test]
fn native_group_is_one_tab_stop_and_space_never_deselects() {
    let mut fixture = Fixture::new(|_| {});
    fixture.key("tab");
    fixture.key("tab");
    assert!(fixture.focused(2));
    fixture.key("space");
    assert!(fixture.changes().is_empty());
    fixture.key("tab");
    let after = fixture
        .consumer
        .read_with(&fixture.cx, |consumer, _| consumer.after.clone());
    fixture
        .cx
        .update_window(fixture.window.into(), |_, window, _| {
            assert!(after.is_focused(window));
        })
        .unwrap();
    fixture.key("shift-tab");
    assert!(fixture.focused(2));
}

#[test]
fn native_empty_group_enters_at_first_enabled_and_exits_normally() {
    let mut fixture = Fixture::new(|consumer| {
        consumer.default_value = None;
        consumer.option_disabled[0] = Some(true);
    });
    fixture.key("tab");
    fixture.key("tab");
    assert!(fixture.focused(2));
    fixture.key("space");
    fixture.key("space");
    assert_eq!(fixture.changes(), [2]);
}

#[test]
fn native_arrows_wrap_skip_disabled_and_emit_option_before_group() {
    let mut fixture = Fixture::new(|consumer| consumer.option_disabled[1] = Some(true));
    fixture.focus(1);
    fixture.key("right");
    assert!(fixture.focused(3));
    fixture.key("down");
    assert!(fixture.focused(1));
    fixture.key("left");
    assert!(fixture.focused(3));
    fixture.key("up");
    assert!(fixture.focused(1));
    assert_eq!(fixture.changes(), [3, 1, 3, 1]);
    let events = fixture
        .consumer
        .read_with(&fixture.cx, |consumer, _| consumer.events.borrow().clone());
    assert_eq!(events.len(), 4);
    assert!(
        events
            .iter()
            .all(|event| { event.0 == "option" && event.1 == Activation::KeyboardNavigation })
    );
}

#[test]
fn native_home_end_skip_disabled_options() {
    let mut fixture = Fixture::new(|consumer| consumer.option_disabled[2] = Some(true));
    fixture.focus(2);
    fixture.key("home");
    assert!(fixture.focused(1));
    fixture.key("end");
    assert!(fixture.focused(2));
    fixture.key("ctrl-left");
    assert!(fixture.focused(2));
    assert_eq!(fixture.changes(), [1, 2]);
}

#[test]
fn native_controlled_group_waits_for_owner_and_option_false_overrides_disabled() {
    let mut fixture = Fixture::new(|consumer| consumer.value = Some(2));
    fixture.focus(1);
    fixture.key("space");
    fixture.key("space");
    assert_eq!(fixture.changes(), [1, 1]);
    fixture.edit(|consumer| {
        consumer.value = Some(1);
        consumer.disabled = Some(true);
        consumer.option_disabled[2] = Some(false);
    });
    fixture.focus(2);
    fixture.key("space");
    assert_eq!(fixture.changes().len(), 2);
    fixture.focus(3);
    fixture.key("space");
    assert_eq!(fixture.changes(), [1, 1, 3]);
}

#[test]
fn native_rapid_activation_uses_live_selection_even_before_redraw() {
    let mut fixture = Fixture::new(|_| {});
    fixture.focus(1);
    fixture.key_without_draw("space");
    fixture.key_without_draw("space");
    fixture.focus_without_draw(2);
    fixture.key_without_draw("space");
    fixture.draw();
    assert_eq!(fixture.changes(), [1, 2]);
}

#[test]
fn native_reordering_keeps_option_identity_and_selection() {
    let mut fixture = Fixture::new(|_| {});
    fixture.focus(2);
    fixture.edit(|consumer| {
        consumer.order = vec![3, 2, 1];
        consumer.default_value = Some(3);
    });
    assert!(fixture.focused(2));
    fixture.key("right");
    assert!(fixture.focused(1));
    assert_eq!(fixture.changes(), [1]);
}

#[test]
fn native_internal_focus_survives_reorder_and_option_removal_restores_selection() {
    let mut fixture = Fixture::new(|consumer| consumer.external_focus = false);
    fixture.key("tab");
    fixture.key("tab");
    let focus = fixture
        .cx
        .update_window(fixture.window.into(), |_, window, cx| {
            window.focused(cx).expect("group option focused")
        })
        .unwrap();
    fixture.edit(|consumer| consumer.order = vec![3, 2, 1]);
    fixture
        .cx
        .update_window(fixture.window.into(), |_, window, _| {
            assert!(focus.is_focused(window));
        })
        .unwrap();
    fixture.edit(|consumer| consumer.order = vec![3, 1]);
    fixture.edit(|consumer| consumer.order = vec![1, 2, 3]);
    let before = fixture
        .consumer
        .read_with(&fixture.cx, |consumer, _| consumer.before.clone());
    fixture
        .cx
        .update_window(fixture.window.into(), |_, window, cx| {
            before.focus(window, cx)
        })
        .unwrap();
    fixture.key("tab");
    fixture.key("space");
    // The retained value is still 2; the restored selection is the tab stop.
    assert!(fixture.changes().is_empty());
}

#[test]
fn native_scope_direction_updates_cached_group_navigation() {
    let mut fixture = Fixture::new(|_| {});
    fixture.focus(2);
    fixture.scope_patch(ConfigPatch {
        direction: Some(Direction::Rtl),
        motion: Some(false),
        ..Default::default()
    });
    fixture.key("left");
    assert!(fixture.focused(3));
    fixture.key("right");
    assert!(fixture.focused(2));
    fixture.key("down");
    assert!(fixture.focused(3));
    assert_eq!(fixture.changes(), [3, 2, 3]);
}

#[test]
fn native_all_disabled_group_has_no_tab_stop_and_live_scope_blocks_changes() {
    let mut fixture = Fixture::new(|_| {});
    fixture.focus(2);
    fixture.scope_patch(ConfigPatch {
        disabled: Some(true),
        motion: Some(false),
        ..Default::default()
    });
    assert!(!fixture.focused(2));
    fixture.key("space");
    fixture.key("right");
    assert!(fixture.changes().is_empty());
    let before = fixture
        .consumer
        .read_with(&fixture.cx, |consumer, _| consumer.before.clone());
    let after = fixture
        .consumer
        .read_with(&fixture.cx, |consumer, _| consumer.after.clone());
    fixture
        .cx
        .update_window(fixture.window.into(), |_, window, cx| {
            before.focus(window, cx)
        })
        .unwrap();
    fixture.key("tab");
    fixture
        .cx
        .update_window(fixture.window.into(), |_, window, _| {
            assert!(after.is_focused(window));
        })
        .unwrap();
}

#[test]
fn native_button_and_vertical_groups_keep_single_selection_navigation() {
    let mut fixture = Fixture::new(|consumer| {
        consumer.button = true;
        consumer.block = true;
        consumer.vertical = true;
    });
    fixture.focus(2);
    fixture.key("down");
    fixture.key("down");
    fixture.key("space");
    assert!(fixture.focused(1));
    assert_eq!(fixture.changes(), [3, 1]);
    let node = fixture.consumer.read_with(&fixture.cx, |consumer, _| {
        consumer.semantics.borrow().clone()
    });
    assert_eq!(node.orientation(), Some(accesskit::Orientation::Vertical));
}

#[test]
fn native_empty_option_list_is_a_valid_group() {
    let fixture = Fixture::new(|consumer| consumer.order.clear());
    assert!(fixture.changes().is_empty());
}
