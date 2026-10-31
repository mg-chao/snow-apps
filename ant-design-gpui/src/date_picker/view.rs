// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::{
    DatePickerTheme, DatePickerTokens, DisabledDateInfo, DisabledTime, PickerChange,
    PickerChangeSource, PickerEndpoint, PickerEvent, PickerFormat, PickerLanguage, PickerMode,
    PickerOptions, PickerPanelMode, PickerSelection, PickerState, PickerStyleContext, PickerStyles,
    TimeOptions, TimeUnit,
    calendar::{self, CalendarCell, WeekRules},
    control::PickerAction,
    state::{
        ActionHandler, ChangeHandler, DisabledDate, DisabledTimeFactory, EventHandler,
        FormatCallback, OpenHandler, PanelHandler, PickerCellGeometry,
    },
    time_list::TimeList,
};
use crate::{
    ConfigScope, Direction, Input, Size, ValidationStatus, Variant,
    behavior::Accessibility,
    icons::{IconName, icon},
    input::InputInteraction,
    locale::LocaleId,
    popup::{Popup, PopupArrow, PopupOpenReason, PopupPlacement, PopupTriggers},
    scroll_area::{ExternalScrollbar, ExternalScrollbarGeometry},
};
use chrono::{Datelike, NaiveDate, NaiveDateTime, Weekday};
use gpui::{
    AnyElement, App, Bounds, Context, ElementId, Entity, Focusable, IntoElement, MouseButton,
    Pixels, Refineable, RenderOnce, Role, ScrollStrategy, SharedString, StyleRefinement, Styled,
    Subscription, WeakEntity, Window, accesskit, canvas, div, point, prelude::*, px, relative,
    uniform_list,
};
use std::{cell::Cell, rc::Rc, sync::Arc};

type Content = Rc<dyn Fn(&mut Window, &mut App) -> AnyElement>;
type CellRender = Rc<dyn Fn(&PickerCellContext, AnyElement, &mut Window, &mut App) -> AnyElement>;
type PanelRender<S> = Rc<dyn Fn(&PickerPanelParts<S>, &mut Window, &mut App) -> AnyElement>;
type StylesRender = Rc<dyn Fn(&PickerStyleContext, &mut Window, &mut App) -> PickerStyles>;
type InputRender = Rc<dyn Fn(usize, Input, &mut Window, &mut App) -> AnyElement>;
type TagRender = Rc<dyn Fn(&PickerTagContext, AnyElement, &mut Window, &mut App) -> AnyElement>;

#[derive(Clone, Default)]
struct NavigationIcons {
    previous: Option<Content>,
    next: Option<Content>,
    super_previous: Option<Content>,
    super_next: Option<Content>,
}

#[cfg(test)]
#[path = "view_api_tests.rs"]
mod api_tests;

/// Stable date identity and a weak removal command for a multiple-selection tag.
#[derive(Clone)]
pub struct PickerTagContext {
    pub value: NaiveDate,
    pub disabled: bool,
    remove: ActionHandler,
}
impl PickerTagContext {
    pub fn remove(&self, w: &mut Window, c: &mut App) {
        if !self.disabled {
            (self.remove)(w, c);
        }
    }
}

pub struct PickerPreset<S: PickerSelection> {
    pub key: SharedString,
    pub label: SharedString,
    pub value: S::Value,
    factory: Option<Rc<dyn Fn() -> S::Value>>,
}
impl<S: PickerSelection> Clone for PickerPreset<S> {
    fn clone(&self) -> Self {
        Self {
            key: self.key.clone(),
            label: self.label.clone(),
            value: self.value.clone(),
            factory: self.factory.clone(),
        }
    }
}
impl<S: PickerSelection> PickerPreset<S> {
    pub fn new(
        key: impl Into<SharedString>,
        label: impl Into<SharedString>,
        value: S::Value,
    ) -> Self {
        Self {
            key: key.into(),
            label: label.into(),
            value,
            factory: None,
        }
    }
    pub fn dynamic(mut self, f: impl Fn() -> S::Value + 'static) -> Self {
        self.factory = Some(Rc::new(f));
        self
    }
}

/// Content customization keeps the library's cell hit target and semantics.
#[derive(Clone, Debug)]
pub struct PickerCellContext {
    pub date: Option<NaiveDate>,
    pub unit: Option<TimeUnit>,
    pub value: Option<u32>,
    pub mode: PickerPanelMode,
    pub endpoint: PickerEndpoint,
    pub today: NaiveDate,
    pub selected: bool,
    pub disabled: bool,
    pub in_range: bool,
    pub hovered: bool,
}

#[derive(IntoElement)]
pub struct Picker<S: PickerSelection> {
    id: ElementId,
    state: Entity<PickerState<S>>,
    scope: Option<ConfigScope>,
    value: Option<S::Value>,
    default_value: Option<S::Value>,
    open: Option<bool>,
    default_open: bool,
    options: PickerOptions,
    disabled: Option<bool>,
    size: Option<Size>,
    variant: Option<Variant>,
    status: ValidationStatus,
    format: PickerFormat,
    format_callback: Option<FormatCallback>,
    allow_clear: Option<bool>,
    read_only: Option<bool>,
    preview: Option<bool>,
    picker_value: Option<NaiveDate>,
    default_picker_value: Option<NaiveDate>,
    panel_mode: Option<PickerPanelMode>,
    disabled_date: Option<DisabledDate>,
    disabled_time: Option<DisabledTimeFactory>,
    clock: Option<Rc<dyn Fn() -> NaiveDateTime>>,
    placement: Option<PopupPlacement>,
    arrow: Option<PopupArrow>,
    placeholder: Option<SharedString>,
    range_placeholders: Option<[SharedString; 2]>,
    label: Option<SharedString>,
    automation_id: Option<SharedString>,
    description: Option<SharedString>,
    prefix: Option<AnyElement>,
    suffix: Option<AnyElement>,
    presets: Arc<[PickerPreset<S>]>,
    footer: Option<Content>,
    navigation_icons: NavigationIcons,
    cell_render: Option<CellRender>,
    panel_render: Option<PanelRender<S>>,
    input_render: Option<InputRender>,
    tag_render: Option<TagRender>,
    styles: PickerStyles,
    styles_fn: Option<StylesRender>,
    on_change: Option<ChangeHandler<S::Value>>,
    on_calendar: Option<ChangeHandler<S::Value>>,
    on_open: Option<OpenHandler>,
    on_panel: Option<PanelHandler>,
    on_clear: Option<ActionHandler>,
    on_ok: Option<ActionHandler>,
    on_event: Option<EventHandler<S::Value>>,
}
impl<S: PickerSelection> Picker<S> {
    pub fn new(id: impl Into<ElementId>, state: &Entity<PickerState<S>>) -> Self {
        Self {
            id: id.into(),
            state: state.clone(),
            scope: None,
            value: None,
            default_value: None,
            open: None,
            default_open: false,
            options: PickerOptions::default(),
            disabled: None,
            size: None,
            variant: None,
            status: ValidationStatus::None,
            format: PickerFormat::default(),
            format_callback: None,
            allow_clear: None,
            read_only: None,
            preview: None,
            picker_value: None,
            default_picker_value: None,
            panel_mode: None,
            disabled_date: None,
            disabled_time: None,
            clock: None,
            placement: None,
            arrow: None,
            placeholder: None,
            range_placeholders: None,
            label: None,
            automation_id: None,
            description: None,
            prefix: None,
            suffix: None,
            presets: Arc::from([]),
            footer: None,
            navigation_icons: NavigationIcons::default(),
            cell_render: None,
            panel_render: None,
            input_render: None,
            tag_render: None,
            styles: PickerStyles::default(),
            styles_fn: None,
            on_change: None,
            on_calendar: None,
            on_open: None,
            on_panel: None,
            on_clear: None,
            on_ok: None,
            on_event: None,
        }
    }
    pub fn scope(mut self, v: ConfigScope) -> Self {
        self.scope = Some(v);
        self
    }
    pub fn value(mut self, v: S::Value) -> Self {
        self.value = Some(v);
        self
    }
    pub fn default_value(mut self, v: S::Value) -> Self {
        self.default_value = Some(v);
        self
    }
    pub fn open(mut self, v: bool) -> Self {
        self.open = Some(v);
        self
    }
    pub fn default_open(mut self, v: bool) -> Self {
        self.default_open = v;
        self
    }
    pub fn options(mut self, v: PickerOptions) -> Self {
        self.options = v;
        self
    }
    pub fn picker_mode(mut self, v: PickerMode) -> Self
    where
        S: super::DateGranularitySelection,
    {
        self.options.mode = v;
        self
    }
    pub fn picker_value(mut self, v: NaiveDate) -> Self {
        self.picker_value = Some(v);
        self
    }
    pub fn default_picker_value(mut self, v: NaiveDate) -> Self {
        self.default_picker_value = Some(v);
        self
    }
    pub fn panel_mode(mut self, v: PickerPanelMode) -> Self {
        self.panel_mode = Some(v);
        self
    }
    pub fn disabled(mut self, v: bool) -> Self {
        self.disabled = Some(v);
        self
    }
    pub fn disabled_endpoints(mut self, v: [bool; 2]) -> Self {
        self.options.disabled_endpoints = v;
        self
    }
    pub fn allow_empty(mut self, v: [bool; 2]) -> Self {
        self.options.allow_empty = v;
        self
    }
    pub fn need_confirm(mut self, v: bool) -> Self {
        self.options.need_confirm = Some(v);
        self
    }
    pub fn order(mut self, v: bool) -> Self {
        self.options.order = v;
        self
    }
    pub fn input_read_only(mut self, v: bool) -> Self {
        self.read_only = Some(v);
        self
    }
    pub fn preserve_invalid_on_blur(mut self, v: bool) -> Self {
        self.options.preserve_invalid_on_blur = v;
        self
    }
    pub fn preview_value(mut self, v: bool) -> Self {
        self.preview = Some(v);
        self
    }
    pub fn show_now(mut self, v: bool) -> Self {
        self.options.show_now = Some(v);
        self
    }
    pub fn show_week(mut self, v: bool) -> Self {
        self.options.show_week = v;
        self
    }
    pub fn min_date(mut self, v: NaiveDate) -> Self {
        self.options.min_date = Some(v);
        self
    }
    pub fn max_date(mut self, v: NaiveDate) -> Self {
        self.options.max_date = Some(v);
        self
    }
    pub fn time_options(mut self, v: TimeOptions) -> Self
    where
        S: super::TimePickerSelection,
    {
        self.options.time = v;
        self
    }
    pub fn format(mut self, v: PickerFormat) -> Self {
        self.format = v;
        self
    }
    pub fn format_render(mut self, f: impl Fn(NaiveDateTime) -> SharedString + 'static) -> Self {
        self.format_callback = Some(Rc::new(f));
        self
    }
    pub fn allow_clear(mut self, v: bool) -> Self {
        self.allow_clear = Some(v);
        self
    }
    pub fn size(mut self, v: Size) -> Self {
        self.size = Some(v);
        self
    }
    pub fn variant(mut self, v: Variant) -> Self {
        self.variant = Some(v);
        self
    }
    pub fn status(mut self, v: ValidationStatus) -> Self {
        self.status = v;
        self
    }
    pub fn placement(mut self, v: PopupPlacement) -> Self {
        self.placement = Some(v);
        self
    }
    /// Configure the popup arrow. Ranges point at the active endpoint.
    pub fn arrow(mut self, arrow: impl Into<PopupArrow>) -> Self {
        self.arrow = Some(arrow.into());
        self
    }
    pub fn placeholder(mut self, v: impl Into<SharedString>) -> Self {
        self.placeholder = Some(v.into());
        self
    }
    pub fn range_placeholders(mut self, v: [SharedString; 2]) -> Self {
        self.range_placeholders = Some(v);
        self
    }
    pub fn accessibility_label(mut self, v: impl Into<SharedString>) -> Self {
        self.label = Some(v.into());
        self
    }
    pub fn accessibility_description(mut self, v: impl Into<SharedString>) -> Self {
        self.description = Some(v.into());
        self
    }
    pub fn automation_id(mut self, v: impl Into<SharedString>) -> Self {
        self.automation_id = Some(v.into());
        self
    }
    pub fn prefix(mut self, v: impl IntoElement) -> Self {
        self.prefix = Some(v.into_any_element());
        self
    }
    pub fn suffix_icon(mut self, v: impl IntoElement) -> Self {
        self.suffix = Some(v.into_any_element());
        self
    }
    /// Rebuild the previous-period icon without changing its native action.
    pub fn prev_icon<E: IntoElement>(
        mut self,
        f: impl Fn(&mut Window, &mut App) -> E + 'static,
    ) -> Self {
        self.navigation_icons.previous = Some(Rc::new(move |w, c| f(w, c).into_any_element()));
        self
    }
    pub fn next_icon<E: IntoElement>(
        mut self,
        f: impl Fn(&mut Window, &mut App) -> E + 'static,
    ) -> Self {
        self.navigation_icons.next = Some(Rc::new(move |w, c| f(w, c).into_any_element()));
        self
    }
    pub fn super_prev_icon<E: IntoElement>(
        mut self,
        f: impl Fn(&mut Window, &mut App) -> E + 'static,
    ) -> Self {
        self.navigation_icons.super_previous =
            Some(Rc::new(move |w, c| f(w, c).into_any_element()));
        self
    }
    pub fn super_next_icon<E: IntoElement>(
        mut self,
        f: impl Fn(&mut Window, &mut App) -> E + 'static,
    ) -> Self {
        self.navigation_icons.super_next = Some(Rc::new(move |w, c| f(w, c).into_any_element()));
        self
    }
    pub fn presets(mut self, v: impl IntoIterator<Item = PickerPreset<S>>) -> Self {
        self.presets = v.into_iter().collect();
        self
    }
    pub fn extra_footer<E: IntoElement>(
        mut self,
        f: impl Fn(&mut Window, &mut App) -> E + 'static,
    ) -> Self {
        self.footer = Some(Rc::new(move |w, c| f(w, c).into_any_element()));
        self
    }
    pub fn cell_render<E: IntoElement>(
        mut self,
        f: impl Fn(&PickerCellContext, AnyElement, &mut Window, &mut App) -> E + 'static,
    ) -> Self {
        self.cell_render = Some(Rc::new(move |ctx, e, w, c| {
            f(ctx, e, w, c).into_any_element()
        }));
        self
    }
    pub fn panel_render<E: IntoElement>(
        mut self,
        f: impl Fn(&PickerPanelParts<S>, &mut Window, &mut App) -> E + 'static,
    ) -> Self {
        self.panel_render = Some(Rc::new(move |parts, w, c| {
            f(parts, w, c).into_any_element()
        }));
        self
    }
    pub fn input_render<E: IntoElement>(
        mut self,
        f: impl Fn(usize, Input, &mut Window, &mut App) -> E + 'static,
    ) -> Self {
        self.input_render = Some(Rc::new(move |index, input, w, c| {
            f(index, input, w, c).into_any_element()
        }));
        self
    }
    pub fn tag_render<E: IntoElement>(
        mut self,
        f: impl Fn(&PickerTagContext, AnyElement, &mut Window, &mut App) -> E + 'static,
    ) -> Self {
        self.tag_render = Some(Rc::new(move |ctx, tag, w, c| {
            f(ctx, tag, w, c).into_any_element()
        }));
        self
    }
    pub fn styles(mut self, v: PickerStyles) -> Self {
        self.styles = v;
        self
    }
    pub fn styles_fn(
        mut self,
        f: impl Fn(&PickerStyleContext, &mut Window, &mut App) -> PickerStyles + 'static,
    ) -> Self {
        self.styles_fn = Some(Rc::new(f));
        self
    }
    pub fn disabled_date(
        mut self,
        f: impl Fn(NaiveDate, DisabledDateInfo) -> bool + 'static,
    ) -> Self {
        self.disabled_date = Some(Rc::new(f));
        self
    }
    pub fn disabled_time(
        mut self,
        f: impl Fn(NaiveDateTime, PickerEndpoint) -> DisabledTime + 'static,
    ) -> Self {
        self.disabled_time = Some(Rc::new(f));
        self
    }
    pub fn clock(mut self, f: impl Fn() -> NaiveDateTime + 'static) -> Self {
        self.clock = Some(Rc::new(f));
        self
    }
    pub fn on_change(
        mut self,
        f: impl Fn(&PickerChange<S::Value>, &mut Window, &mut App) + 'static,
    ) -> Self {
        self.on_change = Some(Rc::new(f));
        self
    }
    pub fn on_calendar_change(
        mut self,
        f: impl Fn(&PickerChange<S::Value>, &mut Window, &mut App) + 'static,
    ) -> Self {
        self.on_calendar = Some(Rc::new(f));
        self
    }
    pub fn on_open_change(mut self, f: impl Fn(bool, &mut Window, &mut App) + 'static) -> Self {
        self.on_open = Some(Rc::new(f));
        self
    }
    pub fn on_panel_change(
        mut self,
        f: impl Fn(NaiveDate, PickerPanelMode, &mut Window, &mut App) + 'static,
    ) -> Self {
        self.on_panel = Some(Rc::new(f));
        self
    }
    pub fn on_clear(mut self, f: impl Fn(&mut Window, &mut App) + 'static) -> Self {
        self.on_clear = Some(Rc::new(f));
        self
    }
    pub fn on_ok(mut self, f: impl Fn(&mut Window, &mut App) + 'static) -> Self {
        self.on_ok = Some(Rc::new(f));
        self
    }
    pub fn on_event(
        mut self,
        f: impl Fn(&PickerEvent<S::Value>, &mut Window, &mut App) + 'static,
    ) -> Self {
        self.on_event = Some(Rc::new(f));
        self
    }
    /// Embed the configured editor without an input trigger or popup.
    pub fn into_panel(self) -> PickerPanel<S> {
        PickerPanel { picker: self }
    }

