// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::{
    Spin, SpinPercent, SpinStyle,
    geometry::{auto_percent, finite_percent},
};
use crate::{
    ConfigScope,
    behavior::{MotionPolicy, ScalarTransition},
    theme::CubicBezier,
};
use gpui::{App, Context, Subscription, Task, Window};
use std::time::{Duration, Instant};

const EASE: CubicBezier = CubicBezier::new(0.25, 0.1, 0.25, 1.);

#[derive(Clone, Copy, Debug)]
pub(super) struct Sample {
    pub elapsed: Duration,
    pub percent: Option<f64>,
    pub progress: f64,
    pub ring: f64,
    pub dots: f64,
    pub dim: f64,
    pub mask: f64,
    pub indicator_transitioning: bool,
    pub surface_transitioning: bool,
}

pub(super) struct SpinState {
    pub scope: ConfigScope,
    pub loading: bool,
    pub visible: bool,
    request: (bool, Duration),
    epoch: Instant,
    started: Option<Duration>,
    auto_started: Option<Duration>,
    automatic: bool,
    generation: u64,
    delay_task: Option<Task<()>>,
    auto_task: Option<Task<()>>,
    frame_pending: bool,
    ring: ScalarTransition,
    dots: ScalarTransition,
    ring_mounted: bool,
    indicator_new_run: bool,
    custom: bool,
    progress: ScalarTransition,
    dim: ScalarTransition,
    mask: ScalarTransition,
    initialized: bool,
    _config: Subscription,
    pub ring_cache: super::paint::RingCache,
    pub dot_cache: super::paint::DotCache,
}

impl SpinState {
    pub fn new(scope: ConfigScope, window: &mut Window, cx: &mut Context<Self>) -> Self {
        let subscription = scope.observe_in(window, cx, |_, _, cx| cx.notify());
        Self {
            scope,
            loading: false,
            visible: false,
            request: (false, Duration::ZERO),
            epoch: cx.background_executor().now(),
            started: None,
            auto_started: None,
            automatic: false,
            generation: 0,
            delay_task: None,
            auto_task: None,
            frame_pending: false,
            ring: ScalarTransition::new(0., EASE),
            dots: ScalarTransition::new(1., EASE),
            ring_mounted: false,
            indicator_new_run: false,
            custom: false,
            progress: ScalarTransition::new(0., EASE),
            dim: ScalarTransition::new(0., EASE),
            mask: ScalarTransition::new(0., EASE),
            initialized: false,
            _config: subscription,
            ring_cache: super::paint::RingCache::default(),
            dot_cache: super::paint::DotCache::default(),
        }
    }
    pub fn now(&self, cx: &App) -> Duration {
        cx.background_executor().now().duration_since(self.epoch)
    }

    pub fn set_custom(&mut self, custom: bool) {
        if self.custom != custom {
            self.custom = custom;
            self.ring_mounted = false;
            self.indicator_new_run = true;
        }
    }

    fn start(&mut self, cx: &App) {
        if !self.loading {
            self.loading = true;
            self.indicator_new_run = true;
            self.started = Some(self.now(cx));
            if self.automatic {
                self.auto_started = self.started;
            }
        }
    }

    pub fn update(
        &mut self,
        input: &Spin,
        scope: ConfigScope,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.scope != scope {
            self._config = scope.observe_in(window, cx, |_, _, cx| cx.notify());
            self.scope = scope;
        }
        let automatic = input.percent == Some(SpinPercent::Auto);
        if self.automatic != automatic {
            self.automatic = automatic;
            self.auto_started = (automatic && self.loading).then(|| self.now(cx));
            self.auto_task = None;
        }
        let request = (input.spinning, input.delay);
        if self.request != request {
            self.request = request;
            self.generation = self.generation.wrapping_add(1);
            self.delay_task = None;
            if !input.spinning {
                self.loading = false;
                self.ring_mounted = false;
                self.started = None;
                self.auto_started = None;
                self.auto_task = None;
            } else if !self.loading {
                if input.delay.is_zero() {
                    self.start(cx);
                } else {
                    // An unrepresentable deadline remains pending until the owner
                    // changes/cancels it, rather than overflowing Instant in the timer.
                    if cx
                        .background_executor()
                        .now()
                        .checked_add(input.delay)
                        .is_none()
                    {
                        return;
                    }
                    let generation = self.generation;
                    let timer = cx.background_executor().timer(input.delay);
                    self.delay_task = Some(cx.spawn_in(window, async move |state, cx| {
                        timer.await;
                        let _ = state.update_in(cx, |state, _, cx| {
                            if state.generation == generation && state.request.0 {
                                state.start(cx);
                                state.delay_task = None;
                                cx.notify();
                            }
                        });
                    }));
                }
            }
        }
    }

