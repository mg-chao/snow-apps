// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::model::{
    PaginationItemKind as Item, PaginationMode, PaginationModel, PaginationSettings, QuickJumper,
    digit_page, page_count, simple_page,
};

fn model(total: u64, current: u64, size: u64) -> PaginationModel {
    let mut model = PaginationModel {
        total,
        current,
        page_size: size,
        settings: Default::default(),
    };
    model.normalize();
    model
}
fn sequence(model: &PaginationModel) -> Vec<Item> {
    model.items().into_iter().map(|item| item.kind).collect()
}

#[test]
fn pagination_empty_single_and_small_page_sequences() {
    let empty = model(0, 20, 10);
    assert_eq!(empty.current(), 1);
    assert_eq!(empty.page_count(), 0);
    assert_eq!(empty.range(), (0, 0));
    assert_eq!(
        sequence(&empty),
        [Item::Previous, Item::Page(1), Item::Next]
    );
    assert!(empty.items().iter().all(|item| item.disabled));
    let single = model(4, 0, 10);
    assert_eq!(single.range(), (1, 4));
    assert!(!single.items()[1].disabled);
    let small = model(70, 3, 10);
    assert_eq!(
        sequence(&small),
        [
            Item::Previous,
            Item::Page(1),
            Item::Page(2),
            Item::Page(3),
            Item::Page(4),
            Item::Page(5),
            Item::Page(6),
            Item::Page(7),
            Item::Next
        ]
    );
}

#[test]
fn pagination_reference_windows_shrink_around_jumpers() {
    assert_eq!(
        sequence(&model(500, 1, 10)),
        [
            Item::Previous,
            Item::Page(1),
            Item::Page(2),
            Item::Page(3),
            Item::Page(4),
            Item::Page(5),
            Item::JumpNext,
            Item::Page(50),
            Item::Next
        ]
    );
    assert_eq!(
        sequence(&model(500, 6, 10)),
        [
            Item::Previous,
            Item::Page(1),
            Item::JumpPrevious,
            Item::Page(5),
            Item::Page(6),
            Item::Page(7),
            Item::JumpNext,
            Item::Page(50),
            Item::Next
        ]
    );
    assert_eq!(
        sequence(&model(500, 50, 10)),
        [
            Item::Previous,
            Item::Page(1),
            Item::JumpPrevious,
            Item::Page(46),
            Item::Page(47),
            Item::Page(48),
            Item::Page(49),
            Item::Page(50),
            Item::Next
        ]
    );
    let middle = model(500, 6, 10);
    let items = middle.items();
    assert_eq!(
        items
            .iter()
            .find(|item| item.kind == Item::JumpPrevious)
            .unwrap()
            .target,
        1
    );
    assert_eq!(
        items
            .iter()
            .find(|item| item.kind == Item::JumpNext)
            .unwrap()
            .target,
        11
    );
    assert_eq!(
        items
            .iter()
            .filter(|item| item.hidden_at_narrow)
            .map(|item| item.kind)
            .collect::<Vec<_>>(),
        [Item::Page(5), Item::Page(7)]
    );
}

#[test]
fn pagination_less_items_and_hidden_jumpers_preserve_reference_sequence() {
    let mut pager = model(500, 6, 10);
    pager.settings.show_less_items = true;
    assert_eq!(
        sequence(&pager),
        [
            Item::Previous,
            Item::Page(1),
            Item::JumpPrevious,
            Item::Page(5),
            Item::Page(6),
            Item::Page(7),
            Item::JumpNext,
            Item::Page(50),
            Item::Next
        ]
    );
    assert_eq!(
        pager
            .items()
            .iter()
            .find(|item| item.kind == Item::JumpPrevious)
            .unwrap()
            .target,
        3
    );
    assert_eq!(
        pager
            .items()
            .iter()
            .find(|item| item.kind == Item::JumpNext)
            .unwrap()
            .target,
        9
    );
    pager.settings.show_less_items = false;
    pager.settings.show_prev_next_jumpers = false;
    assert_eq!(
        sequence(&pager),
        [
            Item::Previous,
            Item::Page(1),
            Item::Page(4),
            Item::Page(5),
            Item::Page(6),
            Item::Page(7),
            Item::Page(8),
            Item::Page(50),
            Item::Next
        ]
    );
    assert!(pager.items().iter().all(|item| !item.hidden_at_narrow));
}