    fn configure(&self, inline: bool, window: &mut Window, cx: &mut App) -> PickerPanelParts<S> {
        let scope = self
            .scope
            .clone()
            .unwrap_or_else(|| ConfigScope::application(cx));
        let config = scope.snapshot(cx);
        let provider = if S::RANGE {
            &config.range_picker
        } else if !S::HAS_DATE {
            &config.time_picker
        } else {
            &config.date_picker
        };
        let mut options = self.options.clone();
        if S::HAS_TIME {
            options.mode = PickerMode::Date;
        }
        options.input_read_only = self
            .read_only
            .unwrap_or(options.input_read_only || provider.input_read_only)
            || S::MULTIPLE
            || self.format_callback.is_some()
            || self.format.is_read_only();
        options.preview_value = self
            .preview
            .unwrap_or(options.preview_value && provider.preview_value);
        options.need_confirm = options.need_confirm.or(provider.need_confirm);
        options.show_now = options.show_now.or(provider.show_now);
        if let Some(fields) = self.format.time_fields() {
            options.time.show_hour = fields.hour;
            options.time.show_minute = fields.minute;
            options.time.show_second = fields.second;
            options.time.show_millisecond = fields.millisecond;
            options.time.use_12_hours = options.time.use_12_hours || fields.use_12_hours;
        }
        self.state.update(cx, |s, cx| {
            s.scope = Some(scope.clone());
            s.inline = inline;
            s.mounted = true;
            if self.prefix.is_none() {
                s.prefix_width = px(0.);
            }
            s.disabled = self.disabled.unwrap_or(config.disabled)
                || (S::RANGE && options.disabled_endpoints.iter().all(|v| *v));
            s.controlled = self.value.is_some();
            s.open_controlled = self.open.is_some();
            s.on_change = self.on_change.clone();
            s.on_calendar = self.on_calendar.clone();
            s.on_open = self.on_open.clone();
            s.on_panel = self.on_panel.clone();
            s.on_clear = self.on_clear.clone();
            s.on_ok = self.on_ok.clone();
            s.on_event = self.on_event.clone();
            s.disabled_date = self.disabled_date.clone();
            s.disabled_time = self.disabled_time.clone();
            if let Some(clock) = &self.clock {
                s.clock = clock.clone();
            }
            let old_mode = s.options.mode;
            if s.options.time != options.time || old_mode != options.mode || s.format != self.format
            {
                s.cancel_scroll_tasks();
                s.scroll_keys = [None; 5];
            }
            s.options = options;
            let locale = &config.locale.picker;
            let previous_context = s.format_context;
            let changed_locale = s.locale != *locale;
            if changed_locale {
                s.locale = locale.clone();
            }
            s.format_context = super::PickerFormatContext {
                mode: s.options.mode,
                has_date: S::HAS_DATE,
                has_time: S::HAS_TIME,
                use_12_hours: s.options.time.use_12_hours,
                week_rules: WeekRules {
                    first_weekday: match locale.first_weekday {
                        0 => Weekday::Mon,
                        1 => Weekday::Tue,
                        2 => Weekday::Wed,
                        3 => Weekday::Thu,
                        4 => Weekday::Fri,
                        5 => Weekday::Sat,
                        _ => Weekday::Sun,
                    },
                    min_days_in_first_week: locale.min_week_days as u8,
                },
                language: match config.locale.id {
                    LocaleId::ZhCn => PickerLanguage::ZhCn,
                    LocaleId::ZhTw => PickerLanguage::ZhTw,
                    _ => PickerLanguage::EnUs,
                },
            };
            let changed_format = s.format != self.format;
            let changed_format_callback = match (&s.format_callback, &self.format_callback) {
                (Some(previous), Some(current)) => !Rc::ptr_eq(previous, current),
                (None, None) => false,
                _ => true,
            };
            if previous_context != s.format_context {
                s.cancel_scroll_tasks();
            }
            s.format = self.format.clone();
            s.format_callback = self.format_callback.clone();
            let mut sync = changed_format
                || changed_format_callback
                || changed_locale
                || previous_context != s.format_context;
            if let Some(value) = &self.value {
                if &s.accepted != value {
                    s.replace_controlled_value(value.clone(), window, cx);
                    sync = true;
                }
            } else if !s.initialized
                && let Some(value) = &self.default_value
            {
                s.accepted = value.clone();
                s.draft = S::to_slots(value);
                sync = true;
            }
            if !s.initialized {
                s.cursor = s
                    .draft
                    .iter()
                    .flatten()
                    .copied()
                    .next()
                    .unwrap_or_else(|| s.clock_value());
                s.view_date = s.cursor.date();
            }
            s.initialized = true;
            s.view_controlled = self.picker_value.is_some();
            s.default_picker_value = self.default_picker_value;
            if let Some(value) = self.picker_value {
                s.view_date = value;
            } else if !s.open_initialized
                && let Some(value) = self.default_picker_value
            {
                s.view_date = value;
            }
            if let Some(mode) = self.panel_mode {
                s.panel_mode = mode;
            } else if old_mode != s.options.mode || !s.open_initialized {
                s.panel_mode = s.target_panel();
            }
            let open = self.open.unwrap_or(if s.open_initialized {
                s.open
            } else {
                self.default_open
            });
            if open != s.open {
                s.set_open(open, window, cx);
            }
            if let Some(mode) = self.panel_mode {
                s.panel_mode = mode;
            }
            s.open_initialized = true;
            if S::HAS_DATE && s.view_controlled && !s.cursor_in_view(s.cursor.date()) {
                s.cursor = s.view_date.and_time(s.cursor.time());
            }
            if s.disabled {
                s.open = false;
                s.reset_draft(window, cx);
            }
            if sync {
                s.sync_editors(window, cx);
            }
            s.geometry.clear_frame();
        });
        let s = self.state.read(cx);
        let context = PickerStyleContext {
            picker: s.options.mode,
            panel_mode: s.panel_mode,
            size: self.size.unwrap_or(config.size),
            variant: self.variant.unwrap_or(config.variant),
            status: self.status,
            disabled: s.disabled,
            open: s.is_open(),
            hovered: s.hovered,
            focused: s.owns_focus(window, cx),
            range: S::RANGE,
            multiple: S::MULTIPLE,
        };
        let mut styles = provider.styles.clone();
        if let Some(f) = &provider.styles_fn {
            styles.refine(&f.resolve(&context, window, cx));
        }
        styles.refine(&self.styles);
        if let Some(f) = &self.styles_fn {
            styles.refine(&f(&context, window, cx));
        }
        PickerPanelParts {
            state: self.state.clone(),
            scope,
            size: self.size.unwrap_or(config.size),
            today: (self.state.read(cx).clock)().date(),
            tokens: config.theme.component::<DatePickerTheme>(),
            styles,
            presets: self.presets.clone(),
            footer: self.footer.clone(),
            arrow: self.arrow.unwrap_or(provider.arrow),
            navigation_icons: self.navigation_icons.clone(),
            cell_render: self.cell_render.clone(),
            generation: self.state.read(cx).generation,
        }
    }
}
impl<S: PickerSelection> Styled for Picker<S> {
    fn style(&mut self) -> &mut StyleRefinement {
        &mut self.styles.root
    }
}

struct Mount<S: PickerSelection> {
    state: WeakEntity<PickerState<S>>,
    scope: ConfigScope,
    _observer: Subscription,
    _scope: Subscription,
    _release: Subscription,
}
impl<S: PickerSelection> Mount<S> {
    fn new(state: &Entity<PickerState<S>>, scope: &ConfigScope, cx: &mut Context<Self>) -> Self {
        Self {
            state: state.downgrade(),
            scope: scope.clone(),
            _observer: cx.observe(state, |_, _, cx| cx.notify()),
            _scope: scope.observe(cx),
            _release: cx.on_release(|this: &mut Self, cx| {
                let _ = this.state.update(cx, |s, _| s.release_mount());
            }),
        }
    }
    fn sync(
        &mut self,
        state: &Entity<PickerState<S>>,
        scope: &ConfigScope,
        cx: &mut Context<Self>,
    ) {
        if self.state.entity_id() != state.entity_id() {
            let _ = self
                .state
                .update(cx, |previous, _| previous.release_mount());
            self.state = state.downgrade();
            self._observer = cx.observe(state, |_, _, cx| cx.notify());
        }
        if self.scope != *scope {
            self._scope = scope.observe(cx);
            self.scope = scope.clone();
        }
    }
}

pub struct PickerPanelParts<S: PickerSelection> {
    state: Entity<PickerState<S>>,
    scope: ConfigScope,
    size: Size,
    today: NaiveDate,
    tokens: Arc<DatePickerTokens>,
    styles: PickerStyles,
    presets: Arc<[PickerPreset<S>]>,
    footer: Option<Content>,
    arrow: PopupArrow,
    navigation_icons: NavigationIcons,
    cell_render: Option<CellRender>,
    generation: u64,
}
impl<S: PickerSelection> Clone for PickerPanelParts<S> {
    fn clone(&self) -> Self {
        Self {
            state: self.state.clone(),
            scope: self.scope.clone(),
            size: self.size,
            today: self.today,
            tokens: self.tokens.clone(),
            styles: self.styles.clone(),
            presets: self.presets.clone(),
            footer: self.footer.clone(),
            arrow: self.arrow,
            navigation_icons: self.navigation_icons.clone(),
            cell_render: self.cell_render.clone(),
            generation: self.generation,
        }
    }
}

