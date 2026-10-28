// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
use super::{ModalAutoFocus, ModalKind, ModalOptions, ModalState, host};
use crate::ConfigScope;
use gpui::{App, AppContext, Entity, WeakEntity, Window, WindowId};
use std::{
    cell::RefCell,
    future::Future,
    pin::Pin,
    rc::Rc,
    task::{Context, Poll, Waker},
};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ModalOutcome {
    Confirmed,
    Cancelled,
    Destroyed,
}
#[derive(Default)]
pub(super) struct Completion {
    outcome: RefCell<Option<ModalOutcome>>,
    wakers: RefCell<Vec<Waker>>,
}
impl Completion {
    pub(super) fn finish(&self, outcome: ModalOutcome) {
        if self.outcome.borrow().is_some() {
            return;
        }
        *self.outcome.borrow_mut() = Some(outcome);
        for waker in self.wakers.take() {
            waker.wake();
        }
    }
}
/// A cloneable, awaitable result; every waiter observes the same terminal outcome.
#[derive(Clone)]
pub struct ModalResult(Rc<Completion>);
impl Future for ModalResult {
    type Output = ModalOutcome;
    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        if let Some(outcome) = *self.0.outcome.borrow() {
            Poll::Ready(outcome)
        } else {
            let mut wakers = self.0.wakers.borrow_mut();
            if !wakers.iter().any(|w| w.will_wake(cx.waker())) {
                wakers.push(cx.waker().clone());
            }
            Poll::Pending
        }
    }
}
#[derive(Clone)]
pub struct ModalHandle {
    pub(super) state: WeakEntity<ModalState>,
    completion: Option<Rc<Completion>>,
}
impl ModalHandle {
    pub(super) fn from_entity(state: &Entity<ModalState>, cx: &App) -> Self {
        Self {
            state: state.downgrade(),
            completion: state.read(cx).completion.clone(),
        }
    }
    pub fn state(&self) -> Option<Entity<ModalState>> {
        self.state.upgrade()
    }
    pub fn is_open(&self, cx: &App) -> bool {
        self.state.upgrade().is_some_and(|s| s.read(cx).is_open())
    }
    pub fn update(&self, update: impl FnOnce(&mut ModalOptions), cx: &mut App) {
        let _ = self.state.update(cx, |s, cx| {
            let mut options = s.options.clone();
            update(&mut options);
            s.set_options(options, cx);
        });
    }
    pub fn close(&self, window: &mut Window, cx: &mut App) {
        let _ = self.state.update(cx, |s, cx| {
            if let Some(completion) = &s.completion {
                completion.finish(ModalOutcome::Destroyed);
            }
            s.set_open(false, window, cx);
        });
    }
    pub fn destroy(&self, window: &mut Window, cx: &mut App) {
        self.close(window, cx);
    }
    pub fn result(&self, _: &App) -> ModalResult {
        let completion = self.completion.clone().unwrap_or_else(|| {
            let completion = Rc::new(Completion::default());
            completion.finish(ModalOutcome::Destroyed);
            completion
        });
        ModalResult(completion)
    }
    pub fn confirm(&self, window: &mut Window, cx: &mut App) {
        if let Some(entity) = self.state.upgrade() {
            super::state::activate(&entity, None, window, cx);
        }
    }
    pub fn cancel(&self, window: &mut Window, cx: &mut App) {
        if let Some(entity) = self.state.upgrade() {
            super::state::activate(&entity, Some(super::ModalCancelReason::Button), window, cx);
        }
    }
}

/// A context-preserving native equivalent of Modal.useModal. Never crosses windows.
#[derive(Clone)]
pub struct ModalManager {
    scope: ConfigScope,
    window: WindowId,
    handles: Rc<RefCell<Vec<ModalHandle>>>,
}
impl ModalManager {
    pub fn new(scope: ConfigScope, window: &Window) -> Self {
        Self {
            scope,
            window: window.window_handle().window_id(),
            handles: Rc::default(),
        }
    }
    pub fn show(&self, options: ModalOptions, window: &mut Window, cx: &mut App) -> ModalHandle {
        assert_eq!(
            self.window,
            window.window_handle().window_id(),
            "ModalManager belongs to another window"
        );
        let state = cx.new(|cx| {
            let mut s = ModalState::new(window, cx);
            s.set_scope(self.scope.clone(), cx);
            s.options = options;
            s.managed = true;
            s.completion = Some(Rc::default());
            s.mounted = true;
            s
        });
        host::manage(state.clone(), window, cx);
        state.update(cx, |s, cx| s.set_open(true, window, cx));
        let handle = ModalHandle::from_entity(&state, cx);
        let mut handles = self.handles.borrow_mut();
        handles.retain(|h| h.state.upgrade().is_some());
        handles.push(handle.clone());
        handle
    }
    fn kind(
        &self,
        kind: ModalKind,
        mut options: ModalOptions,
        window: &mut Window,
        cx: &mut App,
    ) -> ModalHandle {
        options.kind = kind;
        if options.auto_focus == ModalAutoFocus::Dialog {
            options.auto_focus = ModalAutoFocus::Ok;
        }
        self.show(options, window, cx)
    }
    pub fn confirm(&self, options: ModalOptions, window: &mut Window, cx: &mut App) -> ModalHandle {
        self.kind(ModalKind::Confirm, options, window, cx)
    }
    pub fn info(&self, options: ModalOptions, window: &mut Window, cx: &mut App) -> ModalHandle {
        self.kind(ModalKind::Info, options, window, cx)
    }
    pub fn success(&self, options: ModalOptions, window: &mut Window, cx: &mut App) -> ModalHandle {
        self.kind(ModalKind::Success, options, window, cx)
    }
    pub fn warning(&self, options: ModalOptions, window: &mut Window, cx: &mut App) -> ModalHandle {
        self.kind(ModalKind::Warning, options, window, cx)
    }
    pub fn error(&self, options: ModalOptions, window: &mut Window, cx: &mut App) -> ModalHandle {
        self.kind(ModalKind::Error, options, window, cx)
    }
    pub fn destroy_all(&self, window: &mut Window, cx: &mut App) {
        assert_eq!(
            self.window,
            window.window_handle().window_id(),
            "ModalManager belongs to another window"
        );
        let handles = self.handles.borrow().clone();
        for handle in handles {
            handle.destroy(window, cx);
        }
    }
}
impl Drop for ModalState {
    fn drop(&mut self) {
        if let Some(completion) = &self.completion {
            completion.finish(ModalOutcome::Destroyed);
        }
    }
}