#[test]
fn pagination_simple_and_hide_on_single_page() {
    let mut pager = model(100, 4, 10);
    for mode in [PaginationMode::Simple, PaginationMode::SimpleReadOnly] {
        pager.settings.mode = mode;
        assert_eq!(sequence(&pager), [Item::Previous, Item::Next]);
    }
    pager.settings.hide_on_single_page = true;
    assert!(!pager.hidden());
    pager.total = 10;
    pager.normalize();
    assert!(pager.hidden());
    pager.total = 0;
    assert!(pager.hidden());
}

#[test]
fn pagination_arithmetic_remains_exact_at_u64_limits() {
    assert_eq!(page_count(u64::MAX, 1), u64::MAX);
    assert_eq!(page_count(u64::MAX, u64::MAX), 1);
    assert_eq!(page_count(u64::MAX, 10), u64::MAX / 10 + 1);
    let pager = model(u64::MAX, u64::MAX, 1);
    assert_eq!(pager.range(), (u64::MAX, u64::MAX));
    assert_eq!(pager.items().last().unwrap().target, u64::MAX);
    assert!(pager.items().last().unwrap().disabled);
    let pager = model(u64::MAX, u64::MAX, 10);
    assert_eq!(pager.range().1, u64::MAX);
    assert!(pager.range().0 <= pager.range().1);
    assert_eq!(model(100, 20, 0).page_size(), 1);
}

#[test]
fn pagination_page_windows_are_bounded_unique_and_ordered() {
    for less in [false, true] {
        for jumpers in [false, true] {
            for last in [1, 2, 5, 7, 8, 10, 25, 100, u64::MAX] {
                for current in [
                    1,
                    2,
                    3,
                    4,
                    5,
                    6,
                    last / 2,
                    last.saturating_sub(3),
                    last.saturating_sub(1),
                    last,
                ] {
                    let mut pager = model(last, current, 1);
                    pager.settings = PaginationSettings {
                        show_less_items: less,
                        show_prev_next_jumpers: jumpers,
                        ..Default::default()
                    };
                    let items = pager.items();
                    assert!(items.len() <= 11);
                    let pages = items
                        .iter()
                        .filter_map(|item| {
                            if let Item::Page(page) = item.kind {
                                Some(page)
                            } else {
                                None
                            }
                        })
                        .collect::<Vec<_>>();
                    assert_eq!(pages.first(), Some(&1));
                    assert_eq!(pages.last(), Some(&last));
                    assert!(pages.windows(2).all(|pair| pair[0] < pair[1]));
                    assert!(pages.contains(&pager.current()));
                    assert!(
                        items
                            .iter()
                            .all(|item| item.target <= last && (item.disabled || item.target >= 1))
                    );
                }
            }
        }
    }
}

#[test]
fn pagination_native_jumper_parsing_is_bounded_and_validates_digits() {
    assert_eq!(digit_page(""), None);
    assert_eq!(digit_page("0005"), Some(5));
    assert_eq!(digit_page("0"), Some(0));
    assert_eq!(digit_page("-1"), None);
    assert_eq!(digit_page("１２"), None);
    assert_eq!(digit_page("1.5"), None);
    assert_eq!(digit_page(&"9".repeat(10_000)), Some(u64::MAX));
    assert_eq!(simple_page("-5"), Some(0));
    assert_eq!(simple_page("1e2"), Some(100));
    assert_eq!(simple_page("3.0"), Some(3));
    assert_eq!(simple_page("3.5"), None);
    assert_eq!(simple_page("NaN"), None);
    assert_eq!(simple_page("inf"), None);
    assert_eq!(simple_page("18446744073709551615"), Some(u64::MAX));
}

