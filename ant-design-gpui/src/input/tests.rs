// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::{cell::RefCell, rc::Rc, time::Duration};

use gpui::{
    AppContext, Context, Entity, EntityInputHandler, IntoElement, KeyDownEvent, KeyUpEvent,
    Keystroke, MouseButton, MouseDownEvent, MouseMoveEvent, MouseUpEvent, PlatformInput, Render,
    Subscription, TestAppContext, Window, WindowHandle, div, point, prelude::*, px,
};

use super::*;
use crate::{ConfigScope, UiRoot};

struct Consumer {
    state: Entity<InputState>,
    scope: ConfigScope,
    multiline: bool,
    auto_size: bool,
    disabled: Option<bool>,
    mode: state::InputMode,
    allow_clear: bool,
    adornments: bool,
    loading: bool,
    outer_wheel: Rc<RefCell<usize>>,
    height: Option<f32>,
    events: Rc<RefCell<Vec<InputEvent>>>,
    _subscription: Subscription,
}

impl Consumer {
    fn new(state: Entity<InputState>, scope: ConfigScope, cx: &mut Context<Self>) -> Self {
        let events = Rc::new(RefCell::new(Vec::new()));
        let subscription = cx.subscribe(&state, |this: &mut Self, _, event, _| {
            this.events.borrow_mut().push(event.clone());
        });
        Self {
            state,
            scope,
            multiline: false,
            auto_size: false,
            disabled: None,
            mode: state::InputMode::Text,
            allow_clear: false,
            adornments: false,
            loading: false,
            outer_wheel: Rc::default(),
            height: None,
            events,
            _subscription: subscription,
        }
    }
}

impl Render for Consumer {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        let configure = |input: Input| {
            let mut input = input
                .scope(self.scope.clone())
                .accessibility_label("Name")
                .automation_id("input-test.name")
                .placeholder("Name")
                .multiline(self.multiline)
                .allow_clear(self.allow_clear)
                .rows(2);
            if self.auto_size {
                input = input.auto_size(2, 4);
            }
            if let Some(disabled) = self.disabled {
                input = input.disabled(disabled);
            }
            if self.adornments {
                input = input.prefix("￥").suffix("元").show_count(true);
            }
            if let Some(height) = self.height {
                input = input.h(px(height));
            }
            input
        };
        let input = match self.mode {
            state::InputMode::Text if self.multiline => TextArea::new("test-input", &self.state)
                .configure(configure)
                .into_any_element(),
            state::InputMode::Text => {
                configure(Input::new("test-input", &self.state)).into_any_element()
            }
            state::InputMode::Password => PasswordInput::new("test-input", &self.state)
                .configure(configure)
                .into_any_element(),
            state::InputMode::Search => SearchInput::new("test-input", &self.state)
                .loading(self.loading)
                .configure(configure)
                .into_any_element(),
        };
        let outer_wheel = self.outer_wheel.clone();
        div()
            .id("input-container")
            .w(px(320.))
            .p(px(10.))
            .on_scroll(move |_, _, _| *outer_wheel.borrow_mut() += 1)
            .child(input)
    }
}

struct Fixture {
    cx: TestAppContext,
    window: WindowHandle<UiRoot>,
    consumer: Entity<Consumer>,
    state: Entity<InputState>,
    events: Rc<RefCell<Vec<InputEvent>>>,
}

impl Fixture {
    fn new(multiline: bool) -> Self {
        let mut cx = TestAppContext::single();
        let mut result = None;
        let window = cx.add_window(|window, cx| {
            let scope = ConfigScope::new_window(cx);
            let state = cx.new(|cx| InputState::new(window, cx));
            let consumer = cx.new(|cx| {
                let mut view = Consumer::new(state.clone(), scope.clone(), cx);
                view.multiline = multiline;
                view
            });
            result = Some((consumer.clone(), state, consumer.read(cx).events.clone()));
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
                window.draw(cx).clear(cx);
            })
            .unwrap();
        self.cx.run_until_parked();
    }

    fn edit(
        &mut self,
        operation: impl FnOnce(&mut InputState, &mut Window, &mut Context<InputState>),
    ) {
        let state = self.state.clone();
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                state.update(cx, |state, cx| operation(state, window, cx));
            })
            .unwrap();
        self.draw();
    }

    fn value(&mut self) -> String {
        self.state
            .read_with(&self.cx, |state, _| state.value().to_owned())
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
    fn configure(&mut self, configure: impl FnOnce(&mut Consumer)) {
        self.consumer.update(&mut self.cx, |consumer, cx| {
            configure(consumer);
            cx.notify();
        });
        self.draw();
    }
    fn key_up(&mut self, key: &'static str) {
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                window.dispatch_event(
                    PlatformInput::KeyUp(KeyUpEvent {
                        keystroke: Keystroke::parse(key).unwrap(),
                    }),
                    cx,
                );
            })
            .unwrap();
        self.draw();
    }
    fn click(&mut self, position: gpui::Point<gpui::Pixels>) {
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

    fn advance(&mut self, millis: u64) {
        self.cx
            .executor()
            .advance_clock(Duration::from_millis(millis));
        self.draw();
    }

    fn caret_painted(&mut self) -> bool {
        self.cx
            .update_window(self.window.into(), |_, window, _| {
                window.rendered_scene().quads.iter().any(|quad| {
                    (quad.bounds.size.width.0 / window.scale_factor() - 1.).abs() < 0.1
                        && quad.bounds.size.height.0 / window.scale_factor() > 10.
                })
            })
            .unwrap()
    }

    fn scrollbar_thumb(&mut self) -> Option<gpui::Bounds<gpui::Pixels>> {
        self.cx
            .update_window(self.window.into(), |_, window, _| {
                let scale = window.scale_factor();
                window.rendered_scene().quads.iter().find_map(|quad| {
                    let bounds = gpui::Bounds::new(
                        point(
                            px(quad.bounds.left().0 / scale),
                            px(quad.bounds.top().0 / scale),
                        ),
                        gpui::size(
                            px(quad.bounds.size.width.0 / scale),
                            px(quad.bounds.size.height.0 / scale),
                        ),
                    );
                    ((3. ..=8.).contains(&f32::from(bounds.size.width))
                        && bounds.size.height >= px(24.))
                    .then_some(bounds)
                })
            })
            .unwrap()
    }

    fn pointer(&mut self, event: PlatformInput) {
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                window.dispatch_event(event, cx);
            })
            .unwrap();
        self.draw();
    }

    fn drag_move(&mut self, position: gpui::Point<gpui::Pixels>) {
        self.pointer(PlatformInput::MouseMove(MouseMoveEvent {
            position,
            pressed_button: Some(MouseButton::Left),
            ..Default::default()
        }));
    }
}

#[test]
fn caret_blinks_resets_on_edit_and_stops_for_selection_blur_and_reduced_motion() {
    let mut f = Fixture::new(false);
    f.edit(|state, window, cx| {
        window.activate_window();
        state.focus(window, cx);
        state.replace_text_in_range(None, "abc", window, cx);
    });
    assert!(f.caret_painted());
    f.advance(499);
    assert!(f.caret_painted());
    f.advance(1);
    assert!(!f.caret_painted());
    f.edit(|state, window, cx| state.replace_text_in_range(None, "d", window, cx));
    assert!(f.caret_painted());
    f.advance(500);
    assert!(!f.caret_painted());
    f.advance(500);
    assert!(f.caret_painted());
    f.edit(|state, window, cx| state.select_all(window, cx));
    assert!(!f.caret_painted());
    f.cx.executor().advance_clock(Duration::from_secs(2));
    assert!(
        !f.cx.executor().tick(),
        "selected input has no runnable blink task"
    );
    f.edit(|state, window, cx| state.select(TextSelection::collapsed(0), window, cx));
    f.cx.update(|cx| cx.set_reduce_motion(true));
    f.draw();
    assert!(f.caret_painted());
    f.cx.executor().advance_clock(Duration::from_secs(2));
    assert!(
        !f.cx.executor().tick(),
        "reduced motion has no runnable blink task"
    );
    f.draw();
    assert!(f.caret_painted());
    f.edit(|state, window, cx| state.blur(window, cx));
    assert!(!f.caret_painted());
}

