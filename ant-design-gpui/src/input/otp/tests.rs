// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;
use crate::{
    ConfigScope, Direction, UiRoot,
    input::{CommitReason, EditOrigin, InputState},
};
use gpui::{
    AppContext, Context, Entity, EntityInputHandler, FocusHandle, Focusable, IntoElement,
    KeyDownEvent, Keystroke, MouseButton, MouseDownEvent, MouseUpEvent, PlatformInput, Render,
    Subscription, TestAppContext, Window, WindowHandle, div, prelude::*, px,
};
use std::{cell::RefCell, rc::Rc};

struct Consumer {
    state: Entity<OtpState>,
    scope: ConfigScope,
    before: FocusHandle,
    after: FocusHandle,
    width: f32,
    block: bool,
    events: Rc<RefCell<Vec<OtpEvent>>>,
    _events: Subscription,
}
impl Render for Consumer {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        div()
            .flex()
            .flex_col()
            .items_start()
            .w(px(self.width))
            .p(px(10.))
            .gap(px(12.))
            .child(div().id("before").track_focus(&self.before).child("Before"))
            .child(
                OtpInput::new("otp-test", &self.state)
                    .block(self.block)
                    .scope(self.scope.clone())
                    .accessibility_label("Security code")
                    .automation_id("otp-test.code"),
            )
            .child(div().id("after").track_focus(&self.after).child("After"))
    }
}
struct Fixture {
    cx: TestAppContext,
    window: WindowHandle<UiRoot>,
    consumer: Entity<Consumer>,
    state: Entity<OtpState>,
    events: Rc<RefCell<Vec<OtpEvent>>>,
}
impl Fixture {
    fn new(options: OtpOptions) -> Self {
        let mut cx = TestAppContext::single();
        let mut result = None;
        let window = cx.add_window(|window, cx| {
            window.activate_window();
            let scope = ConfigScope::new_window(cx);
            let state = cx.new(|cx| OtpState::with_options(options, window, cx).unwrap());
            let events = Rc::new(RefCell::new(Vec::new()));
            let consumer = cx.new(|cx| Consumer {
                _events: cx.subscribe(&state, |this: &mut Consumer, _, event, _| {
                    this.events.borrow_mut().push(event.clone())
                }),
                before: crate::behavior::focus_handle(cx),
                after: crate::behavior::focus_handle(cx),
                width: 400.,
                block: false,
                state: state.clone(),
                scope: scope.clone(),
                events: events.clone(),
            });
            result = Some((consumer.clone(), state, events));
            UiRoot::new(scope, consumer, window, cx)
        });
        let (consumer, state, events) = result.unwrap();
        let mut this = Self {
            cx,
            window,
            consumer,
            state,
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
    fn update(&mut self, update: impl FnOnce(&mut OtpState, &mut Window, &mut Context<OtpState>)) {
        let state = self.state.clone();
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                state.update(cx, |state, cx| update(state, window, cx))
            })
            .unwrap();
        self.draw();
    }
    fn editor(&self, index: usize) -> Entity<InputState> {
        self.state
            .read_with(&self.cx, |state, _| state.editors[index].editor.clone())
    }
    fn edit(
        &mut self,
        index: usize,
        edit: impl FnOnce(&mut InputState, &mut Window, &mut Context<InputState>),
    ) {
        let editor = self.editor(index);
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                editor.update(cx, |editor, cx| edit(editor, window, cx))
            })
            .unwrap();
        self.draw();
    }
    fn type_text(&mut self, text: &str) {
        self.edit(self.active(), |editor, window, cx| {
            editor.replace_text_in_range(None, text, window, cx)
        });
    }
    fn value(&self) -> String {
        self.state.read_with(&self.cx, |state, _| state.value())
    }
    fn active(&self) -> usize {
        self.state
            .read_with(&self.cx, |state, _| state.active_index())
    }
    fn focused(&mut self, index: usize) -> bool {
        let editor = self.editor(index);
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                editor.read(cx).focus_handle(cx).is_focused(window)
            })
            .unwrap()
    }
    fn key(&mut self, key: &'static str) {
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                window.dispatch_event(
                    PlatformInput::KeyDown(KeyDownEvent {
                        keystroke: Keystroke::parse(key).unwrap(),
                        is_held: false,
                        prefer_character_input: false,
                    }),
                    cx,
                );
            })
            .unwrap();
        self.draw();
    }
    fn click(&mut self, index: usize) {
        let position = self.editor(index).read_with(&self.cx, |editor, _| {
            editor.geometry().unwrap().viewport.center()
        });
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                window.dispatch_event(
                    PlatformInput::MouseDown(MouseDownEvent {
                        button: MouseButton::Left,
                        position,
                        modifiers: Default::default(),
                        click_count: 1,
                        first_mouse: false,
                    }),
                    cx,
                );
                window.dispatch_event(
                    PlatformInput::MouseUp(MouseUpEvent {
                        button: MouseButton::Left,
                        position,
                        modifiers: Default::default(),
                        click_count: 1,
                    }),
                    cx,
                );
            })
            .unwrap();
        self.draw();
    }
    fn clipboard(&mut self, text: &str) {
        self.cx
            .update(|cx| cx.write_to_clipboard(gpui::ClipboardItem::new_string(text.to_owned())));
    }
    fn clipboard_text(&mut self) -> Option<String> {
        self.cx
            .update(|cx| cx.read_from_clipboard().and_then(|item| item.text()))
    }
}