    pub fn sample(
        &mut self,
        input: &Spin,
        metrics: SpinStyle,
        motion: MotionPolicy,
        cx: &App,
    ) -> Sample {
        let now = self.now(cx);
        let percent = match input.percent {
            Some(SpinPercent::Value(value)) => Some(finite_percent(value)),
            Some(SpinPercent::Auto) => Some(auto_percent(self.auto_started.map_or(0, |start| {
                (now.saturating_sub(start).as_millis() / 200) as u64
            }))),
            None => None,
        };
        let safe = percent.unwrap_or(0.).clamp(0., 100.);
        let ring_target = if safe > 0. { 1. } else { 0. };
        let dim_target = if self.loading && input.has_children {
            1.
        } else {
            0.
        };
        let mask_target = if self.loading && input.fullscreen {
            1.
        } else {
            0.
        };
        if !self.initialized {
            self.ring = ScalarTransition::new(ring_target, EASE);
            self.dots = ScalarTransition::new(1. - ring_target, EASE);
            self.progress = ScalarTransition::new(safe, EASE);
            self.dim = ScalarTransition::new(dim_target, EASE);
            self.mask = ScalarTransition::new(mask_target, EASE);
            self.initialized = true;
        }
        if self.indicator_new_run {
            self.indicator_new_run = false;
            self.started = self.loading.then_some(now);
            self.ring = ScalarTransition::new(ring_target, EASE);
            self.dots = ScalarTransition::new(1. - ring_target, EASE);
            self.progress = ScalarTransition::new(safe, EASE);
        }
        // Upstream lazily mounts the ring at the first positive value, so there
        // is no entering opacity/dash transition for that initial ring frame.
        if self.loading && percent.unwrap_or(0.) != 0. && !self.ring_mounted {
            self.ring_mounted = true;
            self.ring = ScalarTransition::new(ring_target, EASE);
            self.progress = ScalarTransition::new(safe, EASE);
        }
        self.ring
            .retarget(ring_target, now, metrics.motion_duration, motion);
        self.dots
            .retarget(1. - ring_target, now, metrics.motion_duration, motion);
        self.progress
            .retarget(safe, now, metrics.motion_duration, motion);
        self.dim
            .retarget(dim_target, now, metrics.motion_duration, motion);
        self.mask
            .retarget(mask_target, now, metrics.fullscreen_duration, motion);
        let ring = self.ring.sample(now);
        let dots = self.dots.sample(now);
        let progress = self.progress.sample(now);
        let dim = self.dim.sample(now);
        let mask = self.mask.sample(now);
        Sample {
            elapsed: self
                .started
                .map_or(Duration::ZERO, |start| now.saturating_sub(start)),
            percent,
            progress: progress.value,
            ring: ring.value,
            dots: dots.value,
            dim: dim.value,
            mask: mask.value,
            indicator_transitioning: self.loading
                && (!ring.finished || !dots.finished || !progress.finished),
            surface_transitioning: if input.fullscreen {
                !mask.finished
            } else {
                input.has_children && !dim.finished
            },
        }
    }

    /// Called only for actually painted, unclipped content. One wakeup source owns
    /// each visible Spin: a continuous frame, or a coarse automatic-progress timer.
    pub fn schedule(
        &mut self,
        visible: bool,
        animation: bool,
        window: &Window,
        cx: &Context<Self>,
    ) {
        self.visible = visible;
        if !visible {
            self.auto_task = None;
            return;
        }
        if animation && !self.frame_pending {
            self.frame_pending = true;
            let state = cx.weak_entity();
            window.on_next_frame(move |_, cx| {
                let _ = state.update(cx, |state, cx| {
                    state.frame_pending = false;
                    cx.notify();
                });
            });
        }
        if self.frame_pending {
            // Auto progress is sampled from elapsed time on every render. A
            // frame already guarantees the next sample; a simultaneous timer
            // only adds synchronized entity notifications for large groups.
            self.auto_task = None;
        } else if self.loading && self.automatic && self.auto_task.is_none() {
            let elapsed = self
                .now(cx)
                .saturating_sub(self.auto_started.unwrap_or_default());
            let ticks = elapsed.as_millis() / 200;
            if ticks < 235 {
                let wait = Duration::from_millis(200 - (elapsed.as_millis() % 200) as u64);
                let timer = cx.background_executor().timer(wait);
                let state = cx.weak_entity();
                self.auto_task = Some(cx.spawn(async move |_, cx| {
                    timer.await;
                    let _ = state.update(cx, |state, cx| {
                        state.auto_task = None;
                        if state.visible && state.loading && state.automatic {
                            cx.notify();
                        }
                    });
                }));
            }
        }
    }
}
