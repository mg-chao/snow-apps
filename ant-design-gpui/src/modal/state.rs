// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
use super::{ModalAction, ModalCancelReason, ModalHandle, ModalOptions, ModalOutcome, host};
use crate::{
    ConfigScope, ScrollAreaState,
    behavior::{MotionPolicy, ScalarTransition, focus_handle},
    theme::CubicBezier,
};
use gpui::{
    AnyView, App, AppContext, Bounds, Context, Entity, EventEmitter, FocusHandle, Focusable,
    Pixels, SharedString, Subscription, Task, Window,
};
use std::{
    rc::Rc,
    time::{Duration, Instant},
};

type ViewportKeyHandler = Rc<dyn Fn(&gpui::KeyDownEvent, &mut Window, &mut App)>;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum ModalPhase {
    #[default]
    Closed,
    Opening,
    Open,
    Closing,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ModalEvent {
    OkRequested,
    CancelRequested(ModalCancelReason),
    AfterOpenChange(bool),
    AfterClose,
    ActionFailed(SharedString),
}

/// A single dialog's lifetime. Setters are silent; interactions emit typed requests.
pub struct ModalState {
    pub(super) viewport_surface: bool,
    pub(super) viewport_keys: Option<ViewportKeyHandler>,
    viewport_motion: Option<(Duration, bool)>,
    pub(super) options: ModalOptions,
    pub(super) scope: ConfigScope,
    pub(super) open: bool,
    pub(super) phase: ModalPhase,
    pub(super) confirm_loading: bool,
    pub(super) pending_ok: bool,
    pub(super) pending_cancel: bool,
    dispatching: bool,
    pub(super) generation: u64,
    pub(super) focus: FocusHandle,
    pub(super) presentation: Option<ModalPresentation>,
    pub(super) presentation_generation: u64,
    pub(super) return_focus: Option<FocusHandle>,
    pub(super) needs_focus: bool,
    pub(super) mask_down: bool,
    pub(super) bounds: Bounds<Pixels>,
    pub(super) body_view: Option<AnyView>,
    pub(super) title_view: Option<AnyView>,
    pub(super) footer_view: Option<AnyView>,
    pub(super) managed: bool,
    pub(super) completion: Option<Rc<super::manager::Completion>>,
    pub(super) mounted: bool,
    pub(super) mount_generation: u64,
    epoch: Instant,
    opacity: ScalarTransition,
    frame_pending: bool,
    scope_subscription: Subscription,
    settle_task: Option<Task<()>>,
}
pub(super) struct ModalPresentation {
    pub ok_focus: FocusHandle,
    pub cancel_focus: FocusHandle,
    pub close_focus: FocusHandle,
    pub start_focus: FocusHandle,
    pub end_focus: FocusHandle,
    pub scroll: Entity<ScrollAreaState>,
    close_scope: Option<(crate::theme::Color, crate::theme::Color, ConfigScope)>,
}
impl EventEmitter<ModalEvent> for ModalState {}
impl Focusable for ModalState {
    fn focus_handle(&self, _: &App) -> FocusHandle {
        self.focus.clone()
    }
}

impl ModalState {
    pub fn new(_window: &mut Window, cx: &mut Context<Self>) -> Self {
        let scope = ConfigScope::application(cx);
        let subscription = scope.observe(cx);
        Self {
            viewport_surface: false,
            viewport_keys: None,
            viewport_motion: None,
            options: ModalOptions::default(),
            scope,
            open: false,
            phase: ModalPhase::Closed,
            confirm_loading: false,
            pending_ok: false,
            pending_cancel: false,
            dispatching: false,
            generation: 0,
            focus: cx.focus_handle(),
            presentation: None,
            presentation_generation: 0,
            return_focus: None,
            needs_focus: false,
            mask_down: false,
            bounds: Bounds::default(),
            body_view: None,
            title_view: None,
            footer_view: None,
            managed: false,
            completion: None,
            mounted: false,
            mount_generation: 0,
            epoch: cx.background_executor().now(),
            opacity: ScalarTransition::new(0., CubicBezier::new(0.08, 0.82, 0.17, 1.)),
            frame_pending: false,
            scope_subscription: subscription,
            settle_task: None,
        }
    }
    pub fn is_open(&self) -> bool {
        self.open
    }
    /// Specialized native surfaces reuse dialog ownership and focus without
    /// inheriting the themed card's geometry or semantic-slot styles.
    pub(crate) fn use_viewport_surface(
        &mut self,
        on_key: impl Fn(&gpui::KeyDownEvent, &mut Window, &mut App) + 'static,
    ) {
        self.viewport_surface = true;
        self.opacity = ScalarTransition::new(0., CubicBezier::new(0.25, 0.1, 0.25, 1.));
        self.viewport_keys = Some(Rc::new(on_key));
    }
    pub(crate) fn set_viewport_motion(&mut self, duration: Duration, enabled: bool) {
        self.viewport_motion = Some((duration, enabled));
    }
    /// Transfer only focus intent when a specialized surface discards its shell.
    /// The outgoing marker must not restore focus as part of its later unmount.
    pub(crate) fn take_viewport_focus(
        &mut self,
        window: &Window,
        cx: &App,
    ) -> (bool, Option<gpui::WeakFocusHandle>) {
        let owned = self.needs_focus || self.focus.contains_focused(window, cx);
        let return_to = self.take_viewport_return_focus();
        (owned, return_to)
    }
    pub(crate) fn take_viewport_return_focus(&mut self) -> Option<gpui::WeakFocusHandle> {
        self.return_focus.take().map(|focus| focus.downgrade())
    }
    /// Apply after reopening so a replacement shell neither steals outside focus
    /// nor mistakes a disposed operation control for the original opener.
    pub(crate) fn restore_viewport_focus(
        &mut self,
        owned: bool,
        return_to: Option<gpui::WeakFocusHandle>,
    ) {
        self.needs_focus = owned;
        self.return_focus = return_to.and_then(|focus| focus.upgrade());
    }
    fn motion(&self, cx: &App) -> (Duration, MotionPolicy) {
        let config = self.scope.snapshot(cx);
        self.viewport_motion.map_or_else(
            || {
                (
                    config.theme.tokens.motion_duration_slow,
                    config.motion_policy(cx),
                )
            },
            |(duration, enabled)| {
                (
                    duration,
                    MotionPolicy::new(config.motion && enabled, cx.reduce_motion()),
                )
            },
        )
    }
    pub fn phase(&self) -> ModalPhase {
        self.phase
    }
    pub fn is_pending(&self) -> bool {
        self.pending_ok || self.pending_cancel
    }
    /// The effective scope, for composing custom native content.
    pub fn scope(&self) -> &ConfigScope {
        &self.scope
    }
    pub fn options(&self) -> &ModalOptions {
        &self.options
    }
    pub fn bounds(&self) -> Bounds<Pixels> {
        self.bounds
    }
    pub fn set_scope(&mut self, scope: ConfigScope, cx: &mut Context<Self>) {
        if scope == self.scope {
            return;
        }
        self.scope_subscription = scope.observe(cx);
        self.scope = scope;
        if let Some(presentation) = &mut self.presentation {
            presentation.close_scope = None;
        }
        cx.notify();
    }
    pub fn set_options(&mut self, options: ModalOptions, cx: &mut Context<Self>) {
        self.replace_options(options);
        cx.notify();
    }
    pub(super) fn replace_options(&mut self, options: ModalOptions) {
        let same = |a: Option<&super::ModalContent>, b: Option<&super::ModalContent>| match (a, b) {
            (Some(a), Some(b)) => a.same(b),
            (None, None) => true,
            _ => false,
        };
        if !same(self.options.content.as_ref(), options.content.as_ref()) {
            self.body_view = None;
        }
        if !same(self.options.title.as_ref(), options.title.as_ref()) {
            self.title_view = None;
        }
        let old = match &self.options.footer {
            super::ModalFooter::Content(c) => Some(c),
            _ => None,
        };
        let new = match &options.footer {
            super::ModalFooter::Content(c) => Some(c),
            _ => None,
        };
        if !same(old, new) {
            self.footer_view = None;
        }
        self.options = options;
    }
    pub(super) fn ensure_presentation(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if self.presentation.is_none() {
            self.presentation_generation = self.presentation_generation.wrapping_add(1);
            self.presentation = Some(ModalPresentation {
                ok_focus: focus_handle(cx),
                cancel_focus: focus_handle(cx),
                close_focus: focus_handle(cx),
                start_focus: focus_handle(cx),
                end_focus: focus_handle(cx),
                scroll: cx.new(|cx| ScrollAreaState::new(window, cx)),
                close_scope: None,
            });
        }
    }
    pub(super) fn presentation(&self) -> &ModalPresentation {
        self.presentation
            .as_ref()
            .expect("presented dialog resources")
    }
    pub(super) fn suspend(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if self.presentation.is_some() {
            let resume = self.open && (self.needs_focus || self.focus.contains_focused(window, cx));
            self.release_presentation();
            self.needs_focus = resume;
            cx.notify();
        }
    }
    pub(super) fn release_presentation(&mut self) {
        self.release_content();
        self.presentation = None;
        self.bounds = Bounds::default();
        self.mask_down = false;
        self.frame_pending = false;
    }
    fn release_content(&mut self) {
        self.body_view = None;
        self.title_view = None;
        self.footer_view = None;
    }
    pub fn set_confirm_loading(&mut self, loading: bool, cx: &mut Context<Self>) {
        if self.confirm_loading != loading {
            self.confirm_loading = loading;
            cx.notify();
        }
    }
    pub fn set_open(&mut self, open: bool, window: &mut Window, cx: &mut Context<Self>) {
        if self.open == open {
            return;
        }
        self.open = open;
        self.generation = self.generation.wrapping_add(1);
        self.pending_ok = false;
        self.pending_cancel = false;
        self.dispatching = false;
        self.mask_down = false;
        self.frame_pending = false;
        let (duration, policy) = self.motion(cx);
        self.opacity.retarget(
            if open { 1. } else { 0. },
            self.elapsed(cx),
            duration,
            policy,
        );
        self.phase = if open {
            ModalPhase::Opening
        } else {
            ModalPhase::Closing
        };
        self.settle_task = None;
        let generation = self.generation;
        let delay = if policy.allows_motion() {
            duration
        } else {
            Duration::ZERO
        };
        let timer = cx.background_executor().timer(delay);
        self.settle_task = Some(cx.spawn_in(window, async move |weak, cx| {
            timer.await;
            let _ = cx.update(|window, cx| {
                let _ = weak.update(cx, |state, cx| {
                    if state.generation == generation {
                        state.settle_task = None;
                        state.sample(window, cx);
                        cx.notify();
                    }
                });
            });
        }));
        if open {
            if self.return_focus.is_none() {
                self.return_focus = window.focused(cx);
            }
            self.needs_focus = true;
            window.release_pointer();
            host::activate(&cx.entity(), self.options.z_index, window, cx);
        } else {
            self.needs_focus = false;
            let id = cx.entity_id();
            let weak = cx.weak_entity();
            let generation = self.generation;
            window.defer(cx, move |w, cx| {
                if weak
                    .upgrade()
                    .is_none_or(|s| s.read(cx).generation != generation)
                {
                    return;
                }
                crate::popup::host::dismiss_modal(Some(id), w, cx);
                host::close_descendants(id, w, cx);
            });
        }
        cx.notify();
    }
    pub fn handle(entity: &Entity<Self>, cx: &App) -> ModalHandle {
        ModalHandle::from_entity(entity, cx)
    }
    pub(super) fn elapsed(&self, cx: &App) -> Duration {
        cx.background_executor()
            .now()
            .saturating_duration_since(self.epoch)
    }
    pub(super) fn close_scope(&mut self, cx: &mut Context<Self>) -> ConfigScope {
        let config = self.scope.snapshot(cx);
        let colors = (
            config.theme.tokens.color_icon,
            config.theme.tokens.color_icon_hover,
        );
        if let Some((a, b, scope)) = &self.presentation().close_scope
            && (*a, *b) == colors
        {
            return scope.clone();
        }
        let mut theme = crate::theme::ThemeConfig::default();
        theme
            .components
            .set::<crate::button::ButtonTheme>(crate::button::ButtonTokenPatch {
                text_text_color: Some(colors.0),
                text_text_hover_color: Some(colors.1),
                text_text_active_color: Some(colors.1),
                ..Default::default()
            });
        let scope = self
            .scope
            .child(
                crate::ConfigPatch {
                    theme: Some(theme),
                    ..Default::default()
                },
                cx,
            )
            .expect("valid close button theme");
        self.presentation
            .as_mut()
            .expect("presented dialog")
            .close_scope = Some((colors.0, colors.1, scope.clone()));
        scope
    }
    pub(super) fn mask(&self, cx: &App) -> bool {
        self.options
            .mask
            .or(self.scope.snapshot(cx).modal.mask)
            .unwrap_or(true)
    }
    pub(super) fn trap(&self, cx: &App) -> bool {
        self.options
            .focus_trap
            .or(self.scope.snapshot(cx).modal.focus_trap)
            .unwrap_or(self.mask(cx))
    }
    pub(super) fn sample(&mut self, window: &mut Window, cx: &mut Context<Self>) -> f32 {
        let (_, policy) = self.motion(cx);
        if !policy.allows_motion() || !window.is_visible() {
            self.opacity.retarget(
                if self.open { 1. } else { 0. },
                self.elapsed(cx),
                Duration::ZERO,
                MotionPolicy::new(false, false),
            );
        }
        let sample = self.opacity.sample(self.elapsed(cx));
        if sample.finished {
            let was_transitioning = matches!(self.phase, ModalPhase::Opening | ModalPhase::Closing);
            self.phase = if self.open {
                ModalPhase::Open
            } else {
                ModalPhase::Closed
            };
            if was_transitioning {
                let open = self.open;
                let weak = cx.weak_entity();
                let generation = self.generation;
                window.defer(cx, move |w, cx| {
                    let Some(entity) = weak.upgrade() else {
                        return;
                    };
                    if entity.read(cx).generation != generation {
                        return;
                    }
                    emit(&entity, ModalEvent::AfterOpenChange(open), w, cx);
                    if entity.read(cx).generation != generation {
                        return;
                    }
                    if !open {
                        emit(&entity, ModalEvent::AfterClose, w, cx);
                        if entity.read(cx).generation != generation {
                            return;
                        }
                        host::retire(entity.entity_id(), w, cx);
                        entity.update(cx, |s, cx| {
                            let restore = s
                                .options
                                .focus_return
                                .or(s.scope.snapshot(cx).modal.focus_return)
                                .unwrap_or(true);
                            let target = s.return_focus.take();
                            if restore && let Some(target) = target {
                                w.focus(&target, cx);
                            }
                            s.release_presentation();
                        });
                        host::restore_focus(w, cx);
                    }
                });
            }
        }
        let loading_motion = self.open
            && self.options.loading
            && window.is_visible()
            && policy.allows_motion()
            && host::is_top(cx.entity_id(), window, cx);
        if (!sample.finished || loading_motion) && !self.frame_pending {
            self.frame_pending = true;
            let generation = self.generation;
            let weak = cx.weak_entity();
            window.on_next_frame(move |_, cx| {
                let _ = weak.update(cx, |s, cx| {
                    if s.generation == generation {
                        s.frame_pending = false;
                        cx.notify();
                    }
                });
            });
            window.request_animation_frame();
        }
        sample.value as f32
    }
    pub(super) fn unmount(&mut self, cx: &mut Context<Self>) {
        self.open = false;
        self.phase = ModalPhase::Closed;
        self.generation = self.generation.wrapping_add(1);
        self.frame_pending = false;
        self.pending_ok = false;
        self.pending_cancel = false;
        self.dispatching = false;
        self.mounted = false;
        self.settle_task = None;
        self.release_presentation();
        if let Some(completion) = &self.completion {
            completion.finish(ModalOutcome::Destroyed);
        }
        cx.notify();
    }
}

pub(super) fn emit(
    entity: &Entity<ModalState>,
    event: ModalEvent,
    window: &mut Window,
    cx: &mut App,
) {
    let handler = entity.update(cx, |s, cx| {
        cx.emit(event.clone());
        s.options.on_event.clone()
    });
    if let Some(handler) = handler {
        handler(&event, window, cx);
    }
}

pub(super) fn activate(
    entity: &Entity<ModalState>,
    reason: Option<ModalCancelReason>,
    window: &mut Window,
    cx: &mut App,
) {
    let allowed = {
        let s = entity.read(cx);
        let config = s.scope.snapshot(cx);
        let ok = s.options.ok_button.resolve(&config.modal.ok_button);
        let cancel = s.options.cancel_button.resolve(&config.modal.cancel_button);
        s.open
            && !s.dispatching
            && !s.confirm_loading
            && if let Some(reason) = reason {
                !s.pending_cancel
                    && match reason {
                        ModalCancelReason::Button => {
                            !cancel.disabled.unwrap_or(false) && !cancel.loading.unwrap_or(false)
                        }
                        ModalCancelReason::Close => !s.options.close_disabled,
                        _ => true,
                    }
            } else {
                !s.pending_ok && !ok.disabled.unwrap_or(false) && !ok.loading.unwrap_or(false)
            }
    };
    if !allowed {
        return;
    }
    let (generation, callback, managed) = entity.update(cx, |s, _| {
        s.dispatching = true;
        (
            s.generation,
            if reason.is_some() {
                s.options.on_cancel.clone()
            } else {
                s.options.on_ok.clone()
            },
            s.managed,
        )
    });
    emit(
        entity,
        reason.map_or(ModalEvent::OkRequested, ModalEvent::CancelRequested),
        window,
        cx,
    );
    if entity.read(cx).generation != generation {
        return;
    }
    let handle = ModalHandle::from_entity(entity, cx);
    let action = callback.map_or(
        if managed {
            ModalAction::Close
        } else {
            ModalAction::KeepOpen
        },
        |f| f(&handle, window, cx),
    );
    entity.update(cx, |s, _| {
        if s.generation == generation {
            s.dispatching = false;
        }
    });
    let outcome = if reason.is_some() {
        ModalOutcome::Cancelled
    } else {
        ModalOutcome::Confirmed
    };
    match action {
        ModalAction::KeepOpen => {}
        ModalAction::Close => finish_action(entity, generation, outcome, window, cx),
        ModalAction::Pending(task) => {
            entity.update(cx, |s, cx| {
                if s.generation == generation {
                    if reason.is_some() {
                        s.pending_cancel = true;
                    } else {
                        s.pending_ok = true;
                    }
                    cx.notify();
                }
                // Detach even when the callback closed/unmounted the dialog. The task belongs
                // to the application; only its effect on this opening is generation-scoped.
                cx.spawn_in(window, async move |weak, cx| {
                    let result = task.await;
                    let _ = cx.update(|w, cx| {
                        let Some(entity) = weak.upgrade() else {
                            return;
                        };
                        if entity.read(cx).generation != generation {
                            return;
                        }
                        entity.update(cx, |s, cx| {
                            if reason.is_some() {
                                s.pending_cancel = false;
                            } else {
                                s.pending_ok = false;
                            }
                            cx.notify();
                        });
                        match result {
                            Ok(()) => finish_action(&entity, generation, outcome, w, cx),
                            Err(error) => emit(&entity, ModalEvent::ActionFailed(error), w, cx),
                        }
                    });
                })
                .detach();
            });
        }
    }
}

fn finish_action(
    entity: &Entity<ModalState>,
    generation: u64,
    outcome: ModalOutcome,
    window: &mut Window,
    cx: &mut App,
) {
    entity.update(cx, |s, cx| {
        if s.generation != generation || !s.open {
            return;
        }
        if let Some(completion) = &s.completion {
            completion.finish(outcome);
        }
        s.set_open(false, window, cx);
    });
}
