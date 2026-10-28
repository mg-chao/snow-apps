// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::{
    cell::RefCell,
    collections::HashMap,
    rc::{Rc, Weak},
    time::Instant,
};

use gpui::{
    AnyElement, App, Bounds, Element, ElementId, EntityId, Global, GlobalElementId,
    InspectorElementId, IntoElement, KeyDownEvent, LayoutId, MouseDownEvent, Pixels, Point,
    RenderOnce, WeakEntity, Window, WindowId,
};

use super::{
    PopupOpenReason,
    runtime::{PopupState, request_open},
};

#[derive(Default)]
struct Registry(HashMap<WindowId, Weak<RefCell<HostState>>>);
impl Global for Registry {}

pub(super) type Host = Rc<RefCell<HostState>>;

#[derive(Clone)]
pub(super) struct Entry {
    pub state: WeakEntity<PopupState>,
    pub parent: Option<EntityId>,
    pub trigger: Bounds<Pixels>,
    pub surface: Bounds<Pixels>,
    pub layer: usize,
    pub order: usize,
}

pub(super) struct UniqueGeometry {
    pub owner: WeakEntity<PopupState>,
    pub bounds: Bounds<Pixels>,
    pub expires: Option<Instant>,
}

#[derive(Default)]
pub(super) struct HostState {
    pub entries: HashMap<EntityId, Entry>,
    pub unique: HashMap<EntityId, WeakEntity<PopupState>>,
    pub unique_geometry: HashMap<EntityId, UniqueGeometry>,
    pub parent: Option<EntityId>,
    sequence: usize,
}

impl HostState {
    pub fn register(&mut self, id: EntityId, mut entry: Entry) -> usize {
        if let Some(old) = self.entries.get(&id) {
            entry.order = old.order;
        } else {
            self.sequence = self.sequence.wrapping_add(1);
            entry.order = self.sequence;
        }
        if let Some(parent) = entry.parent.and_then(|parent| self.entries.get(&parent)) {
            entry.layer = entry.layer.max(parent.layer.saturating_add(1));
        }
        let priority = entry
            .layer
            .saturating_mul(1 << 20)
            .saturating_add(entry.order % (1 << 20));
        self.entries.insert(id, entry);
        priority
    }

    fn contains_branch(&self, ancestor: EntityId, mut node: EntityId) -> bool {
        // Ownership is established by the render tree. The bound also makes a
        // malformed externally recomposed tree harmless instead of looping.
        for _ in 0..=self.entries.len() {
            if node == ancestor {
                return true;
            }
            let Some(parent) = self.entries.get(&node).and_then(|entry| entry.parent) else {
                return false;
            };
            node = parent;
        }
        false
    }

    pub fn descendants(&self, id: EntityId) -> Vec<WeakEntity<PopupState>> {
        self.entries
            .iter()
            .filter(|(child, _)| **child != id && self.contains_branch(id, **child))
            .map(|(_, entry)| entry.state.clone())
            .collect()
    }

    fn outside(&self, position: Point<Pixels>) -> Vec<WeakEntity<PopupState>> {
        let inside = self
            .entries
            .iter()
            .filter(|(_, entry)| {
                entry.surface.contains(&position) || entry.trigger.contains(&position)
            })
            .max_by_key(|(_, entry)| (entry.layer, entry.order))
            .map(|(id, _)| *id);
        self.entries
            .iter()
            .filter(|(id, _)| inside.is_none_or(|inside| !self.contains_branch(**id, inside)))
            .map(|(_, entry)| entry.state.clone())
            .collect()
    }

    fn topmost(&self) -> Option<WeakEntity<PopupState>> {
        self.entries
            .values()
            .max_by_key(|entry| (entry.layer, entry.order))
            .map(|entry| entry.state.clone())
    }
}

pub(super) fn current(window: &Window, cx: &App) -> Option<Host> {
    cx.try_global::<Registry>()?
        .0
        .get(&window.window_handle().window_id())?
        .upgrade()
}