#[test]
fn unicode_native_entry_advances_graphemes_and_commits_only_when_leaving_the_group() {
    let mut f = Fixture::new(OtpOptions {
        length: 4,
        ..Default::default()
    });
    f.update(|state, window, cx| state.focus(window, cx));
    for (index, text) in ["1", "你", "e\u{301}", "👩🏽‍💻"].into_iter().enumerate() {
        assert!(f.focused(index));
        f.type_text(text);
    }
    assert_eq!(f.value(), "1你e\u{301}👩🏽‍💻");
    assert!(f.focused(3));
    assert_eq!(
        f.events
            .borrow()
            .iter()
            .filter(|event| matches!(event, OtpEvent::Change(_)))
            .count(),
        4
    );
    assert_eq!(
        f.events
            .borrow()
            .iter()
            .filter(|event| matches!(event, OtpEvent::Complete(_)))
            .count(),
        1
    );
    assert!(
        !f.events
            .borrow()
            .iter()
            .any(|event| matches!(event, OtpEvent::Commit(_) | OtpEvent::Blur))
    );
    f.key("enter");
    f.update(|state, window, cx| state.blur(window, cx));
    let commits: Vec<_> = f
        .events
        .borrow()
        .iter()
        .filter_map(|event| match event {
            OtpEvent::Commit(commit) => Some(commit.reason),
            _ => None,
        })
        .collect();
    assert_eq!(commits, [CommitReason::Enter, CommitReason::Blur]);
}

#[test]
fn native_and_keyboard_paste_format_before_limit_and_full_codes_replace_from_middle() {
    let mut f = Fixture::new(OtpOptions {
        formatter: Some(Rc::new(|text| {
            text.chars().filter(char::is_ascii_digit).collect()
        })),
        ..Default::default()
    });
    f.update(|state, window, cx| {
        state.set_value("12", window, cx);
        state.focus_cell(2, window, cx);
    });
    f.edit(2, |editor, window, cx| {
        editor.paste(
            gpui::ClipboardItem::new_string("3-4".to_owned()),
            window,
            cx,
        )
    });
    assert_eq!(f.value(), "1234");
    assert!(f.focused(4));
    f.clipboard("9 8 7 6 5 4 3");
    f.key("ctrl-v");
    assert_eq!(f.value(), "987654");
    assert!(f.focused(5));
    f.key("ctrl-z");
    assert_eq!(f.value(), "1234");
    assert!(f.focused(4));
    f.key("ctrl-shift-z");
    assert_eq!(f.value(), "987654");
    assert!(f.events.borrow().iter().any(
        |event| matches!(event, OtpEvent::Change(change) if change.origin == EditOrigin::Paste)
    ));
}