#[test]
fn pagination_models_match_independent_pinned_browser_snapshots() {
    let fixture: serde_json::Value = serde_json::from_str(include_str!(
        "../../fixtures/pagination/behavior-reference.json"
    ))
    .unwrap();
    let cases = fixture["models"].as_array().unwrap();
    assert_eq!(cases.len(), 222);
    for case in cases {
        let scenario = &case["scenario"];
        let observed = &case["observed"];
        let mut pager = model(
            scenario["total"].as_u64().unwrap(),
            scenario["current"].as_u64().unwrap_or(1),
            scenario["page_size"].as_u64().unwrap_or(10),
        );
        pager.settings.show_less_items = scenario["show_less"].as_bool().unwrap_or(false);
        pager.settings.hide_on_single_page = scenario["hide_single"].as_bool().unwrap_or(false);
        let actual = if pager.hidden() {
            Vec::new()
        } else {
            sequence(&pager)
        };
        let expected = observed["items"]
            .as_array()
            .unwrap()
            .iter()
            .filter_map(|item| {
                Some(match item["kind"].as_str().unwrap() {
                    "prev" => Item::Previous,
                    "next" => Item::Next,
                    "jump-prev" => Item::JumpPrevious,
                    "jump-next" => Item::JumpNext,
                    "page" => Item::Page(item["text"].as_str().unwrap().parse().unwrap()),
                    _ => return None,
                })
            })
            .collect::<Vec<_>>();
        assert_eq!(actual, expected, "{scenario}");
        if let Some(active) = observed["active"].as_u64() {
            assert_eq!(pager.current(), active, "{scenario}");
        }
    }
}

mod retained {
    use super::super::state::{
        PaginationChangeSource as Source, PaginationEvent, PaginationRenderConfig,
    };
    use super::*;
    use crate::{
        ConfigScope, InputChange, InputEvent, PaginationState,
        input::{CommitReason, EditOrigin, InputCommit},
    };
    use gpui::{
        AppContext, Context, Entity, EntityInputHandler, IntoElement, Render, Subscription,
        TestAppContext, Window, WindowHandle, div,
    };
    use std::{cell::RefCell, rc::Rc};