#[test]
fn caret_is_steady_during_ime_and_hidden_windows_cancel_pending_wakes() {
    let mut f = Fixture::new(false);
    f.edit(|state, window, cx| {
        window.activate_window();
        state.focus(window, cx);
        state.replace_and_mark_text_in_range(None, "ni", Some(2..2), window, cx);
    });
    assert!(f.caret_painted());
    f.advance(700);
    assert!(f.caret_painted());
    f.edit(|state, window, cx| state.replace_text_in_range(None, "你", window, cx));
    f.advance(500);
    assert!(!f.caret_painted());
    f.cx.simulate_window_visibility_change(f.window.into(), gpui::WindowVisibility::Hidden);
    f.cx.run_until_parked();
    f.cx.executor().advance_clock(Duration::from_secs(2));
    assert!(
        !f.cx.executor().tick(),
        "hidden input has no runnable blink task"
    );
    f.cx.simulate_window_visibility_change(f.window.into(), gpui::WindowVisibility::Visible);
    f.draw();
    assert!(f.caret_painted());
}

#[test]
fn selection_drag_autoscrolls_outside_single_line_and_stops_after_release() {
    let mut f = Fixture::new(false);
    f.edit(|state, window, cx| {
        window.activate_window();
        state.set_value("abcdefghij".repeat(60), window, cx);
        state.select(TextSelection::collapsed(0), window, cx);
    });
    let viewport = f
        .state
        .read_with(&f.cx, |state, _| state.geometry().unwrap().viewport);
    let start = point(viewport.left(), viewport.center().y);
    let outside = point(viewport.right() + px(100.), viewport.center().y);
    f.pointer(PlatformInput::MouseDown(MouseDownEvent {
        button: MouseButton::Left,
        position: start,
        modifiers: Default::default(),
        click_count: 1,
        first_mouse: false,
    }));
    f.drag_move(outside);
    let first = f.state.read_with(&f.cx, |state, _| state.selection());
    for _ in 0..12 {
        f.advance(16);
    }
    f.state.read_with(&f.cx, |state, _| {
        assert_eq!(state.selection().anchor, first.anchor);
        assert!(state.selection().caret > first.caret);
        assert!(state.geometry().unwrap().scroll.x > px(100.));
    });
    f.pointer(PlatformInput::MouseUp(MouseUpEvent {
        button: MouseButton::Left,
        position: outside,
        modifiers: Default::default(),
        click_count: 1,
    }));
    let after = f.state.read_with(&f.cx, |state, _| {
        (state.selection(), state.geometry().unwrap().scroll)
    });
    f.advance(1000);
    f.state.read_with(&f.cx, |state, _| {
        assert_eq!((state.selection(), state.geometry().unwrap().scroll), after)
    });
}

#[test]
fn multiline_drag_scrolls_both_directions_and_provider_disable_cancels_it() {
    let mut f = Fixture::new(true);
    f.edit(|state, window, cx| {
        window.activate_window();
        state.set_value(
            (0..60).map(|i| format!("Line {i}\n")).collect::<String>(),
            window,
            cx,
        );
        state.select(TextSelection::collapsed(0), window, cx);
    });
    let viewport = f
        .state
        .read_with(&f.cx, |state, _| state.geometry().unwrap().viewport);
    let start = viewport.origin;
    f.pointer(PlatformInput::MouseDown(MouseDownEvent {
        button: MouseButton::Left,
        position: start,
        modifiers: Default::default(),
        click_count: 1,
        first_mouse: false,
    }));
    f.drag_move(point(
        viewport.left() + px(15.),
        viewport.bottom() + px(100.),
    ));
    for _ in 0..12 {
        f.advance(16);
    }
    let below = f
        .state
        .read_with(&f.cx, |state, _| state.geometry().unwrap().scroll.y);
    assert!(below > px(100.));
    f.drag_move(point(viewport.left(), viewport.top() - px(100.)));
    for _ in 0..4 {
        f.advance(16);
    }
    let above = f
        .state
        .read_with(&f.cx, |state, _| state.geometry().unwrap().scroll.y);
    assert!(above < below);
    let scope = f.consumer.read_with(&f.cx, |view, _| view.scope.clone());
    f.cx.update(|cx| {
        scope
            .update_patch(cx, |patch| patch.disabled = Some(true))
            .unwrap()
    });
    f.draw();
    let stopped = f.state.read_with(&f.cx, |state, _| {
        (state.selection(), state.geometry().unwrap().scroll)
    });
    f.advance(1000);
    f.state.read_with(&f.cx, |state, _| {
        assert_eq!(
            (state.selection(), state.geometry().unwrap().scroll),
            stopped
        )
    });
}

#[test]
fn password_reveal_is_keyboard_accessible_preserves_edits_and_keeps_export_protected() {
    let mut f = Fixture::new(false);
    f.configure(|view| view.mode = state::InputMode::Password);
    f.edit(|state, window, cx| {
        assert!(state.is_masked());
        state.focus(window, cx);
        state.replace_selection("秘密😀", EditOrigin::Typing, window, cx);
        state.select(
            TextSelection {
                anchor: 3,
                caret: 10,
            },
            window,
            cx,
        );
    });
    let eye = f.state.read_with(&f.cx, |state, _| {
        let viewport = state.geometry().unwrap().viewport;
        point(viewport.right() + px(11.), viewport.center().y)
    });
    f.click(eye);
    f.edit(|state, window, cx| {
        assert!(state.password_visible());
        assert!(!state.is_masked());
        assert!(state.is_protected());
        assert!(state.focus.is_focused(window));
        assert_eq!(
            state.selection(),
            TextSelection {
                anchor: 3,
                caret: 10
            }
        );
        cx.write_to_clipboard(gpui::ClipboardItem::new_string("unchanged".to_owned()));
        state.copy(cx);
        assert_eq!(
            cx.read_from_clipboard().unwrap().text().as_deref(),
            Some("unchanged")
        );
    });
    f.key("tab");
    f.key("enter");
    f.key_up("enter");
    f.state
        .read_with(&f.cx, |state, _| assert!(!state.password_visible()));
    assert_eq!(
        f.events
            .borrow()
            .iter()
            .filter(|event| matches!(event, InputEvent::PasswordVisibilityChanged(_)))
            .count(),
        2
    );
    f.key("shift-tab");
    f.key("ctrl-z");
    assert_eq!(f.value(), "");
}

#[test]
fn clear_click_preserves_focus_and_emits_one_undoable_change() {
    let mut f = Fixture::new(false);
    f.configure(|view| view.allow_clear = true);
    f.edit(|state, window, cx| {
        state.set_value("value", window, cx);
        state.focus(window, cx);
    });
    let clear = f.state.read_with(&f.cx, |state, _| {
        let viewport = state.geometry().unwrap().viewport;
        point(viewport.right() + px(10.), viewport.center().y)
    });
    f.events.borrow_mut().clear();
    f.click(clear);
    assert_eq!(f.value(), "");
    assert_eq!(f.events.borrow().iter().filter(|event| matches!(event, InputEvent::Change(change) if change.origin == EditOrigin::Clear)).count(), 1);
    f.edit(|state, window, _| assert!(state.focus.is_focused(window)));
    f.key("ctrl-z");
    assert_eq!(f.value(), "value");
}

