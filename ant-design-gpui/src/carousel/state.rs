// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::{
    CarouselEffect, CarouselItems, CarouselOptions,
    model::{CarouselModel, ViewFactory},
};
use crate::{ConfigScope, Direction, behavior::evaluate_cubic_bezier, theme::CubicBezier};
use gpui::{
    AnyView, App, Bounds, Context, Entity, EventEmitter, FocusHandle, Focusable, Pixels, Point,
    SharedString, Subscription, Task, Window,
};
use std::{
    collections::HashMap,
    rc::Rc,
    time::{Duration, Instant},
};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CarouselSource {
    Arrow,
    Dot,
    Keyboard,
    Drag,
    Autoplay,
    Programmatic,
    Accessibility,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CarouselChange {
    pub from: usize,
    pub to: usize,
    pub from_key: SharedString,
    pub to_key: SharedString,
    pub source: CarouselSource,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum CarouselEvent {
    BeforeChange(CarouselChange),
    AfterChange(CarouselChange),
}
pub(super) type EventHandler = Rc<dyn Fn(&CarouselEvent, &mut Window, &mut App)>;

pub(super) struct SlideView {
    pub view: AnyView,
    pub factory: ViewFactory,
    _observer: Option<Subscription>,
}

struct Motion {
    change: Option<CarouselChange>,
    from_position: f64,
    to_position: f64,
    fade_layers: Vec<(usize, f64)>,
    started: Duration,
    duration: Duration,
    curve: CubicBezier,
}

#[derive(Clone, Debug)]
pub(super) struct Sample {
    /// Position in logical slide units; may be -1 or `items.len()` at a seam.
    pub position: f64,
    /// Back-to-front native fade layers, including interrupted retiring slides.
    pub fade_layers: Vec<(usize, f64)>,
    pub animating: bool,
    pub autoplay_progress: f64,
}

pub(super) struct Drag {
    start: Point<Pixels>,
    original_position: f64,
    pub active: bool,
    rejected: bool,
    displacement: f64,
}

/// Retain one controller per mounted Carousel. Keyed content entities survive
/// navigation and reorder; timers and pointer gestures belong to this mount.
pub struct CarouselState {
    pub(super) model: CarouselModel,
    pub(super) options: CarouselOptions,
    pub(super) direction: Direction,
    pub(super) scope: Option<ConfigScope>,
    pub(super) views: HashMap<SharedString, SlideView>,
    pub(super) focus: FocusHandle,
    pub(super) bounds: Option<Bounds<Pixels>>,
    pub(super) dot_bounds: HashMap<usize, Bounds<Pixels>>,
    pub(super) previous_arrow_bounds: Option<Bounds<Pixels>>,
    pub(super) next_arrow_bounds: Option<Bounds<Pixels>>,
    pub(super) drag: Option<Drag>,
    pub(super) hovered: bool,
    pub(super) focused: bool,
    pub(super) visible: bool,
    pub(super) mounted: bool,
    pub(super) generation: u64,
    pub(super) frame_pending: bool,
    pub(super) measurement_generation: u64,
    pub(super) measurement_revisions: HashMap<SharedString, u64>,
    pub(super) measurement_revision_version: u64,
    pub(super) on_event: Option<EventHandler>,
    position: f64,
    motion: Option<Motion>,
    epoch: Instant,
    paused: bool,
    autoplay_started: Option<Duration>,
    autoplay_elapsed: Duration,
    autoplay_task: Option<Task<()>>,
    autoplay_generation: u64,
    scope_subscription: Option<Subscription>,
    _visibility: Subscription,
    #[cfg(any(test, feature = "test-support"))]
    test_time: Option<Duration>,
}
impl EventEmitter<CarouselEvent> for CarouselState {}
impl Focusable for CarouselState {
    fn focus_handle(&self, _: &App) -> FocusHandle {
        self.focus.clone()
    }
}
impl CarouselState {
    pub fn new(items: CarouselItems, window: &mut Window, cx: &mut Context<Self>) -> Self {
        let visibility = cx.observe_window_visibility(window, |state, _, window, cx| {
            if !window.is_visible() && !state.clock_frozen() {
                state.visible = false;
                state.cancel_drag();
                state.finish_motion(window, cx);
            }
            state.sync_autoplay(window, cx);
            cx.notify();
        });
        Self {
            model: CarouselModel::new(items),
            options: CarouselOptions::default(),
            direction: Direction::Ltr,
            scope: None,
            views: HashMap::new(),
            focus: cx.focus_handle().tab_stop(false),
            bounds: None,
            dot_bounds: HashMap::new(),
            previous_arrow_bounds: None,
            next_arrow_bounds: None,
            drag: None,
            hovered: false,
            focused: false,
            visible: false,
            mounted: false,
            generation: 0,
            frame_pending: false,
            measurement_generation: 0,
            measurement_revisions: HashMap::new(),
            measurement_revision_version: 0,
            on_event: None,
            position: 0.,
            motion: None,
            epoch: cx.background_executor().now(),
            paused: false,
            autoplay_started: None,
            autoplay_elapsed: Duration::ZERO,
            autoplay_task: None,
            autoplay_generation: 0,
            scope_subscription: None,
            _visibility: visibility,
            #[cfg(any(test, feature = "test-support"))]
            test_time: None,
        }
    }
    pub fn current_index(&self) -> Option<usize> {
        self.model.current
    }
    /// Stable application identity of the current slide, independent of its index.
    pub fn current_key(&self) -> Option<&SharedString> {
        self.current_index()
            .and_then(|index| self.items().entries().get(index))
            .map(|slide| &slide.key)
    }
    pub fn items(&self) -> &CarouselItems {
        &self.model.items
    }
    pub fn focus(&self, window: &mut Window, cx: &mut App) {
        self.focus.focus(window, cx);
    }
    pub fn viewport_bounds(&self) -> Option<Bounds<Pixels>> {
        self.bounds
    }
    pub fn dot_bounds(&self, index: usize) -> Option<Bounds<Pixels>> {
        self.dot_bounds.get(&index).copied()
    }
    pub fn previous_arrow_bounds(&self) -> Option<Bounds<Pixels>> {
        self.previous_arrow_bounds
    }
    pub fn next_arrow_bounds(&self) -> Option<Bounds<Pixels>> {
        self.next_arrow_bounds
    }
    pub fn retained_view_count(&self) -> usize {
        self.views.len()
    }
    pub fn is_animating(&self, cx: &App) -> bool {
        self.motion
            .as_ref()
            .is_some_and(|motion| self.now(cx).saturating_sub(motion.started) < motion.duration)
    }
    pub fn autoplay_running(&self) -> bool {
        self.autoplay_started.is_some()
    }
    pub fn set_items(&mut self, items: CarouselItems, window: &mut Window, cx: &mut Context<Self>) {
        if !self.model.set_items(items) {
            return;
        }
        self.views.retain(|key, view| {
            self.model
                .items
                .get(key)
                .and_then(|item| item.factory.as_ref())
                .is_some_and(|factory| Rc::ptr_eq(factory, &view.factory))
        });
        self.motion = None;
        self.cancel_drag();
        self.position = self.model.current.unwrap_or(0) as f64;
        self.dot_bounds.clear();
        self.measurement_revisions
            .retain(|key, _| self.model.items.get(key).is_some());
        self.invalidate_measurements(cx);
        self.reset_autoplay();
        self.sync_autoplay(window, cx);
        cx.notify();
    }
    /// Re-measure intrinsic content after changes outside a typed slide view's
    /// own notifications, such as external data read by a content closure.
    pub fn invalidate_measurements(&mut self, cx: &mut Context<Self>) {
        self.measurement_generation = self.measurement_generation.wrapping_add(1);
        cx.notify();
    }
    /// Invalidate one retained slide's intrinsic measurement without forcing a
    /// scan of unaffected slides. Unknown or removed keys are ignored.
    pub fn invalidate_slide_measurement(&mut self, key: &str, cx: &mut Context<Self>) {
        if let Some(key) = self.items().get(key).map(|slide| slide.key.clone()) {
            let revision = self.measurement_revisions.entry(key).or_default();
            *revision = revision.wrapping_add(1);
            self.measurement_revision_version = self.measurement_revision_version.wrapping_add(1);
            cx.notify();
        }
    }
    pub fn go_to(
        &mut self,
        index: usize,
        dont_animate: bool,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        self.activate(
            index,
            CarouselSource::Programmatic,
            dont_animate,
            window,
            cx,
        );
    }
    pub fn next(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        self.navigate(1, CarouselSource::Programmatic, window, cx);
    }
    pub fn prev(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        self.navigate(-1, CarouselSource::Programmatic, window, cx);
    }
    pub fn pause(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        self.paused = true;
        self.sync_autoplay(window, cx);
        cx.notify();
    }
    pub fn resume(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        self.paused = false;
        self.sync_autoplay(window, cx);
        cx.notify();
    }
    /// Resume the configured autoplay countdown from its paused elapsed time.
    pub fn play(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        self.resume(window, cx);
    }
    pub(super) fn now(&self, cx: &App) -> Duration {
        #[cfg(any(test, feature = "test-support"))]
        if let Some(now) = self.test_time {
            return now;
        }
        cx.background_executor()
            .now()
            .saturating_duration_since(self.epoch)
    }
    fn clock_frozen(&self) -> bool {
        #[cfg(any(test, feature = "test-support"))]
        {
            self.test_time.is_some()
        }
        #[cfg(not(any(test, feature = "test-support")))]
        {
            false
        }
    }
    /// Deterministic visual/performance clock. Frozen clocks do not schedule
    /// timers or frames; hidden native capture windows can still sample autoplay.
    #[cfg(any(test, feature = "test-support"))]
    pub fn set_test_time(&mut self, elapsed: Option<Duration>, cx: &mut Context<Self>) {
        if self.test_time.is_some() != elapsed.is_some() {
            self.autoplay_elapsed = self.autoplay_elapsed_at(self.now(cx));
            self.autoplay_started = None;
            self.cancel_autoplay_task();
            self.frame_pending = false;
        }
        self.test_time = elapsed;
        cx.notify();
    }
    pub(super) fn mount(&mut self) -> u64 {
        self.generation = self.generation.wrapping_add(1);
        self.mounted = true;
        self.frame_pending = false;
        self.generation
    }
    pub(super) fn unmount(&mut self, generation: u64) {
        if self.generation != generation {
            return;
        }
        self.generation = self.generation.wrapping_add(1);
        self.mounted = false;
        self.visible = false;
        self.hovered = false;
        self.focused = false;
        self.frame_pending = false;
        self.cancel_drag();
        self.motion = None;
        self.position = self.model.current.unwrap_or(0) as f64;
        self.reset_autoplay();
        self.on_event = None;
    }
    pub(super) fn configure(
        &mut self,
        options: CarouselOptions,
        scope: ConfigScope,
        on_event: Option<EventHandler>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        self.on_event = on_event;
        let config = scope.snapshot(cx);
        let direction = config.direction;
        let initial_changed =
            self.model.initialized && self.options.initial_slide != options.initial_slide;
        if self.scope.as_ref() != Some(&scope) {
            self.scope_subscription = Some(scope.observe_in(window, cx, |state, window, cx| {
                state.cancel_drag();
                state.invalidate_measurements(cx);
                if !state.motion_allowed(cx) {
                    state.finish_motion(window, cx);
                }
                state.sync_autoplay(window, cx);
                cx.notify();
            }));
            self.scope = Some(scope);
            self.invalidate_measurements(cx);
        }
        if !self.model.initialized && !self.items().is_empty() {
            self.model.initialized = true;
            self.model.current = options.normalize(options.initial_slide, self.items().len());
            self.position = self.model.current.unwrap_or(0) as f64;
        }
        let geometry_changed = self.options.is_vertical() != options.is_vertical()
            || self.options.effect != options.effect
            || self.direction != direction;
        if geometry_changed {
            self.cancel_drag();
            self.finish_motion(window, cx);
            self.invalidate_measurements(cx);
        }
        if self.options.autoplay != options.autoplay
            || self.options.autoplay_speed != options.autoplay_speed
        {
            self.reset_autoplay();
        }
        self.options = options;
        self.direction = direction;
        if initial_changed {
            self.cancel_drag();
            self.motion = None;
            self.model.current = self
                .options
                .normalize(self.options.initial_slide, self.items().len());
            self.position = self.model.current.unwrap_or(0) as f64;
            self.reset_autoplay();
        }
        if !self.options.draggable {
            self.cancel_drag();
        }
        if !self.motion_allowed(cx) {
            self.finish_motion(window, cx);
        } else {
            self.finish_if_elapsed(window, cx);
        }
        self.sync_autoplay(window, cx);
    }
    pub(super) fn ensure_view(
        state: &Entity<Self>,
        index: usize,
        window: &mut Window,
        cx: &mut App,
    ) -> Option<AnyView> {
        let (key, factory) = {
            let state = state.read(cx);
            let item = state.items().entries().get(index)?;
            if let Some(view) = state.views.get(&item.key) {
                return Some(view.view.clone());
            }
            (item.key.clone(), item.factory.clone()?)
        };
        // Application factories may inspect or update this controller. Invoke
        // them without an outstanding entity borrow, then validate their key
        // and factory in case the callback replaced or reordered the slides.
        let content = factory(window, cx);
        state.update(cx, |state, cx| {
            if !state
                .items()
                .get(&key)
                .and_then(|item| item.factory.as_ref())
                .is_some_and(|current| Rc::ptr_eq(current, &factory))
            {
                return None;
            }
            if let Some(view) = state.views.get(&key) {
                return Some(view.view.clone());
            }
            let observer = content.observer.map(|observe| observe(key.clone(), cx));
            let view = content.view.clone();
            state.views.insert(
                key,
                SlideView {
                    view: content.view,
                    factory,
                    _observer: observer,
                },
            );
            Some(view)
        })
    }
    fn motion_allowed(&self, cx: &App) -> bool {
        self.scope.as_ref().map_or(!cx.reduce_motion(), |scope| {
            scope.snapshot(cx).motion_policy(cx).allows_motion()
        })
    }
    fn disabled(&self, cx: &App) -> bool {
        self.scope
            .as_ref()
            .is_some_and(|scope| scope.snapshot(cx).disabled)
    }
    pub(super) fn sample(&self, cx: &App) -> Sample {
        let mut position = self.position;
        let mut fade_layers = if self.options.effect == CarouselEffect::Fade {
            self.model
                .current
                .map_or_else(Vec::new, |index| vec![(index, 1.)])
        } else {
            Vec::new()
        };
        let mut animating = false;
        if let Some(motion) = &self.motion {
            let elapsed = self.now(cx).saturating_sub(motion.started);
            let progress = if motion.duration.is_zero() {
                1.
            } else {
                (elapsed.as_secs_f64() / motion.duration.as_secs_f64()).clamp(0., 1.)
            };
            let eased = evaluate_cubic_bezier(motion.curve, progress);
            position = motion.from_position + (motion.to_position - motion.from_position) * eased;
            animating = progress < 1.;
            if self.options.effect == CarouselEffect::Fade {
                let target = self.model.current;
                let alpha = eased.clamp(0., 1.);
                fade_layers = motion
                    .fade_layers
                    .iter()
                    .filter_map(|&(index, weight)| {
                        let weight = if Some(index) == target {
                            weight + (1. - weight) * alpha
                        } else {
                            weight * (1. - alpha)
                        };
                        (Some(index) == target || weight > 0.000001).then_some((index, weight))
                    })
                    .collect();
                if let Some(target) = target
                    && !motion.fade_layers.iter().any(|&(index, _)| index == target)
                {
                    fade_layers.push((target, alpha));
                }
            }
        }
        if let Some(drag) = &self.drag
            && drag.active
        {
            position = drag.original_position + drag.displacement;
        }
        let interval = self.options.autoplay_speed.max(Duration::from_millis(1));
        let autoplay_progress = if self.options.autoplay {
            (self.autoplay_elapsed_at(self.now(cx)).as_secs_f64() / interval.as_secs_f64())
                .clamp(0., 1.)
        } else {
            0.
        };
        Sample {
            position,
            fade_layers,
            animating,
            autoplay_progress,
        }
    }
    fn emit(&mut self, event: CarouselEvent, window: &mut Window, cx: &mut Context<Self>) {
        cx.emit(event.clone());
        if let Some(callback) = self.on_event.clone() {
            window.defer(cx, move |window, cx| callback(&event, window, cx));
        }
    }
    pub(super) fn activate(
        &mut self,
        index: usize,
        source: CarouselSource,
        dont_animate: bool,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        self.activate_at(index, None, source, dont_animate, window, cx);
    }
    fn activate_at(
        &mut self,
        index: usize,
        logical_target: Option<f64>,
        source: CarouselSource,
        dont_animate: bool,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        self.finish_if_elapsed(window, cx);
        if source != CarouselSource::Programmatic && self.disabled(cx) {
            return;
        }
        let sample = self.sample(cx);
        if self.options.wait_for_animate && sample.animating {
            return;
        }
        let Some(target) = self.options.normalize(index, self.items().len()) else {
            return;
        };
        let Some(from) = self.model.current else {
            return;
        };
        self.model.initialized = true;
        if target == from {
            self.cancel_drag();
            if dont_animate {
                self.finish_motion(window, cx);
                cx.notify();
            }
            self.reset_autoplay();
            self.sync_autoplay(window, cx);
            return;
        }
        let change = CarouselChange {
            from,
            to: target,
            from_key: self.items().entries()[from].key.clone(),
            to_key: self.items().entries()[target].key.clone(),
            source,
        };
        self.motion = None;
        self.cancel_drag();
        self.model.current = Some(target);
        self.reset_autoplay();
        self.emit(CarouselEvent::BeforeChange(change.clone()), window, cx);
        if !dont_animate && !self.options.speed.is_zero() && self.motion_allowed(cx) {
            self.motion = Some(Motion {
                change: Some(change),
                from_position: sample.position,
                to_position: logical_target.unwrap_or(target as f64),
                fade_layers: sample.fade_layers,
                started: self.now(cx),
                duration: self.options.speed,
                curve: self.options.easing,
            });
            self.request_frame(window, cx);
        } else {
            self.position = target as f64;
            self.emit(CarouselEvent::AfterChange(change), window, cx);
        }
        self.sync_autoplay(window, cx);
        cx.notify();
    }
    pub(super) fn navigate(
        &mut self,
        step: isize,
        source: CarouselSource,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        // A deadline can precede the next paint. Settle its unwrapped seam
        // coordinate before deriving the following transition's destination.
        self.finish_if_elapsed(window, cx);
        let Some(target) = self.model.next(step, self.options.infinite) else {
            return;
        };
        let current = self.model.current.unwrap_or(0);
        let logical = if self.options.infinite {
            self.motion
                .as_ref()
                .map_or(current as f64, |motion| motion.to_position)
                + step as f64
        } else {
            target as f64
        };
        self.activate_at(target, Some(logical), source, false, window, cx);
    }
    fn finish_if_elapsed(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if self.motion.is_some() && !self.is_animating(cx) {
            self.finish_motion(window, cx);
        }
    }
    fn finish_motion(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if let Some(motion) = self.motion.take() {
            self.position = self.model.current.unwrap_or(0) as f64;
            if let Some(change) = motion.change {
                self.emit(CarouselEvent::AfterChange(change), window, cx);
            }
        }
    }
    pub(super) fn set_hovered(
        &mut self,
        hovered: bool,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.hovered != hovered {
            self.hovered = hovered;
            self.sync_autoplay(window, cx);
            cx.notify();
        }
    }
    pub(super) fn set_focused(
        &mut self,
        focused: bool,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.focused != focused {
            self.focused = focused;
            self.sync_autoplay(window, cx);
            cx.notify();
        }
    }
    pub(super) fn set_visible(
        &mut self,
        visible: bool,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.visible != visible {
            self.visible = visible;
            if !visible && !self.clock_frozen() {
                self.cancel_drag();
                self.finish_motion(window, cx);
            }
            self.sync_autoplay(window, cx);
        }
    }
    pub(super) fn schedule(&mut self, visible: bool, window: &mut Window, cx: &mut Context<Self>) {
        self.set_visible(visible, window, cx);
        self.finish_if_elapsed(window, cx);
        self.sync_autoplay(window, cx);
        if visible
            && (self.is_animating(cx)
                || (self.options.dot_duration
                    && self.options.dots
                    && self.autoplay_started.is_some()
                    && self.motion_allowed(cx)))
        {
            self.request_frame(window, cx);
        }
    }
    pub(super) fn request_frame(&mut self, window: &Window, cx: &Context<Self>) {
        if self.frame_pending || !self.mounted || !self.visible || self.clock_frozen() {
            return;
        }
        self.frame_pending = true;
        let state = cx.weak_entity();
        let generation = self.generation;
        window.on_next_frame(move |window, cx| {
            let _ = state.update(cx, |state, cx| {
                if state.mounted && state.generation == generation {
                    state.frame_pending = false;
                    state.finish_if_elapsed(window, cx);
                    state.sync_autoplay(window, cx);
                    cx.notify();
                }
            });
        });
    }
    fn cancel_autoplay_task(&mut self) {
        self.autoplay_generation = self.autoplay_generation.wrapping_add(1);
        self.autoplay_task = None;
    }
    fn reset_autoplay(&mut self) {
        self.cancel_autoplay_task();
        self.autoplay_started = None;
        self.autoplay_elapsed = Duration::ZERO;
    }
    fn autoplay_elapsed_at(&self, now: Duration) -> Duration {
        self.autoplay_elapsed.saturating_add(
            self.autoplay_started
                .map_or(Duration::ZERO, |started| now.saturating_sub(started)),
        )
    }
    fn autoplay_allowed(&self, window: &Window, cx: &App) -> bool {
        let visible = self.visible && window.is_visible() || self.clock_frozen();
        self.mounted
            && visible
            && self.options.autoplay
            && !self.paused
            && !self.hovered
            && !self.focused
            && !self.disabled(cx)
            && self.drag.is_none()
            && self.model.next(1, self.options.infinite).is_some()
    }
    fn sync_autoplay(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let allowed = self.autoplay_allowed(window, cx);
        if !allowed {
            if let Some(started) = self.autoplay_started.take() {
                self.autoplay_elapsed = self
                    .autoplay_elapsed
                    .saturating_add(self.now(cx).saturating_sub(started));
            }
            self.cancel_autoplay_task();
            return;
        }
        if self.autoplay_started.is_none() {
            self.autoplay_started = Some(self.now(cx));
        }
        if self.autoplay_task.is_some() || self.clock_frozen() {
            return;
        }
        let interval = self.options.autoplay_speed.max(Duration::from_millis(1));
        let wait = interval.saturating_sub(self.autoplay_elapsed_at(self.now(cx)));
        if cx.background_executor().now().checked_add(wait).is_none() {
            return;
        }
        let timer = cx.background_executor().timer(wait);
        let generation = self.autoplay_generation;
        self.autoplay_task = Some(cx.spawn_in(window, async move |state, cx| {
            timer.await;
            let _ = state.update_in(cx, |state, window, cx| {
                if generation != state.autoplay_generation || !state.mounted {
                    return;
                }
                state.autoplay_task = None;
                if state.autoplay_allowed(window, cx) {
                    // A deadline skipped by wait_for_animate still begins a new
                    // interval; repeatedly scheduling a zero wait would spin.
                    state.reset_autoplay();
                    state.navigate(1, CarouselSource::Autoplay, window, cx);
                }
                state.sync_autoplay(window, cx);
            });
        }));
    }
    pub(super) fn pointer_start(
        &mut self,
        position: Point<Pixels>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> bool {
        if !self.options.draggable
            || self.disabled(cx)
            || self.items().len() < 2
            || self.bounds.is_none()
            || self.options.wait_for_animate && self.is_animating(cx)
        {
            return false;
        }
        self.drag = Some(Drag {
            start: position,
            original_position: self.sample(cx).position,
            active: false,
            rejected: false,
            displacement: 0.,
        });
        self.sync_autoplay(window, cx);
        true
    }
    pub(super) fn pointer_move(
        &mut self,
        position: Point<Pixels>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> bool {
        let Some(bounds) = self.bounds else {
            return false;
        };
        let vertical = self.options.is_vertical();
        let extent = f64::from(f32::from(if vertical {
            bounds.size.height
        } else {
            bounds.size.width
        }));
        if extent <= 0. {
            return false;
        }
        let sign = self.options.axis_direction(self.direction);
        let Some(drag) = self.drag.as_mut() else {
            return false;
        };
        if drag.rejected {
            return false;
        }
        let dx = f64::from(f32::from(position.x - drag.start.x));
        let dy = f64::from(f32::from(position.y - drag.start.y));
        let (axis, cross) = if vertical { (dy, dx) } else { (dx, dy) };
        if !drag.active {
            if axis.abs().max(cross.abs()) < 10. {
                return false;
            }
            if cross.abs() > axis.abs() {
                drag.rejected = true;
                return false;
            }
            drag.active = true;
            self.motion = None;
        }
        let displacement = -axis / extent * sign;
        let desired = drag.original_position + displacement;
        drag.displacement = if !self.options.infinite
            && (desired < 0. || desired > (self.model.items.len() - 1) as f64)
        {
            displacement * 0.35
        } else {
            displacement
        };
        self.sync_autoplay(window, cx);
        cx.notify();
        true
    }
    pub(super) fn pointer_end(&mut self, window: &mut Window, cx: &mut Context<Self>) -> bool {
        let Some(drag) = self.drag.take() else {
            return false;
        };
        if !drag.active {
            self.sync_autoplay(window, cx);
            return false;
        }
        let position = drag.original_position + drag.displacement;
        let step = if drag.displacement.abs() >= 0.2 {
            if drag.displacement > 0. { 1 } else { -1 }
        } else {
            0
        };
        if self.model.next(step, self.options.infinite).is_some() {
            self.position = position;
            self.navigate(step, CarouselSource::Drag, window, cx);
        } else {
            self.motion = Some(Motion {
                change: None,
                from_position: position,
                to_position: self.model.current.unwrap_or(0) as f64,
                fade_layers: self
                    .model
                    .current
                    .map_or_else(Vec::new, |index| vec![(index, 1.)]),
                started: self.now(cx),
                duration: self.options.speed,
                curve: self.options.easing,
            });
            if !self.motion_allowed(cx) || self.options.speed.is_zero() {
                self.finish_motion(window, cx);
            } else {
                self.request_frame(window, cx);
            }
        }
        self.sync_autoplay(window, cx);
        cx.notify();
        true
    }
    fn cancel_drag(&mut self) {
        self.drag = None;
    }
}

#[cfg(test)]
mod core_tests {
    use super::*;
    use crate::carousel::CarouselSlide;
    use gpui::{AppContext, Entity, IntoElement, Render, TestAppContext, WindowHandle, div};
    use std::cell::RefCell;

    struct Empty;
    impl Render for Empty {
        fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
            div()
        }
    }
    struct Fixture {
        cx: TestAppContext,
        window: WindowHandle<Empty>,
        state: Entity<CarouselState>,
        events: Rc<RefCell<Vec<CarouselEvent>>>,
    }
    impl Fixture {
        fn new(options: CarouselOptions) -> Self {
            let mut cx = TestAppContext::single();
            let events = Rc::new(RefCell::new(Vec::new()));
            let output = events.clone();
            let mut state = None;
            let window = cx.add_window(|window, cx| {
                let scope = ConfigScope::new_window(cx);
                let items = CarouselItems::new(
                    (0..4).map(|index| CarouselSlide::new(index.to_string()).text("Slide")),
                )
                .unwrap();
                let retained = cx.new(|cx| CarouselState::new(items, window, cx));
                retained.update(cx, |state, cx| {
                    state.set_test_time(Some(Duration::ZERO), cx);
                    state.mount();
                    state.configure(
                        options,
                        scope,
                        Some(Rc::new(move |event, _, _| {
                            output.borrow_mut().push(event.clone())
                        })),
                        window,
                        cx,
                    );
                    state.set_visible(true, window, cx);
                });
                state = Some(retained);
                Empty
            });
            Self {
                cx,
                window,
                state: state.unwrap(),
                events,
            }
        }
        fn update(
            &mut self,
            callback: impl FnOnce(&mut CarouselState, &mut Window, &mut Context<CarouselState>),
        ) {
            self.cx
                .update_window(self.window.into(), |_, window, cx| {
                    self.state
                        .update(cx, |state, cx| callback(state, window, cx));
                })
                .unwrap();
            self.cx.run_until_parked();
        }
        fn time(&mut self, millis: u64) {
            self.update(|state, window, cx| {
                state.set_test_time(Some(Duration::from_millis(millis)), cx);
                state.schedule(true, window, cx);
            });
        }
        fn sample(&self) -> Sample {
            self.state.read_with(&self.cx, |state, cx| state.sample(cx))
        }
    }

    #[test]
    fn programmatic_navigation_emits_ordered_completion_at_the_deadline() {
        let mut f = Fixture::new(CarouselOptions::default());
        f.update(|state, window, cx| state.go_to(3, false, window, cx));
        assert_eq!(
            f.state.read_with(&f.cx, |state, _| state.current_index()),
            Some(3)
        );
        assert_eq!(f.events.borrow().len(), 1);
        assert!(matches!(
            f.events.borrow()[0],
            CarouselEvent::BeforeChange(CarouselChange {
                from: 0,
                to: 3,
                source: CarouselSource::Programmatic,
                ..
            })
        ));
        f.time(250);
        assert!(f.sample().position > 0. && f.sample().position < 3.);
        f.time(500);
        assert!(!f.sample().animating);
        assert_eq!(f.sample().position, 3.);
        assert!(matches!(
            f.events.borrow()[1],
            CarouselEvent::AfterChange(CarouselChange { from: 0, to: 3, .. })
        ));
    }

    #[test]
    fn scroll_interruption_is_continuous_and_seam_navigation_stays_in_direction() {
        let mut f = Fixture::new(CarouselOptions::default());
        f.update(|state, window, cx| state.prev(window, cx));
        f.time(100);
        let previous = f.sample().position;
        assert!(previous < 0. && previous > -1.);
        f.update(|state, window, cx| state.prev(window, cx));
        assert!((f.sample().position - previous).abs() < 1e-9);
        f.time(200);
        assert!(f.sample().position < previous);
        f.time(600);
        assert_eq!(f.sample().position, 2.);
        assert_eq!(
            f.events
                .borrow()
                .iter()
                .filter(|event| matches!(event, CarouselEvent::AfterChange(_)))
                .count(),
            1
        );
    }

    #[test]
    fn elapsed_seam_settles_before_the_next_navigation_without_a_completion_frame() {
        for (initial, first_step, settled, next_target) in [(0, -1, 3., 4.), (3, 1, 0., -1.)] {
            let mut f = Fixture::new(CarouselOptions::default());
            f.update(|state, window, cx| state.go_to(initial, true, window, cx));
            f.events.borrow_mut().clear();
            f.update(|state, window, cx| {
                state.navigate(first_step, CarouselSource::Programmatic, window, cx)
            });
            // Advance only the clock: no paint/schedule call gets an opportunity
            // to fold the previous seam coordinate back into the item range.
            f.update(|state, window, cx| {
                state.set_test_time(Some(Duration::from_millis(501)), cx);
                state.navigate(-first_step, CarouselSource::Programmatic, window, cx);
                let motion = state.motion.as_ref().unwrap();
                assert_eq!(motion.from_position, settled);
                assert_eq!(motion.to_position, next_target);
                assert_eq!((motion.to_position - motion.from_position).abs(), 1.);
            });
            let events = f.events.borrow();
            assert_eq!(events.len(), 3);
            assert!(matches!(events[0], CarouselEvent::BeforeChange(_)));
            assert!(matches!(events[1], CarouselEvent::AfterChange(_)));
            assert!(matches!(events[2], CarouselEvent::BeforeChange(_)));
        }
    }

    #[test]
    fn current_key_survives_reordering_and_tracks_removal_and_empty_content() {
        let mut f = Fixture::new(CarouselOptions::default());
        f.update(|state, window, cx| {
            assert_eq!(state.current_key().unwrap().as_ref(), "0");
            let entries = state.items().entries();
            let reordered = CarouselItems::new([
                entries[2].clone(),
                entries[0].clone(),
                entries[1].clone(),
                entries[3].clone(),
            ])
            .unwrap();
            state.set_items(reordered, window, cx);
            assert_eq!(state.current_index(), Some(1));
            assert_eq!(state.current_key().unwrap().as_ref(), "0");
            state.set_items(
                CarouselItems::new([
                    state.items().get("1").unwrap().clone(),
                    state.items().get("3").unwrap().clone(),
                ])
                .unwrap(),
                window,
                cx,
            );
            assert_eq!(state.current_index(), Some(1));
            assert_eq!(state.current_key().unwrap().as_ref(), "3");
            state.set_items(CarouselItems::default(), window, cx);
            assert_eq!(state.current_key(), None);
        });
    }

    #[test]
    fn interrupted_fade_preserves_all_nonzero_retiring_layers() {
        let mut f = Fixture::new(CarouselOptions {
            effect: CarouselEffect::Fade,
            ..Default::default()
        });
        f.update(|state, window, cx| state.next(window, cx));
        f.time(100);
        let before = f.sample().fade_layers;
        f.update(|state, window, cx| state.next(window, cx));
        let after = f.sample().fade_layers;
        for (index, opacity) in before {
            assert!((after.iter().find(|&&(i, _)| i == index).unwrap().1 - opacity).abs() < 1e-9);
        }
        assert_eq!(after.last().unwrap().0, 2);
        f.time(200);
        assert_eq!(f.sample().fade_layers.len(), 3);
        f.time(600);
        assert_eq!(f.sample().fade_layers, vec![(2, 1.)]);
    }

    #[test]
    fn fading_back_to_a_retiring_slide_preserves_its_compositing_order() {
        let mut f = Fixture::new(CarouselOptions {
            effect: CarouselEffect::Fade,
            ..Default::default()
        });
        f.update(|state, window, cx| state.next(window, cx));
        f.time(100);
        let before = f.sample().fade_layers;
        f.update(|state, window, cx| state.prev(window, cx));
        assert_eq!(
            f.sample().fade_layers,
            before,
            "retargeting preserves the full sampled composition"
        );
        f.time(200);
        let layers = f.sample().fade_layers;
        assert_eq!(
            layers.iter().map(|&(index, _)| index).collect::<Vec<_>>(),
            vec![0, 1]
        );
        assert!(layers[0].1 > before[0].1 && layers[1].1 < before[1].1);
        f.time(600);
        assert_eq!(f.sample().fade_layers, vec![(0, 1.)]);
    }

    #[test]
    fn waiting_ignores_repeated_navigation_and_immediate_changes_do_not_animate() {
        let mut f = Fixture::new(CarouselOptions {
            wait_for_animate: true,
            ..Default::default()
        });
        f.update(|state, window, cx| {
            state.next(window, cx);
            state.next(window, cx);
        });
        assert_eq!(
            f.state.read_with(&f.cx, |state, _| state.current_index()),
            Some(1)
        );
        assert_eq!(f.events.borrow().len(), 1);
        f.time(500);
        f.update(|state, window, cx| state.go_to(3, true, window, cx));
        assert_eq!(f.sample().position, 3.);
        assert!(!f.sample().animating);
        assert_eq!(f.events.borrow().len(), 4);
    }

    #[test]
    fn native_autoplay_pauses_elapsed_progress_for_hover_focus_and_explicit_pause() {
        let mut f = Fixture::new(CarouselOptions {
            autoplay: true,
            dot_duration: true,
            ..Default::default()
        });
        f.time(1000);
        assert!((f.sample().autoplay_progress - 1. / 3.).abs() < 1e-9);
        f.update(|state, window, cx| state.set_hovered(true, window, cx));
        f.time(2000);
        assert!((f.sample().autoplay_progress - 1. / 3.).abs() < 1e-9);
        f.update(|state, window, cx| state.set_hovered(false, window, cx));
        f.time(3000);
        assert!((f.sample().autoplay_progress - 2. / 3.).abs() < 1e-9);
        f.update(|state, window, cx| state.set_focused(true, window, cx));
        f.time(4000);
        assert!((f.sample().autoplay_progress - 2. / 3.).abs() < 1e-9);
        f.update(|state, window, cx| {
            state.set_focused(false, window, cx);
            state.pause(window, cx);
        });
        f.time(5000);
        assert!((f.sample().autoplay_progress - 2. / 3.).abs() < 1e-9);
        f.update(|state, window, cx| state.resume(window, cx));
        f.time(5500);
        assert!((f.sample().autoplay_progress - 5. / 6.).abs() < 1e-9);
        assert!(
            f.state
                .read_with(&f.cx, |state, _| state.autoplay_task.is_none()
                    && !state.frame_pending)
        );
    }

    #[test]
    fn typed_content_notifications_invalidate_only_their_own_measurement() {
        let mut f = Fixture::new(CarouselOptions::default());
        let mut child = None;
        f.update(|state, window, cx| {
            let view = cx.new(|_| Empty);
            child = Some(view.clone());
            let items =
                CarouselItems::new([CarouselSlide::new("tracked").view(move |_, _| view.clone())])
                    .unwrap();
            state.set_items(items, window, cx);
        });
        f.cx.update_window(f.window.into(), |_, window, cx| {
            CarouselState::ensure_view(&f.state, 0, window, cx);
        })
        .unwrap();
        let generation = f
            .state
            .read_with(&f.cx, |state, _| state.measurement_generation);
        child.unwrap().update(&mut f.cx, |_, cx| cx.notify());
        f.cx.run_until_parked();
        assert_eq!(
            f.state.read_with(&f.cx, |state, _| state
                .measurement_revisions
                .get("tracked")
                .copied()),
            Some(1)
        );
        assert_eq!(
            f.state
                .read_with(&f.cx, |state, _| state.measurement_generation),
            generation
        );
    }

    #[test]
    fn unmount_discards_pending_motion_gestures_and_wakeup_sources() {
        let mut f = Fixture::new(CarouselOptions {
            autoplay: true,
            ..Default::default()
        });
        f.update(|state, window, cx| {
            state.next(window, cx);
            state.unmount(state.generation);
        });
        f.time(1000);
        assert_eq!(f.sample().position, 1.);
        assert!(!f.state.read_with(&f.cx, |state, _| state.mounted
            || state.frame_pending
            || state.autoplay_task.is_some()
            || state.drag.is_some()));
        assert_eq!(f.events.borrow().len(), 1);
    }

    #[test]
    fn short_drag_snaps_back_without_change_events() {
        let mut f = Fixture::new(CarouselOptions {
            draggable: true,
            ..Default::default()
        });
        f.update(|state, window, cx| {
            state.bounds = Some(Bounds::new(
                gpui::point(gpui::px(0.), gpui::px(0.)),
                gpui::size(gpui::px(400.), gpui::px(160.)),
            ));
            assert!(state.pointer_start(gpui::point(gpui::px(200.), gpui::px(80.)), window, cx));
            assert!(state.pointer_move(gpui::point(gpui::px(180.), gpui::px(80.)), window, cx));
            assert!(state.pointer_end(window, cx));
        });
        assert_eq!(
            f.state.read_with(&f.cx, |state, _| state.current_index()),
            Some(0)
        );
        assert!(f.sample().animating);
        assert!(f.sample().position > 0.);
        f.time(500);
        assert_eq!(f.sample().position, 0.);
        assert!(f.events.borrow().is_empty());
    }

    #[test]
    fn drag_deadzone_threshold_and_finite_edge_friction_match_the_reference() {
        for (distance, expected) in [(79., 0), (80., 1)] {
            let mut f = Fixture::new(CarouselOptions {
                draggable: true,
                infinite: false,
                ..Default::default()
            });
            f.update(|state, window, cx| {
                state.bounds = Some(Bounds::new(
                    gpui::point(gpui::px(0.), gpui::px(0.)),
                    gpui::size(gpui::px(400.), gpui::px(160.)),
                ));
                let start = gpui::point(gpui::px(200.), gpui::px(80.));
                assert!(state.pointer_start(start, window, cx));
                assert!(!state.pointer_move(
                    start - gpui::point(gpui::px(9.), gpui::px(0.)),
                    window,
                    cx
                ));
                assert!(!state.drag.as_ref().unwrap().active);
                assert!(state.pointer_move(
                    start - gpui::point(gpui::px(10.), gpui::px(0.)),
                    window,
                    cx
                ));
                assert!(state.drag.as_ref().unwrap().active);
                state.pointer_move(
                    start - gpui::point(gpui::px(distance), gpui::px(0.)),
                    window,
                    cx,
                );
                assert!(state.pointer_end(window, cx));
                assert_eq!(state.current_index(), Some(expected));
            });
        }
        let mut f = Fixture::new(CarouselOptions {
            draggable: true,
            infinite: false,
            ..Default::default()
        });
        f.update(|state, window, cx| {
            state.bounds = Some(Bounds::new(
                gpui::point(gpui::px(0.), gpui::px(0.)),
                gpui::size(gpui::px(400.), gpui::px(160.)),
            ));
            assert!(state.pointer_start(gpui::point(gpui::px(200.), gpui::px(80.)), window, cx));
            assert!(state.pointer_move(gpui::point(gpui::px(300.), gpui::px(80.)), window, cx));
            assert!((state.sample(cx).position + 0.25 * 0.35).abs() < 1e-9);
            assert!(state.pointer_end(window, cx));
            assert_eq!(state.current_index(), Some(0));
        });
        f.time(500);
        assert_eq!(f.sample().position, 0.);
        assert!(f.events.borrow().is_empty());
    }

    #[test]
    fn countdown_begins_at_transition_acceptance_and_continues_during_motion() {
        let mut f = Fixture::new(CarouselOptions {
            autoplay: true,
            ..Default::default()
        });
        f.update(|state, window, cx| state.next(window, cx));
        f.time(250);
        assert!(f.sample().animating);
        assert!((f.sample().autoplay_progress - 250. / 3000.).abs() < 1e-9);
        f.time(500);
        assert!(!f.sample().animating);
        assert!((f.sample().autoplay_progress - 500. / 3000.).abs() < 1e-9);
    }

    #[test]
    fn inherited_disabled_suppresses_all_user_sources_but_preserves_owner_control() {
        let mut f = Fixture::new(CarouselOptions {
            draggable: true,
            ..Default::default()
        });
        f.update(|state, window, cx| {
            state
                .scope
                .clone()
                .unwrap()
                .update_patch(cx, |patch| patch.disabled = Some(true))
                .unwrap();
            for source in [
                CarouselSource::Arrow,
                CarouselSource::Dot,
                CarouselSource::Keyboard,
                CarouselSource::Drag,
                CarouselSource::Autoplay,
                CarouselSource::Accessibility,
            ] {
                state.activate(1, source, true, window, cx);
                assert_eq!(state.current_index(), Some(0));
            }
            state.bounds = Some(Bounds::new(
                gpui::point(gpui::px(0.), gpui::px(0.)),
                gpui::size(gpui::px(400.), gpui::px(160.)),
            ));
            assert!(!state.pointer_start(gpui::point(gpui::px(100.), gpui::px(80.)), window, cx));
            state.go_to(2, true, window, cx);
        });
        assert_eq!(
            f.state.read_with(&f.cx, |state, _| state.current_index()),
            Some(2)
        );
        assert_eq!(f.events.borrow().len(), 2);
        assert!(f.events.borrow().iter().all(|event| match event {
            CarouselEvent::BeforeChange(change) | CarouselEvent::AfterChange(change) =>
                change.source == CarouselSource::Programmatic,
        }));
    }

    #[test]
    fn only_changed_initial_slide_configuration_resynchronizes_selection() {
        let mut f = Fixture::new(CarouselOptions::default());
        f.update(|state, window, cx| state.go_to(2, true, window, cx));
        f.update(|state, window, cx| {
            state.configure(
                state.options.clone(),
                state.scope.clone().unwrap(),
                state.on_event.clone(),
                window,
                cx,
            )
        });
        assert_eq!(
            f.state.read_with(&f.cx, |state, _| state.current_index()),
            Some(2)
        );
        let before = f.events.borrow().len();
        f.update(|state, window, cx| {
            let mut options = state.options.clone();
            options.initial_slide = 3;
            state.configure(
                options,
                state.scope.clone().unwrap(),
                state.on_event.clone(),
                window,
                cx,
            );
        });
        assert_eq!(
            f.state.read_with(&f.cx, |state, _| state.current_index()),
            Some(3)
        );
        assert_eq!(f.sample().position, 3.);
        assert!(!f.sample().animating);
        assert_eq!(f.events.borrow().len(), before);
    }
}
