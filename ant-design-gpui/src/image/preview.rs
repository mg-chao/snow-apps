// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::{
    ImageInfo, ImageStyleContext, ImageStyles, ImageTheme, PreviewAction, PreviewEvent,
    PreviewItem, PreviewOptions, PreviewRenderInfo, PreviewTransform,
    picture::{Picture, PictureState, PictureStatus},
};
use crate::{
    ConfigScope, Modal, ModalState,
    behavior::{
        Accessibility, ActivationGuard, Availability, FocusRing, MotionPolicy, ScalarTransition,
        focus_handle,
    },
    icons::{IconName, IconRef, icon},
    modal::{ModalAction, ModalFooter, ModalLength, ModalOptions, ModalStyles, ModalWidth},
};
use gpui::{
    AnyElement, App, AppContext, Bounds, Context, ElementId, Entity, EventEmitter, FocusHandle,
    ImageSource, IntoElement, MouseButton, MouseMoveEvent, MouseUpEvent, ParentElement, Pixels,
    Point, Refineable, RenderOnce, Role, ScrollSource, SharedString, Size, StyleRefinement, Styled,
    Subscription, TouchPhase, Window, canvas, div, point, prelude::*, px, radians, rgba, size,
};
use std::{
    rc::Rc,
    time::{Duration, Instant},
};

/// A preview collection and its native dialog lifetime. Setters are silent; user
/// requests emit typed events and honor the corresponding controlled option.
pub struct PreviewState {
    scope: ConfigScope,
    items: Vec<PreviewItem>,
    items_explicit: bool,
    registered: Vec<PreviewItem>,
    options: PreviewOptions,
    scope_defaults: bool,
    open: bool,
    current: usize,
    transform: PreviewTransform,
    dialog: Entity<ModalState>,
    picture: Entity<PictureState>,
    operation_focus: [FocusHandle; 9],
    viewport: Size<Pixels>,
    drag: Option<Drag>,
    touch_pan: Option<PreviewTransform>,
    pinch_origin: Option<PreviewTransform>,
    mask_down: bool,
    open_origin: Bounds<Pixels>,
    opening: Option<Instant>,
    closing: Option<Instant>,
    visual: VisualMotion,
    frame_pending: bool,
    last_action: Option<PreviewAction>,
    mount_generation: u64,
    scope_subscription: Subscription,
    _picture_subscription: Subscription,
}

#[derive(Clone, Copy)]
struct Drag {
    origin: Point<Pixels>,
    translation: Point<Pixels>,
    moved: bool,
}

impl EventEmitter<PreviewEvent> for PreviewState {}

impl PreviewState {
    pub fn new(items: Vec<PreviewItem>, window: &mut Window, cx: &mut Context<Self>) -> Self {
        let scope = ConfigScope::application(cx);
        let scope_subscription = scope.observe(cx);
        let picture = cx.new(|cx| PictureState::new(None, cx));
        let picture_subscription = cx.observe(&picture, |_, _, cx| cx.notify());
        let keys = cx.weak_entity();
        let items_explicit = !items.is_empty();
        Self {
            scope,
            items,
            items_explicit,
            registered: Vec::new(),
            options: PreviewOptions::default(),
            scope_defaults: true,
            open: false,
            current: 0,
            transform: PreviewTransform::default(),
            dialog: cx.new(|cx| {
                let mut dialog = ModalState::new(window, cx);
                dialog.use_viewport_surface(move |event, w, cx| {
                    if event.keystroke.modifiers.modified() {
                        return;
                    }
                    let action = match event.keystroke.key.as_str() {
                        "left" => Some(PreviewAction::Previous),
                        "right" => Some(PreviewAction::Next),
                        _ => None,
                    };
                    if let Some(action) = action {
                        let _ = keys.update(cx, |s, cx| s.apply_action(action, w, cx));
                        cx.stop_propagation();
                    }
                });
                dialog
            }),
            picture,
            operation_focus: std::array::from_fn(|_| focus_handle(cx)),
            viewport: window.viewport_size(),
            drag: None,
            touch_pan: None,
            pinch_origin: None,
            mask_down: false,
            open_origin: Bounds::default(),
            opening: None,
            closing: None,
            visual: VisualMotion::new(cx.background_executor().now()),
            frame_pending: false,
            last_action: None,
            mount_generation: 0,
            scope_subscription,
            _picture_subscription: picture_subscription,
        }
    }

    pub fn scope(&self) -> &ConfigScope {
        &self.scope
    }
    pub fn options(&self) -> &PreviewOptions {
        &self.options
    }
    pub fn is_open(&self) -> bool {
        self.open
    }
    pub fn current(&self) -> usize {
        self.current
    }
    pub fn transform(&self) -> PreviewTransform {
        self.transform
    }
    pub fn items(&self) -> &[PreviewItem] {
        if self.items_explicit {
            &self.items
        } else {
            &self.registered
        }
    }
    pub fn current_item(&self) -> Option<&PreviewItem> {
        self.items().get(self.current)
    }
    pub fn picture(&self) -> Entity<PictureState> {
        self.picture.clone()
    }
    /// Bounds of the fitted image before the user's transform, in window coordinates.
    pub fn image_bounds(&self, cx: &App) -> Bounds<Pixels> {
        Bounds::centered_at(
            point(self.viewport.width / 2., self.viewport.height / 2.),
            self.fitted_size(cx),
        )
    }
    pub fn set_open_origin(&mut self, bounds: Bounds<Pixels>, _: &mut Context<Self>) {
        self.open_origin = bounds;
    }

    pub fn set_scope(&mut self, scope: ConfigScope, cx: &mut Context<Self>) {
        if scope != self.scope {
            self.scope_subscription = scope.observe(cx);
            self.scope = scope.clone();
            self.dialog.update(cx, |s, cx| s.set_scope(scope, cx));
            cx.notify();
        }
    }

    pub fn set_items(&mut self, items: Vec<PreviewItem>, cx: &mut Context<Self>) {
        if self.items_explicit && same_items(&self.items, &items) {
            return;
        }
        let selected = self.current_item().map(|i| i.id.clone());
        self.items = items;
        self.items_explicit = true;
        self.reconcile(selected, cx);
        cx.notify();
    }

    pub fn register_item(&mut self, item: PreviewItem, cx: &mut Context<Self>) {
        let was_empty = self.items().is_empty();
        if let Some(index) = self.registered.iter().position(|i| i.id == item.id) {
            if same_item(&self.registered[index], &item) {
                return;
            }
            self.registered[index] = item;
        } else {
            self.registered.push(item);
        }
        if !self.items_explicit {
            if was_empty {
                self.reconcile(None, cx);
            } else if self.open {
                self.load_current(cx);
            }
        }
        cx.notify();
    }

    pub fn unregister_item(&mut self, id: &str, cx: &mut Context<Self>) {
        let selected = self.current_item().map(|i| i.id.clone());
        let old = self.registered.len();
        self.registered.retain(|i| i.id.as_ref() != id);
        if old != self.registered.len() {
            if !self.items_explicit {
                self.reconcile(selected, cx);
            }
            cx.notify();
        }
    }

    /// Stop supplying an explicit collection and preview registered thumbnails.
    pub fn use_registered_items(&mut self, cx: &mut Context<Self>) {
        if !self.items_explicit {
            return;
        }
        let selected = self.current_item().map(|item| item.id.clone());
        self.items.clear();
        self.items_explicit = false;
        self.reconcile(selected, cx);
        cx.notify();
    }