#[test]
fn search_sources_are_exactly_once_and_composition_and_loading_suppress_activation() {
    let mut f = Fixture::new(false);
    f.configure(|view| {
        view.mode = state::InputMode::Search;
        view.allow_clear = true;
    });
    f.edit(|state, window, cx| {
        state.set_value("query", window, cx);
        state.focus(window, cx);
    });
    f.key("enter");
    // The joined action occupies the right end of this 300px control.
    f.click(point(px(295.), px(26.)));
    f.edit(|state, window, cx| state.clear(window, cx));
    let sources: Vec<_> = f
        .events
        .borrow()
        .iter()
        .filter_map(|event| {
            if let InputEvent::Search(search) = event {
                Some((search.source, search.value.to_string()))
            } else {
                None
            }
        })
        .collect();
    assert_eq!(
        sources,
        [
            (SearchSource::Enter, "query".to_owned()),
            (SearchSource::Action, "query".to_owned()),
            (SearchSource::Clear, "".to_owned())
        ]
    );
    f.configure(|view| view.loading = true);
    f.edit(|state, window, cx| state.focus(window, cx));
    f.key("enter");
    f.click(point(px(295.), px(26.)));
    f.configure(|view| view.loading = false);
    f.edit(|state, window, cx| {
        state.focus(window, cx);
        state.replace_and_mark_text_in_range(None, "ni", Some(2..2), window, cx);
    });
    f.key("enter");
    assert_eq!(
        f.events
            .borrow()
            .iter()
            .filter(|event| matches!(event, InputEvent::Search(_)))
            .count(),
        3
    );
}

#[test]
fn affixes_and_count_reserve_viewport_space_and_hard_limits_use_graphemes() {
    let mut f = Fixture::new(false);
    let before = f
        .state
        .read_with(&f.cx, |state, _| state.geometry().unwrap().viewport);
    f.configure(|view| view.adornments = true);
    let after = f
        .state
        .read_with(&f.cx, |state, _| state.geometry().unwrap().viewport);
    assert!(after.left() > before.left());
    assert!(after.right() < before.right());
    f.edit(|state, window, cx| {
        state.set_max_length(Some(2), cx);
        state.replace_selection("👩🏽‍💻e\u{301}中", EditOrigin::Paste, window, cx);
        assert_eq!(state.character_count(), 2);
    });
    assert_eq!(f.value(), "👩🏽‍💻e\u{301}");
    let config = CountConfig::new().maximum(1);
    assert_eq!(
        config
            .format(CountInfo {
                value: "👩🏽‍💻e\u{301}",
                count: 2,
                maximum: config.maximum
            })
            .as_ref(),
        "2 / 1"
    );
    let custom = config.formatter(|info| format!("{} characters", info.count).into());
    assert_eq!(
        custom
            .format(CountInfo {
                value: "",
                count: 0,
                maximum: None
            })
            .as_ref(),
        "0 characters"
    );
}

#[test]
fn state_disabled_setting_survives_render_and_can_be_overridden_explicitly() {
    let mut f = Fixture::new(false);
    f.edit(|state, window, cx| state.set_disabled(true, window, cx));
    f.state
        .read_with(&f.cx, |state, _| assert!(state.is_disabled()));
    f.configure(|view| view.disabled = Some(false));
    f.state
        .read_with(&f.cx, |state, _| assert!(!state.is_disabled()));
    f.configure(|view| view.disabled = None);
    f.state
        .read_with(&f.cx, |state, _| assert!(state.is_disabled()));
}

#[test]
fn application_can_rebind_context_scoped_input_actions() {
    let mut f = Fixture::new(false);
    f.cx.update(|cx| {
        cx.bind_keys([gpui::KeyBinding::new(
            "f6",
            actions::SelectAll,
            Some("AntDesignInput"),
        )])
    });
    f.edit(|state, window, cx| {
        state.set_value("abcd", window, cx);
        state.focus(window, cx);
    });
    f.key("f6");
    f.state.read_with(&f.cx, |state, _| {
        assert_eq!(state.selection().range(), 0..4)
    });
}

#[test]
fn rtl_alignment_updates_each_wrapped_row_selection_and_candidate_geometry() {
    let mut f = Fixture::new(true);
    f.edit(|state, window, cx| {
        state.set_value(
            "短行\none two three four five six seven eight nine ten eleven twelve",
            window,
            cx,
        );
        state.select(TextSelection::collapsed(0), window, cx);
        state.focus(window, cx);
    });
    let previous = f
        .state
        .read_with(&f.cx, |state, _| state.geometry_snapshot().unwrap());
    let scope = f.consumer.read_with(&f.cx, |view, _| view.scope.clone());
    f.cx.update(|cx| {
        scope
            .update_patch(cx, |patch| patch.direction = Some(crate::Direction::Rtl))
            .unwrap()
    });
    f.draw();
    let (start, end) = f.state.read_with(&f.cx, |state, _| {
        let g = state.geometry().unwrap();
        assert!(!Rc::ptr_eq(&previous, &state.geometry_snapshot().unwrap()));
        assert_eq!(state.selection(), TextSelection::collapsed(0));
        let mut rows = g.carets.iter().map(|c| c.position.y).collect::<Vec<_>>();
        rows.dedup();
        assert!(rows.len() >= 3);
        for row in rows {
            let right = g
                .carets
                .iter()
                .filter(|c| c.position.y == row)
                .map(|c| c.position.x)
                .max()
                .unwrap();
            assert!((right - g.viewport.size.width).abs() < px(0.01));
        }
        let start = g.window_point(g.caret(0).unwrap());
        let end = g.window_point(g.caret("短行".len()).unwrap());
        assert!(start.x > g.viewport.left());
        assert!((end.x - g.viewport.right()).abs() < px(0.01));
        let selected = g.selection_rects(0.."短行".len());
        assert_eq!(selected.len(), 1);
        assert_eq!(selected[0].left(), start.x);
        // Rectangle width addition and caret positioning may round differently.
        assert!((selected[0].right() - end.x).abs() < px(0.01));
        (start, end)
    });
    f.click(start + point(px(1.), px(2.)));
    assert_eq!(f.state.read_with(&f.cx, |s, _| s.selection().caret), 0);
    f.key("end");
    f.edit(|state, window, cx| {
        assert_eq!(state.selection().caret, "短行".len());
        // GPUI's native bridge requests UTF-16 offsets.
        let bounds = state
            .bounds_for_range(2..2, Default::default(), window, cx)
            .unwrap();
        assert_eq!(bounds.origin, end);
    });
    f.cx.update(|cx| {
        scope
            .update_patch(cx, |patch| patch.direction = Some(crate::Direction::Ltr))
            .unwrap()
    });
    f.draw();
    f.state.read_with(&f.cx, |state, _| {
        assert_eq!(state.geometry().unwrap().caret(0).unwrap().x, px(0.))
    });
}

#[test]
fn empty_rtl_placeholder_keeps_the_native_caret_visible_inside_the_right_edge() {
    let mut f = Fixture::new(false);
    let scope = f.consumer.read_with(&f.cx, |view, _| view.scope.clone());
    f.cx.update(|cx| {
        scope
            .update_patch(cx, |patch| patch.direction = Some(crate::Direction::Rtl))
            .unwrap()
    });
    f.edit(|state, window, cx| {
        window.activate_window();
        state.focus(window, cx);
    });
    let viewport = f.state.read_with(&f.cx, |state, _| {
        let g = state.geometry().unwrap();
        assert!(g.clusters.is_empty());
        assert_eq!(g.caret(0).unwrap().x, g.viewport.size.width);
        g.viewport
    });
    assert!(f.caret_painted());
    f.cx.update_window(f.window.into(), |_, window, _| {
        let scale = window.scale_factor();
        assert!(window.rendered_scene().quads.iter().any(|quad| {
            (quad.bounds.size.width.0 / scale - 1.).abs() < 0.1
                && quad.bounds.size.height.0 / scale > 10.
                && (quad.bounds.right().0 / scale - f32::from(viewport.right())).abs() < 0.01
        }));
    })
    .unwrap();
}

