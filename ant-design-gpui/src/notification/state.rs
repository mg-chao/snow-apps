// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::{
    CloseHandler, Completion, NotificationCloseEvent, NotificationCloseReason, NotificationConfig,
    NotificationConfigPatch, NotificationHandle, NotificationLayout, NotificationOptions,
    NotificationPlacement, NotificationTheme,
};
use crate::{
    ConfigScope,
    behavior::{evaluate_cubic_bezier, notice::FocusBoundary},
};
use gpui::{App, Context, IntoElement, Render, SharedString, Subscription, Task, Window, WindowId};
use std::{
    cell::RefCell,
    rc::Rc,
    time::{Duration, Instant},
};

/// One monotonic budget drives both expiry and visual progress; renders never reset it.
#[derive(Clone, Copy)]
pub(super) struct Budget {
    duration: Duration,
    remaining: Duration,
    deadline: Option<Instant>,
}
impl Budget {
    fn new(duration: Duration, now: Instant) -> Self {
        Self {
            duration,
            remaining: duration,
            deadline: (!duration.is_zero())
                .then(|| now.checked_add(duration))
                .flatten(),
        }
    }
    fn remaining(self, now: Instant) -> Duration {
        self.deadline
            .map_or(self.remaining, |d| d.saturating_duration_since(now))
    }
    fn pause(&mut self, paused: bool, now: Instant) {
        if paused {
            self.remaining = self.remaining(now);
            self.deadline = None;
        } else if self.deadline.is_none() && !self.duration.is_zero() {
            self.deadline = now.checked_add(self.remaining);
        }
    }
    pub(super) fn fraction(self, now: Instant) -> f32 {
        if self.duration.is_zero() {
            0.
        } else {
            (self.remaining(now).as_secs_f64() / self.duration.as_secs_f64()).clamp(0., 1.) as f32
        }
    }
    pub(super) fn running(self) -> bool {
        self.deadline.is_some()
    }
    pub(super) fn timed(self) -> bool {
        !self.duration.is_zero()
    }
}

pub(super) struct Notice {
    id: u64,
    key: SharedString,
    options: NotificationOptions,
    placement: NotificationPlacement,
    created: Instant,
    closing: Option<Instant>,
    exit_duration: Duration,
    close_reason: Option<NotificationCloseReason>,
    budget: Budget,
    hovered: bool,
    completion: Rc<RefCell<Completion>>,
}

#[derive(Clone)]
pub(super) struct RenderNotice {
    pub id: u64,
    pub key: SharedString,
    pub options: NotificationOptions,
    pub placement: NotificationPlacement,
    pub created: Instant,
    pub closing: Option<Instant>,
    pub exit_duration: Duration,
    pub budget: Budget,
    pub handle: NotificationHandle,
}
pub(super) struct Dismissal {
    event: NotificationCloseEvent,
    callback: Option<CloseHandler>,
    completion: Rc<RefCell<Completion>>,
    boundary: Option<FocusBoundary>,
}
pub(super) fn dispatch(pending: Vec<Dismissal>, window: &mut Window, cx: &mut App) {
    for dismissal in pending {
        Completion::settle(&dismissal.completion, dismissal.event.clone());
        if let Some(boundary) = dismissal.boundary {
            boundary.dismiss(window, cx);
        }
        if let Some(callback) = dismissal.callback {
            callback(&dismissal.event, window, cx);
        }
    }
}
fn settle(pending: Vec<Dismissal>) {
    for dismissal in pending {
        Completion::settle(&dismissal.completion, dismissal.event);
    }
}
fn defer_dispatch(pending: Vec<Dismissal>, window: &Window, cx: &mut App) {
    if pending.is_empty() {
        return;
    }
    let window = window.window_handle();
    cx.defer(move |cx| {
        let mut pending = Some(pending);
        if window
            .update(cx, |_, window, cx| {
                dispatch(pending.take().unwrap_or_default(), window, cx)
            })
            .is_err()
        {
            settle(pending.unwrap_or_default());
        }
    });
}
pub(super) fn unmount(state: &gpui::WeakEntity<NotificationState>, cx: &mut App) {
    settle(
        state
            .update(cx, |state, _| state.detach())
            .unwrap_or_default(),
    );
}

