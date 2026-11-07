// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::{
    cell::{Cell, RefCell},
    rc::Rc,
    time::Duration,
};

use gpui::{
    AppContext, Context, Entity, FocusHandle, IntoElement, KeyDownEvent, KeyUpEvent, Keystroke,
    MouseButton, MouseDownEvent, MouseMoveEvent, MouseUpEvent, PlatformInput, Render,
    StyleRefinement, TestAppContext, Window, WindowHandle, div, point, prelude::*, px,
};

use super::*;
use crate::{Button, ConfigPatch, ConfigScope, UiRoot, behavior::focus_handle};

pub(super) struct Consumer {
    scope: ConfigScope,
    handle: PopupHandle,
    nested_handle: PopupHandle,
    trigger_focus: FocusHandle,
    inner_focus: FocusHandle,
    outside_focus: FocusHandle,
    changes: Rc<RefCell<Vec<PopupOpenChange>>>,
    clicks: Rc<Cell<usize>>,
    outside_clicks: Rc<Cell<usize>>,
    popup_clicks: Rc<Cell<usize>>,
    content_builds: Rc<Cell<usize>>,
    hidden_calls: Rc<Cell<usize>>,
    presented_calls: Rc<Cell<usize>>,
    pub(super) triggers: PopupTriggers,
    pub(super) controlled: Option<bool>,
    mounted: bool,
    nested: bool,
    clipped: bool,
    pub(super) initial_focus: bool,
    width: PopupWidth,
    trigger_width: f32,
    trigger_disabled: bool,
    trigger_loading: bool,
    captures: Rc<RefCell<Vec<std::rc::Weak<()>>>>,
}

impl Render for Consumer {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        let mut root = div()
            .size_full()
            .flex()
            .flex_col()
            .items_start()
            .gap(px(80.))
            .p(px(100.));
        if self.mounted {
            let changes = self.changes.clone();
            let clicks = self.clicks.clone();
            let popup_clicks = self.popup_clicks.clone();
            let builds = self.content_builds.clone();
            let hidden = self.hidden_calls.clone();
            let presented = self.presented_calls.clone();
            let hidden_handle = self.handle.clone();
            let scope = self.scope.clone();
            let inner_focus = self.inner_focus.clone();
            let nested = self.nested;
            let nested_handle = self.nested_handle.clone();
            let capture = Rc::new(());
            self.captures.borrow_mut().push(Rc::downgrade(&capture));
            let mut popup = Popup::new("popup-test")
                .width(self.width)
                .scope(self.scope.clone())
                .on_presented(move |_, _| presented.set(presented.get() + 1))
                .on_hidden(move |cx| {
                    // Hidden consumers must run without a PopupState entity lease.
                    let _ = hidden_handle.is_open(cx);
                    hidden.set(hidden.get() + 1);
                })
                .handle(&self.handle)
                .triggers(self.triggers)
                .on_open_change(move |change, _, _| changes.borrow_mut().push(*change))
                .on_popup_click(move |_, _, _| popup_clicks.set(popup_clicks.get() + 1))
                .child(
                    Button::new("trigger-button")
                        .disabled(self.trigger_disabled)
                        .loading(self.trigger_loading)
                        .scope(self.scope.clone())
                        .focus_handle(&self.trigger_focus)
                        .label("Open")
                        .w(px(self.trigger_width))
                        .h(px(32.))
                        .on_click(move |_, _, _| clicks.set(clicks.get() + 1)),
                )
                .content(move |_, _, _| {
                    let _keep_alive = &capture;
                    builds.set(builds.get() + 1);
                    let body = div().w(px(160.)).h(px(64.));
                    if nested {
                        body.child(
                            Popup::new("nested-popup")
                                .scope(scope.clone())
                                .handle(&nested_handle)
                                .placement(PopupPlacement::Right)
                                .child(
                                    Button::new("nested-trigger")
                                        .scope(scope.clone())
                                        .label("Nested"),
                                )
                                .content(|_, _, _| {
                                    div().w(px(80.)).h(px(40.)).child("Nested body")
                                }),
                        )
                    } else {
                        body.child(
                            Button::new("inside")
                                .scope(scope.clone())
                                .focus_handle(&inner_focus)
                                .label("Inside"),
                        )
                    }
                });
            if let Some(open) = self.controlled {
                popup = popup.open(open);
            }
            if self.initial_focus {
                popup = popup.initial_focus(&self.inner_focus);
            }
            root = if self.clipped {
                root.child(div().w(px(80.)).h(px(32.)).overflow_hidden().child(popup))
            } else {
                root.child(popup)
            };
        }
        let clicks = self.outside_clicks.clone();
        root.child(
            Button::new("outside")
                .scope(self.scope.clone())
                .label("Outside")
                .focus_handle(&self.outside_focus)
                .w(px(80.))
                .h(px(32.))
                .on_click(move |_, _, _| clicks.set(clicks.get() + 1)),
        )
    }
}