#[test]
fn compact_rtl_textarea_reserves_the_full_clear_hit_area() {
    let mut f = Fixture::new(true);
    let scope = f.consumer.read_with(&f.cx, |view, _| view.scope.clone());
    let (padding, icon, top) = f.cx.update(|cx| {
        scope
            .update_patch(cx, |patch| {
                patch.direction = Some(crate::Direction::Rtl);
                patch.theme = Some(crate::theme::ThemeConfig {
                    algorithms: Some(crate::theme::AlgorithmChain::compact()),
                    ..Default::default()
                });
            })
            .unwrap();
        let config = scope.snapshot(cx);
        let tokens = config.theme.component::<InputTheme>();
        (
            tokens.padding_inline,
            config.theme.tokens.font_size_icon,
            config.theme.tokens.padding_xs,
        )
    });
    f.configure(|view| view.allow_clear = true);
    f.edit(|state, window, cx| state.set_value("First line\n第二行", window, cx));
    f.state.read_with(&f.cx, |state, _| {
        assert!(
            f32::from(state.geometry().unwrap().viewport.left())
                >= (10. + padding + icon + 1.) as f32
        );
    });
    f.click(point(
        px((11. + padding + icon / 2.) as f32),
        px((11. + top + icon / 2.) as f32),
    ));
    assert_eq!(f.value(), "");
}

#[test]
fn horizontal_navigation_uses_logical_graphemes_in_both_layout_directions() {
    use unicode_segmentation::UnicodeSegmentation;

    for direction in [crate::Direction::Ltr, crate::Direction::Rtl] {
        let mut f = Fixture::new(false);
        let scope = f.consumer.read_with(&f.cx, |view, _| view.scope.clone());
        f.cx.update(|cx| {
            scope
                .update_patch(cx, |patch| patch.direction = Some(direction))
                .unwrap();
        });
        f.draw();
        for text in ["אבג", "abc אבג xyz", "لا", "中文 e\u{301} 👩🏽‍💻"] {
            // Several commands in one native event batch, without intermediate draws.
            f.edit(|state, window, cx| {
                state.set_value(text, window, cx);
                state.select(TextSelection::collapsed(0), window, cx);
                let boundaries: Vec<_> = text
                    .grapheme_indices(true)
                    .map(|(index, _)| index)
                    .chain([text.len()])
                    .collect();
                state.move_horizontal(false, false, false, window, cx);
                assert_eq!(state.selection(), TextSelection::collapsed(0));
                for &offset in boundaries.iter().skip(1) {
                    state.move_horizontal(true, false, false, window, cx);
                    assert_eq!(state.selection(), TextSelection::collapsed(offset));
                }
                state.move_horizontal(true, false, false, window, cx);
                assert_eq!(state.selection(), TextSelection::collapsed(text.len()));
                for &offset in boundaries.iter().rev().skip(1) {
                    state.move_horizontal(false, false, false, window, cx);
                    assert_eq!(state.selection(), TextSelection::collapsed(offset));
                }
                state.move_horizontal(true, true, false, window, cx);
                assert_eq!(
                    state.selection(),
                    TextSelection {
                        anchor: 0,
                        caret: boundaries[1]
                    }
                );
                state.move_horizontal(false, false, false, window, cx);
                assert_eq!(state.selection(), TextSelection::collapsed(0));
                state.select_all(window, cx);
                state.move_horizontal(true, false, false, window, cx);
                assert_eq!(state.selection(), TextSelection::collapsed(text.len()));
            });
        }
        f.edit(|state, window, cx| {
            state.set_value("אבג", window, cx);
            state.select(TextSelection::collapsed(0), window, cx);
            state.move_horizontal(true, true, true, window, cx);
            assert_eq!(
                state.selection(),
                TextSelection {
                    anchor: 0,
                    caret: "אבג".len()
                }
            );
            state.move_horizontal(false, false, true, window, cx);
            assert_eq!(state.selection(), TextSelection::collapsed(0));
        });
        assert!(
            !f.events
                .borrow()
                .iter()
                .any(|event| matches!(event, InputEvent::Change(_))),
            "navigation must not emit value changes"
        );
    }
}

#[test]
fn external_values_do_not_emit_user_changes_and_native_edits_do() {
    let mut f = Fixture::new(false);
    f.edit(|state, window, cx| state.set_value("a", window, cx));
    assert!(f.events.borrow().is_empty());
    f.edit(|state, window, cx| {
        state.select_all(window, cx);
        state.replace_text_in_range(None, "你好", window, cx);
    });
    assert_eq!(f.value(), "你好");
    let changes: Vec<_> = f
        .events
        .borrow()
        .iter()
        .filter_map(|event| {
            if let InputEvent::Change(change) = event {
                Some(change.clone())
            } else {
                None
            }
        })
        .collect();
    assert_eq!(changes.len(), 1);
    assert_eq!(changes[0].value.as_ref(), "你好");
    assert_eq!(changes[0].origin, EditOrigin::Typing);
}

#[test]
fn native_composition_does_not_commit_and_undo_restores_before_composition() {
    let mut f = Fixture::new(false);
    f.edit(|state, window, cx| {
        state.focus(window, cx);
        state.replace_and_mark_text_in_range(None, "nihao", Some(5..5), window, cx);
    });
    f.key("enter");
    assert!(
        !f.events
            .borrow()
            .iter()
            .any(|event| matches!(event, InputEvent::Commit(_)))
    );
    f.edit(|state, window, cx| state.replace_text_in_range(None, "你好", window, cx));
    assert_eq!(f.value(), "你好");
    f.key("ctrl-z");
    assert_eq!(f.value(), "");
    f.key("ctrl-y");
    assert_eq!(f.value(), "你好");
    f.key("enter");
    assert_eq!(
        f.events
            .borrow()
            .iter()
            .filter(|event| matches!(event, InputEvent::Commit(_)))
            .count(),
        1
    );
}

#[test]
fn read_only_allows_selection_and_copy_but_rejects_paste_delete_and_native_edits() {
    let mut f = Fixture::new(false);
    f.edit(|state, window, cx| {
        state.set_value("readonly", window, cx);
        state.focus(window, cx);
        state.set_read_only(true, window, cx);
        state.select_all(window, cx);
        state.copy(cx);
        assert_eq!(
            cx.read_from_clipboard().unwrap().text().as_deref(),
            Some("readonly")
        );
        state.replace_text_in_range(None, "changed", window, cx);
        state.paste_clipboard(window, cx);
    });
    f.key("backspace");
    f.key("ctrl-x");
    assert_eq!(f.value(), "readonly");
}

#[test]
fn password_masking_preserves_selection_and_does_not_copy_plaintext() {
    let mut f = Fixture::new(false);
    f.edit(|state, window, cx| {
        state.replace_selection("secret", EditOrigin::Typing, window, cx);
        state.select_all(window, cx);
        let selection = state.selection();
        state.set_masked(true, window, cx);
        assert_eq!(state.selection(), selection);
        cx.write_to_clipboard(gpui::ClipboardItem::new_string("before".to_owned()));
        state.copy(cx);
        state.cut(window, cx);
        assert_eq!(
            cx.read_from_clipboard().unwrap().text().as_deref(),
            Some("before")
        );
        assert_eq!(state.value(), "secret");
        state.set_masked(false, window, cx);
        state.undo(window, cx);
        assert_eq!(state.value(), "");
    });
}

#[test]
fn multiline_enter_inserts_line_break_and_control_enter_commits() {
    let mut f = Fixture::new(true);
    f.edit(|state, window, cx| state.focus(window, cx));
    f.key("enter");
    assert_eq!(f.value(), "\n");
    assert!(
        !f.events
            .borrow()
            .iter()
            .any(|event| matches!(event, InputEvent::Commit(_)))
    );
    f.key("ctrl-enter");
    assert_eq!(
        f.events
            .borrow()
            .iter()
            .filter(|event| matches!(event, InputEvent::Commit(_)))
            .count(),
        1
    );
}