impl<S: PickerSelection> RenderOnce for Picker<S> {
    fn render(mut self, window: &mut Window, cx: &mut App) -> impl IntoElement {
        let parts = self.configure(false, window, cx);
        let config = parts.scope.snapshot(cx);
        let tokens = &parts.tokens;
        let g = tokens.globals();
        let s = self.state.read(cx);
        let disabled = s.disabled;
        let open = s.is_open();
        let hovered = s.hovered;
        let focused = s.owns_focus(window, cx);
        let size = self.size.unwrap_or(config.size);
        let variant = self.variant.unwrap_or(config.variant);
        let active = s.active;
        let style = tokens.input_style(
            size,
            variant,
            self.status,
            InputInteraction {
                disabled,
                hovered,
                focused,
                keyboard_focus: false,
            },
        );
        let metrics = tokens.trigger_metrics(size);
        let options = s.options.clone();
        let editors = s.editors.clone();
        let popup = s.popup.clone();
        let accepted = S::to_slots(&s.accepted);
        let nonempty = accepted.iter().any(Option::is_some);
        let provider = if S::RANGE {
            &config.range_picker
        } else if !S::HAS_DATE {
            &config.time_picker
        } else {
            &config.date_picker
        };
        let allow_clear = self.allow_clear.unwrap_or(provider.allow_clear) && nonempty && !disabled;
        let observed = self.state.clone();
        let observed_scope = parts.scope.clone();
        let mount = window.use_keyed_state(self.id.clone(), cx, move |_, cx| {
            Mount::new(&observed, &observed_scope, cx)
        });
        mount.update(cx, |mount, cx| mount.sync(&self.state, &parts.scope, cx));
        let character_count = if S::HAS_DATE && S::HAS_TIME {
            19.
        } else if !S::HAS_DATE {
            if options.time.use_12_hours { 10. } else { 8. }
        } else {
            10.
        };
        let character_count = character_count
            + if options.time.show_millisecond {
                4.
            } else {
                0.
            };
        let intrinsic = if metrics.font_size < 14. {
            37. + character_count * 6.
        } else {
            42. + character_count * (if metrics.font_size >= 16. { 8. } else { 6.5 })
        };
        let width = (intrinsic * if S::RANGE { 2. } else { 1. }
            + g.font_size
            + metrics.suffix_gap
            + 2. * (metrics.padding_inline
                + if style.underline_only {
                    0.
                } else {
                    style.border_width
                })
            + if S::RANGE {
                g.font_size + 2. + g.padding_xs + metrics.suffix_gap * 2.
            } else {
                0.
            }) as f32
            + f32::from(self.state.read(cx).prefix_width)
            + if self.prefix.is_some() {
                g.padding_xxs as f32
            } else {
                0.
            };
        let target = self.state.clone();
        let hover_target = self.state.clone();
        // Snap each complete content inset once. Snapping a border and its
        // adjacent padding separately otherwise loses a device pixel at 150%.
        let padding = |value: f64, border: f64| {
            (window.pixel_snap(px((value + border) as f32)) - window.pixel_snap(px(border as f32)))
                .max(px(0.))
        };
        let horizontal_border = if style.underline_only {
            0.
        } else {
            style.border_width
        };
        let padding_left = padding(
            if S::MULTIPLE && config.direction != Direction::Rtl {
                (g.padding_xxs - g.line_width).max(0.)
            } else {
                metrics.padding_inline
            },
            horizontal_border,
        );
        let padding_right = padding(
            if S::MULTIPLE && config.direction == Direction::Rtl {
                (g.padding_xxs - g.line_width).max(0.)
            } else {
                metrics.padding_inline
            },
            horizontal_border,
        );
        let block = if S::MULTIPLE {
            (metrics.height - metrics.tag_height - 2. * g.line_width).max(0.) / 2.
        } else {
            metrics.padding_block
        };
        let padding_top = padding(block, horizontal_border);
        let padding_bottom = padding(block, style.border_width);
        let mut trigger = div()
            .id("trigger")
            .relative()
            .flex()
            .items_center()
            .min_w_0()
            .w(px(width))
            .when(S::MULTIPLE, |d| d.w_full())
            .min_h(px(metrics.height as f32))
            .pl(padding_left)
            .pr(padding_right)
            .pt(padding_top)
            .pb(padding_bottom)
            .gap(px(metrics.suffix_gap as f32))
            .border(px(style.border_width as f32))
            .border_color(style.border)
            .rounded(px(style.radius as f32))
            .bg(style.background)
            .font(config.font.clone())
            .text_size(px(metrics.font_size as f32))
            .line_height(px(metrics.line_height as f32))
            .text_color(style.text)
            .when(config.direction == Direction::Rtl, |d| d.flex_row_reverse())
            .when(disabled, |d| d.cursor_not_allowed())
            .when(!disabled, |d| d.cursor_text())
            .on_hover(move |hovered, _, cx| {
                hover_target.update(cx, |s, cx| {
                    if s.hovered != *hovered {
                        s.hovered = *hovered;
                        cx.notify();
                    }
                })
            })
            .on_mouse_down(MouseButton::Left, move |_, w, c| {
                target.update(c, |s, c| {
                    if !s.disabled {
                        s.request_open(true, false, w, c);
                        if !s.owns_focus(w, c) {
                            s.focus(w, c);
                        }
                    }
                })
            });
        if style.underline_only {
            trigger = trigger.border_0().border_b(px(style.border_width as f32));
        }
        if !style.shadow.is_empty() {
            trigger = trigger.shadow(
                style
                    .shadow
                    .iter()
                    .map(|s| gpui::BoxShadow {
                        color: s.color.to_hsla(),
                        offset: point(px(s.x as f32), px(s.y as f32)),
                        blur_radius: px(s.blur as f32),
                        spread_radius: px(s.spread as f32),
                        inset: s.inset,
                    })
                    .collect::<Vec<_>>(),
            );
        }
        if let Some(prefix) = self.prefix.take() {
            let measure = self.state.clone();
            let mut slot = div()
                .relative()
                .flex_none()
                .line_height(relative(1.))
                .when(config.direction == Direction::Rtl, |d| {
                    d.ml(px((g.padding_xxs - metrics.suffix_gap).max(0.) as f32))
                })
                .when(config.direction == Direction::Ltr, |d| {
                    d.mr(px((g.padding_xxs - metrics.suffix_gap).max(0.) as f32))
                })
                .child(prefix)
                .child(measure_element(move |bounds, c| {
                    measure.update(c, |s, c| {
                        s.geometry.prefix = Some(bounds);
                        if (s.prefix_width - bounds.size.width).abs() > px(0.01) {
                            s.prefix_width = bounds.size.width;
                            c.notify();
                        }
                    })
                }));
            slot.style().refine(&parts.styles.prefix);
            trigger = trigger.child(slot);
        }
        if S::MULTIPLE {
            let mut tags = div()
                .flex()
                .flex_wrap()
                .items_center()
                .gap(px(4.))
                .flex_1()
                .min_w_0()
                .when(config.direction == Direction::Rtl, |d| d.flex_row_reverse());
            if !nonempty {
                tags = tags.child(
                    div()
                        .when(config.direction == Direction::Rtl, |d| {
                            d.mr(px((g.padding_sm - 1.).max(0.) as f32))
                        })
                        .when(config.direction == Direction::Ltr, |d| {
                            d.ml(px((g.padding_sm - 1.).max(0.) as f32))
                        })
                        .text_color(style.placeholder)
                        .child(self.placeholder.clone().unwrap_or_else(|| {
                            single_placeholder(&config.locale.picker, options.mode, true)
                        })),
                );
            }
            for value in accepted.iter().flatten() {
                let label = self.state.read(cx).format_value(*value);
                // Keep the shaped advance through a layout ratio: GPUI snaps
                // authored pixel widths before flex layout, which would round
                // every chip separately and accumulate error along the row.
                let label_width = window
                    .text_system()
                    .shape_line(
                        label.clone(),
                        px(metrics.font_size as f32),
                        &[gpui::TextRun {
                            len: label.len(),
                            font: config.font.clone(),
                            color: style.text,
                            background_color: None,
                            underline: None,
                            strikethrough: None,
                        }],
                        None,
                    )
                    .width;
                let label_height = window
                    .pixel_snap(px((metrics.tag_height - 2. * g.line_width).max(1.) as f32))
                    .max(px(1. / window.scale_factor()));
                let tag_height = window
                    .pixel_snap(px(metrics.tag_height as f32))
                    .max(px(1. / window.scale_factor()));
                let tag_width = f32::from(label_width)
                    + (2. * g.padding_xs + 2. * g.line_width) as f32
                    + if disabled { 0. } else { 10. };
                let metric_label = label.clone();
                let date = value.date();
                let content_bounds = Rc::new(Cell::new(None));
                let measured_content = content_bounds.clone();
                let remove_bounds = Rc::new(Cell::new(None));
                let measured_remove = remove_bounds.clone();
                let measure_tag = self.state.clone();
                let tag_border = window.pixel_snap(px(g.line_width as f32));
                let weak = self.state.downgrade();
                let context = PickerTagContext {
                    value: date,
                    disabled,
                    remove: Rc::new(move |w, c| {
                        let _ = weak.update(c, |s, c| s.remove_date(date, w, c));
                    }),
                };
                let removal = context.clone();
                let mut tag = div()
                    .id(SharedString::from(format!("tag:{date}")))
                    .flex()
                    .flex_none()
                    .min_w_0()
                    .items_center()
                    .justify_between()
                    .h(tag_height)
                    .aspect_ratio(tag_width / f32::from(tag_height))
                    .pl(px((if config.direction == Direction::Rtl {
                        g.padding_xs / 2.
                    } else {
                        g.padding_xs
                    }) as f32))
                    .pr(px((if config.direction == Direction::Rtl {
                        g.padding_xs
                    } else {
                        g.padding_xs / 2.
                    }) as f32))
                    .when(config.direction == Direction::Rtl, |d| d.flex_row_reverse())
                    .border(px(g.line_width as f32))
                    .border_color(if disabled {
                        tokens.multiple_item_border_color_disabled
                    } else {
                        tokens.multiple_item_border_color
                    })
                    .line_height(px((metrics.tag_height - 2. * g.line_width).max(0.) as f32))
                    .rounded(px(g.border_radius_sm as f32))
                    .bg(tokens.multiple_item_bg)
                    .child(
                        div()
                            .relative()
                            .flex_none()
                            .min_w_0()
                            .h(label_height)
                            .aspect_ratio(f32::from(label_width) / f32::from(label_height))
                            .whitespace_nowrap()
                            .when(config.direction == Direction::Rtl, |d| {
                                d.ml(px((g.padding_xs / 2.) as f32))
                            })
                            .when(config.direction == Direction::Ltr, |d| {
                                d.mr(px((g.padding_xs / 2.) as f32))
                            })
                            .child(label)
                            .child(measure_element(move |bounds, _| {
                                measured_content.set(Some(bounds));
                            })),
                    );
                if !disabled {
                    tag = tag.child(
                        PickerAction::new(
                            "remove",
                            config.locale.common.remove_selection.clone(),
                            div()
                                .relative()
                                .w(px(10.))
                                .h(px((metrics.tag_height - 2. * g.line_width).max(0.) as f32))
                                .flex()
                                .items_center()
                                .justify_center()
                                .child(
                                    div()
                                        .relative()
                                        .size(px(10.))
                                        .child(picker_icon(IconName::CloseOutlined, 10.))
                                        .child(measure_element(move |bounds, _| {
                                            measured_remove.set(Some(bounds))
                                        })),
                                )
                                .into_any_element(),
                            move |w, c| removal.remove(w, c),
                        )
                        .scope(parts.scope.clone())
                        .text_color(g.color_icon)
                        .hover_style(div().text_color(g.color_icon_hover).style().clone()),
                    );
                }
                let tag = tag
                    .child(measure_element(move |bounds, c| {
                        measure_tag.update(c, |s, _| {
                            s.geometry.tags.push(super::PickerTagGeometry {
                                key: date.to_string().into(),
                                label: metric_label.clone(),
                                bounds: bounds.dilate(tag_border),
                                content: content_bounds.get().unwrap_or(bounds),
                                remove: remove_bounds.get(),
                            });
                        })
                    }))
                    .into_any_element();
                tags = tags.child(if let Some(render) = &self.tag_render {
                    render(&context, tag, window, cx)
                } else {
                    tag
                });
            }
            trigger = trigger.child(tags);
            let focus = editors[0].read(cx).focus_handle(cx).tab_stop(!disabled);
            let expand = self.state.clone();
            let collapse = self.state.clone();
            trigger = trigger
                .track_focus(&focus)
                .role(Role::ComboBox)
                .aria_expanded(open)
                .on_a11y_action(accesskit::Action::Expand, move |_, w, c| {
                    expand.update(c, |s, c| s.request_open(true, false, w, c))
                })
                .on_a11y_action(accesskit::Action::Collapse, move |_, w, c| {
                    collapse.update(c, |s, c| s.request_open(false, true, w, c))
                })
                .aria_value(
                    accepted
                        .iter()
                        .flatten()
                        .map(|v| self.state.read(cx).format_value(*v).to_string())
                        .collect::<Vec<_>>()
                        .join(", "),
                );
        } else {
            for (index, editor) in editors
                .iter()
                .enumerate()
                .take(if S::RANGE { 2 } else { 1 })
            {
                if index == 1 {
                    trigger = trigger.child(
                        div()
                            .flex_none()
                            .px(px((g.padding_xs - metrics.suffix_gap).max(0.) as f32))
                            .text_color(g.color_text_disabled)
                            .child(
                                picker_icon(IconName::SwapRightOutlined, g.font_size_lg as f32)
                                    .mirror(config.direction == Direction::Rtl),
                            ),
                    );
                }
                let placeholder = if S::RANGE {
                    self.range_placeholders
                        .as_ref()
                        .map(|p| p[index].clone())
                        .unwrap_or_else(|| {
                            range_placeholder(
                                &config.locale.picker,
                                options.mode,
                                S::HAS_DATE,
                                index,
                            )
                        })
                } else {
                    self.placeholder.clone().unwrap_or_else(|| {
                        single_placeholder(&config.locale.picker, options.mode, S::HAS_DATE)
                    })
                };
                let label = self.label.clone().unwrap_or_else(|| placeholder.clone());
                let label = if S::RANGE && self.label.is_some() {
                    format!(
                        "{} {}",
                        label,
                        if index == 0 {
                            config.locale.picker.range_start.as_ref()
                        } else {
                            config.locale.picker.range_end.as_ref()
                        }
                    )
                    .into()
                } else {
                    label
                };
                let target = self.state.clone();
                let key_target = self.state.clone();
                let a11y_target = self.state.clone();
                let mut input = Input::new(("editor", index), editor)
                    .scope(parts.scope.clone())
                    .size(size)
                    .disabled(disabled || options.disabled_endpoints[index])
                    .read_only(options.input_read_only)
                    .placeholder(placeholder)
                    .accessibility_label(label)
                    .semantic_role(Role::ComboBox);
                input.embedded = true;
                if let Some(id) = &self.automation_id {
                    input = input.automation_id(if S::RANGE {
                        format!("{}.{}", id, if index == 0 { "start" } else { "end" }).into()
                    } else {
                        id.clone()
                    });
                }
                if let Some(description) = &self.description {
                    input = input.accessibility_description(description.clone());
                }
                input.style().refine(&parts.styles.input);
                input.frame_decorator = Some(Box::new(move |mut root, _, _, _| {
                    let target = target.clone();
                    let collapse = a11y_target.clone();
                    macro_rules! mask_action {
                        ($action:ty,$key:literal) => {{
                            let mask_target = key_target.clone();
                            root = root.on_action(move |_: &$action, w, c| {
                                if mask_target.update(c, |s, c| s.mask_key(index, $key, w, c)) {
                                    w.prevent_default();
                                } else {
                                    c.propagate();
                                }
                            });
                        }};
                    }
                    mask_action!(crate::input::actions::Left, "left");
                    mask_action!(crate::input::actions::Right, "right");
                    mask_action!(crate::input::actions::Up, "up");
                    mask_action!(crate::input::actions::Down, "down");
                    mask_action!(crate::input::actions::Backspace, "backspace");
                    mask_action!(crate::input::actions::Delete, "delete");
                    mask_action!(crate::input::actions::Paste, "paste");
                    mask_action!(crate::input::actions::Cut, "cut");
                    let pointer_target = key_target.clone();
                    root.aria_expanded(open)
                        .capture_key_down(move |event, w, c| {
                            if !event.keystroke.modifiers.control
                                && !event.keystroke.modifiers.platform
                                && !event.keystroke.modifiers.alt
                                && key_target
                                    .update(c, |s, c| s.mask_key(index, &event.keystroke.key, w, c))
                            {
                                w.prevent_default();
                                c.stop_propagation();
                            }
                        })
                        .on_mouse_up(MouseButton::Left, move |_, w, c| {
                            pointer_target.update(c, |s, c| {
                                let offset = s.editors[index].read(c).selection().caret;
                                s.mask_select_at(index, offset, w, c);
                            })
                        })
                        .on_a11y_action(accesskit::Action::Expand, move |_, w, c| {
                            target.update(c, |s, c| s.request_open(true, false, w, c))
                        })
                        .on_a11y_action(accesskit::Action::Collapse, move |_, w, c| {
                            collapse.update(c, |s, c| s.request_open(false, true, w, c))
                        })
                }));
                let input = if let Some(render) = &self.input_render {
                    render(index, input, window, cx)
                } else {
                    input.into_any_element()
                };
                let mut slot = div()
                    .id(("input-slot", index))
                    .relative()
                    .flex_1()
                    .min_w_0()
                    .child(input);
                if S::RANGE && open && active == index {
                    let measure = self.state.clone();
                    let containing_border = if style.underline_only {
                        0.
                    } else {
                        style.border_width as f32
                    };
                    slot = slot.child(
                        div()
                            .absolute()
                            // The source bar uses the range root's padding
                            // box, whose origin includes its physical border.
                            .left(px(containing_border))
                            .right(px(-containing_border))
                            .bottom(px(-(metrics.padding_block as f32) - 1.))
                            .h(px(2.))
                            .bg(g.color_primary)
                            .child(measure_element(move |bounds, c| {
                                measure.update(c, |s, _| s.geometry.active_bar = Some(bounds))
                            })),
                    );
                }
                let measure = self.state.clone();
                slot = slot.child(measure_element(move |bounds, c| {
                    measure.update(c, |s, _| {
                        while s.geometry.inputs.len() <= index {
                            s.geometry.inputs.push(bounds);
                        }
                        s.geometry.inputs[index] = bounds;
                    })
                }));
                trigger = trigger.child(slot);
            }
        }
        let suffix = self.suffix.take().unwrap_or_else(|| {
            picker_icon(
                if S::HAS_DATE {
                    IconName::CalendarOutlined
                } else {
                    IconName::ClockCircleOutlined
                },
                g.font_size as f32,
            )
            .into_any_element()
        });
        let mut suffix_slot = div()
            .relative()
            .flex_none()
            .size(px(g.font_size as f32))
            .text_color(g.color_text_disabled)
            .child(
                div()
                    .opacity(if allow_clear && (hovered || focused) {
                        0.
                    } else {
                        1.
                    })
                    .child(suffix),
            );
        if allow_clear {
            let target = self.state.clone();
            let accessible = self.state.clone();
            suffix_slot = suffix_slot.child(
                Accessibility::new(Role::Button)
                    .label(config.locale.picker.clear.clone())
                    .apply(
                        div()
                            .id("clear")
                            .absolute()
                            .inset_0()
                            .opacity(if hovered || focused { 1. } else { 0. })
                            .cursor_pointer()
                            .on_mouse_down(MouseButton::Left, |_, w, c| {
                                w.prevent_default();
                                c.stop_propagation();
                            })
                            .on_click(move |_, w, c| {
                                target.update(c, |s, c| s.clear(w, c));
                                c.stop_propagation();
                            })
                            .on_a11y_action(accesskit::Action::Click, move |_, w, c| {
                                accessible.update(c, |s, c| s.clear(w, c))
                            })
                            .child(picker_icon(IconName::CloseCircleFilled, g.font_size as f32)),
                    ),
            );
        }
        suffix_slot.style().refine(&parts.styles.suffix);
        trigger = trigger.child(suffix_slot);
        let key_target = self.state.clone();
        let measure = self.state.clone();
        let measured_border = window.pixel_snap(px(style.border_width as f32));
        let underline = style.underline_only;
        trigger = trigger
            .capture_key_down(move |e, w, c| {
                let multiple_trigger = S::MULTIPLE
                    && key_target.read(c).editors[0]
                        .read(c)
                        .focus_handle(c)
                        .is_focused(w);
                if multiple_trigger && matches!(e.keystroke.key.as_str(), "enter" | "space") {
                    key_target.update(c, |s, c| s.request_open(true, false, w, c));
                    w.prevent_default();
                    c.stop_propagation();
                } else if e.keystroke.key == "escape" {
                    key_target.update(c, |s, c| {
                        s.request_open(false, true, w, c);
                        s.focus(w, c);
                    });
                    w.prevent_default();
                    c.stop_propagation();
                } else if e.keystroke.modifiers.alt && e.keystroke.key == "down" {
                    key_target.update(c, |s, c| {
                        s.request_open(true, false, w, c);
                        s.panel_focus.focus(w, c);
                    });
                    w.prevent_default();
                    c.stop_propagation();
                }
            })
            .child(measure_element(move |bounds, c| {
                let bounds = if underline {
                    Bounds::new(
                        bounds.origin,
                        gpui::size(bounds.size.width, bounds.size.height + measured_border),
                    )
                } else {
                    bounds.dilate(measured_border)
                };
                measure.update(c, |s, _| s.geometry.trigger = Some(bounds))
            }));
        trigger.style().refine(&parts.styles.root);
        let trigger = if S::MULTIPLE {
            let semantics = Accessibility {
                label: Some(
                    self.label
                        .clone()
                        .unwrap_or_else(|| config.locale.picker.select_date.clone()),
                ),
                description: self.description.clone(),
                automation_id: self.automation_id.clone(),
                disabled,
                read_only: true,
                ..Accessibility::new(Role::ComboBox)
            };
            crate::behavior::FocusRing::new(g.color_primary_border, px(g.line_width_focus as f32))
                .radius(px(metrics.radius as f32))
                .apply(
                    semantics.apply(trigger),
                    &editors[0].read(cx).focus_handle(cx),
                )
                .into_any_element()
        } else {
            trigger.into_any_element()
        };
        let content = parts.clone();
        let changed = self.state.clone();
        let render = self.panel_render.clone();
        let arrow_state = self.state.downgrade();
        let range_arrow = parts.arrow;
        let arrow_size = g.size_popup_arrow as f32;
        let arrow_padding = metrics.padding_inline as f32;
        let direction = config.direction;
        Popup::new((self.id, "popup"))
            .scope(parts.scope.clone())
            .handle(&popup)
            .child(trigger)
            .open(open)
            .disabled(disabled)
            .triggers(PopupTriggers::NONE)
            .arrow(if S::RANGE {
                PopupArrow {
                    point_at_center: false,
                    ..parts.arrow
                }
            } else {
                parts.arrow
            })
            .gap(px(if S::RANGE && parts.arrow.visible {
                // CSS layout quantizes the range dropdown's 2/3-arrow padding.
                // Popup's arrow layer adds half its width to the body distance.
                (arrow_size * (2. / 3.) * 64.).floor() / 64. + 4. - arrow_size / 2.
            } else {
                4.
            }))
            .when(S::RANGE, |popup| {
                popup.arrow_attachment(move |layout, cx| {
                    let state = arrow_state.upgrade()?;
                    let state = state.read(cx);
                    let input = state.geometry.inputs.get(state.active)?;
                    if layout.placement.is_vertical() {
                        let x = if range_arrow.point_at_center {
                            input.center().x
                        } else if direction == Direction::Rtl {
                            input.right() - px(arrow_padding * 1.5 + arrow_size / 2.)
                        } else {
                            input.left() + px(arrow_padding * 1.5 + arrow_size / 2.)
                        } - layout.bounds.left();
                        let inset = px(arrow_size).min(layout.bounds.size.width / 2.);
                        Some(point(
                            x.clamp(inset, layout.bounds.size.width - inset),
                            if layout.placement.before() {
                                layout.bounds.size.height
                            } else {
                                px(0.)
                            },
                        ))
                    } else {
                        let inset = px(arrow_size).min(layout.bounds.size.height / 2.);
                        Some(point(
                            if layout.placement.before() {
                                layout.bounds.size.width
                            } else {
                                px(0.)
                            },
                            (input.center().y - layout.bounds.top())
                                .clamp(inset, layout.bounds.size.height - inset),
                        ))
                    }
                })
            })
            .placement(self.placement.or(provider.placement).unwrap_or(
                if config.direction == Direction::Rtl {
                    PopupPlacement::BottomRight
                } else {
                    PopupPlacement::BottomLeft
                },
            ))
            .fresh(true)
            .z_index(tokens.z_index_popup as usize)
            .background(g.color_bg_elevated)
            .radius(px(g.border_radius_lg as f32))
            .shadows(g.box_shadow_secondary.clone())
            .surface_style(div().p_0().style().clone())
            .accessibility_role(Role::Group)
            .accessibility_label(config.locale.picker.panel_label.clone())
            .on_open_change(move |change, w, c| {
                changed.update(c, |s, c| {
                    if !change.open && change.reason == PopupOpenReason::Escape {
                        s.close_and_restore_focus(w, c);
                    } else {
                        s.request_open(change.open, false, w, c);
                    }
                })
            })
            .content(move |_, w, c| {
                if let Some(render) = &render {
                    render(&content, w, c)
                } else {
                    content.panel(w, c)
                }
            })
    }
}