    fn reconcile(&mut self, selected: Option<SharedString>, cx: &mut Context<Self>) {
        self.current = selected
            .and_then(|id| self.items().iter().position(|i| i.id == id))
            .unwrap_or_else(|| self.current.min(self.items().len().saturating_sub(1)));
        self.transform = PreviewTransform::default();
        self.visual
            .snap(self.transform, cx.background_executor().now());
        self.drag = None;
        self.touch_pan = None;
        self.pinch_origin = None;
        let previous_open = self.open;
        if self.items().is_empty() {
            self.open = false;
        } else if let Some(open) = self.options.controlled_open {
            self.open = open && self.options.enabled;
        }
        if self.open && !previous_open {
            self.opening = Some(cx.background_executor().now());
            self.closing = None;
        }
        self.load_current(cx);
    }

    pub fn set_options(&mut self, mut options: PreviewOptions, cx: &mut Context<Self>) {
        normalize_options(&mut options);
        let inherited = self.scope_defaults;
        self.scope_defaults = false;
        if self.options == options {
            if inherited {
                cx.notify();
            }
            return;
        }
        self.replace_options(options, cx);
        cx.notify();
    }

    fn replace_options(&mut self, mut options: PreviewOptions, cx: &mut Context<Self>) {
        normalize_options(&mut options);
        if let Some(current) = options.controlled_current {
            self.set_current(current, cx);
        }
        if let Some(open) = options.controlled_open {
            let open = open && options.enabled && !self.items().is_empty();
            if self.open != open {
                self.open = open;
                self.drag = None;
                self.touch_pan = None;
                self.pinch_origin = None;
                if open {
                    self.transform = PreviewTransform::default();
                    self.transform.scale = options.min_scale;
                    self.visual
                        .snap(self.transform, cx.background_executor().now());
                    self.load_current(cx);
                    self.opening = Some(cx.background_executor().now());
                    self.closing = None;
                } else {
                    self.transform = PreviewTransform::default();
                    self.transform.scale = options.min_scale;
                    self.closing = Some(cx.background_executor().now());
                    self.opening = None;
                }
            }
        }
        self.transform.scale = self
            .transform
            .scale
            .clamp(options.min_scale, options.max_scale);
        self.options = options;
        if !self.options.movable {
            self.drag = None;
            self.touch_pan = None;
        }
        if !self.options.enabled {
            self.open = false;
            self.drag = None;
            self.touch_pan = None;
            self.pinch_origin = None;
            self.opening = None;
        }
    }

    fn refresh_scope_options(&mut self, cx: &mut Context<Self>) {
        if !self.scope_defaults {
            return;
        }
        let config = self.scope.snapshot(cx);
        let inherited = &config.image;
        let mut options = self.options.clone();
        options.enabled = inherited.preview_enabled;
        options.mask = inherited.mask;
        options.mask_closable = inherited.mask_closable;
        options.focus_trap = inherited.focus_trap;
        options.wheel = inherited.wheel;
        options.movable = inherited.movable;
        options.close_icon = inherited.close_icon.clone();
        self.replace_options(options, cx);
    }

    pub fn set_open(&mut self, open: bool, window: &mut Window, cx: &mut Context<Self>) {
        let open = open && self.options.enabled && !self.items().is_empty();
        if self.open == open {
            return;
        }
        self.open = open;
        self.drag = None;
        self.touch_pan = None;
        self.pinch_origin = None;
        self.mask_down = false;
        if open {
            self.transform = PreviewTransform::default();
            self.transform.scale = self.options.min_scale;
            self.visual
                .snap(self.transform, cx.background_executor().now());
            self.load_current(cx);
            self.opening = Some(cx.background_executor().now());
            self.closing = None;
        } else {
            self.transform = PreviewTransform::default();
            self.transform.scale = self.options.min_scale;
            self.opening = None;
            self.closing = Some(cx.background_executor().now());
        }
        self.dialog.update(cx, |s, cx| s.set_open(open, window, cx));
        cx.notify();
    }

    pub fn set_current(&mut self, current: usize, cx: &mut Context<Self>) {
        let current = current.min(self.items().len().saturating_sub(1));
        if self.current == current {
            return;
        }
        self.current = current;
        self.transform = PreviewTransform::default();
        self.transform.scale = self.options.min_scale;
        self.visual
            .snap(self.transform, cx.background_executor().now());
        self.opening = None;
        self.drag = None;
        self.touch_pan = None;
        self.pinch_origin = None;
        self.load_current(cx);
        cx.notify();
    }

    pub fn request_open(&mut self, current: usize, window: &mut Window, cx: &mut Context<Self>) {
        if !self.options.enabled || self.items().is_empty() {
            return;
        }
        let requested = current.min(self.items().len() - 1);
        if requested != self.current {
            self.request_current(requested, window, cx);
        }
        if self.options.controlled_open.is_none() {
            self.set_open(true, window, cx);
        }
        self.emit(
            PreviewEvent::OpenChange {
                open: true,
                current: current.min(self.items().len() - 1),
            },
            window,
            cx,
        );
    }

    pub fn request_item(&mut self, id: &str, window: &mut Window, cx: &mut Context<Self>) {
        let index = self
            .items()
            .iter()
            .position(|i| i.id.as_ref() == id)
            .or_else(|| {
                let registered = self.registered.iter().find(|i| i.id.as_ref() == id)?;
                self.items()
                    .iter()
                    .position(|i| same_source(&i.source, &registered.source))
                    .or_else(|| (!self.items().is_empty()).then_some(0))
            });
        if let Some(index) = index {
            self.request_open(index, window, cx);
        }
    }

    pub fn request_close(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if !self.open {
            return;
        }
        if self.options.controlled_open.is_none() {
            self.set_open(false, window, cx);
        }
        self.emit(
            PreviewEvent::OpenChange {
                open: false,
                current: self.current,
            },
            window,
            cx,
        );
    }

    pub fn request_current(&mut self, current: usize, window: &mut Window, cx: &mut Context<Self>) {
        if current >= self.items().len() || current == self.current {
            return;
        }
        let previous = self.current;
        if self.options.controlled_current.is_none() {
            self.set_current(current, cx);
        }
        self.emit(PreviewEvent::Change { current, previous }, window, cx);
    }

    fn load_current(&mut self, cx: &mut Context<Self>) {
        let item = self.current_item().cloned();
        self.picture.update(cx, |p, cx| {
            p.set_source(item.as_ref().map(|i| i.source.clone()), cx);
            p.set_fallback(item.and_then(|i| i.fallback), cx);
        });
    }

    fn emit(&self, event: PreviewEvent, window: &mut Window, cx: &mut Context<Self>) {
        cx.emit(event.clone());
        if let Some(handler) = self.options.on_event.clone() {
            // Application callbacks may update this entity. Dispatch after the
            // current borrow ends, exactly as the other retained controls do.
            window.defer(cx, move |w, cx| handler(&event, w, cx));
        }
    }

    pub fn apply_action(
        &mut self,
        action: PreviewAction,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if !self.open {
            return;
        }
        match action {
            PreviewAction::Close => {
                self.request_close(window, cx);
                return;
            }
            PreviewAction::Previous => {
                self.request_current(self.current.saturating_sub(1), window, cx);
                return;
            }
            PreviewAction::Next => {
                self.request_current(self.current.saturating_add(1), window, cx);
                return;
            }
            PreviewAction::FlipY => self.transform.flip_y = !self.transform.flip_y,
            PreviewAction::FlipX => self.transform.flip_x = !self.transform.flip_x,
            PreviewAction::RotateLeft => self.transform.rotate -= 90.,
            PreviewAction::RotateRight => self.transform.rotate += 90.,
            PreviewAction::ZoomOut => self.zoom(
                self.transform.scale / (1. + self.options.scale_step),
                None,
                cx,
            ),
            PreviewAction::ZoomIn => self.zoom(
                self.transform.scale * (1. + self.options.scale_step),
                None,
                cx,
            ),
            PreviewAction::Reset => {
                self.transform = PreviewTransform::default();
                self.transform.scale = self.options.min_scale;
            }
            _ => return,
        }
        self.changed(action, window, cx);
    }

