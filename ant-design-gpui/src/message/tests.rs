// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;
use crate::{Button, ConfigScope, UiRoot};
use gpui::{
    AppContext, Context as ViewContext, Entity, FocusHandle, IntoElement, MouseButton,
    MouseDownEvent, MouseMoveEvent, MouseUpEvent, PlatformInput, Render, TestAppContext,
    WindowHandle, div, point, prelude::*, px, size,
};
use std::{cell::Cell, task::Context as FutureContext};

struct Page {
    scope: ConfigScope,
    focus: FocusHandle,
    clicks: Rc<Cell<usize>>,
}
impl Render for Page {
    fn render(&mut self, _: &mut Window, _: &mut ViewContext<Self>) -> impl IntoElement {
        let clicks = self.clicks.clone();
        div().size_full().child(
            Button::new("under-message")
                .scope(self.scope.clone())
                .label("Underlying action")
                .focus_handle(&self.focus)
                .wave(false)
                .on_click(move |_, _, _| clicks.set(clicks.get() + 1)),
        )
    }
}

struct Fixture {
    cx: TestAppContext,
    window: WindowHandle<UiRoot>,
    messages: Message,
    scope: ConfigScope,
    page: Entity<Page>,
}
impl Fixture {
    fn new(motion: bool) -> Self {
        let mut cx = TestAppContext::single();
        let mut created = None;
        let window = cx.open_window(size(px(420.), px(360.)), |window, cx| {
            let scope = ConfigScope::new_window(cx);
            scope
                .update_patch(cx, |patch| patch.motion = Some(motion))
                .unwrap();
            let page = cx.new(|cx| Page {
                scope: scope.clone(),
                focus: cx.focus_handle(),
                clicks: Rc::new(Cell::new(0)),
            });
            let root = UiRoot::new(scope.clone(), page.clone(), window, cx);
            created = Some((root.messages(), scope, page));
            root
        });
        let (messages, scope, page) = created.unwrap();
        let mut fixture = Self {
            cx,
            window,
            messages,
            scope,
            page,
        };
        fixture.draw();
        fixture
    }
    fn in_window<R>(&mut self, f: impl FnOnce(&Message, &mut Window, &mut App) -> R) -> R {
        let messages = self.messages.clone();
        self.cx
            .update_window(self.window.into(), |_, window, cx| f(&messages, window, cx))
            .unwrap()
    }
    fn open(&mut self, options: MessageOptions) -> MessageHandle {
        let result = self.in_window(|messages, window, cx| messages.open(options, window, cx));
        self.draw();
        result
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
    fn advance(&mut self, millis: u64) {
        self.cx
            .executor()
            .advance_clock(Duration::from_millis(millis));
        self.cx.run_until_parked();
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                window.simulate_next_frame(cx);
            })
            .unwrap();
        self.draw();
    }
    fn count(&mut self) -> usize {
        self.in_window(|messages, _, cx| messages.len(cx))
    }
    fn hover(&mut self, handle: &MessageHandle, hovered: bool) {
        let id = handle.id;
        self.in_window(|messages, window, cx| {
            messages
                .state
                .upgrade()
                .unwrap()
                .update(cx, |state, cx| state.hover(id, hovered, window, cx));
        });
    }
    fn config(&mut self, patch: MessageConfigPatch) {
        self.in_window(|messages, window, cx| messages.set_config(patch, window, cx));
        self.draw();
    }
    fn layouts(&mut self) -> Vec<MessageLayout> {
        self.in_window(|messages, _, cx| messages.layouts(cx))
    }
    fn click(&mut self, position: gpui::Point<Pixels>) {
        self.in_window(|_, window, cx| {
            window.dispatch_event(
                PlatformInput::MouseMove(MouseMoveEvent {
                    position,
                    ..Default::default()
                }),
                cx,
            );
            window.dispatch_event(
                PlatformInput::MouseDown(MouseDownEvent {
                    button: MouseButton::Left,
                    position,
                    ..Default::default()
                }),
                cx,
            );
            window.dispatch_event(
                PlatformInput::MouseUp(MouseUpEvent {
                    button: MouseButton::Left,
                    position,
                    ..Default::default()
                }),
                cx,
            );
        });
        self.draw();
    }
}

fn completed(handle: &MessageHandle) -> Option<MessageCloseEvent> {
    let mut future = Box::pin(handle.completion());
    match future
        .as_mut()
        .poll(&mut FutureContext::from_waker(Waker::noop()))
    {
        Poll::Ready(event) => Some(event),
        Poll::Pending => None,
    }
}