pub(super) struct Fixture {
    pub(super) cx: TestAppContext,
    pub(super) window: WindowHandle<UiRoot>,
    consumer: Entity<Consumer>,
}

#[test]
fn popup_open_at_rejects_nonfinite_positions_and_realigns_the_same_session() {
    let mut f = Fixture::new(|consumer| consumer.triggers = PopupTriggers::CONTEXT_MENU);
    let handle = f.handle();
    for position in [
        point(px(f32::NAN), px(20.)),
        point(px(20.), px(f32::INFINITY)),
    ] {
        f.cx.update_window(f.window.into(), |_, window, cx| {
            handle.open_at(position, window, cx)
        })
        .unwrap();
        f.draw();
        assert!(!f.open());
    }
    f.cx.update_window(f.window.into(), |_, window, cx| {
        handle.open_at(point(px(240.), px(220.)), window, cx)
    })
    .unwrap();
    f.draw();
    assert!(f.open());
    let (first, session) =
        f.cx.update(|cx| (handle.layout(cx).unwrap(), handle.session_id(cx)));
    f.cx.update_window(f.window.into(), |_, window, cx| {
        handle.open_at(point(px(320.), px(300.)), window, cx)
    })
    .unwrap();
    f.draw();
    let (second, new_session) =
        f.cx.update(|cx| (handle.layout(cx).unwrap(), handle.session_id(cx)));
    assert_ne!(first.bounds.origin, second.bounds.origin);
    assert_eq!(session, new_session);
    assert_eq!(
        f.changes().len(),
        1,
        "repositioning does not emit another opening"
    );
    f.cx.update_window(f.window.into(), |_, window, cx| {
        handle.open_at(point(px(f32::NEG_INFINITY), px(20.)), window, cx)
    })
    .unwrap();
    f.draw();
    assert_eq!(f.cx.update(|cx| handle.layout(cx).unwrap()), second);
    f.set_open(false);
    assert!(!f.open());
}

#[test]
fn trigger_width_policy_resolves_and_resizes_in_the_same_layout_pass() {
    let mut f = Fixture::new(|c| {
        c.width = PopupWidth::Trigger;
        c.trigger_width = 180.;
    });
    f.set_open(true);
    let handle = f.handle();
    assert_eq!(
        f.cx.update(|cx| handle.layout(cx).unwrap().bounds.size.width),
        px(180.)
    );
    f.edit(|c| c.trigger_width = 240.);
    assert_eq!(
        f.cx.update(|cx| handle.layout(cx).unwrap().bounds.size.width),
        px(240.)
    );
    f.edit(|c| c.width = PopupWidth::Fixed(px(350.)));
    assert_eq!(
        f.cx.update(|cx| handle.layout(cx).unwrap().bounds.size.width),
        px(350.)
    );
    f.edit(|c| c.width = PopupWidth::TriggerMinimum(px(200.)));
    assert_eq!(
        f.cx.update(|cx| handle.layout(cx).unwrap().bounds.size.width),
        px(240.)
    );
    f.edit(|c| c.width = PopupWidth::TriggerMinimum(px(350.)));
    assert_eq!(
        f.cx.update(|cx| handle.layout(cx).unwrap().bounds.size.width),
        px(350.)
    );
    f.edit(|c| {
        c.width = PopupWidth::AtLeastTrigger;
        c.trigger_width = 400.;
    });
    assert!(f.cx.update(|cx| handle.layout(cx).unwrap().bounds.size.width) >= px(400.));
}