fn picker_icon(name: IconName, size: f32) -> crate::icons::Icon {
    // Ant's 64..960 frame crops the catalog's 0..1024 frame. Request the
    // final raster density instead of enlarging a smaller mask after rendering.
    let artwork = px(size * 8. / 7.);
    icon(name)
        .paint_size(gpui::size(artwork, artwork))
        .inherit_color()
        .size(px(size))
}

fn measure_element(f: impl Fn(Bounds<Pixels>, &mut App) + 'static) -> impl IntoElement {
    canvas(move |bounds, _, cx| f(bounds, cx), |_, (), _, _| ())
        .absolute()
        .inset_0()
}
fn single_placeholder(
    locale: &crate::locale::PickerLocale,
    mode: PickerMode,
    date: bool,
) -> SharedString {
    if !date {
        return locale.select_time.clone();
    }
    match mode {
        PickerMode::Date => locale.select_date.clone(),
        PickerMode::Week => locale.select_week.clone(),
        PickerMode::Month => locale.select_month.clone(),
        PickerMode::Quarter => locale.select_quarter.clone(),
        PickerMode::Year => locale.select_year.clone(),
    }
}
fn range_placeholder(
    locale: &crate::locale::PickerLocale,
    mode: PickerMode,
    date: bool,
    index: usize,
) -> SharedString {
    let start = index == 0;
    if !date {
        return if start {
            locale.range_start_time.clone()
        } else {
            locale.range_end_time.clone()
        };
    }
    match (mode, start) {
        (PickerMode::Date, true) => locale.range_start.clone(),
        (PickerMode::Date, false) => locale.range_end.clone(),
        (PickerMode::Week, true) => locale.range_start_week.clone(),
        (PickerMode::Week, false) => locale.range_end_week.clone(),
        (PickerMode::Month, true) => locale.range_start_month.clone(),
        (PickerMode::Month, false) => locale.range_end_month.clone(),
        (PickerMode::Quarter, true) => locale.range_start_quarter.clone(),
        (PickerMode::Quarter, false) => locale.range_end_quarter.clone(),
        (PickerMode::Year, true) => locale.range_start_year.clone(),
        (PickerMode::Year, false) => locale.range_end_year.clone(),
    }
}

#[derive(IntoElement)]
pub struct PickerPanel<S: PickerSelection> {
    picker: Picker<S>,
}
impl<S: PickerSelection> PickerPanel<S> {
    pub fn new(id: impl Into<ElementId>, state: &Entity<PickerState<S>>) -> Self {
        Self {
            picker: Picker::new(id, state),
        }
    }
    pub fn scope(mut self, v: ConfigScope) -> Self {
        self.picker = self.picker.scope(v);
        self
    }
    pub fn value(mut self, v: S::Value) -> Self {
        self.picker = self.picker.value(v);
        self
    }
    pub fn default_value(mut self, v: S::Value) -> Self {
        self.picker = self.picker.default_value(v);
        self
    }
    pub fn picker_mode(mut self, v: PickerMode) -> Self
    where
        S: super::DateGranularitySelection,
    {
        self.picker = self.picker.picker_mode(v);
        self
    }
    pub fn time_options(mut self, v: TimeOptions) -> Self
    where
        S: super::TimePickerSelection,
    {
        self.picker = self.picker.time_options(v);
        self
    }
    pub fn format(mut self, v: PickerFormat) -> Self {
        self.picker = self.picker.format(v);
        self
    }
    pub fn disabled(mut self, v: bool) -> Self {
        self.picker = self.picker.disabled(v);
        self
    }
    pub fn on_change(
        mut self,
        f: impl Fn(&PickerChange<S::Value>, &mut Window, &mut App) + 'static,
    ) -> Self {
        self.picker = self.picker.on_change(f);
        self
    }
}
impl<S: PickerSelection> Styled for PickerPanel<S> {
    fn style(&mut self) -> &mut StyleRefinement {
        &mut self.picker.styles.popup
    }
}
impl<S: PickerSelection> RenderOnce for PickerPanel<S> {
    fn render(self, w: &mut Window, c: &mut App) -> impl IntoElement {
        let parts = self.picker.configure(true, w, c);
        let observed = self.picker.state.clone();
        let scope = parts.scope.clone();
        let mount = w.use_keyed_state(self.picker.id.clone(), c, move |_, cx| {
            Mount::new(&observed, &scope, cx)
        });
        mount.update(c, |mount, cx| {
            mount.sync(&self.picker.state, &parts.scope, cx)
        });
        parts.panel(w, c)
    }
}

