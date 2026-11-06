// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::{
    cell::RefCell,
    rc::Rc,
    time::{Duration, Instant},
};

use gpui::{
    AnyElement, App, Bounds, Context, ElementId, EntityId, FocusHandle, Hsla, IntoElement, Pixels,
    Point, Refineable, RenderOnce, Role, SharedString, StyleRefinement, Subscription, Task,
    WeakEntity, Window, div, px,
};

use super::{
    PopupArrow, PopupConfig, PopupConfigPatch, PopupLayout, PopupOpenChange, PopupOpenReason,
    PopupPlacement, PopupTriggers, PopupWidth,
    element::PopupElement,
    host::{self, Host, UniqueGeometry},
    lifecycle::Lifecycle,
    motion::BoundsTransition,
};
use crate::{
    ConfigScope,
    behavior::{MotionPolicy, ScalarTransition},
    theme::{AliasTokens, CubicBezier, ShadowList},
};

pub(super) type Content = Rc<dyn Fn(&PopupHandle, &mut Window, &mut App) -> AnyElement>;
pub(super) type ChangeHandler = Rc<dyn Fn(&PopupOpenChange, &mut Window, &mut App)>;
pub(super) type SurfaceClickHandler = Rc<dyn Fn(&gpui::ClickEvent, &mut Window, &mut App)>;
pub(super) type HiddenHandler = Rc<dyn Fn(&mut App)>;
pub(super) type PresentedHandler = Rc<dyn Fn(&mut Window, &mut App)>;
pub(super) type InitialFocus = Rc<dyn Fn(&App) -> Option<FocusHandle>>;

/// This description belongs to the current frame, never to a hidden controller.
pub(super) struct Source {
    pub content: Content,
    pub on_hidden: Option<HiddenHandler>,
    pub on_presented: Option<PresentedHandler>,
    pub on_popup_click: Option<SurfaceClickHandler>,
    pub arrow_attachment: Option<ArrowAttachment>,
    pub initial_focus: Option<FocusHandle>,
    pub initial_focus_with: Option<InitialFocus>,
}

pub(super) type ArrowAttachment = Rc<dyn Fn(&PopupLayout, &App) -> Option<Point<Pixels>>>;

type HiddenSlot = RefCell<Option<HiddenHandler>>;
type HiddenQueue = RefCell<Vec<std::rc::Weak<HiddenSlot>>>;

/// A weak connection to a mounted popup. Keeping a handle never keeps an
/// unmounted popup, native window, pending hover timer or content tree alive.
#[derive(Clone, Default)]
pub struct PopupHandle(Rc<RefCell<Option<WeakEntity<PopupState>>>>, Rc<HiddenQueue>);

/// Deferred cleanup avoids re-entering a consumer which closes its popup inside
/// its own update. Handles keep weak slots only; the app effect owns the callback.
pub(super) struct HiddenEffect {
    callback: Rc<HiddenSlot>,
    pending: std::rc::Weak<HiddenQueue>,
}

pub(super) fn dispatch_hidden(effect: Option<HiddenEffect>, cx: &mut App) {
    if let Some(effect) = effect {
        cx.defer(move |cx| {
            let callback = effect.callback.borrow_mut().take();
            if let Some(callback) = callback {
                callback(cx);
            }
            if let Some(pending) = effect.pending.upgrade() {
                let mut pending = pending.borrow_mut();
                pending.retain(|weak| weak.upgrade().is_some_and(|slot| slot.borrow().is_some()));
                if pending.is_empty() {
                    *pending = Vec::new();
                }
            }
        });
    }
}

/// Identity of an opening, including its mounted instance. Compare these values
/// before applying asynchronous results to a popup that may have been replaced.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct PopupSession {
    instance: EntityId,
    generation: u64,
}

impl PopupHandle {
    pub fn new() -> Self {
        Self::default()
    }

    fn pending_hidden(&self, callback: HiddenHandler) -> HiddenEffect {
        let callback = Rc::new(RefCell::new(Some(callback)));
        self.1.borrow_mut().push(Rc::downgrade(&callback));
        HiddenEffect {
            callback,
            pending: Rc::downgrade(&self.1),
        }
    }

    /// Cleanup from a prior presentation must precede resource initialization
    /// for a replacement, even if close/reopen happened within one app update.
    pub(super) fn flush_hidden(&self, cx: &mut App) {
        let pending = std::mem::take(&mut *self.1.borrow_mut());
        for weak in pending {
            if let Some(slot) = weak.upgrade() {
                let callback = slot.borrow_mut().take();
                if let Some(callback) = callback {
                    callback(cx);
                }
            }
        }
    }

    pub fn is_open(&self, cx: &App) -> bool {
        self.state()
            .and_then(|weak| weak.upgrade())
            .is_some_and(|state| state.read(cx).lifecycle.open())
    }

    pub fn set_open(&self, open: bool, window: &mut Window, cx: &mut App) {
        self.request(open, PopupOpenReason::Programmatic, window, cx);
    }

    pub fn close(&self, window: &mut Window, cx: &mut App) {
        self.set_open(false, window, cx);
    }

    /// Open at a window-relative pointer position. Reopening an already visible
    /// context popup realigns it immediately without creating another session.
    pub fn open_at(&self, position: Point<Pixels>, window: &mut Window, cx: &mut App) {
        self.open_at_with_reason(position, PopupOpenReason::ContextMenu, window, cx);
    }

