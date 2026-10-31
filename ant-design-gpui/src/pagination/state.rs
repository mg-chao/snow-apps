// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::model::{
    PaginationItemKind, PaginationMode, PaginationModel, PaginationSettings, QuickJumper,
    digit_page, page_count, simple_page,
};
use crate::{
    ConfigScope, InputEvent, InputState, SelectEvent, SelectOption, SelectOptions, SelectState,
    behavior::{ActivationGuard, Availability, focus_handle},
    input::{
        CommitReason, TextSelection,
        motion::{ColorMotion, CssTransition},
    },
    select::SelectSearch,
};
use gpui::{
    App, AppContext, Context, Entity, EventEmitter, FocusHandle, Focusable, SharedString,
    Subscription, Window,
};
use std::{collections::HashMap, rc::Rc, sync::Arc, time::Instant};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PaginationChangeSource {
    Page,
    Previous,
    Next,
    JumpPrevious,
    JumpNext,
    QuickJumper,
    Simple,
    PageSize,
    Accessibility,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PaginationChange {
    pub current: u64,
    pub page_size: u64,
    pub source: PaginationChangeSource,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PaginationSizeChange {
    /// The accepted page before changing the size, matching onShowSizeChange.
    pub current: u64,
    pub page_size: u64,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum PaginationEvent {
    ShowSizeChange(PaginationSizeChange),
    Change(PaginationChange),
}
pub(super) type EventHandler = Rc<dyn Fn(&PaginationEvent, &mut Window, &mut App)>;
pub(super) type ChangeHandler = Rc<dyn Fn(&PaginationChange, &mut Window, &mut App)>;
pub(super) type SizeChangeHandler = Rc<dyn Fn(&PaginationSizeChange, &mut Window, &mut App)>;

pub(super) struct ItemState {
    pub focus: FocusHandle,
    pub guard: ActivationGuard,
    pub hovered: bool,
    pub pressed: bool,
    pub suppressed: bool,
    pub colors: (ColorMotion, ColorMotion, ColorMotion),
    pub jump_opacity: CssTransition<f64>,
    _blur: Subscription,
    _focus: Subscription,
    _focus_out: Subscription,
}

pub(super) struct PaginationRenderConfig {
    pub total: u64,
    pub current: Option<u64>,
    pub default_current: u64,
    pub page_size: Option<u64>,
    pub default_page_size: u64,
    pub settings: PaginationSettings,
    pub disabled: Option<bool>,
    pub show_size_changer: bool,
    pub options_visible: bool,
    pub narrow: bool,
}

/// Retain one controller for each mounted Pagination. Text editors, the native
/// size selector, and a bounded collection of page focus handles survive renders.
pub struct PaginationState {
    pub(super) model: PaginationModel,
    pub(super) quick_editor: Entity<InputState>,
    pub(super) simple_editor: Entity<InputState>,
    pub(super) size_selector: Entity<SelectState<u64>>,
    pub(super) size_options: Arc<[u64]>,
    pub(super) confirm_focus: Option<FocusHandle>,
    pub(super) confirm_guard: Option<ActivationGuard>,
    pub(super) confirm_hovered: bool,
    pub(super) confirm_pointer_pressed: bool,
    pub(super) confirm_space_pressed: bool,
    pub(super) items: HashMap<PaginationItemKind, ItemState>,
    pub(super) epoch: Instant,
    pub(super) frame_pending: bool,
    pub(super) mounted: bool,
    pub(super) controlled_current: bool,
    pub(super) controlled_page_size: bool,
    pub(super) disabled_override: Option<bool>,
    pub(super) show_size_changer: bool,
    pub(super) options_visible: bool,
    pub(super) scope: Option<ConfigScope>,
    pub(super) on_event: Option<EventHandler>,
    pub(super) on_change: Option<ChangeHandler>,
    pub(super) on_show_size_change: Option<SizeChangeHandler>,
    pub(super) _scope_subscription: Option<Subscription>,
    fallback_focus: FocusHandle,
    raw_current: u64,
    current_initialized: bool,
    page_size_initialized: bool,
    recover_focus: bool,
    narrow: bool,
    quick_draft: SharedString,
    simple_draft: SharedString,
    size_options_key: Option<(Vec<u64>, SharedString, u64)>,
    _subscriptions: Vec<Subscription>,
}
impl EventEmitter<PaginationEvent> for PaginationState {}
impl Focusable for PaginationState {
    fn focus_handle(&self, cx: &App) -> FocusHandle {
        self.eligible_focus(cx)
            .unwrap_or_else(|| self.fallback_focus.clone())
    }
}
impl PaginationState {
    pub fn new(window: &mut Window, cx: &mut Context<Self>) -> Self {
        let quick_editor = cx.new(|cx| InputState::new(window, cx));
        let simple_editor = cx.new(|cx| {
            let mut input = InputState::new(window, cx);
            input.set_value("1", window, cx);
            input
        });
        let size_selector = cx.new(|cx| {
            let mut select = SelectState::new(SelectOptions::default(), window, cx);
            select.set_search(
                SelectSearch {
                    enabled: Some(true),
                    ..Default::default()
                },
                cx,
            );
            select
        });
        let quick_subscription =
            cx.subscribe_in(&quick_editor, window, |state, _, event, window, cx| {
                state.quick_event(event, window, cx);
            });
        let simple_subscription =
            cx.subscribe_in(&simple_editor, window, |state, _, event, window, cx| {
                state.simple_event(event, window, cx);
            });
        let size_subscription =
            cx.subscribe_in(&size_selector, window, |state, _, event, window, cx| {
                if let SelectEvent::Change(change) = event
                    && let Some(value) = change.values.first()
                {
                    state.change_page_size(value.value, window, cx);
                }
            });
        Self {
            model: PaginationModel::default(),
            quick_editor,
            simple_editor,
            size_selector,
            size_options: Arc::from([]),
            confirm_focus: None,
            confirm_guard: None,
            confirm_hovered: false,
            confirm_pointer_pressed: false,
            confirm_space_pressed: false,
            items: HashMap::new(),
            epoch: cx.background_executor().now(),
            frame_pending: false,
            mounted: false,
            controlled_current: false,
            controlled_page_size: false,
            disabled_override: None,
            show_size_changer: false,
            options_visible: true,
            scope: None,
            on_event: None,
            on_change: None,
            on_show_size_change: None,
            _scope_subscription: None,
            fallback_focus: cx.focus_handle().tab_stop(false),
            raw_current: 1,
            current_initialized: false,
            page_size_initialized: false,
            recover_focus: false,
            narrow: false,
            quick_draft: "".into(),
            simple_draft: "1".into(),
            size_options_key: None,
            _subscriptions: vec![quick_subscription, simple_subscription, size_subscription],
        }
    }
    pub fn current(&self) -> u64 {
        self.model.current()
    }
    pub fn page_size(&self) -> u64 {
        self.model.page_size()
    }
    pub fn total(&self) -> u64 {
        self.model.total()
    }
    pub fn page_count(&self) -> u64 {
        self.model.page_count()
    }
    pub fn range(&self) -> (u64, u64) {
        self.model.range()
    }
    pub fn quick_input(&self) -> &Entity<InputState> {
        &self.quick_editor
    }
    pub fn simple_input(&self) -> &Entity<InputState> {
        &self.simple_editor
    }
    pub fn size_selector(&self) -> &Entity<SelectState<u64>> {
        &self.size_selector
    }
    /// Programmatic updates are silent and do not create a user change event.
    pub fn set_current(&mut self, current: u64, window: &mut Window, cx: &mut Context<Self>) {
        self.raw_current = current.max(1);
        self.current_initialized = true;
        self.reconcile(window, cx);
    }
    pub fn set_page_size(&mut self, size: u64, window: &mut Window, cx: &mut Context<Self>) {
        self.page_size_initialized = true;
        if self.model.page_size == size.max(1) {
            return;
        }
        self.model.page_size = size.max(1);
        self.reconcile(window, cx);
        cx.notify();
    }
    pub fn set_total(&mut self, total: u64, window: &mut Window, cx: &mut Context<Self>) {
        if self.model.total == total {
            return;
        }
        self.model.total = total;
        self.reconcile(window, cx);
        cx.notify();
    }
    pub fn focus_page(&self, page: u64, window: &mut Window, cx: &mut App) {
        if self.mounted
            && let Some(item) = self.items.get(&PaginationItemKind::Page(page))
            && item.guard.availability().can_focus()
        {
            item.focus.focus(window, cx);
        }
    }
    pub fn focus(&self, window: &mut Window, cx: &mut App) {
        if let Some(focus) = self.eligible_focus(cx) {
            focus.focus(window, cx);
        }
    }
    fn eligible_focus(&self, cx: &App) -> Option<FocusHandle> {
        if !self.mounted || self.disabled(cx) || self.model.hidden() {
            return None;
        }
        if self.model.settings.mode == PaginationMode::Simple
            && !self.simple_editor.read(cx).is_disabled()
        {
            return Some(self.simple_editor.read(cx).focus_handle(cx));
        }
        self.items
            .get(&PaginationItemKind::Page(self.model.current))
            .filter(|item| item.guard.availability().can_focus())
            .or_else(|| {
                self.model.items().iter().find_map(|visible| {
                    self.items
                        .get(&visible.kind)
                        .filter(|item| item.guard.availability().can_focus())
                })
            })
            .map(|item| item.focus.clone())
    }
    pub fn blur(&self, window: &mut Window, cx: &mut App) {
        if self
            .items
            .values()
            .any(|item| item.focus.is_focused(window))
            || self
                .quick_editor
                .read(cx)
                .focus_handle(cx)
                .is_focused(window)
            || self
                .simple_editor
                .read(cx)
                .focus_handle(cx)
                .is_focused(window)
            || self
                .size_selector
                .read(cx)
                .focus_handle(cx)
                .is_focused(window)
            || self
                .confirm_focus
                .as_ref()
                .is_some_and(|focus| focus.is_focused(window))
        {
            window.blur(cx);
        }
    }
    pub(super) fn disabled(&self, cx: &App) -> bool {
        self.disabled_override.unwrap_or_else(|| {
            self.scope
                .as_ref()
                .is_some_and(|scope| scope.snapshot(cx).disabled)
        })
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
        self._scope_subscription = Some(scope.observe_in(window, cx, |state, window, cx| {
            state.sync_availability(window, cx);
            cx.notify();
        }));
        self.scope = Some(scope);
    }
    pub(super) fn sync_render(
        &mut self,
        config: PaginationRenderConfig,
        scope: ConfigScope,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        self.configure_scope(scope, window, cx);
        self.controlled_current = config.current.is_some();
        self.controlled_page_size = config.page_size.is_some();
        self.disabled_override = config.disabled;
        self.show_size_changer = config.show_size_changer;
        self.options_visible = config.options_visible;
        self.narrow = config.narrow;
        self.model.total = config.total;
        self.model.settings = config.settings;
        if let Some(current) = config.current {
            self.raw_current = current.max(1);
        } else if !self.current_initialized {
            self.raw_current = config.default_current.max(1);
        }
        if let Some(size) = config.page_size {
            self.model.page_size = size.max(1);
        } else if !self.page_size_initialized {
            self.model.page_size = config.default_page_size.max(1);
        }
        self.current_initialized = true;
        self.page_size_initialized = true;
        self.reconcile(window, cx);
    }
    fn reconcile(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let previous = self.model.current;
        self.model.current = self.raw_current;
        self.model.normalize();
        if self.model.current != previous {
            self.sync_simple_value(window, cx);
            cx.notify();
        }
        self.sync_items(window, cx);
        self.sync_availability(window, cx);
    }
    fn sync_simple_value(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        self.simple_draft = self.model.current.to_string().into();
        let value = self.simple_draft.clone();
        self.simple_editor
            .update(cx, |input, cx| input.set_value(&value, window, cx));
    }
    fn sync_items(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let items = if self.model.hidden() {
            Vec::new()
        } else {
            self.model.items()
        };
        let mut lost_focus = false;
        self.items.retain(|kind, item| {
            let retain = items
                .iter()
                .any(|visible| &visible.kind == kind && !(self.narrow && visible.hidden_at_narrow));
            if !retain {
                lost_focus |= item.focus.is_focused(window);
                item.guard.cancel();
            }
            retain
        });
        for item in items
            .into_iter()
            .filter(|item| !(self.narrow && item.hidden_at_narrow))
        {
            self.items.entry(item.kind).or_insert_with(|| {
                let focus = focus_handle(cx);
                let guard = ActivationGuard::default();
                let blur = guard.observe_blur(&focus, window, cx);
                let focused = cx.on_focus(&focus, window, |_, _, cx| cx.notify());
                let kind = item.kind;
                let focus_out = cx.on_blur(&focus, window, move |state, _, cx| {
                    if let Some(item) = state.items.get_mut(&kind) {
                        item.pressed = false;
                    }
                    cx.notify();
                });
                ItemState {
                    focus,
                    guard,
                    hovered: false,
                    pressed: false,
                    suppressed: false,
                    colors: Default::default(),
                    jump_opacity: Default::default(),
                    _blur: blur,
                    _focus: focused,
                    _focus_out: focus_out,
                }
            });
        }
        if lost_focus {
            if self.disabled(cx) || self.model.hidden() {
                window.blur(cx);
            } else if self.model.settings.mode == PaginationMode::Simple {
                self.simple_editor
                    .read(cx)
                    .focus_handle(cx)
                    .focus(window, cx);
            } else if let Some(focus) = self
                .items
                .get(&PaginationItemKind::Page(self.model.current))
                .map(|item| item.focus.clone())
            {
                focus.focus(window, cx);
            } else if let Some(focus) = self
                .model
                .items()
                .iter()
                .find(|item| !item.disabled)
                .and_then(|item| self.items.get(&item.kind))
                .map(|item| item.focus.clone())
            {
                focus.focus(window, cx);
            } else {
                window.blur(cx);
            }
        }
    }
    fn sync_availability(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let disabled = self.disabled(cx) || self.model.hidden() || !self.mounted;
        let confirm_disabled = disabled
            || !self.quick_visible()
            || self.model.settings.quick_jumper != QuickJumper::Confirm;
        if let Some(guard) = &self.confirm_guard {
            guard.set_availability(Availability {
                disabled: confirm_disabled,
                busy: false,
            });
        }
        if confirm_disabled {
            self.confirm_hovered = false;
            self.confirm_pointer_pressed = false;
            self.confirm_space_pressed = false;
        }
        if confirm_disabled
            && let Some(focus) = self.confirm_focus.take()
            && focus.is_focused(window)
        {
            window.blur(cx);
        }
        let visible = self.model.items();
        for (kind, state) in &mut self.items {
            let unavailable = disabled
                || state.suppressed
                || visible
                    .iter()
                    .find(|item| item.kind == *kind)
                    .is_none_or(|item| item.disabled);
            state.guard.update(
                Availability {
                    disabled: unavailable,
                    busy: false,
                },
                &state.focus,
                window,
                cx,
            );
            state.focus = state.focus.clone().tab_stop(!unavailable);
            if unavailable {
                state.hovered = false;
                state.pressed = false;
            }
        }
        let quick_disabled = disabled || !self.quick_visible();
        self.quick_editor.update(cx, |input, cx| {
            input.set_disabled(quick_disabled, window, cx)
        });
        let simple_disabled = disabled || self.model.settings.mode != PaginationMode::Simple;
        self.simple_editor.update(cx, |input, cx| {
            input.set_disabled(simple_disabled, window, cx)
        });
        if disabled || !self.show_size_changer || !self.options_visible {
            self.size_selector
                .update(cx, |select, cx| select.set_open(false, cx));
        }
    }
    pub(super) fn sync_size_options(
        &mut self,
        options: &[u64],
        suffix: &str,
        cx: &mut Context<Self>,
    ) {
        let unchanged = self
            .size_options_key
            .as_ref()
            .is_some_and(|(sizes, label, size)| {
                sizes == options && label.as_ref() == suffix && *size == self.model.page_size
            });
        if unchanged {
            return;
        }
        let mut sizes = options
            .iter()
            .copied()
            .filter(|size| *size > 0)
            .collect::<Vec<_>>();
        let mut seen = std::collections::HashSet::new();
        sizes.retain(|size| seen.insert(*size));
        if !sizes.contains(&self.model.page_size) {
            sizes.push(self.model.page_size);
            sizes.sort_unstable();
        }
        let choices = SelectOptions::new(sizes.iter().copied().map(|size| {
            SelectOption::new(
                SharedString::from(format!("pagination-size-{size}")),
                size,
                format!("{size} {suffix}"),
            )
        }))
        .expect("deduplicated page-size choices");
        self.size_options = sizes.into();
        self.size_selector.update(cx, |select, cx| {
            select.set_options(choices, cx);
            select.set_values([self.model.page_size], cx);
        });
        self.size_options_key = Some((
            options.to_vec(),
            suffix.to_owned().into(),
            self.model.page_size,
        ));
    }
    pub(super) fn restore_item(
        &mut self,
        kind: PaginationItemKind,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let boundary_disabled = match kind {
            PaginationItemKind::Page(_) => self.model.total == 0,
            PaginationItemKind::Previous => self.model.current <= 1,
            PaginationItemKind::Next => self.model.current >= self.model.page_count(),
            PaginationItemKind::JumpPrevious | PaginationItemKind::JumpNext => false,
        };
        let disabled =
            self.disabled(cx) || !self.mounted || self.model.hidden() || boundary_disabled;
        if let Some(item) = self.items.get_mut(&kind) {
            item.suppressed = false;
            item.guard.update(
                Availability {
                    disabled,
                    busy: false,
                },
                &item.focus,
                window,
                cx,
            );
            item.focus = item.focus.clone().tab_stop(!disabled);
        }
    }
    pub(super) fn suppress_item(
        &mut self,
        kind: PaginationItemKind,
        window: &mut Window,
        _: &mut Context<Self>,
    ) {
        if let Some(item) = self.items.get_mut(&kind) {
            self.recover_focus |= item.focus.is_focused(window);
            item.suppressed = true;
            item.hovered = false;
            item.pressed = false;
            item.guard.cancel();
            item.guard.set_availability(Availability {
                disabled: true,
                busy: false,
            });
            item.focus = item.focus.clone().tab_stop(false);
        }
    }
    pub(super) fn recover_rendered_focus(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if !std::mem::take(&mut self.recover_focus) {
            return;
        }
        if self.disabled(cx) || !self.mounted || self.model.hidden() {
            window.blur(cx);
            return;
        }
        let current = self
            .items
            .get(&PaginationItemKind::Page(self.model.current))
            .filter(|item| item.guard.availability().can_focus())
            .map(|item| item.focus.clone());
        let first = || {
            self.model
                .items()
                .iter()
                .filter_map(|visible| self.items.get(&visible.kind))
                .find(|item| item.guard.availability().can_focus())
                .map(|item| item.focus.clone())
        };
        if let Some(focus) = current.or_else(first) {
            focus.focus(window, cx);
        } else if self.model.settings.mode == PaginationMode::Simple
            && !self.simple_editor.read(cx).is_disabled()
        {
            self.simple_editor
                .read(cx)
                .focus_handle(cx)
                .focus(window, cx);
        } else {
            window.blur(cx);
        }
    }
    pub(super) fn quick_visible(&self) -> bool {
        self.options_visible
            && self.model.settings.quick_jumper != QuickJumper::Hidden
            && self.model.total > self.model.page_size
    }
    pub(super) fn activate(
        &mut self,
        kind: PaginationItemKind,
        source: PaginationChangeSource,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.disabled(cx) || !self.mounted || self.model.hidden() {
            return;
        }
        let Some(state) = self.items.get(&kind) else {
            return;
        };
        if !state.guard.availability().can_activate() {
            return;
        }
        if let Some(item) = self
            .model
            .items()
            .into_iter()
            .find(|item| item.kind == kind && !item.disabled)
        {
            self.request_page(item.target, source, window, cx);
        }
    }
    pub(super) fn request_page(
        &mut self,
        requested: u64,
        source: PaginationChangeSource,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.disabled(cx)
            || !self.mounted
            || self.model.hidden()
            || self.model.total == 0
            || requested == self.model.current
        {
            return;
        }
        let current = self.model.clamp(requested);
        if !self.controlled_current {
            self.raw_current = current;
            self.reconcile(window, cx);
        }
        if source == PaginationChangeSource::Simple && current == self.model.current {
            self.sync_simple_value(window, cx);
        }
        self.emit(
            PaginationEvent::Change(PaginationChange {
                current,
                page_size: self.model.page_size,
                source,
            }),
            window,
            cx,
        );
        cx.notify();
    }
    pub(super) fn change_page_size(
        &mut self,
        size: u64,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if size == 0
            || self.disabled(cx)
            || !self.mounted
            || self.model.hidden()
            || !self.show_size_changer
            || !self.options_visible
        {
            return;
        }
        let old_current = self.model.current;
        let last = page_count(self.model.total, size);
        let current = if last == 0 {
            old_current
        } else {
            old_current.min(last)
        };
        if !self.controlled_page_size {
            self.model.page_size = size;
        }
        if !self.controlled_current {
            self.raw_current = current;
        }
        self.reconcile(window, cx);
        self.emit(
            PaginationEvent::ShowSizeChange(PaginationSizeChange {
                current: old_current,
                page_size: size,
            }),
            window,
            cx,
        );
        self.emit(
            PaginationEvent::Change(PaginationChange {
                current,
                page_size: size,
                source: PaginationChangeSource::PageSize,
            }),
            window,
            cx,
        );
        cx.notify();
    }
    fn emit(&mut self, event: PaginationEvent, window: &mut Window, cx: &mut Context<Self>) {
        cx.emit(event.clone());
        if let Some(callback) = &self.on_event {
            callback(&event, window, cx);
        }
        match &event {
            PaginationEvent::Change(change) => {
                if let Some(callback) = &self.on_change {
                    callback(change, window, cx);
                }
            }
            PaginationEvent::ShowSizeChange(change) => {
                if let Some(callback) = &self.on_show_size_change {
                    callback(change, window, cx);
                }
            }
        }
    }
    fn rewrite(
        input: &Entity<InputState>,
        text: &str,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        input.update(cx, |input, cx| {
            let selection = input.selection();
            input.format_value(
                text,
                TextSelection {
                    anchor: selection.anchor.min(text.len()),
                    caret: selection.caret.min(text.len()),
                },
                true,
                window,
                cx,
            );
        });
    }
    fn quick_event(&mut self, event: &InputEvent, window: &mut Window, cx: &mut Context<Self>) {
        match event {
            InputEvent::Change(change) if !change.is_composing => {
                if change.value.bytes().all(|byte| byte.is_ascii_digit()) {
                    self.quick_draft = change.value.clone();
                } else {
                    Self::rewrite(&self.quick_editor, &self.quick_draft, window, cx);
                }
            }
            InputEvent::Commit(commit) if commit.reason == CommitReason::Enter => {
                self.commit_quick(window, cx)
            }
            InputEvent::Commit(commit)
                if commit.reason == CommitReason::Blur
                    && self.model.settings.quick_jumper == QuickJumper::Blur =>
            {
                let requested = digit_page(&self.quick_draft);
                self.clear_quick(window, cx);
                if let Some(requested) = requested {
                    cx.defer_in(window, move |state, window, cx| {
                        if state.quick_visible()
                            && !state
                                .items
                                .values()
                                .any(|item| item.focus.is_focused(window))
                        {
                            state.request_page(
                                requested,
                                PaginationChangeSource::QuickJumper,
                                window,
                                cx,
                            );
                        }
                    });
                }
            }
            _ => {}
        }
    }
    fn clear_quick(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        self.quick_draft = "".into();
        self.quick_editor
            .update(cx, |input, cx| input.set_value("", window, cx));
    }
    pub(super) fn commit_quick(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if !self.quick_visible() || self.disabled(cx) || self.quick_editor.read(cx).is_composing() {
            return;
        }
        let requested = digit_page(self.quick_editor.read(cx).value());
        self.clear_quick(window, cx);
        if let Some(requested) = requested {
            self.request_page(requested, PaginationChangeSource::QuickJumper, window, cx);
        }
    }
    fn simple_event(&mut self, event: &InputEvent, window: &mut Window, cx: &mut Context<Self>) {
        match event {
            InputEvent::Change(change) if !change.is_composing => {
                let text = change.value.as_ref();
                let number = text
                    .trim()
                    .parse::<f64>()
                    .ok()
                    .filter(|value| value.is_finite());
                if text.is_empty() {
                    self.simple_draft = "".into();
                } else if let Some(value) = digit_page(text) {
                    self.simple_draft = value.min(self.model.page_count()).to_string().into();
                    if self.simple_draft != change.value {
                        Self::rewrite(&self.simple_editor, &self.simple_draft, window, cx);
                    }
                } else if let Some(value) = number {
                    self.simple_draft =
                        value.min(self.model.page_count() as f64).to_string().into();
                    if self.simple_draft != change.value {
                        Self::rewrite(&self.simple_editor, &self.simple_draft, window, cx);
                    }
                } else {
                    Self::rewrite(&self.simple_editor, &self.simple_draft, window, cx);
                }
            }
            InputEvent::Commit(commit)
                if matches!(commit.reason, CommitReason::Enter | CommitReason::Blur) =>
            {
                self.commit_simple(window, cx)
            }
            _ => {}
        }
    }
    pub(super) fn commit_simple(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if self.model.settings.mode != PaginationMode::Simple
            || self.disabled(cx)
            || self.simple_editor.read(cx).is_composing()
        {
            return;
        }
        let text = self.simple_editor.read(cx).value();
        let value = if text.is_empty() {
            Some(0)
        } else {
            simple_page(text)
        };
        if let Some(value) = value {
            self.request_page(value, PaginationChangeSource::Simple, window, cx);
        }
    }
    /// In Ant Design's simple editor, Up selects the preceding page and Down
    /// selects the following page, rather than incrementing a numeric spinner.
    pub(super) fn step_simple(
        &mut self,
        increase: bool,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.model.settings.mode != PaginationMode::Simple
            || self.disabled(cx)
            || self.simple_editor.read(cx).is_composing()
        {
            return;
        }
        let text = self.simple_editor.read(cx).value();
        let value = if text.is_empty() {
            0
        } else if let Some(value) = simple_page(text) {
            value
        } else {
            return;
        };
        self.request_page(
            if increase {
                value.saturating_add(1)
            } else {
                value.saturating_sub(1)
            },
            PaginationChangeSource::Simple,
            window,
            cx,
        );
    }
    pub(super) fn unmount(&mut self, cx: &mut Context<Self>) {
        self.mounted = false;
        self.confirm_focus = None;
        if let Some(guard) = self.confirm_guard.take() {
            guard.cancel();
            guard.set_availability(Availability {
                disabled: true,
                busy: false,
            });
        }
        self.confirm_hovered = false;
        self.confirm_pointer_pressed = false;
        self.confirm_space_pressed = false;
        self.frame_pending = false;
        for item in self.items.values_mut() {
            item.guard.cancel();
            item.guard.set_availability(Availability {
                disabled: true,
                busy: false,
            });
            item.focus = item.focus.clone().tab_stop(false);
            item.hovered = false;
            item.pressed = false;
        }
        self.size_selector
            .update(cx, |select, cx| select.set_open(false, cx));
    }
    pub(super) fn request_frame(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if self.frame_pending || !self.mounted {
            return;
        }
        self.frame_pending = true;
        let weak = cx.weak_entity();
        window.on_next_frame(move |_, cx| {
            let _ = weak.update(cx, |state, cx| {
                state.frame_pending = false;
                if state.mounted {
                    cx.notify();
                }
            });
        });
    }
}
