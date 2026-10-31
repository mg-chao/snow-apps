// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::{
    SelectMode, SelectOption, SelectOptions, SelectSearch, SelectSettings, SelectTagFactory,
    SelectValue,
    model::{SelectModel, SelectionProposal},
};
use crate::{ConfigScope, InputEvent, InputState, popup::PopupOpenReason};
use gpui::{
    App, AppContext, Context, Entity, EventEmitter, FocusHandle, Focusable, ScrollStrategy,
    SharedString, Subscription, UniformListScrollHandle, Window,
};
use std::{hash::Hash, rc::Rc};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SelectChangeSource {
    Option,
    Remove,
    Clear,
    Keyboard,
    Token,
    Blur,
    Accessibility,
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SelectChange<T> {
    pub values: Vec<SelectValue<T>>,
    pub source: SelectChangeSource,
}
#[derive(Clone, Debug)]
pub enum SelectEvent<T> {
    Change(SelectChange<T>),
    Select(SelectOption<T>),
    Deselect(SelectValue<T>),
    Search(SharedString),
    Clear,
    OpenChange { open: bool, reason: PopupOpenReason },
    Active(Option<SelectValue<T>>),
    Focus,
    Blur,
}
pub(super) type EventHandler<T> = Rc<dyn Fn(&SelectEvent<T>, &mut Window, &mut App)>;

/// Keep one entity per mounted Select. It owns native text editing, selection,
/// filtered row snapshots and scroll position; rebuilding the view is inexpensive.
pub struct SelectState<T: 'static> {
    pub(super) model: SelectModel<T>,
    pub(super) editor: Entity<InputState>,
    pub(super) scroll: UniformListScrollHandle,
    pub(super) variable_list: Option<gpui::ListState>,
    pub(super) variable_key: Option<(u64, f32, usize)>,
    pub(super) open: bool,
    pub(super) hovered: bool,
    pub(super) initialized: bool,
    pub(super) open_initialized: bool,
    pub(super) controlled: bool,
    pub(super) controlled_open: bool,
    pub(super) controlled_search: bool,
    pub(super) disabled_override: Option<bool>,
    pub(super) scope: Option<ConfigScope>,
    pub(super) on_event: Option<EventHandler<T>>,
    pub(super) rendered_rows: usize,
    pub(super) surface: crate::input::motion::SurfaceMotion,
    pub(super) epoch: std::time::Instant,
    pub(super) mounted: bool,
    frame_pending: bool,
    enter_held: bool,
    _editor_subscription: Subscription,
    _scope_subscription: Option<Subscription>,
}
impl<T: 'static> EventEmitter<SelectEvent<T>> for SelectState<T> {}
impl<T: 'static> Focusable for SelectState<T> {
    fn focus_handle(&self, cx: &App) -> FocusHandle {
        self.editor.read(cx).focus_handle(cx)
    }
}
impl<T: Clone + Eq + Hash + 'static> SelectState<T> {
    /// Create an editable tags selector for values constructed from strings.
    /// Use `set_tag_factory` when a domain type needs validation or conversion.
    pub fn tags(options: SelectOptions<T>, window: &mut Window, cx: &mut Context<Self>) -> Self
    where
        T: From<String> + std::fmt::Display,
    {
        let mut state = Self::new(options, window, cx);
        state.model.set_settings(SelectSettings {
            mode: SelectMode::Tags,
            ..Default::default()
        });
        state.model.tag_factory = Some(Rc::new(|text| {
            Some(SelectOption::new(
                SharedString::from(format!("select-tag:{text}")),
                T::from(text.to_owned()),
                text.to_owned(),
            ))
        }));
        state
    }
    pub fn new(options: SelectOptions<T>, window: &mut Window, cx: &mut Context<Self>) -> Self {
        let editor = cx.new(|cx| InputState::new(window, cx));
        let subscription = cx.subscribe_in(&editor, window, |this, _, event, window, cx| {
            this.input_event(event, window, cx);
        });
        Self {
            model: SelectModel::new(options),
            editor,
            scroll: UniformListScrollHandle::default(),
            variable_list: None,
            variable_key: None,
            open: false,
            hovered: false,
            initialized: false,
            open_initialized: false,
            controlled: false,
            controlled_open: false,
            controlled_search: false,
            disabled_override: None,
            scope: None,
            on_event: None,
            rendered_rows: 0,
            surface: Default::default(),
            epoch: cx.background_executor().now(),
            mounted: false,
            frame_pending: false,
            enter_held: false,
            _editor_subscription: subscription,
            _scope_subscription: None,
        }
    }
    pub fn values(&self) -> &[SelectValue<T>] {
        &self.model.values
    }
    pub fn options(&self) -> &SelectOptions<T> {
        &self.model.options
    }
    pub fn settings(&self) -> &SelectSettings {
        &self.model.settings
    }
    pub fn search_value(&self) -> &str {
        &self.model.query
    }
    pub fn is_open(&self) -> bool {
        self.open
    }
    pub fn active_value(&self) -> Option<&T> {
        self.model
            .active
            .and_then(|i| self.model.option(i))
            .map(|o| &o.value)
    }
    pub fn input_state(&self) -> &Entity<InputState> {
        &self.editor
    }
    /// Silent programmatic update. Missing remote options retain their supplied labels.
    pub fn set_value(
        &mut self,
        values: impl IntoIterator<Item = SelectValue<T>>,
        cx: &mut Context<Self>,
    ) {
        self.initialized = true;
        if self.model.set_values(values.into_iter().collect()) {
            cx.notify();
        }
    }
    pub fn set_values(&mut self, values: impl IntoIterator<Item = T>, cx: &mut Context<Self>)
    where
        T: std::fmt::Display,
    {
        let values = values
            .into_iter()
            .map(|v| {
                let label = self
                    .model
                    .options
                    .option(&v)
                    .map(|o| o.label.clone())
                    .or_else(|| {
                        self.model
                            .values
                            .iter()
                            .find(|s| s.value == v)
                            .map(|s| s.label.clone())
                    })
                    .unwrap_or_else(|| v.to_string().into());
                SelectValue::new(v, label)
            })
            .collect::<Vec<_>>();
        self.set_value(values, cx);
    }
    pub fn set_options(&mut self, options: SelectOptions<T>, cx: &mut Context<Self>) {
        if self.model.set_options(options) {
            self.scroll_active();
            cx.notify();
        }
    }
    pub fn set_settings(&mut self, settings: SelectSettings, cx: &mut Context<Self>) {
        if self.model.set_settings(settings) {
            self.scroll_active();
            cx.notify();
        }
    }
    pub fn set_search(&mut self, policy: SelectSearch<T>, cx: &mut Context<Self>) {
        if self.model.set_search(policy) {
            self.scroll_active();
            cx.notify();
        }
    }
    pub fn set_tag_factory(&mut self, factory: SelectTagFactory<T>, cx: &mut Context<Self>) {
        if self
            .model
            .tag_factory
            .as_ref()
            .is_some_and(|f| Rc::ptr_eq(f, &factory))
        {
            return;
        }
        self.model.tag_factory = Some(factory);
        self.model.refilter();
        cx.notify();
    }
    pub fn set_tokenizer(
        &mut self,
        tokenizer: Option<super::SelectTokenizer>,
        cx: &mut Context<Self>,
    ) {
        self.model.tokenizer = tokenizer;
        cx.notify();
    }
    pub fn set_search_value(
        &mut self,
        value: impl Into<SharedString>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let value = value.into();
        if self.editor.read(cx).is_composing() && self.model.query == value {
            return;
        }
        if self.model.set_query(value.clone()) {
            self.scroll_active();
            cx.notify();
        }
        self.editor
            .update(cx, |editor, cx| editor.set_value(value, window, cx));
    }
    pub fn set_open(&mut self, open: bool, cx: &mut Context<Self>) {
        self.open_initialized = true;
        if self.open != open {
            self.open = open;
            if open {
                self.model.activate_selected();
                self.scroll_active();
            }
            cx.notify();
        }
    }
    pub fn focus(&self, window: &mut Window, cx: &mut App) {
        if !self.disabled(cx) {
            self.focus_handle(cx).focus(window, cx);
        }
    }
    pub fn blur(&self, window: &mut Window, cx: &mut App) {
        if self.focus_handle(cx).is_focused(window) {
            window.blur(cx);
        }
    }
    /// Scroll using the stable option identity, even after filtering or sorting.
    pub fn scroll_to(&self, value: &T, cx: &mut Context<Self>) -> bool {
        if let Some(i) = self.model.index_of_value(value) {
            self.scroll.scroll_to_item(i, ScrollStrategy::Top);
            if let Some(list) = &self.variable_list {
                list.scroll_to(gpui::ListOffset {
                    item_ix: i,
                    offset_in_item: gpui::px(0.),
                });
            }
            cx.notify();
            true
        } else {
            false
        }
    }
    pub(super) fn disabled(&self, cx: &App) -> bool {
        self.disabled_override
            .unwrap_or_else(|| self.scope.as_ref().is_some_and(|s| s.snapshot(cx).disabled))
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
        self._scope_subscription = Some(scope.observe_in(window, cx, |this, _, cx| {
            if this.disabled(cx) {
                this.open = false;
                this.enter_held = false;
            }
            cx.notify();
        }));
        self.scope = Some(scope);
    }
    fn emit(&mut self, event: SelectEvent<T>, window: &mut Window, cx: &mut Context<Self>) {
        cx.emit(event.clone());
        if let Some(callback) = &self.on_event {
            callback(&event, window, cx);
        }
    }
    pub(super) fn request_open(
        &mut self,
        open: bool,
        reason: PopupOpenReason,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if (open && self.disabled(cx)) || self.open == open {
            return;
        }
        if !self.controlled_open {
            self.set_open(open, cx);
            if !open && !self.model.settings.mode.multiple() && !self.model.query.is_empty() {
                self.query_request("".into(), true, window, cx);
            }
        }
        self.emit(SelectEvent::OpenChange { open, reason }, window, cx);
    }
    fn query_request(
        &mut self,
        query: SharedString,
        notify_search: bool,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if !self.controlled_search {
            self.set_search_value(query.clone(), window, cx);
        } else {
            // A rejected controlled query must not leave an uncommitted editor value.
            let accepted = self.model.query.clone();
            self.editor
                .update(cx, |editor, cx| editor.set_value(accepted, window, cx));
        }
        if notify_search {
            self.emit(SelectEvent::Search(query), window, cx);
        }
    }
    fn input_event(&mut self, event: &InputEvent, window: &mut Window, cx: &mut Context<Self>) {
        match event {
            InputEvent::Change(change)
                if !self.disabled(cx) && self.model.searchable() && !self.model.at_limit() =>
            {
                if change.is_composing {
                    // Preserve native preedit. Do not tokenize, select or rewrite it.
                    if !self.controlled_search && self.model.set_query(change.value.clone()) {
                        cx.notify();
                    }
                    return;
                }
                if self.model.settings.mode.multiple()
                    && let Some((proposal, tail)) = self.model.tokenize(&change.value, false)
                {
                    self.propose(proposal, SelectChangeSource::Token, window, cx);
                    self.query_request(tail, false, window, cx);
                    self.request_open(false, PopupOpenReason::Programmatic, window, cx);
                } else {
                    self.query_request(change.value.clone(), true, window, cx);
                    self.request_open(true, PopupOpenReason::Programmatic, window, cx);
                }
            }
            InputEvent::Focus => {
                self.emit(SelectEvent::Focus, window, cx);
                cx.notify();
            }
            InputEvent::Blur => {
                self.enter_held = false;
                if !self.disabled(cx) {
                    if self.model.settings.mode == SelectMode::Tags {
                        self.submit_tag(SelectChangeSource::Blur, window, cx);
                    } else if self.model.settings.mode.multiple() {
                        self.query_request("".into(), false, window, cx);
                    }
                    self.request_open(false, PopupOpenReason::Focus, window, cx);
                }
                self.emit(SelectEvent::Blur, window, cx);
                cx.notify();
            }
            _ => {}
        }
    }
    fn propose(
        &mut self,
        proposal: SelectionProposal<T>,
        source: SelectChangeSource,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let changed = proposal.values.as_slice() != self.model.values.as_ref();
        if !self.controlled {
            self.model.set_values(proposal.values.clone());
        }
        if changed {
            self.emit(
                SelectEvent::Change(SelectChange {
                    values: proposal.values,
                    source,
                }),
                window,
                cx,
            );
        }
        for option in proposal.selected {
            self.emit(SelectEvent::Select(option), window, cx);
        }
        if source != SelectChangeSource::Clear {
            for value in proposal.deselected {
                self.emit(SelectEvent::Deselect(value), window, cx);
            }
        }
        cx.notify();
    }
    pub(super) fn choose(
        &mut self,
        index: usize,
        source: SelectChangeSource,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.disabled(cx) || !self.model.enabled(index) {
            return;
        }
        let Some(proposal) = self.model.option(index).and_then(|o| self.model.toggle(o)) else {
            return;
        };
        self.propose(proposal, source, window, cx);
        if !self.model.settings.mode.multiple() || self.model.search.auto_clear {
            self.query_request("".into(), false, window, cx);
        }
        if !self.model.settings.mode.multiple() {
            self.request_open(false, PopupOpenReason::Click, window, cx);
        }
    }
    pub(super) fn remove(
        &mut self,
        value: &T,
        source: SelectChangeSource,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.disabled(cx) {
            return;
        }
        if let Some(proposal) = self.model.remove(value) {
            self.propose(proposal, source, window, cx);
        }
    }
    pub(super) fn clear(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if self.disabled(cx) {
            return;
        }
        self.emit(SelectEvent::Clear, window, cx);
        self.propose(
            SelectionProposal {
                values: Vec::new(),
                selected: Vec::new(),
                deselected: Vec::new(),
            },
            SelectChangeSource::Clear,
            window,
            cx,
        );
        self.query_request("".into(), false, window, cx);
        self.focus(window, cx);
    }
    fn submit_tag(
        &mut self,
        source: SelectChangeSource,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let query = self.model.query.trim();
        if query.is_empty() {
            return;
        }
        if let Some((proposal, _)) = self.model.tokenize(query, true) {
            self.propose(proposal, source, window, cx);
            self.query_request("".into(), false, window, cx);
        }
    }
    pub(super) fn activate(&mut self, index: usize, window: &mut Window, cx: &mut Context<Self>) {
        if self.model.active == Some(index) || !self.model.enabled(index) {
            return;
        }
        self.model.active = Some(index);
        self.emit_active(window, cx);
    }
    fn emit_active(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let value = self
            .model
            .active
            .and_then(|i| self.model.option(i))
            .map(SelectValue::from);
        self.emit(SelectEvent::Active(value), window, cx);
        cx.notify();
    }
    fn scroll_active(&self) {
        if let Some(index) = self.model.active {
            self.scroll.scroll_to_item(index, ScrollStrategy::Center);
            if let Some(list) = &self.variable_list {
                if list.bounds_for_item(index).is_some() {
                    list.scroll_to_reveal_item(index);
                } else {
                    list.scroll_to(gpui::ListOffset {
                        item_ix: index,
                        offset_in_item: gpui::px(0.),
                    });
                }
            }
        }
    }
    /// Notify the variable-height list when custom content changes its geometry.
    /// Dataset, width, theme and filter changes invalidate it automatically.
    pub fn invalidate_row_heights(&mut self, cx: &mut Context<Self>) {
        self.variable_key = None;
        cx.notify();
    }
    pub(super) fn key(&mut self, key: &str, window: &mut Window, cx: &mut Context<Self>) -> bool {
        if self.disabled(cx)
            || !self.focus_handle(cx).is_focused(window)
            || self.editor.read(cx).is_composing()
        {
            return false;
        }
        match key {
            "up" | "down" => {
                if self.open {
                    self.model.navigate(if key == "up" { -1 } else { 1 });
                    self.scroll_active();
                    self.emit_active(window, cx);
                } else {
                    self.request_open(true, PopupOpenReason::Programmatic, window, cx);
                }
                true
            }
            "enter" => {
                if self.enter_held {
                    return true;
                }
                self.enter_held = true;
                if self.open {
                    if let Some(index) = self.model.active {
                        self.choose(index, SelectChangeSource::Keyboard, window, cx);
                    } else if self.model.settings.mode == SelectMode::Tags {
                        self.submit_tag(SelectChangeSource::Keyboard, window, cx);
                    }
                } else if self.model.settings.mode == SelectMode::Tags
                    && !self.model.query.is_empty()
                {
                    self.submit_tag(SelectChangeSource::Keyboard, window, cx);
                } else {
                    self.request_open(true, PopupOpenReason::Programmatic, window, cx);
                }
                true
            }
            "escape" if self.open => {
                self.request_open(false, PopupOpenReason::Escape, window, cx);
                true
            }
            "tab" if self.open => {
                if let Some(index) = self.model.active {
                    self.choose(index, SelectChangeSource::Keyboard, window, cx);
                }
                self.request_open(false, PopupOpenReason::Programmatic, window, cx);
                true
            }
            "space" if !self.open => {
                self.request_open(true, PopupOpenReason::Programmatic, window, cx);
                !self.model.searchable()
            }
            "backspace" if self.model.settings.mode.multiple() && self.model.query.is_empty() => {
                let value = self
                    .model
                    .values
                    .iter()
                    .rev()
                    .find(|v| {
                        !self
                            .model
                            .options
                            .option(&v.value)
                            .is_some_and(|o| o.disabled)
                    })
                    .map(|v| v.value.clone());
                if let Some(value) = value {
                    self.remove(&value, SelectChangeSource::Keyboard, window, cx);
                }
                true
            }
            _ => false,
        }
    }
    pub(super) fn key_up(&mut self, key: &str) {
        if key == "enter" {
            self.enter_held = false;
        }
    }
    pub(super) fn request_frame(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if self.frame_pending || !self.mounted {
            return;
        }
        self.frame_pending = true;
        let weak = cx.entity().downgrade();
        window.on_next_frame(move |_, cx| {
            let _ = weak.update(cx, |this, cx| {
                this.frame_pending = false;
                if this.mounted {
                    cx.notify();
                }
            });
        });
    }
}
