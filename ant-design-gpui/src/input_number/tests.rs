// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::{cell::RefCell, rc::Rc, time::Duration};

use gpui::{
    AppContext, Context, Entity, EntityInputHandler, IntoElement, KeyDownEvent, Keystroke,
    MouseButton, MouseDownEvent, MouseUpEvent, PlatformInput, Render, ScrollDelta, ScrollEvent,
    Subscription, TestAppContext, Window, WindowHandle, div, point, prelude::*, px,
};

use super::*;
use crate::{
    ConfigScope, UiRoot,
    input::{CommitReason, EditOrigin, InputState, TextSelection},
};

struct Consumer {
    state: Entity<InputNumberState>,
    scope: ConfigScope,
    shown: bool,
    controls_visible: bool,
    events: Rc<RefCell<Vec<InputNumberEvent>>>,
    _subscription: Subscription,
}
impl Render for Consumer {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        div().w(px(320.)).p(px(10.)).when(self.shown, |root| {
            root.child(
                InputNumber::new("number", &self.state, cx)
                    .scope(self.scope.clone())
                    .block(true)
                    .controls_visible(self.controls_visible)
                    .accessibility_label("Amount")
                    .automation_id("number-test.amount"),
            )
        })
    }
}
struct Fixture {
    cx: TestAppContext,
    window: WindowHandle<UiRoot>,
    consumer: Entity<Consumer>,
    state: Entity<InputNumberState>,
    editor: Entity<InputState>,
    events: Rc<RefCell<Vec<InputNumberEvent>>>,
}
impl Fixture {
    fn new(options: NumberOptions) -> Self {
        let mut cx = TestAppContext::single();
        let mut result = None;
        let window = cx.add_window(|window, cx| {
            window.activate_window();
            let scope = ConfigScope::new_window(cx);
            let state = cx.new(|cx| InputNumberState::with_options(options, window, cx).unwrap());
            let events = Rc::new(RefCell::new(Vec::new()));
            let consumer = cx.new(|cx| Consumer {
                _subscription: cx.subscribe(&state, |this: &mut Consumer, _, event, _| {
                    this.events.borrow_mut().push(event.clone());
                }),
                state: state.clone(),
                scope: scope.clone(),
                shown: true,
                controls_visible: true,
                events: events.clone(),
            });
            result = Some((
                consumer.clone(),
                state.clone(),
                state.read(cx).editor.clone(),
                events,
            ));
            UiRoot::new(scope, consumer, window, cx)
        });
        let (consumer, state, editor, events) = result.unwrap();
        let mut this = Self {
            cx,
            window,
            consumer,
            state,
            editor,
            events,
        };
        this.draw();
        this
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
    fn edit(&mut self, edit: impl FnOnce(&mut InputState, &mut Window, &mut Context<InputState>)) {
        let editor = self.editor.clone();
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                editor.update(cx, |state, cx| edit(state, window, cx));
            })
            .unwrap();
        self.draw();
    }
    fn update(
        &mut self,
        update: impl FnOnce(&mut InputNumberState, &mut Window, &mut Context<InputNumberState>),
    ) {
        let state = self.state.clone();
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                state.update(cx, |state, cx| update(state, window, cx));
            })
            .unwrap();
        self.draw();
    }
    fn raw(&mut self, text: &str) {
        self.edit(|state, window, cx| {
            state.select_all(window, cx);
            state.replace_text_in_range(None, text, window, cx);
        });
    }
    fn text(&self) -> String {
        self.editor
            .read_with(&self.cx, |state, _| state.value().to_owned())
    }
    fn value(&self) -> Option<String> {
        self.state
            .read_with(&self.cx, |state, _| state.value().map(ToString::to_string))
    }
    fn event(&mut self, event: PlatformInput) {
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                window.dispatch_event(event, cx);
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
    fn wheel(&mut self, y: f32) {
        self.event(PlatformInput::Scroll(ScrollEvent {
            position: point(px(30.), px(25.)),
            delta: ScrollDelta::Lines(point(0., y)),
            ..Default::default()
        }));
    }
    fn press_up(&mut self) {
        self.event(PlatformInput::MouseDown(MouseDownEvent {
            button: MouseButton::Left,
            position: point(px(300.), px(17.)),
            modifiers: Default::default(),
            click_count: 1,
            first_mouse: false,
        }));
    }
    fn release(&mut self) {
        self.event(PlatformInput::MouseUp(MouseUpEvent {
            button: MouseButton::Left,
            position: point(px(300.), px(17.)),
            modifiers: Default::default(),
            click_count: 1,
        }));
    }
    fn advance(&mut self, millis: u64) {
        self.cx
            .executor()
            .advance_clock(Duration::from_millis(millis));
        self.draw();
    }

    fn pressed_handle_painted(&mut self) -> bool {
        let color = self.consumer.read_with(&self.cx, |view, cx| {
            view.scope
                .snapshot(cx)
                .theme
                .component::<InputNumberTheme>()
                .handle_active_bg
                .to_hsla()
        });
        self.cx
            .update_window(self.window.into(), |_, window, _| {
                window
                    .rendered_scene()
                    .quads
                    .iter()
                    .any(|quad| quad.background.as_solid() == Some(color))
            })
            .unwrap()
    }
}
fn decimal(text: &str) -> ExactDecimal {
    text.parse().unwrap()
}