    fn changed(&mut self, action: PreviewAction, window: &mut Window, cx: &mut Context<Self>) {
        self.last_action = Some(action);
        self.emit(
            PreviewEvent::Transform {
                transform: self.transform,
                action,
            },
            window,
            cx,
        );
        cx.notify();
    }

    fn fitted_size(&self, cx: &App) -> Size<Pixels> {
        let intrinsic = self
            .picture
            .read(cx)
            .natural_size()
            .unwrap_or(size(px(200.), px(200.)));
        fit(intrinsic, self.viewport)
    }

    fn zoom(&mut self, scale: f32, pointer: Option<Point<Pixels>>, cx: &App) {
        let old = self.transform.scale;
        let next = scale.clamp(self.options.min_scale, self.options.max_scale);
        let pointer = pointer.unwrap_or(point(self.viewport.width / 2., self.viewport.height / 2.));
        {
            let x = f32::from(pointer.x - self.viewport.width / 2.);
            let y = f32::from(pointer.y - self.viewport.height / 2.);
            let ratio = next / old;
            self.transform.x = x - (x - self.transform.x) * ratio;
            self.transform.y = y - (y - self.transform.y) * ratio;
        }
        self.transform.scale = next;
        let (width, height) = rotated_extent(self.fitted_size(cx), self.transform);
        if next <= self.options.min_scale
            && width <= f32::from(self.viewport.width)
            && height <= f32::from(self.viewport.height)
        {
            self.transform.x = 0.;
            self.transform.y = 0.;
        }
    }

    fn wheel(
        &mut self,
        delta: f32,
        pointer: Point<Pixels>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if !self.open || !self.options.wheel || !delta.is_finite() || delta == 0. {
            return;
        }
        let factor = 1. + (delta.abs() / 100.).min(1.) * self.options.scale_step;
        let next = if delta > 0. {
            self.transform.scale * factor
        } else {
            self.transform.scale / factor
        };
        self.zoom(next, Some(pointer), cx);
        self.changed(PreviewAction::Wheel, window, cx);
    }

    fn double_click(
        &mut self,
        pointer: Point<Pixels>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let scale = if self.transform.scale > self.options.min_scale {
            self.options.min_scale
        } else {
            self.options.min_scale * (1. + self.options.scale_step)
        };
        self.zoom(scale, Some(pointer), cx);
        self.changed(PreviewAction::DoubleClick, window, cx);
    }

    fn pointer_move(
        &mut self,
        pointer: Point<Pixels>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let Some(drag) = &mut self.drag else {
            return;
        };
        let delta = pointer - drag.origin;
        drag.moved |= delta.x.abs() > px(2.) || delta.y.abs() > px(2.);
        self.transform.x = f32::from(drag.translation.x + delta.x);
        self.transform.y = f32::from(drag.translation.y + delta.y);
        self.changed(PreviewAction::Move, window, cx);
    }

    fn pointer_end(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let Some(drag) = self.drag.take() else {
            return;
        };
        if !drag.moved {
            cx.notify();
            return;
        }
        self.rebound(window, cx);
    }

    fn rebound(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let image = self.fitted_size(cx);
        let (width, height) = rotated_extent(image, self.transform);
        self.transform.x = rebound(self.transform.x, width, f32::from(self.viewport.width));
        self.transform.y = rebound(self.transform.y, height, f32::from(self.viewport.height));
        self.changed(PreviewAction::DragRebound, window, cx);
    }

    fn touch_scroll(
        &mut self,
        event: &gpui::ScrollEvent,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if !self.open {
            return;
        }
        if event.touch_phase == TouchPhase::Cancelled {
            if let Some(origin) = self.touch_pan.take() {
                self.transform = origin;
                self.changed(PreviewAction::DragRebound, window, cx);
            }
            return;
        }
        if !self.options.movable {
            return;
        }
        let delta = event.delta.pixel_delta(px(16.));
        if !f32::from(delta.x).is_finite() || !f32::from(delta.y).is_finite() {
            return;
        }
        match event.touch_phase {
            TouchPhase::Started | TouchPhase::Moved => {
                if self.touch_pan.is_none() {
                    if !contains(
                        self.fitted_size(cx),
                        self.viewport,
                        self.transform,
                        event.position,
                    ) {
                        return;
                    }
                    self.touch_pan = Some(self.transform);
                    self.drag = None;
                    self.pinch_origin = None;
                    self.mask_down = false;
                }
                self.transform.x += f32::from(delta.x);
                self.transform.y += f32::from(delta.y);
                self.changed(PreviewAction::Move, window, cx);
            }
            TouchPhase::Ended => {
                if self.touch_pan.take().is_some() {
                    self.transform.x += f32::from(delta.x);
                    self.transform.y += f32::from(delta.y);
                    self.rebound(window, cx);
                }
            }
            TouchPhase::Cancelled => {}
        }
    }

    fn pinch(&mut self, event: &gpui::PinchEvent, window: &mut Window, cx: &mut Context<Self>) {
        if !self.open {
            return;
        }
        if event.phase == TouchPhase::Cancelled {
            if let Some(origin) = self.pinch_origin.take() {
                self.transform = origin;
                self.changed(PreviewAction::Pinch, window, cx);
            }
            return;
        }
        if !event.delta.is_finite() {
            return;
        }
        if self.pinch_origin.is_none() {
            self.pinch_origin = Some(self.transform);
            self.drag = None;
            self.touch_pan = None;
            self.mask_down = false;
        }
        if event.delta != 0. {
            self.zoom(
                self.transform.scale * (1. + event.delta).max(f32::EPSILON),
                Some(event.position),
                cx,
            );
            self.changed(PreviewAction::Pinch, window, cx);
        }
        if event.phase == TouchPhase::Ended {
            self.pinch_origin = None;
            self.rebound(window, cx);
        }
    }

    fn visual_transform(&mut self, window: &mut Window, cx: &mut Context<Self>) -> [f32; 5] {
        let config = self.scope.snapshot(cx);
        let tokens = config.theme.component::<ImageTheme>();
        let globals = tokens.globals();
        let now = cx.background_executor().now();
        let policy = MotionPolicy::new(config.motion && globals.motion, cx.reduce_motion());
        let (mut result, mut moving) = self.visual.sample(now, globals.motion_ease_out);
        let target = pose(self.transform);
        if self.visual.target != target {
            self.visual.from = result;
            self.visual.target = target;
            self.visual.started = now;
            self.visual.duration = if policy.allows_motion()
                && (self.closing.is_some()
                    || (self.drag.is_none()
                        && !matches!(
                            self.last_action,
                            Some(PreviewAction::Wheel | PreviewAction::Move | PreviewAction::Pinch)
                        ))) {
                globals.motion_duration_slow
            } else {
                Duration::ZERO
            };
            (result, moving) = self.visual.sample(now, globals.motion_ease_out);
        }
        if let Some(start) = self.opening.or(self.closing) {
            let elapsed = now.saturating_duration_since(start);
            let duration = globals.motion_duration_slow;
            if policy.allows_motion() && elapsed < duration && !duration.is_zero() {
                let progress = crate::behavior::evaluate_cubic_bezier(
                    crate::theme::CubicBezier::new(0.25, 0.1, 0.25, 1.),
                    elapsed.as_secs_f64() / duration.as_secs_f64(),
                ) as f32;
                let progress = if self.opening.is_some() {
                    progress
                } else {
                    1. - progress
                };
                result[3] *= progress;
                result[4] *= progress;
                result[0] *= progress;
                result[1] *= progress;
                if !self.open_origin.is_empty() {
                    result[0] += f32::from(self.open_origin.center().x - self.viewport.width / 2.)
                        * (1. - progress);
                    result[1] += f32::from(self.open_origin.center().y - self.viewport.height / 2.)
                        * (1. - progress);
                }
                moving = true;
            } else {
                self.opening = None;
                self.closing = None;
            }
        }
        if !policy.allows_motion() {
            result = target;
            moving = false;
            self.visual.snap(self.transform, now);
        }
        if moving {
            self.frame(window, cx);
        }
        result
    }
    fn frame(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if !self.frame_pending && window.is_visible() {
            self.frame_pending = true;
            let weak = cx.weak_entity();
            window.on_next_frame(move |_, cx| {
                let _ = weak.update(cx, |s, cx| {
                    s.frame_pending = false;
                    cx.notify();
                });
            });
            window.request_animation_frame();
        }
    }
}