impl<S: PickerSelection> PickerPanelParts<S> {
    pub fn state(&self) -> &Entity<PickerState<S>> {
        &self.state
    }
    /// Commands from a custom panel are valid only for the session that created it.
    pub fn select(&self, date: NaiveDate, w: &mut Window, c: &mut App) {
        self.state.update(c, |s, c| {
            if s.mounted && s.generation == self.generation {
                s.select_date(date, PickerChangeSource::Calendar, w, c);
            }
        });
    }
    pub fn submit(&self, w: &mut Window, c: &mut App) {
        self.state.update(c, |s, c| {
            if s.mounted && s.generation == self.generation {
                s.submit(PickerChangeSource::Calendar, w, c);
            }
        });
    }
    pub fn hover(&self, date: Option<NaiveDate>, w: &mut Window, c: &mut App) {
        self.state.update(c, |s, c| {
            if s.mounted && s.generation == self.generation {
                s.hover_date(date, w, c);
            }
        });
    }
    pub fn navigate(&self, amount: i32, large: bool, w: &mut Window, c: &mut App) {
        self.state.update(c, |s, c| {
            if s.mounted && s.generation == self.generation {
                s.navigate(amount, large, w, c);
            }
        });
    }
    pub fn change_mode(&self, mode: PickerPanelMode, w: &mut Window, c: &mut App) {
        self.state.update(c, |s, c| {
            if s.mounted && s.generation == self.generation {
                s.change_panel(mode, w, c);
            }
        });
    }
    pub fn select_time(&self, unit: TimeUnit, value: u32, w: &mut Window, c: &mut App) {
        self.state.update(c, |s, c| {
            if s.mounted && s.generation == self.generation {
                s.select_time(unit, value, PickerChangeSource::Time, w, c);
            }
        });
    }
    pub fn clear(&self, w: &mut Window, c: &mut App) {
        self.state.update(c, |s, c| {
            if s.mounted && s.generation == self.generation {
                s.clear(w, c);
            }
        });
    }
    pub fn panel(&self, w: &mut Window, c: &mut App) -> AnyElement {
        let config = self.scope.snapshot(c);
        let g = self.tokens.globals();
        let s = self.state.read(c);
        let focus = s.panel_focus.clone();
        let dual = S::RANGE && S::HAS_DATE && !S::HAS_TIME && s.panel_mode == s.target_panel();
        let view = s.view_date;
        let mode = s.panel_mode;
        let right = if dual {
            match mode {
                PickerPanelMode::Date => calendar::add_months(view, 1),
                PickerPanelMode::Month | PickerPanelMode::Quarter => calendar::add_years(view, 1),
                PickerPanelMode::Year => calendar::add_years(view, 10),
                _ => None,
            }
        } else {
            None
        };
        let has_footer = self.footer.is_some()
            || (mode == s.target_panel()
                && (s.need_confirm()
                    || s.options.show_now.unwrap_or(
                        !S::RANGE && !S::MULTIPLE && matches!(s.options.mode, PickerMode::Date),
                    )));
        let show_time = S::HAS_TIME && (!S::HAS_DATE || mode == PickerPanelMode::Date);
        let time_width =
            s.options.time.units().len() as f64 * self.tokens.time_column_width + g.line_width;
        let mut content = div()
            .relative()
            .flex()
            .when(config.direction == Direction::Rtl, |d| d.flex_row_reverse());
        if !self.presets.is_empty() {
            content = content.child(self.presets(w, c));
        }
        let mut main = div().flex().flex_col();
        let mut panels = div()
            .flex()
            .when(right.is_some(), |d| {
                d.w(px((self.tokens.panel_width() * 2.) as f32)).flex_none()
            })
            .when(config.direction == Direction::Rtl, |d| d.flex_row_reverse());
        if S::HAS_DATE {
            let share = right.map(|_| 0.5);
            panels = panels.child(self.calendar_with_width(view, 0, dual, share, w, c));
            if let Some(date) = right {
                panels = panels.child(self.calendar_with_width(date, 1, true, share, w, c));
            }
        }
        if show_time {
            let mut time = div().flex().flex_col();
            if S::HAS_DATE {
                // Reserve separator and body together before pixel snapping.
                time = time.w(px(time_width as f32)).flex_none();
                let s = self.state.read(c);
                let value = s.draft.get(s.active).copied().flatten().unwrap_or(s.cursor);
                let measure = self.state.clone();
                let header_border = w.pixel_snap(px(g.line_width as f32));
                time = time.child(
                    div()
                        .relative()
                        .h(px(self.tokens.header_height() as f32))
                        .flex()
                        .items_center()
                        .justify_center()
                        .border_b(px(g.line_width as f32))
                        .border_color(g.color_split)
                        .child(value.format("%H:%M:%S").to_string())
                        .child(measure_element(move |bounds, c| {
                            measure.update(c, |s, _| {
                                let bounds = Bounds::new(
                                    bounds.origin,
                                    gpui::size(
                                        bounds.size.width,
                                        bounds.size.height + header_border,
                                    ),
                                );
                                while s.geometry.headers.len() < 2 {
                                    s.geometry.headers.push(bounds);
                                }
                                s.geometry.headers[1] = bounds;
                            })
                        })),
                );
            }
            time = time.child(self.time(w, c));
            if S::HAS_DATE {
                time = if config.direction == Direction::Rtl {
                    time.border_r(px(g.line_width as f32))
                } else {
                    time.border_l(px(g.line_width as f32))
                }
                .border_color(g.color_split);
            }
            panels = panels.child(time);
        }
        if show_time {
            let measure = self.state.clone();
            panels = panels.relative().child(measure_element(move |bounds, c| {
                measure.update(c, |s, _| {
                    if s.geometry.panels.is_empty() {
                        s.geometry.panels.push(bounds);
                    } else {
                        s.geometry.panels[0] = bounds;
                    }
                })
            }));
        }
        main = main.child(panels);
        if has_footer {
            main = main.child(self.footer(w, c));
        }
        content = content.child(main);
        content.style().refine(&self.styles.content);
        let target = self.state.clone();
        let measure = self.state.clone();
        let mut root = div()
            .id("panel")
            .relative()
            .track_focus(&focus)
            .key_context("AntDesignPickerPanel")
            .tab_index(0)
            .font(config.font.clone())
            .text_size(px(g.font_size as f32))
            .line_height(relative(g.line_height as f32))
            .text_color(g.color_text)
            .bg(g.color_bg_elevated)
            .rounded(px(g.border_radius_lg as f32))
            .child(content)
            .capture_key_down(move |e, w, c| {
                let handled = target.update(c, |s, c| {
                    if !s.panel_focus.is_focused(w) {
                        return false;
                    }
                    let key = if config.direction == Direction::Rtl {
                        match e.keystroke.key.as_str() {
                            "left" => "right",
                            "right" => "left",
                            other => other,
                        }
                    } else {
                        e.keystroke.key.as_str()
                    };
                    s.panel_key(key, e.keystroke.modifiers.shift, w, c)
                });
                if handled {
                    w.prevent_default();
                    c.stop_propagation();
                }
            })
            .on_mouse_down(MouseButton::Left, |_, w, _| w.prevent_default())
            .child(measure_element(move |bounds, c| {
                measure.update(c, |s, _| s.geometry.popup = Some(bounds))
            }));
        root.style().refine(&self.styles.popup);
        root.style().refine(&self.styles.container);
        root.into_any_element()
    }
    pub fn calendar(
        &self,
        date: NaiveDate,
        panel: usize,
        dual: bool,
        w: &mut Window,
        c: &mut App,
    ) -> AnyElement {
        self.calendar_with_width(date, panel, dual, None, w, c)
    }
    fn calendar_with_width(
        &self,
        date: NaiveDate,
        panel: usize,
        dual: bool,
        width_share: Option<f32>,
        w: &mut Window,
        c: &mut App,
    ) -> AnyElement {
        let config = self.scope.snapshot(c);
        let g = self.tokens.globals();
        let mode = self.state.read(c).panel_mode;
        let cells = self.state.update(c, |s, _| s.calendar_cells(date, mode));
        let s = self.state.read(c);
        let rules = s.format_context.week_rules;
        let show_week = s.options.show_week || s.options.mode == PickerMode::Week;
        let columns = if mode == PickerPanelMode::Date {
            7
        } else if mode == PickerPanelMode::Quarter {
            4
        } else {
            3
        };
        let row_height = if mode == PickerPanelMode::Date {
            self.tokens.cell_height + self.tokens.cell_padding() * 2.
        } else if mode == PickerPanelMode::Quarter {
            self.tokens.quarter_content_height()
        } else {
            self.tokens.without_time_cell_height
        };
        let heading_height = if mode == PickerPanelMode::Date {
            row_height + if show_week { 2. } else { 0. }
        } else {
            0.
        };
        let table_height =
            (row_height * cells.len().div_ceil(columns) as f64 + heading_height).max(0.);
        let share = |height: f64| {
            relative(if table_height > 0. {
                (height / table_height) as f32
            } else {
                0.
            })
        };
        let mut table = div()
            .id("calendar-grid")
            .flex()
            .flex_col()
            .w_full()
            .h(px(table_height as f32))
            .role(Role::Grid)
            .aria_label(config.locale.picker.panel_label.clone())
            .a11y_synthetic_children(|builder| {
                if S::MULTIPLE || S::RANGE {
                    builder.parent_node().set_multiselectable();
                }
            });
        if mode == PickerPanelMode::Date {
            let mut headings = div()
                .id("weekdays")
                .flex()
                .flex_none()
                .min_h_0()
                .role(Role::Row)
                .h(share(heading_height))
                .when(config.direction == Direction::Rtl, |d| d.flex_row_reverse());
            if show_week {
                headings = headings.child(
                    div()
                        .id("week-heading")
                        .flex_1()
                        .flex()
                        .items_center()
                        .justify_center()
                        .role(Role::ColumnHeader)
                        .text_color(g.color_text_disabled)
                        .child(config.locale.picker.week.clone()),
                );
            }
            for index in 0..7 {
                let day = (rules.first_weekday.num_days_from_monday() as usize + index) % 7;
                headings = headings.child(
                    div()
                        .id(("weekday", index))
                        .flex_1()
                        .flex()
                        .items_center()
                        .justify_center()
                        .role(Role::ColumnHeader)
                        .child(config.locale.picker.short_weekdays[day].clone()),
                );
            }
            table = table.child(headings);
        }
        for (row_index, row) in cells.chunks(columns).enumerate() {
            let mut line = div()
                .id(("row", row_index))
                .flex()
                .flex_none()
                .min_h_0()
                .role(Role::Row)
                .h(share(row_height))
                .when(config.direction == Direction::Rtl, |d| d.flex_row_reverse());
            if mode == PickerPanelMode::Date && show_week {
                line = line.child(self.week_number_cell(row[0].date, panel, c));
            }
            for cell in row {
                line = line.child(self.date_cell(cell, panel, w, c));
            }
            table = table.child(line);
        }
        let measure = self.state.clone();
        table = table.relative().child(measure_element(move |bounds, c| {
            measure.update(c, |s, _| {
                while s.geometry.tables.len() <= panel {
                    s.geometry.tables.push(bounds);
                }
                s.geometry.tables[panel] = bounds;
            })
        }));
        let mut body = div()
            .py(px((if mode == PickerPanelMode::Date {
                g.padding_xs
            } else {
                0.
            }) as f32))
            .px(px((if mode == PickerPanelMode::Date {
                if show_week {
                    g.padding_sm
                } else {
                    self.tokens.date_panel_padding()
                }
            } else if mode == PickerPanelMode::Decade {
                0.
            } else {
                g.padding_xs
            }) as f32))
            .child(table);
        body.style().refine(&self.styles.body);
        let measure = self.state.clone();
        let element = div()
            .id(("calendar", panel))
            .relative()
            .flex()
            .flex_col()
            .flex_none()
            .when_some(width_share, |d, share| d.w(relative(share)))
            .when(width_share.is_none(), |d| {
                d.w(px(self.tokens.panel_width() as f32))
            })
            .child(self.header(date, panel, dual, w, c))
            .child(body)
            .child(measure_element(move |bounds, c| {
                measure.update(c, |s, _| {
                    while s.geometry.panels.len() <= panel {
                        s.geometry.panels.push(bounds);
                    }
                    s.geometry.panels[panel] = bounds;
                })
            }));
        element.into_any_element()
    }
    fn header(
        &self,
        date: NaiveDate,
        panel: usize,
        dual: bool,
        w: &mut Window,
        c: &mut App,
    ) -> AnyElement {
        let config = self.scope.snapshot(c);
        let g = self.tokens.globals();
        let locale = &config.locale.picker;
        let mode = self.state.read(c).panel_mode;
        let mut header = div()
            .relative()
            .flex()
            .items_center()
            .px(px(g.padding_xs as f32))
            .h(px(self.tokens.header_height() as f32))
            .border_b(px(g.line_width as f32))
            .border_color(g.color_split)
            .when(config.direction == Direction::Rtl, |d| d.flex_row_reverse());
        let previous = !(dual && panel == 1);
        let next = !(dual && panel == 0);
        let right_panel = dual && panel == 1;
        let prev_label = match mode {
            PickerPanelMode::Date | PickerPanelMode::Month | PickerPanelMode::Quarter => {
                locale.prev_year.clone()
            }
            PickerPanelMode::Year => locale.prev_decade.clone(),
            PickerPanelMode::Decade => locale.prev_century.clone(),
            _ => locale.prev_year.clone(),
        };
        header = header.child(self.nav_button(
            "super-prev",
            (date, right_panel, -1, true),
            previous,
            prev_label,
            w,
            c,
        ));
        if mode == PickerPanelMode::Date {
            header = header.child(self.nav_button(
                "prev",
                (date, right_panel, -1, false),
                previous,
                locale.prev_month.clone(),
                w,
                c,
            ));
        }
        let mut label = div()
            .flex_1()
            .flex()
            .items_center()
            .justify_center()
            .gap(px(8.))
            .font_weight(gpui::FontWeight::SEMIBOLD);
        if mode == PickerPanelMode::Date {
            let month = self.header_label(
                "month",
                locale.short_months[date.month0() as usize].clone(),
                PickerPanelMode::Month,
                c,
            );
            let year = self.header_label(
                "year",
                format!("{}{}", date.year(), locale.year_suffix).into(),
                PickerPanelMode::Year,
                c,
            );
            label = if locale.month_before_year {
                label.child(month).child(year)
            } else {
                label.child(year).child(month)
            };
        } else {
            let text = match mode {
                PickerPanelMode::Year => {
                    let base = date.year().div_euclid(10) * 10;
                    format!("{}-{}", base, base + 9)
                }
                PickerPanelMode::Decade => {
                    let base = date.year().div_euclid(100) * 100;
                    format!("{}-{}", base, base + 99)
                }
                _ => format!("{}{}", date.year(), locale.year_suffix),
            };
            let next = match mode {
                PickerPanelMode::Month | PickerPanelMode::Quarter => PickerPanelMode::Year,
                _ => PickerPanelMode::Decade,
            };
            label = label.child(self.header_label("period", text.into(), next, c));
        }
        header = header.child(label);
        if mode == PickerPanelMode::Date {
            header = header.child(self.nav_button(
                "next",
                (date, right_panel, 1, false),
                next,
                locale.next_month.clone(),
                w,
                c,
            ));
        }
        let next_label = match mode {
            PickerPanelMode::Date | PickerPanelMode::Month | PickerPanelMode::Quarter => {
                locale.next_year.clone()
            }
            PickerPanelMode::Year => locale.next_decade.clone(),
            PickerPanelMode::Decade => locale.next_century.clone(),
            _ => locale.next_year.clone(),
        };
        header = header.child(self.nav_button(
            "super-next",
            (date, right_panel, 1, true),
            next,
            next_label,
            w,
            c,
        ));
        let measure = self.state.clone();
        let header_border = w.pixel_snap(px(g.line_width as f32));
        header = header.child(measure_element(move |bounds, c| {
            measure.update(c, |s, _| {
                while s.geometry.headers.len() <= panel {
                    s.geometry.headers.push(bounds);
                }
                s.geometry.headers[panel] = Bounds::new(
                    bounds.origin,
                    gpui::size(bounds.size.width, bounds.size.height + header_border),
                );
            })
        }));
        header.style().refine(&self.styles.header);
        header.into_any_element()
    }
    fn header_label(
        &self,
        id: &'static str,
        text: SharedString,
        mode: PickerPanelMode,
        c: &App,
    ) -> AnyElement {
        let target = self.state.clone();
        let color = self.tokens.globals().color_primary;
        let config = self.scope.snapshot(c);
        let label = match mode {
            PickerPanelMode::Month => config.locale.picker.choose_month.clone(),
            PickerPanelMode::Decade => config.locale.picker.choose_decade.clone(),
            _ => config.locale.picker.choose_year.clone(),
        };
        PickerAction::new(id, label, text.into_any_element(), move |w, c| {
            target.update(c, |s, c| s.change_panel(mode, w, c))
        })
        .scope(self.scope.clone())
        .disabled(self.state.read(c).disabled)
        .hover_style(div().text_color(color).style().clone())
        .into_any_element()
    }
    fn nav_button(
        &self,
        id: &'static str,
        navigation: (NaiveDate, bool, i32, bool),
        visible: bool,
        label: SharedString,
        w: &mut Window,
        c: &mut App,
    ) -> AnyElement {
        let target = self.state.clone();
        let (origin, right_panel, amount, large) = navigation;
        let disabled = !visible
            || self.state.read(c).disabled
            || !self.state.read(c).can_navigate_from(origin, amount, large);
        let (custom, name) = match (amount < 0, large) {
            (true, false) => (&self.navigation_icons.previous, IconName::LeftOutlined),
            (false, false) => (&self.navigation_icons.next, IconName::RightOutlined),
            (true, true) => (
                &self.navigation_icons.super_previous,
                IconName::DoubleLeftOutlined,
            ),
            (false, true) => (
                &self.navigation_icons.super_next,
                IconName::DoubleRightOutlined,
            ),
        };
        let content = custom.as_ref().map_or_else(
            || icon(name).inherit_color().size(px(12.)).into_any_element(),
            |factory| factory(w, c),
        );
        let g = self.tokens.globals();
        let semantics = Accessibility {
            hidden: !visible,
            ..Default::default()
        };
        semantics
            .apply(
                PickerAction::new(id, label, content, move |w, c| {
                    target.update(c, |s, c| {
                        s.navigate_from(origin, right_panel, (amount, large), w, c)
                    })
                })
                .scope(self.scope.clone())
                .disabled(disabled)
                .flex_none()
                .flex()
                .items_center()
                .justify_center()
                .w(px(g.control_height_sm as f32))
                .h(px(g.control_height_lg as f32))
                .text_color(g.color_icon)
                .opacity(if !visible {
                    0.
                } else if disabled {
                    0.25
                } else {
                    1.
                })
                .hover_style(div().text_color(g.color_icon_hover).style().clone()),
            )
            .into_any_element()
    }
    /// Hover is a presentation value. It never enters the draft or accepted selection.
    fn displayed_range(&self, c: &App) -> [Option<NaiveDate>; 2] {
        let s = self.state.read(c);
        let mut values = [
            s.draft.first().copied().flatten().map(|v| v.date()),
            s.draft.get(1).copied().flatten().map(|v| v.date()),
        ];
        if S::RANGE
            && s.options.preview_value
            && values[1 - s.active].is_some()
            && let Some(hover) = s.hover
        {
            values[s.active] = Some(hover.value.date());
        }
        match values {
            [Some(a), Some(b)] if a > b => [Some(b), Some(a)],
            [Some(a), None] | [None, Some(a)] => [Some(a), Some(a)],
            _ => values,
        }
    }
    fn week_number_cell(&self, date: NaiveDate, panel: usize, c: &App) -> AnyElement {
        let s = self.state.read(c);
        let g = self.tokens.globals();
        let rules = s.format_context.week_rules;
        let (_, week) = calendar::week_year_number(date, rules);
        let text = s.numeric_label(week, 0);
        let picker = s.options.mode;
        let active = picker == PickerMode::Week;
        let first_selected = active
            && s.draft
                .iter()
                .flatten()
                .take(if S::MULTIPLE { 1 } else { usize::MAX })
                .any(|v| calendar::same_period(date, v.date(), PickerMode::Week, rules));
        let selected = active
            && s.draft
                .iter()
                .flatten()
                .any(|v| calendar::same_period(date, v.date(), PickerMode::Week, rules));
        let [first, last] = self.displayed_range(c);
        let range_start = active
            && S::RANGE
            && first.is_some_and(|v| calendar::same_period(date, v, PickerMode::Week, rules));
        let range_end = active
            && S::RANGE
            && last.is_some_and(|v| calendar::same_period(date, v, PickerMode::Week, rules));
        let hovered = active
            && (selected
                || s.hover.is_some_and(|v| {
                    calendar::same_period(date, v.value.date(), PickerMode::Week, rules)
                }));
        let range_hover = active
            && S::RANGE
            && (first.zip(last).is_some_and(|(a, b)| {
                let start = |d| calendar::start_of_week(d, rules).unwrap_or(d);
                start(date) > start(a).min(start(b)) && start(date) < start(a).max(start(b))
            }));
        let primary = if S::RANGE {
            range_start || range_end
        } else {
            selected || hovered
        };
        let key: SharedString = format!("{panel}:week:{date}").into();
        let inner_key = key.clone();
        let outer_key = key.clone();
        let measure = self.state.clone();
        let outer_measure = self.state.clone();
        let target = self.state.clone();
        let mut outer = div()
            .id(key)
            .relative()
            .flex_1()
            .h_full()
            .flex()
            .items_center()
            .justify_center()
            .role(Role::GridCell)
            .aria_label(text.clone())
            .aria_selected(selected);
        if range_hover && !primary {
            outer = outer.child(
                div()
                    .absolute()
                    .left_0()
                    .right_0()
                    .h(px(self.tokens.cell_height as f32))
                    .bg(self.tokens.cell_active_with_range_bg),
            );
        }
        if primary {
            outer = outer.child(
                div()
                    .absolute()
                    .left_0()
                    .right_0()
                    .h(px(self.tokens.cell_height as f32))
                    .bg(g.color_primary)
                    .rounded_l(px(g.border_radius_sm as f32)),
            );
        }
        let metric_text = text.clone();
        let inner = div()
            .relative()
            .size(px(self.tokens.cell_height as f32))
            .flex()
            .items_center()
            .justify_center()
            .text_color(if primary {
                g.color_text_light_solid.with_alpha(0.5)
            } else {
                g.color_text_disabled
            })
            .child(text)
            .child(measure_element(move |bounds, c| {
                measure.update(c, |s, _| {
                    s.geometry.cells.insert(
                        inner_key.clone(),
                        PickerCellGeometry {
                            key: inner_key.clone(),
                            date: None,
                            unit: None,
                            value: None,
                            bounds,
                            inner: bounds,
                            selected: false,
                            today: false,
                            disabled: false,
                            in_range: false,
                            range_start: false,
                            range_end: false,
                            range_hover: false,
                            text: metric_text.clone(),
                            in_view: false,
                            panel,
                            week_selected: first_selected && !S::RANGE,
                            week_range_start: range_start,
                            week_range_end: range_end,
                            week_range_hover: range_hover,
                            week_hover: hovered,
                        },
                    );
                })
            }));
        let pointer = target.clone();
        let accessible = target.clone();
        outer
            .child(inner)
            .on_mouse_down(MouseButton::Left, move |_, w, c| {
                target.update(c, |s, c| {
                    if !s.disabled && !s.disabled_day(date) {
                        s.panel_focus.focus(w, c);
                    }
                });
                w.prevent_default();
            })
            .on_click(move |_, w, c| {
                pointer.update(c, |s, c| {
                    s.select_date(date, PickerChangeSource::Calendar, w, c)
                });
                c.stop_propagation();
            })
            .on_a11y_action(accesskit::Action::Click, move |_, w, c| {
                accessible.update(c, |s, c| {
                    s.select_date(date, PickerChangeSource::Accessibility, w, c)
                });
            })
            .child(measure_element(move |bounds, c| {
                outer_measure.update(c, |s, _| {
                    if let Some(cell) = s.geometry.cells.get_mut(&outer_key) {
                        cell.bounds = bounds;
                    }
                })
            }))
            .into_any_element()
    }
    fn date_cell(
        &self,
        cell: &CalendarCell,
        panel: usize,
        w: &mut Window,
        c: &mut App,
    ) -> AnyElement {
        let config = self.scope.snapshot(c);
        let g = self.tokens.globals();
        let s = self.state.read(c);
        let mode = s.panel_mode;
        let picker = s.options.mode;
        let today = self.today;
        let endpoint = s.active_endpoint();
        let rules = s.format_context.week_rules;
        let first_selected_week = picker == PickerMode::Week
            && !S::RANGE
            && s.draft
                .first()
                .copied()
                .flatten()
                .is_some_and(|v| calendar::same_period(cell.date, v.date(), picker, rules));
        let value_selected = (cell.in_view || picker == PickerMode::Week)
            && s.draft.iter().flatten().any(|v| {
                if mode == s.target_panel() {
                    calendar::same_period(cell.date, v.date(), picker, rules)
                } else {
                    cell.contains(v.date())
                }
            });
        let disabled = s.disabled_cell(cell);
        let cursor_active =
            cell.in_view && cell.contains(s.cursor.date()) && s.panel_focus.is_focused(w);
        let panel_focus = s.panel_focus.clone();
        let is_today = mode == PickerPanelMode::Date && cell.contains(today);
        let [first, last] = self.displayed_range(c);
        let range_start = S::RANGE
            && cell.in_view
            && first.is_some_and(|d| calendar::same_period(cell.date, d, picker, rules));
        let range_end = S::RANGE
            && cell.in_view
            && last.is_some_and(|d| calendar::same_period(cell.date, d, picker, rules));
        let period = |date| calendar::period_bounds(date, picker, rules).map_or(date, |p| p.0);
        let in_range = S::RANGE
            && cell.in_view
            && first.zip(last).is_some_and(|(a, b)| {
                period(cell.date) > period(a).min(period(b))
                    && period(cell.date) < period(a).max(period(b))
            });
        let hovered = s
            .hover
            .is_some_and(|h| calendar::same_period(cell.date, h.value.date(), picker, rules));
        let selected = if S::RANGE {
            range_start || range_end
        } else {
            value_selected
        };
        let week_hover = picker == PickerMode::Week && (hovered || (!S::RANGE && value_selected));
        let week_primary = picker == PickerMode::Week && (selected || week_hover);
        let text = match mode {
            PickerPanelMode::Date => s.numeric_label(cell.date.day(), 0),
            PickerPanelMode::Month => {
                config.locale.picker.short_months[cell.date.month0() as usize].clone()
            }
            PickerPanelMode::Quarter => s.quarter_label(cell.date.month0() / 3 + 1),
            PickerPanelMode::Year => s.numeric_label(cell.date.year(), 0),
            PickerPanelMode::Decade => {
                s.decade_label(cell.period_start.year(), cell.period_end.year())
            }
            PickerPanelMode::Time => SharedString::default(),
        };
        let key: SharedString = format!("{}:{}", panel, cell.date).into();
        let date = cell.date;
        let cell_in_view = cell.in_view;
        let width = if mode == PickerPanelMode::Date {
            self.tokens.cell_height
        } else if mode == PickerPanelMode::Decade {
            let run = gpui::TextRun {
                len: text.len(),
                font: config.font.clone(),
                color: g.color_text.to_hsla(),
                background_color: None,
                underline: None,
                strikethrough: None,
            };
            f64::from(
                w.text_system()
                    .shape_line(text.clone(), px(g.font_size as f32), &[run], None)
                    .width,
            ) + g.padding_xs
        } else {
            self.tokens.year_month_cell_width()
        };
        let color = if (selected || week_primary) && !disabled {
            g.color_text_light_solid
        } else if disabled || !cell.in_view {
            g.color_text_disabled
        } else {
            g.color_text
        };
        let target = self.state.clone();
        let pointer = target.clone();
        let accessible = target.clone();
        let hover_target = self.state.clone();
        let measure = self.state.clone();
        let outer_measure = self.state.clone();
        let inner_key = key.clone();
        let outer_key = key.clone();
        let context = PickerCellContext {
            date: Some(date),
            unit: None,
            value: None,
            mode,
            endpoint,
            today,
            selected,
            disabled,
            in_range,
            hovered,
        };
        let default = div()
            .flex()
            .items_center()
            .justify_center()
            .size_full()
            .child(text.clone())
            .into_any_element();
        let content = if let Some(render) = &self.cell_render {
            render(&context, default, w, c)
        } else {
            default
        };
        let inner_height = w.pixel_snap(px(self.tokens.cell_height as f32));
        let mut inner = div()
            .id("inner")
            .relative()
            .flex_none()
            .min_w_0()
            .h(inner_height)
            .when(mode == PickerPanelMode::Decade, |d| {
                d.aspect_ratio(width as f32 / f32::from(inner_height).max(1.))
            })
            .when(mode != PickerPanelMode::Decade, |d| d.w(px(width as f32)))
            .rounded(px(g.border_radius_sm as f32))
            .when(range_start && !range_end, |d| {
                if config.direction == Direction::Rtl {
                    d.rounded_l(px(0.))
                } else {
                    d.rounded_r(px(0.))
                }
            })
            .when(range_end && !range_start, |d| {
                if config.direction == Direction::Rtl {
                    d.rounded_r(px(0.))
                } else {
                    d.rounded_l(px(0.))
                }
            })
            .text_color(color)
            .bg(if selected && !disabled && picker != PickerMode::Week {
                g.color_primary
            } else if selected && disabled {
                g.color_fill_secondary
            } else {
                crate::theme::Color::TRANSPARENT
            })
            .when(!disabled && !selected, |d| {
                d.hover(|d| d.bg(self.tokens.cell_hover_bg))
            })
            .when(is_today && cell.in_view, |d| {
                d.child(
                    div()
                        .absolute()
                        .inset_0()
                        .border(px(g.line_width as f32))
                        .border_color(if disabled {
                            g.color_text_disabled
                        } else {
                            g.color_primary
                        })
                        .rounded(px(g.border_radius_sm as f32)),
                )
            })
            .child(content);
        inner.style().refine(&self.styles.item);
        let inner_text = text.clone();
        inner = inner.child(measure_element(move |bounds, c| {
            measure.update(c, |s, _| {
                s.geometry.cells.insert(
                    inner_key.clone(),
                    PickerCellGeometry {
                        key: inner_key.clone(),
                        date: Some(date),
                        unit: None,
                        value: None,
                        bounds,
                        inner: bounds,
                        selected: selected && !S::RANGE && picker != PickerMode::Week,
                        today: is_today,
                        disabled,
                        in_range: in_range && picker != PickerMode::Week,
                        range_start: range_start && picker != PickerMode::Week,
                        range_end: range_end && picker != PickerMode::Week,
                        range_hover: false,
                        text: inner_text.clone(),
                        in_view: cell_in_view,
                        panel,
                        week_selected: first_selected_week,
                        week_range_start: picker == PickerMode::Week && range_start,
                        week_range_end: picker == PickerMode::Week && range_end,
                        week_range_hover: picker == PickerMode::Week
                            && in_range
                            && !range_start
                            && !range_end,
                        week_hover,
                    },
                );
            })
        }));
        let mut outer = div()
            .id(key)
            .relative()
            .flex_1()
            .h_full()
            .flex()
            .items_center()
            .justify_center()
            .role(Role::GridCell)
            .aria_label(format!("{}", date))
            .aria_selected(selected)
            .when(cursor_active, |d| d.aria_active_descendant())
            .when(!disabled, |d| d.cursor_pointer())
            .on_mouse_down(MouseButton::Left, move |_, w, c| {
                target.update(c, |s, c| {
                    if !disabled && !s.disabled {
                        s.panel_focus.focus(w, c);
                    }
                });
                w.prevent_default();
            })
            .on_click(move |_, w, c| {
                pointer.update(c, |s, c| {
                    s.select_date(date, PickerChangeSource::Calendar, w, c)
                });
                c.stop_propagation();
            })
            .on_a11y_action(accesskit::Action::Click, move |_, w, c| {
                accessible.update(c, |s, c| {
                    s.select_date(date, PickerChangeSource::Accessibility, w, c)
                });
            })
            .on_hover(move |hover, w, c| {
                hover_target.update(c, |s, c| s.hover_cell(panel, date, *hover, w, c))
            });
        if week_primary && !disabled {
            outer = outer.child(
                div()
                    .absolute()
                    .left_0()
                    .right_0()
                    .h(px(self.tokens.cell_height as f32))
                    .bg(g.color_primary),
            );
        }
        if !disabled && (in_range || (picker != PickerMode::Week && (range_start != range_end))) {
            outer = outer.child(
                div()
                    .absolute()
                    .left_0()
                    .right_0()
                    .when(range_start && !range_end, |d| {
                        if config.direction == Direction::Rtl {
                            d.right(relative(0.5))
                        } else {
                            d.left(relative(0.5))
                        }
                    })
                    .when(range_end && !range_start, |d| {
                        if config.direction == Direction::Rtl {
                            d.left(relative(0.5))
                        } else {
                            d.right(relative(0.5))
                        }
                    })
                    .h(px(self.tokens.cell_height as f32))
                    .bg(self.tokens.cell_active_with_range_bg),
            );
        }
        if disabled {
            outer = outer.child(
                div()
                    .absolute()
                    .left_0()
                    .right_0()
                    .h(px(self.tokens.cell_height as f32))
                    .bg(self.tokens.cell_bg_disabled),
            );
        }
        let semantics = Accessibility {
            disabled,
            current: is_today.then_some(accesskit::AriaCurrent::Date),
            ..Default::default()
        };
        let inner = if cursor_active {
            crate::behavior::FocusRing::new(g.color_primary_border, px(g.line_width_focus as f32))
                .radius(px(g.border_radius_sm as f32))
                .apply(inner, &panel_focus)
                .into_any_element()
        } else {
            inner.into_any_element()
        };
        outer = outer.child(inner).child(measure_element(move |bounds, c| {
            outer_measure.update(c, |s, _| {
                if let Some(cell) = s.geometry.cells.get_mut(&outer_key) {
                    cell.bounds = bounds;
                }
            })
        }));
        semantics.apply(outer).into_any_element()
    }
    fn time_viewport_height(&self) -> f32 {
        (if S::HAS_DATE {
            ((self.tokens.cell_height + self.tokens.cell_padding() * 2.) * 7.
                + 2. * self.tokens.globals().padding_xs
                - 8.) as f32
        } else {
            self.tokens.time_column_height as f32 - 8.
        })
        .max(0.)
    }

