// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
use super::{ModalCancelReason, ModalState, state};
use gpui::prelude::*;
use gpui::{
    AnyElement, App, Bounds, Element, ElementId, Entity, EntityId, Global, GlobalElementId,
    InspectorElementId, IntoElement, KeyDownEvent, LayoutId, Pixels, RenderOnce, Subscription,
    WeakEntity, Window, WindowId,
};
use std::{
    cell::RefCell,
    collections::HashMap,
    rc::{Rc, Weak},
};

#[derive(Default)]
struct Registry(HashMap<WindowId, Weak<RefCell<HostState>>>);
impl Global for Registry {}
type Host = Rc<RefCell<HostState>>;
struct Entry {
    state: WeakEntity<ModalState>,
    parent: Option<EntityId>,
    order: usize,
    z_index: usize,
}
#[derive(Default)]
struct HostState {
    entries: HashMap<EntityId, Entry>,
    managed: Vec<Entity<ModalState>>,
    observers: HashMap<EntityId, Subscription>,
    owner: Option<EntityId>,
    sequence: usize,
}
fn current(window: &Window, cx: &App) -> Option<Host> {
    cx.try_global::<Registry>()?
        .0
        .get(&window.window_handle().window_id())?
        .upgrade()
}
pub(crate) fn owner(window: &Window, cx: &App) -> Option<EntityId> {
    current(window, cx)?.borrow().owner
}
pub(super) fn with_owner<R>(
    id: EntityId,
    window: &mut Window,
    cx: &mut App,
    f: impl FnOnce(&mut Window, &mut App) -> R,
) -> R {
    struct Restore(Host, Option<EntityId>);
    impl Drop for Restore {
        fn drop(&mut self) {
            self.0.borrow_mut().owner = self.1;
        }
    }
    let host = current(window, cx).expect("Modal requires UiRoot or ModalHost");
    let old = host.borrow_mut().owner.replace(id);
    let _restore = Restore(host, old);
    f(window, cx)
}
pub(super) fn activate(
    entity: &Entity<ModalState>,
    z_index: usize,
    window: &mut Window,
    cx: &mut App,
) {
    let Some(host) = current(window, cx) else {
        return;
    };
    let mut h = host.borrow_mut();
    let id = entity.entity_id();
    let owner = h.owner.filter(|parent| *parent != id);
    if let Some(entry) = h.entries.get_mut(&id) {
        entry.z_index = z_index;
        if owner.is_some() {
            entry.parent = owner;
        }
        return;
    }
    // Paint ownership describes nesting. Merely being the previous top dialog does
    // not make an independent manager's dialog a child.
    let parent = h.owner;
    h.sequence = h.sequence.wrapping_add(1);
    let order = h.sequence;
    h.entries.insert(
        id,
        Entry {
            state: entity.downgrade(),
            parent,
            order,
            z_index,
        },
    );
    drop(h);
    window.defer(cx, move |w, cx| {
        crate::popup::host::dismiss_unowned_modal(id, w, cx)
    });
}
pub(super) fn manage(entity: Entity<ModalState>, window: &mut Window, cx: &mut App) {
    let target = window.window_handle();
    let observer = cx.observe(&entity, move |_, cx| {
        let _ = target.update(cx, |_, w, _| w.refresh());
    });
    let host = current(window, cx).expect("ModalManager requires UiRoot or ModalHost");
    let mut h = host.borrow_mut();
    h.observers.insert(entity.entity_id(), observer);
    h.managed.push(entity);
    window.refresh();
}
pub(super) fn retire(id: EntityId, window: &mut Window, cx: &mut App) {
    if let Some(host) = current(window, cx) {
        let mut h = host.borrow_mut();
        h.entries.remove(&id);
        h.observers.remove(&id);
        h.managed.retain(|s| s.entity_id() != id);
    }
    window.refresh();
}
pub(super) fn close_descendants(id: EntityId, window: &mut Window, cx: &mut App) {
    let Some(host) = current(window, cx) else {
        return;
    };
    let descendants = {
        let h = host.borrow();
        h.entries
            .iter()
            .filter(|(_, e)| {
                let mut parent = e.parent;
                for _ in 0..h.entries.len() {
                    if parent == Some(id) {
                        return true;
                    }
                    parent = parent
                        .and_then(|p| h.entries.get(&p))
                        .and_then(|e| e.parent);
                }
                false
            })
            .map(|(_, e)| e.state.clone())
            .collect::<Vec<_>>()
    };
    for weak in descendants {
        if let Some(entity) = weak.upgrade() {
            super::ModalHandle::from_entity(&entity, cx).destroy(window, cx);
        }
    }
}
pub(crate) fn top(window: &Window, cx: &App) -> Option<Entity<ModalState>> {
    let host = current(window, cx)?;
    let state = host
        .borrow()
        .entries
        .values()
        .max_by_key(|e| (e.z_index, e.order))?
        .state
        .clone();
    state.upgrade()
}
pub(super) fn is_top(id: EntityId, window: &Window, cx: &App) -> bool {
    top(window, cx).is_some_and(|s| s.entity_id() == id)
}
pub(super) fn priority(id: EntityId, window: &Window, cx: &App) -> usize {
    let Some(host) = current(window, cx) else {
        return 0;
    };
    let h = host.borrow();
    let Some(entry) = h.entries.get(&id) else {
        return 0;
    };
    let rank = h
        .entries
        .values()
        .filter(|e| (e.z_index, e.order) < (entry.z_index, entry.order))
        .count();
    (rank + 1).saturating_mul(1usize << 42)
}
pub(crate) fn popup_priority(
    owner: Option<EntityId>,
    fallback: usize,
    window: &Window,
    cx: &App,
) -> usize {
    owner.map_or(fallback, |id| {
        priority(id, window, cx)
            .saturating_add(1usize << 41)
            .saturating_add(fallback.min((1usize << 41) - 1))
    })
}