#[test]
fn auto_height_tracks_newlines_and_clamps_at_maximum_rows() {
    let mut f = Fixture::new(true);
    f.consumer.update(&mut f.cx, |view, cx| {
        view.auto_size = true;
        cx.notify();
    });
    f.draw();
    let initial = f.state.read_with(&f.cx, |state, _| {
        state.geometry().unwrap().viewport.size.height
    });
    f.edit(|state, window, cx| state.set_value("one\ntwo\nthree", window, cx));
    let three = f.state.read_with(&f.cx, |state, _| {
        state.geometry().unwrap().viewport.size.height
    });
    assert!(three > initial);
    f.edit(|state, window, cx| state.set_value("1\n2\n3\n4\n5\n6", window, cx));
    f.state.read_with(&f.cx, |state, _| {
        let geometry = state.geometry().unwrap();
        assert!((f32::from(geometry.viewport.size.height - geometry.line_height * 4.)).abs() < 0.1);
    });
}

#[test]
fn textarea_scrollbar_tracks_overflow_wheel_and_content_shrink() {
    let mut f = Fixture::new(true);
    assert!(f.scrollbar_thumb().is_none());
    f.edit(|state, window, cx| {
        state.set_value("Line\n".repeat(20), window, cx);
        state.select(TextSelection::collapsed(0), window, cx);
    });
    let initial = f
        .scrollbar_thumb()
        .expect("overflow must display a scrollbar");
    let position = f.state.read_with(&f.cx, |state, _| {
        state.geometry().unwrap().viewport.center()
    });
    f.pointer(PlatformInput::MouseMove(MouseMoveEvent {
        position,
        ..Default::default()
    }));
    f.pointer(PlatformInput::Scroll(gpui::ScrollEvent {
        position,
        delta: gpui::ScrollDelta::Pixels(point(px(0.), px(-100.))),
        ..Default::default()
    }));
    assert_eq!(
        f.state
            .read_with(&f.cx, |state, _| state.geometry().unwrap().scroll.y),
        px(100.)
    );
    assert!(f.scrollbar_thumb().unwrap().top() > initial.top());
    assert_eq!(
        f.consumer.read_with(&f.cx, |c, _| *c.outer_wheel.borrow()),
        0
    );
    for _ in 0..2 {
        f.pointer(PlatformInput::Scroll(gpui::ScrollEvent {
            position,
            delta: gpui::ScrollDelta::Pixels(point(px(0.), px(-10000.))),
            ..Default::default()
        }));
    }
    assert_eq!(
        f.consumer.read_with(&f.cx, |c, _| *c.outer_wheel.borrow()),
        1
    );
    f.edit(|state, window, cx| state.set_value("Short", window, cx));
    assert!(f.scrollbar_thumb().is_none());
    f.state.read_with(&f.cx, |state, _| {
        assert_eq!(state.geometry().unwrap().scroll, point(px(0.), px(0.)));
    });
}

#[test]
fn textarea_preserves_the_full_padded_editing_width() {
    let mut f = Fixture::new(false);
    let single_line = f
        .state
        .read_with(&f.cx, |state, _| state.geometry().unwrap().viewport);
    f.configure(|view| view.multiline = true);
    for value in ["Short".to_owned(), "1".repeat(400)] {
        f.edit(|state, window, cx| state.set_value(value, window, cx));
        f.state.read_with(&f.cx, |state, _| {
            let viewport = state.geometry().unwrap().viewport;
            assert_eq!(viewport.left(), single_line.left());
            assert_eq!(viewport.right(), single_line.right());
            assert_eq!(viewport.size.width, px(276.));
        });
    }
}

#[test]
fn textarea_scrollbar_drag_preserves_focus_selection_and_native_geometry() {
    for direction in [crate::Direction::Ltr, crate::Direction::Rtl] {
        let mut f = Fixture::new(true);
        let scope = f.consumer.read_with(&f.cx, |view, _| view.scope.clone());
        f.cx.update(|cx| {
            scope
                .update_patch(cx, |patch| {
                    patch.direction = Some(direction);
                    patch.motion = Some(false);
                })
                .unwrap();
        });
        f.edit(|state, window, cx| {
            window.activate_window();
            state.set_value("Line\n".repeat(20), window, cx);
            state.focus(window, cx);
            state.select(TextSelection::collapsed(0), window, cx);
        });
        let thumb = f.scrollbar_thumb().unwrap();
        let viewport = f
            .state
            .read_with(&f.cx, |state, _| state.geometry().unwrap().viewport);
        if direction == crate::Direction::Ltr {
            assert!(thumb.left() >= viewport.right());
            // Anchor to the inner frame edge, independently of text padding:
            // frame right (310) - border (1) - scrollbar inset (2).
            assert_eq!(thumb.right(), px(307.));
        } else {
            assert!(thumb.right() <= viewport.left());
            assert_eq!(thumb.left(), px(13.));
        }
        let start = thumb.center();
        f.pointer(PlatformInput::MouseMove(MouseMoveEvent {
            position: start,
            ..Default::default()
        }));
        let expanded = f.scrollbar_thumb().unwrap();
        if direction == crate::Direction::Ltr {
            assert_eq!(expanded.right(), thumb.right());
        } else {
            assert_eq!(expanded.left(), thumb.left());
        }
        f.pointer(PlatformInput::MouseDown(MouseDownEvent {
            position: start,
            button: MouseButton::Left,
            click_count: 1,
            ..Default::default()
        }));
        let end = point(start.x, viewport.bottom() + px(100.));
        f.drag_move(end);
        f.pointer(PlatformInput::MouseUp(MouseUpEvent {
            position: end,
            button: MouseButton::Left,
            ..Default::default()
        }));
        f.edit(|state, window, cx| {
            assert!(state.focus.is_focused(window));
            assert_eq!(state.selection(), TextSelection::collapsed(0));
            assert!(state.geometry().unwrap().scroll.y > px(300.));
            let geometry = state.geometry().unwrap();
            let expected = geometry.caret_bounds(state.caret_location()).unwrap();
            let actual = state
                .bounds_for_range(0..0, Default::default(), window, cx)
                .unwrap();
            assert_eq!(actual, expected);
        });
        // Editor navigation still owns the arrow keys and reveals the caret.
        f.key("down");
        f.state.read_with(&f.cx, |state, _| {
            assert!(state.selection().caret > 0);
            let geometry = state.geometry().unwrap();
            let caret = geometry.caret_bounds(state.caret_location()).unwrap();
            assert!(geometry.scroll.y <= geometry.line_height);
            assert!(caret.top() >= geometry.viewport.top());
            assert!(caret.bottom() <= geometry.viewport.bottom());
        });
        let selection = f.state.read_with(&f.cx, |state, _| state.selection());
        f.click(point(start.x, viewport.bottom() - px(3.)));
        f.edit(|state, window, _| {
            assert!(state.focus.is_focused(window));
            assert_eq!(state.selection(), selection);
            assert!(state.geometry().unwrap().scroll.y > px(40.));
        });
    }
}

#[test]
fn textarea_scrollbars_follow_wrapping_auto_size_and_explicit_height() {
    let mut f = Fixture::new(true);
    f.configure(|view| view.auto_size = true);
    f.edit(|state, window, cx| state.set_value("one\ntwo\nthree", window, cx));
    assert!(f.scrollbar_thumb().is_none());
    f.edit(|state, window, cx| state.set_value("wrapped words ".repeat(60), window, cx));
    let initial = f.scrollbar_thumb().unwrap();
    f.state.read_with(&f.cx, |state, _| {
        let geometry = state.geometry().unwrap();
        assert_eq!(geometry.viewport.size.height, geometry.line_height * 4.);
    });
    f.edit(|state, window, cx| {
        state.select(TextSelection::collapsed(state.value().len()), window, cx)
    });
    assert!(f.scrollbar_thumb().unwrap().top() > initial.top());
    f.configure(|view| {
        view.auto_size = false;
        view.height = Some(180.);
    });
    f.state.read_with(&f.cx, |state, _| {
        assert!(state.geometry().unwrap().viewport.size.height > px(150.));
    });
    assert!(f.scrollbar_thumb().is_some());
    f.configure(|view| {
        view.adornments = true;
        view.allow_clear = true;
    });
    f.state.read_with(&f.cx, |state, _| {
        assert!(state.geometry().unwrap().viewport.size.height > px(120.));
    });
    assert!(f.scrollbar_thumb().is_some());
    f.configure(|view| view.height = Some(1600.));
    assert!(f.scrollbar_thumb().is_none());
}

