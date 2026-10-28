// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::{
    TabPlacement, TabsAnimation, TabsItems, TabsType,
    geometry::{Span, reveal},
    model::{PaneFactory, TabsModel},
};
use crate::{
    ConfigScope, Direction,
    behavior::{MotionPolicy, ScalarTransition},
    input::motion::{CSS_EASE, ColorMotion},
    popup::PopupHandle,
};
use gpui::{
    AnyView, App, Bounds, Context, EventEmitter, FocusHandle, Focusable, Pixels, SharedString,
    Subscription, UniformListScrollHandle, Window,
};
use std::{
    cell::Cell,
    collections::HashMap,
    rc::Rc,
    time::{Duration, Instant},
};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TabsSource {
    Pointer,
    Keyboard,
    Overflow,
    Accessibility,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TabsChange {
    pub key: SharedString,
    pub source: TabsSource,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum TabsEdit {
    Add,
    Remove(SharedString),
}
#[derive(Clone, Debug, PartialEq)]
pub enum TabsEvent {
    Change(TabsChange),
    Activate(TabsChange),
    Edit {
        action: TabsEdit,
        source: TabsSource,
    },
    Scroll {
        offset: f32,
        vertical: bool,
        forward: bool,
    },
}
pub(super) type EventHandler = Rc<dyn Fn(&TabsEvent, &mut Window, &mut App)>;
pub(super) type MeasuredBounds = Rc<Cell<Option<Bounds<Pixels>>>>;

pub(super) struct ItemState {
    pub focus: FocusHandle,
    pub close_focus: FocusHandle,
    pub bounds: MeasuredBounds,
    pub surface_bounds: MeasuredBounds,
    pub hovered: bool,
    pub pressed: bool,
    pub color: ColorMotion,
    pub background: ColorMotion,
}
pub(super) struct PaneState {
    pub factory: PaneFactory,
    pub view: AnyView,
    pub opacity: ScalarTransition,
    pub focus: FocusHandle,
    _focus_subscription: Subscription,
}

/// Retain one controller per mounted Tabs. Item snapshots and pane entities are
/// shared across inexpensive view builders; setters do not emit user events.
pub struct TabsState {
    pub(super) model: TabsModel,
    pub(super) items: HashMap<SharedString, ItemState>,
    pub(super) panes: HashMap<SharedString, PaneState>,
    pub(super) focus: FocusHandle,
    pub(super) focused_key: Option<SharedString>,
    pub(super) scope: Option<ConfigScope>,
    pub(super) placement: TabPlacement,
    pub(super) kind: TabsType,
    pub(super) direction: Direction,
    pub(super) disabled: bool,
    pub(super) animation: TabsAnimation,
    pub(super) destroy_on_hidden: bool,
    pub(super) hide_add: bool,
    pub(super) offset: f32,
    pub(super) viewport: f32,
    pub(super) total: f32,
    pub(super) overflow: bool,
    pub(super) reserved_more: f32,
    pub(super) hidden: Vec<SharedString>,
    pub(super) spans: HashMap<SharedString, Span>,
    pub(super) reveal_key: Option<SharedString>,
    pub(super) viewport_bounds: MeasuredBounds,
    pub(super) list_bounds: MeasuredBounds,
    pub(super) popup: PopupHandle,
    pub(super) popup_focus: FocusHandle,
    pub(super) add_focus: FocusHandle,
    pub(super) more_focus: FocusHandle,
    pub(super) popup_active: Option<SharedString>,
    pub(super) popup_scroll: UniformListScrollHandle,
    pub(super) rendered_popup_rows: usize,
    pane_selection: Option<SharedString>,
    panes_initialized: bool,
    pub(super) on_event: Option<EventHandler>,
    pub(super) epoch: Instant,
    pub(super) indicator_start: ScalarTransition,
    pub(super) indicator_length: ScalarTransition,
    pub(super) indicator_initialized: bool,
    pub(super) frame_pending: bool,
    pub(super) mounted: bool,
    pub(super) generation: u64,
    scope_subscription: Option<Subscription>,
}
impl EventEmitter<TabsEvent> for TabsState {}
impl Focusable for TabsState {
    fn focus_handle(&self, _: &App) -> FocusHandle {
        self.focused_key
            .as_ref()
            .or(self.model.active.as_ref())
            .and_then(|key| self.items.get(key))
            .map_or_else(|| self.focus.clone(), |item| item.focus.clone())
    }
}
impl TabsState {
    pub fn new(items: TabsItems, _window: &mut Window, cx: &mut Context<Self>) -> Self {
        let model = TabsModel::new(items);
        let reveal_key = model.active.clone();
        let mut state = Self {
            model,
            items: HashMap::new(),
            panes: HashMap::new(),
            focus: cx.focus_handle().tab_stop(false),
            focused_key: None,
            scope: None,
            placement: TabPlacement::Top,
            kind: TabsType::Line,
            direction: Direction::Ltr,
            disabled: false,
            animation: TabsAnimation::default(),
            destroy_on_hidden: false,
            hide_add: false,
            offset: 0.,
            viewport: 0.,
            total: 0.,
            overflow: false,
            reserved_more: 0.,
            hidden: Vec::new(),
            spans: HashMap::new(),
            reveal_key,
            viewport_bounds: Rc::default(),
            list_bounds: Rc::default(),
            popup: PopupHandle::new(),
            popup_focus: cx.focus_handle(),
            popup_active: None,
            popup_scroll: UniformListScrollHandle::default(),
            add_focus: cx.focus_handle().tab_index(0),
            more_focus: cx.focus_handle().tab_index(0),
            rendered_popup_rows: 0,
            pane_selection: None,
            panes_initialized: false,
            on_event: None,
            epoch: cx.background_executor().now(),
            indicator_start: ScalarTransition::new(0., CSS_EASE),
            indicator_length: ScalarTransition::new(0., CSS_EASE),
            indicator_initialized: false,
            frame_pending: false,
            mounted: false,
            generation: 0,
            scope_subscription: None,
        };
        state.sync_items(cx);
        state
    }
    pub fn active_key(&self) -> Option<&SharedString> {
        self.model.active.as_ref()
    }
    pub fn items(&self) -> &TabsItems {
        &self.model.items
    }
    pub fn scroll_offset(&self) -> f32 {
        self.offset
    }
    /// Last native layout bounds, useful for custom tab-bar wrappers and overlays.
    pub fn tab_bounds(&self, key: &str) -> Option<Bounds<Pixels>> {
        self.items.get(key)?.bounds.get()
    }
    pub fn hidden_keys(&self) -> &[SharedString] {
        &self.hidden
    }
    pub fn pane(&self, key: &str) -> Option<&AnyView> {
        self.panes.get(key).map(|pane| &pane.view)
    }
    pub fn set_active_key(&mut self, key: Option<SharedString>, cx: &mut Context<Self>) {
        self.model.initialized = true;
        if self.model.active != key {
            self.reveal_key = key.clone();
            self.model.active = key;
            self.sync_tab_stops();
            cx.notify();
        }
    }
    pub fn set_items(&mut self, items: TabsItems, window: &mut Window, cx: &mut Context<Self>) {
        let focused = self
            .items
            .iter()
            .find(|(_, item)| item.focus.contains_focused(window, cx))
            .map(|(key, _)| key.clone())
            .or_else(|| {
                self.panes
                    .iter()
                    .find(|(_, pane)| pane.focus.contains_focused(window, cx))
                    .map(|(key, _)| key.clone())
            });
        let had_focus = focused.is_some();
        if self.model.set_items(items) {
            self.sync_items(cx);
            self.reveal_key = self.model.active.clone();
            if had_focus && focused.as_deref().is_none_or(|key| !self.enabled(key)) {
                self.focus(window, cx);
            }
            cx.notify();
        }
    }
    pub fn focus(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let key = self
            .model
            .active
            .as_ref()
            .filter(|key| self.enabled(key))
            .cloned()
            .or_else(|| self.model.next_enabled(None, 1));
        if let Some(key) = key {
            self.focus_tab(&key, window, cx);
        } else {
            self.focus.focus(window, cx);
        }
    }
    pub fn focus_tab(&mut self, key: &str, window: &mut Window, cx: &mut Context<Self>) {
        if !self.enabled(key) {
            return;
        }
        if let Some(item) = self.items.get(key) {
            item.focus.focus(window, cx);
            self.focused_key = Some(key.to_owned().into());
            self.reveal_key = self.focused_key.clone();
            self.sync_tab_stops();
            cx.notify();
        }
    }
    pub fn scroll_to(&mut self, key: &str, cx: &mut Context<Self>) {
        if self.model.items.get(key).is_some() {
            self.reveal_key = Some(key.to_owned().into());
            cx.notify();
        }
    }
    pub(super) fn configure_scope(
        &mut self,
        scope: ConfigScope,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.scope.as_ref() == Some(&scope) {
            return;
        }
        self.scope_subscription = Some(scope.observe_in(window, cx, |_, _, cx| cx.notify()));
        self.scope = Some(scope);
    }
    pub(super) fn enabled(&self, key: &str) -> bool {
        !self.disabled && self.model.items.get(key).is_some_and(|item| !item.disabled)
    }
    fn sync_items(&mut self, cx: &mut Context<Self>) {
        for item in self.model.items.entries() {
            self.items
                .entry(item.key.clone())
                .or_insert_with(|| ItemState {
                    focus: cx.focus_handle().tab_index(0),
                    bounds: Rc::default(),
                    surface_bounds: Rc::default(),
                    hovered: false,
                    close_focus: crate::behavior::focus_handle(cx),
                    pressed: false,
                    color: ColorMotion::default(),
                    background: ColorMotion::default(),
                });
        }
        self.items
            .retain(|key, _| self.model.items.get(key).is_some());
        self.panes.retain(|key, pane| {
            self.model
                .items
                .get(key)
                .and_then(|item| item.pane.as_ref())
                .is_some_and(|factory| Rc::ptr_eq(factory, &pane.factory))
        });
        self.spans
            .retain(|key, _| self.model.items.get(key).is_some());
        self.hidden
            .retain(|key| self.model.items.get(key).is_some());
        if self
            .focused_key
            .as_deref()
            .is_some_and(|key| !self.enabled(key))
        {
            self.focused_key = None;
        }
        self.sync_tab_stops();
    }
    pub(super) fn sync_tab_stops(&self) {
        let key = self
            .focused_key
            .as_ref()
            .or(self.model.active.as_ref())
            .filter(|key| self.enabled(key))
            .cloned()
            .or_else(|| {
                if self.disabled {
                    None
                } else {
                    self.model.next_enabled(None, 1)
                }
            });
        for (id, item) in &self.items {
            item.focus.clone().tab_stop(key.as_ref() == Some(id));
        }
    }
    pub(super) fn emit(&mut self, event: TabsEvent, window: &mut Window, cx: &mut Context<Self>) {
        cx.emit(event.clone());
        if let Some(callback) = self.on_event.clone() {
            // Owner callbacks may synchronously replace items or accept selection.
            // Run after the controller's mutable borrow has been released.
            window.defer(cx, move |window, cx| callback(&event, window, cx));
        }
    }
    pub(super) fn activate(
        &mut self,
        key: &str,
        source: TabsSource,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if !self.enabled(key) {
            return;
        }
        let change = TabsChange {
            key: key.to_owned().into(),
            source,
        };
        self.emit(TabsEvent::Activate(change.clone()), window, cx);
        if self.model.active.as_ref() != Some(&change.key) {
            if !self.model.controlled {
                self.model.active = Some(change.key.clone());
                self.reveal_key = self.model.active.clone();
            }
            self.emit(TabsEvent::Change(change), window, cx);
        }
        self.sync_tab_stops();
        cx.notify();
    }
    pub(super) fn edit(
        &mut self,
        action: TabsEdit,
        source: TabsSource,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.disabled || self.kind != TabsType::EditableCard {
            return;
        }
        match &action {
            TabsEdit::Add if self.hide_add => return,
            TabsEdit::Remove(key)
                if !self.enabled(key)
                    || !self.model.items.get(key).is_some_and(|item| item.closable) =>
            {
                return;
            }
            _ => {}
        }
        self.emit(TabsEvent::Edit { action, source }, window, cx);
    }
    pub(super) fn key(&mut self, key: &str, window: &mut Window, cx: &mut Context<Self>) -> bool {
        let offset = match key {
            "left" if !self.placement.vertical() => Some(if self.direction == Direction::Ltr {
                -1
            } else {
                1
            }),
            "right" if !self.placement.vertical() => Some(if self.direction == Direction::Ltr {
                1
            } else {
                -1
            }),
            "up" if self.placement.vertical() => Some(-1),
            "down" if self.placement.vertical() => Some(1),
            _ => None,
        };
        let focused = self
            .items
            .iter()
            .find(|(_, item)| item.focus.is_focused(window))
            .map(|(key, _)| key.clone())
            .or_else(|| {
                self.focused_key
                    .as_ref()
                    .or(self.model.active.as_ref())
                    .cloned()
            });
        let next = if let Some(offset) = offset {
            self.model.next_enabled(focused.as_deref(), offset)
        } else {
            match key {
                "home" => self.model.next_enabled(None, 1),
                "end" => self.model.next_enabled(None, -1),
                "enter" | "space" => {
                    if let Some(key) = focused {
                        self.activate(&key, TabsSource::Keyboard, window, cx);
                    }
                    return true;
                }
                "delete" | "backspace" if self.kind == TabsType::EditableCard => {
                    if let Some(key) = focused {
                        self.edit(TabsEdit::Remove(key), TabsSource::Keyboard, window, cx);
                    }
                    return true;
                }
                _ => return false,
            }
        };
        if let Some(key) = next {
            self.focus_tab(&key, window, cx);
        }
        true
    }
    pub(super) fn now(&self, cx: &App) -> Duration {
        cx.background_executor().now().duration_since(self.epoch)
    }
    pub(super) fn request_frame(&mut self, window: &Window, cx: &Context<Self>) {
        if self.frame_pending || !self.mounted {
            return;
        }
        self.frame_pending = true;
        let weak = cx.weak_entity();
        let generation = self.generation;
        window.on_next_frame(move |_, cx| {
            let _ = weak.update(cx, |s, cx| {
                if s.mounted && s.generation == generation {
                    s.frame_pending = false;
                    cx.notify();
                }
            });
        });
    }
    pub(super) fn scroll(
        &mut self,
        amount: f32,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> bool {
        if !amount.is_finite() {
            return false;
        }
        let offset = (self.offset + amount).clamp(0., (self.total - self.viewport).max(0.));
        if (offset - self.offset).abs() < 0.01 {
            return false;
        }
        let forward = offset > self.offset;
        self.offset = offset;
        self.reveal_key = None;
        self.emit(
            TabsEvent::Scroll {
                offset,
                vertical: self.placement.vertical(),
                forward,
            },
            window,
            cx,
        );
        cx.notify();
        true
    }
    pub(super) fn measure(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let (Some(viewport), Some(list)) = (self.viewport_bounds.get(), self.list_bounds.get())
        else {
            return;
        };
        let vertical = self.placement.vertical();
        self.viewport = f32::from(if vertical {
            viewport.size.height
        } else {
            viewport.size.width
        });
        self.total = f32::from(if vertical {
            list.size.height
        } else {
            list.size.width
        });
        let overflow = self.total > self.viewport + self.reserved_more + 0.5;
        if self.overflow != overflow {
            self.overflow = overflow;
            cx.notify();
        }
        for (key, item) in &self.items {
            if let Some(bounds) = item.bounds.get() {
                let start = if vertical {
                    bounds.top() - list.top()
                } else if self.direction == Direction::Rtl {
                    list.right() - bounds.right()
                } else {
                    bounds.left() - list.left()
                };
                self.spans.insert(
                    key.clone(),
                    Span {
                        start: f32::from(start),
                        length: f32::from(if vertical {
                            bounds.size.height
                        } else {
                            bounds.size.width
                        }),
                    },
                );
            }
        }
        // Decide overflow against the viewport before reserving the more button.
        // Its measured extent is supplied by the view in the available-space
        // calculation, avoiding alternating overflow/no-overflow layouts.
        let max = (self.total - self.viewport).max(0.);
        let mut offset = self.offset.clamp(0., max);
        if let Some(key) = self.reveal_key.take()
            && let Some(span) = self.spans.get(&key)
        {
            offset = reveal(*span, offset, self.viewport, self.total);
        }
        if (offset - self.offset).abs() > 0.01 {
            self.offset = offset;
            cx.notify();
        }
        let hidden: Vec<_> = self
            .model
            .items
            .entries()
            .iter()
            .filter(|item| {
                self.spans
                    .get(&item.key)
                    .is_some_and(|span| !span.visible(self.offset, self.viewport))
            })
            .map(|item| item.key.clone())
            .collect();
        if self.hidden != hidden {
            self.hidden = hidden;
            cx.notify();
        }
        if self.hidden.is_empty() && self.popup.is_open(cx) {
            let popup = self.popup.clone();
            window.defer(cx, move |window, cx| popup.close(window, cx));
        }
    }
    pub(super) fn sync_panes(
        &mut self,
        duration: Duration,
        motion: MotionPolicy,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let now = self.now(cx);
        let items = self.model.items.clone();
        let animate = motion.allows_motion() && self.animation.panes;
        let entering = self.panes_initialized && self.pane_selection != self.model.active;
        if entering
            && self
                .pane_selection
                .as_ref()
                .and_then(|key| self.panes.get(key))
                .is_some_and(|pane| pane.focus.contains_focused(window, cx))
        {
            self.focus(window, cx);
        }
        for item in items.entries() {
            let active = self.model.active.as_ref() == Some(&item.key);
            if (active || item.force_render)
                && !self.panes.contains_key(&item.key)
                && let Some(factory) = &item.pane
            {
                let view = factory(window, cx);
                let focus = cx.focus_handle().tab_index(0);
                let weak = cx.weak_entity();
                let key = item.key.clone();
                let subscription = window.on_focus_in(&focus, cx, move |window, cx| {
                    let weak = weak.clone();
                    let key = key.clone();
                    window.defer(cx, move |window, cx| {
                        let _ = weak.update(cx, |s, cx| {
                            if s.model.active.as_ref() != Some(&key) {
                                s.focus(window, cx);
                            }
                        });
                    });
                });
                self.panes.insert(
                    item.key.clone(),
                    PaneState {
                        factory: factory.clone(),
                        view,
                        focus,
                        _focus_subscription: subscription,
                        opacity: ScalarTransition::new(
                            if active && !(animate && entering) {
                                1.
                            } else {
                                0.
                            },
                            CSS_EASE,
                        ),
                    },
                );
            }
            if let Some(pane) = self.panes.get_mut(&item.key) {
                pane.opacity.retarget(
                    if active { 1. } else { 0. },
                    now,
                    duration,
                    MotionPolicy::new(animate, false),
                );
            }
        }
        self.panes.retain(|key, pane| {
            self.model.active.as_ref() == Some(key)
                || items.get(key).is_some_and(|item| item.force_render)
                || !items
                    .get(key)
                    .and_then(|item| item.destroy_on_hidden)
                    .unwrap_or(self.destroy_on_hidden)
                || !pane.opacity.sample(now).finished
        });
        self.pane_selection = self.model.active.clone();
        self.panes_initialized = true;
        if self
            .panes
            .values()
            .any(|pane| !pane.opacity.sample(now).finished)
        {
            self.request_frame(window, cx);
        }
    }
}