    pub fn time(&self, w: &mut Window, c: &mut App) -> AnyElement {
        let g = self.tokens.globals();
        let s = self.state.read(c);
        let units = s.options.time.units();
        let datetime = S::HAS_DATE;
        // Time units keep chronological left-to-right order. Each column owns
        // its RTL text, clipping and scrollbar placement independently.
        let column_count = units.len();
        let body_height = w.pixel_snap(px(self.time_viewport_height() + 8.));
        let body_width = self.tokens.time_column_width as f32 * column_count as f32;
        // Preserve the authored total width until flex layout. Pre-snapping an
        // odd number of compact columns would discard half a device pixel and
        // move the rail edges before their own rasterization.
        let mut body = div()
            .relative()
            .flex()
            .h(body_height)
            .aspect_ratio(body_width / f32::from(body_height).max(1.))
            .when(datetime, |d| d.w_full());
        for (index, unit) in units.into_iter().enumerate() {
            body = body.child(self.time_column(unit, index, column_count, w, c));
        }
        body = body.text_size(px(g.font_size as f32));
        let measure = self.state.clone();
        let index = usize::from(datetime);
        body = body.child(measure_element(move |bounds, c| {
            measure.update(c, |s, _| {
                while s.geometry.tables.len() <= index {
                    s.geometry.tables.push(bounds);
                }
                s.geometry.tables[index] = bounds;
            })
        }));
        if datetime {
            body.into_any_element()
        } else {
            div().pt(px(4.)).child(body).into_any_element()
        }
    }
    fn time_column(
        &self,
        unit: TimeUnit,
        index: usize,
        column_count: usize,
        w: &mut Window,
        c: &mut App,
    ) -> AnyElement {
        let config = self.scope.snapshot(c);
        let s = self.state.read(c);
        let value = s.draft.get(s.active).copied().flatten().unwrap_or(s.cursor);
        let time = value.time();
        let constraints = s.time_constraints(value).snapshot(unit, time);
        let selected = unit.value(time);
        let options = s.options.time.clone();
        let values = options.visible_values(unit, time, &constraints);
        let row_height = self.tokens.time_cell_height as f32;
        let height = self.time_viewport_height();
        if row_height <= 0. || height <= 0. {
            return div()
                .w(px(self.tokens.time_column_width as f32))
                .h(px(height))
                .into_any_element();
        }
        let handle = s.scrolls[index].clone();
        let focus = s.column_focus[index].clone();
        let has_value = s.draft.get(s.active).copied().flatten().is_some()
            || (!S::HAS_DATE && s.options.time.default_open_value.is_some());
        let initial = values.iter().position(|v| *v == selected).unwrap_or(0);
        self.state.update(c, |s, _| {
            if s.scroll_keys[index] != Some((unit, selected)) {
                s.scroll_keys[index] = Some((unit, selected));
                handle.scroll_to_item_strict(initial, ScrollStrategy::Top);
            }
        });
        let count = values.len();
        let gutter = if count > 1 { 10. } else { 0. };
        let direction = config.direction;
        let native_scroll = handle.0.borrow().base_handle.clone();
        let maximum = px(count.saturating_sub(1) as f32 * row_height);
        let rows = self.clone();
        let row_values = values.clone();
        let disabled = constraints;
        let label = match unit {
            TimeUnit::Hour => config.locale.picker.hour.clone(),
            TimeUnit::Minute => config.locale.picker.minute.clone(),
            TimeUnit::Second => config.locale.picker.second.clone(),
            TimeUnit::Millisecond => config.locale.picker.millisecond.clone(),
            TimeUnit::Meridiem => config.locale.picker.meridiem.clone(),
        };
        let key_target = self.state.clone();
        let key_values = values.clone();
        let wheel_target = self.state.clone();
        let mut list = uniform_list(
            ("time-list", index),
            count + (height / row_height).ceil() as usize - 1,
            move |range, w, c| {
                let g = rows.tokens.globals();
                range
                    .map(|row| {
                        if row >= count {
                            return div().h(px(row_height)).into_any_element();
                        }
                        let value = row_values[row];
                        let blocked = disabled.disabled(value);
                        let selected = has_value && value == selected;
                        let text: SharedString = match unit {
                            TimeUnit::Meridiem => {
                                if value == 0 {
                                    rows.scope.snapshot(c).locale.picker.am.clone()
                                } else {
                                    rows.scope.snapshot(c).locale.picker.pm.clone()
                                }
                            }
                            TimeUnit::Millisecond => rows.state.read(c).numeric_label(value, 3),
                            TimeUnit::Hour if options.use_12_hours => {
                                rows.state.read(c).numeric_label(
                                    if value.is_multiple_of(12) {
                                        12
                                    } else {
                                        value % 12
                                    },
                                    2,
                                )
                            }
                            _ => rows.state.read(c).numeric_label(value, 2),
                        };
                        let key: SharedString = format!("time:{index}:{value}").into();
                        let key_inner = key.clone();
                        let target = rows.state.clone();
                        let pointer = target.clone();
                        let accessible = target.clone();
                        let measure = rows.state.clone();
                        let outer = rows.state.clone();
                        let outer_key = key.clone();
                        let ctx = PickerCellContext {
                            date: None,
                            unit: Some(unit),
                            value: Some(value),
                            mode: PickerPanelMode::Time,
                            endpoint: rows.state.read(c).active_endpoint(),
                            today: rows.today,
                            selected,
                            disabled: blocked,
                            in_range: false,
                            hovered: false,
                        };
                        let default = text.clone().into_any_element();
                        let content = if let Some(render) = &rows.cell_render {
                            render(&ctx, default, w, c)
                        } else {
                            default
                        };
                        let direction = rows.scope.snapshot(c).direction;
                        let start_padding =
                            (rows.tokens.time_column_width - rows.tokens.time_cell_height) / 2.;
                        let mut inner = div()
                            .relative()
                            .flex_none()
                            .w(px(rows.tokens.time_column_width as f32 - 8.))
                            .h(px(row_height))
                            .when(direction == Direction::Ltr, |d| {
                                d.pl(px(start_padding as f32))
                            })
                            .when(direction == Direction::Rtl, |d| {
                                d.pr(px(start_padding as f32)).flex_row_reverse()
                            })
                            .flex()
                            .items_center()
                            .rounded(px(g.border_radius_sm as f32))
                            .text_color(if blocked {
                                g.color_text_disabled
                            } else {
                                g.color_text
                            })
                            .when(selected, |d| d.bg(g.control_item_bg_active))
                            .when(!blocked && !selected, |d| {
                                d.hover(|d| d.bg(rows.tokens.cell_hover_bg))
                            })
                            .child(content);
                        let metric_text = text.clone();
                        inner = inner.child(measure_element(move |bounds, c| {
                            measure.update(c, |s, _| {
                                s.geometry.cells.insert(
                                    key_inner.clone(),
                                    PickerCellGeometry {
                                        key: key_inner.clone(),
                                        date: None,
                                        unit: Some(unit),
                                        value: Some(value),
                                        bounds,
                                        inner: bounds,
                                        selected,
                                        today: false,
                                        disabled: blocked,
                                        in_range: false,
                                        range_start: false,
                                        range_end: false,
                                        range_hover: false,
                                        text: metric_text.clone(),
                                        in_view: true,
                                        panel: index,
                                        week_selected: false,
                                        week_range_start: false,
                                        week_range_end: false,
                                        week_range_hover: false,
                                        week_hover: false,
                                    },
                                );
                            })
                        }));
                        let row = div()
                            .id(key)
                            .relative()
                            .w_full()
                            .min_w_0()
                            .flex()
                            .when(direction == Direction::Rtl, |d| d.flex_row_reverse())
                            .h(px(row_height))
                            .px(px(4.))
                            .role(Role::ListBoxOption)
                            .aria_label(text)
                            .aria_selected(selected)
                            .when(!blocked, |d| d.cursor_pointer())
                            .on_mouse_down(MouseButton::Left, move |_, w, c| {
                                target.update(c, |s, c| {
                                    if !blocked && !s.disabled {
                                        s.column_focus[index].focus(w, c);
                                    }
                                });
                                w.prevent_default();
                            })
                            .on_click(move |_, w, c| {
                                pointer.update(c, |s, c| {
                                    s.select_time(unit, value, PickerChangeSource::Time, w, c)
                                });
                                c.stop_propagation();
                            })
                            .on_a11y_action(accesskit::Action::Click, move |_, w, c| {
                                accessible.update(c, |s, c| {
                                    s.select_time(
                                        unit,
                                        value,
                                        PickerChangeSource::Accessibility,
                                        w,
                                        c,
                                    )
                                });
                            })
                            .child(inner)
                            .child(measure_element(move |bounds, c| {
                                outer.update(c, |s, _| {
                                    if let Some(cell) = s.geometry.cells.get_mut(&outer_key) {
                                        cell.bounds = bounds;
                                    }
                                })
                            }));
                        let a11y = Accessibility {
                            disabled: blocked,
                            ..Default::default()
                        };
                        a11y.apply(row).into_any_element()
                    })
                    .collect::<Vec<_>>()
            },
        )
        .track_scroll(&handle)
        .w(px(self.tokens.time_column_width as f32 - gutter))
        .h(px(height))
        .when(index > 0, |list| {
            let list = if direction == Direction::Ltr {
                list.border_l(px(self.tokens.globals().line_width as f32))
            } else {
                list.border_r(px(self.tokens.globals().line_width as f32))
            };
            list.border_color(self.tokens.globals().color_split)
        })
        .on_scroll(move |_, w, c| {
            wheel_target.update(c, |s, c| {
                if s.options.time.change_on_scroll {
                    s.schedule_scroll(index, unit, row_height, w, c);
                }
                c.notify();
            })
        });
        list.style().refine(&self.styles.time_column);
        let mut list_style = gpui::Style::default();
        list_style.refine(list.style());
        let rem_size = w.rem_size();
        let viewport_border = list_style.border_widths.to_pixels(rem_size);
        let viewport_padding = list_style.padding;
        let list = TimeList::new(list, native_scroll.clone(), maximum);
        let measure = self.state.clone();
        let viewport_measure = self.state.clone();
        let viewport_handle = native_scroll.clone();
        let viewport = div()
            .relative()
            .w(px(self.tokens.time_column_width as f32 - gutter))
            .h(px(height))
            .when(direction == Direction::Rtl, |d| d.ml(px(gutter)))
            .child(list)
            .child(measure_element(move |_, c| {
                viewport_measure.update(c, |s, _| {
                    let bounds = viewport_handle.bounds();
                    // UniformList's handle retains the outer box; rows are laid
                    // out inside its authored border and padding. Report that
                    // content viewport, including at fractional display scales.
                    let padding = viewport_padding.to_pixels(bounds.size.into(), rem_size);
                    let bounds = Bounds::from_corners(
                        bounds.origin
                            + point(
                                viewport_border.left + padding.left,
                                viewport_border.top + padding.top,
                            ),
                        bounds.bottom_right()
                            - point(
                                viewport_border.right + padding.right,
                                viewport_border.bottom + padding.bottom,
                            ),
                    );
                    while s.geometry.time_viewports.len() <= index {
                        s.geometry.time_viewports.push(bounds);
                    }
                    s.geometry.time_viewports[index] = bounds;
                })
            }));
        let scrollbar_state = self.state.clone();
        let geometry_state = self.state.clone();
        let scrollbar = ExternalScrollbar::new("scrollbar", native_scroll)
            .scope(self.scope.clone())
            .metrics(height, height + f32::from(maximum))
            .disabled(self.state.read(c).disabled)
            .on_scroll(move |w, c| {
                scrollbar_state.update(c, |s, c| {
                    if s.options.time.change_on_scroll {
                        s.schedule_scroll(index, unit, row_height, w, c);
                    }
                    c.notify();
                })
            })
            .on_geometry(move |g: &ExternalScrollbarGeometry, c| {
                geometry_state.update(c, |s, _| {
                    if g.visible {
                        s.geometry.scrollbars.push(super::PickerScrollbarGeometry {
                            column: index,
                            rail: g.rail,
                            track: g.track,
                            thumb: g.thumb,
                            decrement: g.decrement,
                            increment: g.increment,
                            viewport: g.viewport,
                            content: g.content,
                            maximum: g.maximum,
                            offset: g.offset,
                        });
                    }
                })
            });
        div()
            .id(("time-column", index))
            .relative()
            .my(px(4.))
            .w(relative(1. / column_count.max(1) as f32))
            .flex_none()
            .track_focus(&focus)
            .role(Role::ListBox)
            .aria_label(label)
            .on_key_down(move |e, w, c| {
                let key = e.keystroke.key.as_str();
                if !matches!(
                    key,
                    "up" | "down"
                        | "left"
                        | "right"
                        | "home"
                        | "end"
                        | "pageup"
                        | "pagedown"
                        | "enter"
                ) {
                    return;
                }
                key_target.update(c, |s, c| {
                    if key == "left" || key == "right" {
                        let forward = key == "right";
                        let next = if forward {
                            index.checked_add(1)
                        } else {
                            index.checked_sub(1)
                        };
                        if let Some(next) = next.filter(|i| *i < s.options.time.units().len()) {
                            s.column_focus[next].focus(w, c);
                        }
                        return;
                    }
                    if key == "enter" {
                        s.submit(PickerChangeSource::Keyboard, w, c);
                        return;
                    }
                    let current = s.draft.get(s.active).copied().flatten().unwrap_or(s.cursor);
                    let disabled = s.time_constraints(current).snapshot(unit, current.time());
                    let active = key_values
                        .iter()
                        .position(|v| *v == unit.value(current.time()))
                        .unwrap_or(0) as i32;
                    let step = if matches!(key, "pageup" | "pagedown") {
                        10
                    } else {
                        1
                    };
                    let mut at = match key {
                        "home" => 0,
                        "end" => key_values.len() as i32 - 1,
                        "up" | "pageup" => active - step,
                        _ => active + step,
                    };
                    let direction = if matches!(key, "up" | "pageup" | "end") {
                        -1
                    } else {
                        1
                    };
                    while at >= 0 && (at as usize) < key_values.len() {
                        let v = key_values[at as usize];
                        if !disabled.disabled(v) {
                            s.select_time(unit, v, PickerChangeSource::Keyboard, w, c);
                            break;
                        }
                        at += direction;
                    }
                });
                w.prevent_default();
                c.stop_propagation();
            })
            .child(viewport)
            .child(scrollbar)
            .child(measure_element(move |bounds, c| {
                measure.update(c, |s, _| {
                    while s.geometry.time_columns.len() <= index {
                        s.geometry.time_columns.push(bounds);
                    }
                    s.geometry.time_columns[index] = bounds;
                })
            }))
            .into_any_element()
    }
    pub fn footer(&self, w: &mut Window, c: &mut App) -> AnyElement {
        let config = self.scope.snapshot(c);
        let g = self.tokens.globals();
        let footer_border = w.pixel_snap(px(g.line_width as f32));
        let s = self.state.read(c);
        let need_confirm = s.panel_mode == s.target_panel() && s.need_confirm();
        let can_commit = s.can_submit();
        let show_now = s.panel_mode == s.target_panel()
            && s.options
                .show_now
                .unwrap_or(!S::RANGE && !S::MULTIPLE && matches!(s.options.mode, PickerMode::Date));
        let mut footer = div()
            .id("footer")
            .relative()
            .flex()
            .flex_col()
            .border_t(px(g.line_width as f32))
            .border_color(g.color_split);
        if let Some(extra) = &self.footer {
            let separator = if need_confirm || show_now {
                g.line_width
            } else {
                0.
            };
            footer = footer.child(
                div()
                    .px(px(g.padding_sm as f32))
                    .min_h(px(
                        (self.tokens.text_height - 2. * g.line_width + separator).max(0.) as f32,
                    ))
                    .border_b(px(separator as f32))
                    .border_color(g.color_split)
                    .line_height(px(
                        (self.tokens.text_height - 2. * g.line_width).max(0.) as f32
                    ))
                    .flex()
                    .items_center()
                    .child(extra(w, c)),
            );
        }
        if need_confirm || show_now {
            let mut actions = div()
                .flex()
                .items_center()
                .justify_between()
                .px(px(g.padding_sm as f32))
                .h(px((self.tokens.footer_height() - g.line_width
                    + if need_confirm { 4. * g.line_width } else { 0. })
                    as f32))
                .when(show_now && !need_confirm && !S::HAS_TIME, |d| {
                    d.justify_center()
                })
                .when(config.direction == Direction::Rtl, |d| d.flex_row_reverse());
            if show_now {
                let target = self.state.clone();
                let label = if S::HAS_TIME {
                    config.locale.picker.now.clone()
                } else {
                    config.locale.picker.today.clone()
                };
                actions = actions.child(
                    PickerAction::new(
                        "now",
                        label.clone(),
                        label.into_any_element(),
                        move |w, c| target.update(c, |s, c| s.now(w, c)),
                    )
                    .scope(self.scope.clone())
                    .disabled(self.state.read(c).disabled)
                    .text_color(g.color_link)
                    .cursor_pointer(),
                );
            } else {
                actions = actions.child(div());
            }
            if need_confirm {
                let target = self.state.clone();
                let action_measure = self.state.clone();
                let action_label = config.locale.picker.ok.clone();
                let tokens = config.theme.component::<crate::button::ButtonTheme>();
                let button_globals = tokens.globals();
                let border = if button_globals.line_width > 0. {
                    w.pixel_snap(px(button_globals.line_width as f32))
                        .max(px(1. / w.scale_factor()))
                } else {
                    px(0.)
                };
                // Fit each authored content inset as a whole. Independent
                // padding/border rounding otherwise loses one pixel per edge.
                let padding = (w.pixel_snap(px((tokens.padding_inline_sm.max(0.)
                    + button_globals.line_width)
                    as f32))
                    - border)
                    .max(px(0.));
                let mut button = crate::Button::new("ok")
                    .scope(self.scope.clone())
                    .label(action_label.clone())
                    .button_type(crate::button::ButtonType::Primary)
                    .size(Size::Small)
                    .px(padding)
                    .disabled(!can_commit)
                    .on_click(move |_, w, c| {
                        target.update(c, |s, c| {
                            s.submit(PickerChangeSource::Calendar, w, c);
                        });
                    });
                // Preserve the natural label advance instead of GPUI Text's
                // logical-pixel ceiling. Keep Button's special CJK spacing.
                let mut characters = action_label.chars();
                let two_chinese = matches!(
                    (characters.next(), characters.next(), characters.next()),
                    (Some(a), Some(b), None)
                        if ('\u{4e00}'..='\u{9fa5}').contains(&a)
                            && ('\u{4e00}'..='\u{9fa5}').contains(&b)
                );
                if !two_chinese {
                    let mut font = tokens.font().clone();
                    font.weight = gpui::FontWeight(tokens.font_weight as f32);
                    let run = gpui::TextRun {
                        len: action_label.len(),
                        font,
                        color: g.color_text.to_hsla(),
                        background_color: None,
                        underline: None,
                        strikethrough: None,
                    };
                    let width = w
                        .text_system()
                        .shape_line(
                            action_label.clone(),
                            px(tokens.content_font_size_sm as f32),
                            &[run],
                            None,
                        )
                        .width;
                    let height = w.pixel_snap(px((tokens.content_font_size_sm
                        * button_globals.line_height)
                        as f32));
                    if width > px(0.) && height > px(0.) {
                        button = button.content_style(
                            div()
                                .min_w_0()
                                .h(height)
                                .aspect_ratio(width / height)
                                .style()
                                .clone(),
                        );
                    }
                }
                actions = actions.child(div().relative().flex_none().child(button).child(
                    measure_element(move |bounds, c| {
                        action_measure.update(c, |s, _| {
                            s.geometry
                                .footer_actions
                                .push(super::PickerFooterActionGeometry {
                                    key: "ok".into(),
                                    label: action_label.clone(),
                                    bounds,
                                })
                        })
                    }),
                ));
            }
            footer = footer.child(actions);
        }
        let measure = self.state.clone();
        footer = footer.child(measure_element(move |bounds, c| {
            measure.update(c, |s, _| {
                s.geometry.footer = Some(Bounds::new(
                    bounds.origin - point(px(0.), footer_border),
                    gpui::size(bounds.size.width, bounds.size.height + footer_border),
                ))
            })
        }));
        footer.style().refine(&self.styles.footer);
        footer.into_any_element()
    }
    pub fn presets(&self, _w: &mut Window, c: &mut App) -> AnyElement {
        let config = self.scope.snapshot(c);
        let g = self.tokens.globals();
        let mut presets = div()
            .id("presets")
            .relative()
            .flex()
            .flex_col()
            .min_w(px(self.tokens.presets_width as f32))
            .max_w(px(self.tokens.presets_max_width as f32))
            .p(px(g.padding_xs as f32))
            .gap(px(g.padding_xxs as f32))
            .when(config.direction == Direction::Rtl, |d| {
                d.border_l(px(g.line_width as f32))
            })
            .when(config.direction == Direction::Ltr, |d| {
                d.border_r(px(g.line_width as f32))
            })
            .border_color(g.color_split);
        for preset in self.presets.iter() {
            let target = self.state.clone();
            let value = preset.value.clone();
            let factory = preset.factory.clone();
            presets = presets.child(
                PickerAction::new(
                    preset.key.clone(),
                    preset.label.clone(),
                    preset.label.clone().into_any_element(),
                    move |w, c| {
                        target.update(c, |s, c| {
                            s.preset(
                                factory.as_ref().map_or_else(|| value.clone(), |f| f()),
                                w,
                                c,
                            )
                        })
                    },
                )
                .scope(self.scope.clone())
                .disabled(self.state.read(c).disabled)
                .px(px(g.padding_xs as f32))
                .py(px(g.padding_xxs as f32))
                .rounded(px(g.border_radius_sm as f32))
                .cursor_pointer()
                .hover_style(div().bg(self.tokens.cell_hover_bg).style().clone()),
            );
        }
        let measure = self.state.clone();
        presets = presets.child(measure_element(move |bounds, c| {
            measure.update(c, |s, _| s.geometry.presets = Some(bounds))
        }));
        presets.style().refine(&self.styles.presets);
        presets.into_any_element()
    }
}