    pub(crate) fn open_at_with_reason(
        &self,
        position: Point<Pixels>,
        reason: PopupOpenReason,
        window: &mut Window,
        cx: &mut App,
    ) {
        if !f32::from(position.x).is_finite() || !f32::from(position.y).is_finite() {
            return;
        }
        if let Some(state) = self.state() {
            let _ = state.update(cx, |state, cx| {
                state.context_position = Some(position);
                cx.notify();
            });
            request_open(&state, true, reason, window, cx);
        }
    }

    pub fn force_align(&self, cx: &mut App) {
        if let Some(state) = self.state() {
            let _ = state.update(cx, |_, cx| cx.notify());
        }
    }

    pub fn layout(&self, cx: &App) -> Option<PopupLayout> {
        self.state()?.upgrade()?.read(cx).layout
    }

    /// Native control bindings own focus when present. Composite controls use
    /// this to provide a fallback tab stop only for a noninteractive trigger.
    pub(crate) fn trigger_focus_handle(&self, cx: &App) -> Option<FocusHandle> {
        self.state()?
            .upgrade()?
            .read(cx)
            .trigger_metadata
            .borrow()
            .focus
            .clone()
    }

    /// Changes whenever an opening ends. Async work may use this to distinguish
    /// its original dialog from a later opening of the same mounted control.
    pub fn session_id(&self, cx: &App) -> Option<PopupSession> {
        let entity = self.state()?.upgrade()?;
        Some(PopupSession {
            instance: entity.entity_id(),
            generation: entity.read(cx).session,
        })
    }

    pub(crate) fn request(
        &self,
        open: bool,
        reason: PopupOpenReason,
        window: &mut Window,
        cx: &mut App,
    ) {
        if let Some(state) = self.state() {
            request_open(&state, open, reason, window, cx);
        }
    }

    pub(super) fn state(&self) -> Option<WeakEntity<PopupState>> {
        self.0.borrow().clone()
    }
}

#[derive(Clone)]
pub(super) struct Options {
    pub width: PopupWidth,
    pub placement: PopupPlacement,
    pub triggers: PopupTriggers,
    pub arrow: PopupArrow,
    pub arrow_attachment: Option<ArrowAttachment>,
    pub auto_adjust_overflow: bool,
    pub gap: Option<Pixels>,
    pub flipped_gap: Option<Pixels>,
    pub offset: Point<Pixels>,
    pub z_index: usize,
    pub enter_delay: Duration,
    pub leave_delay: Duration,
    pub disabled: bool,
    pub background: Option<Hsla>,
    pub radius: Option<Pixels>,
    pub alignment_radius: Option<Pixels>,
    pub arrow_size: Option<Pixels>,
    pub arrow_tip_radius: Option<Pixels>,
    pub arrow_outer_radius: Option<Pixels>,
    pub shadows: Option<ShadowList>,
    pub surface_style: StyleRefinement,
    pub role: Role,
    pub label: Option<SharedString>,
    pub description: Option<SharedString>,
    pub busy: bool,
    pub automation_id: Option<SharedString>,
    pub initial_focus: Option<FocusHandle>,
    pub initial_focus_with: Option<InitialFocus>,
    pub unique_group: Option<EntityId>,
    pub motion_duration: Option<Duration>,
    pub exit_duration: Duration,
    pub handoff_duration: Duration,
    pub motion_curve: Option<CubicBezier>,
    pub motion_enabled: Option<bool>,
    pub on_popup_click: Option<SurfaceClickHandler>,
    pub menu_trigger: bool,
    pub viewport_margin: Option<Pixels>,
}

impl Default for Options {
    fn default() -> Self {
        Self {
            width: PopupWidth::Content,
            placement: PopupPlacement::Top,
            triggers: PopupTriggers::CLICK,
            arrow: PopupArrow::default(),
            arrow_attachment: None,
            auto_adjust_overflow: true,
            gap: None,
            flipped_gap: None,
            offset: Point::default(),
            z_index: 1030,
            enter_delay: Duration::from_millis(100),
            leave_delay: Duration::from_millis(100),
            disabled: false,
            background: None,
            radius: None,
            alignment_radius: None,
            arrow_size: None,
            arrow_tip_radius: None,
            arrow_outer_radius: None,
            shadows: None,
            surface_style: StyleRefinement::default(),
            role: Role::Dialog,
            label: None,
            description: None,
            busy: false,
            automation_id: None,
            initial_focus: None,
            initial_focus_with: None,
            unique_group: None,
            motion_duration: None,
            exit_duration: Duration::ZERO,
            handoff_duration: Duration::from_millis(300),
            motion_curve: None,
            motion_enabled: None,
            on_popup_click: None,
            menu_trigger: false,
            viewport_margin: None,
        }
    }
}

/// An anchored native popup. Supply a fresh trigger element each render and a
/// content factory that can rebuild native elements. State lives behind the ID.
#[derive(IntoElement)]
pub struct Popup {
    pub(crate) id: ElementId,
    scope: Option<ConfigScope>,
    child: AnyElement,
    content: Content,
    open: Option<bool>,
    default_open: bool,
    on_hidden: Option<HiddenHandler>,
    on_presented: Option<PresentedHandler>,
    handle: Option<PopupHandle>,
    on_change: Option<ChangeHandler>,
    pub(super) options: Options,
    overrides: PopupConfigPatch,
    explicit_z_index: bool,
}

impl Popup {
    pub fn new(id: impl Into<ElementId>) -> Self {
        Self {
            id: id.into(),
            scope: None,
            child: div().into_any_element(),
            content: Rc::new(|_, _, _| div().into_any_element()),
            open: None,
            default_open: false,
            on_hidden: None,
            on_presented: None,
            handle: None,
            on_change: None,
            options: Options::default(),
            overrides: PopupConfigPatch::default(),
            explicit_z_index: false,
        }
    }

