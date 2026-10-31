// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Window-owned, nonmodal notifications with native content and retained lifetimes.

mod card;
mod config;
mod geometry;
mod state;
mod style;
mod view;

pub use config::{
    NotificationConfig, NotificationConfigPatch, NotificationIcon, NotificationStack,
};
pub use state::NotificationState;
pub use style::{
    NotificationStyleContext, NotificationStyleResolver, NotificationStyles, NotificationTheme,
    NotificationTokenPatch, NotificationTokens,
};
pub use view::NotificationHost;

#[cfg(test)]
mod tests;

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

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Default)]
pub enum NotificationPlacement {
    Top,
    TopLeft,
    #[default]
    TopRight,
    Bottom,
    BottomLeft,
    BottomRight,
}
impl NotificationPlacement {
    pub const ALL: [Self; 6] = [
        Self::Top,
        Self::TopLeft,
        Self::TopRight,
        Self::Bottom,
        Self::BottomLeft,
        Self::BottomRight,
    ];
    pub(crate) fn index(self) -> usize {
        self as usize
    }
    pub(crate) fn bottom(self) -> bool {
        matches!(self, Self::Bottom | Self::BottomLeft | Self::BottomRight)
    }
    pub(crate) fn center(self) -> bool {
        matches!(self, Self::Top | Self::Bottom)
    }
    pub(crate) fn right(self) -> bool {
        matches!(self, Self::TopRight | Self::BottomRight)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum NotificationType {
    Info,
    Success,
    Warning,
    Error,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum NotificationCloseReason {
    Timeout,
    CloseButton,
    Programmatic,
    Destroyed,
    Evicted,
    OwnerDropped,
    Unavailable,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct NotificationCloseEvent {
    pub key: SharedString,
    pub reason: NotificationCloseReason,
}

/// Measured client-window geometry in logical pixels, including outgoing lifetimes.
#[derive(Clone, Debug, PartialEq)]
pub struct NotificationLayout {
    pub id: u64,
    pub key: SharedString,
    pub placement: NotificationPlacement,
    pub bounds: Bounds<Pixels>,
    pub closing: bool,
    pub visible: bool,
}

type ContentFactory = Rc<dyn Fn(&NotificationHandle, &mut Window, &mut App) -> AnyElement>;
type CloseHandler = Rc<dyn Fn(&NotificationCloseEvent, &mut Window, &mut App)>;
type ClickHandler = Rc<dyn Fn(&ClickEvent, &mut Window, &mut App)>;

#[derive(Clone)]
pub(super) enum Content {
    Text(SharedString),
    Factory(ContentFactory),
}
impl Content {
    fn factory<E: IntoElement>(
        f: impl Fn(&NotificationHandle, &mut Window, &mut App) -> E + 'static,
    ) -> Self {
        Self::Factory(Rc::new(move |handle, window, cx| {
            f(handle, window, cx).into_any_element()
        }))
    }
}

/// Retained content descriptions. Factories build native elements; keep application state in entities.
#[derive(Clone)]
pub struct NotificationOptions {
    pub(super) title: Option<Content>,
    pub(super) description: Option<Content>,
    pub(super) actions: Option<Content>,
    pub(super) kind: Option<NotificationType>,
    pub(super) key: Option<SharedString>,
    pub(super) placement: Option<NotificationPlacement>,
    pub(super) duration: Option<Duration>,
    pub(super) pause_on_hover: Option<bool>,
    pub(super) show_progress: Option<bool>,
    pub(super) closable: Option<bool>,
    pub(super) close_disabled: bool,
    pub(super) close_label: Option<SharedString>,
    pub(super) close_icon: Option<IconRef>,
    pub(super) icon: Option<IconRef>,
    pub(super) icon_content: Option<Content>,
    pub(super) show_icon: bool,
    pub(super) styles: NotificationStyles,
    pub(super) styles_fn: Option<NotificationStyleResolver>,
    pub(super) accessibility: Accessibility,
    pub(super) on_close: Option<CloseHandler>,
    pub(super) on_click: Option<ClickHandler>,
}
impl Default for NotificationOptions {
    fn default() -> Self {
        Self {
            title: None,
            description: None,
            actions: None,
            kind: None,
            key: None,
            placement: None,
            duration: None,
            pause_on_hover: None,
            show_progress: None,
            closable: None,
            close_disabled: false,
            close_label: None,
            close_icon: None,
            icon: None,
            icon_content: None,
            show_icon: true,
            styles: Default::default(),
            styles_fn: None,
            accessibility: Accessibility::new(gpui::Role::Alert),
            on_close: None,
            on_click: None,
        }
    }
}
impl NotificationOptions {
    pub fn new(title: impl Into<SharedString>) -> Self {
        Self::default().title(title)
    }
    pub fn title(mut self, text: impl Into<SharedString>) -> Self {
        self.title = Some(Content::Text(text.into()));
        self
    }
    pub fn description(mut self, text: impl Into<SharedString>) -> Self {
        self.description = Some(Content::Text(text.into()));
        self
    }
    pub fn title_content<E: IntoElement>(
        mut self,
        f: impl Fn(&NotificationHandle, &mut Window, &mut App) -> E + 'static,
    ) -> Self {
        self.title = Some(Content::factory(f));
        self
    }
    pub fn description_content<E: IntoElement>(
        mut self,
        f: impl Fn(&NotificationHandle, &mut Window, &mut App) -> E + 'static,
    ) -> Self {
        self.description = Some(Content::factory(f));
        self
    }
    pub fn actions<E: IntoElement>(
        mut self,
        f: impl Fn(&NotificationHandle, &mut Window, &mut App) -> E + 'static,
    ) -> Self {
        self.actions = Some(Content::factory(f));
        self
    }
    pub fn icon_content<E: IntoElement>(
        mut self,
        f: impl Fn(&NotificationHandle, &mut Window, &mut App) -> E + 'static,
    ) -> Self {
        self.icon_content = Some(Content::factory(f));
        self.show_icon = true;
        self
    }
    pub fn kind(mut self, kind: NotificationType) -> Self {
        self.kind = Some(kind);
        self
    }
    pub fn key(mut self, key: impl Into<SharedString>) -> Self {
        self.key = Some(key.into());
        self
    }
    pub fn placement(mut self, placement: NotificationPlacement) -> Self {
        self.placement = Some(placement);
        self
    }
    /// Zero disables automatic dismissal and its countdown bar.
    pub fn duration(mut self, duration: Duration) -> Self {
        self.duration = Some(duration);
        self
    }
    pub fn pause_on_hover(mut self, enabled: bool) -> Self {
        self.pause_on_hover = Some(enabled);
        self
    }
    pub fn show_progress(mut self, enabled: bool) -> Self {
        self.show_progress = Some(enabled);
        self
    }
    pub fn closable(mut self, enabled: bool) -> Self {
        self.closable = Some(enabled);
        self
    }
    pub fn close_disabled(mut self, disabled: bool) -> Self {
        self.close_disabled = disabled;
        self
    }
    pub fn close_label(mut self, label: impl Into<SharedString>) -> Self {
        self.close_label = Some(label.into());
        self
    }
    pub fn close_icon(mut self, icon: impl Into<IconRef>) -> Self {
        self.close_icon = Some(icon.into());
        self
    }
    pub fn icon(mut self, icon: impl Into<IconRef>) -> Self {
        self.icon = Some(icon.into());
        self.icon_content = None;
        self.show_icon = true;
        self
    }
    pub fn show_icon(mut self, enabled: bool) -> Self {
        self.show_icon = enabled;
        self
    }
    pub fn styles(mut self, styles: NotificationStyles) -> Self {
        self.styles = styles;
        self
    }
    pub fn styles_fn(
        mut self,
        f: impl Fn(&NotificationStyleContext, &mut Window, &mut App) -> NotificationStyles
        + Send
        + Sync
        + 'static,
    ) -> Self {
        self.styles_fn = Some(NotificationStyleResolver::new(f));
        self
    }
    pub fn accessibility_label(mut self, label: impl Into<SharedString>) -> Self {
        self.accessibility.label = Some(label.into());
        self
    }
    pub fn accessibility_description(mut self, label: impl Into<SharedString>) -> Self {
        self.accessibility.description = Some(label.into());
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
        f: impl Fn(&NotificationCloseEvent, &mut Window, &mut App) + 'static,
    ) -> Self {
        self.on_close = Some(Rc::new(f));
        self
    }
    pub fn on_click(mut self, f: impl Fn(&ClickEvent, &mut Window, &mut App) + 'static) -> Self {
        self.on_click = Some(Rc::new(f));
        self
    }
}

#[derive(Default)]
pub(super) struct Completion {
    dismissed: bool,
    result: Option<NotificationCloseEvent>,
    waiters: Vec<Option<Waker>>,
}
impl Completion {
    fn settle(inner: &Rc<RefCell<Self>>, event: NotificationCloseEvent) {
        let waiters = {
            let mut inner = inner.borrow_mut();
            if inner.result.is_some() {
                return;
            }
            inner.dismissed = true;
            inner.result = Some(event);
            std::mem::take(&mut inner.waiters)
        };
        for waker in waiters.into_iter().flatten() {
            waker.wake();
        }
    }
}

/// Resolves at logical dismissal, before the outgoing visual finishes animating.
pub struct NotificationCompletion {
    inner: Rc<RefCell<Completion>>,
    waiter: Option<usize>,
}
impl Future for NotificationCompletion {
    type Output = NotificationCloseEvent;
    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        let this = self.get_mut();
        let mut inner = this.inner.borrow_mut();
        if let Some(event) = &inner.result {
            return Poll::Ready(event.clone());
        }
        if let Some(index) = this.waiter {
            if !inner.waiters[index]
                .as_ref()
                .is_some_and(|w| w.will_wake(cx.waker()))
            {
                inner.waiters[index] = Some(cx.waker().clone());
            }
        } else {
            let index = inner
                .waiters
                .iter()
                .position(Option::is_none)
                .unwrap_or(inner.waiters.len());
            if index == inner.waiters.len() {
                inner.waiters.push(None);
            }
            inner.waiters[index] = Some(cx.waker().clone());
            this.waiter = Some(index);
        }
        Poll::Pending
    }
}
impl Drop for NotificationCompletion {
    fn drop(&mut self) {
        if let Some(index) = self.waiter {
            let mut inner = self.inner.borrow_mut();
            if let Some(slot) = inner.waiters.get_mut(index) {
                *slot = None;
            }
            while inner.waiters.last().is_some_and(Option::is_none) {
                inner.waiters.pop();
            }
        }
    }
}

#[derive(Clone)]
pub struct NotificationHandle {
    state: WeakEntity<NotificationState>,
    id: u64,
    key: SharedString,
    completion: Rc<RefCell<Completion>>,
}
impl NotificationHandle {
    pub fn key(&self) -> &SharedString {
        &self.key
    }
    pub fn is_dismissed(&self) -> bool {
        self.completion.borrow().dismissed
    }
    pub fn completion(&self) -> NotificationCompletion {
        NotificationCompletion {
            inner: self.completion.clone(),
            waiter: None,
        }
    }
    pub fn close(&self, window: &mut Window, cx: &mut App) {
        self.close_with(NotificationCloseReason::Programmatic, window, cx);
    }
    fn close_with(&self, reason: NotificationCloseReason, window: &mut Window, cx: &mut App) {
        if self.is_dismissed() {
            return;
        }
        if let Some(state) = self.state.upgrade()
            && state.read(cx).belongs_to(window)
        {
            let pending = state.update(cx, |state, cx| state.close(self.id, reason, window, cx));
            state::dispatch(pending, window, cx);
        }
    }
}

#[derive(Default)]
struct Registry(HashMap<WindowId, WeakEntity<NotificationState>>);
impl Global for Registry {}

/// Cheap scoped service. Neither the service nor its handles retain the native window.
#[derive(Clone)]
pub struct Notification {
    state: WeakEntity<NotificationState>,
}
impl Notification {
    pub fn new(state: &Entity<NotificationState>) -> Self {
        Self {
            state: state.downgrade(),
        }
    }
    pub fn for_window(window: &Window, cx: &App) -> Option<Self> {
        let state = cx
            .try_global::<Registry>()?
            .0
            .get(&window.window_handle().window_id())?
            .clone();
        state
            .upgrade()?
            .read(cx)
            .belongs_to(window)
            .then_some(Self { state })
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
        options: NotificationOptions,
        window: &mut Window,
        cx: &mut App,
    ) -> NotificationHandle {
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
            NotificationCloseEvent {
                key: key.clone(),
                reason: NotificationCloseReason::Unavailable,
            },
        );
        NotificationHandle {
            state: self.state.clone(),
            id: 0,
            key,
            completion,
        }
    }
    pub fn info(
        &self,
        options: NotificationOptions,
        window: &mut Window,
        cx: &mut App,
    ) -> NotificationHandle {
        self.open(options.kind(NotificationType::Info), window, cx)
    }
    pub fn success(
        &self,
        options: NotificationOptions,
        window: &mut Window,
        cx: &mut App,
    ) -> NotificationHandle {
        self.open(options.kind(NotificationType::Success), window, cx)
    }
    pub fn warning(
        &self,
        options: NotificationOptions,
        window: &mut Window,
        cx: &mut App,
    ) -> NotificationHandle {
        self.open(options.kind(NotificationType::Warning), window, cx)
    }
    pub fn error(
        &self,
        options: NotificationOptions,
        window: &mut Window,
        cx: &mut App,
    ) -> NotificationHandle {
        self.open(options.kind(NotificationType::Error), window, cx)
    }
    pub fn destroy(&self, key: impl Into<SharedString>, window: &mut Window, cx: &mut App) {
        self.destroy_matching(Some(key.into()), window, cx);
    }
    pub fn destroy_all(&self, window: &mut Window, cx: &mut App) {
        self.destroy_matching(None, window, cx);
    }
    fn destroy_matching(&self, key: Option<SharedString>, window: &mut Window, cx: &mut App) {
        if let Some(state) = self.state.upgrade()
            && state.read(cx).belongs_to(window)
        {
            let pending = state.update(cx, |state, cx| state.destroy(key.as_ref(), window, cx));
            state::dispatch(pending, window, cx);
        }
    }
    pub fn set_config(&self, config: NotificationConfigPatch, window: &mut Window, cx: &mut App) {
        if let Some(state) = self.state.upgrade()
            && state.read(cx).belongs_to(window)
        {
            state.update(cx, |state, cx| state.set_config(config, window, cx));
        }
    }
    pub fn layouts(&self, cx: &App) -> Vec<NotificationLayout> {
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