struct VisualMotion {
    from: [f32; 5],
    target: [f32; 5],
    started: Instant,
    duration: Duration,
}
impl VisualMotion {
    fn new(now: Instant) -> Self {
        Self {
            from: pose(PreviewTransform::default()),
            target: pose(PreviewTransform::default()),
            started: now,
            duration: Duration::ZERO,
        }
    }
    fn snap(&mut self, transform: PreviewTransform, now: Instant) {
        self.from = pose(transform);
        self.target = self.from;
        self.started = now;
        self.duration = Duration::ZERO;
    }
    fn sample(&self, now: Instant, curve: crate::theme::CubicBezier) -> ([f32; 5], bool) {
        let elapsed = now.saturating_duration_since(self.started);
        if self.duration.is_zero() || elapsed >= self.duration {
            return (self.target, false);
        }
        let progress = crate::behavior::evaluate_cubic_bezier(
            curve,
            elapsed.as_secs_f64() / self.duration.as_secs_f64(),
        ) as f32;
        let mix = |a: f32, b: f32| a + (b - a) * progress;
        (
            std::array::from_fn(|i| mix(self.from[i], self.target[i])),
            true,
        )
    }
}

fn pose(t: PreviewTransform) -> [f32; 5] {
    [
        t.x,
        t.y,
        t.rotate,
        t.scale * if t.flip_x { -1. } else { 1. },
        t.scale * if t.flip_y { -1. } else { 1. },
    ]
}
fn pose_matrix(p: [f32; 5], extent: Size<Pixels>) -> [f32; 6] {
    let (sin, cos) = p[2].to_radians().sin_cos();
    let (a, b, c, d) = (p[3] * cos, p[4] * sin, -p[3] * sin, p[4] * cos);
    let (x, y) = (f32::from(extent.width) / 2., f32::from(extent.height) / 2.);
    [
        a,
        b,
        c,
        d,
        x + p[0] - a * x - c * y,
        y + p[1] - b * x - d * y,
    ]
}

fn positive(value: f32, fallback: f32) -> f32 {
    if value.is_finite() && value > 0. {
        value
    } else {
        fallback
    }
}
fn normalize_options(options: &mut PreviewOptions) {
    options.min_scale = positive(options.min_scale, 1.);
    options.max_scale = positive(options.max_scale, 50.).max(options.min_scale);
    options.scale_step = positive(options.scale_step, 0.5);
}

fn fit(image: Size<Pixels>, viewport: Size<Pixels>) -> Size<Pixels> {
    let width = f32::from(image.width).max(1.);
    let height = f32::from(image.height).max(1.);
    let scale = (f32::from(viewport.width) / width)
        .min(f32::from(viewport.height) * 0.7 / height)
        .clamp(0., 1.);
    size(px(width * scale), px(height * scale))
}

fn rotated_extent(image: Size<Pixels>, transform: PreviewTransform) -> (f32, f32) {
    let angle = transform.rotate.to_radians();
    let (sine, cosine) = (angle.sin().abs(), angle.cos().abs());
    let (width, height) = (f32::from(image.width), f32::from(image.height));
    (
        (width * cosine + height * sine) * transform.scale,
        (width * sine + height * cosine) * transform.scale,
    )
}

fn rebound(translation: f32, extent: f32, viewport: f32) -> f32 {
    if extent <= viewport {
        0.
    } else {
        translation.clamp((viewport - extent) / 2., (extent - viewport) / 2.)
    }
}

fn contains(
    image: Size<Pixels>,
    viewport: Size<Pixels>,
    transform: PreviewTransform,
    pointer: Point<Pixels>,
) -> bool {
    let x = f32::from(pointer.x - viewport.width / 2.) - transform.x;
    let y = f32::from(pointer.y - viewport.height / 2.) - transform.y;
    let x = x / (transform.scale * if transform.flip_x { -1. } else { 1. });
    let y = y / (transform.scale * if transform.flip_y { -1. } else { 1. });
    let (sine, cosine) = (-transform.rotate).to_radians().sin_cos();
    (x * cosine - y * sine).abs() <= f32::from(image.width) / 2.
        && (x * sine + y * cosine).abs() <= f32::from(image.height) / 2.
}

fn same_source(a: &ImageSource, b: &ImageSource) -> bool {
    match (a, b) {
        (ImageSource::Resource(a), ImageSource::Resource(b)) => a == b,
        (ImageSource::Render(a), ImageSource::Render(b)) => a.id == b.id,
        (ImageSource::Image(a), ImageSource::Image(b)) => a.id == b.id,
        (ImageSource::Custom(a), ImageSource::Custom(b)) => std::sync::Arc::ptr_eq(a, b),
        _ => false,
    }
}
fn same_item(a: &PreviewItem, b: &PreviewItem) -> bool {
    a.id == b.id
        && a.alt == b.alt
        && same_source(&a.source, &b.source)
        && match (&a.fallback, &b.fallback) {
            (Some(a), Some(b)) => same_source(a, b),
            (None, None) => true,
            _ => false,
        }
}
fn same_items(a: &[PreviewItem], b: &[PreviewItem]) -> bool {
    a.len() == b.len() && a.iter().zip(b).all(|(a, b)| same_item(a, b))
}