    pub fn scope(mut self, scope: ConfigScope) -> Self {
        self.scope = Some(scope);
        self
    }
    pub fn child(mut self, child: impl IntoElement) -> Self {
        self.child = child.into_any_element();
        self
    }
    pub(crate) fn menu_trigger(mut self) -> Self {
        self.options.menu_trigger = true;
        self
    }
    /// Arrowless menu surfaces may shift on both axes after normal placement.
    /// Ordinary popups retain their Ant Design alignment contract.
    pub(crate) fn constrain_to_viewport(mut self, margin: Pixels) -> Self {
        self.options.viewport_margin = Some(finite_nonnegative(margin));
        self
    }
    pub fn content<E: IntoElement>(
        mut self,
        content: impl Fn(&PopupHandle, &mut Window, &mut App) -> E + 'static,
    ) -> Self {
        self.content =
            Rc::new(move |handle, window, cx| content(handle, window, cx).into_any_element());
        self
    }
    pub fn open(mut self, open: bool) -> Self {
        self.open = Some(open);
        self
    }
    pub fn default_open(mut self, open: bool) -> Self {
        self.default_open = open;
        self
    }
    /// Release popup-only resources after its final presentation ends.
    /// Internal consumers must capture their controller weakly.
    pub(crate) fn on_hidden(mut self, callback: impl Fn(&mut App) + 'static) -> Self {
        self.on_hidden = Some(Rc::new(callback));
        self
    }
    /// Initialize presentation-only resources before the first content build.
    pub(crate) fn on_presented(
        mut self,
        callback: impl Fn(&mut Window, &mut App) + 'static,
    ) -> Self {
        self.on_presented = Some(Rc::new(callback));
        self
    }
    pub(crate) fn initial_focus_with(
        mut self,
        callback: impl Fn(&App) -> Option<FocusHandle> + 'static,
    ) -> Self {
        self.options.initial_focus_with = Some(Rc::new(callback));
        self
    }
    pub fn handle(mut self, handle: &PopupHandle) -> Self {
        self.handle = Some(handle.clone());
        self
    }
    pub fn on_open_change(
        mut self,
        callback: impl Fn(&PopupOpenChange, &mut Window, &mut App) + 'static,
    ) -> Self {
        self.on_change = Some(Rc::new(callback));
        self
    }
    pub fn placement(mut self, placement: PopupPlacement) -> Self {
        self.options.placement = placement;
        self
    }
    /// Observe a click that bubbles through the popup content, independently
    /// of trigger activation and visibility requests.
    pub fn on_popup_click(
        mut self,
        callback: impl Fn(&gpui::ClickEvent, &mut Window, &mut App) + 'static,
    ) -> Self {
        self.options.on_popup_click = Some(Rc::new(callback));
        self
    }
    pub fn triggers(mut self, triggers: PopupTriggers) -> Self {
        self.options.triggers = triggers;
        self.overrides.triggers = Some(triggers);
        self
    }
    pub fn arrow(mut self, arrow: impl Into<PopupArrow>) -> Self {
        self.options.arrow = arrow.into();
        self.overrides.arrow = Some(self.options.arrow);
        self
    }
    pub fn auto_adjust_overflow(mut self, adjust: bool) -> Self {
        self.options.auto_adjust_overflow = adjust;
        self
    }
    pub fn gap(mut self, gap: Pixels) -> Self {
        self.options.gap = Some(gap);
        self
    }

    /// Some menu references keep their wrapper's original padding after a flip.
    pub(crate) fn flipped_gap(mut self, gap: Pixels) -> Self {
        self.options.flipped_gap = Some(finite_nonnegative(gap));
        self
    }
    pub fn offset(mut self, offset: Point<Pixels>) -> Self {
        self.options.offset = offset;
        self
    }
    pub fn z_index(mut self, z_index: usize) -> Self {
        self.options.z_index = z_index;
        self.explicit_z_index = true;
        self
    }
    pub fn mouse_enter_delay(mut self, delay: Duration) -> Self {
        self.options.enter_delay = delay;
        self.overrides.mouse_enter_delay = Some(delay);
        self
    }
    pub fn mouse_leave_delay(mut self, delay: Duration) -> Self {
        self.options.leave_delay = delay;
        self.overrides.mouse_leave_delay = Some(delay);
        self
    }
    pub fn disabled(mut self, disabled: bool) -> Self {
        self.options.disabled = disabled;
        self
    }
    pub fn background(mut self, color: impl Into<Hsla>) -> Self {
        self.options.background = Some(color.into());
        self
    }
    pub fn radius(mut self, radius: Pixels) -> Self {
        self.options.radius = Some(radius);
        self
    }
    pub fn arrow_size(mut self, size: Pixels) -> Self {
        self.options.arrow_size = Some(size);
        self
    }
    /// Attach composite controls to a measured sub-anchor while keeping the
    /// arrow outside the popup body's native content scroll mask.
    pub(crate) fn arrow_attachment(
        mut self,
        attachment: impl Fn(&PopupLayout, &App) -> Option<Point<Pixels>> + 'static,
    ) -> Self {
        self.options.arrow_attachment = Some(Rc::new(attachment));
        self
    }
    pub fn arrow_rounding(mut self, tip: Pixels, outer: Pixels) -> Self {
        self.options.arrow_tip_radius = Some(tip);
        self.options.arrow_outer_radius = Some(outer);
        self
    }
    pub fn shadows(mut self, shadows: ShadowList) -> Self {
        self.options.shadows = Some(shadows);
        self
    }
    pub fn surface_style(mut self, style: StyleRefinement) -> Self {
        self.options.surface_style = style;
        self
    }
    pub fn width(mut self, width: PopupWidth) -> Self {
        self.options.width = width;
        self
    }
    pub fn accessibility_role(mut self, role: Role) -> Self {
        self.options.role = role;
        self
    }
    pub fn accessibility_label(mut self, label: impl Into<SharedString>) -> Self {
        self.options.label = Some(label.into());
        self
    }
    pub fn automation_id(mut self, id: impl Into<SharedString>) -> Self {
        self.options.automation_id = Some(id.into());
        self
    }
    pub fn accessibility_description(mut self, description: impl Into<SharedString>) -> Self {
        self.options.description = Some(description.into());
        self
    }
    pub fn accessibility_busy(mut self, busy: bool) -> Self {
        self.options.busy = busy;
        self
    }
    pub fn initial_focus(mut self, focus: &FocusHandle) -> Self {
        self.options.initial_focus = Some(focus.clone());
        self
    }
    /// Override the entry fade duration. Reduced motion always takes precedence.
    pub fn motion_duration(mut self, duration: Duration) -> Self {
        self.options.motion_duration = Some(duration);
        self
    }

