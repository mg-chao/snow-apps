// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::{MenuItems, MenuKey, MenuMode, MenuOptions, MenuTrigger};
use crate::{
    ConfigScope, Direction,
    popup::{PopupHandle, PopupOpenChange},
};
use gpui::{
    App, Context, Entity, EventEmitter, FocusHandle, Focusable, KeyDownEvent, SharedString,
    Subscription, Task, Window,
};
use std::{
    collections::{HashMap, HashSet},
    rc::Rc,
    time::{Duration, Instant},
};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MenuSource {
    Pointer,
    Keyboard,
    Accessibility,
    Programmatic,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct MenuActivation {
    pub key: MenuKey,
    /// Activated item first, then its ancestor submenus.
    pub key_path: Vec<MenuKey>,
    pub label: SharedString,
    pub source: MenuSource,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct MenuSelection {
    pub activation: MenuActivation,
    pub selected_keys: Vec<MenuKey>,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum MenuEvent {
    Activate(MenuActivation),
    Select(MenuSelection),
    Deselect(MenuSelection),
    OpenChange {
        open_keys: Vec<MenuKey>,
        source: MenuSource,
    },
    TitleClick(MenuActivation),
    Focus,
    Blur,
}
pub(super) type EventHandler = Rc<dyn Fn(&MenuEvent, &mut Window, &mut App)>;

/// Owns interaction state independently of the cheap, render-time Menu builder.
pub struct MenuState {
    pub(super) items: MenuItems,
    pub(super) options: MenuOptions,
    pub(super) selected: Vec<MenuKey>,
    pub(super) open: Vec<MenuKey>,
    pub(super) active: Option<MenuKey>,
    pub(super) focus: FocusHandle,
    pub(super) row_focus: HashMap<MenuKey, FocusHandle>,
    pub(super) popups: HashMap<MenuKey, PopupHandle>,
    pub(super) scope: Option<ConfigScope>,
    pub(super) on_event: Option<EventHandler>,
    pub(super) disabled: bool,
    pub(super) mounted: bool,
    pub(super) revision: u64,
    pub(super) overflow_start: usize,
    pub(super) overflow_open: bool,
    pub(super) overflow_handle: PopupHandle,
    pub(super) overflow_focus: FocusHandle,
    controlled_selected: bool,
    controlled_open: bool,
    initialized_selected: bool,
    initialized_open: bool,
    inline_cache: Vec<MenuKey>,
    generation: u64,
    hover_tasks: HashMap<MenuKey, (u64, Task<()>)>,
    hover_sequence: u64,
    hovered_rows: HashSet<MenuKey>,
    hovered_levels: HashSet<MenuKey>,
    query: String,
    query_time: Option<Instant>,
    _scope: Option<Subscription>,
    _focus_subscriptions: Vec<Subscription>,
    has_focus: bool,
    focus_repair_from: Vec<FocusHandle>,
    pending_child_focus: Option<MenuKey>,
}
impl EventEmitter<MenuEvent> for MenuState {}
impl Focusable for MenuState {
    fn focus_handle(&self, _: &App) -> FocusHandle {
        self.focus.clone()
    }
}
impl MenuState {
    pub fn new(items: MenuItems, window: &mut Window, cx: &mut Context<Self>) -> Self {
        let active = items.visible_keys(&[], false, None).first().cloned();
        let focus = crate::behavior::focus_handle(cx);
        let weak = cx.weak_entity();
        let focused = window.on_focus_in(&focus, cx, move |window, cx| {
            if let Some(state) = weak.upgrade() {
                let changed = state.update(cx, |s, _| {
                    if s.disabled || !s.mounted || s.has_focus {
                        false
                    } else {
                        s.has_focus = true;
                        true
                    }
                });
                if changed {
                    emit(&state, MenuEvent::Focus, window, cx);
                }
            }
        });
        let weak = cx.weak_entity();
        let blurred = window.on_focus_out(&focus, cx, move |_, window, cx| {
            let weak = weak.clone();
            window.defer(cx, move |window, cx| {
                if let Some(state) = weak.upgrade() {
                    let owns_focus = {
                        let s = state.read(cx);
                        s.focus.contains_focused(window, cx)
                            || s.row_focus.values().any(|focus| focus.is_focused(window))
                            || s.overflow_focus.is_focused(window)
                    };
                    let changed = state.update(cx, |s, _| {
                        if !owns_focus && s.has_focus {
                            s.has_focus = false;
                            s.cancel_pending();
                            true
                        } else {
                            false
                        }
                    });
                    if changed {
                        emit(&state, MenuEvent::Blur, window, cx);
                    }
                }
            });
        });
        Self {
            items,
            options: MenuOptions::default(),
            selected: Vec::new(),
            open: Vec::new(),
            active,
            focus,
            row_focus: HashMap::new(),
            popups: HashMap::new(),
            scope: None,
            on_event: None,
            disabled: false,
            mounted: false,
            revision: 0,
            overflow_start: usize::MAX,
            overflow_open: false,
            overflow_handle: PopupHandle::new(),
            overflow_focus: cx.focus_handle().tab_stop(false),
            controlled_selected: false,
            controlled_open: false,
            initialized_selected: false,
            initialized_open: false,
            inline_cache: Vec::new(),
            generation: 0,
            hover_tasks: HashMap::new(),
            hover_sequence: 0,
            hovered_rows: HashSet::new(),
            hovered_levels: HashSet::new(),
            query: String::new(),
            query_time: None,
            _scope: None,
            _focus_subscriptions: vec![focused, blurred],
            has_focus: false,
            focus_repair_from: Vec::new(),
            pending_child_focus: None,
        }
    }
    pub fn items(&self) -> &MenuItems {
        &self.items
    }
    pub fn options(&self) -> &MenuOptions {
        &self.options
    }
    pub fn selected_keys(&self) -> &[MenuKey] {
        &self.selected
    }
    pub fn open_keys(&self) -> &[MenuKey] {
        &self.open
    }
    pub fn active_key(&self) -> Option<&MenuKey> {
        self.active.as_ref()
    }
    pub fn set_items(&mut self, items: MenuItems, cx: &mut Context<Self>) {
        if self.items.same_collection(&items) {
            return;
        }
        self.cancel_pending();
        self.remember_active_focus();
        self.items = items;
        self.revision = self.revision.wrapping_add(1);
        self.row_focus.retain(|key, _| self.items.enabled(key));
        self.popups.retain(|key, _| self.items.is_submenu(key));
        self.repair_active();
        cx.notify();
    }
    pub fn set_selected_keys<K: Into<MenuKey>>(
        &mut self,
        keys: impl IntoIterator<Item = K>,
        cx: &mut Context<Self>,
    ) {
        self.initialized_selected = true;
        let keys = unique(keys);
        if self.selected != keys {
            self.selected = keys;
            cx.notify();
        }
    }
    pub fn set_open_keys<K: Into<MenuKey>>(
        &mut self,
        keys: impl IntoIterator<Item = K>,
        cx: &mut Context<Self>,
    ) {
        self.initialized_open = true;
        let keys = unique(keys);
        if self.open != keys {
            self.cancel_pending();
            self.remember_active_focus();
            self.open = keys;
            if self.options.is_inline() {
                self.inline_cache = self.open.clone();
            }
            self.repair_active();
            cx.notify();
        }
    }
    pub fn focus(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        self.repair_active();
        if self.options.mode == MenuMode::Horizontal
            && !self.overflow_open
            && self.active.as_ref().is_some_and(|key| {
                self.items
                    .root_position(key)
                    .is_some_and(|index| index >= self.overflow_start)
            })
        {
            window.focus(&self.overflow_focus, cx);
            return;
        }
        let focus = self
            .active
            .as_ref()
            .and_then(|key| self.row_focus.get(key))
            .unwrap_or(&self.focus);
        window.focus(focus, cx);
    }
    pub fn blur(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if self.focus.contains_focused(window, cx)
            || self
                .row_focus
                .values()
                .any(|focus| focus.is_focused(window))
            || self.overflow_focus.is_focused(window)
        {
            window.blur(cx);
        }
    }
    pub(super) fn bind_scope(&mut self, scope: ConfigScope, cx: &mut Context<Self>) {
        if self
            .scope
            .as_ref()
            .is_some_and(|s| s.identity() == scope.identity())
        {
            return;
        }
        self._scope = Some(scope.observe(cx));
        self.scope = Some(scope);
    }
    pub(super) fn configure(
        &mut self,
        mut options: MenuOptions,
        selected: Option<Vec<MenuKey>>,
        default_selected: Option<Vec<MenuKey>>,
        open: Option<Vec<MenuKey>>,
        default_open: Option<Vec<MenuKey>>,
        cx: &mut Context<Self>,
    ) {
        if !options.inline_indent.is_finite() || options.inline_indent < 0. {
            options.inline_indent = 24.;
        }
        let old_inline = self.options.is_inline();
        let new_inline = options.is_inline();
        if self.options != options {
            self.cancel_pending();
        }
        if old_inline {
            self.inline_cache = self.open.clone();
        }
        if self.options.mode != options.mode
            || self.options.inline_collapsed != options.inline_collapsed
        {
            self.remember_active_focus();
            self.overflow_open = false;
            if open.is_none() {
                if old_inline && !new_inline {
                    self.open.clear();
                } else if !old_inline && new_inline {
                    self.open = self.inline_cache.clone();
                } else if !new_inline {
                    self.open.clear();
                }
            }
        }
        self.options = options;
        self.controlled_selected = selected.is_some();
        self.controlled_open = open.is_some();
        if let Some(keys) = selected {
            self.selected = unique(keys);
            self.initialized_selected = true;
        } else if !self.initialized_selected {
            if let Some(keys) = default_selected {
                self.selected = unique(keys);
            }
            self.initialized_selected = true;
        }
        if let Some(keys) = open {
            let keys = unique(keys);
            if self.open != keys {
                self.remember_active_focus();
            }
            self.open = keys;
            self.initialized_open = true;
        } else if !self.initialized_open {
            if let Some(keys) = default_open {
                self.open = unique(keys);
            }
            self.initialized_open = true;
        }
        if new_inline {
            self.inline_cache = self.open.clone();
        }
        self.repair_active();
        let _ = cx;
    }
    fn repair_active(&mut self) {
        if self
            .active
            .as_ref()
            .is_some_and(|k| self.visible_enabled(k))
        {
            return;
        }
        let ancestor = self.active.as_ref().and_then(|key| {
            self.items
                .path(key)
                .into_iter()
                .skip(1)
                .find(|key| self.visible_enabled(key))
        });
        self.active = ancestor.or_else(|| {
            self.selected
                .iter()
                .find(|key| self.visible_enabled(key))
                .cloned()
                .or_else(|| {
                    self.items
                        .visible_keys(&self.open, self.options.is_inline(), None)
                        .first()
                        .cloned()
                })
        });
    }
    fn visible_enabled(&self, key: &str) -> bool {
        self.items.enabled(key)
            && self
                .items
                .path(key)
                .iter()
                .skip(1)
                .all(|parent| self.open.contains(parent))
    }
    fn remember_active_focus(&mut self) {
        if let Some(focus) = self.active.as_ref().and_then(|key| self.row_focus.get(key)) {
            self.focus_repair_from.push(focus.clone());
        }
    }
    pub(super) fn repair_rendered_focus(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if let Some(parent) = self.pending_child_focus.take()
            && self.open.contains(&parent)
            && (self.focus.is_focused(window)
                || self
                    .row_focus
                    .get(&parent)
                    .is_some_and(|focus| focus.is_focused(window)))
            && let Some(child) = self
                .items
                .visible_keys(&self.open, false, Some(&parent))
                .first()
                .cloned()
        {
            let weak = cx.weak_entity();
            window.defer(cx, move |window, cx| {
                if let Some(state) = weak.upgrade()
                    && state.read(cx).open.contains(&parent)
                {
                    focus_key(&state, child, window, cx);
                }
            });
        }
        let displaced = std::mem::take(&mut self.focus_repair_from);
        if displaced.iter().any(|focus| focus.is_focused(window)) {
            // Focus the replacement before GPUI retires the removed row. The
            // check above preserves any focus chosen by an application callback.
            let focus = if let Some(key) = self.active.clone() {
                self.row_handle(&key, cx)
            } else {
                self.focus.clone()
            };
            window.focus(&focus, cx);
        }
    }
    fn cancel_pending(&mut self) {
        self.generation = self.generation.wrapping_add(1);
        self.hover_tasks.clear();
        self.hovered_rows.clear();
        self.hovered_levels.clear();
        self.pending_child_focus = None;
    }
    fn hover_interested(&self, key: &str) -> bool {
        self.hovered_rows
            .iter()
            .chain(&self.hovered_levels)
            .any(|candidate| candidate.as_ref() == key || self.items.descendant_of(candidate, key))
    }
    pub(super) fn unmount(&mut self) {
        self.cancel_pending();
        self.mounted = false;
        self.on_event = None;
        self.overflow_open = false;
        self.query.clear();
        self.has_focus = false;
    }
    pub(super) fn selected_descendant(&self, key: &str) -> bool {
        self.selected
            .iter()
            .any(|selected| selected.as_ref() == key || self.items.descendant_of(selected, key))
    }
    pub(super) fn row_handle(&mut self, key: &MenuKey, cx: &mut App) -> FocusHandle {
        self.row_focus
            .entry(key.clone())
            .or_insert_with(|| cx.focus_handle().tab_stop(false))
            .clone()
    }
}
fn unique<K: Into<MenuKey>>(keys: impl IntoIterator<Item = K>) -> Vec<MenuKey> {
    let mut seen = HashSet::new();
    keys.into_iter()
        .map(Into::into)
        .filter(|key| seen.insert(key.clone()))
        .collect()
}
fn emit(state: &Entity<MenuState>, event: MenuEvent, window: &mut Window, cx: &mut App) {
    let handler = state.update(cx, |s, cx| {
        cx.emit(event.clone());
        s.on_event.clone()
    });
    if let Some(handler) = handler {
        handler(&event, window, cx);
    }
}
fn activation(s: &MenuState, key: &MenuKey, source: MenuSource) -> Option<MenuActivation> {
    if s.disabled || !s.mounted || !s.items.enabled(key) {
        return None;
    }
    let item = s.items.item(key)?;
    Some(MenuActivation {
        key: key.clone(),
        key_path: s.items.path(key),
        label: item.label.clone(),
        source,
    })
}
pub(super) fn activate(
    state: &Entity<MenuState>,
    key: &MenuKey,
    source: MenuSource,
    window: &mut Window,
    cx: &mut App,
) {
    if state.read(cx).items.is_submenu(key) {
        if let Some(info) = activation(state.read(cx), key, source) {
            emit(state, MenuEvent::TitleClick(info), window, cx);
        }
        let next = !state.read(cx).open.contains(key);
        toggle(state, key, next, source, window, cx);
        return;
    }
    let Some(info) = activation(state.read(cx), key, source) else {
        return;
    };
    let revision = state.read(cx).revision;
    emit(state, MenuEvent::Activate(info.clone()), window, cx);
    if state.read(cx).revision != revision || !state.read(cx).items.enabled(key) {
        return;
    }
    let selection = state.update(cx, |s, cx| {
        s.active = Some(key.clone());
        if !s.options.selectable {
            return None;
        }
        let existed = s.selected.contains(key);
        let mut selected = s.selected.clone();
        let deselect = s.options.multiple && existed;
        if deselect {
            selected.retain(|k| k != key);
        } else if s.options.multiple {
            selected.push(key.clone());
        } else {
            selected = vec![key.clone()];
        }
        if !s.controlled_selected {
            s.selected = selected.clone();
            cx.notify();
        }
        if existed && !s.options.multiple {
            None
        } else {
            Some((deselect, selected))
        }
    });
    if let Some((deselect, selected_keys)) = selection {
        let change = MenuSelection {
            activation: info,
            selected_keys,
        };
        emit(
            state,
            if deselect {
                MenuEvent::Deselect(change)
            } else {
                MenuEvent::Select(change)
            },
            window,
            cx,
        );
    }
    if !state.read(cx).options.multiple && !state.read(cx).options.is_inline() {
        propose_open(state, Vec::new(), source, window, cx);
        state.update(cx, |s, cx| {
            s.overflow_open = false;
            cx.notify();
        });
    }
}
fn propose_open(
    state: &Entity<MenuState>,
    keys: Vec<MenuKey>,
    source: MenuSource,
    window: &mut Window,
    cx: &mut App,
) {
    if state.read(cx).open == keys {
        return;
    }
    state.update(cx, |s, cx| {
        if !s.controlled_open {
            s.remember_active_focus();
            s.open = keys.clone();
            if s.options.is_inline() {
                s.inline_cache = s.open.clone();
            }
            s.repair_active();
            cx.notify();
        }
    });
    emit(
        state,
        MenuEvent::OpenChange {
            open_keys: keys,
            source,
        },
        window,
        cx,
    );
}
pub(super) fn toggle(
    state: &Entity<MenuState>,
    key: &MenuKey,
    open: bool,
    source: MenuSource,
    window: &mut Window,
    cx: &mut App,
) {
    let s = state.read(cx);
    if s.disabled || !s.mounted || !s.items.enabled(key) || !s.items.is_submenu(key) {
        return;
    }
    let inline = s.options.is_inline();
    let mut keys = s.open.clone();
    keys.retain(|k| k != key);
    if open {
        if !inline {
            let parent = s.items.parent(key);
            let siblings: Vec<_> = keys
                .iter()
                .filter(|candidate| {
                    s.items.is_submenu(candidate)
                        && s.items.parent(candidate) == parent
                        && !s.items.descendant_of(key, candidate)
                })
                .cloned()
                .collect();
            keys.retain(|candidate| {
                !siblings.iter().any(|sibling| {
                    candidate == sibling || s.items.descendant_of(candidate, sibling)
                })
            });
        }
        keys.push(key.clone());
    } else if !inline {
        keys.retain(|k| !s.items.descendant_of(k, key));
    }
    let restore = (!open).then(|| s.row_focus.get(key).cloned()).flatten();
    propose_open(state, keys, source, window, cx);
    if !open
        && source == MenuSource::Keyboard
        && let Some(focus) = restore
    {
        window.focus(&focus, cx);
    }
}
pub(super) fn popup_changed(
    state: &Entity<MenuState>,
    key: &MenuKey,
    change: &PopupOpenChange,
    window: &mut Window,
    cx: &mut App,
) {
    let source = match change.reason {
        crate::popup::PopupOpenReason::Accessibility => MenuSource::Accessibility,
        crate::popup::PopupOpenReason::Escape => MenuSource::Keyboard,
        crate::popup::PopupOpenReason::Programmatic => MenuSource::Programmatic,
        _ => MenuSource::Pointer,
    };
    toggle(state, key, change.open, source, window, cx);
}
pub(super) fn hover(
    state: &Entity<MenuState>,
    key: &MenuKey,
    hovered: bool,
    window: &mut Window,
    cx: &mut App,
) {
    state.update(cx, |s, _| {
        if hovered {
            s.hovered_rows.insert(key.clone());
        } else {
            s.hovered_rows.remove(key);
        }
    });
    if hovered {
        state.update(cx, |s, cx| {
            if s.items.enabled(key) && !s.disabled && s.active.as_ref() != Some(key) {
                s.active = Some(key.clone());
                cx.notify();
            }
        });
    }
    if state.read(cx).options.is_inline() || state.read(cx).options.trigger != MenuTrigger::Hover {
        return;
    }
    if state.read(cx).items.is_submenu(key) {
        let interested = state.read(cx).hover_interested(key);
        schedule_hover(state, key, interested, window, cx);
    } else if hovered {
        let sibling = {
            let s = state.read(cx);
            s.open
                .iter()
                .find(|candidate| s.items.parent(candidate) == s.items.parent(key))
                .cloned()
        };
        if let Some(sibling) = sibling {
            schedule_hover(state, &sibling, false, window, cx);
        }
    }
}
pub(super) fn hover_level(
    state: &Entity<MenuState>,
    parent: Option<&MenuKey>,
    hovered: bool,
    window: &mut Window,
    cx: &mut App,
) {
    if state.read(cx).options.trigger != MenuTrigger::Hover {
        return;
    }
    if let Some(parent) = parent {
        state.update(cx, |s, _| {
            if hovered {
                s.hovered_levels.insert(parent.clone());
            } else {
                s.hovered_levels.remove(parent);
            }
        });
        let interested = state.read(cx).hover_interested(parent);
        schedule_hover(state, parent, interested, window, cx);
    }
}
fn schedule_hover(
    state: &Entity<MenuState>,
    key: &MenuKey,
    open: bool,
    window: &mut Window,
    cx: &mut App,
) {
    let key = key.clone();
    state.update(cx, |s, cx| {
        if s.disabled || !s.mounted {
            return;
        }
        let generation = s.generation;
        s.hover_sequence = s.hover_sequence.wrapping_add(1);
        let sequence = s.hover_sequence;
        let delay = if open {
            s.options.open_delay
        } else {
            s.options.close_delay
        };
        let weak = cx.entity().downgrade();
        let pending_key = key.clone();
        let task = window.spawn(cx, async move |cx| {
            cx.background_executor().timer(delay).await;
            let _ = cx.update(|window, cx| {
                if let Some(state) = weak.upgrade()
                    && state.read(cx).generation == generation
                    && state.read(cx).mounted
                    && state
                        .read(cx)
                        .hover_tasks
                        .get(&pending_key)
                        .is_some_and(|(id, _)| *id == sequence)
                {
                    let interested = state.read(cx).hover_interested(&pending_key);
                    if interested == open {
                        toggle(&state, &pending_key, open, MenuSource::Pointer, window, cx);
                    }
                }
            });
        });
        s.hover_tasks.insert(key, (sequence, task));
    });
}
pub(super) fn focus_key(
    state: &Entity<MenuState>,
    key: MenuKey,
    window: &mut Window,
    cx: &mut App,
) {
    let focus = state.update(cx, |s, cx| {
        if s.disabled || !s.items.enabled(&key) {
            return None;
        }
        s.active = Some(key.clone());
        let focus = s.row_handle(&key, cx);
        cx.notify();
        Some(focus)
    });
    if let Some(focus) = focus {
        window.focus(&focus, cx);
    }
}
pub(super) fn key(
    state: &Entity<MenuState>,
    parent: Option<&MenuKey>,
    event: &KeyDownEvent,
    window: &mut Window,
    cx: &mut App,
) {
    if state.read(cx).disabled {
        return;
    }
    if event.is_held && matches!(event.keystroke.key.as_str(), "enter" | "space") {
        cx.stop_propagation();
        return;
    }
    let keystroke = &event.keystroke;
    if keystroke.modifiers.control || keystroke.modifiers.platform || keystroke.modifiers.alt {
        return;
    }
    let s = state.read(cx);
    if let Some(parent) = parent
        && (!s.open.contains(parent) || !s.visible_enabled(parent))
    {
        cx.stop_propagation();
        return;
    }
    let inline = s.options.is_inline();
    let root_horizontal = s.options.mode == MenuMode::Horizontal && parent.is_none();
    let overflow_popup = root_horizontal && s.overflow_open;
    let horizontal = root_horizontal && !overflow_popup;
    let rtl = s
        .scope
        .as_ref()
        .is_some_and(|scope| scope.snapshot(cx).direction == Direction::Rtl);
    let forward = if rtl { "left" } else { "right" };
    let backward = if rtl { "right" } else { "left" };
    let mut keys = s.items.visible_keys(
        &s.open,
        inline,
        if inline {
            None
        } else {
            parent.map(AsRef::as_ref)
        },
    );
    if root_horizontal {
        keys.retain(|key| {
            s.items.root_position(key).is_some_and(|index| {
                if s.overflow_open {
                    index >= s.overflow_start
                } else {
                    index < s.overflow_start
                }
            })
        });
    }
    let active = s.active.clone().filter(|key| keys.contains(key));
    let position = if s.focus.is_focused(window) {
        None
    } else {
        active
            .as_ref()
            .and_then(|key| keys.iter().position(|k| k == key))
    };
    let input = keystroke.key.as_str();
    let has_overflow = s.overflow_start < s.items.len();
    let overflow_focused = s.overflow_focus.is_focused(window);
    if horizontal && has_overflow {
        let enter_overflow =
            (overflow_focused || keys.is_empty()) && matches!(input, "down" | "enter" | "space");
        if enter_overflow {
            let first = s
                .items
                .visible_keys(&s.open, false, None)
                .into_iter()
                .find(|key| {
                    s.items
                        .root_position(key)
                        .is_some_and(|index| index >= s.overflow_start)
                });
            state.update(cx, |s, cx| {
                s.overflow_open = true;
                cx.notify();
            });
            if let Some(first) = first {
                let state = state.clone();
                window.defer(cx, move |window, cx| focus_key(&state, first, window, cx));
            }
            cx.stop_propagation();
            return;
        }
        let into_indicator = input == "end"
            || !overflow_focused
                && (input == forward && position == keys.len().checked_sub(1)
                    || input == backward && position == Some(0));
        if into_indicator {
            let focus = s.overflow_focus.clone();
            state.update(cx, |s, cx| {
                s.active = None;
                cx.notify();
            });
            window.focus(&focus, cx);
            cx.stop_propagation();
            return;
        }
        if overflow_focused && (input == forward || input == backward) {
            let next = if input == forward {
                keys.first()
            } else {
                keys.last()
            }
            .cloned();
            if let Some(next) = next {
                focus_key(state, next, window, cx);
            }
            cx.stop_propagation();
            return;
        }
    }
    let delta = if input == "down" && !horizontal || input == forward && horizontal {
        Some(1isize)
    } else if input == "up" && !horizontal || input == backward && horizontal {
        Some(-1)
    } else {
        None
    };
    if let Some(delta) = delta {
        if !keys.is_empty() {
            let index = position
                .map(|i| i as isize)
                .unwrap_or(if delta > 0 { -1 } else { 0 });
            let next = (index + delta).rem_euclid(keys.len() as isize) as usize;
            focus_key(state, keys[next].clone(), window, cx);
        }
    } else if input == "home" || input == "end" {
        if let Some(key) = if input == "home" {
            keys.first()
        } else {
            keys.last()
        } {
            focus_key(state, key.clone(), window, cx);
        }
    } else if input == "escape" || input == backward && !inline && !horizontal {
        if let Some(parent) = parent {
            toggle(state, parent, false, MenuSource::Keyboard, window, cx);
        } else {
            propose_open(state, Vec::new(), MenuSource::Keyboard, window, cx);
            state.update(cx, |s, cx| {
                s.overflow_open = false;
                cx.notify();
            });
            if overflow_popup {
                let focus = state.read(cx).overflow_focus.clone();
                window.focus(&focus, cx);
            }
        }
    } else if input == "enter"
        || input == "space"
        || input == forward && !horizontal
        || input == "down" && horizontal
    {
        if let Some(active) = active.or_else(|| keys.first().cloned()) {
            if state.read(cx).items.is_submenu(&active) {
                if inline && (input == "enter" || input == "space") {
                    activate(state, &active, MenuSource::Keyboard, window, cx);
                } else {
                    toggle(state, &active, true, MenuSource::Keyboard, window, cx);
                    if !state.read(cx).open.contains(&active) {
                        state.update(cx, |s, cx| {
                            s.pending_child_focus = Some(active);
                            cx.notify();
                        });
                        cx.stop_propagation();
                        return;
                    }
                    let child = state
                        .read(cx)
                        .items
                        .visible_keys(&state.read(cx).open, false, Some(&active))
                        .first()
                        .cloned();
                    if let Some(child) = child {
                        let weak = state.downgrade();
                        window.defer(cx, move |window, cx| {
                            if let Some(state) = weak.upgrade() {
                                focus_key(&state, child, window, cx);
                            }
                        });
                    }
                }
            } else if input == "enter" || input == "space" {
                activate(state, &active, MenuSource::Keyboard, window, cx);
            }
        }
    } else if input == backward && inline {
        if let Some(active) = active {
            if state.read(cx).open.contains(&active) {
                toggle(state, &active, false, MenuSource::Keyboard, window, cx);
            } else if let Some(parent) = state.read(cx).items.parent(&active).cloned() {
                focus_key(state, parent, window, cx);
            }
        }
    } else if keystroke.key.chars().count() == 1 {
        let now = cx.background_executor().now();
        let query = state.update(cx, |s, _| {
            if s.query_time
                .is_none_or(|last| now.duration_since(last) > Duration::from_millis(500))
            {
                s.query.clear();
            }
            s.query_time = Some(now);
            s.query.push_str(&input.to_lowercase());
            s.query.clone()
        });
        let repeated = query
            .chars()
            .next()
            .is_some_and(|first| query.chars().all(|ch| ch == first));
        let search = if repeated {
            query.chars().next().unwrap().to_string()
        } else {
            query
        };
        let start = position.map_or(0, |i| i + 1);
        if let Some(found) = (0..keys.len())
            .map(|i| &keys[(start + i) % keys.len()])
            .find(|key| {
                state
                    .read(cx)
                    .items
                    .item(key)
                    .is_some_and(|item| item.label.to_lowercase().starts_with(&search))
            })
            .cloned()
        {
            focus_key(state, found, window, cx);
        }
    } else {
        return;
    }
    cx.stop_propagation();
}

pub(super) fn traverse(
    state: &Entity<MenuState>,
    previous: bool,
    window: &mut Window,
    cx: &mut App,
) {
    if !state.read(cx).options.is_inline() {
        propose_open(state, Vec::new(), MenuSource::Keyboard, window, cx);
    }
    state.update(cx, |s, cx| {
        s.cancel_pending();
        s.overflow_open = false;
        cx.notify();
    });
    let focus = state.read(cx).focus.clone();
    window.focus(&focus, cx);
    if previous {
        window.focus_prev(cx);
    } else {
        window.focus_next(cx);
    }
}
