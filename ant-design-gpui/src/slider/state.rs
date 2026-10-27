// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::{
    SliderDisabled, SliderError, SliderOptions, SliderOrientation, SliderValue,
    model::{HandleValue, Proposal, SliderModel},
};
use crate::{
    ConfigScope, Direction,
    behavior::{ScalarTransition, focus_handle},
    theme::CubicBezier,
};
use gpui::{
    App, Bounds, Context, EventEmitter, FocusHandle, Focusable, Pixels, Point, Subscription, Window,
};
use std::{collections::HashMap, rc::Rc};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SliderChangeSource {
    Pointer,
    Keyboard,
    Mark,
    Accessibility,
}
#[derive(Clone, Debug, PartialEq)]
pub struct SliderChange {
    pub value: SliderValue,
    pub source: SliderChangeSource,
}
#[derive(Clone, Debug, PartialEq)]
pub enum SliderEvent {
    Change(SliderChange),
    ChangeComplete(SliderChange),
    Focus { handle: usize },
    Blur { handle: usize },
}
pub(super) type ChangeHandler = Rc<dyn Fn(&SliderChange, &mut Window, &mut App)>;

pub(super) struct HandleState {
    pub focus: FocusHandle,
    pub hovered: bool,
    // Mouse release closes focus-triggered tooltips without removing keyboard focus.
    pub tooltip_focus_open: bool,
    pub motion: ScalarTransition,
    _subscriptions: Vec<Subscription>,
}
pub(super) struct Drag {
    pub active: Option<u64>,
    pub start: Point<Pixels>,
    pub origin: Vec<HandleValue>,
    pub working: Vec<HandleValue>,
    pub deleting: bool,
}