    /// Native navigation menus retain a noninteractive surface during their exit fade.
    pub(crate) fn exit_duration(mut self, duration: Duration) -> Self {
        self.options.exit_duration = duration;
        self
    }

    pub(crate) fn resolve_scope(&self, cx: &mut App) -> ConfigScope {
        self.scope
            .clone()
            .unwrap_or_else(|| ConfigScope::application(cx))
    }

    pub(crate) fn defaults(mut self, defaults: PopupConfig, z_index: usize) -> Self {
        let defaults = self.overrides.resolve(defaults);
        self.options.triggers = defaults.triggers;
        self.options.arrow = defaults.arrow;
        self.options.enter_delay = defaults.mouse_enter_delay;
        self.options.leave_delay = defaults.mouse_leave_delay;
        if !self.explicit_z_index {
            self.options.z_index = z_index;
        }
        self
    }

    pub(crate) fn appearance(
        mut self,
        mut style: StyleRefinement,
        background: Hsla,
        radius: Pixels,
        globals: &AliasTokens,
        outer_radius: f64,
    ) -> Self {
        style.refine(&self.options.surface_style);
        self.options.surface_style = style;
        self.options.background.get_or_insert(background);
        self.options.radius.get_or_insert(radius);
        self.options
            .alignment_radius
            .get_or_insert(px(globals.border_radius as f32));
        self.options
            .arrow_size
            .get_or_insert(px(globals.size_popup_arrow as f32));
        self.options
            .gap
            .get_or_insert(px(globals.margin_xxs as f32));
        self.options
            .arrow_tip_radius
            .get_or_insert(px(globals.border_radius_xs as f32));
        self.options
            .arrow_outer_radius
            .get_or_insert(px(outer_radius as f32));
        self.options
            .shadows
            .get_or_insert_with(|| globals.drop_shadow_popover.clone());
        self.options
            .motion_curve
            .get_or_insert(globals.motion_ease_out_circ);
        self.options.motion_enabled.get_or_insert(globals.motion);
        self.options.handoff_duration = globals.motion_duration_slow;
        self.options
            .motion_duration
            .get_or_insert(globals.motion_duration_mid);
        self
    }

    pub(crate) fn placement_value(&self) -> PopupPlacement {
        self.options.placement
    }
    pub(crate) fn disabled_when(mut self, disabled: bool) -> Self {
        self.options.disabled |= disabled;
        self
    }
    pub(crate) fn label_default(mut self, label: Option<SharedString>) -> Self {
        if self.options.label.is_none() {
            self.options.label = label;
        }
        self
    }
    pub(crate) fn handle_value(&self) -> Option<PopupHandle> {
        self.handle.clone()
    }
    pub(crate) fn description_default(mut self, description: Option<SharedString>) -> Self {
        if self.options.description.is_none() {
            self.options.description = description;
        }
        self
    }
    pub(crate) fn unique_group(mut self, group: Option<EntityId>) -> Self {
        self.options.unique_group = group;
        if group.is_some() {
            self.options.arrow.visible = false;
        }
        self
    }
}

pub(super) struct PopupState {
    pub id: EntityId,
    pub host: Host,
    pub parent: Option<EntityId>,
    pub modal_owner: Option<EntityId>,
    pub scope: ConfigScope,
    pub lifecycle: Lifecycle,
    pub options: Options,
    pub content: Option<Content>,
    pub handle: PopupHandle,
    pub on_change: Option<ChangeHandler>,
    pub trigger_focus: FocusHandle,
    pub surface_focus: Option<FocusHandle>,
    pub return_focus: Option<FocusHandle>,
    pub focus_pending: bool,
    resume_focus: bool,
    pub context_position: Option<Point<Pixels>>,
    pub anchor: Bounds<Pixels>,
    pub layout: Option<PopupLayout>,
    pub visual_layout: Option<PopupLayout>,
    pub unique_replacement: bool,
    unique_origin: Option<Bounds<Pixels>>,
    unique_motion: Option<BoundsTransition>,
    pub pointer_down: bool,
    pub keyboard_down: Option<SharedString>,
    hidden_callback: Option<HiddenHandler>,
    exit_generation: u64,
    exit_task: Option<Task<()>>,
    pub session: u64,
    pub trigger_metadata: Rc<RefCell<super::trigger::Metadata>>,
    epoch: Instant,
    opacity: ScalarTransition,
    visual_open: bool,
    pub exiting: bool,
    entry_started: Duration,
    frame_pending: bool,
    frame_generation: u64,
    unique_claimed: bool,
    timer: Option<(u64, Task<()>)>,
    _subscriptions: Vec<Subscription>,
}