/// Place a window-wide overlay in the dialog stack using its configured z-index.
/// Modal layers reserve large ranges for their owned popups, so multiplying the
/// overlay's z-index alone would leave it behind every modal, including its mask.
pub(crate) fn overlay_priority(z_index: usize, window: &Window, cx: &App) -> usize {
    let below = current(window, cx).and_then(|host| {
        host.borrow()
            .entries
            .iter()
            .filter(|(_, entry)| entry.z_index <= z_index)
            .max_by_key(|(_, entry)| (entry.z_index, entry.order))
            .map(|(id, _)| *id)
    });
    popup_priority(below, z_index.saturating_mul(1 << 20), window, cx)
}

pub(crate) fn popup_allowed(owner: Option<EntityId>, window: &Window, cx: &App) -> bool {
    top(window, cx).is_none_or(|top| owner == Some(top.entity_id()) && top.read(cx).open)
}
pub(crate) fn restore_focus(window: &mut Window, cx: &mut App) {
    let Some(top) = top(window, cx) else {
        return;
    };
    let s = top.read(cx);
    if s.trap(cx)
        && !s.focus.contains_focused(window, cx)
        && !crate::popup::host::modal_contains_focus(top.entity_id(), window, cx)
    {
        let focus = s.focus.clone();
        window.focus(&focus, cx);
    }
}
pub(super) fn traverse(backward: bool, window: &mut Window, cx: &mut App) {
    let Some(top) = top(window, cx) else {
        return;
    };
    if !top.read(cx).trap(cx) {
        if backward {
            window.focus_prev(cx);
        } else {
            window.focus_next(cx);
        }
        return;
    }
    let (start, end, focus) = {
        let s = top.read(cx);
        (s.start_focus.clone(), s.end_focus.clone(), s.focus.clone())
    };
    if backward {
        window.focus_prev(cx);
    } else {
        window.focus_next(cx);
    }
    if start.is_focused(window) || end.is_focused(window) || !focus.contains_focused(window, cx) {
        window.focus(if backward { &end } else { &start }, cx);
        if backward {
            window.focus_prev(cx);
        } else {
            window.focus_next(cx);
        }
        if start.is_focused(window) || end.is_focused(window) || !focus.contains_focused(window, cx)
        {
            window.focus(&focus, cx);
        }
    }
}