#[test]
fn aggregate_undo_clipboard_cut_and_mask_protection_work_across_cell_focus_changes() {
    let mut f = Fixture::new(OtpOptions::default());
    f.update(|state, window, cx| state.focus(window, cx));
    for digit in ["1", "2", "3"] {
        f.type_text(digit);
    }
    f.key("ctrl-z");
    assert_eq!(f.value(), "12");
    assert!(f.focused(2));
    f.key("ctrl-shift-z");
    assert_eq!(f.value(), "123");
    assert!(f.focused(3));
    f.key("ctrl-a");
    f.key("ctrl-c");
    assert_eq!(f.clipboard_text().as_deref(), Some("123"));
    f.key("ctrl-x");
    assert_eq!(f.value(), "");
    assert!(f.focused(0));
    f.key("ctrl-z");
    assert_eq!(f.value(), "123");
    f.update(|state, window, cx| {
        let mut options = state.options().clone();
        options.mask = Some("※".into());
        state.set_options(options, window, cx).unwrap();
    });
    f.clipboard("untouched");
    f.key("ctrl-a");
    f.key("ctrl-c");
    f.key("ctrl-x");
    assert_eq!(f.value(), "123");
    assert_eq!(f.clipboard_text().as_deref(), Some("untouched"));
    f.editor(0)
        .read_with(&f.cx, |editor, _| assert!(editor.is_protected()));
}

#[test]
fn ime_defers_distribution_and_same_preedit_commit_is_one_aggregate_transaction() {
    let mut f = Fixture::new(OtpOptions::default());
    f.update(|state, window, cx| state.focus(window, cx));
    f.edit(0, |editor, window, cx| {
        editor.replace_and_mark_text_in_range(None, "nihao", Some(5..5), window, cx)
    });
    assert_eq!(f.value(), "");
    f.key("right");
    f.key("enter");
    assert!(f.focused(0));
    assert!(
        !f.events
            .borrow()
            .iter()
            .any(|event| matches!(event, OtpEvent::Change(_) | OtpEvent::Commit(_)))
    );
    f.edit(0, |editor, window, cx| {
        editor.replace_and_mark_text_in_range(None, "你好", Some(2..2), window, cx)
    });
    f.edit(0, |editor, window, cx| {
        editor.replace_text_in_range(None, "你好", window, cx)
    });
    assert_eq!(f.value(), "你好");
    assert!(f.focused(2));
    assert_eq!(
        f.events
            .borrow()
            .iter()
            .filter(|event| matches!(event, OtpEvent::Change(_)))
            .count(),
        1
    );
    f.key("ctrl-z");
    assert_eq!(f.value(), "");
    assert!(f.focused(0));
}

#[test]
fn cancelling_composition_preserves_the_active_cell_code_and_history() {
    let mut f = Fixture::new(OtpOptions::default());
    f.update(|state, window, cx| {
        state.set_value("123456", window, cx);
        state.focus(window, cx);
    });
    f.edit(0, |editor, window, cx| {
        editor.replace_and_mark_text_in_range(None, "ni", Some(2..2), window, cx)
    });
    f.edit(0, |editor, window, cx| {
        editor.cancel_composition(window, cx)
    });
    assert_eq!(f.value(), "123456");
    assert!(f.focused(0));
    f.state
        .read_with(&f.cx, |state, _| assert!(!state.can_undo()));
    assert!(
        !f.events
            .borrow()
            .iter()
            .any(|event| matches!(event, OtpEvent::Change(_)))
    );
    f.key("shift-right");
    assert!(f.focused(1));
    f.type_text("9");
    assert_eq!(f.value(), "193456");
}

#[test]
fn otp_is_one_tab_stop_with_guarded_click_navigation_and_rtl_arrows() {
    let mut f = Fixture::new(OtpOptions::default());
    f.key("tab");
    let before = f.consumer.read_with(&f.cx, |view, _| view.before.clone());
    f.cx.update_window(f.window.into(), |_, window, _| {
        assert!(before.is_focused(window))
    })
    .unwrap();
    f.key("tab");
    assert!(f.focused(0));
    f.type_text("1");
    f.type_text("2");
    assert!(f.focused(2));
    f.click(5);
    assert!(f.focused(2));
    f.key("tab");
    let after = f.consumer.read_with(&f.cx, |view, _| view.after.clone());
    f.cx.update_window(f.window.into(), |_, window, _| {
        assert!(after.is_focused(window))
    })
    .unwrap();
    f.key("shift-tab");
    assert!(f.focused(2));
    f.update(|state, window, cx| state.set_value("123456", window, cx));
    let scope = f.consumer.read_with(&f.cx, |view, _| view.scope.clone());
    f.cx.update(|cx| {
        scope
            .update_patch(cx, |patch| patch.direction = Some(Direction::Rtl))
            .unwrap()
    });
    f.draw();
    f.key("left");
    assert!(f.focused(3));
    f.key("right");
    assert!(f.focused(2));
    f.key("home");
    assert!(f.focused(0));
    f.key("end");
    assert!(f.focused(5));
}

