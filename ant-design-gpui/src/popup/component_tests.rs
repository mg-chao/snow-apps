// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;
use crate::{
    Button, ConfigPatch, ConfigScope, Popconfirm, Popover, Tooltip, UiRoot,
    behavior::focus_handle,
    popconfirm::{Confirmation, PopconfirmButtonOptions},
};
use gpui::{
    AnyElement, AppContext, Context, Entity, FocusHandle, IntoElement, KeyDownEvent, KeyUpEvent,
    Keystroke, PlatformInput, Render, TestAppContext, Window, WindowHandle, div, prelude::*, px,
};
use std::{
    cell::{Cell, RefCell},
    rc::Rc,
    time::Duration,
};

#[derive(Clone, Copy)]
enum Kind {
    Tooltip,
    Popover,
    Popconfirm,
}
#[derive(Clone, Copy)]
enum ResultKind {
    Close,
    Keep,
    Success,
    Failure,
}

struct Consumer {
    scope: ConfigScope,
    kind: Kind,
    handle: PopupHandle,
    trigger: FocusHandle,
    mounted: bool,
    empty: bool,
    show_cancel: bool,
    icon_style: gpui::StyleRefinement,
    disabled_cancel: bool,
    explicit_triggers: Option<PopupTriggers>,
    result: Rc<Cell<ResultKind>>,
    delay: Rc<Cell<u64>>,
    started: Rc<Cell<usize>>,
    finished: Rc<Cell<usize>>,
    canceled: Rc<Cell<usize>>,
    errors: Rc<RefCell<Vec<String>>>,
    changes: Rc<RefCell<Vec<PopupOpenChange>>>,
}
impl Render for Consumer {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        let mut root = div()
            .size_full()
            .flex()
            .flex_col()
            .items_start()
            .p(px(100.));
        if self.mounted {
            let trigger = Button::new("trigger")
                .label("Open")
                .focus_handle(&self.trigger)
                .w(px(80.))
                .h(px(32.));
            let element: AnyElement = match self.kind {
                Kind::Tooltip => {
                    let mut popup = Tooltip::new("control")
                        .scope(self.scope.clone())
                        .handle(&self.handle)
                        .child(trigger)
                        .title(if self.empty { "" } else { "Useful detail" });
                    if let Some(triggers) = self.explicit_triggers {
                        popup = popup.triggers(triggers);
                    }
                    popup.into_any_element()
                }
                Kind::Popover => {
                    let mut popup = Popover::new("control")
                        .scope(self.scope.clone())
                        .handle(&self.handle)
                        .child(trigger);
                    if !self.empty {
                        popup = popup.title("Title").text("Content");
                    }
                    if let Some(triggers) = self.explicit_triggers {
                        popup = popup.triggers(triggers);
                    }
                    popup.into_any_element()
                }
                Kind::Popconfirm => {
                    let result = self.result.clone();
                    let delay = self.delay.clone();
                    let started = self.started.clone();
                    let finished = self.finished.clone();
                    let canceled = self.canceled.clone();
                    let errors = self.errors.clone();
                    let changes = self.changes.clone();
                    Popconfirm::new("control")
                        .scope(self.scope.clone())
                        .handle(&self.handle)
                        .child(trigger)
                        .title("Delete the item?")
                        .description("This operation cannot be undone.")
                        .show_cancel(self.show_cancel)
                        .icon_style(self.icon_style.clone())
                        .cancel_button(PopconfirmButtonOptions {
                            disabled: self.disabled_cancel,
                            ..Default::default()
                        })
                        .on_confirm(move |_, _, cx| {
                            started.set(started.get() + 1);
                            let result = result.get();
                            match result {
                                ResultKind::Close => Confirmation::Close,
                                ResultKind::Keep => Confirmation::KeepOpen,
                                ResultKind::Success | ResultKind::Failure => {
                                    let timer = cx
                                        .background_executor()
                                        .timer(Duration::from_millis(delay.get()));
                                    let finished = finished.clone();
                                    Confirmation::Pending(cx.spawn(async move |_| {
                                        timer.await;
                                        finished.set(finished.get() + 1);
                                        if matches!(result, ResultKind::Success) {
                                            Ok(())
                                        } else {
                                            Err("Please retry".into())
                                        }
                                    }))
                                }
                            }
                        })
                        .on_cancel(move |_, _, _| canceled.set(canceled.get() + 1))
                        .on_confirm_error(move |error, _, _| {
                            errors.borrow_mut().push(error.to_string())
                        })
                        .on_open_change(move |change, _, _| changes.borrow_mut().push(*change))
                        .into_any_element()
                }
            };
            root = root.child(element);
        }
        root.child(Button::new("after").label("After"))
    }
}