/// Collection wrapper. It owns no layout styling and mounts one retained preview
/// shell alongside its children. Nested wrappers have independent controllers.
#[derive(IntoElement)]
pub struct PreviewGroup {
    id: ElementId,
    state: Entity<PreviewState>,
    scope: Option<ConfigScope>,
    options: Option<PreviewOptions>,
    children: Vec<AnyElement>,
    style: StyleRefinement,
}
impl PreviewGroup {
    pub fn new(id: impl Into<ElementId>, state: &Entity<PreviewState>) -> Self {
        Self {
            id: id.into(),
            state: state.clone(),
            scope: None,
            options: None,
            children: Vec::new(),
            style: StyleRefinement::default(),
        }
    }
    pub fn scope(mut self, scope: ConfigScope) -> Self {
        self.scope = Some(scope);
        self
    }
    pub fn options(mut self, options: PreviewOptions) -> Self {
        self.options = Some(options);
        self
    }
}
impl ParentElement for PreviewGroup {
    fn extend(&mut self, children: impl IntoIterator<Item = AnyElement>) {
        self.children.extend(children);
    }
}
impl Styled for PreviewGroup {
    fn style(&mut self) -> &mut StyleRefinement {
        &mut self.style
    }
}
struct GroupMount {
    _observer: Subscription,
    _release: Subscription,
}
impl RenderOnce for PreviewGroup {
    fn render(self, window: &mut Window, cx: &mut App) -> impl IntoElement {
        let observed = self.state.clone();
        let _mount = window.with_element_namespace(self.id.clone(), |w| {
            w.use_keyed_state("preview-observer", cx, move |_, cx| {
                let generation = observed.update(cx, |s, _| {
                    s.mount_generation = s.mount_generation.wrapping_add(1);
                    s.mount_generation
                });
                let weak = observed.downgrade();
                GroupMount {
                    _observer: cx.observe(&observed, |_, _, cx| cx.notify()),
                    _release: cx.on_release(move |_: &mut GroupMount, cx| {
                        cx.defer(move |cx| {
                            let _ = weak.update(cx, |s, cx| {
                                if s.mount_generation != generation {
                                    return;
                                }
                                s.open = false;
                                s.drag = None;
                                s.touch_pan = None;
                                s.pinch_origin = None;
                                s.mask_down = false;
                                s.opening = None;
                                s.closing = None;
                                s.picture.update(cx, |p, cx| p.set_source(None, cx));
                                cx.notify();
                            });
                        });
                    }),
                }
            })
        });
        self.state.update(cx, |s, cx| {
            if let Some(scope) = self.scope {
                s.set_scope(scope, cx);
            }
            if let Some(options) = self.options {
                s.scope_defaults = false;
                s.replace_options(options, cx);
            }
            s.refresh_scope_options(cx);
            s.viewport = window.viewport_size();
        });
        let (dialog, scope, open, initial_focus) = {
            let s = self.state.read(cx);
            (
                s.dialog.clone(),
                s.scope.clone(),
                s.open,
                s.operation_focus[6].clone(),
            )
        };
        let weak = self.state.downgrade();
        let cancel = weak.clone();
        let closed = weak.clone();
        let (
            mask,
            focus_trap,
            mut z_index,
            mut styles,
            duration,
            motion,
            mask_color,
            resolver,
            context,
            local_styles,
        ) = {
            let s = self.state.read(cx);
            let config = s.scope.snapshot(cx);
            let tokens = config.theme.component::<ImageTheme>();
            let styles = config.image.styles.clone();
            let status = s.picture.read(cx).status();
            (
                s.options.mask,
                s.options.focus_trap,
                tokens.z_index_popup,
                styles,
                tokens.globals().motion_duration_slow,
                tokens.globals().motion,
                tokens.globals().color_bg_mask,
                config.image.styles_fn.clone(),
                ImageStyleContext {
                    preview: true,
                    loading: status == PictureStatus::Loading,
                    failed: status == PictureStatus::Error,
                    direction: config.direction,
                },
                s.options.styles.clone(),
            )
        };
        // Resolve outside the controller borrow, then share one snapshot with
        // both the Modal-owned mask and our native content surface.
        if let Some(resolver) = resolver {
            styles.refine(&resolver.resolve(&context, window, cx));
        }
        styles.refine(&local_styles);
        let mut mask_style = StyleRefinement::default().bg(mask_color);
        mask_style.refine(&styles.popup_mask);
        styles.popup_mask = mask_style;
        let styles = Rc::new(styles);
        if let Some(parent_z) = crate::modal::host::owner_z_index(window, cx) {
            z_index = z_index.max(parent_z.saturating_add(1));
        }
        let options = ModalOptions {
            title: None,
            content: None,
            footer: ModalFooter::None,
            width: Some(ModalWidth::Fixed(ModalLength::Fraction(1.))),
            top: 0.,
            centered: Some(false),
            mask: Some(mask),
            mask_closable: Some(false),
            focus_trap: Some(focus_trap),
            focus_return: Some(true),
            closable: Some(false),
            initial_focus: Some(initial_focus),
            z_index,
            accessibility_label: Some(scope.snapshot(cx).locale.image.preview.clone()),
            automation_id: Some("image.preview".into()),
            styles: ModalStyles {
                root: StyleRefinement::default()
                    .w(window.viewport_size().width)
                    .h(window.viewport_size().height),
                wrapper: StyleRefinement::default().p_0(),
                mask: if mask {
                    styles.popup_mask.clone()
                } else {
                    StyleRefinement::default()
                },
                ..Default::default()
            },
            surface_render: Some(Rc::new(move |_, _, w, cx| {
                weak.upgrade().map_or_else(
                    || div().into_any_element(),
                    |s| render_surface(&s, &styles, w, cx),
                )
            })),
            on_cancel: Some(Rc::new(move |_, w, cx| {
                if let Some(state) = cancel.upgrade() {
                    state.update(cx, |s, cx| s.request_close(w, cx));
                }
                ModalAction::KeepOpen
            })),
            on_event: Some(Rc::new(move |event, _, cx| {
                if matches!(event, crate::modal::ModalEvent::AfterClose) {
                    let _ = closed.update(cx, |s, cx| {
                        if !s.open {
                            s.picture.update(cx, |p, cx| p.set_source(None, cx));
                            s.transform = PreviewTransform::default();
                            s.transform.scale = s.options.min_scale;
                            s.visual.snap(s.transform, cx.background_executor().now());
                            s.closing = None;
                        }
                    });
                }
            })),
            ..Default::default()
        };
        dialog.update(cx, |s, cx| {
            s.set_scope(scope.clone(), cx);
            s.set_viewport_motion(duration, motion);
            s.set_open(open, window, cx);
        });
        let mut result = div().id(self.id).children(self.children).child(
            Modal::new("preview-shell", &dialog, cx)
                .scope(scope)
                .options(options),
        );
        result.style().refine(&self.style);
        result
    }
}