#[test]
fn default_timeout_and_persistent_loading_have_distinct_lifetimes() {
    let mut f = Fixture::new(false);
    let automatic = f.open(MessageOptions::new("Default duration").kind(MessageType::Loading));
    let persistent = f.open(MessageOptions::new("Persistent").duration(Duration::ZERO));
    f.advance(2999);
    assert_eq!(f.count(), 2);
    f.advance(1);
    assert_eq!(
        completed(&automatic).unwrap().reason,
        MessageCloseReason::Timeout
    );
    assert!(!persistent.is_dismissed());
    assert_eq!(f.count(), 1);
}

#[test]
fn hover_preserves_remaining_duration_and_false_keeps_running() {
    let mut f = Fixture::new(false);
    let paused = f.open(MessageOptions::new("Pause").duration(Duration::from_secs(1)));
    let running = f.open(
        MessageOptions::new("Run")
            .duration(Duration::from_secs(1))
            .pause_on_hover(false),
    );
    f.advance(400);
    f.hover(&paused, true);
    f.hover(&running, true);
    f.advance(2000);
    assert!(!paused.is_dismissed());
    assert!(running.is_dismissed());
    f.hover(&paused, false);
    f.advance(599);
    assert!(!paused.is_dismissed());
    f.advance(1);
    assert!(paused.is_dismissed());
}

#[test]
fn keyed_update_keeps_position_and_same_duration_deadline() {
    let mut f = Fixture::new(false);
    let first = f.open(
        MessageOptions::new("First")
            .key("same")
            .duration(Duration::from_secs(1)),
    );
    f.open(MessageOptions::new("Newer").duration(Duration::ZERO));
    f.advance(700);
    let updated = f.open(
        MessageOptions::new("Updated")
            .key("same")
            .duration(Duration::from_secs(1)),
    );
    assert_eq!(first.id, updated.id);
    assert_eq!(f.count(), 2);
    assert_eq!(f.layouts().last().unwrap().key.as_ref(), "same");
    f.advance(300);
    assert!(first.is_dismissed());
    assert!(updated.is_dismissed());
}

#[test]
fn changed_duration_invalidates_old_timer_including_a_paused_update() {
    let mut f = Fixture::new(false);
    let first = f.open(
        MessageOptions::new("First")
            .key("update")
            .duration(Duration::from_secs(1)),
    );
    f.advance(700);
    f.hover(&first, true);
    let replacement = f.open(
        MessageOptions::new("Replacement")
            .key("update")
            .duration(Duration::from_secs(2)),
    );
    f.advance(3000);
    assert!(!replacement.is_dismissed());
    f.hover(&replacement, false);
    f.advance(1999);
    assert!(!replacement.is_dismissed());
    f.advance(1);
    assert!(first.is_dismissed());
    assert_eq!(completed(&first), completed(&replacement));
}

#[test]
fn latest_callback_runs_once_and_older_handle_closes_its_keyed_replacement() {
    let mut f = Fixture::new(false);
    let old_count = Rc::new(Cell::new(0));
    let new_count = Rc::new(Cell::new(0));
    let old = old_count.clone();
    let first = f.open(
        MessageOptions::new("Old")
            .key("key")
            .on_close(move |_, _, _| old.set(old.get() + 1)),
    );
    let new = new_count.clone();
    let updated = f.open(
        MessageOptions::new("New")
            .key("key")
            .on_close(move |_, _, _| new.set(new.get() + 1)),
    );
    f.in_window(|_, window, cx| {
        first.close(window, cx);
        first.close(window, cx);
    });
    assert_eq!(old_count.get(), 0);
    assert_eq!(new_count.get(), 1);
    assert!(updated.is_dismissed());
    assert_eq!(
        completed(&first).unwrap().reason,
        MessageCloseReason::Programmatic
    );
}

#[test]
fn old_handle_cannot_dismiss_a_new_lifetime_using_the_same_key() {
    let mut f = Fixture::new(false);
    let old = f.open(MessageOptions::new("Old").key("reused"));
    f.in_window(|_, window, cx| old.close(window, cx));
    let new = f.open(MessageOptions::new("New").key("reused"));
    f.in_window(|_, window, cx| old.close(window, cx));
    assert!(!new.is_dismissed());
    assert_ne!(old.id, new.id);
}