/// Retained slider controller. Construct once, then render `Slider::new(id, &state)`.
pub struct SliderState {
    pub(super) model: SliderModel,
    pub(super) handles: HashMap<u64, HandleState>,
    pub(super) bounds: Option<Bounds<Pixels>>,
    pub(super) drag: Option<Drag>,
    pub(super) hovered: bool,
    pub(super) rail_motion: ScalarTransition,
    pub(super) epoch: std::time::Instant,
    pub(super) frame_pending: bool,
    pub(super) scope: Option<ConfigScope>,
    pub(super) disabled_override: Option<SliderDisabled>,
    pub(super) controlled: bool,
    pub(super) initialized: bool,
    pub(super) pending: Option<Proposal>,
    pub(super) on_change: Option<ChangeHandler>,
    pub(super) on_complete: Option<ChangeHandler>,
    key_active: Option<u64>,
    _scope_subscription: Option<Subscription>,
}
impl EventEmitter<SliderEvent> for SliderState {}
impl Focusable for SliderState {
    fn focus_handle(&self, _: &App) -> FocusHandle {
        self.model
            .handles
            .iter()
            .enumerate()
            .find(|(i, _)| !self.model.options.disabled.handle(*i))
            .or_else(|| self.model.handles.iter().enumerate().next())
            .and_then(|(_, value)| self.handles.get(&value.id))
            .map(|h| h.focus.clone())
            .unwrap_or_else(|| self.handles[&u64::MAX].focus.clone())
    }
}
impl SliderState {
    pub fn new(window: &mut Window, cx: &mut Context<Self>) -> Self {
        Self::with_options(SliderOptions::default(), window, cx).expect("valid default slider")
    }
    pub fn with_options(
        options: SliderOptions,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Result<Self, SliderError> {
        let value = options.default_value();
        let mut this = Self {
            model: SliderModel::new(options, value)?,
            handles: HashMap::new(),
            bounds: None,
            drag: None,
            hovered: false,
            rail_motion: ScalarTransition::new(0., CubicBezier::new(0.25, 0.1, 0.25, 1.)),
            epoch: cx.background_executor().now(),
            frame_pending: false,
            scope: None,
            disabled_override: None,
            controlled: false,
            initialized: false,
            pending: None,
            on_change: None,
            on_complete: None,
            key_active: None,
            _scope_subscription: None,
        };
        this.sync_handles(window, cx);
        Ok(this)
    }
    pub fn value(&self) -> SliderValue {
        self.model.value()
    }
    pub fn options(&self) -> &SliderOptions {
        &self.model.options
    }
    /// External updates are silent; only user interactions emit change events.
    pub fn set_value(
        &mut self,
        value: impl Into<SliderValue>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Result<(), SliderError> {
        let value = value.into();
        if let Some(proposal) = self
            .pending
            .take()
            .filter(|p| p.value(self.model.options.range.is_some()) == value)
        {
            self.model.accept(&proposal);
            self.sync_handles(window, cx);
            cx.notify();
        } else if self.model.set_value(value)? {
            self.sync_handles(window, cx);
            cx.notify();
        }
        self.initialized = true;
        Ok(())
    }
    pub fn set_options(
        &mut self,
        options: SliderOptions,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Result<(), SliderError> {
        if self.model.options == options {
            return Ok(());
        }
        self.model.set_options(options)?;
        self.cancel_interaction();
        self.sync_handles(window, cx);
        cx.notify();
        Ok(())
    }
    pub fn focus(&self, index: usize, window: &mut Window, cx: &mut App) {
        if !self.disabled(index, cx)
            && let Some(value) = self.model.handles.get(index)
            && let Some(handle) = self.handles.get(&value.id)
        {
            handle.focus.focus(window, cx);
        }
    }
    pub fn blur(&self, window: &mut Window, cx: &mut App) {
        if self.handles.values().any(|h| h.focus.is_focused(window)) {
            window.blur(cx);
        }
    }
    pub(super) fn configure_scope(
        &mut self,
        scope: ConfigScope,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.scope.as_ref() != Some(&scope) {
            self._scope_subscription = Some(scope.observe_in(window, cx, |this, window, cx| {
                this.sync_handles(window, cx);
                this.cancel_unavailable_interaction(cx);
                cx.notify();
            }));
            self.scope = Some(scope);
        }
    }
    fn disabled_settings(&self, cx: &App) -> SliderDisabled {
        self.disabled_override.clone().unwrap_or_else(|| {
            if self
                .scope
                .as_ref()
                .is_some_and(|scope| scope.snapshot(cx).disabled)
            {
                SliderDisabled::All(true)
            } else {
                self.model.options.disabled.clone()
            }
        })
    }
    pub(super) fn disabled(&self, index: usize, cx: &App) -> bool {
        self.disabled_settings(cx).handle(index)
    }
    pub(super) fn all_disabled(&self, cx: &App) -> bool {
        self.disabled_settings(cx).all(self.model.handles.len())
    }
    pub(super) fn any_disabled(&self, cx: &App) -> bool {
        self.disabled_settings(cx).any(self.model.handles.len())
    }
    pub(super) fn sync_handles(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let mut ids: Vec<_> = self.model.handles.iter().map(|h| h.id).collect();
        if let Some(id) = self.drag.as_ref().and_then(|drag| drag.active) {
            ids.push(id);
        }
        if ids.is_empty() {
            ids.push(u64::MAX);
        }
        self.handles.retain(|id, _| ids.contains(id));
        for id in ids {
            self.handles.entry(id).or_insert_with(|| {
                let focus = focus_handle(cx);
                let enter = cx.on_focus(&focus, window, move |this, _, cx| {
                    if let Some(handle) = this.handles.get_mut(&id) {
                        handle.tooltip_focus_open = true;
                    }
                    if let Some(handle) = this.model.handles.iter().position(|h| h.id == id) {
                        cx.emit(SliderEvent::Focus { handle });
                    }
                    cx.notify();
                });
                let leave = cx.on_blur(&focus, window, move |this, _, cx| {
                    if let Some(handle) = this.handles.get_mut(&id) {
                        handle.tooltip_focus_open = false;
                    }
                    this.key_active = None;
                    if this.drag.as_ref().is_some_and(|d| d.active == Some(id)) {
                        this.cancel_interaction();
                    }
                    if let Some(handle) = this.model.handles.iter().position(|h| h.id == id) {
                        cx.emit(SliderEvent::Blur { handle });
                    }
                    cx.notify();
                });
                HandleState {
                    focus,
                    hovered: false,
                    tooltip_focus_open: false,
                    motion: ScalarTransition::new(0., CubicBezier::new(0.25, 0.1, 0.25, 1.)),
                    _subscriptions: vec![enter, leave],
                }
            });
        }
        for (index, value) in self.model.handles.iter().enumerate() {
            let disabled = self.disabled(index, cx);
            let focus = &self.handles[&value.id].focus;
            focus.clone().tab_stop(!disabled);
            if disabled && focus.is_focused(window) {
                window.blur(cx);
            }
        }
    }
    pub(super) fn cancel_interaction(&mut self) {
        self.drag = None;
        self.key_active = None;
        self.pending = None;
    }
    pub(super) fn cancel_unavailable_interaction(&mut self, cx: &App) {
        let available = |id| {
            self.model
                .handles
                .iter()
                .position(|h| h.id == id)
                .or_else(|| {
                    self.drag
                        .as_ref()
                        .and_then(|d| d.working.iter().position(|h| h.id == id))
                })
                .is_some_and(|i| !self.disabled(i, cx))
        };
        if self.all_disabled(cx)
            || self.key_active.is_some_and(|id| !available(id))
            || self.drag.as_ref().is_some_and(|d| {
                d.active
                    .map_or_else(|| self.any_disabled(cx), |id| !available(id))
            })
        {
            self.cancel_interaction();
        }
    }
    fn proposal(
        &mut self,
        proposal: Proposal,
        source: SliderChangeSource,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let value = proposal.value(self.model.options.range.is_some());
        let changed = value != self.value();
        if !self.controlled {
            self.model.accept(&proposal);
        } else {
            self.pending = Some(proposal.clone());
        }
        self.sync_handles(window, cx);
        if let Some(active) = proposal.active
            && let Some(handle) = self.handles.get(&active)
        {
            handle.focus.focus(window, cx);
        }
        if changed {
            let change = SliderChange { value, source };
            cx.emit(SliderEvent::Change(change.clone()));
            if let Some(handler) = self.on_change.clone() {
                handler(&change, window, cx);
            }
            cx.notify();
        }
    }
    pub(super) fn complete(
        &mut self,
        source: SliderChangeSource,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let change = SliderChange {
            value: self.value(),
            source,
        };
        cx.emit(SliderEvent::ChangeComplete(change.clone()));
        if let Some(handler) = self.on_complete.clone() {
            handler(&change, window, cx);
        }
    }
    pub(super) fn activate_value(
        &mut self,
        value: f64,
        source: SliderChangeSource,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.all_disabled(cx) {
            return;
        }
        let saved = self.model.options.disabled.clone();
        self.model.options.disabled = self.disabled_settings(cx);
        let proposal = self.model.click(value);
        self.model.options.disabled = saved;
        if let Some(proposal) = proposal {
            self.proposal(proposal, source, window, cx);
            self.complete(source, window, cx);
        }
    }
    pub(super) fn pointer_start(
        &mut self,
        active: Option<u64>,
        track: bool,
        position: Point<Pixels>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.all_disabled(cx) {
            return;
        }
        if let Some(id) = active
            && self
                .model
                .handles
                .iter()
                .position(|h| h.id == id)
                .is_none_or(|i| self.disabled(i, cx))
        {
            return;
        }
        if track && (!self.model.track_draggable() || self.any_disabled(cx)) {
            return;
        }
        let active = if active.is_none() && !track {
            let Some(target) = self.pointer_value(position, cx) else {
                return;
            };
            let saved = self.model.options.disabled.clone();
            self.model.options.disabled = self.disabled_settings(cx);
            let proposal = self.model.click(target);
            self.model.options.disabled = saved;
            let Some(proposal) = proposal else {
                return;
            };
            let id = proposal.active;
            self.proposal(proposal, SliderChangeSource::Pointer, window, cx);
            id
        } else {
            active
        };
        let origin = self
            .pending
            .as_ref()
            .filter(|_| self.controlled)
            .map_or_else(|| self.model.handles.clone(), |p| p.handles.clone());
        self.drag = Some(Drag {
            active,
            start: position,
            working: origin.clone(),
            origin,
            deleting: false,
        });
        if let Some(active) = active
            && let Some(handle) = self.handles.get_mut(&active)
        {
            handle.tooltip_focus_open = true;
            handle.focus.focus(window, cx);
        }
        cx.notify();
    }
    fn physical_fraction(&self, position: Point<Pixels>) -> Option<f64> {
        let bounds = self.bounds?;
        let (offset, extent) = match self.model.options.orientation {
            SliderOrientation::Horizontal => (position.x - bounds.left(), bounds.size.width),
            SliderOrientation::Vertical => (position.y - bounds.top(), bounds.size.height),
        };
        (extent > gpui::px(0.)).then(|| offset.to_f64() / extent.to_f64())
    }
    pub(super) fn direction(&self, cx: &App) -> Direction {
        self.scope
            .as_ref()
            .map_or(Direction::Ltr, |scope| scope.snapshot(cx).direction)
    }
    fn pointer_value(&self, position: Point<Pixels>, cx: &App) -> Option<f64> {
        let fraction = self.physical_fraction(position)?;
        self.model
            .scale
            .from_fraction(if self.model.options.inverted(self.direction(cx)) {
                1. - fraction
            } else {
                fraction
            })
    }
    pub(super) fn pointer_move(
        &mut self,
        position: Point<Pixels>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        self.cancel_unavailable_interaction(cx);
        let Some(mut drag) = self.drag.take() else {
            return;
        };
        let Some(current) = self.physical_fraction(position) else {
            self.drag = Some(drag);
            return;
        };
        let start = self.physical_fraction(drag.start).unwrap_or(current);
        let sign = if self.model.options.inverted(self.direction(cx)) {
            -1.
        } else {
            1.
        };
        let delta = (current - start) * sign * (self.model.options.max - self.model.options.min);
        let saved_disabled = self.model.options.disabled.clone();
        self.model.options.disabled = self.disabled_settings(cx);
        let saved_handles = std::mem::replace(&mut self.model.handles, drag.working.clone());
        let mut proposal = if let Some(id) = drag.active {
            let origin = drag.origin.iter().find(|h| h.id == id).map(|h| h.value);
            origin.and_then(|value| self.model.drag_handle(id, value, delta))
        } else {
            self.model.shift_track(&drag.origin, delta)
        };
        let previous_delete = drag.deleting;
        drag.deleting = false;
        if let Some(proposal) = &mut proposal {
            drag.working.clone_from(&proposal.handles);
            let perpendicular = match self.model.options.orientation {
                SliderOrientation::Horizontal => position.y - drag.start.y,
                SliderOrientation::Vertical => position.x - drag.start.x,
            };
            if self.model.editable()
                && perpendicular.abs() > gpui::px(130.)
                && self
                    .model
                    .options
                    .range
                    .as_ref()
                    .is_some_and(|r| proposal.handles.len() > r.min_count)
                && let Some(id) = drag.active
            {
                proposal.handles.retain(|h| h.id != id);
                proposal.active = None;
                drag.deleting = true;
            }
        }
        self.model.handles = saved_handles;
        self.model.options.disabled = saved_disabled;
        self.drag = Some(drag);
        if let Some(proposal) = proposal {
            self.proposal(proposal, SliderChangeSource::Pointer, window, cx);
        }
        if self
            .drag
            .as_ref()
            .is_some_and(|d| d.deleting != previous_delete)
        {
            cx.notify();
        }
    }
    pub(super) fn pointer_end(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        self.cancel_unavailable_interaction(cx);
        // Ant Design clears tooltip focus one frame after mouseup so a click's
        // later focus notification cannot reopen it. Native focus stays intact.
        let target = cx.weak_entity();
        window.on_next_frame(move |_, cx| {
            let _ = target.update(cx, |state, cx| {
                let mut changed = false;
                for handle in state.handles.values_mut() {
                    changed |= std::mem::take(&mut handle.tooltip_focus_open);
                }
                if changed {
                    cx.notify();
                }
            });
        });
        if self.drag.take().is_some() {
            self.complete(SliderChangeSource::Pointer, window, cx);
            self.sync_handles(window, cx);
            cx.notify();
        }
    }
    pub(super) fn key(
        &mut self,
        id: u64,
        key: &str,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> bool {
        let Some(index) = self.model.handles.iter().position(|h| h.id == id) else {
            return false;
        };
        if !self.model.options.keyboard || self.disabled(index, cx) {
            return false;
        }
        let value = self.model.handles[index].value;
        let inverted = self.model.options.inverted(self.direction(cx));
        let horizontal = self.model.options.orientation == SliderOrientation::Horizontal;
        let forward = if horizontal { !inverted } else { inverted };
        let target = match key {
            "left" => self.model.scale.offset(value, if forward { -1 } else { 1 }),
            "right" => self.model.scale.offset(value, if forward { 1 } else { -1 }),
            "up" => self
                .model
                .scale
                .offset(value, if horizontal || inverted { 1 } else { -1 }),
            "down" => self
                .model
                .scale
                .offset(value, if horizontal || inverted { -1 } else { 1 }),
            "pageup" => self.model.scale.offset(value, 2),
            "pagedown" => self.model.scale.offset(value, -2),
            "home" => Some(self.model.options.min),
            "end" => Some(self.model.options.max),
            "backspace" | "delete" => {
                if !self.any_disabled(cx)
                    && let Some(proposal) = self.model.remove(id)
                {
                    self.proposal(proposal, SliderChangeSource::Keyboard, window, cx);
                }
                return self.model.editable();
            }
            _ => return false,
        };
        if let Some(target) = target {
            self.move_to(id, target, SliderChangeSource::Keyboard, window, cx);
        }
        self.key_active = Some(id);
        true
    }
    pub(super) fn move_to(
        &mut self,
        id: u64,
        value: f64,
        source: SliderChangeSource,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> bool {
        let Some(index) = self.model.handles.iter().position(|h| h.id == id) else {
            return false;
        };
        if self.disabled(index, cx) || !value.is_finite() {
            return false;
        }
        let saved = self.model.options.disabled.clone();
        self.model.options.disabled = self.disabled_settings(cx);
        let proposal = self.model.move_handle(id, value);
        self.model.options.disabled = saved;
        if let Some(proposal) = proposal {
            self.proposal(proposal, source, window, cx);
        }
        true
    }
    pub(super) fn key_up(&mut self, key: &str, window: &mut Window, cx: &mut Context<Self>) {
        self.cancel_unavailable_interaction(cx);
        if self.key_active.is_some()
            && matches!(
                key,
                "left" | "right" | "up" | "down" | "pageup" | "pagedown" | "home" | "end"
            )
        {
            self.key_active = None;
            self.complete(SliderChangeSource::Keyboard, window, cx);
        }
    }
    pub(super) fn request_frame(&mut self, window: &Window, cx: &Context<Self>) {
        if self.frame_pending {
            return;
        }
        self.frame_pending = true;
        let weak = cx.weak_entity();
        window.on_next_frame(move |_, cx| {
            let _ = weak.update(cx, |state, cx| {
                state.frame_pending = false;
                cx.notify();
            });
        });
    }
}