#[test]
fn clicking_an_occupied_cell_selects_it_for_replacement_and_backspace_preserves_holes() {
    let mut f = Fixture::new(OtpOptions::default());
    f.update(|state, window, cx| state.set_value("123456", window, cx));
    f.click(2);
    f.type_text("9");
    assert_eq!(f.value(), "129456");
    assert!(f.focused(3));
    f.key("backspace");
    assert_eq!(f.value(), "12956");
    assert!(f.focused(3));
    f.state
        .read_with(&f.cx, |state, _| assert_eq!(state.cells()[3].as_ref(), ""));
    f.key("backspace");
    assert!(f.focused(2));
    f.key("backspace");
    assert_eq!(f.value(), "1256");
    f.key("ctrl-z");
    assert_eq!(f.value(), "12956");
    assert!(f.focused(2));
}

#[test]
fn external_echo_preserves_composition_and_replacement_and_resize_are_silent() {
    let mut f = Fixture::new(OtpOptions::default());
    f.update(|state, window, cx| {
        state.set_value("12", window, cx);
        state.focus_cell(2, window, cx);
    });
    f.edit(2, |editor, window, cx| {
        editor.replace_and_mark_text_in_range(None, "ni", Some(2..2), window, cx)
    });
    f.update(|state, window, cx| state.set_value("12", window, cx));
    f.editor(2)
        .read_with(&f.cx, |editor, _| assert!(editor.is_composing()));
    f.update(|state, window, cx| state.set_value("34", window, cx));
    assert_eq!(f.value(), "34");
    f.state
        .read_with(&f.cx, |state, cx| assert!(!state.is_composing(cx)));
    let first = f.editor(0);
    f.update(|state, window, cx| {
        state
            .set_options(
                OtpOptions {
                    length: 1,
                    ..Default::default()
                },
                window,
                cx,
            )
            .unwrap()
    });
    assert_eq!(f.value(), "3");
    assert_eq!(f.editor(0), first);
    assert!(f.focused(0));
    assert!(
        !f.events
            .borrow()
            .iter()
            .any(|event| matches!(event, OtpEvent::Change(_)))
    );
}

#[test]
fn batched_native_changes_do_not_lose_the_latest_text_and_owner_replacement_wins() {
    let mut f = Fixture::new(OtpOptions::default());
    f.update(|state, window, cx| state.focus(window, cx));
    f.edit(0, |editor, window, cx| {
        editor.replace_text_in_range(None, "1", window, cx);
        editor.replace_text_in_range(None, "2", window, cx);
    });
    assert_eq!(f.value(), "12");
    assert!(f.focused(2));
    let editor = f.editor(2);
    let state = f.state.clone();
    f.events.borrow_mut().clear();
    f.cx.update_window(f.window.into(), |_, window, cx| {
        editor.update(cx, |editor, cx| {
            editor.replace_text_in_range(None, "9", window, cx)
        });
        state.update(cx, |state, cx| state.set_value("456789", window, cx));
    })
    .unwrap();
    f.draw();
    assert_eq!(f.value(), "456789");
    assert!(
        !f.events
            .borrow()
            .iter()
            .any(|event| matches!(event, OtpEvent::Change(_)))
    );
}

#[test]
fn read_only_and_scope_disabled_reject_native_edits_but_allow_external_updates() {
    let mut f = Fixture::new(OtpOptions::default());
    f.update(|state, window, cx| {
        state.set_value("123456", window, cx);
        state.set_read_only(true, window, cx);
        state.focus(window, cx);
    });
    f.type_text("9");
    f.key("backspace");
    f.clipboard("000000");
    f.key("ctrl-v");
    assert_eq!(f.value(), "123456");
    f.key("right");
    assert!(f.focused(1));
    f.update(|state, window, cx| {
        state.set_read_only(false, window, cx);
        state.set_value("987654", window, cx);
    });
    let scope = f.consumer.read_with(&f.cx, |view, _| view.scope.clone());
    f.cx.update(|cx| {
        scope
            .update_patch(cx, |patch| patch.disabled = Some(true))
            .unwrap()
    });
    f.cx.run_until_parked();
    f.state
        .read_with(&f.cx, |state, _| assert!(state.is_disabled()));
    f.type_text("1");
    assert_eq!(f.value(), "987654");
    assert!(!f.focused(1));
    assert!(
        !f.events
            .borrow()
            .iter()
            .any(|event| matches!(event, OtpEvent::Change(_)))
    );
}