#[test]
fn stale_geometry_is_replaced_with_current_geometry_for_native_queries() {
    let mut f = Fixture::new(false);
    f.edit(|state, window, cx| {
        let old = state.geometry_snapshot().unwrap();
        state.replace_text_in_range(None, "text", window, cx);
        assert!(state.geometry().is_none());
        assert!(
            state
                .bounds_for_range(0..0, Default::default(), window, cx)
                .is_some()
        );
        let new = state.geometry_snapshot().unwrap();
        assert_ne!(old.revision, new.revision);
        assert_eq!(new.revision, state.buffer.revision());
        assert_eq!(new.clusters.last().unwrap().range.end, 4);
    });
}

#[test]
fn wrapped_row_affinity_drives_pointer_home_end_and_native_candidate_bounds() {
    use super::geometry::{CaretAffinity, CaretLocation};
    let mut f = Fixture::new(true);
    f.edit(|state, window, cx| {
        state.set_value(
            "one two three four five six seven eight nine ten ".repeat(12),
            window,
            cx,
        );
        state.select(
            TextSelection {
                anchor: 0,
                caret: 0,
            },
            window,
            cx,
        );
        state.focus(window, cx);
    });
    let (offset, end, start) = f.state.read_with(&f.cx, |state, _| {
        let geometry = state.geometry().unwrap();
        let pair = geometry
            .carets
            .windows(2)
            .find(|pair| {
                pair[0].offset == pair[1].offset && pair[0].position.y != pair[1].position.y
            })
            .expect("soft wrap has two visual positions");
        assert_eq!(pair[0].position.y, px(0.));
        assert_eq!(pair[1].position.x, px(0.));
        let last = geometry
            .clusters
            .iter()
            .find(|cluster| cluster.range.end == pair[0].offset)
            .unwrap();
        assert_eq!(last.bounds.right(), pair[0].position.x);
        (
            pair[0].offset,
            geometry.window_point(pair[0].position),
            geometry.window_point(pair[1].position),
        )
    });
    f.click(end + point(px(4.), px(3.)));
    f.edit(|state, window, cx| {
        assert_eq!(
            state.caret_location(),
            CaretLocation {
                offset,
                affinity: CaretAffinity::Upstream
            }
        );
        let bounds = state
            .bounds_for_range(offset..offset, Default::default(), window, cx)
            .unwrap();
        assert_eq!(bounds.origin, end);
        let selections =
            state.selection_bounds_for_range(offset..offset, Default::default(), window, cx);
        assert_eq!(selections, [bounds]);
    });
    f.key("shift-left");
    f.state.read_with(&f.cx, |state, _| {
        assert_eq!(
            state.anchor_location(),
            CaretLocation {
                offset,
                affinity: CaretAffinity::Upstream
            }
        );
        assert_eq!(state.selection().caret, offset - 1);
    });
    f.click(end + point(px(4.), px(3.)));
    f.key("home");
    assert_eq!(
        f.state.read_with(&f.cx, |state, _| state.selection().caret),
        0
    );
    f.key("shift-end");
    f.state.read_with(&f.cx, |state, _| {
        assert_eq!(
            state.selection(),
            TextSelection {
                anchor: 0,
                caret: offset
            }
        );
        assert_eq!(state.caret_location().affinity, CaretAffinity::Upstream);
    });
    f.click(start + point(px(0.), px(3.)));
    f.state.read_with(&f.cx, |state, _| {
        assert_eq!(
            state.caret_location(),
            CaretLocation {
                offset,
                affinity: CaretAffinity::Downstream
            }
        );
        assert_eq!(
            state
                .geometry()
                .unwrap()
                .caret_bounds(state.caret_location())
                .unwrap()
                .origin,
            start
        );
    });
    f.key("home");
    assert_eq!(
        f.state.read_with(&f.cx, |state, _| state.selection().caret),
        offset
    );
    f.key("up");
    assert_eq!(
        f.state.read_with(&f.cx, |state, _| state.selection().caret),
        0
    );
    f.key("down");
    assert_eq!(
        f.state.read_with(&f.cx, |state, _| state.caret_location()),
        CaretLocation {
            offset,
            affinity: CaretAffinity::Downstream
        }
    );
}

#[test]
fn unchanged_redraw_reuses_geometry_and_edits_and_resize_replace_it() {
    let mut f = Fixture::new(true);
    f.edit(|state, window, cx| state.set_value("Long notes 中文\n".repeat(7000), window, cx));
    let before = f
        .state
        .read_with(&f.cx, |state, _| state.geometry_snapshot().unwrap());
    f.configure(|_| {});
    let after = f
        .state
        .read_with(&f.cx, |state, _| state.geometry_snapshot().unwrap());
    assert!(Rc::ptr_eq(&before, &after));
    f.edit(|state, window, cx| {
        state.replace_range(Some(0..4), "Updated", EditOrigin::Typing, window, cx);
    });
    let changed = f
        .state
        .read_with(&f.cx, |state, _| state.geometry_snapshot().unwrap());
    assert!(!Rc::ptr_eq(&after, &changed));
    assert_ne!(changed.revision, after.revision);
    f.configure(|view| view.adornments = true);
    let resized = f
        .state
        .read_with(&f.cx, |state, _| state.geometry_snapshot().unwrap());
    assert!(!Rc::ptr_eq(&changed, &resized));
    assert!(resized.viewport.size.width < changed.viewport.size.width);
}

#[cfg(target_os = "windows")]
#[test]
fn windows_empty_preedit_sentinel_cancels_and_restores_selected_text() {
    let mut f = Fixture::new(false);
    f.edit(|state, window, cx| {
        state.set_value("原文", window, cx);
        state.select_all(window, cx);
        let selected = state.selection();
        state.replace_and_mark_text_in_range(None, "ni", Some(2..2), window, cx);
        assert!(state.is_composing());
        state.replace_and_mark_text_in_range(None, "", None, window, cx);
        assert!(!state.is_composing());
        assert_eq!(state.value(), "原文");
        assert_eq!(state.selection(), selected);
        assert!(!state.can_undo());
        // Redundant cleanup after cancellation must not create an empty session.
        state.replace_and_mark_text_in_range(None, "", None, window, cx);
        assert!(!state.is_composing());
        // A new session after a temporary empty update still replaces the same
        // selection and commits as one transaction.
        state.replace_and_mark_text_in_range(None, "hao", Some(3..3), window, cx);
        state.replace_text_in_range(None, "好", window, cx);
        assert_eq!(state.value(), "好");
        state.undo(window, cx);
        assert_eq!(state.value(), "原文");
        assert_eq!(state.selection(), selected);
    });
}

#[test]
fn search_input_and_action_share_height_at_fractional_scale() {
    let mut f = Fixture::new(false);
    f.cx.update_window(f.window.into(), |_, window, _| window.set_scale_factor(1.5))
        .unwrap();
    f.configure(|view| view.mode = state::InputMode::Search);
    f.cx.update_window(f.window.into(), |_, window, _| {
        let scale = window.scale_factor();
        let input = window
            .rendered_scene()
            .quads
            .iter()
            .find(|quad| {
                (quad.bounds.left().0 / scale - 10.).abs() < 0.1
                    && quad.bounds.size.height.0 / scale > 20.
                    && quad.bounds.size.height.0 / scale < 40.
                    && quad.background.as_solid().is_some_and(|color| color.a > 0.)
            })
            .expect("input frame");
        assert!((input.bounds.size.height.0 / scale - 32.).abs() < 0.1);
    })
    .unwrap();
}