impl PopupState {
    fn new(
        control: &Popup,
        scope: ConfigScope,
        host: Host,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        // A keyed popup can move while its controller/handle stays mounted.
        // Retire the old presentation before a replacement initializes resources.
        if let Some(previous) = control.handle.as_ref().and_then(PopupHandle::state) {
            suspend(&previous, cx);
        }
        let trigger_focus = cx.focus_handle();
        let weak = cx.weak_entity();
        let enter = window.on_focus_in(&trigger_focus, cx, move |window, cx| {
            if window.last_input_was_keyboard() {
                update_interest(&weak, window, cx, |state| {
                    if state.options.triggers.contains(PopupTriggers::FOCUS) {
                        state.lifecycle.focus(true);
                    }
                });
            }
        });
        let weak = cx.weak_entity();
        let leave = window.on_focus_out(&trigger_focus, cx, move |_, window, cx| {
            update_interest(&weak, window, cx, |state| {
                state.keyboard_down = None;
                state.lifecycle.focus(false);
            });
        });
        let scope_observer = scope.observe(cx);
        let handle = control.handle.clone().unwrap_or_default();
        *handle.0.borrow_mut() = Some(cx.weak_entity());
        let parent = host.borrow().parent;
        host.borrow_mut()
            .states
            .insert(cx.entity_id(), cx.weak_entity());
        cx.on_release(|state: &mut Self, cx| {
            let effect = state.finish_hidden(cx.background_executor().now());
            dispatch_hidden(effect, cx);
        })
        .detach();
        Self {
            id: cx.entity_id(),
            host,
            parent,
            modal_owner: crate::modal::host::owner(window, cx),
            scope,
            lifecycle: Lifecycle::new(control.open, control.default_open),
            options: control.options.clone(),
            content: None,
            handle,
            on_change: control.on_change.clone(),
            trigger_focus,
            surface_focus: None,
            return_focus: None,
            focus_pending: false,
            resume_focus: false,
            context_position: None,
            anchor: Bounds::default(),
            layout: None,
            visual_layout: None,
            unique_replacement: false,
            unique_origin: None,
            unique_motion: None,
            pointer_down: false,
            keyboard_down: None,
            hidden_callback: None,
            exit_generation: 0,
            exit_task: None,
            session: 0,
            trigger_metadata: Rc::default(),
            epoch: cx.background_executor().now(),
            opacity: ScalarTransition::new(0., CubicBezier::new(0.08, 0.82, 0.17, 1.)),
            visual_open: false,
            exiting: false,
            entry_started: Duration::ZERO,
            frame_pending: false,
            frame_generation: 0,
            unique_claimed: false,
            timer: None,
            _subscriptions: vec![enter, leave, scope_observer],
        }
    }