#[test]
fn centered_source_and_custom_mask_geometry_match_painted_cell_coordinates() {
    let mut f = Fixture::new(OtpOptions::default());
    f.update(|state, window, cx| state.set_value("A", window, cx));
    let center = |editor: &InputState| {
        let geometry = editor.geometry().unwrap();
        let cluster = &geometry.clusters[0];
        let painted = geometry.window_point(cluster.bounds.center());
        assert!((f32::from(painted.x - geometry.viewport.center().x)).abs() < 0.1);
    };
    f.editor(0).read_with(&f.cx, |editor, _| center(editor));
    f.update(|state, window, cx| {
        let mut options = state.options().clone();
        options.mask = Some("※".into());
        state.set_options(options, window, cx).unwrap();
    });
    f.editor(0).read_with(&f.cx, |editor, _| center(editor));
    f.edit(0, |editor, window, cx| {
        let viewport = editor.geometry().unwrap().viewport;
        let candidate = editor
            .bounds_for_range(0..1, Default::default(), window, cx)
            .unwrap();
        assert!(candidate.size.width > px(0.));
        assert!((f32::from(candidate.center().x - viewport.center().x)).abs() < 0.1);
    });
}

#[test]
fn masked_otp_uses_surrounding_font_with_custom_input_typography() {
    use crate::theme::{Color, ComponentThemeConfig, ThemeConfig, TokenPatch};
    use crate::{
        Size,
        input::{InputTheme, InputTokenPatch},
    };
    let mut f = Fixture::new(OtpOptions {
        mask: Some("●".into()),
        ..Default::default()
    });
    f.update(|state, window, cx| state.set_value("A", window, cx));
    let editor = f.editor(0);
    let original = editor.read_with(&f.cx, |state, _| {
        state.geometry().unwrap().clusters[0].bounds.size.width
    });
    let scope = f.consumer.read_with(&f.cx, |view, _| view.scope.clone());
    f.cx.update(|cx| {
        scope
            .update_patch(cx, |patch| {
                patch.size = Some(Size::Large);
                let mut theme = ThemeConfig::default();
                theme.components.set_theme(
                    ComponentThemeConfig::<InputTheme>::new(InputTokenPatch {
                        input_font_size_lg: Some(28.),
                        ..Default::default()
                    })
                    .with_tokens(TokenPatch {
                        color_text: Some(Color::rgb(0xff0000)),
                        ..Default::default()
                    }),
                );
                patch.theme = Some(theme);
            })
            .unwrap()
    });
    f.draw();
    editor.read_with(&f.cx, |state, _| {
        let geometry = state.geometry().unwrap();
        assert_eq!(geometry.clusters[0].bounds.size.width, original);
        assert!((f32::from(geometry.viewport.size.height) - 42.).abs() < 0.1);
        let mask_center = geometry.window_point(geometry.clusters[0].bounds.center());
        assert!((mask_center.x - geometry.viewport.center().x).abs() < px(0.1));
    });
}

#[test]
fn otp_cells_shrink_together_in_a_constrained_container_and_block_layout() {
    for block in [false, true] {
        let mut f = Fixture::new(OtpOptions::default());
        f.consumer.update(&mut f.cx, |view, cx| {
            view.width = 180.;
            view.block = block;
            view.scope
                .update_patch(cx, |patch| patch.size = Some(crate::Size::Large))
                .unwrap();
            cx.notify();
        });
        f.update(|state, window, cx| state.set_value("123456", window, cx));
        let cells = (0..6)
            .map(|index| {
                f.editor(index)
                    .read_with(&f.cx, |state, _| state.geometry().unwrap().viewport)
            })
            .collect::<Vec<_>>();
        assert!(cells[0].left() >= px(10.));
        assert!(cells[5].right() <= px(170.));
        for pair in cells.windows(2) {
            assert!((pair[0].size.width - pair[1].size.width).abs() <= px(1.));
            assert!(pair[0].right() < pair[1].left());
        }
    }
}
