// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;
use crate::{ConfigScope, UiRoot};
use gpui::{
    AppContext, Context, Entity, EntityInputHandler, Focusable, IntoElement, KeyDownEvent,
    KeyUpEvent, Keystroke, MouseButton, MouseDownEvent, MouseMoveEvent, MouseUpEvent,
    PlatformInput, Render, Subscription, TestAppContext, Window, WindowHandle, div, point,
    prelude::*, px,
};
use std::{
    cell::{Cell, RefCell},
    rc::Rc,
};

struct Consumer {
    state: Entity<SelectState<String>>,
    scope: ConfigScope,
    value: Option<Vec<SelectValue<String>>>,
    accept: bool,
    shown: bool,
    disabled: Option<bool>,
    open: Option<bool>,
    search: Option<gpui::SharedString>,
    builds: Rc<Cell<usize>>,
    tag_builds: Rc<Cell<usize>>,
    variable: bool,
    responsive: bool,
    width: f32,
    size: crate::Size,
    loading: bool,
    clipped: bool,
    nested: bool,
    parent_popup: crate::popup::PopupHandle,
    _subscription: Subscription,
}
impl Render for Consumer {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let mut root = div().w(px(400.)).h(px(600.)).p(px(10.));
        if self.shown {
            let builds = self.builds.clone();
            let variable = self.variable;
            let mut select = Select::new("select", &self.state)
                .scope(self.scope.clone())
                .w(px(self.width))
                .size(self.size)
                .loading(self.loading)
                .accessibility_label("Choose a value")
                .automation_id("test-select")
                .allow_clear(true)
                .placeholder("Choose")
                .option_render(move |o, w, c| {
                    let _ = (w, c);
                    builds.set(builds.get() + 1);
                    div()
                        .when(variable, |d| {
                            d.h(px(if o.index.is_multiple_of(2) { 60. } else { 40. }))
                        })
                        .child(o.option.label.clone())
                })
                .on_event(cx.listener(|this, event: &SelectEvent<String>, _, cx| {
                    if this.accept {
                        match event {
                            SelectEvent::Change(change) if this.value.is_some() => {
                                this.value = Some(change.values.clone())
                            }
                            SelectEvent::OpenChange { open, .. } if this.open.is_some() => {
                                this.open = Some(*open)
                            }
                            SelectEvent::Search(search) if this.search.is_some() => {
                                this.search = Some(search.clone())
                            }
                            _ => {}
                        }
                        cx.notify();
                    }
                }));
            if self.variable {
                select = select.list_layout(SelectListLayout::Variable);
            }
            if self.responsive {
                let builds = self.tag_builds.clone();
                select = select.responsive_tags(true).tag_render(move |tag, _, _| {
                    builds.set(builds.get() + 1);
                    div().w(px(70.)).h(px(24.)).child(tag.value.label.clone())
                });
            }
            if let Some(value) = &self.value {
                select = select.value(value.clone());
            }
            if let Some(disabled) = self.disabled {
                select = select.disabled(disabled);
            }
            if let Some(open) = self.open {
                select = select.open(open);
            }
            if let Some(search) = &self.search {
                select = select.search_value(search.clone());
            }
            root = if self.nested {
                let state = self.state.clone();
                let scope = self.scope.clone();
                root.child(
                    crate::popup::Popup::new("parent")
                        .scope(self.scope.clone())
                        .handle(&self.parent_popup)
                        .default_open(true)
                        .placement(crate::popup::PopupPlacement::BottomLeft)
                        .child(
                            crate::Button::new("parent-trigger")
                                .label("Choose")
                                .w(px(140.)),
                        )
                        .content(move |_, _, _| {
                            Select::new("nested-select", &state)
                                .scope(scope.clone())
                                .w(px(300.))
                        }),
                )
            } else if self.clipped {
                root.h(px(40.))
                    .overflow_hidden()
                    .child(div().mt(px(100.)).child(select))
            } else {
                root.child(select)
            };
        }
        root
    }
}
struct Fixture {
    cx: TestAppContext,
    window: WindowHandle<UiRoot>,
    consumer: Entity<Consumer>,
    state: Entity<SelectState<String>>,
    events: Rc<RefCell<Vec<SelectEvent<String>>>>,
}
impl Fixture {
    fn new(mode: SelectMode, count: usize) -> Self {
        let options = SelectOptions::new((0..count).map(|i| {
            SelectOption::new(("item", i), i.to_string(), format!("Option {i}")).disabled(i == 1)
        }))
        .unwrap();
        let mut cx = TestAppContext::single();
        let mut result = None;
        let events = Rc::new(RefCell::new(Vec::new()));
        let output = events.clone();
        let window = cx.add_window(|window, cx| {
            window.activate_window();
            let scope = ConfigScope::new_window(cx);
            scope.update_patch(cx, |p| p.motion = Some(false)).unwrap();
            let state = cx.new(|cx| SelectState::new(options, window, cx));
            state.update(cx, |s, cx| {
                s.set_settings(
                    SelectSettings {
                        mode,
                        token_separators: vec![',', ';'],
                        ..Default::default()
                    },
                    cx,
                );
                s.set_tag_factory(
                    Rc::new(|text| {
                        Some(SelectOption::new(
                            gpui::SharedString::from(format!("created-{text}")),
                            text.to_owned(),
                            text.to_owned(),
                        ))
                    }),
                    cx,
                );
            });
            let consumer = cx.new(|cx| Consumer {
                state: state.clone(),
                scope: scope.clone(),
                value: None,
                accept: false,
                shown: true,
                disabled: None,
                open: None,
                search: None,
                builds: Rc::new(Cell::new(0)),
                tag_builds: Rc::new(Cell::new(0)),
                variable: false,
                responsive: false,
                width: 300.,
                size: crate::Size::Medium,
                loading: false,
                clipped: false,
                nested: false,
                parent_popup: crate::popup::PopupHandle::new(),
                _subscription: cx.subscribe(&state, move |_, _, event, _| {
                    output.borrow_mut().push(event.clone())
                }),
            });
            result = Some((consumer.clone(), state));
            UiRoot::new(scope, consumer, window, cx)
        });
        let (consumer, state) = result.unwrap();
        let mut f = Self {
            cx,
            window,
            consumer,
            state,
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
    fn click(&mut self, x: f32, y: f32) {
        self.event(PlatformInput::MouseDown(MouseDownEvent {
            position: point(px(x), px(y)),
            button: MouseButton::Left,
            click_count: 1,
            ..Default::default()
        }));
        self.event(PlatformInput::MouseUp(MouseUpEvent {
            position: point(px(x), px(y)),
            button: MouseButton::Left,
            ..Default::default()
        }));
    }
    fn key(&mut self, key: &'static str) {
        self.event(PlatformInput::KeyDown(KeyDownEvent {
            keystroke: Keystroke::parse(key).unwrap(),
            is_held: false,
            prefer_character_input: false,
        }));
    }
    fn key_up(&mut self, key: &'static str) {
        self.event(PlatformInput::KeyUp(KeyUpEvent {
            keystroke: Keystroke::parse(key).unwrap(),
        }));
    }
    fn focus(&mut self) {
        let state = self.state.clone();
        self.cx
            .update_window(self.window.into(), |_, w, c| {
                state.update(c, |s, c| s.focus(w, c))
            })
            .unwrap();
        self.draw();
    }
    fn edit(&mut self, edit: impl FnOnce(&mut Consumer)) {
        self.consumer.update(&mut self.cx, |s, c| {
            edit(s);
            c.notify();
        });
        self.draw();
    }
    fn input(&mut self, text: &str, composing: bool) {
        let state = self.state.clone();
        self.cx
            .update_window(self.window.into(), |_, w, c| {
                let editor = state.read(c).editor.clone();
                editor.update(c, |s, c| {
                    if composing {
                        s.replace_and_mark_text_in_range(
                            None,
                            text,
                            Some(text.len()..text.len()),
                            w,
                            c,
                        );
                    } else {
                        s.replace_text_in_range(None, text, w, c);
                    }
                });
            })
            .unwrap();
        self.draw();
    }
    fn values(&self) -> Vec<String> {
        self.state.read_with(&self.cx, |s, _| {
            s.values().iter().map(|v| v.value.clone()).collect()
        })
    }
    fn open(&self) -> bool {
        self.state.read_with(&self.cx, |s, _| s.is_open())
    }
    fn query(&self) -> String {
        self.state
            .read_with(&self.cx, |s, _| s.search_value().to_owned())
    }
    fn change_count(&self) -> usize {
        self.events
            .borrow()
            .iter()
            .filter(|e| matches!(e, SelectEvent::Change(_)))
            .count()
    }
}

#[test]
fn native_single_keyboard_skips_disabled_wraps_and_orders_change_before_select() {
    let mut f = Fixture::new(SelectMode::Single, 4);
    f.focus();
    f.key("down");
    assert!(f.open());
    f.key("down");
    assert_eq!(
        f.state.read_with(&f.cx, |s, _| s.active_value().cloned()),
        Some("2".into())
    );
    f.key("enter");
    f.key_up("enter");
    assert_eq!(f.values(), vec!["2"]);
    assert!(!f.open());
    let events = f.events.borrow();
    let change = events
        .iter()
        .position(|e| matches!(e, SelectEvent::Change(_)))
        .unwrap();
    assert!(matches!(events[change + 1], SelectEvent::Select(_)));
}
#[test]
fn native_pointer_keeps_editor_focus_and_outside_press_dismisses() {
    let mut f = Fixture::new(SelectMode::Multiple, 4);
    f.click(80., 25.);
    assert!(f.open());
    f.click(80., 65.);
    assert_eq!(f.values(), vec!["0"]);
    assert!(f.open());
    f.cx.update_window(f.window.into(), |_, w, c| {
        assert!(f.state.read(c).focus_handle(c).is_focused(w))
    })
    .unwrap();
    f.click(800., 500.);
    assert!(!f.open());
}

#[test]
fn nested_select_dismisses_before_its_parent_and_keeps_parent_open_during_selection() {
    let mut f = Fixture::new(SelectMode::Multiple, 4);
    f.edit(|consumer| consumer.nested = true);
    let parent = f
        .consumer
        .read_with(&f.cx, |consumer, _| consumer.parent_popup.clone());
    assert!(f.cx.update(|cx| parent.is_open(cx)));
    f.focus();
    f.key("down");
    f.key("enter");
    f.key_up("enter");
    assert_eq!(f.values(), vec!["0"]);
    assert!(f.open());
    assert!(f.cx.update(|cx| parent.is_open(cx)));
    f.key("escape");
    assert!(!f.open());
    assert!(f.cx.update(|cx| parent.is_open(cx)));
    f.key("escape");
    assert!(!f.cx.update(|cx| parent.is_open(cx)));
}
#[test]
fn native_controlled_selection_open_and_search_accept_or_reject_proposals() {
    let mut f = Fixture::new(SelectMode::Multiple, 4);
    f.edit(|c| {
        c.value = Some(vec![]);
        c.open = Some(false);
        c.search = Some("".into());
    });
    f.focus();
    f.key("down");
    assert!(!f.open());
    f.edit(|c| c.accept = true);
    f.key("down");
    assert!(f.open());
    f.edit(|c| c.accept = false);
    f.key("enter");
    f.key_up("enter");
    assert!(f.values().is_empty());
    assert_eq!(f.change_count(), 1);
    f.edit(|c| c.accept = true);
    f.key("enter");
    f.key_up("enter");
    assert_eq!(f.values(), vec!["0"]);
    f.input("2", false);
    assert_eq!(f.query(), "2");
    f.edit(|c| c.accept = false);
    f.input("3", false);
    assert_eq!(f.query(), "2");
    assert_eq!(
        f.state
            .read_with(&f.cx, |s, c| s.editor.read(c).value().to_owned()),
        "2"
    );
}
#[test]
fn native_tags_tokenize_paste_and_never_select_composition_text() {
    let mut f = Fixture::new(SelectMode::Tags, 3);
    f.focus();
    f.input("ni,hao", true);
    f.key("enter");
    f.key_up("enter");
    assert!(f.values().is_empty());
    f.input("你好", false);
    assert_eq!(f.query(), "你好");
    assert!(f.open());
    f.key("enter");
    f.key_up("enter");
    assert_eq!(f.values(), vec!["你好"]);
    f.input("alpha,beta", false);
    assert_eq!(f.values(), vec!["你好", "alpha", "beta"]);
    assert_eq!(f.query(), "");
    assert!(!f.open());
}
#[test]
fn native_multiple_label_tokens_backspace_and_limit_preserve_disabled_values() {
    let mut f = Fixture::new(SelectMode::Multiple, 4);
    f.state.update(&mut f.cx, |s, c| {
        s.set_values(["1".to_owned()], c);
        let mut settings = s.settings().clone();
        settings.max_count = Some(2);
        s.set_settings(settings, c);
    });
    f.draw();
    f.focus();
    f.input("Option 0,Option 2", false);
    assert_eq!(f.values(), vec!["1", "0"]);
    f.key("backspace");
    f.key_up("backspace");
    assert_eq!(f.values(), vec!["1"]);
    f.key("backspace");
    f.key_up("backspace");
    assert_eq!(f.values(), vec!["1"]);
}
#[test]
fn native_tab_commits_active_then_next_tab_traverses_and_enter_repeat_is_locked() {
    let mut f = Fixture::new(SelectMode::Multiple, 4);
    f.focus();
    f.key("down");
    f.key("enter");
    f.key("enter");
    assert_eq!(f.values(), vec!["0"]);
    assert_eq!(f.change_count(), 1);
    f.key_up("enter");
    f.key("down");
    f.key("tab");
    assert_eq!(f.values(), vec!["0", "2"]);
    assert!(!f.open());
}
#[test]
fn hundred_thousand_options_build_only_visible_rows_and_reuse_filter_snapshot() {
    let mut f = Fixture::new(SelectMode::Single, 100_000);
    let revision = f.state.read_with(&f.cx, |s, _| s.model.filter_revision);
    f.focus();
    f.key("down");
    let builds = f.consumer.read_with(&f.cx, |c, _| c.builds.get());
    assert!(builds > 0 && builds < 50, "built {builds} rows");
    for _ in 0..5 {
        f.draw();
    }
    assert_eq!(
        f.state.read_with(&f.cx, |s, _| s.model.filter_revision),
        revision
    );
    assert!(f.state.read_with(&f.cx, |s, _| s.rendered_rows) < 20);
    f.state.update(&mut f.cx, |s, c| {
        assert!(s.scroll_to(&"99999".to_owned(), c))
    });
    f.draw();
    assert!(f.state.read_with(&f.cx, |s, _| s.rendered_rows) < 20);
}
#[test]
fn disabling_and_unmounting_close_popups_and_stop_idle_animation_frames() {
    let mut f = Fixture::new(SelectMode::Single, 4);
    f.focus();
    f.key("down");
    assert!(f.open());
    f.edit(|c| c.disabled = Some(true));
    assert!(!f.open());
    let before = f.change_count();
    f.key("enter");
    assert_eq!(f.change_count(), before);
    f.edit(|c| {
        c.disabled = None;
        c.shown = false;
    });
    assert!(!f.open());
    f.event(PlatformInput::MouseMove(MouseMoveEvent {
        position: point(px(20.), px(20.)),
        ..Default::default()
    }));
    f.cx.update_window(f.window.into(), |_, w, c| {
        w.simulate_next_frame(c);
    })
    .unwrap();
    f.draw();
    f.cx.update_window(f.window.into(), |_, w, c| {
        assert_eq!(w.simulate_next_frame(c), 0)
    })
    .unwrap();
}

#[test]
fn clipped_loading_controls_stop_scheduling_and_resume_when_visible() {
    let mut f = Fixture::new(SelectMode::Single, 4);
    let scope = f
        .consumer
        .read_with(&f.cx, |consumer, _| consumer.scope.clone());
    f.cx.update(|cx| {
        scope
            .update_patch(cx, |patch| patch.motion = Some(true))
            .unwrap()
    });
    f.edit(|consumer| consumer.loading = true);
    f.cx.update_window(f.window.into(), |_, window, cx| {
        assert!(window.simulate_next_frame(cx) > 0);
    })
    .unwrap();
    f.edit(|consumer| consumer.clipped = true);
    f.cx.update_window(f.window.into(), |_, window, cx| {
        window.simulate_next_frame(cx);
    })
    .unwrap();
    f.draw();
    f.cx.update_window(f.window.into(), |_, window, cx| {
        assert_eq!(
            window.simulate_next_frame(cx),
            0,
            "clipped loading must be idle"
        );
    })
    .unwrap();
    f.edit(|consumer| consumer.clipped = false);
    f.cx.update_window(f.window.into(), |_, window, cx| {
        assert!(window.simulate_next_frame(cx) > 0);
    })
    .unwrap();
    f.cx.update(|cx| cx.set_reduce_motion(true));
    f.draw();
    f.cx.update_window(f.window.into(), |_, window, cx| {
        window.simulate_next_frame(cx);
    })
    .unwrap();
    f.draw();
    f.cx.update_window(f.window.into(), |_, window, cx| {
        assert_eq!(
            window.simulate_next_frame(cx),
            0,
            "reduced-motion loading must be idle"
        );
    })
    .unwrap();
}

#[test]
fn stale_editor_events_cannot_edit_non_searchable_or_at_limit_selects() {
    let mut f = Fixture::new(SelectMode::Single, 4);
    let emit_change = |f: &mut Fixture| {
        let editor = f.state.read_with(&f.cx, |state, _| state.editor.clone());
        editor.update(&mut f.cx, |_, cx| {
            cx.emit(crate::input::InputEvent::Change(
                crate::input::InputChange {
                    value: "stale".into(),
                    origin: crate::input::EditOrigin::Typing,
                    is_composing: false,
                    revision: 1,
                },
            ))
        });
        f.draw();
    };
    emit_change(&mut f);
    assert!(!f.open());
    assert!(
        f.state
            .read_with(&f.cx, |state, _| state.search_value().is_empty())
    );
    f.state.update(&mut f.cx, |state, cx| {
        state.set_settings(
            SelectSettings {
                mode: SelectMode::Multiple,
                max_count: Some(1),
                ..Default::default()
            },
            cx,
        );
        state.set_values(vec!["0".to_owned()], cx);
    });
    f.draw();
    emit_change(&mut f);
    assert!(!f.open());
    assert!(
        f.state
            .read_with(&f.cx, |state, _| state.search_value().is_empty())
    );
}

#[test]
fn variable_height_rows_stay_virtual_and_support_scrolling_to_the_last_option() {
    let mut f = Fixture::new(SelectMode::Single, 100_000);
    f.edit(|c| c.variable = true);
    f.focus();
    f.key("down");
    assert!(f.state.read_with(&f.cx, |s, _| s.rendered_rows) < 30);
    f.state.update(&mut f.cx, |s, c| {
        assert!(s.scroll_to(&"99999".to_owned(), c))
    });
    f.draw();
    assert!(
        f.state.read_with(&f.cx, |s, _| s
            .variable_list
            .as_ref()
            .unwrap()
            .logical_scroll_top()
            .item_ix)
            > 99_970
    );
    assert!(f.state.read_with(&f.cx, |s, _| s.rendered_rows) < 30);
    f.edit(|c| c.width = 180.);
    assert!(f.state.read_with(&f.cx, |s, _| s.rendered_rows) < 30);
}

#[test]
fn responsive_tags_measure_only_visible_candidates_and_resize_without_notify_loops() {
    let mut f = Fixture::new(SelectMode::Multiple, 100);
    f.state.update(&mut f.cx, |s, c| {
        s.set_values((0..100).map(|i| i.to_string()), c)
    });
    f.edit(|c| {
        c.responsive = true;
        c.tag_builds.set(0);
    });
    let first = f.consumer.read_with(&f.cx, |c, _| c.tag_builds.get());
    assert!(first > 0 && first < 8, "measured {first} tags");
    f.edit(|c| {
        c.width = 160.;
        c.tag_builds.set(0);
    });
    let second = f.consumer.read_with(&f.cx, |c, _| c.tag_builds.get());
    assert!(
        second > 0 && second < first,
        "narrower width should build fewer tags: {first}, {second}"
    );
    f.cx.update_window(f.window.into(), |_, w, c| {
        w.simulate_next_frame(c);
    })
    .unwrap();
    f.draw();
    f.cx.update_window(f.window.into(), |_, w, c| {
        assert_eq!(w.simulate_next_frame(c), 0)
    })
    .unwrap();
    assert_eq!(f.values().len(), 100);
}

#[test]
fn narrow_responsive_tags_keep_a_visible_editor_in_both_directions() {
    let mut f = Fixture::new(SelectMode::Multiple, 100);
    f.state.update(&mut f.cx, |state, cx| {
        state.set_values((0..100).map(|i| i.to_string()), cx)
    });
    f.edit(|c| {
        c.responsive = true;
        c.width = 80.;
    });
    for direction in [crate::Direction::Ltr, crate::Direction::Rtl] {
        let scope = f.consumer.read_with(&f.cx, |c, _| c.scope.clone());
        f.cx.update_window(f.window.into(), |_, _, cx| {
            scope
                .update_patch(cx, |p| p.direction = Some(direction))
                .unwrap()
        })
        .unwrap();
        f.draw();
        for query in ["", "a very long query that needs horizontal scrolling"] {
            f.cx.update_window(f.window.into(), |_, window, cx| {
                f.state
                    .update(cx, |state, cx| state.set_search_value(query, window, cx));
            })
            .unwrap();
            f.draw();
            f.state.read_with(&f.cx, |state, cx| {
                let geometry = state
                    .editor
                    .read(cx)
                    .geometry()
                    .expect("native editor geometry");
                assert!(geometry.viewport.size.width > px(0.));
                assert!(
                    geometry.viewport.left() >= px(10.) && geometry.viewport.right() <= px(90.),
                    "{direction:?}, {query}: {:?}",
                    geometry.viewport
                );
            });
        }
    }
}

#[test]
fn small_multiple_editor_does_not_inflate_the_control_line_box() {
    for compact in [false, true] {
        for scale in [1., 1.5, 2.] {
            let mut f = Fixture::new(SelectMode::Multiple, 4);
            f.state.update(&mut f.cx, |state, cx| {
                state.set_values(vec!["0".to_owned()], cx)
            });
            let scope = f
                .consumer
                .read_with(&f.cx, |consumer, _| consumer.scope.clone());
            f.cx.update_window(f.window.into(), |_, window, cx| {
                window.set_scale_factor(scale);
                scope
                    .update_patch(cx, |patch| {
                        patch.theme.get_or_insert_default().algorithms = Some(if compact {
                            crate::theme::AlgorithmChain::compact()
                        } else {
                            Default::default()
                        })
                    })
                    .unwrap();
            })
            .unwrap();
            f.edit(|consumer| consumer.size = crate::Size::Small);
            let expected = f.cx.update(|cx| {
                scope.snapshot(cx).theme.tokens.control_height_sm as f32
                    - 2. * (1. - scale.floor() / scale)
            });
            f.cx.update_window(f.window.into(), |_, window, _| {
                let surface = window
                    .rendered_scene()
                    .quads
                    .iter()
                    .find(|quad| {
                        (quad.bounds.size.width.0 / scale - 300.).abs() < 0.01
                            && quad.border_color.a > 0.
                    })
                    .unwrap();
                assert!(
                    (surface.bounds.size.height.0 / scale - expected).abs() < 0.7,
                    "compact={compact}, scale={scale}: actual={}, expected={expected}",
                    surface.bounds.size.height.0 / scale
                );
            })
            .unwrap();
        }
    }
}

#[test]
fn controlled_query_does_not_erase_native_composition_during_an_unrelated_render() {
    let mut f = Fixture::new(SelectMode::Tags, 3);
    f.edit(|c| c.search = Some("".into()));
    f.focus();
    f.input("ni", true);
    f.edit(|c| c.width = 250.);
    assert!(
        f.state
            .read_with(&f.cx, |s, c| s.editor.read(c).is_composing())
    );
    assert_eq!(
        f.state
            .read_with(&f.cx, |s, c| s.editor.read(c).value().to_owned()),
        "ni"
    );
    f.key("enter");
    f.key_up("enter");
    assert!(f.values().is_empty());
    f.input("你", false);
    assert_eq!(f.query(), "");
    assert_eq!(
        f.state
            .read_with(&f.cx, |s, c| s.editor.read(c).value().to_owned()),
        ""
    );
}