    struct Harness {
        state: Entity<PaginationState>,
        _events: Entity<Events>,
    }
    struct Events {
        _subscription: Subscription,
    }
    impl Render for Harness {
        fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
            div()
        }
    }
    struct Fixture {
        cx: TestAppContext,
        window: WindowHandle<Harness>,
        events: Rc<RefCell<Vec<PaginationEvent>>>,
    }
    impl Fixture {
        fn new() -> Self {
            let mut cx = TestAppContext::single();
            let events = Rc::new(RefCell::new(Vec::new()));
            let output = events.clone();
            let window = cx.add_window(|window, cx| {
                let scope = ConfigScope::new_window(cx);
                let state = cx.new(|cx| {
                    let mut state = PaginationState::new(window, cx);
                    state.mounted = true;
                    state.sync_render(
                        PaginationRenderConfig {
                            total: 500,
                            current: None,
                            default_current: 6,
                            page_size: None,
                            default_page_size: 10,
                            settings: Default::default(),
                            disabled: None,
                            show_size_changer: true,
                            options_visible: true,
                            narrow: false,
                        },
                        scope,
                        window,
                        cx,
                    );
                    state
                });
                let events = cx.new(|cx| Events {
                    _subscription: cx.subscribe(&state, move |_, _, event, _| {
                        output.borrow_mut().push(event.clone())
                    }),
                });
                Harness {
                    state,
                    _events: events,
                }
            });
            Self { cx, window, events }
        }
        fn update<R>(
            &mut self,
            update: impl FnOnce(&mut PaginationState, &mut Window, &mut Context<PaginationState>) -> R,
        ) -> R {
            self.window
                .update(&mut self.cx, |harness, window, cx| {
                    harness
                        .state
                        .update(cx, |state, cx| update(state, window, cx))
                })
                .unwrap()
        }
        fn input(&mut self, simple: bool, text: &str, composing: bool) {
            self.update(|state, window, cx| {
                let editor = if simple {
                    state.simple_editor.clone()
                } else {
                    state.quick_editor.clone()
                };
                editor.update(cx, |input, cx| {
                    input.set_value(text, window, cx);
                    cx.emit(InputEvent::Change(InputChange {
                        value: text.to_owned().into(),
                        origin: EditOrigin::Typing,
                        is_composing: composing,
                        revision: 1,
                    }));
                });
            });
        }
        fn commit(&mut self, simple: bool, reason: CommitReason) {
            self.update(|state, _, cx| {
                let editor = if simple {
                    state.simple_editor.clone()
                } else {
                    state.quick_editor.clone()
                };
                editor.update(cx, |input, cx| {
                    cx.emit(InputEvent::Commit(InputCommit {
                        value: input.value_snapshot(),
                        reason,
                    }))
                });
            });
        }
    }

    #[test]
    fn pagination_interactions_match_independent_pinned_browser_traces() {
        let reference: serde_json::Value = serde_json::from_str(include_str!(
            "../../fixtures/pagination/behavior-reference.json"
        ))
        .unwrap();
        let traces = reference["interactions"].as_array().unwrap();
        assert_eq!(traces.len(), 17);
        for trace in traces {
            let name = trace["name"].as_str().unwrap();
            let scenario = &trace["scenario"];
            let kind = scenario["kind"].as_str().unwrap_or("");
            let simple = kind == "simple";
            let controlled = scenario["controlled"].as_str();
            let mut fixture = Fixture::new();
            fixture.update(|state, window, cx| {
                state.set_current(scenario["current"].as_u64().unwrap(), window, cx);
                state.sync_render(
                    PaginationRenderConfig {
                        total: scenario["total"].as_u64().unwrap(),
                        current: controlled.map(|_| scenario["current"].as_u64().unwrap()),
                        default_current: scenario["current"].as_u64().unwrap(),
                        page_size: None,
                        default_page_size: 10,
                        settings: PaginationSettings {
                            mode: if simple {
                                PaginationMode::Simple
                            } else {
                                PaginationMode::Full
                            },
                            quick_jumper: if kind == "quick-go" {
                                QuickJumper::Confirm
                            } else if kind == "quick" {
                                QuickJumper::Blur
                            } else {
                                QuickJumper::Hidden
                            },
                            ..Default::default()
                        },
                        disabled: scenario["disabled"].as_bool(),
                        show_size_changer: true,
                        options_visible: true,
                        narrow: false,
                    },
                    state.scope.clone().unwrap(),
                    window,
                    cx,
                );
            });
            for observation in trace["observations"].as_array().unwrap() {
                let action = observation["action"].as_str().unwrap();
                if name == "quick-blur-to-page" && action == "first" {
                    fixture.update(|state, window, cx| state.focus_page(1, window, cx));
                    fixture.commit(false, CommitReason::Blur);
                }
                match action {
                    "initial" => {}
                    "input" => fixture.input(
                        false,
                        match name.strip_prefix("quick-enter-").unwrap() {
                            "empty" => "",
                            text => text,
                        },
                        false,
                    ),
                    "input-12" => fixture.input(simple, "12", false),
                    "input-overflow" => fixture.input(true, "9999", false),
                    "clear" => fixture.input(true, "", false),
                    "enter" | "enter-overflow" | "enter-empty" => {
                        fixture.commit(simple, CommitReason::Enter)
                    }
                    "blur-outside" => fixture.commit(false, CommitReason::Blur),
                    "go" => fixture.update(|state, window, cx| state.commit_quick(window, cx)),
                    "up" | "down" => fixture.update(|state, window, cx| {
                        state.step_simple(action == "down", window, cx)
                    }),
                    "size-100" | "size-20" => fixture.update(|state, window, cx| {
                        state.change_page_size(
                            if action == "size-100" { 100 } else { 20 },
                            window,
                            cx,
                        )
                    }),
                    action => fixture.update(|state, window, cx| {
                        let (item, source) = match action {
                            "click-current" => (Item::Page(state.current()), Source::Page),
                            "next" | "next-again" => (Item::Next, Source::Next),
                            "prev" | "disabled-prev" => (Item::Previous, Source::Previous),
                            "jump-next" => (Item::JumpNext, Source::JumpNext),
                            "jump-prev" => (Item::JumpPrevious, Source::JumpPrevious),
                            "first" | "page" => (Item::Page(1), Source::Page),
                            _ => panic!("Unrecognized reference action: {action}"),
                        };
                        state.activate(item, source, window, cx);
                    }),
                }
                if controlled == Some("echo") {
                    let accepted = fixture.events.borrow().iter().rev().find_map(|event| {
                        if let PaginationEvent::Change(change) = event {
                            Some(change.current)
                        } else {
                            None
                        }
                    });
                    if let Some(current) = accepted {
                        fixture.update(|state, window, cx| state.set_current(current, window, cx));
                    }
                }
                let observed = &observation["observed"];
                fixture.update(|state, _, cx| {
                    if let Some(active) = observed["active"].as_u64() {
                        assert_eq!(state.current(), active, "{name}/{action}");
                    }
                    if let Some(simple) = observed["simple"].as_str() {
                        assert_eq!(
                            state.simple_editor.read(cx).value(),
                            simple,
                            "{name}/{action}"
                        );
                    }
                    if let Some(quick) = observed["quick"].as_str() {
                        assert_eq!(
                            state.quick_editor.read(cx).value(),
                            quick,
                            "{name}/{action}"
                        );
                    }
                });
                let actual = fixture.events.borrow().iter().map(|event| match event {
                    PaginationEvent::Change(change) => serde_json::json!({"type":"change","args":[change.current,change.page_size]}),
                    PaginationEvent::ShowSizeChange(change) => serde_json::json!({"type":"size-change","args":[change.current,change.page_size]}),
                }).collect::<Vec<_>>();
                assert_eq!(
                    actual,
                    *observed["events"].as_array().unwrap(),
                    "{name}/{action}"
                );
            }
        }
    }

    #[test]
    fn pagination_retained_control_flags_are_independent_and_size_event_order_matches_reference() {
        let mut fixture = Fixture::new();
        fixture.update(|state, window, cx| {
            assert_eq!(state.current(), 6);
            state.controlled_current = true;
            state.request_page(9, Source::Page, window, cx);
            assert_eq!(state.current(), 6);
            state.change_page_size(100, window, cx);
            assert_eq!(state.page_size(), 100);
            assert_eq!(state.current(), 5);
        });
        let events = fixture.events.borrow();
        assert!(
            matches!(events[0], PaginationEvent::Change(change) if change.current == 9 && change.page_size == 10)
        );
        assert!(
            matches!(events[1], PaginationEvent::ShowSizeChange(change) if change.current == 6 && change.page_size == 100)
        );
        assert!(
            matches!(events[2], PaginationEvent::Change(change) if change.current == 5 && change.page_size == 100 && change.source == Source::PageSize)
        );
        drop(events);
        fixture.update(|state, window, cx| {
            state.controlled_current = false;
            state.controlled_page_size = true;
            state.request_page(2, Source::Next, window, cx);
            state.change_page_size(200, window, cx);
            assert_eq!(state.current(), 2);
            assert_eq!(state.page_size(), 100);
        });
    }

    #[test]
    fn pagination_retained_all_control_combinations_preserve_owner_values() {
        for controlled_current in [false, true] {
            for controlled_size in [false, true] {
                let mut fixture = Fixture::new();
                fixture.update(|state, window, cx| {
                    state.controlled_current = controlled_current;
                    state.controlled_page_size = controlled_size;
                    state.change_page_size(100, window, cx);
                    assert_eq!(state.page_size(), if controlled_size { 10 } else { 100 });
                    assert_eq!(
                        state.current(),
                        if controlled_current && controlled_size {
                            6
                        } else {
                            5
                        }
                    );
                });
                let events = fixture.events.borrow();
                assert!(
                    matches!(events[0], PaginationEvent::ShowSizeChange(change) if change.current == 6 && change.page_size == 100)
                );
                assert!(
                    matches!(events[1], PaginationEvent::Change(change) if change.current == 5 && change.page_size == 100)
                );
            }
        }
    }

    #[test]
    fn pagination_retained_programmatic_initial_values_do_not_suppress_other_defaults() {
        let mut fixture = Fixture::new();
        fixture.update(|existing, window, cx| {
            let scope = existing.scope.clone().unwrap();
            for seed_current in [false, true] {
                let state = cx.new(|cx| {
                    let mut state = PaginationState::new(window, cx);
                    if seed_current {
                        state.set_current(3, window, cx);
                    } else {
                        state.set_page_size(20, window, cx);
                    }
                    state.mounted = true;
                    state.sync_render(
                        PaginationRenderConfig {
                            total: 500,
                            current: None,
                            default_current: 4,
                            page_size: None,
                            default_page_size: 50,
                            settings: Default::default(),
                            disabled: None,
                            show_size_changer: true,
                            options_visible: true,
                            narrow: false,
                        },
                        scope.clone(),
                        window,
                        cx,
                    );
                    state
                });
                assert_eq!(state.read(cx).current(), if seed_current { 3 } else { 4 });
                assert_eq!(
                    state.read(cx).page_size(),
                    if seed_current { 50 } else { 20 }
                );
            }
        });
    }

    #[test]
    fn pagination_retained_programmatic_updates_are_silent_and_restore_raw_current() {
        let mut fixture = Fixture::new();
        fixture.update(|state, window, cx| {
            state.set_total(20, window, cx);
            assert_eq!(state.current(), 2);
            state.set_total(500, window, cx);
            assert_eq!(state.current(), 6);
            state.set_current(8, window, cx);
            state.set_page_size(20, window, cx);
            assert_eq!(state.current(), 8);
            assert_eq!(state.range(), (141, 160));
        });
        assert!(fixture.events.borrow().is_empty());
    }

    #[test]
    fn pagination_retained_focus_storage_stays_bounded_and_stale_actions_are_rejected() {
        let mut fixture = Fixture::new();
        fixture.update(|state, window, cx| {
            state.set_total(u64::MAX, window, cx);
            for current in 1..=200 {
                state.set_current(current, window, cx);
                assert!(state.items.len() <= 11);
            }
            let current = state.current();
            state.activate(Item::Page(1_000), Source::Page, window, cx);
            assert_eq!(state.current(), current);
            state.disabled_override = Some(true);
            state.request_page(2, Source::Page, window, cx);
            state.change_page_size(100, window, cx);
            assert_eq!(state.current(), current);
            state.disabled_override = Some(false);
            state.unmount(cx);
            state.activate(Item::Next, Source::Next, window, cx);
            assert_eq!(state.current(), current);
            assert!(!state.frame_pending);
        });
        assert!(fixture.events.borrow().is_empty());
    }

    #[test]
    fn pagination_retained_size_options_are_cached_deduplicated_and_include_current_size() {
        let mut fixture = Fixture::new();
        fixture.update(|state, window, cx| {
            state.sync_size_options(&[50, 10, 20, 10, 0], "/ page", cx);
            let options = state.size_selector.read(cx).options().clone();
            state.sync_size_options(&[50, 10, 20, 10, 0], "/ page", cx);
            assert!(options.same_collection(state.size_selector.read(cx).options()));
            state.set_page_size(15, window, cx);
            state.sync_size_options(&[50, 10, 20, 10, 0], "/ page", cx);
            let select = state.size_selector.read(cx);
            assert_eq!(
                select
                    .options()
                    .rows()
                    .iter()
                    .filter_map(|row| row.option().map(|option| option.value))
                    .collect::<Vec<_>>(),
                [10, 15, 20, 50]
            );
            assert_eq!(select.values()[0].value, 15);
        });
    }

    #[test]
    fn pagination_retained_quick_and_simple_editors_commit_once_and_respect_modes() {
        let mut fixture = Fixture::new();
        fixture.update(|state, window, cx| {
            state.model.settings.quick_jumper = QuickJumper::Blur;
            state.quick_editor.update(cx, |input, cx| {
                input.set_value("999999999999999999999", window, cx)
            });
            state.commit_quick(window, cx);
            assert_eq!(state.current(), 50);
            assert!(state.quick_editor.read(cx).value().is_empty());
            state.commit_quick(window, cx);
            state.model.settings.mode = PaginationMode::Simple;
            state
                .simple_editor
                .update(cx, |input, cx| input.set_value("3", window, cx));
            state.commit_simple(window, cx);
            assert_eq!(state.current(), 3);
            state.step_simple(false, window, cx);
            assert_eq!(state.current(), 2);
            state.step_simple(true, window, cx);
            assert_eq!(state.current(), 3);
            state.model.settings.mode = PaginationMode::SimpleReadOnly;
            state
                .simple_editor
                .update(cx, |input, cx| input.set_value("7", window, cx));
            state.commit_simple(window, cx);
            assert_eq!(state.current(), 3);
        });
        assert_eq!(fixture.events.borrow().len(), 4);
    }

    #[test]
    fn pagination_retained_jumper_blur_does_not_override_pager_focus_or_hidden_options() {
        let mut fixture = Fixture::new();
        fixture.update(|state, window, cx| {
            let scope = state.scope.clone().unwrap();
            state.sync_render(
                PaginationRenderConfig {
                    total: 500,
                    current: None,
                    default_current: 6,
                    page_size: None,
                    default_page_size: 10,
                    settings: PaginationSettings {
                        quick_jumper: QuickJumper::Blur,
                        ..Default::default()
                    },
                    disabled: None,
                    show_size_changer: true,
                    options_visible: true,
                    narrow: false,
                },
                scope,
                window,
                cx,
            );
        });
        fixture.input(false, "12", false);
        fixture.update(|state, window, cx| state.focus_page(5, window, cx));
        fixture.commit(false, CommitReason::Blur);
        fixture.update(|state, _, cx| {
            assert_eq!(state.current(), 6);
            assert!(state.quick_editor.read(cx).value().is_empty());
        });
        assert!(fixture.events.borrow().is_empty());
        fixture.input(false, "12", false);
        fixture.update(|state, window, cx| {
            state.options_visible = false;
            state.blur(window, cx);
        });
        fixture.commit(false, CommitReason::Blur);
        assert!(fixture.events.borrow().is_empty());
    }

    #[test]
    fn pagination_retained_native_composition_waits_for_final_text() {
        let mut fixture = Fixture::new();
        fixture.update(|state, window, cx| {
            let scope = state.scope.clone().unwrap();
            state.sync_render(
                PaginationRenderConfig {
                    total: 500,
                    current: None,
                    default_current: 6,
                    page_size: None,
                    default_page_size: 10,
                    settings: PaginationSettings {
                        quick_jumper: QuickJumper::Blur,
                        ..Default::default()
                    },
                    disabled: None,
                    show_size_changer: true,
                    options_visible: true,
                    narrow: false,
                },
                scope,
                window,
                cx,
            );
            state.quick_editor.update(cx, |input, cx| {
                input.replace_and_mark_text_in_range(None, "12", Some(2..2), window, cx)
            });
        });
        fixture.update(|state, window, cx| {
            assert!(state.quick_editor.read(cx).is_composing());
            state.commit_quick(window, cx);
            assert_eq!(state.current(), 6);
            assert_eq!(state.quick_editor.read(cx).value(), "12");
            state.quick_editor.update(cx, |input, cx| {
                input.replace_text_in_range(None, "12", window, cx)
            });
        });
        fixture.commit(false, CommitReason::Enter);
        fixture.update(|state, _, cx| {
            assert_eq!(state.current(), 12);
            assert!(!state.quick_editor.read(cx).is_composing());
            assert!(state.quick_editor.read(cx).value().is_empty());
        });
        assert_eq!(fixture.events.borrow().len(), 1);
    }

    #[test]
    fn pagination_retained_scope_disable_blocks_stale_callbacks_and_cancels_guards() {
        let mut fixture = Fixture::new();
        fixture.update(|state, _, cx| {
            state
                .scope
                .as_ref()
                .unwrap()
                .update_patch(cx, |patch| patch.disabled = Some(true))
                .unwrap();
        });
        fixture.update(|state, window, cx| {
            assert!(
                state
                    .items
                    .values()
                    .all(|item| !item.guard.availability().can_activate())
            );
            assert!(state.quick_editor.read(cx).is_disabled());
            state.activate(Item::Next, Source::Next, window, cx);
            state.request_page(10, Source::QuickJumper, window, cx);
            state.change_page_size(100, window, cx);
            assert_eq!(state.current(), 6);
            assert_eq!(state.page_size(), 10);
        });
        assert!(fixture.events.borrow().is_empty());
    }
}
