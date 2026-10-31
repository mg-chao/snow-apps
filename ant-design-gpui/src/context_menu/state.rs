// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::{MenuEntry, MenuItem, MenuItems};
use crate::{
    ConfigScope, Direction,
    popup::{PopupHandle, PopupOpenChange, PopupOpenReason},
};
use gpui::{
    App, Bounds, Context, ElementId, Entity, EventEmitter, FocusHandle, Focusable, KeyDownEvent,
    Pixels, Point, ScrollHandle, SharedString, Subscription, Task, Window,
};
use std::{
    rc::Rc,
    time::{Duration, Instant},
};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MenuActivationSource {
    Pointer,
    Keyboard,
    Accessibility,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct MenuActivation {
    pub id: ElementId,
    pub label: SharedString,
    pub checked: Option<bool>,
    pub source: MenuActivationSource,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum MenuEvent {
    Activate(MenuActivation),
    OpenChange { open: bool, reason: PopupOpenReason },
}
pub(super) type EventHandler = Rc<dyn Fn(&MenuEvent, &mut Window, &mut App)>;

pub(super) struct Level {
    pub items: MenuItems,
    pub active: Option<usize>,
    pub parent_index: Option<usize>,
    pub focus: FocusHandle,
    pub scroll: ScrollHandle,
    pub native_list: Option<gpui::ListState>,
    pub width_cache: Option<super::view::MetricsCache>,
}
impl Level {
    fn new(items: MenuItems, parent_index: Option<usize>, cx: &mut App) -> Self {
        let active = items.next(None, 1);
        Self {
            items,
            active,
            parent_index,
            focus: cx.focus_handle(),
            scroll: ScrollHandle::new(),
            native_list: None,
            width_cache: None,
        }
    }
    pub(super) fn reveal(&self, index: usize) {
        if let Some(list) = &self.native_list {
            // Unmaterialized variable-height rows have no measured bounds. An
            // item offset can jump to them without estimating a pixel distance.
            if list.bounds_for_item(index).is_none() {
                list.scroll_to(gpui::ListOffset {
                    item_ix: index,
                    offset_in_item: gpui::px(0.),
                });
            } else {
                list.scroll_to_reveal_item(index);
            }
        } else {
            self.scroll.scroll_to_item(index);
        }
    }
}

/// Retain one controller per mounted trigger. Immutable item snapshots make normal
/// redraws cheap; submenu factories run only for the branch being opened.
pub struct ContextMenuState {
    pub(super) levels: Vec<Level>,
    pub(super) popup: PopupHandle,
    pub(super) trigger: FocusHandle,
    pub(super) trigger_tab_stop: bool,
    pub(super) scope: Option<ConfigScope>,
    pub(super) on_event: Option<EventHandler>,
    pub(super) automation_id: Option<SharedString>,
    pub(super) open: bool,
    pub(super) disabled: bool,
    pub(super) mounted: bool,
    pub(super) generation: u64,
    mount_generation: u64,
    command_generation: u64,
    pub(super) trigger_bounds: Bounds<Pixels>,
    pub(super) focus_pending: bool,
    pub(super) focus_repair_from: Vec<FocusHandle>,
    return_focus: Option<FocusHandle>,
    hover_task: Option<Task<()>>,
    query: String,
    query_depth: usize,
    last_query: Option<Instant>,
    _scope: Option<Subscription>,
}
impl EventEmitter<MenuEvent> for ContextMenuState {}
impl Focusable for ContextMenuState {
    fn focus_handle(&self, _: &App) -> FocusHandle {
        self.trigger.clone()
    }
}
impl ContextMenuState {
    pub fn new(items: MenuItems, _window: &mut Window, cx: &mut Context<Self>) -> Self {
        Self {
            levels: vec![Level::new(items, None, cx)],
            popup: PopupHandle::new(),
            trigger: crate::behavior::focus_handle(cx),
            trigger_tab_stop: true,
            scope: None,
            on_event: None,
            automation_id: None,
            open: false,
            disabled: false,
            mounted: false,
            generation: 0,
            mount_generation: 0,
            command_generation: 0,
            trigger_bounds: Bounds::default(),
            focus_pending: false,
            focus_repair_from: Vec::new(),
            return_focus: None,
            hover_task: None,
            query: String::new(),
            query_depth: 0,
            last_query: None,
            _scope: None,
        }
    }
    pub fn items(&self) -> &MenuItems {
        &self.levels[0].items
    }
    pub fn is_open(&self) -> bool {
        self.open
    }
    pub fn active_item(&self) -> Option<&MenuItem> {
        let level = self.levels.last()?;
        level.items.item(level.active?)
    }
    pub fn submenu_depth(&self) -> usize {
        self.levels.len() - 1
    }
    /// Replace the authoritative snapshot, keeping the root active identity when
    /// possible. Descendants are discarded so delayed factories cannot use stale data.
    pub fn set_items(&mut self, items: MenuItems, cx: &mut Context<Self>) {
        let active = self.levels[0]
            .active
            .and_then(|i| self.levels[0].items.item(i))
            .map(|i| i.id.clone());
        self.focus_repair_from = self.levels.iter().map(|l| l.focus.clone()).collect();
        self.cancel_pending();
        self.levels.truncate(1);
        self.levels[0].active = active
            .and_then(|id| {
                items.entries().iter().position(|entry| {
                    entry
                        .item()
                        .is_some_and(|item| item.id == id && !item.disabled)
                })
            })
            .or_else(|| items.next(None, 1));
        self.levels[0].items = items;
        self.levels[0].width_cache = None;
        self.levels[0].native_list = None;
        self.focus_pending = self.open;
        cx.notify();
    }
    pub fn open_at(
        &mut self,
        position: Point<Pixels>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let popup = self.popup.clone();
        let weak = cx.entity().downgrade();
        self.command_generation = self.command_generation.wrapping_add(1);
        let command = self.command_generation;
        let mount = self.mount_generation;
        window.defer(cx, move |window, cx| {
            if let Some(state) = weak.upgrade()
                && !state.read(cx).disabled
                && state.read(cx).mounted
                && state.read(cx).mount_generation == mount
                && state.read(cx).command_generation == command
            {
                popup.open_at(position, window, cx);
            }
        });
    }
    pub fn close(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let popup = self.popup.clone();
        let weak = cx.entity().downgrade();
        self.command_generation = self.command_generation.wrapping_add(1);
        let command = self.command_generation;
        let mount = self.mount_generation;
        window.defer(cx, move |window, cx| {
            if let Some(state) = weak.upgrade()
                && state.read(cx).mounted
                && state.read(cx).mount_generation == mount
                && state.read(cx).command_generation == command
            {
                popup.request(false, PopupOpenReason::Cancel, window, cx)
            }
        });
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
    fn cancel_pending(&mut self) {
        self.generation = self.generation.wrapping_add(1);
        self.hover_task = None;
    }
    pub(super) fn unmount(&mut self) {
        self.cancel_pending();
        self.open = false;
        self.mounted = false;
        self.mount_generation = self.mount_generation.wrapping_add(1);
        self.levels.truncate(1);
        self.query.clear();
        self.on_event = None;
        self.return_focus = None;
    }
    fn select(
        &mut self,
        depth: usize,
        index: usize,
        close_descendants: bool,
        cx: &mut Context<Self>,
    ) {
        if !self.open
            || self.disabled
            || !self
                .levels
                .get(depth)
                .is_some_and(|l| l.items.enabled(index))
        {
            return;
        }
        self.cancel_pending();
        self.levels[depth].active = Some(index);
        self.levels[depth].reveal(index);
        if close_descendants {
            self.levels.truncate(depth + 1)
        }
        cx.notify();
    }
    fn typeahead(
        &mut self,
        depth: usize,
        text: &str,
        now: Instant,
        cx: &mut Context<Self>,
    ) -> bool {
        if !self.open || text.is_empty() {
            return false;
        }
        if self.query_depth != depth
            || self
                .last_query
                .is_none_or(|last| now.saturating_duration_since(last) > Duration::from_millis(500))
        {
            self.query.clear();
        }
        self.query_depth = depth;
        self.last_query = Some(now);
        self.query.push_str(&text.to_lowercase());
        let repeated = self
            .query
            .chars()
            .next()
            .is_some_and(|first| self.query.chars().all(|c| c == first));
        let query = if repeated {
            self.query.chars().next().unwrap().to_string()
        } else {
            self.query.clone()
        };
        let Some(level) = self.levels.get(depth) else {
            return false;
        };
        let start = if query.chars().count() == 1 {
            level.active.map_or(0, |i| i + 1)
        } else {
            level.active.unwrap_or(0)
        };
        let count = level.items.len();
        let found = (0..count).map(|step| (start + step) % count).find(|&i| {
            level
                .items
                .item(i)
                .is_some_and(|item| !item.disabled && item.label.to_lowercase().starts_with(&query))
        });
        if let Some(index) = found {
            self.select(depth, index, true, cx);
            true
        } else {
            false
        }
    }
}

fn emit(state: &Entity<ContextMenuState>, event: MenuEvent, window: &mut Window, cx: &mut App) {
    let callback = state.update(cx, |state, cx| {
        cx.emit(event.clone());
        state.on_event.clone()
    });
    if let Some(callback) = callback {
        callback(&event, window, cx)
    }
}
pub(super) fn opened(
    state: &Entity<ContextMenuState>,
    change: &PopupOpenChange,
    window: &mut Window,
    cx: &mut App,
) {
    let focus = state.update(cx, |s, cx| {
        if change.open {
            s.return_focus = Some(
                s.popup
                    .trigger_focus_handle(cx)
                    .unwrap_or_else(|| s.trigger.clone()),
            );
        }
        let restore = (!change.open
            && !s.disabled
            && matches!(
                change.reason,
                PopupOpenReason::Escape
                    | PopupOpenReason::Confirm
                    | PopupOpenReason::Cancel
                    | PopupOpenReason::Accessibility
            ))
        .then(|| s.return_focus.clone())
        .flatten();
        s.open = change.open;
        s.cancel_pending();
        s.levels.truncate(1);
        s.query.clear();
        s.last_query = None;
        if change.open {
            s.levels[0].active = s.levels[0].items.next(None, 1);
            s.levels[0].scroll.set_offset(Point::default());
            if let Some(list) = &s.levels[0].native_list {
                list.scroll_to(gpui::ListOffset {
                    item_ix: 0,
                    offset_in_item: gpui::px(0.),
                });
            }
        }
        cx.notify();
        if change.open {
            Some(s.levels[0].focus.clone())
        } else {
            restore
        }
    });
    if let Some(focus) = focus {
        if change.open {
            let weak = state.downgrade();
            let mount = state.read(cx).mount_generation;
            window.defer(cx, move |window, cx| {
                if weak.upgrade().is_some_and(|state| {
                    state.read(cx).mounted
                        && state.read(cx).open
                        && state.read(cx).mount_generation == mount
                }) {
                    window.focus(&focus, cx)
                }
            });
        } else {
            window.focus(&focus, cx)
        }
    }
    emit(
        state,
        MenuEvent::OpenChange {
            open: change.open,
            reason: change.reason,
        },
        window,
        cx,
    );
}
pub(super) fn hover(
    state: &Entity<ContextMenuState>,
    depth: usize,
    index: usize,
    window: &mut Window,
    cx: &mut App,
) {
    state.update(cx, |s, cx| {
        if !s.open || s.disabled || !s.levels.get(depth).is_some_and(|l| l.items.enabled(index)) {
            return;
        }
        s.select(depth, index, false, cx);
        window.focus(&s.levels[depth].focus, cx);
        // Keep the outgoing branch alive while crossing the gap. Only the final
        // target after the delay may materialize a lazy factory.
        let generation = s.generation;
        let timer = cx.background_executor().timer(Duration::from_millis(150));
        s.hover_task = Some(cx.spawn_in(window, async move |weak, cx| {
            timer.await;
            let _ = cx.update(|window, cx| {
                if let Some(state) = weak.upgrade()
                    && state.read(cx).generation == generation
                {
                    open_submenu(&state, depth, index, false, window, cx);
                }
            });
        }));
    });
}
pub(super) fn leave_row(
    state: &Entity<ContextMenuState>,
    depth: usize,
    index: usize,
    cx: &mut App,
) {
    state.update(cx, |s, _| {
        if s.levels
            .get(depth)
            .is_some_and(|level| level.active == Some(index))
        {
            s.cancel_pending();
        }
    });
}
pub(super) fn open_submenu(
    state: &Entity<ContextMenuState>,
    depth: usize,
    index: usize,
    keyboard: bool,
    window: &mut Window,
    cx: &mut App,
) {
    let candidate = state.update(cx, |s, cx| {
        if !s.open || s.disabled || depth >= 31 {
            return None;
        }
        let item = s.levels.get(depth)?.items.item(index)?.clone();
        if item.disabled {
            return None;
        }
        if s.levels
            .get(depth + 1)
            .is_some_and(|level| level.parent_index == Some(index))
        {
            return Some((
                None,
                None,
                s.generation,
                keyboard.then(|| s.levels[depth + 1].focus.clone()),
            ));
        }
        s.select(depth, index, true, cx);
        Some((item.submenu, item.lazy_submenu, s.generation, None))
    });
    let Some((items, factory, generation, existing_focus)) = candidate else {
        return;
    };
    if let Some(focus) = existing_focus {
        window.focus(&focus, cx);
        return;
    }
    let Some(items) = items.or_else(|| factory.map(|factory| factory(window, cx))) else {
        return;
    };
    let focus = state.update(cx, |s, cx| {
        if !s.open || s.generation != generation {
            return None;
        }
        let level = Level::new(items, Some(index), cx);
        let focus = level.focus.clone();
        s.levels.push(level);
        cx.notify();
        Some(focus)
    });
    if keyboard && let Some(focus) = focus {
        window.focus(&focus, cx)
    }
}
pub(super) fn close_branch(
    state: &Entity<ContextMenuState>,
    depth: usize,
    window: &mut Window,
    cx: &mut App,
) {
    let focus = state.update(cx, |s, cx| {
        if depth == 0 || s.levels.len() <= depth {
            return None;
        }
        s.cancel_pending();
        s.levels.truncate(depth);
        cx.notify();
        Some(s.levels[depth - 1].focus.clone())
    });
    if let Some(focus) = focus {
        window.focus(&focus, cx)
    }
}
pub(super) fn activate(
    state: &Entity<ContextMenuState>,
    depth: usize,
    index: usize,
    source: MenuActivationSource,
    window: &mut Window,
    cx: &mut App,
) {
    let submenu = state
        .read(cx)
        .levels
        .get(depth)
        .and_then(|level| level.items.item(index))
        .is_some_and(MenuItem::has_submenu);
    if submenu {
        open_submenu(
            state,
            depth,
            index,
            source != MenuActivationSource::Pointer,
            window,
            cx,
        );
        return;
    }
    let activation = state.update(cx, |s, cx| {
        if !s.open || s.disabled {
            return None;
        }
        let level = s.levels.get_mut(depth)?;
        let item = level.items.item(index)?.clone();
        if item.disabled || item.has_submenu() {
            return None;
        }
        let checked = level.items.toggle(index);
        // Fold eager snapshots back into their parents. Lazy branches are owned by
        // their factories and intentionally begin from fresh application data.
        for child in (1..=depth).rev() {
            let items = s.levels[child].items.clone();
            let parent_index = s.levels[child].parent_index?;
            if let MenuEntry::Item(parent) =
                &mut std::sync::Arc::make_mut(&mut s.levels[child - 1].items.0)[parent_index]
                && parent.lazy_submenu.is_none()
            {
                parent.submenu = Some(items);
            }
        }
        s.open = false;
        s.cancel_pending();
        cx.notify();
        Some((
            MenuActivation {
                id: item.id,
                label: item.label,
                checked,
                source,
            },
            s.popup.clone(),
        ))
    });
    if let Some((activation, popup)) = activation {
        popup.request(false, PopupOpenReason::Confirm, window, cx);
        emit(state, MenuEvent::Activate(activation), window, cx);
    }
}
pub(super) fn key(
    state: &Entity<ContextMenuState>,
    depth: usize,
    event: &KeyDownEvent,
    window: &mut Window,
    cx: &mut App,
) {
    if !state.read(cx).open || state.read(cx).disabled {
        return;
    }
    let key = event.keystroke.key.as_str();
    let modifiers = event.keystroke.modifiers;
    if modifiers.control || modifiers.platform || modifiers.alt {
        return;
    }
    let rtl = state
        .read(cx)
        .scope
        .as_ref()
        .is_some_and(|scope| scope.snapshot(cx).direction == Direction::Rtl);
    let forward = if rtl { "left" } else { "right" };
    let backward = if rtl { "right" } else { "left" };
    match key {
        "up" | "down" | "home" | "end" => {
            state.update(cx, |s, cx| {
                let Some(level) = s.levels.get(depth) else {
                    return;
                };
                let active = if matches!(key, "home" | "end") {
                    None
                } else {
                    level.active
                };
                let delta = if matches!(key, "up" | "end") { -1 } else { 1 };
                if let Some(index) = level.items.next(active, delta) {
                    s.select(depth, index, true, cx)
                }
            });
        }
        "enter" | "space" => {
            if !event.is_held
                && let Some(index) = state.read(cx).levels.get(depth).and_then(|l| l.active)
            {
                activate(
                    state,
                    depth,
                    index,
                    MenuActivationSource::Keyboard,
                    window,
                    cx,
                );
            }
        }
        key if key == forward => {
            if let Some(index) = state.read(cx).levels.get(depth).and_then(|l| l.active) {
                open_submenu(state, depth, index, true, window, cx);
            }
        }
        key if key == backward && depth > 0 => close_branch(state, depth, window, cx),
        "escape" => {
            if depth > 0 {
                close_branch(state, depth, window, cx)
            } else {
                let popup = state.read(cx).popup.clone();
                popup.request(false, PopupOpenReason::Escape, window, cx);
            }
        }
        "tab" => {
            let popup = state.read(cx).popup.clone();
            popup.request(false, PopupOpenReason::Cancel, window, cx);
            return;
        }
        _ => {
            let text = event.keystroke.key_char.as_deref().unwrap_or(key);
            if text.chars().count() != 1 || text.chars().any(char::is_control) {
                return;
            }
            let now = cx.background_executor().now();
            state.update(cx, |s, cx| {
                s.typeahead(depth, text, now, cx);
            });
        }
    }
    window.prevent_default();
    cx.stop_propagation();
}

pub(super) fn traverse(
    state: &Entity<ContextMenuState>,
    backward: bool,
    window: &mut Window,
    cx: &mut App,
) {
    let popup = state.read(cx).popup.clone();
    popup.request(false, PopupOpenReason::Cancel, window, cx);
    if backward {
        window.focus_prev(cx)
    } else {
        window.focus_next(cx)
    }
    cx.stop_propagation();
}

pub(super) fn show_context_menu(
    state: &Entity<ContextMenuState>,
    window: &mut Window,
    cx: &mut App,
) {
    let s = state.read(cx);
    if !s.mounted || s.disabled || s.popup.trigger_focus_handle(cx).is_some() {
        return;
    }
    let popup = s.popup.clone();
    let bounds = s.trigger_bounds;
    popup.open_at_with_reason(
        gpui::point(bounds.left() + gpui::px(4.), bounds.bottom()),
        PopupOpenReason::Accessibility,
        window,
        cx,
    );
}