#[test]
fn native_drafts_emit_changes_only_when_accepted_and_commit_clamps_and_rounds() {
    let mut f = Fixture::new(NumberOptions {
        maximum: Some(decimal("10")),
        precision: Some(2),
        ..NumberOptions::exact()
    });
    f.update(|state, window, cx| state.focus(window, cx));
    f.raw("2.005");
    assert_eq!(f.value().as_deref(), Some("2.005"));
    f.key("enter");
    assert_eq!(f.text(), "2.01");
    f.raw("-");
    assert_eq!(f.text(), "-");
    assert_eq!(f.value().as_deref(), Some("2.01"));
    f.key("enter");
    assert_eq!(f.text(), "2.01");
    f.raw("999");
    assert_eq!(f.value().as_deref(), Some("2.01"));
    f.update(|state, window, cx| state.blur(window, cx));
    assert_eq!(f.text(), "10.00");
    assert_eq!(f.value().as_deref(), Some("10"));
    let events = f.events.borrow();
    let changes: Vec<_> = events
        .iter()
        .filter_map(|event| match event {
            InputNumberEvent::Change(change) => Some(change.value.as_ref().unwrap().to_string()),
            _ => None,
        })
        .collect();
    assert_eq!(changes, ["2.005", "2.01", "10"]);
    assert!(events.iter().any(
        |e| matches!(e, InputNumberEvent::Commit(commit) if commit.reason == CommitReason::Blur)
    ));
}

#[test]
fn composition_defers_numeric_acceptance_and_same_text_finalization_is_collected() {
    let mut f = Fixture::new(NumberOptions::exact());
    f.update(|state, window, cx| state.focus(window, cx));
    f.raw("2");
    f.edit(|state, window, cx| {
        state.select_all(window, cx);
        state.replace_and_mark_text_in_range(None, "12", Some(2..2), window, cx);
    });
    assert_eq!(f.value().as_deref(), Some("2"));
    assert_eq!(f.text(), "12");
    f.key("up");
    assert_eq!(f.text(), "12");
    f.edit(|state, window, cx| state.replace_text_in_range(None, "12", window, cx));
    assert_eq!(f.value().as_deref(), Some("12"));
    f.update(|state, window, cx| state.undo(window, cx));
    assert_eq!(f.text(), "2");
    assert_eq!(f.value().as_deref(), Some("2"));
}

#[test]
fn exact_keyboard_steps_and_opt_in_focused_wheel_preserve_decimal_values() {
    let mut f = Fixture::new(NumberOptions {
        step: decimal("0.001"),
        ..NumberOptions::exact()
    });
    f.update(|state, window, cx| {
        state
            .set_value(Some(decimal("9007199254740993.001").into()), window, cx)
            .unwrap();
        state.focus(window, cx);
    });
    f.key("up");
    assert_eq!(f.text(), "9007199254740993.002");
    f.key("shift-down");
    assert_eq!(f.text(), "9007199254740992.992");
    f.wheel(1.);
    assert_eq!(f.text(), "9007199254740992.992");
    f.update(|state, window, cx| {
        let mut options = state.options().clone();
        options.change_on_wheel = true;
        options.keyboard = false;
        state.set_options(options, window, cx).unwrap();
    });
    f.key("up");
    assert_eq!(f.text(), "9007199254740992.992");
    f.wheel(1.);
    assert_eq!(f.text(), "9007199254740992.993");
    f.update(|state, window, cx| state.blur(window, cx));
    f.wheel(1.);
    assert_eq!(f.text(), "9007199254740992.993");
}