struct Fixture {
    cx: TestAppContext,
    window: WindowHandle<UiRoot>,
    consumer: Entity<Consumer>,
}
impl Fixture {
    fn new(kind: Kind, patch: ConfigPatch, edit: impl FnOnce(&mut Consumer)) -> Self {
        let mut cx = TestAppContext::single();
        let mut consumer = None;
        let window = cx.add_window(|window, cx| {
            window.activate_window();
            let scope = ConfigScope::new_window(cx);
            scope
                .set_patch(
                    ConfigPatch {
                        motion: Some(false),
                        ..patch
                    },
                    cx,
                )
                .unwrap();
            let view = cx.new(|cx| {
                let mut state = Consumer {
                    scope: scope.clone(),
                    kind,
                    handle: PopupHandle::new(),
                    trigger: focus_handle(cx),
                    mounted: true,
                    empty: false,
                    show_cancel: true,
                    icon_style: Default::default(),
                    disabled_cancel: false,
                    explicit_triggers: None,
                    result: Rc::new(Cell::new(ResultKind::Close)),
                    delay: Rc::new(Cell::new(200)),
                    started: Rc::default(),
                    finished: Rc::default(),
                    canceled: Rc::default(),
                    errors: Rc::default(),
                    changes: Rc::default(),
                };
                edit(&mut state);
                state
            });
            consumer = Some(view.clone());
            UiRoot::new(scope, view, window, cx)
        });
        let mut fixture = Self {
            cx,
            window,
            consumer: consumer.unwrap(),
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
    fn key(&mut self, key: &'static str) {
        let keystroke = Keystroke::parse(key).unwrap();
        for event in [
            PlatformInput::KeyDown(KeyDownEvent {
                keystroke: keystroke.clone(),
                is_held: false,
                prefer_character_input: false,
            }),
            PlatformInput::KeyUp(KeyUpEvent { keystroke }),
        ] {
            self.cx
                .update_window(self.window.into(), |_, window, cx| {
                    window.dispatch_event(event, cx)
                })
                .unwrap();
        }
        self.draw();
    }
    fn handle(&self) -> PopupHandle {
        self.consumer
            .read_with(&self.cx, |state, _| state.handle.clone())
    }
    fn open(&mut self) -> bool {
        let handle = self.handle();
        self.cx.update(|cx| handle.is_open(cx))
    }
    fn set_open(&mut self, open: bool) {
        let handle = self.handle();
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                handle.set_open(open, window, cx)
            })
            .unwrap();
        self.draw();
    }
    fn edit(&mut self, edit: impl FnOnce(&mut Consumer)) {
        self.consumer.update(&mut self.cx, |state, cx| {
            edit(state);
            cx.notify();
        });
        self.draw();
    }
    fn advance(&mut self, ms: u64) {
        self.cx.executor().advance_clock(Duration::from_millis(ms));
        self.draw();
    }
    fn count(&self, field: impl FnOnce(&Consumer) -> &Cell<usize>) -> usize {
        self.consumer
            .read_with(&self.cx, |state, _| field(state).get())
    }
    fn keyboard_open(&mut self) {
        self.key("tab");
        assert!(!self.open());
        self.key("space");
        assert!(self.open());
    }
    fn confirm(&mut self) {
        self.key("tab");
        self.key("enter");
    }
}

#[test]
fn popup_surface_semantics_include_description_and_live_pending_state() {
    let popover = Fixture::new(Kind::Popover, ConfigPatch::default(), |_| {});
    let handle = popover.handle();
    popover.cx.update(|cx| {
        let state = handle.state().unwrap().upgrade().unwrap();
        assert_eq!(
            state.read(cx).options.description.as_deref(),
            Some("Content")
        );
    });
    let mut f = Fixture::new(Kind::Popconfirm, ConfigPatch::default(), |state| {
        state.result.set(ResultKind::Failure);
    });
    let handle = f.handle();
    f.keyboard_open();
    f.confirm();
    f.cx.update(|cx| {
        let state = handle.state().unwrap().upgrade().unwrap();
        assert_eq!(state.read(cx).options.role, gpui::Role::AlertDialog);
        assert_eq!(
            state.read(cx).options.description.as_deref(),
            Some("This operation cannot be undone.")
        );
        assert!(state.read(cx).options.busy);
    });
    f.advance(200);
    f.cx.update(|cx| {
        assert!(
            !handle
                .state()
                .unwrap()
                .upgrade()
                .unwrap()
                .read(cx)
                .options
                .busy
        );
    });
}