#[test]
fn clear_action_colors_animate_and_pointer_release_outside_does_not_clear() {
    let mut f = Fixture::new(false);
    f.configure(|view| view.allow_clear = true);
    f.edit(|state, window, cx| {
        window.activate_window();
        state.set_value("value", window, cx);
    });
    let clear = f.state.read_with(&f.cx, |state, _| {
        let bounds = state.geometry().unwrap().viewport;
        point(bounds.right() + px(10.), bounds.center().y)
    });
    let alpha = |f: &mut Fixture| {
        f.cx.update_window(f.window.into(), |_, window, _| {
            window
                .rendered_scene()
                .monochrome_sprites
                .last()
                .expect("clear icon")
                .color
                .a
        })
        .unwrap()
    };
    let rest = alpha(&mut f);
    f.pointer(PlatformInput::MouseMove(MouseMoveEvent {
        position: clear,
        ..Default::default()
    }));
    assert!((alpha(&mut f) - rest).abs() < 0.01);
    f.advance(150);
    let middle = alpha(&mut f);
    f.advance(150);
    let hover = alpha(&mut f);
    assert!(
        middle > rest && middle < hover,
        "{rest} < {middle} < {hover}"
    );
    f.pointer(PlatformInput::MouseDown(MouseDownEvent {
        button: MouseButton::Left,
        position: clear,
        modifiers: Default::default(),
        click_count: 1,
        first_mouse: false,
    }));
    f.advance(300);
    assert!(alpha(&mut f) > hover);
    let outside = point(px(-10.), px(-10.));
    f.drag_move(outside);
    f.pointer(PlatformInput::MouseUp(MouseUpEvent {
        button: MouseButton::Left,
        position: outside,
        modifiers: Default::default(),
        click_count: 1,
    }));
    f.advance(300);
    assert!((alpha(&mut f) - rest).abs() < 0.01);
    assert_eq!(f.value(), "value");
    // Consume the frame already requested before the manual clock jump; it must
    // not schedule another frame once the transition is settled.
    f.cx.update_window(f.window.into(), |_, window, cx| {
        window.simulate_next_frame(cx);
    })
    .unwrap();
    f.draw();
    f.cx.update_window(f.window.into(), |_, window, cx| {
        assert_eq!(window.simulate_next_frame(cx), 0)
    })
    .unwrap();
    f.pointer(PlatformInput::MouseMove(MouseMoveEvent {
        position: clear,
        ..Default::default()
    }));
    f.pointer(PlatformInput::MouseDown(MouseDownEvent {
        button: MouseButton::Left,
        position: clear,
        modifiers: Default::default(),
        click_count: 1,
        first_mouse: false,
    }));
    f.edit(|state, window, cx| {
        state.set_read_only(true, window, cx);
        state.set_read_only(false, window, cx);
    });
    f.pointer(PlatformInput::MouseUp(MouseUpEvent {
        button: MouseButton::Left,
        position: clear,
        modifiers: Default::default(),
        click_count: 1,
    }));
    assert_eq!(
        f.value(),
        "value",
        "availability round trip cancels the original press"
    );
}