#[test]
fn typing_formatter_keeps_cursor_and_amends_undo_without_destroying_redo() {
    let mut f = Fixture::new(NumberOptions {
        parser: Some(Rc::new(|text| text.replace(',', ""))),
        formatter: Some(Rc::new(|context| {
            let raw = context.input.replace(',', "");
            let text = if context.user_typing {
                raw.as_str()
            } else {
                context.canonical
            };
            if text.len() > 3 {
                format!("{},{}", &text[..text.len() - 3], &text[text.len() - 3..])
            } else {
                text.to_owned()
            }
        })),
        ..NumberOptions::exact()
    });
    f.update(|state, window, cx| state.focus(window, cx));
    f.raw("1234");
    assert_eq!(f.text(), "1,234");
    f.edit(|state, window, cx| {
        state.select(TextSelection::collapsed(3), window, cx);
        state.replace_text_in_range(None, "0", window, cx);
    });
    assert_eq!(f.text(), "12,034");
    f.state.read_with(&f.cx, |state, cx| {
        assert_eq!(state.selection(cx), TextSelection::collapsed(4))
    });
    f.update(|state, window, cx| state.undo(window, cx));
    assert_eq!(f.text(), "1,234");
    f.update(|state, window, cx| state.redo(window, cx));
    assert_eq!(f.text(), "12,034");
    f.update(|state, window, cx| state.undo(window, cx));
    f.update(|state, window, cx| state.undo(window, cx));
    assert_eq!(f.text(), "");
    assert_eq!(f.value(), None);
}

#[test]
fn external_echo_preserves_draft_and_different_value_replaces_queued_edits_silently() {
    let mut f = Fixture::new(NumberOptions::exact());
    f.raw("1.");
    f.events.borrow_mut().clear();
    f.update(|state, window, cx| {
        state
            .set_value(Some(decimal("1").into()), window, cx)
            .unwrap()
    });
    assert_eq!(f.text(), "1.");
    let state = f.state.clone();
    let editor = f.editor.clone();
    f.cx.update_window(f.window.into(), |_, window, cx| {
        editor.update(cx, |editor, cx| {
            editor.replace_text_in_range(None, "2", window, cx)
        });
        state.update(cx, |state, cx| {
            state
                .set_value(Some(decimal("9").into()), window, cx)
                .unwrap()
        });
    })
    .unwrap();
    f.draw();
    assert_eq!(f.text(), "9");
    assert_eq!(f.value().as_deref(), Some("9"));
    assert!(
        !f.events
            .borrow()
            .iter()
            .any(|event| matches!(event, InputNumberEvent::Change(_)))
    );
}

#[test]
fn held_buttons_repeat_after_delay_and_cancel_on_release_blur_disable_and_unmount() {
    let mut f = Fixture::new(NumberOptions::exact());
    f.press_up();
    assert_eq!(f.text(), "1");
    f.advance(599);
    assert_eq!(f.text(), "1");
    f.advance(1);
    assert_eq!(f.text(), "2");
    f.advance(200);
    assert_eq!(f.text(), "3");
    f.release();
    f.advance(1000);
    assert_eq!(f.text(), "3");
    f.press_up();
    f.update(|state, window, cx| state.blur(window, cx));
    f.advance(1000);
    assert_eq!(f.text(), "4");
    f.press_up();
    f.update(|state, window, cx| state.set_disabled(true, window, cx));
    f.advance(1000);
    assert_eq!(f.text(), "5");
    f.update(|state, window, cx| state.set_disabled(false, window, cx));
    f.press_up();
    f.consumer.update(&mut f.cx, |view, cx| {
        view.shown = false;
        cx.notify();
    });
    f.draw();
    f.advance(1000);
    assert_eq!(f.text(), "6");
}

#[test]
fn read_only_rejects_native_edits_clear_and_steps_but_allows_external_values() {
    let mut f = Fixture::new(NumberOptions::exact());
    f.update(|state, window, cx| {
        state
            .set_value(Some(decimal("12").into()), window, cx)
            .unwrap();
        state.set_read_only(true, window, cx);
        state.focus(window, cx);
    });
    f.raw("55");
    f.key("up");
    f.update(|state, window, cx| state.clear(window, cx));
    assert_eq!(f.text(), "12");
    f.update(|state, window, cx| {
        state
            .set_value(Some(decimal("42").into()), window, cx)
            .unwrap()
    });
    assert_eq!(f.text(), "42");
    assert!(
        !f.events
            .borrow()
            .iter()
            .any(|event| matches!(event, InputNumberEvent::Change(_)))
    );
}