fn render_surface(
    state: &Entity<PreviewState>,
    styles: &ImageStyles,
    window: &mut Window,
    cx: &mut App,
) -> AnyElement {
    let visual = state.update(cx, |s, cx| s.visual_transform(window, cx));
    let (options, scope, transform, current, total, item, picture, focus, extent, dragging) = {
        let s = state.read(cx);
        (
            s.options.clone(),
            s.scope.clone(),
            s.transform,
            s.current,
            s.items().len(),
            s.current_item().cloned(),
            s.picture.clone(),
            s.operation_focus.clone(),
            s.fitted_size(cx),
            s.drag.is_some(),
        )
    };
    let config = scope.snapshot(cx);
    let tokens = config.theme.component::<ImageTheme>();
    let globals = tokens.globals();
    let control_extent = tokens.preview_operation_size + 2. * globals.padding_sm as f32;
    let status = picture.read(cx).status();
    let info = PreviewRenderInfo {
        state: state.clone(),
        transform,
        current,
        total,
        image: item.as_ref().map(|item| {
            let natural = picture.read(cx).natural_size().unwrap_or_default();
            ImageInfo {
                id: item.id.clone(),
                source: item.source.clone(),
                alt: item.alt.clone(),
                width: f32::from(natural.width),
                height: f32::from(natural.height),
            }
        }),
    };
    let mut image = Picture::new("preview-picture", &picture)
        .w(extent.width)
        .h(extent.height)
        .affine(pose_matrix(visual, extent))
        .object_fit(gpui::ObjectFit::Contain)
        .when(options.movable, |image| {
            image.cursor(if dragging {
                gpui::CursorStyle::ClosedHand
            } else {
                gpui::CursorStyle::OpenHand
            })
        });
    image.style().refine(&styles.image);
    let mut rendered = Accessibility::new(Role::Image)
        .label(
            item.as_ref()
                .map_or_else(SharedString::default, |item| item.alt.clone()),
        )
        .automation_id("image.preview.image")
        .apply(image)
        .into_any_element();
    if status != PictureStatus::Ready {
        let indicator = if status == PictureStatus::Error {
            div()
                .text_color(rgba(0xffffffff))
                .child(config.locale.image.load_error.clone())
                .into_any_element()
        } else {
            crate::Spin::new("preview-loading")
                .scope(scope.clone())
                .spinning(true)
                .into_any_element()
        };
        rendered = div()
            .relative()
            .w(extent.width)
            .h(extent.height)
            .child(rendered)
            .child(
                div()
                    .absolute()
                    .inset_0()
                    .flex()
                    .items_center()
                    .justify_center()
                    .child(indicator),
            )
            .into_any_element();
    }
    if let Some(render) = &options.image_render {
        rendered = render(rendered, &info, window, cx);
    }
    let down = state.clone();
    let wheel = state.clone();
    let pinch = state.clone();
    let moves = state.downgrade();
    let release = state.downgrade();
    let mut body = div()
        .id("preview-body")
        .absolute()
        .inset_0()
        .flex()
        .items_center()
        .justify_center()
        .child(rendered)
        .child(
            canvas(
                |_, _, _| (),
                move |_, (), w, _| {
                    let target = moves.clone();
                    w.on_mouse_event(move |event: &MouseMoveEvent, phase, w, cx| {
                        if phase.capture() {
                            let _ =
                                target.update(cx, |s, cx| s.pointer_move(event.position, w, cx));
                        }
                    });
                    let target = release.clone();
                    w.on_mouse_event(move |event: &MouseUpEvent, phase, w, cx| {
                        if phase.capture() && event.button == MouseButton::Left {
                            let _ = target.update(cx, |s, cx| {
                                let mask = std::mem::take(&mut s.mask_down);
                                s.pointer_end(w, cx);
                                if mask
                                    && s.options.mask_closable
                                    && !contains(
                                        s.fitted_size(cx),
                                        s.viewport,
                                        s.transform,
                                        event.position,
                                    )
                                {
                                    s.request_close(w, cx);
                                }
                            });
                        }
                    });
                },
            )
            .absolute()
            .inset_0(),
        );
    body.style().refine(&styles.popup_body);
    let mut actions = div()
        .id("preview-actions")
        .flex()
        .items_center()
        .gap(px(globals.padding_sm as f32))
        .px(px(globals.padding_lg as f32))
        .rounded(px(100.))
        .bg(globals.color_bg_mask.with_alpha(0.1));
    let operations = [
        (
            PreviewAction::FlipY,
            IconName::SwapOutlined,
            config.locale.image.flip_y.clone(),
        ),
        (
            PreviewAction::FlipX,
            IconName::SwapOutlined,
            config.locale.image.flip_x.clone(),
        ),
        (
            PreviewAction::RotateLeft,
            IconName::RotateLeftOutlined,
            config.locale.image.rotate_left.clone(),
        ),
        (
            PreviewAction::RotateRight,
            IconName::RotateRightOutlined,
            config.locale.image.rotate_right.clone(),
        ),
        (
            PreviewAction::ZoomOut,
            IconName::ZoomOutOutlined,
            config.locale.image.zoom_out.clone(),
        ),
        (
            PreviewAction::ZoomIn,
            IconName::ZoomInOutlined,
            config.locale.image.zoom_in.clone(),
        ),
    ];
    let mut operation_text = StyleRefinement {
        text: styles.popup_footer.text.clone(),
        ..Default::default()
    };
    operation_text.text.refine(&styles.popup_actions.text);
    // GPUI snaps absolute leaf dimensions before flex layout. Derive the whole
    // toolbar first so fractional-DPI glyph rounding cannot accumulate across
    // six controls and shrink its centered background.
    let extent = operation_text
        .text
        .font_size
        .map_or(px(tokens.preview_operation_size), |value| {
            value.to_pixels(window.rem_size())
        })
        + px(2. * globals.padding_sm as f32);
    let absolute = |value: Option<gpui::DefiniteLength>, fallback: f32| match value {
        Some(gpui::DefiniteLength::Absolute(value)) => Some(value.to_pixels(window.rem_size())),
        Some(gpui::DefiniteLength::Fraction(_)) => None,
        None => Some(px(fallback)),
    };
    if let (Some(left), Some(right), Some(gap)) = (
        absolute(styles.popup_actions.padding.left, globals.padding_lg as f32),
        absolute(
            styles.popup_actions.padding.right,
            globals.padding_lg as f32,
        ),
        absolute(styles.popup_actions.gap.width, globals.padding_sm as f32),
    ) {
        let border = &styles.popup_actions.border_widths;
        let border_width = [border.left, border.right]
            .into_iter()
            .flatten()
            .map(|value| value.to_pixels(window.rem_size()))
            .sum::<Pixels>();
        actions = actions
            .w(extent * 6. + gap * 5. + left + right + border_width)
            .justify_between();
    }
    for (index, (action, glyph, label)) in operations.into_iter().enumerate() {
        let disabled = match action {
            PreviewAction::ZoomOut => transform.scale <= options.min_scale,
            PreviewAction::ZoomIn => transform.scale >= options.max_scale,
            _ => false,
        };
        actions = actions.child(operation(
            state,
            action,
            glyph.into(),
            label,
            index,
            &focus[index],
            disabled,
            action == PreviewAction::FlipY,
            false,
            Some(&operation_text),
            window,
            cx,
        ));
    }
    actions.style().refine(&styles.popup_actions);
    let mut actions = actions.into_any_element();
    if let Some(render) = &options.actions_render {
        actions = render(actions, &info, window, cx);
    }
    let mut footer = div()
        .id("preview-footer")
        .flex_none()
        .flex()
        .flex_col()
        .items_center()
        .text_color(tokens.preview_operation_color)
        .gap(px(globals.margin as f32))
        .on_mouse_down(MouseButton::Left, |_, _, cx| cx.stop_propagation());
    if total >= 1 && options.show_count.unwrap_or(true) {
        let count = options.count_render.as_ref().map_or_else(
            || {
                div()
                    .child(format!("{} / {}", current + 1, total))
                    .into_any_element()
            },
            |render| render(current + 1, total, window, cx),
        );
        footer = footer.child(
            Accessibility::new(Role::Label)
                .label(
                    config
                        .locale
                        .image
                        .count
                        .replace("{current}", &(current + 1).to_string())
                        .replace("{total}", &total.to_string()),
                )
                .automation_id("image.preview.count")
                .apply(div().id("preview-count").child(count)),
        );
    }
    footer = footer.child(actions);
    footer.style().refine(&styles.popup_footer);
    let footer = div()
        .absolute()
        .left_0()
        .right_0()
        .bottom(px(globals.margin_xl as f32))
        .flex()
        .justify_center()
        .child(footer);
    let close = operation(
        state,
        PreviewAction::Close,
        options.close_icon.clone(),
        config.locale.common.close.clone(),
        6,
        &focus[6],
        false,
        false,
        true,
        Some(&styles.popup_close),
        window,
        cx,
    );
    let mut surface = div()
        .id("image-preview-surface")
        .relative()
        .w(window.viewport_size().width)
        .h(window.viewport_size().height)
        .text_size(px(globals.font_size as f32))
        .font(globals.font_family.to_gpui_font())
        .on_mouse_down(MouseButton::Left, move |event, w, cx| {
            down.update(cx, |s, cx| {
                let inside = contains(s.fitted_size(cx), s.viewport, s.transform, event.position);
                s.mask_down = !inside;
                if inside && event.click_count == 2 {
                    s.double_click(event.position, w, cx);
                } else if inside && s.options.movable {
                    s.drag = Some(Drag {
                        origin: event.position,
                        translation: point(px(s.transform.x), px(s.transform.y)),
                        moved: false,
                    });
                    cx.notify();
                }
            });
            w.prevent_default();
            cx.stop_propagation();
        })
        .on_scroll(move |event, w, cx| {
            wheel.update(cx, |s, cx| {
                if event.source == ScrollSource::Touch {
                    s.touch_scroll(event, w, cx);
                } else {
                    s.wheel(
                        -f32::from(event.delta.pixel_delta(px(16.)).y),
                        event.position,
                        w,
                        cx,
                    );
                }
            });
            cx.stop_propagation();
        })
        .on_pinch(move |event, w, cx| {
            pinch.update(cx, |s, cx| s.pinch(event, w, cx));
            cx.stop_propagation();
        })
        .child(body)
        .child(footer)
        .child(close);
    if total > 1 {
        let rtl = config.direction == crate::Direction::Rtl;
        surface = surface
            .child(
                div()
                    .absolute()
                    .left(px(globals.margin_sm as f32))
                    .top(window.viewport_size().height / 2. - px(control_extent / 2.))
                    .child(operation(
                        state,
                        PreviewAction::Previous,
                        if rtl {
                            IconName::RightOutlined
                        } else {
                            IconName::LeftOutlined
                        }
                        .into(),
                        config.locale.image.previous.clone(),
                        7,
                        &focus[7],
                        current == 0,
                        false,
                        true,
                        None,
                        window,
                        cx,
                    )),
            )
            .child(
                div()
                    .absolute()
                    .right(px(globals.margin_sm as f32))
                    .top(window.viewport_size().height / 2. - px(control_extent / 2.))
                    .child(operation(
                        state,
                        PreviewAction::Next,
                        if rtl {
                            IconName::LeftOutlined
                        } else {
                            IconName::RightOutlined
                        }
                        .into(),
                        config.locale.image.next.clone(),
                        8,
                        &focus[8],
                        current + 1 >= total,
                        false,
                        true,
                        None,
                        window,
                        cx,
                    )),
            );
    }
    surface.style().refine(&styles.popup_root);
    surface.into_any_element()
}