#[test]
fn destroy_key_and_all_settle_completions_without_duplicate_callbacks() {
    let mut f = Fixture::new(false);
    let calls = Rc::new(Cell::new(0));
    let make = |key: &'static str| {
        let calls = calls.clone();
        MessageOptions::new(key)
            .key(key)
            .on_close(move |_, _, _| calls.set(calls.get() + 1))
    };
    let a = f.open(make("a"));
    let b = f.open(make("b"));
    f.in_window(|messages, window, cx| messages.destroy("a", window, cx));
    assert!(a.is_dismissed());
    assert!(!b.is_dismissed());
    f.in_window(|messages, window, cx| {
        messages.destroy_all(window, cx);
        messages.destroy_all(window, cx);
    });
    assert_eq!(completed(&b).unwrap().reason, MessageCloseReason::Destroyed);
    assert_eq!(calls.get(), 2);
}

#[test]
fn max_count_evicts_oldest_and_live_limit_changes_take_effect() {
    let mut f = Fixture::new(false);
    let oldest = f.open(MessageOptions::new("Oldest").duration(Duration::ZERO));
    let middle = f.open(MessageOptions::new("Middle").duration(Duration::ZERO));
    f.config(MessageConfigPatch {
        max_count: Some(Some(2)),
        ..Default::default()
    });
    f.open(MessageOptions::new("Newest").duration(Duration::ZERO));
    assert_eq!(
        completed(&oldest).unwrap().reason,
        MessageCloseReason::Evicted
    );
    assert!(!middle.is_dismissed());
    f.config(MessageConfigPatch {
        max_count: Some(Some(1)),
        ..Default::default()
    });
    assert_eq!(
        completed(&middle).unwrap().reason,
        MessageCloseReason::Evicted
    );
    assert_eq!(f.count(), 1);
}

#[test]
fn close_callback_can_reenter_the_service_and_open_a_new_message() {
    let mut f = Fixture::new(false);
    let messages = f.messages.clone();
    let first = f.open(MessageOptions::new("First").on_close(move |_, window, cx| {
        messages.info("Opened by callback", window, cx);
    }));
    f.in_window(|_, window, cx| first.close(window, cx));
    f.draw();
    assert_eq!(f.count(), 1);
}

#[test]
fn completion_futures_can_be_dropped_without_retaining_their_wakers() {
    let mut f = Fixture::new(false);
    let handle = f.open(MessageOptions::new("Persistent").duration(Duration::ZERO));
    for _ in 0..100 {
        let mut future = Box::pin(handle.completion());
        assert!(
            future
                .as_mut()
                .poll(&mut FutureContext::from_waker(Waker::noop()))
                .is_pending()
        );
    }
    assert_eq!(handle.completion.borrow().waiters.len(), 0);
    assert!(!handle.is_dismissed());
}

#[test]
fn style_and_theme_changes_preserve_lifetime_and_deadline() {
    let mut f = Fixture::new(false);
    let handle = f.open(MessageOptions::new("Keep timer").duration(Duration::from_secs(1)));
    f.advance(500);
    f.cx.update(|cx| {
        f.scope
            .update_patch(cx, |patch| {
                patch.direction = Some(crate::Direction::Rtl);
                patch.theme = Some(crate::theme::ThemeConfig {
                    algorithms: Some(crate::theme::AlgorithmChain::dark()),
                    ..Default::default()
                });
            })
            .unwrap()
    });
    f.draw();
    f.advance(500);
    assert!(handle.is_dismissed());
}

#[test]
fn closing_keeps_outgoing_visual_until_exit_completes() {
    let mut f = Fixture::new(true);
    let handle = f.open(MessageOptions::new("Animated"));
    f.advance(220);
    f.in_window(|_, window, cx| handle.close(window, cx));
    f.draw();
    assert_eq!(f.count(), 0);
    assert!(f.layouts().iter().any(|layout| layout.closing));
    f.advance(199);
    assert!(!f.layouts().is_empty());
    f.advance(1);
    assert!(f.layouts().is_empty());
}

#[test]
fn window_service_is_registered_before_first_draw_and_preserves_focus() {
    let mut f = Fixture::new(false);
    let focus = f.page.read_with(&f.cx, |page, _| page.focus.clone());
    f.in_window(|_, window, cx| window.focus(&focus, cx));
    f.in_window(|_, window, cx| {
        let service = Message::for_window(window, cx).expect("root has a service");
        service.info("Nonmodal", window, cx);
        assert!(focus.is_focused(window));
    });
    f.draw();
    f.in_window(|_, window, _| assert!(focus.is_focused(window)));
}

#[test]
fn click_on_message_does_not_dismiss_and_empty_viewport_passes_pointer_input() {
    let mut f = Fixture::new(false);
    let clicks = Rc::new(Cell::new(0));
    let observed = clicks.clone();
    let handle = f.open(
        MessageOptions::new("Click notice")
            .duration(Duration::ZERO)
            .on_click(move |_, _, _| observed.set(observed.get() + 1)),
    );
    let bounds = f.layouts().first().unwrap().bounds;
    f.click(bounds.center());
    assert_eq!(clicks.get(), 1);
    assert!(!handle.is_dismissed());
    f.click(point(px(25.), px(16.)));
    assert_eq!(f.page.read_with(&f.cx, |page, _| page.clicks.get()), 1);
}