#[test]
fn frame_colors_animate_without_reshaping_text_and_reduced_motion_stops_frames() {
    let mut f = Fixture::new(true);
    f.edit(|state, window, cx| {
        window.activate_window();
        state.set_value("Unicode 中文 e\u{301} 😀 ".repeat(500), window, cx);
    });
    let geometry = f
        .state
        .read_with(&f.cx, |state, _| state.geometry_snapshot().unwrap());
    let background = |f: &mut Fixture| {
        f.cx.update_window(f.window.into(), |_, window, _| {
            let frame = window
                .rendered_scene()
                .quads
                .iter()
                .find(|q| {
                    (q.bounds.left().0 / window.scale_factor() - 10.).abs() < 0.1
                        && q.bounds.size.width.0 > 100.
                        && q.background.as_solid().is_some_and(|c| c.a > 0.)
                })
                .expect("input frame");
            frame.background.as_solid().unwrap().a
        })
        .unwrap()
    };
    let rest = background(&mut f);
    f.edit(|state, window, cx| state.set_disabled(true, window, cx));
    assert_eq!(rest, background(&mut f));
    f.advance(100);
    let middle = background(&mut f);
    assert!(
        middle > 0.04 && middle < rest,
        "rest={rest}, middle={middle}"
    );
    f.state.read_with(&f.cx, |state, _| {
        assert!(
            Rc::ptr_eq(&geometry, &state.geometry_snapshot().unwrap()),
            "color-only animation must reuse geometry and shaped text"
        );
    });
    f.cx.update(|cx| cx.set_reduce_motion(true));
    f.draw();
    assert!((background(&mut f) - 0.04).abs() < 0.001);
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
fn password_action_cancels_keyboard_press_across_disable_enable() {
    let mut f = Fixture::new(false);
    f.configure(|view| view.mode = state::InputMode::Password);
    f.edit(|state, window, cx| {
        state.set_value("secret", window, cx);
        state.focus(window, cx);
    });
    f.key("tab");
    f.key("space");
    f.state
        .read_with(&f.cx, |state, _| assert!(!state.password_visible()));
    f.edit(|state, window, cx| {
        state.set_disabled(true, window, cx);
        state.set_disabled(false, window, cx);
    });
    f.key_up("space");
    f.state
        .read_with(&f.cx, |state, _| assert!(!state.password_visible()));
    f.edit(|state, window, cx| state.focus(window, cx));
    f.key("tab");
    f.key("space");
    f.key_up("space");
    f.state
        .read_with(&f.cx, |state, _| assert!(state.password_visible()));
}

#[test]
fn password_actions_use_component_font_palette_and_motion_overrides() {
    use crate::theme::{Color, ComponentThemeConfig, ThemeConfig, TokenPatch};
    let mut f = Fixture::new(false);
    f.configure(|view| view.mode = state::InputMode::Password);
    let scope = f.consumer.read_with(&f.cx, |view, _| view.scope.clone());
    f.cx.update(|cx| {
        scope
            .update_patch(cx, |patch| {
                let mut theme = ThemeConfig::default();
                theme.components.set_theme(
                    ComponentThemeConfig::<InputTheme>::new(InputTokenPatch {
                        input_font_size_lg: Some(28.),
                        ..Default::default()
                    })
                    .with_tokens(TokenPatch {
                        color_icon: Some(Color::rgb(0xff0000)),
                        color_icon_hover: Some(Color::rgb(0x00ff00)),
                        motion: Some(false),
                        ..Default::default()
                    }),
                );
                patch.theme = Some(theme);
                patch.size = Some(crate::Size::Large);
            })
            .unwrap();
    });
    f.edit(|state, window, cx| {
        window.activate_window();
        state.set_value("secret", window, cx);
        state.focus(window, cx);
    });
    let position =
        f.cx.update_window(f.window.into(), |_, window, _| {
            let icon = window
                .rendered_scene()
                .monochrome_sprites
                .last()
                .expect("password icon");
            assert_eq!(icon.color, Color::rgb(0xff0000).to_hsla());
            assert!((icon.bounds.size.width.0 / window.scale_factor() - 28.).abs() < 1.);
            point(
                px(icon.bounds.center().x.0 / window.scale_factor()),
                px(icon.bounds.center().y.0 / window.scale_factor()),
            )
        })
        .unwrap();
    f.pointer(PlatformInput::MouseMove(MouseMoveEvent {
        position,
        ..Default::default()
    }));
    f.cx.update_window(f.window.into(), |_, window, cx| {
        assert_eq!(
            window
                .rendered_scene()
                .monochrome_sprites
                .last()
                .unwrap()
                .color,
            Color::rgb(0x00ff00).to_hsla()
        );
        assert_eq!(window.simulate_next_frame(cx), 0);
    })
    .unwrap();
    f.advance(700);
    assert!(
        f.caret_painted(),
        "component motion=false keeps the caret steady"
    );
}

#[test]
fn disabling_a_focused_input_releases_focus_and_rejects_native_editing() {
    let mut f = Fixture::new(false);
    f.edit(|state, window, cx| state.focus(window, cx));
    f.consumer.update(&mut f.cx, |view, cx| {
        view.disabled = Some(true);
        cx.notify();
    });
    f.draw();
    f.edit(|state, window, cx| {
        assert!(state.disabled);
        assert!(!state.focus.is_focused(window));
        assert!(!state.accepts_text_input(window, cx));
        state.replace_text_in_range(None, "rejected", window, cx);
    });
    assert_eq!(f.value(), "");
}

#[test]
fn clear_slot_preserves_viewport_and_password_suffix_spacing_in_both_directions() {
    for direction in [crate::Direction::Ltr, crate::Direction::Rtl] {
        let mut f = Fixture::new(false);
        let scope = f.consumer.read_with(&f.cx, |view, _| view.scope.clone());
        f.cx.update(|cx| {
            scope
                .update_patch(cx, |patch| {
                    patch.direction = Some(direction);
                    patch.motion = Some(false);
                })
                .unwrap()
        });
        f.configure(|view| {
            view.mode = state::InputMode::Password;
            view.allow_clear = true;
        });
        f.edit(|state, window, cx| state.set_value("secret", window, cx));
        let viewport = f
            .state
            .read_with(&f.cx, |state, _| state.geometry().unwrap().viewport);
        f.cx.update_window(f.window.into(), |_, window, _| {
            let scale = window.scale_factor();
            let sprites = &window.rendered_scene().monochrome_sprites;
            let clear = &sprites
                .iter()
                .find(|s| (s.bounds.size.width.0 / scale - 12.).abs() < 0.1)
                .unwrap()
                .bounds;
            let visibility = &sprites
                .iter()
                .find(|s| (s.bounds.size.width.0 / scale - 14.).abs() < 0.1)
                .unwrap()
                .bounds;
            let (leading, between) = if direction == crate::Direction::Ltr {
                (
                    clear.left().0 / scale - f32::from(viewport.right()),
                    (visibility.left().0 - clear.right().0) / scale,
                )
            } else {
                (
                    f32::from(viewport.left()) - clear.right().0 / scale,
                    (clear.left().0 - visibility.right().0) / scale,
                )
            };
            assert!(
                (leading - 8.).abs() < 0.1,
                "clear leading gap {direction:?}: {leading}"
            );
            assert!(
                (between - 8.).abs() < 0.1,
                "clear/visibility gap {direction:?}: {between}"
            );
        })
        .unwrap();
        f.edit(|state, window, cx| state.set_value("", window, cx));
        f.state.read_with(&f.cx, |state, _| {
            assert_eq!(state.geometry().unwrap().viewport, viewport)
        });
        f.edit(|state, window, cx| {
            state.set_value("secret", window, cx);
            state.set_read_only(true, window, cx);
        });
        f.state.read_with(&f.cx, |state, _| {
            assert_eq!(state.geometry().unwrap().viewport, viewport)
        });
        f.edit(|state, window, cx| state.set_disabled(true, window, cx));
        f.state.read_with(&f.cx, |state, _| {
            assert_eq!(state.geometry().unwrap().viewport, viewport)
        });
    }
}

#[test]
fn search_small_custom_font_stretches_input_to_the_reference_button_minimum() {
    use crate::theme::ThemeConfig;
    for direction in [crate::Direction::Ltr, crate::Direction::Rtl] {
        let mut f = Fixture::new(false);
        let scope = f.consumer.read_with(&f.cx, |view, _| view.scope.clone());
        f.cx.update_window(f.window.into(), |_, window, _| window.set_scale_factor(1.5))
            .unwrap();
        f.cx.update(|cx| {
            scope
                .update_patch(cx, |patch| {
                    patch.direction = Some(direction);
                    patch.size = Some(crate::Size::Small);
                    patch.variant = Some(crate::Variant::Filled);
                    patch.motion = Some(false);
                    let mut theme = ThemeConfig::default();
                    theme.components.set::<InputTheme>(InputTokenPatch {
                        input_font_size: Some(18.),
                        ..Default::default()
                    });
                    patch.theme = Some(theme);
                })
                .unwrap()
        });
        f.configure(|view| view.mode = state::InputMode::Search);
        f.cx.update_window(f.window.into(), |_, window, _| {
            let scale = window.scale_factor();
            let frames = window
                .rendered_scene()
                .quads
                .iter()
                .filter(|quad| {
                    quad.bounds.size.width.0 / scale > 25.
                        && quad.bounds.size.height.0 / scale > 25.
                        && quad
                            .background
                            .as_solid()
                            .is_some_and(|color| (color.a - 0.04).abs() < 0.001)
                })
                .collect::<Vec<_>>();
            assert_eq!(frames.len(), 2, "input and filled search action");
            // Native scene bounds lie on the physical pixel grid; browser CSS
            // metrics retain fractional logical lengths before rasterization.
            let expected = ((18. * 22. / 14. + 2.) * scale).round() / scale;
            for frame in &frames {
                assert!((frame.bounds.size.height.0 / scale - expected).abs() < 0.05);
            }
            assert!((frames[1].bounds.size.width.0 / scale - expected).abs() <= 1. / scale);
        })
        .unwrap();
    }
}

#[test]
fn batched_edits_refresh_wrapped_navigation_and_native_range_geometry_before_draw() {
    let mut f = Fixture::new(true);
    let mut queried = None;
    f.edit(|state, window, cx| {
        state.set_value("A".repeat(120), window, cx);
        state.select(
            TextSelection {
                anchor: 0,
                caret: 0,
            },
            window,
            cx,
        );
        assert!(state.geometry().is_none());
        state.move_boundary(true, false, false, window, cx);
        let first_row_end = state.selection().caret;
        assert!(
            first_row_end > 0 && first_row_end < 120,
            "End must use the new wrapped row before redraw"
        );
        state.move_vertical(true, false, window, cx);
        assert!(state.selection().caret > first_row_end);
        state.set_value("中文👩🏽‍💻", window, cx);
        assert!(state.geometry().is_none());
        let utf16_end = state.value().encode_utf16().count();
        let bounds = state.bounds_for_range(utf16_end..utf16_end, Default::default(), window, cx);
        assert!(
            bounds.is_some(),
            "IME range query must shape the current revision immediately"
        );
        let geometry = state.geometry_snapshot().unwrap();
        assert_eq!(geometry.revision, state.buffer.revision());
        assert_eq!(
            geometry.clusters.last().unwrap().range.end,
            state.value().len()
        );
        queried = Some(geometry);
    });
    let drawn = f
        .state
        .read_with(&f.cx, |state, _| state.geometry_snapshot().unwrap());
    assert!(
        Rc::ptr_eq(&queried.unwrap(), &drawn),
        "the next frame reuses geometry computed for the native query"
    );
}

#[test]
fn immediate_native_candidate_query_reveals_the_new_single_line_caret() {
    let mut f = Fixture::new(false);
    f.edit(|state, window, cx| {
        state.replace_selection(&"漢".repeat(200), EditOrigin::Typing, window, cx);
        let end = state.value().encode_utf16().count();
        let bounds = state
            .bounds_for_range(end..end, Default::default(), window, cx)
            .unwrap();
        let viewport = state.geometry().unwrap().viewport;
        assert!(bounds.left() >= viewport.left());
        assert!(bounds.right() <= viewport.right() + px(1.));
        assert!(state.geometry().unwrap().scroll.x > px(0.));
    });
}