struct OperationState {
    guard: ActivationGuard,
    hovered: bool,
    pressed: bool,
    hover: ScalarTransition,
    epoch: Instant,
    _blur: Subscription,
}
impl OperationState {
    fn retarget(&mut self, disabled: bool, duration: Duration, policy: MotionPolicy, cx: &App) {
        if disabled {
            self.pressed = false;
        }
        self.hover.retarget(
            if self.hovered && !self.pressed && !disabled {
                1.
            } else {
                0.
            },
            cx.background_executor()
                .now()
                .saturating_duration_since(self.epoch),
            duration,
            MotionPolicy::new(policy.enabled && !disabled, policy.reduced),
        );
    }
}

#[allow(clippy::too_many_arguments)]
fn operation(
    state: &Entity<PreviewState>,
    action: PreviewAction,
    glyph: IconRef,
    label: SharedString,
    index: usize,
    focus: &FocusHandle,
    disabled: bool,
    vertical: bool,
    circle: bool,
    style: Option<&StyleRefinement>,
    window: &mut Window,
    cx: &mut App,
) -> AnyElement {
    let scope = state.read(cx).scope.clone();
    let config = scope.snapshot(cx);
    let tokens = config.theme.component::<ImageTheme>();
    let globals = tokens.globals();
    let normal: gpui::Hsla = if disabled {
        tokens.preview_operation_color_disabled.into()
    } else if circle {
        globals.color_text_light_solid.into()
    } else {
        style
            .and_then(|style| style.text.color)
            .unwrap_or_else(|| tokens.preview_operation_color.into())
    };
    let hover: gpui::Hsla = if circle {
        globals.color_text_light_solid.into()
    } else {
        tokens.preview_operation_hover_color.into()
    };
    let retained = window.use_keyed_state(("preview-operation-state", index), cx, |w, cx| {
        let guard = ActivationGuard::default();
        let blur = guard.observe_blur(focus, w, cx);
        OperationState {
            guard,
            hovered: false,
            pressed: false,
            hover: ScalarTransition::new(0., crate::theme::CubicBezier::new(0.25, 0.1, 0.25, 1.)),
            epoch: cx.background_executor().now(),
            _blur: blur,
        }
    });
    let guard = retained.read(cx).guard.clone();
    guard.update(
        Availability {
            disabled,
            busy: false,
        },
        focus,
        window,
        cx,
    );
    let motion = MotionPolicy::new(config.motion && globals.motion, cx.reduce_motion());
    let duration = globals.motion_duration_slow;
    let hover_sample = retained.update(cx, |s, cx| {
        let now = cx
            .background_executor()
            .now()
            .saturating_duration_since(s.epoch);
        s.retarget(disabled, duration, motion, cx);
        s.hover.sample(now)
    });
    if !hover_sample.finished {
        state.update(cx, |s, cx| s.frame(window, cx));
    }
    let amount = hover_sample.value as f32;
    let normal = normal.to_rgb();
    let hover = hover.to_rgb();
    let foreground = gpui::Rgba {
        r: normal.r + (hover.r - normal.r) * amount,
        g: normal.g + (hover.g - normal.g) * amount,
        b: normal.b + (hover.b - normal.b) * amount,
        a: normal.a + (hover.a - normal.a) * amount,
    };
    let target = state.clone();
    let hovered = retained.clone();
    let pressed = retained.clone();
    let released = retained.clone();
    let released_out = retained.clone();
    let pointer_focus = focus.clone();
    let icon_size = style
        .and_then(|style| style.text.font_size)
        .map_or(px(tokens.preview_operation_size), |value| {
            value.to_pixels(window.rem_size())
        });
    let extent = f32::from(icon_size) + 2. * globals.padding_sm as f32;
    let operation_bg = globals
        .color_bg_mask
        .with_alpha(0.1 + 0.1 * f64::from(amount));
    let mut element = div()
        .id(("preview-operation", index))
        .flex()
        .items_center()
        .justify_center()
        .flex_none()
        .size(px(extent))
        .rounded(px(extent / 2.))
        .text_color(foreground)
        .when(circle && !disabled, |d| d.bg(operation_bg))
        .when(action == PreviewAction::Close, |d| {
            d.absolute()
                .top(px(globals.margin_sm as f32))
                .right(px(globals.margin_sm as f32))
        })
        .when(!disabled, |d| d.cursor_pointer())
        .when(!disabled, |d| d.track_focus(focus))
        .when(disabled, |d| d.cursor_not_allowed())
        .on_mouse_down(MouseButton::Left, move |_, w, cx| {
            if !disabled {
                w.focus(&pointer_focus, cx);
                pressed.update(cx, |s, cx| {
                    s.pressed = true;
                    s.retarget(disabled, duration, motion, cx);
                    cx.notify();
                });
            }
            cx.stop_propagation();
        })
        .on_hover(move |over, _, cx| {
            hovered.update(cx, |s, cx| {
                s.hovered = *over;
                if !over {
                    s.pressed = false;
                }
                s.retarget(disabled, duration, motion, cx);
                cx.notify();
            })
        })
        .on_mouse_up(MouseButton::Left, move |_, _, cx| {
            released.update(cx, |s, cx| {
                if s.pressed {
                    s.pressed = false;
                    s.retarget(disabled, duration, motion, cx);
                    cx.notify();
                }
            })
        })
        .on_mouse_up_out(MouseButton::Left, move |_, _, cx| {
            released_out.update(cx, |s, cx| {
                if s.pressed {
                    s.pressed = false;
                    s.hovered = false;
                    s.retarget(disabled, duration, motion, cx);
                    cx.notify();
                }
            })
        })
        .child(
            icon(glyph)
                .inherit_color()
                .ant_design_frame()
                .size_full()
                .paint_size(size(icon_size, icon_size))
                .rotation(radians(if vertical {
                    std::f32::consts::FRAC_PI_2
                } else {
                    0.
                })),
        );
    if let Some(style) = style.filter(|_| circle) {
        element.style().refine(style);
    }
    let element = guard.install(element, move |_, w, cx| {
        target.update(cx, |s, cx| s.apply_action(action, w, cx));
        cx.stop_propagation();
    });
    let name = match action {
        PreviewAction::FlipY => "flip-y",
        PreviewAction::FlipX => "flip-x",
        PreviewAction::RotateLeft => "rotate-left",
        PreviewAction::RotateRight => "rotate-right",
        PreviewAction::ZoomOut => "zoom-out",
        PreviewAction::ZoomIn => "zoom-in",
        PreviewAction::Close => "close",
        PreviewAction::Previous => "previous",
        PreviewAction::Next => "next",
        _ => "action",
    };
    let mut semantics = Accessibility::new(Role::Button)
        .label(label)
        .automation_id(format!("image.preview.{name}"));
    semantics.disabled = disabled;
    semantics
        .apply(
            FocusRing::new(
                globals.color_primary_border,
                px(globals.line_width_focus as f32),
            )
            .radius(px(extent / 2.))
            .apply(element, focus),
        )
        .into_any_element()
}