#[test]
fn list_is_centered_and_newest_first_with_reference_spacing() {
    let mut f = Fixture::new(false);
    f.open(
        MessageOptions::new("Older longer message")
            .key("old")
            .duration(Duration::ZERO),
    );
    f.open(
        MessageOptions::new("New")
            .key("new")
            .duration(Duration::ZERO),
    );
    let layouts = f.layouts();
    assert_eq!(layouts.len(), 2);
    assert_eq!(layouts[0].key.as_ref(), "new");
    assert!((f32::from(layouts[0].bounds.top()) - 8.).abs() < 0.1);
    for layout in &layouts {
        assert!((f32::from(layout.bounds.center().x) - 210.).abs() < 0.6);
        assert!((f32::from(layout.bounds.size.height) - 40.).abs() < 0.6);
    }
    assert!((f32::from(layouts[1].bounds.top() - layouts[0].bounds.bottom()) - 16.).abs() < 0.6);
}

#[test]
fn scoped_configuration_merges_styles_and_explicit_false_overrides_parent() {
    let mut f = Fixture::new(false);
    f.cx.update(|cx| {
        f.scope
            .update_patch(cx, |patch| {
                patch.message.top = Some(34.);
                patch.message.pause_on_hover = Some(true);
                patch.message.max_count = Some(Some(2));
            })
            .unwrap();
    });
    f.config(MessageConfigPatch {
        pause_on_hover: Some(false),
        max_count: Some(None),
        ..Default::default()
    });
    f.in_window(|messages, _, cx| {
        let state = messages.state.upgrade().unwrap();
        let config = state.read(cx).effective_config(cx);
        assert_eq!(config.top, 34.);
        assert!(!config.pause_on_hover);
        assert_eq!(config.max_count, None);
    });
}

#[test]
fn plain_empty_and_custom_content_do_not_require_a_status_icon() {
    let mut f = Fixture::new(false);
    f.open(MessageOptions::new("").duration(Duration::ZERO));
    f.open(
        MessageOptions::content(|_, _, _| div().w(px(80.)).h(px(32.)).child("Rich content"))
            .duration(Duration::ZERO),
    );
    assert_eq!(f.layouts().len(), 2);
    assert!(
        f.layouts()
            .iter()
            .all(|layout| layout.bounds.size.width >= px(24.))
    );
}

#[test]
fn independent_windows_reject_the_other_windows_service() {
    let mut f = Fixture::new(false);
    let mut other_service = None;
    let other = f.cx.add_window(|window, cx| {
        let scope = ConfigScope::new_window(cx);
        let page = cx.new(|cx| Page {
            scope: scope.clone(),
            focus: cx.focus_handle(),
            clicks: Rc::new(Cell::new(0)),
        });
        let root = UiRoot::new(scope, page, window, cx);
        other_service = Some(root.messages());
        root
    });
    let first = f.messages.clone();
    let other_service = other_service.unwrap();
    let rejected =
        f.cx.update_window(other.into(), |_, window, cx| {
            assert!(Message::for_window(window, cx).is_some());
            other_service.info("Second window", window, cx);
            first.info("Wrong window", window, cx)
        })
        .unwrap();
    assert_eq!(
        completed(&rejected).unwrap().reason,
        MessageCloseReason::Unavailable
    );
    assert_eq!(f.count(), 0);
    assert_eq!(f.cx.read(|cx| other_service.len(cx)), 1);
}

#[test]
fn native_window_close_settles_waiters_and_releases_the_controller() {
    let mut f = Fixture::new(false);
    let handle = f.open(MessageOptions::new("Persistent").duration(Duration::ZERO));
    let weak = f.messages.state.clone();
    f.cx.update_window(f.window.into(), |_, window, _| window.remove_window())
        .unwrap();
    f.cx.run_until_parked();
    assert_eq!(
        completed(&handle).unwrap().reason,
        MessageCloseReason::OwnerDropped
    );
    assert!(weak.upgrade().is_none());
}

#[test]
fn disabling_motion_finishes_outgoing_visuals_without_waiting_for_frames() {
    let mut f = Fixture::new(true);
    let handle = f.open(MessageOptions::new("Leaving"));
    f.advance(220);
    f.in_window(|_, window, cx| handle.close(window, cx));
    f.cx.update(|cx| {
        f.scope
            .update_patch(cx, |patch| patch.motion = Some(false))
            .unwrap()
    });
    f.draw();
    assert!(f.layouts().is_empty());
}