impl Fixture {
    pub(super) fn new(edit: impl FnOnce(&mut Consumer)) -> Self {
        Self::with_cache(edit, false)
    }
    fn with_cache(edit: impl FnOnce(&mut Consumer), cached: bool) -> Self {
        let mut cx = TestAppContext::single();
        let mut consumer = None;
        let window = cx.add_window(|window, cx| {
            window.activate_window();
            let scope = ConfigScope::new_window(cx);
            scope
                .set_patch(
                    ConfigPatch {
                        motion: Some(false),
                        ..Default::default()
                    },
                    cx,
                )
                .unwrap();
            let view = cx.new(|cx| {
                let mut state = Consumer {
                    scope: scope.clone(),
                    handle: PopupHandle::new(),
                    nested_handle: PopupHandle::new(),
                    trigger_focus: focus_handle(cx),
                    inner_focus: focus_handle(cx),
                    outside_focus: focus_handle(cx),
                    changes: Rc::default(),
                    clicks: Rc::default(),
                    outside_clicks: Rc::default(),
                    popup_clicks: Rc::default(),
                    content_builds: Rc::default(),
                    hidden_calls: Rc::default(),
                    presented_calls: Rc::default(),
                    triggers: PopupTriggers::CLICK,
                    controlled: None,
                    mounted: true,
                    nested: false,
                    clipped: false,
                    initial_focus: false,
                    width: PopupWidth::Content,
                    trigger_width: 80.,
                    trigger_disabled: false,
                    trigger_loading: false,
                    captures: Rc::default(),
                };
                edit(&mut state);
                state
            });
            consumer = Some(view.clone());
            if cached {
                let view = cx.new(|_| CachedConsumer(view));
                UiRoot::new(scope, view, window, cx)
            } else {
                UiRoot::new(scope, view, window, cx)
            }
        });
        let mut fixture = Self {
            cx,
            window,
            consumer: consumer.unwrap(),
        };
        fixture.draw();
        fixture
    }
    pub(super) fn draw(&mut self) {
        self.cx.run_until_parked();
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                window.draw(cx).clear(cx)
            })
            .unwrap();
        self.cx.run_until_parked();
    }
    fn edit(&mut self, edit: impl FnOnce(&mut Consumer)) {
        self.consumer.update(&mut self.cx, |state, cx| {
            edit(state);
            cx.notify();
        });
        self.draw();
    }
    pub(super) fn handle(&self) -> PopupHandle {
        self.consumer
            .read_with(&self.cx, |state, _| state.handle.clone())
    }
    pub(super) fn open(&mut self) -> bool {
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
    fn dispatch(&mut self, event: PlatformInput) {
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                window.dispatch_event(event, cx)
            })
            .unwrap();
    }
    fn click(&mut self, x: f32, y: f32) {
        let position = point(px(x), px(y));
        self.dispatch(PlatformInput::MouseDown(MouseDownEvent {
            button: MouseButton::Left,
            position,
            ..Default::default()
        }));
        self.dispatch(PlatformInput::MouseUp(MouseUpEvent {
            button: MouseButton::Left,
            position,
            ..Default::default()
        }));
        self.draw();
    }
    fn hover(&mut self, x: f32, y: f32) {
        self.dispatch(PlatformInput::MouseMove(MouseMoveEvent {
            position: point(px(x), px(y)),
            ..Default::default()
        }));
        self.draw();
    }
    fn advance(&mut self, millis: u64) {
        self.cx
            .executor()
            .advance_clock(Duration::from_millis(millis));
        self.draw();
    }
    fn key(&mut self, key: &'static str) {
        let keystroke = Keystroke::parse(key).unwrap();
        self.dispatch(PlatformInput::KeyDown(KeyDownEvent {
            keystroke: keystroke.clone(),
            is_held: false,
            prefer_character_input: false,
        }));
        self.dispatch(PlatformInput::KeyUp(KeyUpEvent { keystroke }));
        self.draw();
    }
    pub(super) fn changes(&self) -> Vec<PopupOpenChange> {
        self.consumer
            .read_with(&self.cx, |state, _| state.changes.borrow().clone())
    }
    fn focused(&mut self, select: impl FnOnce(&Consumer) -> FocusHandle) -> bool {
        let focus = self.consumer.read_with(&self.cx, |state, _| select(state));
        self.cx
            .update_window(self.window.into(), |_, window, _| focus.is_focused(window))
            .unwrap()
    }
}

#[test]
fn unavailable_trigger_blocks_activation_but_keeps_hover_explanation() {
    let mut f = Fixture::new(|state| state.trigger_disabled = true);
    f.click(120., 116.);
    assert!(!f.open());
    assert!(f.changes().is_empty());
    f.edit(|state| {
        state.trigger_disabled = false;
        state.trigger_loading = true;
    });
    f.click(120., 116.);
    assert!(!f.open());
    assert!(f.changes().is_empty());
    f.edit(|state| {
        state.trigger_loading = false;
    });
    f.click(120., 116.);
    assert!(f.open());
    f.set_open(false);
    f.edit(|state| {
        state.trigger_disabled = true;
        state.triggers = PopupTriggers::HOVER;
    });
    f.hover(125., 116.);
    f.advance(100);
    assert!(f.open(), "disabled controls retain their hover explanation");
}

#[test]
fn popup_content_click_callback_excludes_trigger_and_outside_clicks() {
    let mut f = Fixture::new(|_| {});
    f.click(120., 116.);
    assert!(f.open());
    let handle = f.handle();
    let bounds = f.cx.update(|cx| handle.layout(cx).unwrap().bounds);
    f.click(
        f32::from(bounds.right()) - 20.,
        f32::from(bounds.bottom()) - 20.,
    );
    assert!(f.open());
    assert_eq!(
        f.consumer
            .read_with(&f.cx, |state, _| state.popup_clicks.get()),
        1
    );
    f.click(120., 228.);
    assert_eq!(
        f.consumer
            .read_with(&f.cx, |state, _| state.popup_clicks.get()),
        1
    );
}