#[test]
fn popconfirm_actions_align_to_the_logical_end_in_both_directions() {
    for direction in [crate::Direction::Ltr, crate::Direction::Rtl] {
        let mut f = Fixture::new(
            Kind::Popconfirm,
            ConfigPatch {
                direction: Some(direction),
                ..Default::default()
            },
            |_| {},
        );
        f.set_open(true);
        let handle = f.handle();
        let primary = f.consumer.read_with(&f.cx, |state, cx| {
            state
                .scope
                .snapshot(cx)
                .theme
                .tokens
                .color_primary
                .to_hsla()
        });
        f.cx.update_window(f.window.into(), |_, window, cx| {
            let body = handle.layout(cx).unwrap().bounds;
            let scale = window.scale_factor();
            let confirm = window
                .rendered_scene()
                .quads
                .iter()
                .find(|quad| quad.background.as_solid() == Some(primary))
                .expect("native primary confirmation button");
            let edge = if direction == crate::Direction::Ltr {
                (confirm.bounds.origin.x.0 + confirm.bounds.size.width.0) / scale
                    - f32::from(body.right())
                    + 12.
            } else {
                confirm.bounds.origin.x.0 / scale - f32::from(body.left()) - 12.
            };
            assert!(
                edge.abs() < 1.,
                "direction={direction:?}, edge error={edge}"
            );
        })
        .unwrap();
    }
}

#[test]
fn popconfirm_semantic_icon_size_participates_in_native_layout() {
    let mut f = Fixture::new(Kind::Popconfirm, ConfigPatch::default(), |_| {});
    f.set_open(true);
    let handle = f.handle();
    let before = f.cx.update(|cx| handle.layout(cx).unwrap().bounds);
    f.edit(|state| state.icon_style = gpui::StyleRefinement::default().text_size(px(64.)));
    let after = f.cx.update(|cx| handle.layout(cx).unwrap().bounds);
    assert!(
        after.size.height > before.size.height,
        "a larger custom icon must not overlap the actions"
    );
    assert!(after.size.height >= px(64. + 24. + 24. + 8.));
}

#[test]
fn tooltip_and_popover_defaults_open_on_keyboard_focus_and_empty_content_suppresses() {
    for kind in [Kind::Tooltip, Kind::Popover] {
        let mut f = Fixture::new(kind, ConfigPatch::default(), |_| {});
        f.key("tab");
        assert!(f.open());
        let handle = f.handle();
        let bounds = f.cx.update(|cx| handle.layout(cx).unwrap().bounds);
        assert!(bounds.size.width > px(20.));
        f.edit(|state| state.empty = true);
        assert!(!f.open());
        f.set_open(true);
        assert!(!f.open());
    }
}

#[test]
fn popup_component_explicit_values_override_scoped_defaults() {
    let patch = ConfigPatch {
        tooltip: TooltipConfigPatch {
            popup: PopupConfigPatch {
                triggers: Some(PopupTriggers::NONE),
                arrow: Some(false.into()),
                mouse_enter_delay: Some(Duration::from_secs(1)),
                ..Default::default()
            },
            ..Default::default()
        },
        ..Default::default()
    };
    let mut f = Fixture::new(Kind::Tooltip, patch, |state| {
        state.explicit_triggers = Some(PopupTriggers::FOCUS)
    });
    f.key("tab");
    assert!(f.open());
    let handle = f.handle();
    f.cx.update(|cx| {
        let state = handle.state().unwrap().upgrade().unwrap();
        let state = state.read(cx);
        assert_eq!(state.options.triggers, PopupTriggers::FOCUS);
        assert!(!state.options.arrow.visible);
        assert_eq!(state.options.enter_delay, Duration::from_secs(1));
        assert_eq!(state.options.z_index, 1070);
        assert_eq!(state.options.role, gpui::Role::Tooltip);
    });
}

