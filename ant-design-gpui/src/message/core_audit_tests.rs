// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Focused lifecycle audit regressions beyond the ordinary public examples.

use std::{
    cell::{Cell, RefCell},
    future::Future,
    rc::Rc,
    sync::{
        Arc,
        atomic::{AtomicUsize, Ordering},
    },
    task::{Context as FutureContext, Poll, Wake, Waker},
    time::Duration,
};

use gpui::{
    App, AppContext, Context, IntoElement, Render, Styled, TestAppContext, Window, WindowHandle,
    div, px, size,
};

use super::{
    Completion, Message, MessageCloseEvent, MessageCloseReason, MessageConfigPatch, MessageHandle,
    MessageOptions,
};
use crate::{ConfigScope, UiRoot};

struct Page;
impl Render for Page {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        div().size_full()
    }
}

struct Fixture {
    cx: TestAppContext,
    window: WindowHandle<UiRoot>,
    messages: Message,
}
impl Fixture {
    fn new() -> Self {
        let mut cx = TestAppContext::single();
        let mut messages = None;
        let window = cx.open_window(size(px(420.), px(360.)), |window, cx| {
            let scope = ConfigScope::new_window(cx);
            let page = cx.new(|_| Page);
            let root = UiRoot::new(scope, page, window, cx);
            messages = Some(root.messages());
            root
        });
        Self {
            cx,
            window,
            messages: messages.unwrap(),
        }
    }
    fn update<R>(&mut self, callback: impl FnOnce(&Message, &mut Window, &mut App) -> R) -> R {
        let messages = self.messages.clone();
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                callback(&messages, window, cx)
            })
            .unwrap()
    }
    fn open(&mut self, options: MessageOptions) -> MessageHandle {
        self.update(|messages, window, cx| {
            messages.open(options.duration(Duration::ZERO), window, cx)
        })
    }
    fn present(&mut self) {
        self.cx.run_until_parked();
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                window.draw(cx).clear(cx);
            })
            .unwrap();
        self.cx.executor().advance_clock(Duration::from_millis(250));
        self.cx.run_until_parked();
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                window.simulate_next_frame(cx);
                window.draw(cx).clear(cx);
            })
            .unwrap();
        self.cx.run_until_parked();
    }
}

fn result(handle: &MessageHandle) -> Option<MessageCloseEvent> {
    let mut completion = Box::pin(handle.completion());
    match completion
        .as_mut()
        .poll(&mut FutureContext::from_waker(Waker::noop()))
    {
        Poll::Ready(event) => Some(event),
        Poll::Pending => None,
    }
}

#[test]
fn returned_generated_key_updates_the_same_logical_lifetime() {
    let mut fixture = Fixture::new();
    let original = fixture.open(MessageOptions::new("Generated key"));
    let replacement =
        fixture.open(MessageOptions::new("Updated by returned key").key(original.key().clone()));
    assert_eq!(original.id, replacement.id);
    fixture.update(|messages, window, cx| {
        assert_eq!(messages.len(cx), 1);
        original.close(window, cx);
    });
    assert_eq!(result(&original), result(&replacement));
    assert_eq!(
        result(&original).unwrap().reason,
        MessageCloseReason::Programmatic
    );
}

#[test]
fn generated_keys_skip_explicit_keys_and_retained_outgoing_lifetimes() {
    for close_explicit in [false, true] {
        let mut fixture = Fixture::new();
        let explicit =
            fixture.open(MessageOptions::new("Explicit reserved-looking key").key("message-2"));
        if close_explicit {
            fixture.present();
            fixture.update(|_, window, cx| explicit.close(window, cx));
        }
        let generated = fixture.open(MessageOptions::new("Generated next"));
        assert_ne!(generated.key(), explicit.key());
        assert!(generated.id > explicit.id);
        let generated_updated =
            fixture.open(MessageOptions::new("Update generated").key(generated.key().clone()));
        assert_eq!(generated.id, generated_updated.id);
        let another = fixture.open(MessageOptions::new("Another generated"));
        assert_ne!(another.key(), generated.key());
    }
}

#[test]
fn config_eviction_keeps_its_reason_when_owner_unmounts_before_deferred_callbacks() {
    let mut fixture = Fixture::new();
    let events = Rc::new(RefCell::new(Vec::new()));
    let observed = events.clone();
    let oldest = fixture.open(
        MessageOptions::new("Evicted")
            .on_close(move |event, _, _| observed.borrow_mut().push(event.clone())),
    );
    fixture.open(MessageOptions::new("Retained"));
    fixture.present();
    fixture.update(|messages, window, cx| {
        messages.set_config(
            MessageConfigPatch {
                max_count: Some(Some(1)),
                ..Default::default()
            },
            window,
            cx,
        );
        // Models host release in the same update as a scope/configuration change.
        super::state::unmount(&messages.state, cx);
    });
    fixture.cx.run_until_parked();
    let completion = result(&oldest).unwrap();
    let callback = events.borrow();
    assert_eq!(callback.len(), 1);
    assert_eq!(completion.reason, MessageCloseReason::Evicted);
    assert_eq!(callback[0], completion);
}