#[test]
fn popup_click_preserves_trigger_callback_and_outside_activation() {
    let mut f = Fixture::new(|_| {});
    assert!(!f.open());
    assert_eq!(
        f.consumer
            .read_with(&f.cx, |state, _| state.content_builds.get()),
        0
    );
    f.click(120., 116.);
    assert!(f.open());
    assert_eq!(
        f.consumer.read_with(&f.cx, |state, _| state.clicks.get()),
        1
    );
    let handle = f.handle();
    assert!(f.cx.update(|cx| handle.layout(cx)).is_some());
    f.click(120., 228.);
    assert!(!f.open());
    assert_eq!(
        f.consumer
            .read_with(&f.cx, |state, _| state.outside_clicks.get()),
        1
    );
    assert_eq!(
        f.changes().iter().map(|c| c.reason).collect::<Vec<_>>(),
        [PopupOpenReason::Click, PopupOpenReason::OutsidePress]
    );
}

#[test]
fn popup_shadow_raster_is_shared_while_visible_and_recreated_after_reopen() {
    let mut f = Fixture::new(|_| {});
    f.set_open(true);
    let warm = f.cx.update(|cx| popup_shadow_statistics(cx));
    assert_eq!(warm.misses, 1);
    assert!(warm.bytes > 0);
    let handle = f.handle();
    f.cx.update(|cx| handle.force_align(cx));
    f.draw();
    let aligned = f.cx.update(|cx| popup_shadow_statistics(cx));
    assert_eq!(aligned.misses, warm.misses);
    assert!(aligned.hits > warm.hits);
    f.set_open(false);
    assert_eq!(f.cx.update(|cx| popup_shadow_statistics(cx).bytes), 0);
    f.set_open(true);
    let reopened = f.cx.update(|cx| popup_shadow_statistics(cx));
    assert_eq!(reopened.misses, warm.misses + 1);
    assert_eq!(reopened.bytes, warm.bytes);
}

#[test]
fn popup_sessions_distinguish_closed_and_remounted_instances() {
    let mut f = Fixture::new(|_| {});
    let handle = f.handle();
    f.set_open(true);
    let first = f.cx.update(|cx| handle.session_id(cx)).unwrap();
    f.set_open(false);
    let closed = f.cx.update(|cx| handle.session_id(cx)).unwrap();
    assert_ne!(first, closed);
    f.edit(|state| state.mounted = false);
    assert!(f.cx.update(|cx| handle.session_id(cx)).is_none());
    f.edit(|state| state.mounted = true);
    f.set_open(true);
    let remounted = f.cx.update(|cx| handle.session_id(cx)).unwrap();
    assert_ne!(first, remounted);
    assert_ne!(closed, remounted);
}

#[test]
fn popup_factories_are_frame_local_while_hidden_and_rebuild_on_reopen() {
    let mut f = Fixture::with_cache(|_| {}, true);
    let live = |f: &Fixture| {
        f.consumer.read_with(&f.cx, |state, _| {
            state
                .captures
                .borrow()
                .iter()
                .filter(|capture| capture.strong_count() > 0)
                .count()
        })
    };
    assert_eq!(live(&f), 0);
    f.set_open(true);
    assert_eq!(live(&f), 1);
    f.set_open(false);
    assert_eq!(live(&f), 0);
    f.edit(|_| {});
    assert_eq!(live(&f), 0);
    f.set_open(true);
    assert_eq!(live(&f), 1);
    f.set_open(false);
    assert_eq!(live(&f), 0);
}

#[test]
fn popup_hidden_hook_is_exactly_once_per_presentation_and_runs_on_unmount() {
    let mut f = Fixture::new(|_| {});
    let counts = |f: &Fixture| {
        f.consumer.read_with(&f.cx, |state, _| {
            (state.presented_calls.get(), state.hidden_calls.get())
        })
    };
    assert_eq!(counts(&f), (0, 0));
    f.set_open(true);
    f.draw();
    assert_eq!(counts(&f), (1, 0));
    f.set_open(false);
    f.draw();
    assert_eq!(counts(&f), (1, 1));
    f.set_open(true);
    f.edit(|state| state.mounted = false);
    assert_eq!(counts(&f), (2, 2));
}

#[test]
fn popup_shadow_pixels_are_released_after_last_scene_closes() {
    let mut f = Fixture::new(|_| {});
    f.set_open(true);
    assert!(f.cx.update(|cx| popup_shadow_statistics(cx).bytes) > 0);
    f.set_open(false);
    assert_eq!(f.cx.update(|cx| popup_shadow_statistics(cx).bytes), 0);
}