#[test]
fn popconfirm_keyboard_cancel_and_confirm_restore_trigger_focus() {
    let mut f = Fixture::new(Kind::Popconfirm, ConfigPatch::default(), |_| {});
    f.keyboard_open();
    f.key("enter");
    assert!(!f.open());
    assert_eq!(f.count(|state| &state.canceled), 1);
    assert_eq!(f.count(|state| &state.started), 0);
    f.key("space");
    assert!(f.open(), "Cancel should return focus to the trigger");
    f.confirm();
    assert!(!f.open());
    assert_eq!(f.count(|state| &state.started), 1);
    f.key("space");
    assert!(f.open(), "Confirm should return focus to the trigger");
}

#[test]
fn popconfirm_initial_focus_falls_back_to_confirm_without_available_cancel() {
    for hidden in [false, true] {
        let mut f = Fixture::new(Kind::Popconfirm, ConfigPatch::default(), |state| {
            state.show_cancel = !hidden;
            state.disabled_cancel = !hidden;
        });
        f.keyboard_open();
        f.key("enter");
        assert!(!f.open());
        assert_eq!(f.count(|state| &state.started), 1);
    }
}

#[test]
fn popconfirm_async_success_is_single_flight_and_closes_only_after_completion() {
    let mut f = Fixture::new(Kind::Popconfirm, ConfigPatch::default(), |state| {
        state.result.set(ResultKind::Success)
    });
    f.keyboard_open();
    f.confirm();
    assert!(f.open());
    f.key("enter");
    f.key("space");
    assert_eq!(f.count(|state| &state.started), 1);
    f.advance(199);
    assert!(f.open());
    f.advance(1);
    assert!(!f.open());
    assert_eq!(f.count(|state| &state.finished), 1);
}

#[test]
fn popconfirm_async_rejection_keeps_open_and_allows_retry() {
    let mut f = Fixture::new(Kind::Popconfirm, ConfigPatch::default(), |state| {
        state.result.set(ResultKind::Failure)
    });
    f.keyboard_open();
    f.confirm();
    f.advance(200);
    assert!(f.open());
    assert_eq!(
        f.consumer
            .read_with(&f.cx, |state, _| state.errors.borrow().clone()),
        ["Please retry"]
    );
    f.edit(|state| state.result.set(ResultKind::Success));
    f.key("enter");
    assert_eq!(f.count(|state| &state.started), 2);
    f.advance(200);
    assert!(!f.open());
}

#[test]
fn popconfirm_keep_open_callback_is_repeatable() {
    let mut f = Fixture::new(Kind::Popconfirm, ConfigPatch::default(), |state| {
        state.result.set(ResultKind::Keep)
    });
    f.keyboard_open();
    f.confirm();
    f.key("enter");
    assert!(f.open());
    assert_eq!(f.count(|state| &state.started), 2);
}

#[test]
fn popconfirm_dismissal_does_not_cancel_application_task_or_close_a_later_opening() {
    let mut f = Fixture::new(Kind::Popconfirm, ConfigPatch::default(), |state| {
        state.result.set(ResultKind::Success)
    });
    f.keyboard_open();
    f.confirm();
    f.key("escape");
    assert!(!f.open());
    f.edit(|state| state.delay.set(1000));
    f.key("space");
    f.confirm();
    assert_eq!(f.count(|state| &state.started), 2);
    f.advance(200);
    assert_eq!(f.count(|state| &state.finished), 1);
    assert!(f.open(), "old success must not close a later opening");
    f.key("enter");
    assert_eq!(
        f.count(|state| &state.started),
        2,
        "old completion must not clear the newer spinner"
    );
    f.advance(800);
    assert!(!f.open());
}

#[test]
fn popconfirm_unmount_does_not_cancel_application_task() {
    let mut f = Fixture::new(Kind::Popconfirm, ConfigPatch::default(), |state| {
        state.result.set(ResultKind::Success)
    });
    f.keyboard_open();
    f.confirm();
    f.edit(|state| state.mounted = false);
    f.advance(200);
    assert_eq!(f.count(|state| &state.finished), 1);
    assert!(!f.open());
}

struct UniqueConsumer {
    scopes: [ConfigScope; 2],
    handles: [PopupHandle; 2],
    controlled_first: bool,
}
impl Render for UniqueConsumer {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        let mut row = div().flex().gap(px(40.)).p(px(100.));
        for i in 0..2 {
            let mut tooltip = Tooltip::new(("unique", i))
                .scope(self.scopes[i].clone())
                .handle(&self.handles[i])
                .title("Unique detail")
                .child(Button::new(("trigger", i)).label("Hover"));
            if i == 0 && self.controlled_first {
                tooltip = tooltip.open(true);
            }
            row = row.child(tooltip);
        }
        row
    }
}

