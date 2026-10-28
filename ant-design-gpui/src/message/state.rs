// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::{
    CloseHandler, Completion, MessageCloseEvent, MessageCloseReason, MessageConfig,
    MessageConfigPatch, MessageHandle, MessageLayout, MessageOptions, MessageTheme,
};
use crate::ConfigScope;
use gpui::{App, Context, IntoElement, Render, Subscription, Task, Window, WindowId};
use std::{
    cell::RefCell,
    rc::Rc,
    time::{Duration, Instant},
};

pub(super) struct Notice {
    pub id: u64,
    pub key: gpui::SharedString,
    pub options: MessageOptions,
    pub created: Instant,
    pub closing: Option<Instant>,
    pub exit_duration: Duration,
    close_reason: Option<MessageCloseReason>,
    duration: Duration,
    remaining: Duration,
    deadline: Option<Instant>,
    hovered: bool,
    paused: bool,
    completion: Rc<RefCell<Completion>>,
}

#[derive(Clone)]
pub(super) struct RenderNotice {
    pub id: u64,
    pub key: gpui::SharedString,
    pub options: MessageOptions,
    pub created: Instant,
    pub closing: Option<Instant>,
    pub exit_duration: Duration,
    pub handle: MessageHandle,
}

pub(super) struct Dismissal {
    event: MessageCloseEvent,
    callback: Option<CloseHandler>,
    completion: Rc<RefCell<Completion>>,
    focus: Option<super::view::FocusBoundary>,
}

pub(super) fn dispatch(pending: Vec<Dismissal>, window: &mut Window, cx: &mut App) {
    for dismissal in pending {
        // Settle first: callback reentrancy observes a terminal logical lifetime.
        Completion::settle(&dismissal.completion, dismissal.event.clone());
        if let Some(focus) = dismissal.focus {
            focus.dismiss(window, cx);
        }
        if let Some(callback) = dismissal.callback {
            callback(&dismissal.event, window, cx);
        }
    }
}

fn defer_dispatch(pending: Vec<Dismissal>, window: &Window, cx: &mut App) {
    if pending.is_empty() {
        return;
    }
    let window = window.window_handle();
    cx.defer(move |cx| {
        let mut pending = Some(pending);
        let result = window.update(cx, |_, window, cx| {
            dispatch(pending.take().unwrap_or_default(), window, cx)
        });
        if result.is_err() {
            for dismissal in pending.unwrap_or_default() {
                Completion::settle(&dismissal.completion, dismissal.event);
            }
        }
    });
}

pub(super) fn unmount(state: &gpui::WeakEntity<MessageState>, cx: &mut App) {
    let pending = state
        .update(cx, |state, _| state.detach())
        .unwrap_or_default();
    for dismissal in pending {
        Completion::settle(&dismissal.completion, dismissal.event);
    }
}

/// Retain one controller per mounted MessageHost. A controller belongs to one native window.
pub struct MessageState {
    pub(super) scope: ConfigScope,
    pub(super) notices: Vec<Notice>,
    pub(super) visual: Rc<RefCell<super::view::VisualState>>,
    pub(super) layouts: Vec<MessageLayout>,
    pub(super) list_hovered: bool,
    window_id: WindowId,
    overrides: MessageConfigPatch,
    sequence: u64,
    timer: Option<Task<()>>,
    timer_at: Option<Instant>,
    timer_generation: u64,
    pub(super) frame_pending: bool,
    owner_closed: bool,
    _config: Subscription,
    _window: Subscription,
}

impl MessageState {
    pub fn new(scope: ConfigScope, window: &mut Window, cx: &mut Context<Self>) -> Self {
        let subscription = scope.observe_in(window, cx, |state, window, cx| {
            state.sync_config(window, cx);
            cx.notify();
        });
        let window_id = window.window_handle().window_id();
        let weak = cx.weak_entity();
        let closed = cx.on_window_closed(move |cx, id| {
            if id == window_id {
                let pending = weak
                    .update(cx, |state, _| state.detach())
                    .unwrap_or_default();
                for dismissal in pending {
                    Completion::settle(&dismissal.completion, dismissal.event);
                }
            }
        });
        Self {
            scope,
            notices: Vec::new(),
            visual: Default::default(),
            layouts: Vec::new(),
            list_hovered: false,
            window_id,
            overrides: MessageConfigPatch::default(),
            sequence: 0,
            timer: None,
            timer_at: None,
            timer_generation: 0,
            frame_pending: false,
            owner_closed: false,
            _config: subscription,
            _window: closed,
        }
    }