#[test]
fn hidden_window_releases_cached_popup_scene_without_a_platform_frame_and_restores_it() {
    let mut f = Fixture::with_cache(|_| {}, true);
    f.set_open(true);
    f.cx.simulate_window_visibility_change(f.window.into(), gpui::WindowVisibility::Hidden);
    f.cx.run_until_parked();
    assert!(
        f.open(),
        "window suspension preserves authoritative visibility"
    );
    assert_eq!(f.cx.update(|cx| popup_shadow_statistics(cx).bytes), 0);
    assert_eq!(
        f.consumer
            .read_with(&f.cx, |state, _| state.hidden_calls.get()),
        1
    );
    assert!(f.consumer.read_with(&f.cx, |state, _| {
        state
            .captures
            .borrow()
            .iter()
            .all(|capture| capture.strong_count() == 0)
    }));
    f.cx.simulate_window_visibility_change(f.window.into(), gpui::WindowVisibility::Visible);
    f.draw();
    assert!(f.open());
    assert_eq!(
        f.consumer
            .read_with(&f.cx, |state, _| state.presented_calls.get()),
        2
    );
    assert!(f.cx.update(|cx| popup_shadow_statistics(cx).bytes) > 0);
}

#[test]
fn popup_native_visibility_suspension_cancels_pointer_press_until_a_new_click() {
    for initially_open in [false, true] {
        let mut f = Fixture::new(|_| {});
        f.set_open(initially_open);
        let handle = f.handle();
        let state = handle.state().unwrap().upgrade().unwrap();
        let before = f.changes();
        let position = point(px(120.), px(116.));
        f.dispatch(PlatformInput::MouseDown(MouseDownEvent {
            button: MouseButton::Left,
            position,
            ..Default::default()
        }));
        assert!(state.read_with(&f.cx, |s, _| s.pointer_down));
        f.cx.simulate_window_visibility_change(f.window.into(), gpui::WindowVisibility::Hidden);
        f.cx.run_until_parked();
        assert!(!state.read_with(&f.cx, |s, _| s.pointer_down));
        f.cx.simulate_window_visibility_change(f.window.into(), gpui::WindowVisibility::Visible);
        f.draw();
        f.dispatch(PlatformInput::MouseUp(MouseUpEvent {
            button: MouseButton::Left,
            position,
            ..Default::default()
        }));
        f.draw();
        assert_eq!(
            f.open(),
            initially_open,
            "an interrupted click does not toggle"
        );
        assert_eq!(f.changes(), before);
        f.click(120., 116.);
        assert_eq!(f.open(), !initially_open, "a fresh click still toggles");
    }
}

#[test]
fn popup_native_visibility_suspension_cancels_key_press_until_a_new_activation() {
    for key in ["enter", "space"] {
        for initially_open in [false, true] {
            let mut f = Fixture::new(|_| {});
            f.key("tab");
            f.set_open(initially_open);
            let handle = f.handle();
            let state = handle.state().unwrap().upgrade().unwrap();
            let before = f.changes();
            let keystroke = Keystroke::parse(key).unwrap();
            f.dispatch(PlatformInput::KeyDown(KeyDownEvent {
                keystroke: keystroke.clone(),
                is_held: false,
                prefer_character_input: false,
            }));
            assert!(state.read_with(&f.cx, |s, _| s.keyboard_down.is_some()));
            f.cx.simulate_window_visibility_change(f.window.into(), gpui::WindowVisibility::Hidden);
            f.cx.run_until_parked();
            assert!(state.read_with(&f.cx, |s, _| s.keyboard_down.is_none()));
            f.cx.simulate_window_visibility_change(
                f.window.into(),
                gpui::WindowVisibility::Visible,
            );
            f.draw();
            f.dispatch(PlatformInput::KeyUp(KeyUpEvent { keystroke }));
            f.draw();
            assert_eq!(
                f.open(),
                initially_open,
                "an interrupted key does not toggle"
            );
            assert_eq!(f.changes(), before);
            f.key(key);
            assert_eq!(f.open(), !initially_open, "a fresh key still toggles");
        }
    }
}

#[test]
fn popup_native_restore_recovers_owned_focus_and_preserves_the_return_target() {
    let mut f = Fixture::new(|state| state.initial_focus = true);
    f.key("tab");
    f.key("enter");
    assert!(f.focused(|state| state.inner_focus.clone()));
    let handle = f.handle();
    let first = f.cx.update(|cx| handle.session_id(cx));
    f.cx.simulate_window_visibility_change(f.window.into(), gpui::WindowVisibility::Hidden);
    f.cx.run_until_parked();
    assert!(f.open());
    f.cx.simulate_window_visibility_change(f.window.into(), gpui::WindowVisibility::Visible);
    f.draw();
    assert!(f.focused(|state| state.inner_focus.clone()));
    assert_ne!(
        first,
        f.cx.update(|cx| handle.session_id(cx)),
        "suspended async UI sessions are stale"
    );
    f.key("escape");
    assert!(!f.open());
    assert!(f.focused(|state| state.trigger_focus.clone()));
}

#[test]
fn popup_native_restore_does_not_take_focus_from_its_mouse_trigger() {
    let mut f = Fixture::new(|state| state.initial_focus = true);
    f.click(120., 116.);
    assert!(f.open());
    assert!(f.focused(|state| state.trigger_focus.clone()));
    f.cx.simulate_window_visibility_change(f.window.into(), gpui::WindowVisibility::Hidden);
    f.cx.run_until_parked();
    f.cx.simulate_window_visibility_change(f.window.into(), gpui::WindowVisibility::Visible);
    f.draw();
    assert!(f.open());
    assert!(f.focused(|state| state.trigger_focus.clone()));
    assert!(!f.focused(|state| state.inner_focus.clone()));
}