struct CountWake(AtomicUsize);
impl Wake for CountWake {
    fn wake(self: Arc<Self>) {
        self.0.fetch_add(1, Ordering::SeqCst);
    }
    fn wake_by_ref(self: &Arc<Self>) {
        self.0.fetch_add(1, Ordering::SeqCst);
    }
}

#[test]
fn completion_broadcast_survives_cancelled_waiter_holes_and_repolling() {
    let completion = Rc::new(RefCell::new(Completion::default()));
    let make = || super::MessageCompletion {
        inner: completion.clone(),
        waiter: None,
    };
    let mut first = Box::pin(make());
    let mut removed = Box::pin(make());
    let mut last = Box::pin(make());
    let first_wake = Arc::new(CountWake(AtomicUsize::new(0)));
    let removed_wake = Arc::new(CountWake(AtomicUsize::new(0)));
    let last_wake = Arc::new(CountWake(AtomicUsize::new(0)));
    let first_waker = Waker::from(first_wake.clone());
    let removed_waker = Waker::from(removed_wake.clone());
    let last_waker = Waker::from(last_wake.clone());
    for _ in 0..10 {
        assert!(
            first
                .as_mut()
                .poll(&mut FutureContext::from_waker(&first_waker))
                .is_pending()
        );
    }
    assert!(
        removed
            .as_mut()
            .poll(&mut FutureContext::from_waker(&removed_waker))
            .is_pending()
    );
    assert!(
        last.as_mut()
            .poll(&mut FutureContext::from_waker(&last_waker))
            .is_pending()
    );
    drop(removed);
    let mut reused = Box::pin(make());
    assert!(
        reused
            .as_mut()
            .poll(&mut FutureContext::from_waker(Waker::noop()))
            .is_pending()
    );
    let event = MessageCloseEvent {
        key: "broadcast".into(),
        reason: MessageCloseReason::Destroyed,
    };
    Completion::settle(&completion, event.clone());
    Completion::settle(
        &completion,
        MessageCloseEvent {
            key: "ignored".into(),
            reason: MessageCloseReason::OwnerDropped,
        },
    );
    assert_eq!(first_wake.0.load(Ordering::SeqCst), 1);
    assert_eq!(removed_wake.0.load(Ordering::SeqCst), 0);
    assert_eq!(last_wake.0.load(Ordering::SeqCst), 1);
    for mut future in [first, last, reused] {
        assert_eq!(
            future
                .as_mut()
                .poll(&mut FutureContext::from_waker(Waker::noop())),
            Poll::Ready(event.clone())
        );
    }
    assert!(completion.borrow().waiters.is_empty());
}

#[test]
fn timer_batch_callback_can_replace_a_key_without_old_work_closing_the_new_lifetime() {
    let mut fixture = Fixture::new();
    let latest = Rc::new(RefCell::new(None));
    let calls = Rc::new(Cell::new(0));
    let service = fixture.messages.clone();
    let observed_latest = latest.clone();
    let observed_calls = calls.clone();
    fixture.update(|messages, window, cx| {
        messages.open(
            MessageOptions::new("First")
                .key("first")
                .duration(Duration::from_millis(100))
                .on_close(move |_, window, cx| {
                    observed_calls.set(observed_calls.get() + 1);
                    *observed_latest.borrow_mut() = Some(
                        service.open(
                            MessageOptions::new("New second lifetime")
                                .key("second")
                                .duration(Duration::from_millis(500)),
                            window,
                            cx,
                        ),
                    );
                }),
            window,
            cx,
        );
        messages.open(
            MessageOptions::new("Old second")
                .key("second")
                .duration(Duration::from_millis(100)),
            window,
            cx,
        );
    });
    fixture
        .cx
        .executor()
        .advance_clock(Duration::from_millis(100));
    fixture.cx.run_until_parked();
    let handle = latest.borrow().as_ref().unwrap().clone();
    assert_eq!(calls.get(), 1);
    assert!(!handle.is_dismissed());
    fixture
        .cx
        .executor()
        .advance_clock(Duration::from_millis(499));
    fixture.cx.run_until_parked();
    assert!(!handle.is_dismissed());
    fixture
        .cx
        .executor()
        .advance_clock(Duration::from_millis(1));
    fixture.cx.run_until_parked();
    assert_eq!(result(&handle).unwrap().reason, MessageCloseReason::Timeout);
}

#[test]
fn deferred_eviction_keeps_its_reason_when_a_motionless_window_closes() {
    let mut fixture = Fixture::new();
    fixture.update(|messages, _, cx| {
        let scope = messages.state.upgrade().unwrap().read(cx).scope.clone();
        scope
            .update_patch(cx, |patch| patch.motion = Some(false))
            .unwrap();
    });
    let oldest = fixture.open(MessageOptions::new("Evicted before window removal"));
    fixture.open(MessageOptions::new("Active at window removal"));
    fixture.update(|messages, window, cx| {
        messages.set_config(
            MessageConfigPatch {
                max_count: Some(Some(1)),
                ..Default::default()
            },
            window,
            cx,
        );
        window.remove_window();
    });
    fixture.cx.run_until_parked();
    assert_eq!(result(&oldest).unwrap().reason, MessageCloseReason::Evicted);
}
