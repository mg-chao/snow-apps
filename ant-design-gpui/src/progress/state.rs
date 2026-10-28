// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::{Progress, ProgressKind, ProgressStatus, ProgressTokens, geometry, paint::ShapeCache};
use crate::{
    ConfigScope,
    behavior::{MotionPolicy, ScalarTransition},
    theme::CubicBezier,
};
use gpui::{Context, Subscription, Window};
use std::time::{Duration, Instant};

#[derive(Clone, Copy)]
pub(super) struct Sample {
    pub percent: f64,
    pub success: f64,
    pub elapsed: Duration,
    pub animate: bool,
    pub paint_motion: super::paint::PaintMotion,
    pub stroke: f32,
    pub track_opacity: f32,
    pub success_opacity: f32,
}

pub(super) struct ProgressState {
    scope: ConfigScope,
    _subscription: Subscription,
    epoch: Instant,
    active_started: Option<Duration>,
    last_request: Option<(ProgressKind, f64, f64)>,
    last_changed: Option<Duration>,
    percent: ScalarTransition,
    success: ScalarTransition,
    stroke: ScalarTransition,
    last_stroke: Option<f64>,
    track_opacity: ScalarTransition,
    success_opacity: ScalarTransition,
    frame_pending: bool,
    pub paint_animation: bool,
    pub rail: ShapeCache,
    pub track: ShapeCache,
    pub success_track: ShapeCache,
    pub steps: Vec<ShapeCache>,
    pub step_groups: Vec<ShapeCache>,
}
impl ProgressState {
    pub fn new(scope: ConfigScope, window: &mut Window, cx: &mut Context<Self>) -> Self {
        Self {
            _subscription: scope.observe_in(window, cx, |_, _, cx| cx.notify()),
            scope,
            epoch: cx.background_executor().now(),
            active_started: None,
            last_request: None,
            last_changed: None,
            percent: ScalarTransition::new(0., CubicBezier::new(0.25, 0.1, 0.25, 1.)),
            success: ScalarTransition::new(0., CubicBezier::new(0.25, 0.1, 0.25, 1.)),
            stroke: ScalarTransition::new(0., CubicBezier::new(0.25, 0.1, 0.25, 1.)),
            last_stroke: None,
            track_opacity: ScalarTransition::new(0., CubicBezier::new(0.25, 0.1, 0.25, 1.)),
            success_opacity: ScalarTransition::new(0., CubicBezier::new(0.25, 0.1, 0.25, 1.)),
            frame_pending: false,
            paint_animation: false,
            rail: ShapeCache::default(),
            track: ShapeCache::default(),
            success_track: ShapeCache::default(),
            steps: Vec::new(),
            step_groups: Vec::new(),
        }
    }
    pub fn sample(
        &mut self,
        input: &Progress,
        scope: ConfigScope,
        tokens: &ProgressTokens,
        policy: MotionPolicy,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Sample {
        if self.scope != scope {
            self._subscription = scope.observe_in(window, cx, |_, _, cx| cx.notify());
            self.scope = scope;
        }
        let now = cx.background_executor().now().duration_since(self.epoch);
        let active_elapsed = if policy.allows_motion()
            && input.kind == ProgressKind::Line
            && input.steps.is_none()
            && geometry::status(input) == ProgressStatus::Active
        {
            now.saturating_sub(*self.active_started.get_or_insert(now))
        } else {
            self.active_started = None;
            Duration::ZERO
        };
        let target = (
            input.kind,
            geometry::percent(input.percent),
            geometry::percent(input.success.map_or(0., |s| s.percent)),
        );
        let first = self.last_request.is_none_or(|old| old.0 != input.kind);
        let (_, diameter) = geometry::dimensions(&input.size, input.kind);
        let stroke = f64::from(
            geometry::length(
                input
                    .stroke_width
                    .unwrap_or((300. / diameter.max(0.001)).max(6.)),
                6.,
            )
            .min(100.),
        );
        if self.last_stroke != Some(stroke) || !policy.allows_motion() || !window.is_visible() {
            let animate = !first
                && policy.allows_motion()
                && window.is_visible()
                && input.kind != ProgressKind::Line;
            self.stroke.retarget(
                stroke,
                now + if animate {
                    Duration::from_millis(300)
                } else {
                    Duration::ZERO
                },
                if animate {
                    Duration::from_millis(60)
                } else {
                    Duration::ZERO
                },
                policy,
            );
            self.last_stroke = Some(stroke);
        }
        let opacity_duration = if first || !window.is_visible() {
            Duration::ZERO
        } else {
            Duration::from_millis(300)
        };
        self.track_opacity.retarget(
            f64::from(target.1 > target.2),
            now,
            opacity_duration,
            policy,
        );
        self.success_opacity
            .retarget(f64::from(target.2 > 0.), now, opacity_duration, policy);
        if self.last_request != Some(target) {
            let first = self.last_request.is_none_or(|old| old.0 != input.kind);
            let rapid = input.kind != ProgressKind::Line
                && self
                    .last_changed
                    .is_some_and(|last| now.saturating_sub(last) < Duration::from_millis(100));
            let duration = if first || rapid || input.steps.is_some() {
                Duration::ZERO
            } else if input.kind == ProgressKind::Line {
                tokens.globals().motion_duration_slow
            } else {
                Duration::from_millis(300)
            };
            if first {
                let curve = if input.kind == ProgressKind::Line {
                    tokens.globals().motion_ease_in_out_circ
                } else {
                    CubicBezier::new(0.25, 0.1, 0.25, 1.)
                };
                self.percent = ScalarTransition::new(target.1, curve);
                self.success = ScalarTransition::new(target.2, curve);
            }
            self.percent.retarget(target.1, now, duration, policy);
            self.success.retarget(target.2, now, duration, policy);
            self.last_request = Some(target);
            self.last_changed = Some(now);
        }
        if !policy.allows_motion() || !window.is_visible() {
            self.percent.retarget(target.1, now, Duration::ZERO, policy);
            self.success.retarget(target.2, now, Duration::ZERO, policy);
        }
        let percent = self.percent.sample(now);
        let success = self.success.sample(now);
        let stroke = self.stroke.sample(now);
        let track_opacity = self.track_opacity.sample(now);
        let success_opacity = self.success_opacity.sample(now);
        self.paint_animation = false;
        Sample {
            percent: percent.value,
            success: success.value,
            elapsed: active_elapsed,
            stroke: stroke.value as f32,
            track_opacity: track_opacity.value as f32,
            success_opacity: success_opacity.value as f32,
            paint_motion: super::paint::PaintMotion {
                now,
                duration: if input.kind == ProgressKind::Line {
                    tokens.globals().motion_duration_slow
                } else {
                    Duration::from_millis(300)
                },
                curve: if input.kind == ProgressKind::Line && input.steps.is_none() {
                    tokens.globals().motion_ease_in_out_circ
                } else {
                    CubicBezier::new(0.25, 0.1, 0.25, 1.)
                },
                enabled: policy.allows_motion() && window.is_visible(),
            },
            animate: policy.allows_motion()
                && (!percent.finished
                    || !success.finished
                    || (input.kind != ProgressKind::Line
                        && (!stroke.finished
                            || !track_opacity.finished
                            || !success_opacity.finished))
                    || (input.kind == ProgressKind::Line
                        && input.steps.is_none()
                        && geometry::status(input) == ProgressStatus::Active
                        && percent.value > 0.)),
        }
    }
    pub fn schedule(&mut self, visible: bool, animate: bool, window: &Window, cx: &Context<Self>) {
        if !visible || !animate || self.frame_pending {
            return;
        }
        self.frame_pending = true;
        let state = cx.weak_entity();
        window.on_next_frame(move |_, cx| {
            let _ = state.update(cx, |state, cx| {
                state.frame_pending = false;
                cx.notify();
            });
        });
    }
}