#[test]
fn popup_shadow_scene_sharing_keeps_other_windows_alive_until_their_last_use() {
    struct Consumer(PopupHandle);
    impl Render for Consumer {
        fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
            div().p(px(100.)).child(
                Popup::new("shared-shadow")
                    .default_open(true)
                    .handle(&self.0)
                    .child(div().w(px(80.)).h(px(32.)))
                    .content(|_, _, _| div().w(px(160.)).h(px(64.))),
            )
        }
    }
    let mut cx = TestAppContext::single();
    let handles = [PopupHandle::new(), PopupHandle::new()];
    let windows: Vec<_> = handles
        .iter()
        .map(|handle| {
            cx.add_window(|window, cx| {
                crate::init(cx);
                let scope = ConfigScope::new_window(cx);
                scope
                    .update_patch(cx, |patch| patch.motion = Some(false))
                    .unwrap();
                let view = cx.new(|_| Consumer(handle.clone()));
                UiRoot::new(scope, view, window, cx)
            })
        })
        .collect();
    let draw = |cx: &mut TestAppContext, index: usize| {
        cx.update_window(windows[index].into(), |_, window, cx| {
            window.draw(cx).clear(cx)
        })
        .unwrap();
        cx.run_until_parked();
    };
    draw(&mut cx, 0);
    draw(&mut cx, 1);
    assert_eq!(cx.update(|cx| popup_shadow_statistics(cx).live_entries), 1);
    for index in 0..2 {
        cx.update_window(windows[index].into(), |_, window, cx| {
            handles[index].close(window, cx)
        })
        .unwrap();
        draw(&mut cx, index);
        let bytes = cx.update(|cx| popup_shadow_statistics(cx).bytes);
        if index == 0 {
            assert!(bytes > 0, "the other scene still owns its shadow");
        } else {
            assert_eq!(bytes, 0);
        }
    }
}

#[test]
fn popup_inactive_unique_geometry_has_a_fixed_lru_capacity() {
    let f = Fixture::new(|_| {});
    let weak = f.handle().state().unwrap();
    f.cx.update(|cx| {
        let mut host = super::host::HostState::default();
        let mut keys = Vec::new();
        let now = cx.background_executor().now();
        for _ in 0..80 {
            let key = cx.new(|_| ());
            let used = host.geometry_tick();
            host.unique_geometry.insert(
                key.entity_id(),
                super::host::UniqueGeometry {
                    owner: weak.clone(),
                    bounds: gpui::Bounds::default(),
                    expires: Some(now + Duration::from_millis(300)),
                    used,
                },
            );
            keys.push(key);
        }
        host.prune_geometry(now);
        assert_eq!(host.unique_geometry.len(), 64);
        assert!(!host.unique_geometry.contains_key(&keys[0].entity_id()));
        assert!(host.unique_geometry.contains_key(&keys[79].entity_id()));
        host.prune_geometry(now + Duration::from_millis(301));
        assert!(host.unique_geometry.is_empty());
    });
}

#[test]
fn popup_entry_fades_the_native_surface_and_stops_scheduling_when_settled() {
    let mut f = Fixture::new(|_| {});
    let scope = f.consumer.read_with(&f.cx, |state, _| state.scope.clone());
    f.cx.update(|cx| {
        let mut patch = scope.patch(cx);
        patch.motion = Some(true);
        scope.set_patch(patch, cx).unwrap();
    });
    f.draw();
    let alpha = |f: &mut Fixture| {
        let handle = f.handle();
        f.cx.update_window(f.window.into(), |_, window, cx| {
            let layout = handle.layout(cx).unwrap();
            let scale = window.scale_factor();
            window
                .rendered_scene()
                .quads
                .iter()
                .filter(|quad| {
                    (quad.bounds.origin.x.0 / scale - f32::from(layout.bounds.origin.x)).abs()
                        < 0.01
                        && (quad.bounds.origin.y.0 / scale - f32::from(layout.bounds.origin.y))
                            .abs()
                            < 0.01
                        && (quad.bounds.size.width.0 / scale - f32::from(layout.bounds.size.width))
                            .abs()
                            < 0.01
                })
                .filter_map(|quad| quad.background.as_solid())
                .map(|color| color.a)
                .fold(0_f32, f32::max)
        })
        .unwrap()
    };
    f.set_open(true);
    assert!(alpha(&mut f) < 0.01);
    f.advance(50);
    let halfway = alpha(&mut f);
    assert!(
        halfway > 0.1 && halfway < 1.,
        "intermediate alpha: {halfway}"
    );
    f.advance(200);
    assert_eq!(alpha(&mut f), 1.);
    f.cx.update_window(f.window.into(), |_, window, cx| {
        window.simulate_next_frame(cx)
    })
    .unwrap();
    f.draw();
    let idle =
        f.cx.update_window(f.window.into(), |_, window, cx| {
            window.simulate_next_frame(cx)
        })
        .unwrap();
    assert_eq!(idle, 0, "settled surface must not request idle frames");
}