/// Install once around all content in a native window. `UiRoot` does this automatically.
#[derive(IntoElement)]
pub struct ModalHost {
    id: ElementId,
    child: AnyElement,
}
impl ModalHost {
    pub fn new(id: impl Into<ElementId>, child: impl IntoElement) -> Self {
        Self {
            id: id.into(),
            child: child.into_any_element(),
        }
    }
}
impl RenderOnce for ModalHost {
    fn render(self, window: &mut Window, cx: &mut App) -> impl IntoElement {
        let retained = window.use_keyed_state(self.id.clone(), cx, |_, _| {
            Rc::new(RefCell::new(HostState::default()))
        });
        let host = retained.read(cx).clone();
        if !cx.has_global::<Registry>() {
            cx.set_global(Registry::default());
        }
        let registry = cx.global_mut::<Registry>();
        registry.0.retain(|_, v| v.strong_count() > 0);
        registry
            .0
            .insert(window.window_handle().window_id(), Rc::downgrade(&host));
        let hidden = top(window, cx).is_some_and(|s| s.read(cx).trap(cx));
        let child = crate::behavior::Accessibility {
            role: Some(gpui::Role::Group),
            hidden,
            ..Default::default()
        }
        .apply(
            gpui::div()
                .id("modal-background")
                .size_full()
                .child(self.child),
        )
        .into_any_element();
        HostElement {
            id: self.id,
            child,
            host,
        }
    }
}
struct HostElement {
    id: ElementId,
    child: AnyElement,
    host: Host,
}
impl IntoElement for HostElement {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}
impl Element for HostElement {
    type RequestLayoutState = ();
    type PrepaintState = ();
    fn id(&self) -> Option<ElementId> {
        Some(self.id.clone())
    }
    fn source_location(&self) -> Option<&'static std::panic::Location<'static>> {
        None
    }
    fn request_layout(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        w: &mut Window,
        cx: &mut App,
    ) -> (LayoutId, ()) {
        (self.child.request_layout(w, cx), ())
    }
    fn prepaint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        _: Bounds<Pixels>,
        _: &mut (),
        w: &mut Window,
        cx: &mut App,
    ) {
        self.host.borrow_mut().entries.retain(|_, e| {
            e.state
                .upgrade()
                .is_some_and(|s| s.read(cx).phase != super::ModalPhase::Closed)
        });
        for entry in self.host.borrow_mut().entries.values_mut() {
            if let Some(state) = entry.state.upgrade() {
                entry.z_index = state.read(cx).options.z_index;
            }
        }
        self.child.prepaint(w, cx);
        let active = self
            .host
            .borrow()
            .entries
            .values()
            .filter_map(|entry| entry.state.upgrade())
            .collect::<Vec<_>>();
        for state in active {
            super::view::draw(&state, w, cx);
        }
        w.defer(cx, restore_focus);
    }
    fn paint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        _: Bounds<Pixels>,
        _: &mut (),
        _: &mut (),
        w: &mut Window,
        cx: &mut App,
    ) {
        w.on_key_event(|e: &KeyDownEvent, phase, w, cx| {
            if phase.capture() {
                if let Some(top) = top(w, cx)
                    && !top.read(cx).open
                {
                    let focus = top.read(cx).focus.clone();
                    w.focus(&focus, cx);
                    cx.stop_propagation();
                    return;
                }
                let previous = w.focused(cx);
                restore_focus(w, cx);
                if w.focused(cx) != previous {
                    cx.stop_propagation();
                }
                return;
            }
            if !phase.bubble() || e.keystroke.modifiers.modified() || e.keystroke.key != "escape" {
                return;
            }
            let Some(top) = top(w, cx) else {
                return;
            };
            if crate::popup::host::dismiss_top_modal(top.entity_id(), w, cx) {
                cx.stop_propagation();
                return;
            }
            if top.read(cx).options.keyboard {
                state::activate(&top, Some(ModalCancelReason::Escape), w, cx);
            }
            cx.stop_propagation();
        });
        self.child.paint(w, cx);
    }
}