#[cfg(test)]
mod geometry_tests {
    use super::*;
    #[test]
    fn fitting_caps_large_images_and_preserves_small_images() {
        assert_eq!(
            fit(size(px(1200.), px(1200.)), size(px(1000.), px(800.))),
            size(px(560.), px(560.))
        );
        assert_eq!(
            fit(size(px(200.), px(100.)), size(px(1000.), px(800.))),
            size(px(200.), px(100.))
        );
    }
    #[test]
    fn rebound_centers_small_images_and_keeps_large_edges_inside() {
        assert_eq!(rebound(100., 200., 800.), 0.);
        assert_eq!(rebound(900., 1200., 800.), 200.);
        assert_eq!(rebound(-900., 1200., 800.), -200.);
    }
    #[test]
    fn rotated_extent_uses_both_dimensions() {
        let mut transform = PreviewTransform {
            rotate: 90.,
            scale: 2.,
            ..Default::default()
        };
        let (width, height) = rotated_extent(size(px(200.), px(100.)), transform);
        assert!((width - 200.).abs() < 0.001 && (height - 400.).abs() < 0.001);
        transform.rotate = 0.;
        assert_eq!(
            rotated_extent(size(px(200.), px(100.)), transform),
            (400., 200.)
        );
    }
    #[test]
    fn flip_animation_interpolates_signed_axes_and_keeps_its_center() {
        let now = Instant::now();
        let mut visual = VisualMotion::new(now);
        visual.target = pose(PreviewTransform {
            flip_x: true,
            ..Default::default()
        });
        visual.duration = Duration::from_millis(300);
        let (sample, moving) = visual.sample(
            now + Duration::from_millis(150),
            crate::theme::CubicBezier::new(0., 0., 1., 1.),
        );
        assert!(moving);
        assert!(sample[3].abs() < 0.001);
        assert_eq!(sample[4], 1.);
        let matrix = pose_matrix(sample, size(px(200.), px(100.)));
        assert!((matrix[0] * 100. + matrix[2] * 50. + matrix[4] - 100.).abs() < 0.001);
        assert!((matrix[1] * 100. + matrix[3] * 50. + matrix[5] - 50.).abs() < 0.001);
    }
    #[test]
    fn preview_visibility_uses_css_ease_midpoint() {
        let midpoint = crate::behavior::evaluate_cubic_bezier(
            crate::theme::CubicBezier::new(0.25, 0.1, 0.25, 1.),
            0.5,
        );
        assert!((midpoint - 0.802_403_387_695_4).abs() < 1e-7);
    }
    #[test]
    fn preview_motion_matches_independent_browser_transition_samples() {
        fn matrix(text: &str) -> [f32; 6] {
            if text == "none" {
                return [1., 0., 0., 1., 0., 0.];
            }
            let values: Vec<f32> = text
                .strip_prefix("matrix(")
                .unwrap()
                .strip_suffix(')')
                .unwrap()
                .split(',')
                .map(|v| v.trim().parse().unwrap())
                .collect();
            values.try_into().unwrap()
        }
        fn pose_from_css(text: &str) -> [f32; 5] {
            fn values(text: &str, name: &str) -> Vec<f32> {
                text.split_once(name)
                    .unwrap()
                    .1
                    .split_once(')')
                    .unwrap()
                    .0
                    .split(',')
                    .map(|value| {
                        value
                            .trim()
                            .trim_end_matches("px")
                            .trim_end_matches("deg")
                            .parse()
                            .unwrap()
                    })
                    .collect()
            }
            let translation = values(text, "translate3d(");
            let scale = values(text, "scale3d(");
            let rotate = values(text, "rotate(");
            [
                translation[0],
                translation[1],
                rotate[0],
                scale[0],
                scale[1],
            ]
        }
        let reference: serde_json::Value =
            serde_json::from_str(include_str!("../../fixtures/image/motion-reference.json"))
                .unwrap();
        let theme = crate::theme::ThemeConfig::default().resolve();
        let curve = theme.component::<ImageTheme>().globals().motion_ease_out;
        let mut visibility_samples = 0;
        let mut image_samples = 0;
        for case in reference["cases"]
            .as_array()
            .unwrap()
            .iter()
            .filter(|case| {
                case["theme"] == "default" && case["direction"] == "ltr" && case["scale"] == 1
            })
        {
            let transition = case["transition"].as_str().unwrap();
            if matches!(transition, "open" | "close") {
                let progress = crate::behavior::evaluate_cubic_bezier(
                    crate::theme::CubicBezier::new(0.25, 0.1, 0.25, 1.),
                    case["phase_ms"].as_f64().unwrap() / 300.,
                );
                let factor = if transition == "open" {
                    progress
                } else {
                    1. - progress
                };
                let opacity = case["preview"]["styles"]["opacity"]
                    .as_str()
                    .unwrap()
                    .parse::<f64>()
                    .unwrap();
                assert!((factor - opacity).abs() < 1e-5, "{}", case["name"]);
                let body = matrix(case["preview"]["styles"]["bodyTransform"].as_str().unwrap());
                assert!(
                    (f64::from(body[0]) - factor).abs() < 1e-5,
                    "{}",
                    case["name"]
                );
                visibility_samples += 1;
            }
            if let Some(animation) = case["css_animations"].as_array().and_then(|animations| {
                animations.iter().find(|animation| {
                    animation["target"]
                        .as_str()
                        .is_some_and(|target| target.contains("ant-image-preview-img"))
                })
            }) {
                let keyframes = animation["keyframes"].as_array().unwrap();
                let now = Instant::now();
                let visual = VisualMotion {
                    from: pose_from_css(keyframes.first().unwrap()["transform"].as_str().unwrap()),
                    target: pose_from_css(keyframes.last().unwrap()["transform"].as_str().unwrap()),
                    started: now,
                    duration: Duration::from_secs_f64(
                        animation["timing"]["duration"].as_f64().unwrap() / 1000.,
                    ),
                };
                let (sample, _) = visual.sample(
                    now + Duration::from_secs_f64(case["image_phase_ms"].as_f64().unwrap() / 1000.),
                    curve,
                );
                let actual = pose_matrix(sample, Size::default());
                let expected = matrix(
                    case["preview"]["styles"]["imageTransform"]
                        .as_str()
                        .unwrap(),
                );
                for (actual, expected) in actual.into_iter().zip(expected) {
                    assert!(
                        (actual - expected).abs() < 2e-5,
                        "{}: {actual} vs {expected}",
                        case["name"]
                    );
                }
                image_samples += 1;
            }
        }
        assert_eq!(visibility_samples, 9);
        assert_eq!(image_samples, 12);
    }
}