#[test]
fn provider_disable_cancels_held_step_before_next_render_and_reenable_does_not_resume_it() {
    let mut f = Fixture::new(NumberOptions::exact());
    f.press_up();
    let scope = f.consumer.read_with(&f.cx, |view, _| view.scope.clone());
    f.cx.update(|cx| {
        scope
            .update_patch(cx, |patch| patch.disabled = Some(true))
            .unwrap()
    });
    // Process provider notifications without drawing. A pending repeat must not
    // rely on the next rendered frame to observe the disabled state.
    f.cx.run_until_parked();
    f.state
        .read_with(&f.cx, |state, cx| assert!(state.is_disabled(cx)));
    f.cx.executor().advance_clock(Duration::from_millis(800));
    f.cx.run_until_parked();
    assert_eq!(f.text(), "1");
    f.cx.update(|cx| {
        scope
            .update_patch(cx, |patch| patch.disabled = Some(false))
            .unwrap()
    });
    f.advance(1000);
    assert_eq!(f.text(), "1");
}

#[test]
fn cancelled_numeric_hold_does_not_leave_pressed_visuals_after_reenable_or_blur() {
    let mut f = Fixture::new(NumberOptions::exact());
    f.press_up();
    f.advance(200);
    assert!(f.pressed_handle_painted());
    f.update(|state, window, cx| state.set_disabled(true, window, cx));
    f.update(|state, window, cx| state.set_disabled(false, window, cx));
    assert!(!f.pressed_handle_painted());
    f.press_up();
    f.advance(200);
    assert!(f.pressed_handle_painted());
    f.update(|state, window, cx| state.blur(window, cx));
    f.advance(200);
    assert!(!f.pressed_handle_painted());
}

#[test]
fn numeric_handles_animate_in_and_out_and_hidden_windows_cancel_motion() {
    let mut f = Fixture::new(NumberOptions::exact());
    f.consumer.update(&mut f.cx, |view, cx| {
        view.controls_visible = false;
        cx.notify();
    });
    f.draw();
    f.advance(200);
    let width = |f: &Fixture| {
        f.editor.read_with(&f.cx, |state, _| {
            f32::from(state.geometry().unwrap().viewport.size.width)
        })
    };
    let rest = width(&f);
    f.update(|state, window, cx| state.focus(window, cx));
    assert!((width(&f) - rest).abs() < 0.001);
    f.advance(100);
    let middle = width(&f);
    f.advance(100);
    let focused = width(&f);
    assert!(focused < middle && middle < rest);
    assert!((rest - focused - 22.).abs() < 0.001);
    f.update(|state, window, cx| state.blur(window, cx));
    assert!((width(&f) - focused).abs() < 0.001);
    f.advance(100);
    assert!(width(&f) > focused && width(&f) < rest);
    f.cx.simulate_window_visibility_change(f.window.into(), gpui::WindowVisibility::Hidden);
    f.draw();
    assert!((width(&f) - rest).abs() < 0.001);
    f.cx.update_window(f.window.into(), |_, window, cx| {
        window.simulate_next_frame(cx);
    })
    .unwrap();
    f.draw();
    f.cx.update_window(f.window.into(), |_, window, cx| {
        assert_eq!(window.simulate_next_frame(cx), 0)
    })
    .unwrap();
}

#[test]
fn disabled_blur_commit_preserves_draft_but_enter_commits_and_empty_emits_none() {
    let mut f = Fixture::new(NumberOptions {
        change_on_blur: false,
        maximum: Some(decimal("10")),
        ..NumberOptions::exact()
    });
    f.update(|state, window, cx| state.focus(window, cx));
    f.raw("99");
    f.update(|state, window, cx| state.blur(window, cx));
    assert_eq!(f.text(), "99");
    assert_eq!(f.value(), None);
    f.update(|state, window, cx| state.focus(window, cx));
    f.key("enter");
    assert_eq!(f.text(), "10");
    f.update(|state, window, cx| state.clear(window, cx));
    assert_eq!(f.text(), "");
    assert!(f.events.borrow().iter().any(|event| matches!(event, InputNumberEvent::Change(change) if change.value.is_none() && change.source == NumericChangeSource::Input(EditOrigin::Clear))));
}