#[gpui::test]
fn tooltip_unique_groups_are_window_local_and_follow_scope_boundaries(cx: &mut TestAppContext) {
    let scope = cx.update(|cx| {
        let scope = ConfigScope::new_window(cx);
        scope
            .set_patch(
                ConfigPatch {
                    motion: Some(false),
                    tooltip: TooltipConfigPatch {
                        unique: Some(true),
                        ..Default::default()
                    },
                    ..Default::default()
                },
                cx,
            )
            .unwrap();
        scope
    });
    let first = [PopupHandle::new(), PopupHandle::new()];
    let second = [PopupHandle::new(), PopupHandle::new()];
    let window_a = cx.add_window(|window, cx| {
        let child = scope.child(ConfigPatch::default(), cx).unwrap();
        let consumer = cx.new(|_| UniqueConsumer {
            scopes: [scope.clone(), child],
            handles: first.clone(),
            controlled_first: false,
        });
        UiRoot::new(scope.clone(), consumer, window, cx)
    });
    let window_b = cx.add_window(|window, cx| {
        let child = scope
            .child(
                ConfigPatch {
                    tooltip: TooltipConfigPatch {
                        unique: Some(false),
                        ..Default::default()
                    },
                    ..Default::default()
                },
                cx,
            )
            .unwrap();
        let consumer = cx.new(|_| UniqueConsumer {
            scopes: [scope.clone(), child],
            handles: second.clone(),
            controlled_first: false,
        });
        UiRoot::new(scope.clone(), consumer, window, cx)
    });
    for window in [window_a, window_b] {
        cx.update_window(window.into(), |_, window, cx| window.draw(cx).clear(cx))
            .unwrap();
    }
    cx.update_window(window_a.into(), |_, window, cx| {
        first[0].set_open(true, window, cx)
    })
    .unwrap();
    cx.update_window(window_b.into(), |_, window, cx| {
        second[0].set_open(true, window, cx)
    })
    .unwrap();
    cx.update(|cx| {
        assert!(first[0].is_open(cx));
        assert!(second[0].is_open(cx));
    });
    cx.update_window(window_a.into(), |_, window, cx| {
        first[1].set_open(true, window, cx)
    })
    .unwrap();
    cx.update_window(window_b.into(), |_, window, cx| {
        second[1].set_open(true, window, cx)
    })
    .unwrap();
    for window in [window_a, window_b] {
        cx.update_window(window.into(), |_, window, cx| window.draw(cx).clear(cx))
            .unwrap();
    }
    cx.update(|cx| {
        assert!(!first[0].is_open(cx));
        assert!(first[1].is_open(cx));
        assert!(first[1].layout(cx).unwrap().arrow.is_none());
        assert!(second[0].is_open(cx));
        assert!(second[1].is_open(cx));
        assert!(second[1].layout(cx).unwrap().arrow.is_some());
    });
}

#[gpui::test]
fn tooltip_unique_replacement_suppresses_an_unaccepted_controlled_surface(cx: &mut TestAppContext) {
    let handles = [PopupHandle::new(), PopupHandle::new()];
    let window = cx.add_window(|window, cx| {
        let scope = ConfigScope::new_window(cx);
        scope
            .set_patch(
                ConfigPatch {
                    motion: Some(false),
                    tooltip: TooltipConfigPatch {
                        unique: Some(true),
                        ..Default::default()
                    },
                    ..Default::default()
                },
                cx,
            )
            .unwrap();
        let consumer = cx.new(|_| UniqueConsumer {
            scopes: [scope.clone(), scope.clone()],
            handles: handles.clone(),
            controlled_first: true,
        });
        UiRoot::new(scope, consumer, window, cx)
    });
    cx.update_window(window.into(), |_, window, cx| window.draw(cx).clear(cx))
        .unwrap();
    cx.update_window(window.into(), |_, window, cx| {
        handles[1].set_open(true, window, cx);
        window.draw(cx).clear(cx);
    })
    .unwrap();
    cx.update(|cx| {
        assert!(
            handles[0].is_open(cx),
            "controlled value stays authoritative"
        );
        assert!(
            handles[0].layout(cx).is_none(),
            "only the active unique owner has a surface"
        );
        assert!(handles[1].layout(cx).is_some());
    });
}