pub(crate) fn dismiss_modal(owner: Option<EntityId>, window: &mut Window, cx: &mut App) {
    let entries = current(window, cx)
        .map(|h| {
            h.borrow()
                .entries
                .values()
                .map(|e| e.state.clone())
                .collect::<Vec<_>>()
        })
        .unwrap_or_default();
    for weak in entries {
        if weak
            .upgrade()
            .is_some_and(|s| s.read(cx).modal_owner == owner)
        {
            request_open(&weak, false, PopupOpenReason::ParentClosed, window, cx);
        }
    }
}
pub(crate) fn dismiss_unowned_modal(owner: EntityId, window: &mut Window, cx: &mut App) {
    let entries = current(window, cx)
        .map(|h| {
            h.borrow()
                .entries
                .values()
                .map(|e| e.state.clone())
                .collect::<Vec<_>>()
        })
        .unwrap_or_default();
    for weak in entries {
        if weak
            .upgrade()
            .is_some_and(|s| s.read(cx).modal_owner != Some(owner))
        {
            request_open(&weak, false, PopupOpenReason::ParentClosed, window, cx);
        }
    }
}
pub(crate) fn modal_contains_focus(owner: EntityId, window: &Window, cx: &App) -> bool {
    current(window, cx).is_some_and(|h| {
        h.borrow().entries.values().any(|e| {
            e.state.upgrade().is_some_and(|s| {
                s.read(cx).modal_owner == Some(owner)
                    && s.read(cx).surface_focus.contains_focused(window, cx)
            })
        })
    })
}
pub(crate) fn modal_contains_position(
    owner: EntityId,
    position: Point<Pixels>,
    window: &Window,
    cx: &App,
) -> bool {
    current(window, cx).is_some_and(|h| {
        h.borrow().entries.values().any(|e| {
            e.surface.contains(&position)
                && e.state
                    .upgrade()
                    .is_some_and(|s| s.read(cx).modal_owner == Some(owner))
        })
    })
}
pub(crate) fn dismiss_top_modal(owner: EntityId, window: &mut Window, cx: &mut App) -> bool {
    let top = current(window, cx).and_then(|h| {
        h.borrow()
            .entries
            .values()
            .filter(|e| {
                e.state
                    .upgrade()
                    .is_some_and(|s| s.read(cx).modal_owner == Some(owner))
            })
            .max_by_key(|e| (e.layer, e.order))
            .map(|e| e.state.clone())
    });
    if let Some(top) = top {
        request_open(&top, false, PopupOpenReason::Escape, window, cx);
        true
    } else {
        false
    }
}

/// Set popup ancestry while rendering/layouting its content, without holding a
/// RefCell borrow across user code or native GPUI element traversal.
pub(super) fn with_parent<R>(host: &Host, parent: EntityId, f: impl FnOnce() -> R) -> R {
    struct Restore(Host, Option<EntityId>);
    impl Drop for Restore {
        fn drop(&mut self) {
            self.0.borrow_mut().parent = self.1;
        }
    }
    let old = host.borrow_mut().parent.replace(parent);
    let _restore = Restore(host.clone(), old);
    f()
}

/// Window-local popup coordination. `UiRoot` installs this automatically.
/// Custom roots wrap their contents in one host; nested popups share that host.
#[derive(IntoElement)]
pub struct PopupHost {
    id: ElementId,
    child: AnyElement,
}

impl PopupHost {
    pub fn new(id: impl Into<ElementId>, child: impl IntoElement) -> Self {
        Self {
            id: id.into(),
            child: child.into_any_element(),
        }
    }
}

impl RenderOnce for PopupHost {
    fn render(self, window: &mut Window, cx: &mut App) -> impl IntoElement {
        // Retain the manager through keyed native state, not an application-global
        // strong reference. Closing a window releases every registry lease.
        let host = window.use_keyed_state(self.id.clone(), cx, |_, _| {
            Rc::new(RefCell::new(HostState::default()))
        });
        let manager = host.read(cx).clone();
        if !cx.has_global::<Registry>() {
            cx.set_global(Registry::default());
        }
        let registry = cx.global_mut::<Registry>();
        registry.0.retain(|_, weak| weak.strong_count() != 0);
        registry
            .0
            .insert(window.window_handle().window_id(), Rc::downgrade(&manager));
        HostElement {
            id: self.id,
            child: self.child,
            host: manager,
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
        window: &mut Window,
        cx: &mut App,
    ) -> (LayoutId, ()) {
        (self.child.request_layout(window, cx), ())
    }
    fn prepaint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        _: Bounds<Pixels>,
        _: &mut (),
        window: &mut Window,
        cx: &mut App,
    ) {
        {
            let mut host = self.host.borrow_mut();
            host.entries
                .retain(|_, entry| entry.state.upgrade().is_some());
            host.unique.retain(|_, owner| owner.upgrade().is_some());
            let now = cx.background_executor().now();
            host.unique_geometry.retain(|_, entry| {
                entry.owner.upgrade().is_some()
                    && entry.expires.is_none_or(|expires| now <= expires)
            });
        }
        self.child.prepaint(window, cx);
    }
    fn paint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        _: Bounds<Pixels>,
        _: &mut (),
        _: &mut (),
        window: &mut Window,
        cx: &mut App,
    ) {
        let host = self.host.clone();
        window.on_mouse_event(move |event: &MouseDownEvent, phase, window, cx| {
            if phase.capture() {
                let dismissed = host.borrow().outside(event.position);
                for state in dismissed {
                    request_open(&state, false, PopupOpenReason::OutsidePress, window, cx);
                }
            }
        });
        let host = self.host.clone();
        window.on_key_event(move |event: &KeyDownEvent, phase, window, cx| {
            if phase.bubble()
                && event.keystroke.key == "escape"
                && !event.keystroke.modifiers.modified()
            {
                let topmost = host.borrow().topmost();
                if let Some(state) = topmost {
                    request_open(&state, false, PopupOpenReason::Escape, window, cx);
                    cx.stop_propagation();
                }
            }
        });
        self.child.paint(window, cx);
    }
}
