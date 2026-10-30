// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::{
    DisabledTime, PickerFormat, PickerFormatContext, PickerMode, PickerPanelMode, PickerSelection,
    TimeOptions, TimeUnit,
    calendar::{
        CalendarCache, CalendarCell, add_months, add_years, callback_disables_cell, cell_for_date,
        panel_cells, period_bounds, start_of_week,
    },
    label::LabelCache,
    time::DisabledTimeSnapshot,
    value::TIME_ANCHOR,
};
use crate::{
    ConfigScope, InputEvent, InputState,
    input::{CommitReason, EditOrigin, TextSelection},
    popup::PopupHandle,
};
use chrono::{Duration, Local, NaiveDate, NaiveDateTime, NaiveTime};
use gpui::{
    App, AppContext, Bounds, Context, Entity, EventEmitter, FocusHandle, Focusable, Pixels,
    SharedString, Subscription, Task, UniformListScrollHandle, Window,
};
use std::{
    cell::RefCell, collections::HashMap, marker::PhantomData, rc::Rc, sync::Arc,
    time::Duration as StdDuration,
};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum PickerEndpoint {
    #[default]
    Start,
    End,
}
impl PickerEndpoint {
    pub fn index(self) -> usize {
        usize::from(self == Self::End)
    }
    pub(super) fn at(index: usize) -> Self {
        if index == 0 { Self::Start } else { Self::End }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PickerChangeSource {
    Input,
    Calendar,
    Time,
    Preset,
    Now,
    Clear,
    Keyboard,
    Accessibility,
    Tag,
}

#[derive(Clone, Debug, PartialEq)]
pub struct PickerChange<V> {
    pub value: V,
    pub text: Vec<SharedString>,
    pub source: PickerChangeSource,
    pub endpoint: Option<PickerEndpoint>,
}
#[derive(Clone, Debug, PartialEq)]
pub enum PickerEvent<V> {
    Change(PickerChange<V>),
    CalendarChange(PickerChange<V>),
    OpenChange(bool),
    PanelChange {
        date: NaiveDate,
        mode: PickerPanelMode,
    },
    Clear,
    Ok(V),
}

#[derive(Clone, Copy, Debug)]
pub struct DisabledDateInfo {
    pub from: Option<NaiveDate>,
    pub mode: PickerPanelMode,
}
pub(super) type DisabledDate = Rc<dyn Fn(NaiveDate, DisabledDateInfo) -> bool>;
pub(super) type DisabledTimeFactory = Rc<dyn Fn(NaiveDateTime, PickerEndpoint) -> DisabledTime>;
pub(super) type ChangeHandler<V> = Rc<dyn Fn(&PickerChange<V>, &mut Window, &mut App)>;
pub(super) type EventHandler<V> = Rc<dyn Fn(&PickerEvent<V>, &mut Window, &mut App)>;
pub(super) type OpenHandler = Rc<dyn Fn(bool, &mut Window, &mut App)>;
pub(super) type PanelHandler = Rc<dyn Fn(NaiveDate, PickerPanelMode, &mut Window, &mut App)>;
pub(super) type ActionHandler = Rc<dyn Fn(&mut Window, &mut App)>;
pub(super) type FormatCallback = Rc<dyn Fn(NaiveDateTime) -> SharedString>;

/// Selection policy shared by all typed picker facades.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PickerOptions {
    pub mode: PickerMode,
    pub need_confirm: Option<bool>,
    pub order: bool,
    pub allow_empty: [bool; 2],
    pub disabled_endpoints: [bool; 2],
    pub input_read_only: bool,
    pub preserve_invalid_on_blur: bool,
    pub preview_value: bool,
    pub show_now: Option<bool>,
    pub show_week: bool,
    pub min_date: Option<NaiveDate>,
    pub max_date: Option<NaiveDate>,
    pub time: TimeOptions,
}
impl Default for PickerOptions {
    fn default() -> Self {
        Self {
            mode: PickerMode::Date,
            need_confirm: None,
            order: true,
            allow_empty: [false; 2],
            disabled_endpoints: [false; 2],
            input_read_only: false,
            preserve_invalid_on_blur: false,
            preview_value: true,
            show_now: None,
            show_week: false,
            min_date: None,
            max_date: None,
            time: TimeOptions::default(),
        }
    }
}

#[derive(Clone, Debug)]
pub struct PickerCellGeometry {
    pub key: SharedString,
    pub text: SharedString,
    pub in_view: bool,
    pub panel: usize,
    pub date: Option<NaiveDate>,
    pub unit: Option<TimeUnit>,
    pub value: Option<u32>,
    pub bounds: Bounds<Pixels>,
    pub inner: Bounds<Pixels>,
    pub selected: bool,
    pub today: bool,
    pub disabled: bool,
    pub in_range: bool,
    pub range_start: bool,
    pub range_end: bool,
    pub range_hover: bool,
    pub week_selected: bool,
    pub week_range_start: bool,
    pub week_range_end: bool,
    pub week_range_hover: bool,
    pub week_hover: bool,
}
#[derive(Clone, Debug, Default)]
pub struct PickerGeometry {
    pub trigger: Option<Bounds<Pixels>>,
    pub prefix: Option<Bounds<Pixels>>,
    pub inputs: Vec<Bounds<Pixels>>,
    pub popup: Option<Bounds<Pixels>>,
    pub panels: Vec<Bounds<Pixels>>,
    pub headers: Vec<Bounds<Pixels>>,
    pub tables: Vec<Bounds<Pixels>>,
    pub cells: HashMap<SharedString, PickerCellGeometry>,
    pub time_columns: Vec<Bounds<Pixels>>,
    pub time_viewports: Vec<Bounds<Pixels>>,
    pub scrollbars: Vec<PickerScrollbarGeometry>,
    pub footer_actions: Vec<PickerFooterActionGeometry>,
    pub tags: Vec<PickerTagGeometry>,
    pub footer: Option<Bounds<Pixels>>,
    pub active_bar: Option<Bounds<Pixels>>,
    pub presets: Option<Bounds<Pixels>>,
}

impl PickerGeometry {
    /// Clear the previous painted frame while retaining warm collection storage.
    pub(super) fn clear_frame(&mut self) {
        self.trigger = None;
        self.prefix = None;
        self.popup = None;
        self.footer = None;
        self.active_bar = None;
        self.presets = None;
        self.inputs.clear();
        self.panels.clear();
        self.headers.clear();
        self.tables.clear();
        self.cells.clear();
        self.time_columns.clear();
        self.time_viewports.clear();
        self.scrollbars.clear();
        self.footer_actions.clear();
        self.tags.clear();
    }
}

#[derive(Clone, Debug)]
pub struct PickerTagGeometry {
    pub key: SharedString,
    pub label: SharedString,
    pub bounds: Bounds<Pixels>,
    pub content: Bounds<Pixels>,
    pub remove: Option<Bounds<Pixels>>,
}

#[derive(Clone, Debug)]
pub struct PickerScrollbarGeometry {
    pub column: usize,
    pub rail: Bounds<Pixels>,
    pub track: Bounds<Pixels>,
    pub thumb: Bounds<Pixels>,
    pub decrement: Bounds<Pixels>,
    pub increment: Bounds<Pixels>,
    pub viewport: Pixels,
    pub content: Pixels,
    pub maximum: Pixels,
    pub offset: Pixels,
}
#[derive(Clone, Debug)]
pub struct PickerFooterActionGeometry {
    pub key: SharedString,
    pub label: SharedString,
    pub bounds: Bounds<Pixels>,
}

#[derive(Clone, Copy, Debug)]
pub(super) struct PickerHover {
    pub value: NaiveDateTime,
    // Adjacent panels can contain different cells for the same date.
    panel: Option<usize>,
}

/// Retain one entity for each mounted picker. Builders never recreate its native editors.
pub struct PickerState<S: PickerSelection> {
    pub(super) accepted: S::Value,
    pub(super) draft: Vec<Option<NaiveDateTime>>,
    pub(super) controlled: bool,
    pub(super) initialized: bool,
    pub(super) open: bool,
    // Retain a close request's focus owner until a controlled parent accepts it.
    // The inner None represents a request made while the window had no focus.
    pending_focus_return: Option<Option<FocusHandle>>,
    pub(super) open_controlled: bool,
    pub(super) open_initialized: bool,
    pub(super) inline: bool,
    pub(super) mounted: bool,
    pub(super) generation: u64,
    callback_epoch: u64,
    proposal_epoch: u64,
    pub(super) disabled: bool,
    pub(super) hovered: bool,
    pub(super) prefix_width: Pixels,
    pub(super) active: usize,
    pub(super) confirmed: [bool; 2],
    pub(super) touched: [bool; 2],
    pub(super) invalid: [bool; 2],
    pub(super) mask_segment: [usize; 2],
    mask_pending: [String; 2],
    pub(super) options: PickerOptions,
    pub(super) format: PickerFormat,
    pub(super) format_context: PickerFormatContext,
    pub(super) locale: crate::locale::PickerLocale,
    pub(super) format_callback: Option<FormatCallback>,
    pub(super) scope: Option<ConfigScope>,
    pub(super) editors: [Entity<InputState>; 2],
    pub(super) panel_focus: FocusHandle,
    pub(super) column_focus: [FocusHandle; 5],
    pub(super) popup: PopupHandle,
    pub(super) panel_mode: PickerPanelMode,
    pub(super) view_date: NaiveDate,
    pub(super) view_controlled: bool,
    pub(super) default_picker_value: Option<NaiveDate>,
    pub(super) cursor: NaiveDateTime,
    pub(super) hover: Option<PickerHover>,
    range_from: Option<NaiveDate>,
    pub(super) disabled_date: Option<DisabledDate>,
    pub(super) disabled_time: Option<DisabledTimeFactory>,
    pub(super) clock: Rc<dyn Fn() -> NaiveDateTime>,
    pub(super) scrolls: [UniformListScrollHandle; 5],
    pub(super) scroll_keys: [Option<(TimeUnit, u32)>; 5],
    pub(super) scroll_generation: [u64; 5],
    scroll_tasks: [Option<Task<()>>; 5],
    pub(super) geometry: PickerGeometry,
    calendar_cache: CalendarCache,
    label_cache: RefCell<LabelCache>,
    pub(super) on_change: Option<ChangeHandler<S::Value>>,
    pub(super) on_calendar: Option<ChangeHandler<S::Value>>,
    pub(super) on_open: Option<OpenHandler>,
    pub(super) on_panel: Option<PanelHandler>,
    pub(super) on_clear: Option<ActionHandler>,
    pub(super) on_ok: Option<ActionHandler>,
    pub(super) on_event: Option<EventHandler<S::Value>>,
    _subscriptions: Vec<Subscription>,
    _marker: PhantomData<S>,
}
impl<S: PickerSelection> EventEmitter<PickerEvent<S::Value>> for PickerState<S> {}
impl<S: PickerSelection> Focusable for PickerState<S> {
    fn focus_handle(&self, cx: &App) -> FocusHandle {
        self.editors[self.active].read(cx).focus_handle(cx)
    }
}

impl<S: PickerSelection> PickerState<S> {
    pub fn new(window: &mut Window, cx: &mut Context<Self>) -> Self {
        let editors = std::array::from_fn(|_| cx.new(|cx| InputState::new(window, cx)));
        let mut subscriptions: Vec<Subscription> = editors
            .iter()
            .enumerate()
            .map(|(index, editor)| {
                cx.subscribe_in(editor, window, move |this, _, event, window, cx| {
                    this.editor_event(index, event, window, cx)
                })
            })
            .collect();
        subscriptions.push(cx.observe_window_activation(window, |state, window, _| {
            if !window.is_window_active() {
                state.cancel_scroll_tasks();
            }
        }));
        subscriptions.push(
            cx.observe_window_visibility(window, |state, visibility, _, _| {
                if !visibility.is_visible() {
                    state.cancel_scroll_tasks();
                }
            }),
        );
        let now = Local::now().naive_local();
        Self {
            accepted: S::empty(),
            draft: S::to_slots(&S::empty()),
            controlled: false,
            initialized: false,
            open: false,
            pending_focus_return: None,
            open_controlled: false,
            open_initialized: false,
            inline: false,
            mounted: false,
            generation: 0,
            callback_epoch: 0,
            proposal_epoch: 0,
            disabled: false,
            hovered: false,
            prefix_width: gpui::px(0.),
            active: 0,
            confirmed: [false; 2],
            touched: [false; 2],
            invalid: [false; 2],
            mask_segment: [0; 2],
            mask_pending: std::array::from_fn(|_| String::new()),
            options: PickerOptions::default(),
            format: PickerFormat::default(),
            format_context: PickerFormatContext::new(PickerMode::Date, S::HAS_DATE, S::HAS_TIME),
            locale: crate::locale::PickerLocale::default(),
            format_callback: None,
            scope: None,
            editors,
            panel_focus: crate::behavior::focus_handle(cx),
            column_focus: std::array::from_fn(|_| crate::behavior::focus_handle(cx)),
            popup: PopupHandle::new(),
            panel_mode: if S::HAS_DATE {
                PickerPanelMode::Date
            } else {
                PickerPanelMode::Time
            },
            view_date: now.date(),
            view_controlled: false,
            default_picker_value: None,
            cursor: now,
            hover: None,
            range_from: None,
            disabled_date: None,
            disabled_time: None,
            clock: Rc::new(|| Local::now().naive_local()),
            scrolls: std::array::from_fn(|_| UniformListScrollHandle::new()),
            scroll_keys: [None; 5],
            scroll_generation: [0; 5],
            scroll_tasks: std::array::from_fn(|_| None),
            geometry: PickerGeometry::default(),
            calendar_cache: CalendarCache::default(),
            label_cache: RefCell::new(LabelCache::default()),
            on_change: None,
            on_calendar: None,
            on_open: None,
            on_panel: None,
            on_clear: None,
            on_ok: None,
            on_event: None,
            _subscriptions: subscriptions,
            _marker: PhantomData,
        }
    }
    pub fn value(&self) -> &S::Value {
        &self.accepted
    }
    pub fn draft_value(&self) -> S::Value {
        S::from_slots(&self.draft)
    }
    pub fn is_open(&self) -> bool {
        self.open && !self.disabled
    }
    pub fn geometry(&self) -> &PickerGeometry {
        &self.geometry
    }
    pub(super) fn calendar_cells(
        &mut self,
        date: NaiveDate,
        mode: PickerPanelMode,
    ) -> Arc<[CalendarCell]> {
        self.calendar_cache
            .cells(date, mode, self.format_context.week_rules)
    }
    pub(super) fn cursor_in_view(&mut self, date: NaiveDate) -> bool {
        if !S::HAS_DATE {
            return true;
        }
        if self
            .calendar_cells(self.view_date, self.panel_mode)
            .iter()
            .any(|cell| cell.in_view && cell.contains(date))
        {
            return true;
        }
        if S::RANGE && !S::HAS_TIME && self.panel_mode == self.target_panel() {
            let right = match self.panel_mode {
                PickerPanelMode::Date => add_months(self.view_date, 1),
                PickerPanelMode::Month | PickerPanelMode::Quarter => add_years(self.view_date, 1),
                PickerPanelMode::Year => add_years(self.view_date, 10),
                _ => None,
            };
            if let Some(right) = right {
                return self
                    .calendar_cells(right, self.panel_mode)
                    .iter()
                    .any(|cell| cell.in_view && cell.contains(date));
            }
        }
        false
    }
    pub(super) fn numeric_label(
        &self,
        value: impl Into<i64>,
        minimum_digits: usize,
    ) -> SharedString {
        self.label_cache
            .borrow_mut()
            .number(value.into(), minimum_digits)
    }
    pub(super) fn decade_label(&self, start: i32, end: i32) -> SharedString {
        self.label_cache.borrow_mut().decade(start, end)
    }
    pub(super) fn quarter_label(&self, value: u32) -> SharedString {
        self.label_cache.borrow_mut().quarter(value)
    }
    pub fn active_endpoint(&self) -> PickerEndpoint {
        PickerEndpoint::at(self.active)
    }
    pub fn panel_date(&self) -> NaiveDate {
        self.view_date
    }
    pub fn panel_mode(&self) -> PickerPanelMode {
        self.panel_mode
    }
    pub fn input_values(&self, cx: &App) -> Vec<SharedString> {
        self.editors
            .iter()
            .take(if S::RANGE { 2 } else { 1 })
            .map(|editor| editor.read(cx).value_snapshot())
            .collect()
    }
    pub fn retained_editor_ids(&self) -> [gpui::EntityId; 2] {
        std::array::from_fn(|index| self.editors[index].entity_id())
    }
    pub fn focus_input(&mut self, index: usize, window: &mut Window, cx: &mut Context<Self>) {
        if index < if S::RANGE { 2 } else { 1 }
            && !self.disabled
            && !self.options.disabled_endpoints[index]
        {
            self.active = index;
            self.editors[index]
                .read(cx)
                .focus_handle(cx)
                .focus(window, cx);
            cx.notify();
        }
    }
    fn normalized(value: NaiveDateTime) -> NaiveDateTime {
        if !S::HAS_DATE {
            TIME_ANCHOR.and_time(value.time())
        } else if !S::HAS_TIME {
            value.date().and_time(NaiveTime::MIN)
        } else {
            value
        }
    }
    pub(super) fn clock_value(&self) -> NaiveDateTime {
        Self::normalized((self.clock)())
    }
    pub fn set_value(&mut self, value: S::Value, window: &mut Window, cx: &mut Context<Self>) {
        let changed = self.accepted != value
            || self.draft != S::to_slots(&value)
            || self.invalid.iter().any(|v| *v);
        self.accepted = value;
        self.proposal_epoch = self.proposal_epoch.wrapping_add(1);
        self.initialized = true;
        if changed {
            self.cancel_scroll_tasks();
            self.draft = S::to_slots(&self.accepted);
            self.hover = None;
            self.invalid = [false; 2];
            self.sync_editors(window, cx);
            cx.notify();
        }
    }
    pub(super) fn replace_controlled_value(
        &mut self,
        value: S::Value,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        self.cancel_scroll_tasks();
        self.proposal_epoch = self.proposal_epoch.wrapping_add(1);
        self.accepted = value;
        self.draft = S::to_slots(&self.accepted);
        self.hover = None;
        self.invalid = [false; 2];
        self.touched = [false; 2];
        self.range_from = None;
        self.mask_pending.iter_mut().for_each(String::clear);
        self.sync_editors(window, cx);
    }
    pub fn set_open(&mut self, open: bool, window: &mut Window, cx: &mut Context<Self>) {
        self.open_initialized = true;
        let open = open && !self.disabled;
        if open == self.open {
            return;
        }
        let return_focus = if open {
            self.pending_focus_return = None;
            false
        } else {
            self.pending_focus_return
                .take()
                .is_some_and(|owner| owner == window.focused(cx))
        };
        if !open && !self.need_confirm() && self.touched.iter().any(|touched| *touched) {
            self.commit(PickerChangeSource::Input, false, window, cx);
        }
        self.open = open;
        if open {
            self.begin_session(window, cx);
        } else {
            self.reset_draft(window, cx);
            self.geometry.popup = None;
            if return_focus {
                self.restore_editor_focus(window, cx);
            }
        }
        cx.notify();
    }
    pub fn focus(&self, window: &mut Window, cx: &mut App) {
        if !self.disabled {
            self.focus_handle(cx).focus(window, cx);
        }
    }
    pub fn blur(&self, window: &mut Window, cx: &mut App) {
        if self.owns_focus(window, cx) {
            window.blur(cx);
        }
    }
    pub(super) fn owns_focus(&self, window: &Window, cx: &App) -> bool {
        self.panel_focus.contains_focused(window, cx)
            || self
                .editors
                .iter()
                .any(|e| e.read(cx).focus_handle(cx).contains_focused(window, cx))
    }
    pub(super) fn need_confirm(&self) -> bool {
        self.options.need_confirm.unwrap_or(S::HAS_TIME)
    }
    pub(super) fn target_panel(&self) -> PickerPanelMode {
        if !S::HAS_DATE {
            return PickerPanelMode::Time;
        }
        match self.options.mode {
            PickerMode::Date | PickerMode::Week => PickerPanelMode::Date,
            PickerMode::Month => PickerPanelMode::Month,
            PickerMode::Quarter => PickerPanelMode::Quarter,
            PickerMode::Year => PickerPanelMode::Year,
        }
    }
    fn begin_session(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        self.pending_focus_return = None;
        self.cancel_scroll_tasks();
        // Opening from a focused text editor must not overwrite an unsubmitted
        // edit (or clear its native undo history) with the accepted value.
        let pending = (self.touched.iter().any(|value| *value)
            && (self.draft != S::to_slots(&self.accepted)
                || self.invalid.iter().any(|value| *value)))
        .then(|| {
            (
                self.draft.clone(),
                self.confirmed,
                self.touched,
                self.invalid,
                self.cursor,
                self.view_date,
                self.range_from,
                self.mask_segment,
                self.mask_pending.clone(),
            )
        });
        let preserved: Vec<_> = if self.options.preserve_invalid_on_blur {
            self.editors
                .iter()
                .enumerate()
                .filter(|(i, _)| self.invalid[*i])
                .map(|(i, editor)| (i, editor.read(cx).value_snapshot()))
                .collect()
        } else {
            Vec::new()
        };
        self.generation = self.generation.wrapping_add(1);
        self.proposal_epoch = self.proposal_epoch.wrapping_add(1);
        self.draft = S::to_slots(&self.accepted);
        self.confirmed = self.options.disabled_endpoints;
        self.touched = [false; 2];
        self.invalid = [false; 2];
        self.hover = None;
        self.range_from = None;
        self.mask_pending.iter_mut().for_each(String::clear);
        if self.options.disabled_endpoints[self.active] {
            self.active = usize::from(!self.options.disabled_endpoints[1]);
        }
        let now = self.clock_value();
        let now = if S::HAS_TIME {
            self.options
                .time
                .default_open_value
                .map(|time| now.date().and_time(time))
                .unwrap_or(now)
        } else {
            now
        };
        self.cursor = self
            .draft
            .get(self.active)
            .copied()
            .flatten()
            .or_else(|| self.draft.iter().flatten().copied().next())
            .unwrap_or(now);
        self.cursor = Self::normalized(self.cursor);
        if !self.view_controlled {
            self.view_date = self.default_picker_value.unwrap_or(self.cursor.date());
        }
        self.panel_mode = self.target_panel();
        self.scroll_keys = [None; 5];
        if let Some((
            draft,
            confirmed,
            touched,
            invalid,
            cursor,
            view_date,
            range_from,
            mask_segment,
            mask_pending,
        )) = pending
        {
            self.draft = draft;
            self.confirmed = confirmed;
            self.touched = touched;
            self.invalid = invalid;
            self.cursor = cursor;
            self.range_from = range_from;
            self.mask_segment = mask_segment;
            self.mask_pending = mask_pending;
            if !self.view_controlled {
                self.view_date = view_date;
            }
        } else {
            self.sync_editors(window, cx);
            for (index, text) in preserved {
                self.invalid[index] = true;
                self.editors[index].update(cx, |e, c| e.set_value(text.as_ref(), window, c));
            }
        }
    }
    pub(super) fn reset_draft(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        self.pending_focus_return = None;
        self.generation = self.generation.wrapping_add(1);
        self.proposal_epoch = self.proposal_epoch.wrapping_add(1);
        self.cancel_scroll_tasks();
        self.draft = S::to_slots(&self.accepted);
        self.hover = None;
        self.invalid = [false; 2];
        self.touched = [false; 2];
        self.range_from = None;
        self.mask_pending.iter_mut().for_each(String::clear);
        self.sync_editors(window, cx);
    }
    pub(super) fn release_mount(&mut self) {
        self.pending_focus_return = None;
        self.mounted = false;
        self.open = false;
        self.generation = self.generation.wrapping_add(1);
        self.callback_epoch = self.callback_epoch.wrapping_add(1);
        self.proposal_epoch = self.proposal_epoch.wrapping_add(1);
        self.cancel_scroll_tasks();
        self.geometry = PickerGeometry::default();
    }
    pub(super) fn request_open(
        &mut self,
        open: bool,
        cancel: bool,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if (open && self.disabled) || open == self.is_open() {
            return;
        }
        if !open && !cancel {
            self.pending_focus_return = None;
        }
        if self.open_controlled {
            self.emit(PickerEvent::OpenChange(open), window, cx);
            return;
        }
        if open {
            self.begin_session(window, cx);
        } else {
            if !cancel && !self.need_confirm() {
                self.commit(PickerChangeSource::Input, false, window, cx);
            }
            let preserved: Vec<_> = if !cancel && self.options.preserve_invalid_on_blur {
                self.editors
                    .iter()
                    .enumerate()
                    .filter(|(i, _)| self.invalid[*i])
                    .map(|(i, e)| (i, e.read(cx).value_snapshot()))
                    .collect()
            } else {
                Vec::new()
            };
            self.reset_draft(window, cx);
            for (index, text) in preserved {
                self.invalid[index] = true;
                self.editors[index].update(cx, |e, c| e.set_value(text.as_ref(), window, c));
            }
            self.geometry.popup = None;
        }
        if !self.open_controlled {
            self.open = open;
        }
        self.emit(PickerEvent::OpenChange(open), window, cx);
        cx.notify();
    }
    pub(super) fn close_and_restore_focus(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if self.inline {
            return;
        }
        self.pending_focus_return = Some(window.focused(cx));
        self.request_open(false, true, window, cx);
        if !self.is_open() {
            self.pending_focus_return = None;
            self.restore_editor_focus(window, cx);
        }
    }
    fn restore_editor_focus(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if !self.mounted || self.disabled || self.inline {
            return;
        }
        let count = if S::RANGE { 2 } else { 1 };
        let index = if self.active < count && !self.options.disabled_endpoints[self.active] {
            Some(self.active)
        } else {
            (0..count).find(|index| !self.options.disabled_endpoints[*index])
        };
        if let Some(index) = index {
            self.focus_input(index, window, cx);
        }
    }
    pub(super) fn format_value(&self, value: NaiveDateTime) -> SharedString {
        if self.default_week_format() {
            let (year, week) =
                super::week_year_number(value.date(), self.format_context.week_rules);
            return self.locale.week_label(year, week);
        }
        self.format_callback.as_ref().map_or_else(
            || self.format.format(value, self.format_context).into(),
            |f| f(value),
        )
    }
    fn default_week_format(&self) -> bool {
        S::HAS_DATE
            && !S::HAS_TIME
            && self.options.mode == PickerMode::Week
            && self.format.is_automatic()
            && self.format_callback.is_none()
    }
    fn parse_value(&self, text: &str) -> Option<NaiveDateTime> {
        if self.default_week_format() {
            if let crate::locale::PickerWeekOrdinal::Suffix(suffix) = &self.locale.week_ordinal {
                let body = text.trim().strip_suffix(suffix.as_ref())?;
                let (year, week) = body.rsplit_once('-')?;
                return Some(
                    super::date_from_week(
                        year.parse().ok()?,
                        week.parse().ok()?,
                        self.format_context.week_rules,
                    )?
                    .and_time(NaiveTime::MIN),
                );
            }
            let mut context = self.format_context;
            context.language = super::PickerLanguage::EnUs;
            return self.format.parse(text, context);
        }
        self.format.parse(text, self.format_context)
    }
    pub(super) fn sync_editors(&self, window: &mut Window, cx: &mut Context<Self>) {
        for index in 0..if S::RANGE { 2 } else { 1 } {
            let value = self.draft.get(index).copied().flatten();
            let text = value.map(|v| self.format_value(v)).unwrap_or_else(|| {
                if self.editors[index]
                    .read(cx)
                    .focus_handle(cx)
                    .is_focused(window)
                {
                    self.format
                        .mask_format()
                        .map(|m| SharedString::from(m.template().to_owned()))
                        .unwrap_or_default()
                } else {
                    SharedString::default()
                }
            });
            self.editors[index]
                .update(cx, |editor, cx| editor.set_value(text.as_ref(), window, cx));
        }
    }
    fn change(&self, source: PickerChangeSource) -> PickerChange<S::Value> {
        PickerChange {
            value: S::from_slots(&self.draft),
            text: self
                .draft
                .iter()
                .map(|v| v.map(|v| self.format_value(v)).unwrap_or_default())
                .collect(),
            source,
            endpoint: S::RANGE.then(|| self.active_endpoint()),
        }
    }
    fn emit(&self, event: PickerEvent<S::Value>, window: &mut Window, cx: &mut Context<Self>) {
        cx.emit(event.clone());
        let change = self.on_change.clone();
        let calendar = self.on_calendar.clone();
        let open = self.on_open.clone();
        let panel = self.on_panel.clone();
        let clear = self.on_clear.clone();
        let ok = self.on_ok.clone();
        let all = self.on_event.clone();
        let weak = cx.weak_entity();
        let epoch = self.callback_epoch;
        window.defer(cx, move |w, c| {
            let alive = |c: &App| {
                weak.upgrade().is_some_and(|state| {
                    let s = state.read(c);
                    s.callback_epoch == epoch && s.mounted && !s.disabled
                })
            };
            if !alive(c) {
                return;
            }
            if let Some(f) = all {
                f(&event, w, c);
            }
            if !alive(c) {
                return;
            }
            match event {
                PickerEvent::Change(v) => {
                    if let Some(f) = change {
                        f(&v, w, c)
                    }
                }
                PickerEvent::CalendarChange(v) => {
                    if let Some(f) = calendar {
                        f(&v, w, c)
                    }
                }
                PickerEvent::OpenChange(v) => {
                    if let Some(f) = open {
                        f(v, w, c)
                    }
                }
                PickerEvent::PanelChange { date, mode } => {
                    if let Some(f) = panel {
                        f(date, mode, w, c)
                    }
                }
                PickerEvent::Clear => {
                    if let Some(f) = clear {
                        f(w, c)
                    }
                }
                PickerEvent::Ok(_) => {
                    if let Some(f) = ok {
                        f(w, c)
                    }
                }
            }
        });
    }
    pub(super) fn disabled_day(&self, date: NaiveDate) -> bool {
        self.disabled_cell(&cell_for_date(
            date,
            self.panel_mode,
            self.format_context.week_rules,
        ))
    }
    fn disabled_date_info(&self, mode: PickerPanelMode) -> DisabledDateInfo {
        DisabledDateInfo {
            from: if S::RANGE {
                self.range_from.or_else(|| {
                    self.draft
                        .first()
                        .copied()
                        .flatten()
                        .map(|date| date.date())
                })
            } else {
                None
            },
            mode,
        }
    }
    pub(super) fn disabled_cell(&self, cell: &CalendarCell) -> bool {
        self.disabled
            || self.options.disabled_endpoints[self.active]
            || self
                .options
                .min_date
                .zip(self.options.max_date)
                .is_some_and(|(min, max)| min > max)
            || self
                .options
                .min_date
                .is_some_and(|min| cell.period_end < min)
            || self
                .options
                .max_date
                .is_some_and(|max| cell.period_start > max)
            || self.disabled_date.as_ref().is_some_and(|f| {
                let info = self.disabled_date_info(self.panel_mode);
                callback_disables_cell(cell, self.panel_mode, |date| f(date, info))
            })
    }
    pub(super) fn time_constraints(&self, value: NaiveDateTime) -> DisabledTime {
        self.disabled_time
            .as_ref()
            .map(|f| f(value, self.active_endpoint()))
            .unwrap_or_default()
    }
    pub(super) fn visible_time_values(
        &self,
        unit: TimeUnit,
        value: NaiveDateTime,
    ) -> (Arc<[u32]>, DisabledTimeSnapshot) {
        let disabled = self.time_constraints(value).snapshot(unit, value.time());
        let values = self
            .options
            .time
            .visible_values(unit, value.time(), &disabled);
        (values, disabled)
    }
    fn valid_value(&self, value: NaiveDateTime, index: usize) -> bool {
        let (start, end) = period_bounds(
            value.date(),
            self.options.mode,
            self.format_context.week_rules,
        )
        .unwrap_or((value.date(), value.date()));
        if S::HAS_DATE
            && (self
                .options
                .min_date
                .zip(self.options.max_date)
                .is_some_and(|(min, max)| min > max)
                || self.options.min_date.is_some_and(|min| end < min)
                || self.options.max_date.is_some_and(|max| start > max)
                || self.disabled_date.as_ref().is_some_and(|f| {
                    let mode = self.target_panel();
                    let cell = cell_for_date(value.date(), mode, self.format_context.week_rules);
                    let info = self.disabled_date_info(mode);
                    callback_disables_cell(&cell, mode, |date| f(date, info))
                }))
        {
            return false;
        }
        !S::HAS_TIME
            || self
                .disabled_time
                .as_ref()
                .map(|f| f(value, PickerEndpoint::at(index)))
                .unwrap_or_default()
                .valid(value.time())
    }
    pub(super) fn can_commit(&self) -> bool {
        !self.disabled
            && !self
                .invalid
                .iter()
                .take(if S::RANGE { 2 } else { 1 })
                .any(|v| *v)
            && self.draft.iter().enumerate().all(|(i, value)| {
                if S::RANGE && self.options.disabled_endpoints[i] {
                    return true;
                }
                match value {
                    Some(value) => self.valid_value(*value, if S::MULTIPLE { 0 } else { i }),
                    None if S::RANGE => self.options.allow_empty[i],
                    None => !S::HAS_TIME,
                }
            })
            && (!S::RANGE
                || !self.options.order
                || match (self.draft[0], self.draft[1]) {
                    (Some(start), Some(end))
                        if self.options.disabled_endpoints.iter().any(|v| *v) =>
                    {
                        start <= end
                    }
                    _ => true,
                })
    }
    pub(super) fn can_submit(&self) -> bool {
        if self.disabled || self.invalid[self.active] {
            return false;
        }
        if !S::HAS_DATE
            && self.draft.get(self.active).copied().flatten().is_none()
            && let Some(time) = self.options.time.default_open_value
        {
            return self.valid_value(TIME_ANCHOR.and_time(time), self.active);
        }
        if !S::RANGE {
            return self.can_commit() && (S::MULTIPLE || self.draft.iter().any(Option::is_some));
        }
        match self.draft.get(self.active).copied().flatten() {
            Some(value) => self.valid_value(value, self.active),
            None => self.options.allow_empty[self.active] && self.draft.iter().any(Option::is_some),
        }
    }
    fn commit(
        &mut self,
        source: PickerChangeSource,
        clear: bool,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> bool {
        if !clear && !self.can_commit() {
            return false;
        }
        if self.options.order {
            if S::MULTIPLE {
                self.draft.sort_unstable();
                self.draft.dedup();
            }
            if S::RANGE
                && !self.options.disabled_endpoints.iter().any(|v| *v)
                && self.draft.len() == 2
                && let (Some(a), Some(b)) = (self.draft[0], self.draft[1])
                && a > b
            {
                self.draft.swap(0, 1);
            }
        }
        if !clear && !self.can_commit() {
            return false;
        }
        let change = self.change(source);
        if change.value != self.accepted {
            if !self.controlled {
                self.accepted = change.value.clone();
            }
            let proposal = change.value.clone();
            self.emit(PickerEvent::Change(change), window, cx);
            if self.controlled {
                self.proposal_epoch = self.proposal_epoch.wrapping_add(1);
                let epoch = self.proposal_epoch;
                let weak = cx.weak_entity();
                window.defer(cx, move |window, cx| {
                    let _ = weak.update(cx, |state, cx| {
                        if state.proposal_epoch == epoch
                            && state.controlled
                            && state.accepted != proposal
                        {
                            state.draft = S::to_slots(&state.accepted);
                            state.hover = None;
                            state.invalid = [false; 2];
                            state.sync_editors(window, cx);
                            cx.notify();
                        }
                    });
                });
            }
        }
        self.sync_editors(window, cx);
        cx.notify();
        true
    }
    pub(super) fn submit(
        &mut self,
        source: PickerChangeSource,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> bool {
        if self.disabled || self.invalid[self.active] {
            return false;
        }
        if !S::MULTIPLE && self.draft.get(self.active).copied().flatten().is_none() {
            if !S::HAS_DATE
                && let Some(time) = self.options.time.default_open_value
            {
                let value = TIME_ANCHOR.and_time(time);
                if !self.valid_value(value, self.active) {
                    return false;
                }
                self.assign(value);
                self.emit(PickerEvent::CalendarChange(self.change(source)), window, cx);
            } else if (S::RANGE && !self.options.allow_empty[self.active])
                || (!S::RANGE && (S::HAS_TIME || !self.touched[self.active]))
            {
                return false;
            }
        }
        self.confirmed[self.active] = true;
        if S::RANGE
            && !self.confirmed[1 - self.active]
            && !self.options.allow_empty[1 - self.active]
        {
            self.active = 1 - self.active;
            self.focus(window, cx);
            cx.notify();
            return true;
        }
        if self.commit(source, false, window, cx) {
            self.emit(PickerEvent::Ok(S::from_slots(&self.draft)), window, cx);
            self.close_and_restore_focus(window, cx);
            true
        } else {
            false
        }
    }
    fn assign(&mut self, value: NaiveDateTime) {
        let value = Self::normalized(value);
        if S::RANGE && self.range_from.is_none() {
            self.range_from = Some(value.date());
        }
        if S::MULTIPLE {
            let period = |v: NaiveDateTime| {
                period_bounds(v.date(), self.options.mode, self.format_context.week_rules)
                    .map(|p| p.0)
            };
            if let Some(index) = self
                .draft
                .iter()
                .position(|v| v.is_some_and(|v| period(v) == period(value)))
            {
                self.draft.remove(index);
            } else {
                self.draft.push(Some(value));
            }
        } else {
            if self.draft.len() < if S::RANGE { 2 } else { 1 } {
                self.draft.resize(if S::RANGE { 2 } else { 1 }, None);
            }
            self.draft[self.active] = Some(value);
        }
        self.cursor = value;
        self.hover = None;
        self.touched[self.active] = true;
        self.invalid[self.active] = false;
    }
    pub(super) fn select_date(
        &mut self,
        date: NaiveDate,
        source: PickerChangeSource,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let date = cell_for_date(date, self.panel_mode, self.format_context.week_rules).date;
        if self.disabled_day(date) {
            return;
        }
        let target = self.target_panel();
        if self.panel_mode != target {
            if self.is_open() || self.inline {
                self.panel_focus.focus(window, cx);
            }
            if !self.view_controlled {
                self.view_date = date;
            }
            self.cursor = date.and_time(self.cursor.time());
            self.panel_mode = match self.panel_mode {
                PickerPanelMode::Decade => PickerPanelMode::Year,
                PickerPanelMode::Year if target != PickerPanelMode::Year => {
                    if target == PickerPanelMode::Quarter {
                        PickerPanelMode::Quarter
                    } else {
                        PickerPanelMode::Month
                    }
                }
                _ => target,
            };
            self.emit(
                PickerEvent::PanelChange {
                    date,
                    mode: self.panel_mode,
                },
                window,
                cx,
            );
            cx.notify();
            return;
        }
        let time = self
            .draft
            .get(self.active)
            .copied()
            .flatten()
            .map(|v| v.time())
            .or(self.options.time.default_open_value)
            .unwrap_or_else(|| {
                if S::HAS_TIME {
                    (self.clock)().time()
                } else {
                    NaiveTime::MIN
                }
            });
        let value = date.and_time(time);
        let value = if S::HAS_TIME {
            let Some(time) = self.options.time.align(time, &self.time_constraints(value)) else {
                return;
            };
            date.and_time(time)
        } else {
            value
        };
        if !self.valid_value(value, self.active) {
            return;
        }
        self.assign(value);
        self.emit(PickerEvent::CalendarChange(self.change(source)), window, cx);
        self.sync_editors(window, cx);
        if !self.need_confirm() && !S::HAS_TIME {
            if S::RANGE {
                self.confirmed[self.active] = true;
                if self.confirmed[1 - self.active] || self.options.allow_empty[1 - self.active] {
                    if self.commit(source, false, window, cx) {
                        self.close_and_restore_focus(window, cx);
                    }
                } else {
                    self.active = 1 - self.active;
                    self.focus(window, cx);
                }
            } else {
                if self.commit(source, false, window, cx) && !S::MULTIPLE {
                    self.close_and_restore_focus(window, cx);
                }
            }
        }
        cx.notify();
    }
    pub(super) fn select_time(
        &mut self,
        unit: TimeUnit,
        value: u32,
        source: PickerChangeSource,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.disabled || self.options.disabled_endpoints[self.active] {
            return;
        }
        let current = self
            .draft
            .get(self.active)
            .copied()
            .flatten()
            .unwrap_or(self.cursor);
        let disabled = self.time_constraints(current);
        if disabled.disabled(unit, value, current.time()) {
            return;
        }
        let Some(time) = unit
            .set(current.time(), value)
            .and_then(|t| self.options.time.align(t, &disabled))
        else {
            return;
        };
        let value = current.date().and_time(time);
        if !self.valid_value(value, self.active) {
            return;
        }
        self.assign(value);
        self.emit(PickerEvent::CalendarChange(self.change(source)), window, cx);
        self.sync_editors(window, cx);
        if !self.need_confirm() {
            self.commit(source, false, window, cx);
        }
        cx.notify();
    }
    pub(super) fn clear(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if self.disabled {
            return;
        }
        if S::MULTIPLE {
            self.draft.clear();
        } else {
            for (i, value) in self.draft.iter_mut().enumerate() {
                if !self.options.disabled_endpoints[i] {
                    *value = None;
                }
            }
        }
        self.invalid = [false; 2];
        self.hover = None;
        self.emit(
            PickerEvent::CalendarChange(self.change(PickerChangeSource::Clear)),
            window,
            cx,
        );
        if self.commit(PickerChangeSource::Clear, true, window, cx) {
            self.close_and_restore_focus(window, cx);
        }
        self.emit(PickerEvent::Clear, window, cx);
        cx.notify();
    }
    pub(super) fn remove_date(
        &mut self,
        date: NaiveDate,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if !S::MULTIPLE || !self.mounted || self.disabled {
            return;
        }
        let mut slots = S::to_slots(&self.accepted);
        let previous = slots.len();
        slots.retain(|slot| slot.is_none_or(|value| value.date() != date));
        if previous == slots.len() {
            return;
        }
        self.draft = slots;
        self.invalid = [false; 2];
        self.hover = None;
        self.emit(
            PickerEvent::CalendarChange(self.change(PickerChangeSource::Tag)),
            window,
            cx,
        );
        self.commit(PickerChangeSource::Tag, false, window, cx);
    }
    pub(super) fn preset(&mut self, value: S::Value, window: &mut Window, cx: &mut Context<Self>) {
        if self.disabled {
            return;
        }
        let previous = self.draft.clone();
        let mut slots = S::to_slots(&value);
        if S::RANGE {
            for (i, slot) in slots.iter_mut().enumerate() {
                if self.options.disabled_endpoints[i] {
                    *slot = self.draft.get(i).copied().flatten();
                }
            }
        }
        self.draft = slots;
        self.invalid = [false; 2];
        if self.commit(PickerChangeSource::Preset, false, window, cx) {
            self.close_and_restore_focus(window, cx);
        } else {
            self.draft = previous;
        }
        self.sync_editors(window, cx);
        cx.notify();
    }
    pub(super) fn now(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if self.disabled || self.options.disabled_endpoints[self.active] {
            return;
        }
        let mut value = self.clock_value();
        if S::HAS_TIME {
            let Some(time) = self
                .options
                .time
                .align(value.time(), &self.time_constraints(value))
            else {
                return;
            };
            value = value.date().and_time(time);
        }
        if !self.valid_value(value, self.active) {
            return;
        }
        self.assign(value);
        self.emit(
            PickerEvent::CalendarChange(self.change(PickerChangeSource::Now)),
            window,
            cx,
        );
        if S::RANGE {
            self.submit(PickerChangeSource::Now, window, cx);
        } else {
            if self.commit(PickerChangeSource::Now, false, window, cx) {
                self.close_and_restore_focus(window, cx);
            }
        }
        cx.notify();
    }
    fn navigation_target(&self, origin: NaiveDate, amount: i32, large: bool) -> Option<NaiveDate> {
        if self.disabled {
            return None;
        }
        let next = match self.panel_mode {
            PickerPanelMode::Date => amount
                .checked_mul(if large { 12 } else { 1 })
                .and_then(|amount| add_months(origin, amount)),
            PickerPanelMode::Month | PickerPanelMode::Quarter => add_years(origin, amount),
            PickerPanelMode::Year => amount
                .checked_mul(10)
                .and_then(|amount| add_years(origin, amount)),
            PickerPanelMode::Decade => amount
                .checked_mul(100)
                .and_then(|amount| add_years(origin, amount)),
            PickerPanelMode::Time => None,
        };
        let date = next?;
        let mode = match self.panel_mode {
            PickerPanelMode::Date => PickerMode::Month,
            _ => PickerMode::Year,
        };
        let (mut start, mut end) = period_bounds(date, mode, self.format_context.week_rules)?;
        if matches!(
            self.panel_mode,
            PickerPanelMode::Year | PickerPanelMode::Decade
        ) {
            let cells = panel_cells(date, self.panel_mode, self.format_context.week_rules);
            let mut in_view = cells.iter().filter(|cell| cell.in_view);
            if let Some(first) = in_view.next() {
                start = first.period_start;
                end = in_view.next_back().unwrap_or(first).period_end;
            }
        }
        if self.options.min_date.is_some_and(|d| end < d)
            || self.options.max_date.is_some_and(|d| start > d)
            || self
                .options
                .min_date
                .zip(self.options.max_date)
                .is_some_and(|(min, max)| min > max)
        {
            return None;
        }
        Some(date)
    }
    pub(super) fn can_navigate(&self, amount: i32, large: bool) -> bool {
        self.can_navigate_from(self.view_date, amount, large)
    }
    pub(super) fn can_navigate_from(&self, origin: NaiveDate, amount: i32, large: bool) -> bool {
        self.navigation_target(origin, amount, large).is_some()
    }
    pub(super) fn navigate(
        &mut self,
        amount: i32,
        large: bool,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if !self.can_navigate(amount, large) {
            return;
        }
        self.navigate_from(self.view_date, false, (amount, large), window, cx);
    }
    pub(super) fn navigate_from(
        &mut self,
        origin: NaiveDate,
        right_panel: bool,
        navigation: (i32, bool),
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let Some(mut date) = self.navigation_target(origin, navigation.0, navigation.1) else {
            return;
        };
        if right_panel {
            // A range header owns its displayed period. Translate a successful
            // right-panel request back to the shared left period afterwards,
            // preserving the reference's checked month-end clamping order.
            let Some(left) = (match self.panel_mode {
                PickerPanelMode::Date => add_months(date, -1),
                PickerPanelMode::Month | PickerPanelMode::Quarter => add_years(date, -1),
                PickerPanelMode::Year => add_years(date, -10),
                PickerPanelMode::Decade => add_years(date, -100),
                PickerPanelMode::Time => None,
            }) else {
                return;
            };
            date = left;
        }
        if !self.view_controlled {
            self.view_date = date;
            self.cursor = date.and_time(self.cursor.time());
        }
        self.emit(
            PickerEvent::PanelChange {
                date,
                mode: self.panel_mode,
            },
            window,
            cx,
        );
        cx.notify();
    }
    pub(super) fn change_panel(
        &mut self,
        mode: PickerPanelMode,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if !self.disabled && (self.is_open() || self.inline) {
            self.panel_focus.focus(window, cx);
        }
        self.panel_mode = mode;
        self.emit(
            PickerEvent::PanelChange {
                date: self.view_date,
                mode,
            },
            window,
            cx,
        );
        cx.notify();
    }
    pub(super) fn hover_date(
        &mut self,
        date: Option<NaiveDate>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        self.set_hover(date, None, window, cx);
    }
    pub(super) fn hover_cell(
        &mut self,
        panel: usize,
        date: NaiveDate,
        hovered: bool,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if hovered {
            self.set_hover(Some(date), Some(panel), window, cx);
        } else if self
            .hover
            .is_some_and(|hover| hover.panel == Some(panel) && hover.value.date() == date)
        {
            // A different cell may already own the preview when this leave runs.
            self.hover_date(None, window, cx);
        }
    }
    fn set_hover(
        &mut self,
        date: Option<NaiveDate>,
        panel: Option<usize>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        self.hover = date
            .filter(|d| !self.disabled_day(*d))
            .map(|d| PickerHover {
                value: d.and_time(self.cursor.time()),
                panel,
            });
        if self.options.preview_value && !S::MULTIPLE && !self.invalid[self.active] {
            let text = self
                .hover
                .map(|hover| hover.value)
                .or_else(|| self.draft.get(self.active).copied().flatten())
                .map(|v| self.format_value(v))
                .unwrap_or_default();
            self.editors[self.active].update(cx, |e, cx| e.set_value(text.as_ref(), window, cx));
        }
        cx.notify();
    }
    pub(super) fn panel_key(
        &mut self,
        key: &str,
        shift: bool,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> bool {
        if self.disabled {
            return false;
        }
        if key == "escape" {
            self.close_and_restore_focus(window, cx);
            return true;
        }
        if key == "enter" || key == "space" {
            if self.panel_mode == PickerPanelMode::Time {
                self.submit(PickerChangeSource::Keyboard, window, cx);
            } else {
                self.select_date(self.cursor.date(), PickerChangeSource::Keyboard, window, cx);
            }
            return true;
        }
        if self.panel_mode == PickerPanelMode::Time {
            return false;
        }
        let offset = |amount: i32| match self.panel_mode {
            PickerPanelMode::Date => self
                .cursor
                .date()
                .checked_add_signed(Duration::days(i64::from(amount))),
            PickerPanelMode::Month => add_months(self.cursor.date(), amount),
            PickerPanelMode::Quarter => amount
                .checked_mul(3)
                .and_then(|amount| add_months(self.cursor.date(), amount)),
            PickerPanelMode::Year => add_years(self.cursor.date(), amount),
            PickerPanelMode::Decade => amount
                .checked_mul(10)
                .and_then(|amount| add_years(self.cursor.date(), amount)),
            PickerPanelMode::Time => None,
        };
        let row = match self.panel_mode {
            PickerPanelMode::Date => 7,
            PickerPanelMode::Quarter => 4,
            _ => 3,
        };
        let date = match key {
            "left" => offset(-1),
            "right" => offset(1),
            "up" => offset(-row),
            "down" => offset(row),
            "pageup" | "pagedown" => {
                let sign = if key == "pageup" { -1 } else { 1 };
                match self.panel_mode {
                    PickerPanelMode::Date => {
                        add_months(self.cursor.date(), sign * if shift { 12 } else { 1 })
                    }
                    PickerPanelMode::Month | PickerPanelMode::Quarter => {
                        add_years(self.cursor.date(), sign * if shift { 10 } else { 1 })
                    }
                    PickerPanelMode::Year => add_years(self.cursor.date(), sign * 10),
                    PickerPanelMode::Decade => add_years(self.cursor.date(), sign * 100),
                    PickerPanelMode::Time => None,
                }
            }
            "home" | "end" if self.panel_mode == PickerPanelMode::Date => {
                start_of_week(self.cursor.date(), self.format_context.week_rules).and_then(|date| {
                    date.checked_add_signed(Duration::days(if key == "end" { 6 } else { 0 }))
                })
            }
            "home" | "end" => {
                let cells = panel_cells(
                    self.cursor.date(),
                    self.panel_mode,
                    self.format_context.week_rules,
                );
                let mut cells = cells.iter().filter(|cell| cell.in_view);
                if key == "home" {
                    cells.next()
                } else {
                    cells.next_back()
                }
                .map(|cell| cell.date)
            }
            _ => return false,
        };
        if let Some(date) = date.filter(|date| {
            let cell = cell_for_date(*date, self.panel_mode, self.format_context.week_rules);
            !self
                .options
                .min_date
                .is_some_and(|min| cell.period_end < min)
                && !self
                    .options
                    .max_date
                    .is_some_and(|max| cell.period_start > max)
        }) {
            let visible = self.cursor_in_view(date);
            if self.view_controlled && !visible {
                self.emit(
                    PickerEvent::PanelChange {
                        date,
                        mode: self.panel_mode,
                    },
                    window,
                    cx,
                );
                return true;
            }
            self.cursor = date.and_time(self.cursor.time());
            if !self.view_controlled && !visible {
                self.view_date = date;
                self.emit(
                    PickerEvent::PanelChange {
                        date,
                        mode: self.panel_mode,
                    },
                    window,
                    cx,
                );
            }
            cx.notify();
        }
        true
    }
    pub(super) fn schedule_scroll(
        &mut self,
        column: usize,
        unit: TimeUnit,
        row_height: f32,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if column >= 5
            || !self.mounted
            || !(self.is_open() || self.inline)
            || self.disabled
            || !window.is_window_active()
            || !window.is_visible()
            || !self.options.time.change_on_scroll
            || !row_height.is_finite()
            || row_height <= 0.
        {
            return;
        }
        self.scroll_generation[column] = self.scroll_generation[column].wrapping_add(1);
        let generation = self.scroll_generation[column];
        let session = self.generation;
        let active = self.active;
        let time_options = self.options.time.clone();
        let timer = cx
            .background_executor()
            .timer(StdDuration::from_millis(300));
        self.scroll_tasks[column] = Some(cx.spawn_in(window, async move |state, cx| {
            timer.await;
            let _ = state.update_in(cx, |state, window, cx| {
                if state.scroll_generation[column] == generation {
                    state.scroll_tasks[column] = None;
                }
                if !state.mounted
                    || !(state.is_open() || state.inline)
                    || state.disabled
                    || !window.is_window_active()
                    || !window.is_visible()
                    || state.generation != session
                    || state.scroll_generation[column] != generation
                    || state.active != active
                    || state.options.time != time_options
                    || state.scope.as_ref().is_some_and(|scope| {
                        scope
                            .snapshot(cx)
                            .theme
                            .component::<super::DatePickerTheme>()
                            .time_cell_height as f32
                            != row_height
                    })
                {
                    return;
                }
                let offset = f32::from(state.scrolls[column].0.borrow().base_handle.offset().y);
                let index = (-offset / row_height).round().max(0.) as usize;
                let time = state
                    .draft
                    .get(state.active)
                    .copied()
                    .flatten()
                    .unwrap_or(state.cursor);
                let (values, disabled) = state.visible_time_values(unit, time);
                if let Some((_, value)) = values
                    .iter()
                    .enumerate()
                    .filter(|(_, value)| !disabled.disabled(**value))
                    .min_by_key(|(candidate, _)| candidate.abs_diff(index))
                {
                    state.select_time(unit, *value, PickerChangeSource::Time, window, cx);
                }
            });
        }));
    }
    pub(super) fn cancel_scroll_tasks(&mut self) {
        for generation in &mut self.scroll_generation {
            *generation = generation.wrapping_add(1);
        }
        for task in &mut self.scroll_tasks {
            *task = None;
        }
    }
    #[cfg(test)]
    pub(super) fn pending_scroll_tasks(&self) -> usize {
        self.scroll_tasks
            .iter()
            .filter(|task| task.is_some())
            .count()
    }
    pub(super) fn mask_key(
        &mut self,
        index: usize,
        key: &str,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> bool {
        if self.disabled
            || self.options.input_read_only
            || self.options.disabled_endpoints[index]
            || self.editors[index].read(cx).is_composing()
        {
            return false;
        }
        let Some(mask) = self.format.mask_format().cloned() else {
            return false;
        };
        if key == "paste" {
            return self.mask_paste(index, window, cx);
        }
        if key == "cut" {
            return self.mask_cut(index, window, cx);
        }
        let text = self.editors[index].read(cx).value_snapshot();
        let segment = self.mask_segment[index].min(mask.segments().len() - 1);
        let changed = match key {
            "space" => return true,
            "left" => {
                self.mask_segment[index] = segment.saturating_sub(1);
                None
            }
            "right" => {
                self.mask_segment[index] = (segment + 1).min(mask.segments().len() - 1);
                None
            }
            "up" => mask.step(&text, segment, 1),
            "down" => mask.step(&text, segment, -1),
            "backspace" | "delete" => mask.delete(&text, segment),
            _ => {
                let mut chars = key.chars();
                let Some(digit) = chars.next() else {
                    return false;
                };
                if chars.next().is_some() {
                    return false;
                }
                if !digit.is_ascii_digit() {
                    return true;
                }
                let Some(edit) = mask.input_digit(&text, segment, digit, &self.mask_pending[index])
                else {
                    return false;
                };
                self.mask_segment[index] = edit.segment;
                self.mask_pending[index] = edit.pending_digits;
                self.apply_mask_text(index, &edit.text, window, cx);
                self.select_mask_segment(index, window, cx);
                return true;
            }
        };
        self.mask_pending[index].clear();
        if let Some(text) = changed {
            self.apply_mask_text(index, &text, window, cx);
        }
        self.select_mask_segment(index, window, cx);
        true
    }
    fn apply_mask_text(
        &mut self,
        index: usize,
        text: &str,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        self.editors[index].update(cx, |e, c| {
            e.replace_range(
                Some(0..e.value().len()),
                text,
                EditOrigin::Typing,
                window,
                c,
            );
        });
        self.text_change(index, text, window, cx);
    }
    pub(super) fn mask_paste(
        &mut self,
        index: usize,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> bool {
        let Some(mask) = self.format.mask_format().cloned() else {
            return false;
        };
        if self.disabled || self.options.input_read_only || self.options.disabled_endpoints[index] {
            return true;
        }
        if let Some(text) = cx.read_from_clipboard().and_then(|item| item.text())
            && let Some(value) = self
                .parse_value(&text)
                .filter(|value| self.valid_value(*value, index))
        {
            let text = self.format_value(value);
            if mask.matches(&text) {
                self.editors[index].update(cx, |e, c| {
                    e.replace_range(
                        Some(0..e.value().len()),
                        text.as_ref(),
                        EditOrigin::Paste,
                        window,
                        c,
                    )
                });
                self.text_change(index, &text, window, cx);
                self.mask_pending[index].clear();
                self.select_mask_segment(index, window, cx);
            }
        }
        true
    }
    pub(super) fn mask_cut(
        &mut self,
        index: usize,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> bool {
        let Some(mask) = self.format.mask_format().cloned() else {
            return false;
        };
        if self.disabled || self.options.input_read_only || self.options.disabled_endpoints[index] {
            return true;
        }
        let editor = self.editors[index].read(cx);
        let range = editor.selection().range();
        let text = editor.value_snapshot();
        if range.is_empty() {
            return true;
        }
        if let Some(selected) = text.get(range.clone()) {
            cx.write_to_clipboard(gpui::ClipboardItem::new_string(selected.to_owned()));
            let mut next = text.to_string();
            for (segment, field) in mask.segments().iter().enumerate() {
                if field.range.start < range.end && field.range.end > range.start {
                    next = mask.delete(&next, segment).unwrap_or(next);
                }
            }
            self.mask_segment[index] = mask.segment_at(range.start);
            self.mask_pending[index].clear();
            self.editors[index].update(cx, |e, c| {
                e.replace_range(Some(0..e.value().len()), &next, EditOrigin::Cut, window, c)
            });
            self.text_change(index, &next, window, cx);
            self.select_mask_segment(index, window, cx);
        }
        true
    }
    pub(super) fn mask_select_at(
        &mut self,
        index: usize,
        byte_offset: usize,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if let Some(mask) = self.format.mask_format() {
            self.mask_segment[index] = mask.segment_at(byte_offset);
            self.mask_pending[index].clear();
            self.select_mask_segment(index, window, cx);
        }
    }
    fn select_mask_segment(&self, index: usize, window: &mut Window, cx: &mut Context<Self>) {
        if let Some(range) = self
            .format
            .mask_format()
            .and_then(|mask| mask.selection(self.mask_segment[index]))
        {
            self.editors[index].update(cx, |e, c| {
                e.select(
                    TextSelection {
                        anchor: range.start,
                        caret: range.end,
                    },
                    window,
                    c,
                )
            });
        }
    }
    fn text_change(
        &mut self,
        index: usize,
        text: &str,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let previous = self.draft.clone();
        self.active = index;
        self.hover = None;
        self.touched[index] = true;
        self.confirmed[index] = false;
        if text.is_empty() {
            if !S::MULTIPLE {
                self.draft[index] = None;
            }
            self.invalid[index] = false;
        } else if let Some(value) = self.parse_value(text) {
            self.invalid[index] = !self.valid_value(value, index);
            if !self.invalid[index] && !S::MULTIPLE {
                self.draft[index] = Some(Self::normalized(value));
                self.cursor = Self::normalized(value);
                if !self.view_controlled {
                    self.view_date = value.date();
                }
            }
        } else {
            self.invalid[index] = true;
        }
        if previous != self.draft {
            self.emit(
                PickerEvent::CalendarChange(self.change(PickerChangeSource::Input)),
                window,
                cx,
            );
        }
        cx.notify();
    }
    fn editor_event(
        &mut self,
        index: usize,
        event: &InputEvent,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.disabled || self.options.disabled_endpoints[index] {
            return;
        }
        match event {
            InputEvent::Focus => {
                if self.active != index
                    && self.need_confirm()
                    && self.touched[self.active]
                    && !self.confirmed[self.active]
                    && !self.options.allow_empty[self.active]
                {
                    let focus = self.editors[self.active].read(cx).focus_handle(cx);
                    window.defer(cx, move |w, c| focus.focus(w, c));
                } else {
                    self.active = index;
                    if let Some(value) = self.draft.get(index).copied().flatten() {
                        self.cursor = Self::normalized(value);
                        if !self.view_controlled {
                            self.view_date = value.date();
                        }
                    }
                    if self.format.mask_format().is_some() {
                        if self.editors[index].read(cx).value().is_empty() {
                            let template = self.format.mask_format().unwrap().template().to_owned();
                            self.editors[index].update(cx, |e, c| e.set_value(template, window, c));
                        }
                        self.mask_segment[index] = 0;
                        self.mask_pending[index].clear();
                        self.select_mask_segment(index, window, cx);
                    }
                    cx.notify();
                }
            }
            InputEvent::Change(change) if !change.is_composing => {
                if !self.options.input_read_only
                    && !self.format.is_read_only()
                    && self.format_callback.is_none()
                {
                    if matches!(change.origin, EditOrigin::Undo | EditOrigin::Redo)
                        && let Some(mask) = self.format.mask_format()
                    {
                        self.mask_segment[index] =
                            mask.segment_at(self.editors[index].read(cx).selection().caret);
                        self.mask_pending[index].clear();
                    }
                    self.text_change(index, &change.value, window, cx);
                }
            }
            InputEvent::CompositionChanged(false)
                if !self.options.input_read_only
                    && !self.format.is_read_only()
                    && self.format_callback.is_none() =>
            {
                // IME commit can keep the exact preedit bytes, so InputState
                // correctly emits only this composition boundary, not Change.
                let text = self.editors[index].read(cx).value_snapshot();
                self.text_change(index, &text, window, cx);
            }
            InputEvent::Commit(commit) if commit.reason == CommitReason::Enter => {
                if self.is_open() || (self.touched[index] && !self.invalid[index]) {
                    if S::RANGE && !self.is_open() {
                        self.request_open(true, false, window, cx);
                    }
                    self.submit(PickerChangeSource::Input, window, cx);
                } else {
                    self.request_open(true, false, window, cx);
                }
            }
            InputEvent::Blur => {
                let state = cx.weak_entity();
                let generation = self.generation;
                window.defer(cx, move |w, c| {
                    let _ = state.update(c, |s, c| {
                        if !s.mounted || s.generation != generation {
                            return;
                        }
                        if let Some(mask) = s.format.mask_format() {
                            let text = s.editors[index].read(c).value_snapshot();
                            if let Some(repaired) = mask
                                .repair(&text)
                                .filter(|repaired| repaired != text.as_ref())
                            {
                                s.apply_mask_text(index, &repaired, w, c);
                            }
                        }
                        if !s.owns_focus(w, c) && s.is_open() {
                            s.request_open(false, false, w, c);
                        } else if !s.owns_focus(w, c)
                            && s.invalid[index]
                            && !s.options.preserve_invalid_on_blur
                        {
                            s.reset_draft(w, c);
                            c.notify();
                        } else if !s.owns_focus(w, c) && !s.need_confirm() && s.touched[index] {
                            s.commit(PickerChangeSource::Input, false, w, c);
                        }
                    });
                });
            }
            _ => {}
        }
    }
}