    fn sync(
        &mut self,
        control: &Popup,
        scope: ConfigScope,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Option<HiddenEffect> {
        if self.scope != scope {
            self._subscriptions[2] = scope.observe(cx);
            self.scope = scope;
        }
        let was_open = self.lifecycle.open();
        self.lifecycle.sync(control.open, control.options.disabled);
        if was_open && !self.lifecycle.open() {
            self.session = self.session.wrapping_add(1);
        }
        if self.options.unique_group != control.options.unique_group {
            self.release_unique(Some(cx.background_executor().now()));
        }
        self.options = control.options.clone();
        if self.content.is_none() {
            self.options.on_popup_click = None;
            self.options.arrow_attachment = None;
            self.options.initial_focus = None;
            self.options.initial_focus_with = None;
        }
        if was_open && !self.lifecycle.open() {
            self.begin_exit(window, cx);
        }
        self.on_change = control.on_change.clone();
        if let Some(handle) = &control.handle {
            *handle.0.borrow_mut() = Some(cx.weak_entity());
            self.handle = handle.clone();
        }
        if self.timer.as_ref().is_some_and(|(generation, _)| {
            self.lifecycle
                .pending()
                .is_none_or(|pending| pending.generation != *generation)
        }) {
            self.timer = None;
        }
        if !self.lifecycle.open() {
            if !self.exiting {
                self.release_unique(Some(cx.background_executor().now()));
                self.layout = None;
            }
            self.host.borrow_mut().entries.remove(&self.id);
        }
        self.sync_trigger_metadata();
        if !self.lifecycle.open() && !self.exiting {
            self.finish_hidden(cx.background_executor().now())
        } else {
            None
        }
    }

    /// Drop the complete presentation in one place. Essential trigger/controller
    /// state stays mounted; callbacks and heavyweight descendants do not.
    pub(super) fn finish_hidden(&mut self, now: Instant) -> Option<HiddenEffect> {
        if self.content.is_none() && self.surface_focus.is_none() && !self.exiting {
            if !self.lifecycle.open() {
                self.return_focus = None;
                self.resume_focus = false;
            }
            return None;
        }
        let yielded_unique =
            self.lifecycle.open() && self.options.unique_group.is_some() && !self.presented();
        if self.lifecycle.open() {
            // A hidden presentation invalidates async UI work while preserving
            // an authoritative controlled open value for later reconstruction.
            self.session = self.session.wrapping_add(1);
        }
        self.exit_generation = self.exit_generation.wrapping_add(1);
        self.exit_task = None;
        self.exiting = false;
        self.timer = None;
        self.frame_pending = false;
        self.frame_generation = self.frame_generation.wrapping_add(1);
        self.release_unique(Some(now));
        // A controlled owner that declined unique replacement stays yielded
        // until its authoritative open value changes; teardown must not reclaim.
        if yielded_unique {
            self.unique_claimed = true;
        }
        {
            let mut host = self.host.borrow_mut();
            host.entries.remove(&self.id);
            host.presentations.remove(&self.id);
            if host.entries.is_empty() {
                host.entries = Default::default();
            }
            if host.presentations.is_empty() {
                host.presentations = Default::default();
            }
        }
        self.content = None;
        self.surface_focus = None;
        if !self.lifecycle.open() {
            self.return_focus = None;
            self.resume_focus = false;
        }
        self.focus_pending = false;
        self.options.initial_focus = None;
        self.options.initial_focus_with = None;
        self.options.on_popup_click = None;
        self.options.arrow_attachment = None;
        self.layout = None;
        self.visual_layout = None;
        self.hidden_callback
            .take()
            .map(|callback| self.handle.pending_hidden(callback))
    }

    pub(super) fn install_source(&mut self, source: &Source, cx: &mut Context<Self>) {
        if self.content.is_none() {
            self.surface_focus = Some(cx.focus_handle());
            self.host
                .borrow_mut()
                .presentations
                .insert(self.id, cx.weak_entity());
            self.hidden_callback = source.on_hidden.clone();
        }
        self.content = Some(source.content.clone());
        self.options.on_popup_click = source.on_popup_click.clone();
        self.options.arrow_attachment = source.arrow_attachment.clone();
        self.options.initial_focus = source.initial_focus.clone().or_else(|| {
            self.resume_focus
                .then(|| self.surface_focus.clone())
                .flatten()
        });
        self.options.initial_focus_with = source.initial_focus_with.clone();
        if self.resume_focus {
            self.focus_pending = true;
            self.resume_focus = false;
        }
    }

    fn sync_trigger_metadata(&self) {
        let mut metadata = self.trigger_metadata.borrow_mut();
        metadata.tooltip = self.options.role == Role::Tooltip;
        metadata.menu = self.options.menu_trigger;
        metadata.enabled = !self.options.disabled;
        metadata.expanded = self.presented();
        metadata.clickable = self.options.triggers.contains(PopupTriggers::CLICK);
        metadata.description = self.options.label.clone();
    }

    fn release_unique(&mut self, now: Option<Instant>) {
        if let Some(group) = self.options.unique_group {
            let mut host = self.host.borrow_mut();
            if host
                .unique
                .get(&group)
                .is_some_and(|owner| owner.entity_id() == self.id)
            {
                host.unique.remove(&group);
                if host.unique.is_empty() {
                    host.unique = Default::default();
                }
            }
        }
        if let Some(group) = self.options.unique_group {
            let mut host = self.host.borrow_mut();
            if host
                .unique_geometry
                .get(&group)
                .is_some_and(|entry| entry.owner.entity_id() == self.id)
            {
                if let Some(now) = now {
                    let entry = host.unique_geometry.get_mut(&group).unwrap();
                    entry
                        .expires
                        .get_or_insert(now + self.options.handoff_duration);
                    host.prune_geometry(now);
                } else {
                    host.unique_geometry.remove(&group);
                }
            }
        }
        self.unique_claimed = false;
        self.visual_open = false;
        self.unique_motion = None;
        self.unique_origin = None;
        self.unique_replacement = false;
        self.visual_layout = None;
    }

    pub fn sample_background(
        &mut self,
        mut layout: PopupLayout,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> PopupLayout {
        let Some(group) = self.options.unique_group else {
            return layout;
        };
        let config = self.scope.snapshot(cx);
        let policy = MotionPolicy::new(
            config.motion
                && self
                    .options
                    .motion_enabled
                    .unwrap_or(config.theme.tokens.motion),
            cx.reduce_motion(),
        );
        let instant = cx.background_executor().now();
        let now = instant.duration_since(self.epoch);
        let motion = self.unique_motion.get_or_insert_with(|| {
            BoundsTransition::new(self.unique_origin.take().unwrap_or(layout.bounds))
        });
        motion.retarget(layout.bounds, now, self.options.handoff_duration, policy);
        let (bounds, finished) = motion.sample(now);
        layout.bounds = bounds;
        let mut host = self.host.borrow_mut();
        let used = host.geometry_tick();
        host.unique_geometry.insert(
            group,
            UniqueGeometry {
                owner: cx.weak_entity(),
                bounds,
                expires: None,
                used,
            },
        );
        drop(host);
        if !finished {
            self.request_frame(window, cx);
        }
        layout
    }

    fn request_frame(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if self.frame_pending {
            return;
        }
        self.frame_pending = true;
        self.frame_generation = self.frame_generation.wrapping_add(1);
        let generation = self.frame_generation;
        let weak = cx.weak_entity();
        window.on_next_frame(move |_, cx| {
            let _ = weak.update(cx, |state, cx| {
                if state.frame_generation == generation {
                    state.frame_pending = false;
                    cx.notify();
                }
            });
        });
    }

    pub fn presented(&self) -> bool {
        self.lifecycle.open()
            && self.options.unique_group.is_none_or(|group| {
                self.host
                    .borrow()
                    .unique
                    .get(&group)
                    .is_some_and(|owner| owner.entity_id() == self.id)
            })
    }

    fn begin_exit(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let config = self.scope.snapshot(cx);
        let policy = MotionPolicy::new(
            config.motion
                && self
                    .options
                    .motion_enabled
                    .unwrap_or(config.theme.tokens.motion),
            cx.reduce_motion(),
        );
        self.exiting = self.visual_open
            && !self.options.exit_duration.is_zero()
            && !self.options.disabled
            && self.options.unique_group.is_none()
            && policy.allows_motion();
        if self.exiting {
            self.visual_open = false;
            let now = cx.background_executor().now().duration_since(self.epoch);
            self.opacity
                .retarget(0., now, self.options.exit_duration, policy);
            self.exit_generation = self.exit_generation.wrapping_add(1);
            let generation = self.exit_generation;
            let timer = cx.background_executor().timer(self.options.exit_duration);
            self.exit_task = Some(cx.spawn_in(window, async move |weak, cx| {
                timer.await;
                let _ = cx.update(|window, cx| {
                    let callback = weak
                        .update(cx, |state, cx| {
                            if !state.exiting || state.exit_generation != generation {
                                return None;
                            }
                            let callback = state.finish_hidden(cx.background_executor().now());
                            cx.notify();
                            callback
                        })
                        .ok()
                        .flatten();
                    dispatch_hidden(callback, cx);
                    if !window.is_visible() {
                        host::schedule_hidden_cleanup(window, cx);
                    }
                });
            }));
        }
    }

    pub fn sample_opacity(
        &mut self,
        visible: bool,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> (f32, f32) {
        let config = self.scope.snapshot(cx);
        let g = &config.theme.tokens;
        let policy = MotionPolicy::new(
            config.motion && self.options.motion_enabled.unwrap_or(g.motion),
            cx.reduce_motion(),
        );
        let now = cx.background_executor().now().duration_since(self.epoch);
        if visible != self.visual_open {
            self.visual_open = visible;
            if visible {
                self.entry_started = now;
                if !self.exiting {
                    self.opacity = ScalarTransition::new(
                        0.,
                        self.options.motion_curve.unwrap_or(g.motion_ease_out_circ),
                    );
                }
                self.exiting = false;
                self.exit_generation = self.exit_generation.wrapping_add(1);
                self.exit_task = None;
                self.opacity.retarget(
                    1.,
                    now,
                    self.options
                        .motion_duration
                        .unwrap_or(g.motion_duration_mid),
                    policy,
                );
            } else if !self.exiting {
                self.opacity.retarget(0., now, Duration::ZERO, policy);
            }
        }
        if !policy.allows_motion() {
            self.opacity
                .retarget(f64::from(visible), now, Duration::ZERO, policy);
        }
        let sample = self.opacity.sample(now);
        if self.exiting && sample.finished {
            self.exiting = false;
            // Prepaint dispatches the hidden hook after this state borrow ends.
            self.request_frame(window, cx);
        }
        // Unique content uses Ant's linear fade (motionDurationMid), separate
        // from the decorative background's initial entry and geometry motion.
        let content = if self.options.unique_group.is_some()
            && visible
            && policy.allows_motion()
            && !g.motion_duration_mid.is_zero()
        {
            (now.saturating_sub(self.entry_started).as_secs_f64()
                / g.motion_duration_mid.as_secs_f64())
            .clamp(0., 1.) as f32
        } else {
            1.
        };
        if (visible || self.exiting) && (!sample.finished || content < 1.) {
            self.request_frame(window, cx);
        }
        let surface = if self.unique_replacement {
            1.
        } else {
            sample.value.clamp(0., 1.) as f32
        };
        (surface, content)
    }
}

impl Drop for PopupState {
    fn drop(&mut self) {
        self.release_unique(None);
        self.host.borrow_mut().entries.remove(&self.id);
        let mut host = self.host.borrow_mut();
        host.states.remove(&self.id);
        host.presentations.remove(&self.id);
        if host.states.is_empty() {
            host.states = Default::default();
        }
        if host.presentations.is_empty() {
            host.presentations = Default::default();
        }
    }
}

impl RenderOnce for Popup {
    fn render(self, window: &mut Window, cx: &mut App) -> impl IntoElement {
        let host =
            host::current(window, cx).expect("Popup requires UiRoot or PopupHost in this window");
        let scope = self
            .scope
            .clone()
            .unwrap_or_else(|| ConfigScope::application(cx));
        let state = window.use_keyed_state(self.id.clone(), cx, |window, cx| {
            PopupState::new(&self, scope.clone(), host, window, cx)
        });
        let callback = state.update(cx, |state, cx| state.sync(&self, scope, window, cx));
        dispatch_hidden(callback, cx);
        PopupElement {
            id: self.id,
            child: self.child,
            source: Source {
                content: self.content,
                on_hidden: self.on_hidden,
                on_presented: self.on_presented,
                on_popup_click: self.options.on_popup_click,
                arrow_attachment: self.options.arrow_attachment,
                initial_focus: self.options.initial_focus,
                initial_focus_with: self.options.initial_focus_with,
            },
            state,
        }
    }
}

/// Remember focus intent without retaining disposed popup controls. Only a
/// presentation that owned focus before native suspension may reclaim it.
pub(super) fn suspend_for_window(weak: &WeakEntity<PopupState>, window: &Window, cx: &mut App) {
    let _ = weak.update(cx, |state, cx| {
        // Native hiding interrupts the gesture even when a closed trigger has
        // not allocated any presentation resources yet. A later release must
        // not activate a press from before suspension.
        state.pointer_down = false;
        state.keyboard_down = None;
        state.resume_focus |= state.lifecycle.open()
            && state
                .surface_focus
                .as_ref()
                .is_some_and(|focus| focus.contains_focused(window, cx));
    });
    suspend(weak, cx);
}

pub(super) fn suspend(weak: &WeakEntity<PopupState>, cx: &mut App) {
    let callback = weak
        .update(cx, |state, cx| {
            let callback = state.finish_hidden(cx.background_executor().now());
            state.timer = None;
            state.focus_pending = false;
            cx.notify();
            callback
        })
        .ok()
        .flatten();
    dispatch_hidden(callback, cx);
}

pub(super) fn request_open(
    weak: &WeakEntity<PopupState>,
    open: bool,
    reason: PopupOpenReason,
    window: &mut Window,
    cx: &mut App,
) {
    update_interest(weak, window, cx, |state| {
        state.lifecycle.request(open, reason)
    });
}

pub(super) fn update_interest(
    weak: &WeakEntity<PopupState>,
    window: &mut Window,
    cx: &mut App,
    edit: impl FnOnce(&mut PopupState),
) {
    let Some(entity) = weak.upgrade() else {
        return;
    };
    let immediate = entity.update(cx, |state, cx| {
        edit(state);
        let pending = state.lifecycle.pending();
        if pending.is_none() {
            state.timer = None;
            return None;
        }
        let pending = pending.unwrap();
        if pending.delay.is_zero() {
            state.timer = None;
            return Some(pending.generation);
        }
        if state
            .timer
            .as_ref()
            .is_none_or(|(generation, _)| *generation != pending.generation)
        {
            let timer = cx.background_executor().timer(pending.delay);
            let task = cx.spawn_in(window, async move |weak, cx| {
                timer.await;
                let _ = cx.update(|window, cx| commit(&weak, pending.generation, window, cx));
            });
            state.timer = Some((pending.generation, task));
        }
        None
    });
    if let Some(generation) = immediate {
        commit(weak, generation, window, cx);
    }
}

fn commit(weak: &WeakEntity<PopupState>, generation: u64, window: &mut Window, cx: &mut App) {
    let Some(entity) = weak.upgrade() else {
        return;
    };
    let emitted = entity.update(cx, |state, cx| {
        let was_open = state.lifecycle.open();
        let change = state.lifecycle.commit(generation)?;
        if was_open && !state.lifecycle.open() {
            state.session = state.session.wrapping_add(1);
        }
        if change.open {
            state.return_focus = window.focused(cx);
            state.focus_pending =
                window.last_input_was_keyboard() || change.reason == PopupOpenReason::Accessibility;
        } else {
            state.focus_pending = false;
            state.context_position = None;
            if !state.lifecycle.open() {
                state.begin_exit(window, cx);
                if !state.exiting {
                    state.release_unique(Some(cx.background_executor().now()));
                    state.layout = None;
                }
                state.host.borrow_mut().entries.remove(&state.id);
                if matches!(
                    change.reason,
                    PopupOpenReason::Escape
                        | PopupOpenReason::Confirm
                        | PopupOpenReason::Cancel
                        | PopupOpenReason::Accessibility
                ) && state
                    .surface_focus
                    .as_ref()
                    .is_some_and(|focus| focus.contains_focused(window, cx))
                    && let Some(focus) = &state.return_focus
                {
                    window.focus(focus, cx);
                }
            }
        }
        let hidden = if !state.lifecycle.open() && !state.exiting {
            state.finish_hidden(cx.background_executor().now())
        } else {
            None
        };
        cx.notify();
        state.sync_trigger_metadata();
        Some((
            hidden,
            change,
            state.on_change.clone(),
            state.host.clone(),
            state.id,
        ))
    });
    if let Some((hidden, change, callback, host, id)) = emitted {
        dispatch_hidden(hidden, cx);
        if !change.open && !entity.read(cx).lifecycle.open() {
            let children = host.borrow().descendants(id);
            for child in children {
                request_open(&child, false, PopupOpenReason::ParentClosed, window, cx);
            }
        }
        // Release the state lease before calling user code. The callback can
        // synchronously inspect the handle, remove the popup or update its owner.
        if let Some(callback) = callback {
            callback(&change, window, cx);
        }
    }
}

pub(super) fn claim_unique(weak: &WeakEntity<PopupState>, window: &mut Window, cx: &mut App) {
    let Ok(previous) = weak.update(cx, |state, cx| {
        if !state.lifecycle.open() || state.unique_claimed {
            return None;
        }
        let group = state.options.unique_group?;
        state.unique_claimed = true;
        let mut host = state.host.borrow_mut();
        let now = cx.background_executor().now();
        state.unique_origin = host
            .unique_geometry
            .get(&group)
            .filter(|entry| {
                entry.owner.upgrade().is_some()
                    && entry.expires.is_none_or(|expires| now <= expires)
            })
            .map(|entry| entry.bounds);
        state.unique_replacement = state.unique_origin.is_some();
        if let Some(bounds) = state.unique_origin {
            let used = host.geometry_tick();
            host.unique_geometry.insert(
                group,
                UniqueGeometry {
                    owner: weak.clone(),
                    bounds,
                    expires: None,
                    used,
                },
            );
        }
        host.unique
            .insert(group, weak.clone())
            .filter(|old| old.entity_id() != state.id)
    }) else {
        return;
    };
    if let Some(previous) = previous {
        request_open(
            &previous,
            false,
            PopupOpenReason::UniqueReplaced,
            window,
            cx,
        );
        // A controlled owner may decline closure. It still yields the shared
        // surface, and the proposed transition remains observable to its owner.
        let _ = previous.update(cx, |state, cx| {
            state.layout = None;
            state.host.borrow_mut().entries.remove(&state.id);
            state.sync_trigger_metadata();
            cx.notify();
        });
    }
    let _ = weak.update(cx, |state, _| state.sync_trigger_metadata());
}

pub(super) fn finite_nonnegative(value: Pixels) -> Pixels {
    if f32::from(value).is_finite() {
        value.max(px(0.))
    } else {
        px(0.)
    }
}
