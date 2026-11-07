// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::{cell::Cell, rc::Rc, time::Duration};

use gpui::{App, Bounds, Context, FocusHandle, Pixels, Subscription, Window};

use super::{AfterCloseHandler, Alert, AlertStyle};
use crate::{
    ConfigScope,
    behavior::{ActivationGuard, Availability, MotionPolicy, ScalarTransition, focus_handle},
    theme::CubicBezier,
};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum Phase {
    Open,
    Closing,
    Closed,
}

/// Actual, fully styled open geometry; never overwritten by collapsing bounds.
#[derive(Clone, Copy, Debug)]
pub(super) struct OpenGeometry {
    pub bounds: Bounds<Pixels>,
    pub padding_top: Pixels,
    pub padding_bottom: Pixels,
    pub margin_bottom: Pixels,
    pub opacity: f32,
}

pub(super) struct CloseInput {
    pub focus: FocusHandle,
    pub guard: ActivationGuard,
    _blur: Subscription,
}

impl CloseInput {
    fn new(focus: Option<&FocusHandle>, window: &mut Window, cx: &mut App) -> Self {
        let focus = focus.cloned().unwrap_or_else(|| focus_handle(cx));
        let guard = ActivationGuard::default();
        let blur = guard.observe_blur(&focus, window, cx);
        Self {
            focus,
            guard,
            _blur: blur,
        }
    }
    fn set_focus(&mut self, focus: Option<&FocusHandle>, window: &mut Window, cx: &mut App) {
        if let Some(focus) = focus
            && self.focus != *focus
        {
            self.guard.cancel();
            self._blur = self.guard.observe_blur(focus, window, cx);
            self.focus = focus.clone();
        }
    }
}

pub(super) struct AlertState {
    pub scope: ConfigScope,
    pub phase: Phase,
    pub input: Option<CloseInput>,
    pub open_geometry: Rc<Cell<Option<OpenGeometry>>>,
    pub close_hovered: bool,
    pub hover: ScalarTransition,
    pub transition: Option<ScalarTransition>,
    pub collapse: Option<OpenGeometry>,
    pub metrics: Option<AlertStyle>,
    pub after_close: Option<AfterCloseHandler>,
    local_closable: Option<bool>,
    custom_close: bool,
    close_disabled: bool,
    frame_pending: bool,
    epoch: std::time::Instant,
    _config: Option<Subscription>,
}

impl AlertState {
    pub fn new(
        alert: &Alert,
        scope: ConfigScope,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        Self {
            _config: Some(Self::observe_scope(&scope, window, cx)),
            scope,
            phase: Phase::Open,
            input: None,
            open_geometry: Rc::default(),
            close_hovered: false,
            hover: ScalarTransition::new(0., CubicBezier::new(0.25, 0.1, 0.25, 1.)),
            transition: None,
            collapse: None,
            metrics: None,
            after_close: None,
            local_closable: alert.closable,
            custom_close: alert.close_content.is_some() || alert.close_icon.is_some(),
            close_disabled: alert.close_disabled,
            frame_pending: false,
            epoch: cx.background_executor().now(),
        }
    }

    fn observe_scope(
        scope: &ConfigScope,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Subscription {
        scope.observe_in(window, cx, |state, window, cx| {
            state.sync_availability(window, cx);
            cx.notify();
        })
    }

    pub fn closable(&self, cx: &App) -> bool {
        self.local_closable
            .unwrap_or_else(|| self.custom_close || self.scope.snapshot(cx).alert.closable)
    }

    pub fn update(
        &mut self,
        alert: &Alert,
        scope: ConfigScope,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.phase == Phase::Closed {
            return;
        }
        if self.scope != scope {
            if let Some(input) = &self.input {
                input.guard.cancel();
            }
            self._config = Some(Self::observe_scope(&scope, window, cx));
            self.scope = scope;
        }
        self.local_closable = alert.closable;
        self.custom_close = alert.close_content.is_some() || alert.close_icon.is_some();
        self.close_disabled = alert.close_disabled;
        self.after_close = alert.after_close.clone();
        if self.closable(cx) && self.phase != Phase::Closed {
            self.input
                .get_or_insert_with(|| CloseInput::new(alert.close_focus.as_ref(), window, cx))
                .set_focus(alert.close_focus.as_ref(), window, cx);
        }
        self.sync_availability(window, cx);
        if !self.closable(cx) && self.phase == Phase::Open {
            self.input = None;
            self.close_hovered = false;
        }
    }

    fn sync_availability(&self, window: &mut Window, cx: &mut App) {
        if let Some(input) = &self.input {
            let availability = Availability {
                disabled: self.close_disabled || !self.closable(cx) || self.phase != Phase::Open,
                busy: false,
            };
            if self.phase == Phase::Open {
                input.guard.update(availability, &input.focus, window, cx);
            } else {
                // Leave keeps source-order focus until the disappearing node is removed.
                input.guard.set_availability(availability);
            }
        }
    }

    /// Change phase before invoking application code. Every activation shares this gate.
    pub fn begin_close(&mut self, window: &mut Window, cx: &mut Context<Self>) -> bool {
        if self.phase != Phase::Open || !self.closable(cx) || self.close_disabled {
            return false;
        }
        self.phase = Phase::Closing;
        self.collapse = self.open_geometry.get();
        let config = self.scope.snapshot(cx);
        let metrics = self
            .metrics
            .as_ref()
            .expect("painted Alert has resolved metrics");
        let mut transition = ScalarTransition::new(1., metrics.motion_easing);
        transition.retarget(
            0.,
            self.now(cx),
            metrics.motion_duration,
            MotionPolicy::new(
                config.motion && config.theme.tokens.motion,
                cx.reduce_motion(),
            ),
        );
        self.transition = Some(transition);
        self.sync_availability(window, cx);
        cx.notify();
        true
    }

    pub fn now(&self, cx: &App) -> Duration {
        cx.background_executor().now().duration_since(self.epoch)
    }

    pub fn request_frame(&mut self, window: &Window, cx: &Context<Self>) {
        if self.frame_pending || self.phase == Phase::Closed {
            return;
        }
        self.frame_pending = true;
        let weak = cx.weak_entity();
        window.on_next_frame(move |_, cx| {
            let _ = weak.update(cx, |state, cx| {
                state.frame_pending = false;
                if state.phase != Phase::Closed {
                    cx.notify();
                }
            });
        });
    }

    pub fn sample_close(
        &mut self,
        policy: MotionPolicy,
        window: &Window,
        cx: &mut Context<Self>,
    ) -> f32 {
        let now = self.now(cx);
        let Some(transition) = &mut self.transition else {
            return 1.;
        };
        if !policy.allows_motion() {
            transition.retarget(0., now, Duration::ZERO, policy);
        }
        let sample = transition.sample(now);
        if !sample.finished {
            self.request_frame(window, cx);
        }
        sample.value as f32
    }

    pub fn complete(&mut self) -> (Option<AfterCloseHandler>, Option<FocusHandle>) {
        if self.phase != Phase::Closing {
            return (None, None);
        }
        self.phase = Phase::Closed;
        self.transition = None;
        self.collapse = None;
        self.open_geometry.set(None);
        self.metrics = None;
        self._config = None;
        (
            self.after_close.take(),
            self.input.take().map(|input| input.focus),
        )
    }
}