    pub(super) fn belongs_to(&self, window: &Window) -> bool {
        !self.owner_closed && self.window_id == window.window_handle().window_id()
    }

    pub(super) fn window_matches(&self, window: &Window) -> bool {
        self.window_id == window.window_handle().window_id()
    }

    pub(super) fn mount(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if self.window_id == window.window_handle().window_id() && self.owner_closed {
            self.owner_closed = false;
            self.sync_config(window, cx);
            cx.notify();
        }
    }

    pub fn effective_config(&self, cx: &App) -> MessageConfig {
        self.overrides.resolve(&self.scope.snapshot(cx).message)
    }

    pub fn set_config(
        &mut self,
        patch: MessageConfigPatch,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if !self.belongs_to(window) || self.overrides == patch {
            return;
        }
        self.overrides = patch;
        self.sync_config(window, cx);
        cx.notify();
    }

    pub fn active_count(&self) -> usize {
        self.notices
            .iter()
            .filter(|notice| notice.closing.is_none())
            .count()
    }

    pub(super) fn render_notices(&self, cx: &Context<Self>) -> Vec<RenderNotice> {
        let weak = cx.weak_entity();
        self.notices
            .iter()
            .map(|notice| RenderNotice {
                id: notice.id,
                key: notice.key.clone(),
                options: notice.options.clone(),
                created: notice.created,
                closing: notice.closing,
                exit_duration: notice.exit_duration,
                handle: MessageHandle {
                    state: weak.clone(),
                    id: notice.id,
                    key: notice.key.clone(),
                    completion: notice.completion.clone(),
                },
            })
            .collect()
    }