#[test]
fn popup_reduced_motion_finishes_an_in_flight_entry() {
    let mut f = Fixture::new(|_| {});
    let scope = f.consumer.read_with(&f.cx, |state, _| state.scope.clone());
    f.cx.update(|cx| {
        let mut patch = scope.patch(cx);
        patch.motion = Some(true);
        scope.set_patch(patch, cx).unwrap();
    });
    f.set_open(true);
    f.advance(25);
    f.cx.update(|cx| cx.set_reduce_motion(true));
    f.draw();
    f.cx.update_window(f.window.into(), |_, window, cx| {
        window.simulate_next_frame(cx)
    })
    .unwrap();
    f.draw();
    assert_eq!(
        f.cx.update_window(f.window.into(), |_, window, cx| window
            .simulate_next_frame(cx))
            .unwrap(),
        0
    );
    assert!(f.open());
}

#[test]
fn popup_hover_delays_cancel_and_leave_closes() {
    let mut f = Fixture::new(|state| state.triggers = PopupTriggers::HOVER);
    f.hover(120., 116.);
    f.advance(99);
    assert!(!f.open());
    f.hover(400., 300.);
    f.advance(100);
    assert!(!f.open());
    assert!(f.changes().is_empty());
    f.hover(120., 116.);
    f.advance(100);
    assert!(f.open());
    f.hover(400., 300.);
    f.advance(99);
    assert!(f.open());
    f.advance(1);
    assert!(!f.open());
}

#[test]
fn popup_keyboard_focus_opens_without_adding_a_tab_stop() {
    let mut f = Fixture::new(|state| state.triggers = PopupTriggers::HOVER_FOCUS);
    f.key("tab");
    assert!(f.focused(|state| state.trigger_focus.clone()));
    let handle = f.handle();
    let diagnostic =
        f.cx.update_window(f.window.into(), |_, window, cx| {
            let state = handle.state().unwrap().upgrade().unwrap();
            let state = state.read(cx);
            format!(
                "focus subtree: {}, keyboard: {}, lifecycle: {:?}",
                state.trigger_focus.contains_focused(window, cx),
                window.last_input_was_keyboard(),
                state.lifecycle
            )
        })
        .unwrap();
    assert!(f.open(), "{diagnostic}");
    f.key("escape");
    assert!(!f.open());
    f.draw();
    assert!(!f.open());
    f.key("tab");
    assert!(f.focused(|state| state.outside_focus.clone()));
}

#[test]
fn popup_controlled_requests_wait_for_owner_and_handle_is_weak() {
    let mut f = Fixture::new(|state| state.controlled = Some(false));
    f.click(120., 116.);
    assert!(!f.open());
    assert_eq!(f.changes().len(), 1);
    f.edit(|state| state.controlled = Some(true));
    assert!(f.open());
    f.set_open(false);
    assert!(f.open());
    let handle = f.handle();
    f.edit(|state| state.mounted = false);
    assert!(!f.cx.update(|cx| handle.is_open(cx)));
    assert_eq!(f.cx.update(|cx| handle.layout(cx)), None);
}

#[test]
fn popup_escapes_ancestor_clip_and_closes_without_idle_frames() {
    let mut f = Fixture::new(|state| state.clipped = true);
    f.set_open(true);
    let handle = f.handle();
    let layout = f.cx.update(|cx| handle.layout(cx)).expect("popup geometry");
    assert!(layout.bounds.size.width > px(80.));
    let visible =
        f.cx.update_window(f.window.into(), |_, window, _| {
            window.rendered_scene().quads.iter().any(|quad| {
                quad.bounds.size.width.0 > 100.
                    && quad.bounds.size.height.0 < 100.
                    && quad.content_mask.bounds.size.width.0 > 100.
            })
        })
        .unwrap();
    assert!(
        visible,
        "surface should paint outside the 80px trigger clip"
    );
    f.set_open(false);
    let scheduled =
        f.cx.update_window(f.window.into(), |_, window, cx| {
            window.simulate_next_frame(cx)
        })
        .unwrap();
    assert_eq!(scheduled, 0);
}

#[test]
fn popup_keyboard_open_focuses_content_and_escape_restores_trigger() {
    let mut f = Fixture::new(|state| state.initial_focus = true);
    f.key("tab");
    f.key("enter");
    assert!(f.open());
    assert!(f.focused(|state| state.inner_focus.clone()));
    f.key("space");
    assert!(
        f.open(),
        "content activation must not toggle its ancestor trigger"
    );
    f.key("escape");
    assert!(!f.open());
    assert!(f.focused(|state| state.trigger_focus.clone()));
}