/// Retain once per mounted host. A controller is bound to the window in which it was created.
pub struct NotificationState {
    pub(super) scope: ConfigScope,
    notices: Vec<Notice>,
    pub(super) visual: Rc<RefCell<super::view::VisualState>>,
    pub(super) layouts: Vec<NotificationLayout>,
    pub(super) hovered: [bool; 6],
    pub(super) focused: [bool; 6],
    window_id: WindowId,
    overrides: NotificationConfigPatch,
    sequence: u64,
    timer: Option<Task<()>>,
    timer_at: Option<Instant>,
    generation: u64,
    pub(super) frame_pending: bool,
    closed: bool,
    _config: Subscription,
    _window: Subscription,
}
impl NotificationState {
    pub fn new(scope: ConfigScope, window: &mut Window, cx: &mut Context<Self>) -> Self {
        let config = scope.observe_in(window, cx, |state, window, cx| {
            state.sync_config(window, cx);
            cx.notify();
        });
        let window_id = window.window_handle().window_id();
        let weak = cx.weak_entity();
        let subscription = cx.on_window_closed(move |cx, id| {
            if id == window_id {
                settle(
                    weak.update(cx, |state, _| state.detach())
                        .unwrap_or_default(),
                );
            }
        });
        Self {
            scope,
            notices: Vec::new(),
            visual: Default::default(),
            layouts: Vec::new(),
            hovered: [false; 6],
            focused: [false; 6],
            window_id,
            overrides: Default::default(),
            sequence: 0,
            timer: None,
            timer_at: None,
            generation: 0,
            frame_pending: false,
            closed: false,
            _config: config,
            _window: subscription,
        }
    }
    pub(super) fn belongs_to(&self, window: &Window) -> bool {
        !self.closed && self.window_matches(window)
    }
    pub(super) fn window_matches(&self, window: &Window) -> bool {
        self.window_id == window.window_handle().window_id()
    }
    pub(super) fn mount(&mut self, window: &Window) {
        if self.window_matches(window) {
            self.closed = false;
        }
    }
    pub fn effective_config(&self, cx: &App) -> NotificationConfig {
        self.overrides
            .resolve(&self.scope.snapshot(cx).notification)
    }
    pub fn set_config(
        &mut self,
        config: NotificationConfigPatch,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if !self.belongs_to(window) || self.overrides == config {
            return;
        }
        self.overrides = config;
        self.sync_config(window, cx);
        cx.notify();
    }
    pub fn active_count(&self) -> usize {
        self.notices.iter().filter(|n| n.closing.is_none()).count()
    }
    pub(super) fn snapshot(&self, cx: &Context<Self>) -> Vec<RenderNotice> {
        self.notices
            .iter()
            .map(|n| RenderNotice {
                id: n.id,
                key: n.key.clone(),
                options: n.options.clone(),
                placement: n.placement,
                created: n.created,
                closing: n.closing,
                exit_duration: n.exit_duration,
                budget: n.budget,
                handle: NotificationHandle {
                    state: cx.weak_entity(),
                    id: n.id,
                    key: n.key.clone(),
                    completion: n.completion.clone(),
                },
            })
            .collect()
    }
    pub(super) fn open(
        &mut self,
        options: NotificationOptions,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> (NotificationHandle, Vec<Dismissal>) {
        let now = cx.background_executor().now();
        self.finish_exits(now, cx);
        let config = self.effective_config(cx);
        let duration = options.duration.unwrap_or(config.duration);
        let placement = options.placement.unwrap_or(config.placement);
        let existing = options.key.as_ref().and_then(|key| {
            self.notices
                .iter()
                .position(|n| &n.key == key && n.closing.is_none())
        });
        let mut migrated = false;
        let index = if let Some(index) = existing {
            let notice = &mut self.notices[index];
            if notice.budget.duration != duration {
                notice.budget = Budget::new(duration, now);
            }
            migrated = notice.placement != placement;
            if migrated {
                notice.hovered = false;
            }
            notice.options = options;
            notice.placement = placement;
            self.visual.borrow_mut().invalidate(notice.id);
            index
        } else {
            let (id, key) = loop {
                self.sequence = self
                    .sequence
                    .checked_add(1)
                    .expect("Notification lifetime space exhausted");
                let key = options
                    .key
                    .clone()
                    .unwrap_or_else(|| format!("notification-{}", self.sequence).into());
                if options.key.is_some() || !self.notices.iter().any(|n| n.key == key) {
                    break (self.sequence, key);
                }
            };
            self.notices.insert(
                0,
                Notice {
                    id,
                    key,
                    options,
                    placement,
                    created: now,
                    closing: None,
                    exit_duration: Duration::ZERO,
                    close_reason: None,
                    budget: Budget::new(duration, now),
                    hovered: false,
                    completion: Default::default(),
                },
            );
            0
        };
        let notice = &self.notices[index];
        let handle = NotificationHandle {
            state: cx.weak_entity(),
            id: notice.id,
            key: notice.key.clone(),
            completion: notice.completion.clone(),
        };
        if migrated {
            self.sync_focus(window, cx);
        }
        self.sync_paused(&config, now);
        let pending = self.enforce_limit(&config, now, cx);
        self.reschedule(window, cx);
        cx.notify();
        (handle, pending)
    }
    fn begin_close(
        &mut self,
        id: u64,
        reason: NotificationCloseReason,
        now: Instant,
        cx: &App,
    ) -> Option<Dismissal> {
        let snapshot = self.scope.snapshot(cx);
        let tokens = snapshot.theme.component::<NotificationTheme>();
        let base = if snapshot.motion && tokens.globals().motion && !cx.reduce_motion() {
            tokens.globals().motion_duration_mid
        } else {
            Duration::ZERO
        };
        let mut visual = self.visual.borrow_mut();
        let surface = visual.cards.get_mut(&id);
        let presented = surface.as_ref().is_some_and(|s| s.presented);
        let boundary = surface.map(|s| {
            s.boundary.0.closed.set(true);
            s.guard.cancel();
            s.boundary.clone()
        });
        let notice = self
            .notices
            .iter_mut()
            .find(|n| n.id == id && n.closing.is_none())?;
        let entry = if base.is_zero() {
            1.
        } else {
            evaluate_cubic_bezier(
                tokens.globals().motion_ease_in_out,
                now.saturating_duration_since(notice.created).as_secs_f64() / base.as_secs_f64(),
            )
            .clamp(0., 1.)
        };
        notice.closing = Some(now);
        notice.close_reason = Some(reason);
        notice.exit_duration = if presented {
            base.mul_f64(entry)
        } else {
            Duration::ZERO
        };
        notice.budget.pause(true, now);
        notice.completion.borrow_mut().dismissed = true;
        Some(Dismissal {
            event: NotificationCloseEvent {
                key: notice.key.clone(),
                reason,
            },
            callback: notice.options.on_close.take(),
            completion: notice.completion.clone(),
            boundary,
        })
    }
    pub(super) fn close(
        &mut self,
        id: u64,
        reason: NotificationCloseReason,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Vec<Dismissal> {
        let now = cx.background_executor().now();
        let pending = self.begin_close(id, reason, now, cx).into_iter().collect();
        self.finish_exits(now, cx);
        self.reschedule(window, cx);
        cx.notify();
        pending
    }
    pub(super) fn destroy(
        &mut self,
        key: Option<&SharedString>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Vec<Dismissal> {
        let ids: Vec<_> = self
            .notices
            .iter()
            .filter(|n| n.closing.is_none() && key.is_none_or(|key| &n.key == key))
            .map(|n| n.id)
            .collect();
        let now = cx.background_executor().now();
        let pending = ids
            .into_iter()
            .filter_map(|id| self.begin_close(id, NotificationCloseReason::Destroyed, now, cx))
            .collect();
        self.finish_exits(now, cx);
        self.reschedule(window, cx);
        cx.notify();
        pending
    }
    fn enforce_limit(
        &mut self,
        config: &NotificationConfig,
        now: Instant,
        cx: &App,
    ) -> Vec<Dismissal> {
        let Some(limit) = config.max_count else {
            return Vec::new();
        };
        let ids: Vec<_> = self
            .notices
            .iter()
            .filter(|n| n.closing.is_none())
            .skip(limit)
            .map(|n| n.id)
            .collect();
        ids.into_iter()
            .filter_map(|id| self.begin_close(id, NotificationCloseReason::Evicted, now, cx))
            .collect()
    }
    fn sync_paused(&mut self, config: &NotificationConfig, now: Instant) {
        for n in self.notices.iter_mut().filter(|n| n.closing.is_none()) {
            let group = n.placement.index();
            let paused = self.focused[group]
                || (n.options.pause_on_hover.unwrap_or(config.pause_on_hover)
                    && (n.hovered || (config.stack.threshold().is_some() && self.hovered[group])));
            n.budget.pause(paused, now);
        }
    }
    pub(super) fn sync_focus(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let mut focused = [false; 6];
        {
            let visual = self.visual.borrow();
            for notice in self.notices.iter().filter(|n| n.closing.is_none()) {
                if let Some(card) = visual.cards.get(&notice.id) {
                    focused[notice.placement.index()] |=
                        card.boundary.0.root.contains_focused(window, cx);
                }
            }
        }
        if self.focused != focused {
            self.focused = focused;
            self.sync_paused(&self.effective_config(cx), cx.background_executor().now());
            self.reschedule(window, cx);
        }
        cx.notify();
    }
    fn sync_config(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let now = cx.background_executor().now();
        let config = self.effective_config(cx);
        self.visual.borrow_mut().invalidate_all();
        self.sync_paused(&config, now);
        let pending = self.enforce_limit(&config, now, cx);
        self.finish_exits(now, cx);
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
        if let Some(n) = self
            .notices
            .iter_mut()
            .find(|n| n.id == id && n.closing.is_none())
        {
            if n.hovered == hovered {
                return;
            }
            n.hovered = hovered;
            self.sync_paused(&self.effective_config(cx), cx.background_executor().now());
            self.reschedule(window, cx);
            cx.notify();
        }
    }
    pub(super) fn group_interaction(
        &mut self,
        placement: NotificationPlacement,
        hovered: bool,
        focused: bool,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let index = placement.index();
        if self.hovered[index] == hovered && self.focused[index] == focused {
            return;
        }
        self.hovered[index] = hovered;
        self.focused[index] = focused;
        self.sync_paused(&self.effective_config(cx), cx.background_executor().now());
        self.reschedule(window, cx);
        cx.notify();
    }
    fn finish_exits(&mut self, now: Instant, cx: &App) {
        let snapshot = self.scope.snapshot(cx);
        let motion = snapshot.motion
            && snapshot
                .theme
                .component::<NotificationTheme>()
                .globals()
                .motion
            && !cx.reduce_motion();
        self.notices.retain(|n| {
            n.closing.is_none_or(|start| {
                motion && now.saturating_duration_since(start) < n.exit_duration
            })
        });
        let retained: std::collections::HashSet<_> = self.notices.iter().map(|n| n.id).collect();
        self.visual
            .borrow_mut()
            .cards
            .retain(|id, _| retained.contains(id));
        if self.notices.is_empty() {
            self.layouts.clear();
            self.hovered = [false; 6];
            self.focused = [false; 6];
        }
    }
    fn reschedule(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let next = self
            .notices
            .iter()
            .filter_map(|n| {
                n.closing.map_or(n.budget.deadline, |start| {
                    start.checked_add(n.exit_duration)
                })
            })
            .min();
        if next == self.timer_at {
            return;
        }
        self.generation = self.generation.wrapping_add(1);
        self.timer = None;
        self.timer_at = next;
        let Some(deadline) = next else {
            return;
        };
        let generation = self.generation;
        let timer = cx
            .background_executor()
            .timer(deadline.saturating_duration_since(cx.background_executor().now()));
        self.timer = Some(cx.spawn_in(window, async move |weak, cx| {
            timer.await;
            let _ = cx.update(|window, cx| {
                let pending = weak
                    .update(cx, |state, cx| {
                        if generation != state.generation {
                            return Vec::new();
                        }
                        state.timer = None;
                        state.timer_at = None;
                        let now = cx.background_executor().now();
                        let ids: Vec<_> = state
                            .notices
                            .iter()
                            .filter(|n| {
                                n.closing.is_none() && n.budget.deadline.is_some_and(|d| d <= now)
                            })
                            .map(|n| n.id)
                            .collect();
                        let pending = ids
                            .into_iter()
                            .filter_map(|id| {
                                state.begin_close(id, NotificationCloseReason::Timeout, now, cx)
                            })
                            .collect();
                        state.finish_exits(now, cx);
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
        if self.frame_pending || self.closed || self.notices.is_empty() || !window.is_visible() {
            return;
        }
        self.frame_pending = true;
        let weak = cx.weak_entity();
        window.on_next_frame(move |_, cx| {
            let _ = weak.update(cx, |state, cx| {
                state.frame_pending = false;
                if !state.closed && !state.notices.is_empty() {
                    cx.notify();
                }
            });
        });
    }
    fn detach(&mut self) -> Vec<Dismissal> {
        self.closed = true;
        self.generation = self.generation.wrapping_add(1);
        self.timer = None;
        self.timer_at = None;
        self.layouts.clear();
        self.hovered = [false; 6];
        self.focused = [false; 6];
        self.visual.borrow_mut().clear();
        self.notices
            .drain(..)
            .map(|n| Dismissal {
                event: NotificationCloseEvent {
                    key: n.key,
                    reason: n
                        .close_reason
                        .unwrap_or(NotificationCloseReason::OwnerDropped),
                },
                callback: None,
                completion: n.completion,
                boundary: None,
            })
            .collect()
    }
}
impl Render for NotificationState {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        super::view::render(self, window, cx)
    }
}
impl Drop for NotificationState {
    fn drop(&mut self) {
        settle(self.detach());
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn countdown_preserves_elapsed_time_through_repeated_pause_updates() {
        let now = Instant::now();
        let mut budget = Budget::new(Duration::from_secs(4), now);
        budget.pause(true, now + Duration::from_secs(1));
        budget.pause(true, now + Duration::from_secs(10));
        assert_eq!(budget.fraction(now + Duration::from_secs(10)), 0.75);
        budget.pause(false, now + Duration::from_secs(20));
        budget.pause(false, now + Duration::from_secs(21));
        assert_eq!(budget.fraction(now + Duration::from_secs(21)), 0.5);
        assert_eq!(budget.deadline, Some(now + Duration::from_secs(23)));
    }
    #[test]
    fn persistent_budget_never_schedules_or_displays_progress() {
        let now = Instant::now();
        let mut budget = Budget::new(Duration::ZERO, now);
        budget.pause(true, now);
        budget.pause(false, now);
        assert!(!budget.running());
        assert!(!budget.timed());
        assert_eq!(budget.fraction(now), 0.);
    }
}