    pub(super) fn open(
        &mut self,
        options: MessageOptions,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> (MessageHandle, Vec<Dismissal>) {
        let now = cx.background_executor().now();
        self.finish_exits(now, window, cx);
        let config = self.effective_config(cx);
        let duration = options.duration.unwrap_or(config.duration);
        let existing = options.key.as_ref().and_then(|key| {
            self.notices
                .iter()
                .position(|notice| notice.closing.is_none() && &notice.key == key)
        });
        let index = if let Some(index) = existing {
            let notice = &mut self.notices[index];
            if notice.duration != duration {
                notice.remaining = duration;
                notice.duration = duration;
                notice.deadline = (!duration.is_zero() && !notice.paused)
                    .then(|| now.checked_add(duration))
                    .flatten();
            }
            notice.options = options;
            index
        } else {
            // The process cannot practically exhaust this counter; failing closed
            // also avoids ever reusing a lifetime id in async work.
            self.sequence = self
                .sequence
                .checked_add(1)
                .expect("Message lifetime space exhausted");
            let key = if let Some(key) = options.key.clone() {
                key
            } else {
                loop {
                    let candidate: gpui::SharedString = format!("message-{}", self.sequence).into();
                    if !self.notices.iter().any(|notice| notice.key == candidate) {
                        break candidate;
                    }
                    self.sequence = self
                        .sequence
                        .checked_add(1)
                        .expect("Message lifetime space exhausted");
                }
            };
            let id = self.sequence;
            self.notices.insert(
                0,
                Notice {
                    id,
                    key,
                    options,
                    created: now,
                    closing: None,
                    exit_duration: Duration::ZERO,
                    close_reason: None,
                    duration,
                    remaining: duration,
                    deadline: if duration.is_zero() {
                        None
                    } else {
                        now.checked_add(duration)
                    },
                    hovered: false,
                    paused: false,
                    completion: Rc::new(RefCell::new(Completion::default())),
                },
            );
            0
        };
        let handle = {
            let notice = &self.notices[index];
            MessageHandle {
                state: cx.weak_entity(),
                id: notice.id,
                key: notice.key.clone(),
                completion: notice.completion.clone(),
            }
        };
        self.sync_paused(&config, now);
        let pending = self.enforce_limit(&config, now, cx);
        self.reschedule(window, cx);
        cx.notify();
        (handle, pending)
    }

    fn exit_duration(&self, cx: &App) -> Duration {
        let config = self.scope.snapshot(cx);
        let tokens = config.theme.component::<MessageTheme>();
        if config.motion && tokens.globals().motion && !cx.reduce_motion() {
            tokens.globals().motion_duration_mid
        } else {
            Duration::ZERO
        }
    }

    fn begin_close(
        &mut self,
        id: u64,
        reason: MessageCloseReason,
        now: Instant,
        cx: &App,
    ) -> Option<Dismissal> {
        let base_duration = self.exit_duration(cx);
        let curve = self
            .scope
            .snapshot(cx)
            .theme
            .component::<MessageTheme>()
            .globals()
            .motion_ease_in_out;
        let presented = super::view::was_presented(&self.visual, id);
        let focus = super::view::closing_boundary(&self.visual, id);
        let notice = self
            .notices
            .iter_mut()
            .find(|notice| notice.id == id && notice.closing.is_none())?;
        notice.closing = Some(now);
        notice.close_reason = Some(reason);
        notice.completion.borrow_mut().closed = true;
        notice.exit_duration = if presented {
            super::view::interrupted_exit_duration(notice.created, now, base_duration, curve)
        } else {
            Duration::ZERO
        };
        notice.deadline = None;
        Some(Dismissal {
            event: MessageCloseEvent {
                key: notice.key.clone(),
                reason,
            },
            callback: notice.options.on_close.take(),
            completion: notice.completion.clone(),
            focus,
        })
    }

    pub(super) fn close_id(
        &mut self,
        id: u64,
        reason: MessageCloseReason,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Vec<Dismissal> {
        let now = cx.background_executor().now();
        let pending = self.begin_close(id, reason, now, cx).into_iter().collect();
        self.finish_exits(now, window, cx);
        self.reschedule(window, cx);
        cx.notify();
        pending
    }

    pub(super) fn close_key(
        &mut self,
        key: &gpui::SharedString,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Vec<Dismissal> {
        let ids = self
            .notices
            .iter()
            .filter(|notice| &notice.key == key && notice.closing.is_none())
            .map(|notice| notice.id)
            .collect::<Vec<_>>();
        self.close_ids(ids, MessageCloseReason::Destroyed, window, cx)
    }

    pub(super) fn close_all(
        &mut self,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Vec<Dismissal> {
        let ids = self
            .notices
            .iter()
            .filter(|notice| notice.closing.is_none())
            .map(|notice| notice.id)
            .collect::<Vec<_>>();
        self.close_ids(ids, MessageCloseReason::Destroyed, window, cx)
    }

    fn close_ids(
        &mut self,
        ids: Vec<u64>,
        reason: MessageCloseReason,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Vec<Dismissal> {
        let now = cx.background_executor().now();
        let pending = ids
            .into_iter()
            .filter_map(|id| self.begin_close(id, reason, now, cx))
            .collect();
        self.finish_exits(now, window, cx);
        self.reschedule(window, cx);
        cx.notify();
        pending
    }

    fn enforce_limit(&mut self, config: &MessageConfig, now: Instant, cx: &App) -> Vec<Dismissal> {
        let Some(limit) = config.max_count.filter(|limit| *limit > 0) else {
            return Vec::new();
        };
        let ids = self
            .notices
            .iter()
            .filter(|notice| notice.closing.is_none())
            .skip(limit)
            .map(|notice| notice.id)
            .collect::<Vec<_>>();
        ids.into_iter()
            .filter_map(|id| self.begin_close(id, MessageCloseReason::Evicted, now, cx))
            .collect()
    }

    fn sync_paused(&mut self, config: &MessageConfig, now: Instant) {
        for notice in &mut self.notices {
            if notice.closing.is_some() {
                continue;
            }
            let pause = notice
                .options
                .pause_on_hover
                .unwrap_or(config.pause_on_hover)
                && (notice.hovered || (config.stack.threshold().is_some() && self.list_hovered));
            if pause == notice.paused {
                continue;
            }
            notice.paused = pause;
            if pause {
                if let Some(deadline) = notice.deadline.take() {
                    notice.remaining = deadline.saturating_duration_since(now);
                }
            } else if !notice.duration.is_zero() {
                notice.deadline = now.checked_add(notice.remaining);
            }
        }
    }

    fn sync_config(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let now = cx.background_executor().now();
        let config = self.effective_config(cx);
        self.sync_paused(&config, now);
        let pending = self.enforce_limit(&config, now, cx);
        self.finish_exits(now, window, cx);
        self.reschedule(window, cx);
        defer_dispatch(pending, window, cx);
    }

    pub(super) fn hover(
        &mut self,
        id: u64,
        hovered: bool,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if let Some(notice) = self
            .notices
            .iter_mut()
            .find(|notice| notice.id == id && notice.closing.is_none())
        {
            notice.hovered = hovered;
            let config = self.effective_config(cx);
            self.sync_paused(&config, cx.background_executor().now());
            self.reschedule(window, cx);
        }
    }

    pub(super) fn list_hover(
        &mut self,
        hovered: bool,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.list_hovered == hovered {
            return;
        }
        self.list_hovered = hovered;
        let config = self.effective_config(cx);
        self.sync_paused(&config, cx.background_executor().now());
        self.reschedule(window, cx);
        if config.stack.threshold().is_some() {
            cx.notify();
        }
    }

    pub(super) fn finish_exits(&mut self, now: Instant, _: &mut Window, cx: &mut Context<Self>) {
        let config = self.scope.snapshot(cx);
        let motion = config.motion
            && config.theme.component::<MessageTheme>().globals().motion
            && !cx.reduce_motion();
        self.notices.retain(|notice| {
            notice.closing.is_none_or(|start| {
                motion && now.saturating_duration_since(start) < notice.exit_duration
            })
        });
        if self.notices.is_empty() {
            self.layouts.clear();
            self.list_hovered = false;
        }
    }

    fn reschedule(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let next = self
            .notices
            .iter()
            .filter_map(|notice| {
                notice.closing.map_or(notice.deadline, |start| {
                    start.checked_add(notice.exit_duration)
                })
            })
            .min();
        if next == self.timer_at {
            return;
        }
        self.timer_generation = self.timer_generation.wrapping_add(1);
        self.timer = None;
        self.timer_at = next;
        let Some(deadline) = next else {
            return;
        };
        let generation = self.timer_generation;
        let timer = cx
            .background_executor()
            .timer(deadline.saturating_duration_since(cx.background_executor().now()));
        self.timer = Some(cx.spawn_in(window, async move |weak, cx| {
            timer.await;
            let _ = cx.update(|window, cx| {
                let pending = weak
                    .update(cx, |state, cx| {
                        if state.timer_generation != generation {
                            return Vec::new();
                        }
                        state.timer = None;
                        state.timer_at = None;
                        let now = cx.background_executor().now();
                        let ids = state
                            .notices
                            .iter()
                            .filter(|notice| {
                                notice.deadline.is_some_and(|deadline| deadline <= now)
                            })
                            .map(|notice| notice.id)
                            .collect::<Vec<_>>();
                        let pending = ids
                            .into_iter()
                            .filter_map(|id| {
                                state.begin_close(id, MessageCloseReason::Timeout, now, cx)
                            })
                            .collect();
                        state.finish_exits(now, window, cx);
                        state.reschedule(window, cx);
                        cx.notify();
                        pending
                    })
                    .unwrap_or_default();
                dispatch(pending, window, cx);
            });
        }));
    }

    pub(super) fn schedule_frame(&mut self, window: &Window, cx: &Context<Self>) {
        if self.frame_pending
            || self.owner_closed
            || self.notices.is_empty()
            || !window.is_visible()
        {
            return;
        }
        self.frame_pending = true;
        let weak = cx.weak_entity();
        window.on_next_frame(move |_, cx| {
            let _ = weak.update(cx, |state, cx| {
                state.frame_pending = false;
                if !state.owner_closed && !state.notices.is_empty() {
                    cx.notify();
                }
            });
        });
    }

    fn detach(&mut self) -> Vec<Dismissal> {
        self.owner_closed = true;
        self.timer_generation = self.timer_generation.wrapping_add(1);
        self.timer = None;
        self.timer_at = None;
        self.layouts.clear();
        self.list_hovered = false;
        self.visual.borrow_mut().clear_content();
        self.notices
            .drain(..)
            .map(|notice| Dismissal {
                event: MessageCloseEvent {
                    key: notice.key,
                    reason: notice
                        .close_reason
                        .unwrap_or(MessageCloseReason::OwnerDropped),
                },
                callback: None,
                completion: notice.completion,
                focus: None,
            })
            .collect()
    }
}

impl Render for MessageState {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        super::view::render(self, window, cx)
    }
}

impl Drop for MessageState {
    fn drop(&mut self) {
        for dismissal in self.detach() {
            Completion::settle(&dismissal.completion, dismissal.event);
        }
    }
}
