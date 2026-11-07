// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Window-owned, nonmodal feedback with native layout and cancellable scheduling.

mod config;
mod element;
mod state;
mod style;
mod view;

pub use config::{MessageConfig, MessageConfigPatch, MessageStack};
pub use state::MessageState;
pub use style::{
    MessageStyle, MessageStyleContext, MessageStyleResolver, MessageStyles, MessageTheme,
    MessageTokenPatch, MessageTokens,
};
pub use view::MessageHost;

use crate::{behavior::Accessibility, icons::IconRef};
use gpui::{
    AnyElement, App, Bounds, ClickEvent, Entity, Global, IntoElement, Pixels, SharedString,
    WeakEntity, Window, WindowId,
};
use std::{
    cell::RefCell,
    collections::HashMap,
    future::Future,
    pin::Pin,
    rc::Rc,
    task::{Context, Poll, Waker},
    time::Duration,
};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MessageType {
    Info,
    Success,
    Error,
    Warning,
    Loading,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MessageCloseReason {
    Timeout,
    Programmatic,
    Destroyed,
    Evicted,
    OwnerDropped,
    Unavailable,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct MessageCloseEvent {
    pub key: SharedString,
    pub reason: MessageCloseReason,
}

/// Final native card geometry, in logical client-window coordinates.
#[derive(Clone, Debug, PartialEq)]
pub struct MessageLayout {
    pub id: u64,
    pub key: SharedString,
    pub bounds: Bounds<Pixels>,
    pub closing: bool,
}

pub(super) type ContentFactory = Rc<dyn Fn(&MessageHandle, &mut Window, &mut App) -> AnyElement>;
pub(super) type CloseHandler = Rc<dyn Fn(&MessageCloseEvent, &mut Window, &mut App)>;
pub(super) type ClickHandler = Rc<dyn Fn(&ClickEvent, &mut Window, &mut App)>;

#[derive(Clone)]
pub(super) enum MessageContent {
    Text(SharedString),
    Factory(ContentFactory),
}

/// A retained description. Native elements are rebuilt by the host as needed.
#[derive(Clone)]
pub struct MessageOptions {
    pub(super) content: MessageContent,
    pub(super) kind: Option<MessageType>,
    pub(super) key: Option<SharedString>,
    pub(super) duration: Option<Duration>,
    pub(super) pause_on_hover: Option<bool>,
    pub(super) icon: Option<IconRef>,
    pub(super) show_icon: bool,
    pub(super) styles: MessageStyles,
    pub(super) styles_fn: Option<MessageStyleResolver>,
    pub(super) accessibility: Accessibility,
    pub(super) on_close: Option<CloseHandler>,
    pub(super) on_click: Option<ClickHandler>,
}

impl MessageOptions {
    pub fn new(text: impl Into<SharedString>) -> Self {
        Self {
            content: MessageContent::Text(text.into()),
            kind: None,
            key: None,
            duration: None,
            pause_on_hover: None,
            icon: None,
            show_icon: true,
            styles: MessageStyles::default(),
            styles_fn: None,
            accessibility: Accessibility::new(gpui::Role::Alert),
            on_close: None,
            on_click: None,
        }
    }

    pub fn content<E: IntoElement>(
        factory: impl Fn(&MessageHandle, &mut Window, &mut App) -> E + 'static,
    ) -> Self {
        let mut options = Self::new("");
        options.content = MessageContent::Factory(Rc::new(move |handle, window, cx| {
            factory(handle, window, cx).into_any_element()
        }));
        options
    }

    pub fn kind(mut self, kind: MessageType) -> Self {
        self.kind = Some(kind);
        self
    }
    pub fn key(mut self, key: impl Into<SharedString>) -> Self {
        self.key = Some(key.into());
        self
    }
    /// Zero keeps the notice until it is explicitly dismissed.
    pub fn duration(mut self, duration: Duration) -> Self {
        self.duration = Some(duration);
        self
    }
    pub fn pause_on_hover(mut self, pause: bool) -> Self {
        self.pause_on_hover = Some(pause);
        self
    }
    pub fn icon(mut self, icon: impl Into<IconRef>) -> Self {
        self.icon = Some(icon.into());
        self.show_icon = true;
        self
    }
    pub fn show_icon(mut self, show: bool) -> Self {
        self.show_icon = show;
        self
    }
    pub fn styles(mut self, styles: MessageStyles) -> Self {
        self.styles = styles;
        self
    }
    pub fn styles_fn(
        mut self,
        resolver: impl Fn(&MessageStyleContext, &mut Window, &mut App) -> MessageStyles
        + Send
        + Sync
        + 'static,
    ) -> Self {
        self.styles_fn = Some(MessageStyleResolver::new(resolver));
        self
    }
    pub fn accessibility_label(mut self, label: impl Into<SharedString>) -> Self {
        self.accessibility.label = Some(label.into());
        self
    }
    pub fn accessibility_description(mut self, description: impl Into<SharedString>) -> Self {
        self.accessibility.description = Some(description.into());
        self
    }
    pub fn accessibility_role(mut self, role: gpui::Role) -> Self {
        self.accessibility.role = Some(role);
        self
    }
    pub fn automation_id(mut self, id: impl Into<SharedString>) -> Self {
        self.accessibility.automation_id = Some(id.into());
        self
    }
    pub fn on_close(
        mut self,
        callback: impl Fn(&MessageCloseEvent, &mut Window, &mut App) + 'static,
    ) -> Self {
        self.on_close = Some(Rc::new(callback));
        self
    }
    pub fn on_click(
        mut self,
        callback: impl Fn(&ClickEvent, &mut Window, &mut App) + 'static,
    ) -> Self {
        self.on_click = Some(Rc::new(callback));
        self
    }
}

#[derive(Default)]
pub(super) struct Completion {
    closed: bool,
    result: Option<MessageCloseEvent>,
    waiters: Vec<Option<Waker>>,
}

impl Completion {
    fn settle(completion: &Rc<RefCell<Self>>, event: MessageCloseEvent) {
        let waiters = {
            let mut completion = completion.borrow_mut();
            if completion.result.is_some() {
                return;
            }
            completion.closed = true;
            completion.result = Some(event);
            std::mem::take(&mut completion.waiters)
        };
        for waiter in waiters.into_iter().flatten() {
            waiter.wake();
        }
    }
}

/// A future independent of the controller lifetime. Dropping it never dismisses a notice.
pub struct MessageCompletion {
    inner: Rc<RefCell<Completion>>,
    waiter: Option<usize>,
}
impl Future for MessageCompletion {
    type Output = MessageCloseEvent;
    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        let this = self.get_mut();
        let mut completion = this.inner.borrow_mut();
        if let Some(event) = &completion.result {
            return Poll::Ready(event.clone());
        }
        if let Some(index) = this.waiter {
            let slot = &mut completion.waiters[index];
            if !slot
                .as_ref()
                .is_some_and(|waker| waker.will_wake(cx.waker()))
            {
                *slot = Some(cx.waker().clone());
            }
        } else {
            let index = completion
                .waiters
                .iter()
                .position(Option::is_none)
                .unwrap_or(completion.waiters.len());
            if index == completion.waiters.len() {
                completion.waiters.push(Some(cx.waker().clone()));
            } else {
                completion.waiters[index] = Some(cx.waker().clone());
            }
            this.waiter = Some(index);
        }
        Poll::Pending
    }
}
impl Drop for MessageCompletion {
    fn drop(&mut self) {
        if let Some(index) = self.waiter {
            let mut completion = self.inner.borrow_mut();
            if let Some(slot) = completion.waiters.get_mut(index) {
                *slot = None;
            }
            while completion.waiters.last().is_some_and(Option::is_none) {
                completion.waiters.pop();
            }
        }
    }
}

/// A weak reference to one logical notice, including its keyed updates.
#[derive(Clone)]
pub struct MessageHandle {
    pub(super) state: WeakEntity<MessageState>,
    pub(super) id: u64,
    pub(super) key: SharedString,
    pub(super) completion: Rc<RefCell<Completion>>,
}
impl MessageHandle {
    pub fn key(&self) -> &SharedString {
        &self.key
    }
    pub fn close(&self, window: &mut Window, cx: &mut App) {
        if self.is_dismissed() {
            return;
        }
        if let Some(state) = self.state.upgrade()
            && state.read(cx).belongs_to(window)
        {
            let pending = state.update(cx, |state, cx| {
                state.close_id(self.id, MessageCloseReason::Programmatic, window, cx)
            });
            state::dispatch(pending, window, cx);
        }
    }
    /// Resolves when dismissal starts, before the outgoing visual finishes.
    pub fn completion(&self) -> MessageCompletion {
        MessageCompletion {
            inner: self.completion.clone(),
            waiter: None,
        }
    }
    pub fn is_dismissed(&self) -> bool {
        self.completion.borrow().closed
    }
}

#[derive(Default)]
struct Registry(HashMap<WindowId, WeakEntity<MessageState>>);
impl Global for Registry {}

/// Cheap window-scoped service. It never keeps the host or a native window alive.
#[derive(Clone)]
pub struct Message {
    state: WeakEntity<MessageState>,
}
impl Message {
    pub fn new(state: &Entity<MessageState>) -> Self {
        Self {
            state: state.downgrade(),
        }
    }
    pub fn for_window(window: &Window, cx: &App) -> Option<Self> {
        let state = cx
            .try_global::<Registry>()?
            .0
            .get(&window.window_handle().window_id())?;
        if !state.upgrade()?.read(cx).belongs_to(window) {
            return None;
        }
        Some(Self {
            state: state.clone(),
        })
    }
    pub(crate) fn register(&self, window: &Window, cx: &mut App) {
        if !cx.has_global::<Registry>() {
            cx.set_global(Registry::default());
        }
        let registry = cx.global_mut::<Registry>();
        registry.0.retain(|_, weak| weak.upgrade().is_some());
        registry
            .0
            .insert(window.window_handle().window_id(), self.state.clone());
    }
    pub fn open(
        &self,
        options: MessageOptions,
        window: &mut Window,
        cx: &mut App,
    ) -> MessageHandle {
        if let Some(state) = self.state.upgrade()
            && state.read(cx).belongs_to(window)
        {
            let (handle, pending) = state.update(cx, |state, cx| state.open(options, window, cx));
            state::dispatch(pending, window, cx);
            return handle;
        }
        let key = options.key.unwrap_or_default();
        let completion = Rc::new(RefCell::new(Completion::default()));
        Completion::settle(
            &completion,
            MessageCloseEvent {
                key: key.clone(),
                reason: MessageCloseReason::Unavailable,
            },
        );
        MessageHandle {
            state: self.state.clone(),
            id: 0,
            key,
            completion,
        }
    }
    pub fn info(
        &self,
        text: impl Into<SharedString>,
        window: &mut Window,
        cx: &mut App,
    ) -> MessageHandle {
        self.open(
            MessageOptions::new(text).kind(MessageType::Info),
            window,
            cx,
        )
    }
    pub fn success(
        &self,
        text: impl Into<SharedString>,
        window: &mut Window,
        cx: &mut App,
    ) -> MessageHandle {
        self.open(
            MessageOptions::new(text).kind(MessageType::Success),
            window,
            cx,
        )
    }
    pub fn error(
        &self,
        text: impl Into<SharedString>,
        window: &mut Window,
        cx: &mut App,
    ) -> MessageHandle {
        self.open(
            MessageOptions::new(text).kind(MessageType::Error),
            window,
            cx,
        )
    }
    pub fn warning(
        &self,
        text: impl Into<SharedString>,
        window: &mut Window,
        cx: &mut App,
    ) -> MessageHandle {
        self.open(
            MessageOptions::new(text).kind(MessageType::Warning),
            window,
            cx,
        )
    }
    pub fn loading(
        &self,
        text: impl Into<SharedString>,
        window: &mut Window,
        cx: &mut App,
    ) -> MessageHandle {
        self.open(
            MessageOptions::new(text).kind(MessageType::Loading),
            window,
            cx,
        )
    }
    pub fn destroy(&self, key: impl Into<SharedString>, window: &mut Window, cx: &mut App) {
        let key = key.into();
        if let Some(state) = self.state.upgrade()
            && state.read(cx).belongs_to(window)
        {
            let pending = state.update(cx, |state, cx| state.close_key(&key, window, cx));
            state::dispatch(pending, window, cx);
        }
    }
    pub fn destroy_all(&self, window: &mut Window, cx: &mut App) {
        if let Some(state) = self.state.upgrade()
            && state.read(cx).belongs_to(window)
        {
            let pending = state.update(cx, |state, cx| state.close_all(window, cx));
            state::dispatch(pending, window, cx);
        }
    }
    pub fn set_config(&self, config: MessageConfigPatch, window: &mut Window, cx: &mut App) {
        if let Some(state) = self.state.upgrade()
            && state.read(cx).belongs_to(window)
        {
            state.update(cx, |state, cx| state.set_config(config, window, cx));
        }
    }
    pub fn layouts(&self, cx: &App) -> Vec<MessageLayout> {
        self.state
            .upgrade()
            .map_or_else(Vec::new, |state| state.read(cx).layouts.clone())
    }
    pub fn len(&self, cx: &App) -> usize {
        self.state
            .upgrade()
            .map_or(0, |state| state.read(cx).active_count())
    }
    pub fn is_empty(&self, cx: &App) -> bool {
        self.len(cx) == 0
    }
}

#[cfg(test)]
mod tests;

#[cfg(test)]
mod core_audit_tests;
#[cfg(test)]
mod reference_tests;
#[cfg(test)]
mod render_tests;