#[test]
fn popup_nested_escape_closes_only_topmost_and_parent_close_cascades() {
    let mut f = Fixture::new(|state| state.nested = true);
    f.set_open(true);
    let nested = f
        .consumer
        .read_with(&f.cx, |state, _| state.nested_handle.clone());
    f.cx.update_window(f.window.into(), |_, window, cx| {
        nested.set_open(true, window, cx)
    })
    .unwrap();
    f.draw();
    assert!(f.cx.update(|cx| nested.is_open(cx)));
    f.key("escape");
    assert!(!f.cx.update(|cx| nested.is_open(cx)));
    assert!(f.open());
    f.cx.update_window(f.window.into(), |_, window, cx| {
        nested.set_open(true, window, cx)
    })
    .unwrap();
    f.draw();
    f.set_open(false);
    assert!(!f.cx.update(|cx| nested.is_open(cx)));
}

struct CachedConsumer(Entity<Consumer>);
impl Render for CachedConsumer {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        self.0
            .clone()
            .cached(StyleRefinement::default().size_full())
    }
}

#[test]
fn popup_state_invalidates_a_cached_parent_and_survives_scene_reuse() {
    let mut f = Fixture::with_cache(|_| {}, true);
    f.set_open(true);
    for _ in 0..3 {
        f.draw();
    }
    assert!(f.open());
    f.set_open(false);
    assert!(!f.open());
}

struct ScrollContent {
    scope: ConfigScope,
    handle: PopupHandle,
    renders: Rc<Cell<usize>>,
}
impl Render for ScrollContent {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        self.renders.set(self.renders.get() + 1);
        div()
            .w(px(320.))
            .h(px(700.))
            .pt(px(100.))
            .pl(px(100.))
            .child(
                Popup::new("scroll-popup")
                    .scope(self.scope.clone())
                    .handle(&self.handle)
                    .placement(PopupPlacement::Bottom)
                    .default_open(true)
                    .child(Button::new("scroll-trigger").label("Open").w(px(80.))),
            )
    }
}
struct ScrollRoot {
    content: Entity<ScrollContent>,
    scroll: gpui::ScrollHandle,
    extent: f32,
}
impl Render for ScrollRoot {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        div()
            .id("scroller")
            .w(px(320.))
            .h(px(self.extent))
            .overflow_y_scroll()
            .track_scroll(&self.scroll)
            .child(
                self.content
                    .clone()
                    .cached(StyleRefinement::default().w(px(320.)).h(px(700.))),
            )
    }
}

#[gpui::test]
fn popup_in_a_cached_scroller_realigns_and_hides_when_its_anchor_is_clipped(
    cx: &mut TestAppContext,
) {
    let handle = PopupHandle::new();
    let scroll = gpui::ScrollHandle::new();
    let renders = Rc::new(Cell::new(0));
    let mut parent = None;
    let window = cx.add_window(|window, cx| {
        let scope = ConfigScope::new_window(cx);
        scope
            .update_patch(cx, |patch| patch.motion = Some(false))
            .unwrap();
        let content = cx.new(|_| ScrollContent {
            scope: scope.clone(),
            handle: handle.clone(),
            renders: renders.clone(),
        });
        let root = cx.new(|_| ScrollRoot {
            content,
            scroll: scroll.clone(),
            extent: 200.,
        });
        parent = Some(root.clone());
        UiRoot::new(scope, root, window, cx)
    });
    let parent = parent.unwrap();
    let draw = |cx: &mut TestAppContext| {
        cx.update_window(window.into(), |_, window, cx| window.draw(cx).clear(cx))
            .unwrap();
    };
    draw(cx);
    let first = cx.update(|cx| handle.layout(cx).unwrap());
    let count = renders.get();
    draw(cx);
    assert_eq!(renders.get(), count, "baseline uses the retained scene");
    scroll.set_offset(point(px(0.), px(-40.)));
    parent.update(cx, |_, cx| cx.notify());
    draw(cx);
    let moved = cx.update(|cx| handle.layout(cx).unwrap());
    assert_eq!(moved.bounds.top(), first.bounds.top() - px(40.));
    scroll.set_offset(point(px(0.), px(-160.)));
    parent.update(cx, |_, cx| cx.notify());
    draw(cx);
    assert!(
        cx.update(|cx| handle.layout(cx).is_none()),
        "fully clipped anchor removes the deferred surface"
    );
    scroll.set_offset(point(px(0.), px(0.)));
    parent.update(cx, |_, cx| cx.notify());
    draw(cx);
    assert_eq!(cx.update(|cx| handle.layout(cx).unwrap()), first);
    // A mask-only change must invalidate the cached view as well as scrolling.
    parent.update(cx, |root, cx| {
        root.extent = 60.;
        cx.notify();
    });
    draw(cx);
    assert!(cx.update(|cx| handle.layout(cx).is_none()));
}