#[test]
fn stack_hover_pauses_eligible_hidden_notices_but_not_explicit_opt_out() {
    let mut f = Fixture::new(false);
    f.config(MessageConfigPatch {
        stack: Some(MessageStack::Enabled { threshold: 1 }),
        ..Default::default()
    });
    let hidden = f.open(MessageOptions::new("Old").duration(Duration::from_secs(1)));
    let active = f.open(
        MessageOptions::new("New")
            .duration(Duration::from_secs(1))
            .pause_on_hover(false),
    );
    f.advance(400);
    let position = f.layouts()[0].bounds.center();
    f.in_window(|_, window, cx| {
        window.dispatch_event(
            PlatformInput::MouseMove(MouseMoveEvent {
                position,
                ..Default::default()
            }),
            cx,
        );
    });
    f.draw();
    f.advance(1600);
    assert!(!hidden.is_dismissed());
    assert!(active.is_dismissed());
    f.in_window(|_, window, cx| {
        window.dispatch_event(
            PlatformInput::MouseMove(MouseMoveEvent {
                position: point(px(4.), px(300.)),
                ..Default::default()
            }),
            cx,
        );
    });
    f.draw();
    f.advance(600);
    assert!(hidden.is_dismissed());
}

#[test]
fn changing_default_duration_affects_new_opens_without_restarting_existing_notices() {
    let mut f = Fixture::new(false);
    let old = f.open(MessageOptions::new("Old default"));
    f.advance(2000);
    f.config(MessageConfigPatch {
        duration: Some(Duration::from_secs(5)),
        ..Default::default()
    });
    let new = f.open(MessageOptions::new("New default"));
    f.advance(1000);
    assert!(old.is_dismissed());
    assert!(!new.is_dismissed());
    f.advance(4000);
    assert!(new.is_dismissed());
}

#[test]
fn huge_duration_is_safe_and_can_be_cancelled() {
    let mut f = Fixture::new(false);
    let handle = f.open(MessageOptions::new("Long running").duration(Duration::MAX));
    f.advance(10_000);
    assert!(!handle.is_dismissed());
    f.in_window(|_, window, cx| handle.close(window, cx));
    assert!(handle.is_dismissed());
}

#[test]
fn component_local_motion_override_settles_exit_immediately() {
    let mut f = Fixture::new(true);
    f.cx.update(|cx| {
        f.scope
            .update_patch(cx, |patch| {
                let theme = patch.theme.get_or_insert_with(Default::default);
                theme.components.set_theme(
                    crate::theme::ComponentThemeConfig::<MessageTheme>::default().with_tokens(
                        crate::theme::TokenPatch {
                            motion: Some(false),
                            ..Default::default()
                        },
                    ),
                );
            })
            .unwrap();
    });
    let handle = f.open(MessageOptions::new("Component motion override"));
    f.in_window(|_, window, cx| handle.close(window, cx));
    f.draw();
    assert!(handle.is_dismissed());
    assert!(f.layouts().is_empty());
}

#[test]
fn outgoing_notice_rejects_clicks_before_the_next_draw() {
    let mut f = Fixture::new(true);
    let clicks = Rc::new(Cell::new(0));
    let observed = clicks.clone();
    let handle = f.open(
        MessageOptions::new("Click feedback")
            .duration(Duration::ZERO)
            .on_click(move |_, _, _| observed.set(observed.get() + 1)),
    );
    f.advance(220);
    let position = f.layouts()[0].bounds.center();
    f.click(position);
    assert_eq!(
        clicks.replace(0),
        1,
        "the live notice handles pointer activation"
    );
    f.in_window(|_, window, cx| handle.close(window, cx));
    // Dispatch against the preceding frame's handlers before replacing that frame.
    f.click(position);
    assert_eq!(clicks.get(), 0);
}

#[test]
fn eviction_reports_logical_dismissal_before_deferred_close_callbacks() {
    let mut f = Fixture::new(false);
    let oldest = f.open(MessageOptions::new("Oldest").duration(Duration::ZERO));
    f.open(MessageOptions::new("Newest").duration(Duration::ZERO));
    f.in_window(|messages, window, cx| {
        messages.set_config(
            MessageConfigPatch {
                max_count: Some(Some(1)),
                ..Default::default()
            },
            window,
            cx,
        );
        assert!(oldest.is_dismissed());
        assert_eq!(messages.len(cx), 1);
    });
    assert_eq!(
        completed(&oldest).unwrap().reason,
        MessageCloseReason::Evicted
    );
}
