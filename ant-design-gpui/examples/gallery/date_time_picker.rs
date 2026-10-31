// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Examples adapted from Ant Design 6.6.7 (MIT).

use std::{collections::BTreeMap, sync::Arc};

use ant_design_gpui::{
    Button, ConfigPatch, ConfigScope, Size, Tag, ValidationStatus, Variant,
    button::ButtonType,
    date_picker::{
        DatePicker, DatePickerPanel, DateRangeSelection, DateSelection, DateTimePicker,
        DateTimeRangePicker, DateTimeRangeSelection, DateTimeSelection, MultipleDatePicker,
        MultipleDateSelection, NaiveDate, NaiveTime, Picker, PickerChange, PickerFormat,
        PickerMode, PickerPanel, PickerPreset, PickerRange, PickerSelection, PickerState,
        PickerStyles, RangePicker, TimeOptions, TimePicker, TimeRangePicker, TimeRangeSelection,
        TimeSelection,
    },
    icons::{IconName, icon},
    locale::{Locale, LocalePatch},
    popup::PopupPlacement,
};
use chrono::{Datelike, Duration};
use gpui::{
    AnyElement, AppContext, Context, Entity, IntoElement, Render, ScrollAnchor, StyleRefinement,
    Subscription, Window, div, prelude::*, px,
};

use super::input_examples::{self as examples, Coverage, Demo, column, row};

const fn demo(id: &'static str, title: &'static str, description: &'static str) -> Demo {
    Demo {
        id,
        title,
        description,
        coverage: Coverage::Complete,
    }
}

pub(super) const DATE_DEMOS: &[Demo] = &[
    demo(
        "date-basic",
        "Basic",
        "Select a date, week, month, quarter or year.",
    ),
    demo(
        "date-range",
        "Range Picker",
        "Select two dates, date-times or complete calendar periods.",
    ),
    demo(
        "date-multiple",
        "Multiple",
        "Select several dates; selected dates appear as removable tags.",
    ),
    demo(
        "date-confirm",
        "Need Confirm",
        "Keep a draft selection until OK, or accept a date immediately.",
    ),
    demo(
        "date-switchable",
        "Switchable picker",
        "Choose the calendar granularity before selecting a value.",
    ),
    demo(
        "date-format",
        "Date Format",
        "Display and parse dates with Rust chrono format patterns.",
    ),
    demo(
        "date-time",
        "Choose Time",
        "Combine the calendar and time columns with explicit confirmation.",
    ),
    demo(
        "date-mask",
        "Mask Format",
        "Edit date segments with the arrow keys and numeric input.",
    ),
    demo(
        "date-limit",
        "Limit Date Range",
        "Keep selection and panel navigation within the allowed dates.",
    ),
    demo(
        "date-disabled",
        "Disabled",
        "Disable the entire control or one range endpoint.",
    ),
    demo(
        "date-disabled-date",
        "Disabled Date & Time",
        "Restrict dates and cascading time choices.",
    ),
    demo(
        "date-allow-empty",
        "Allow Empty",
        "Keep the end of a range open.",
    ),
    demo(
        "date-select-range",
        "Select range dates",
        "Select a short range relative to the first chosen date.",
    ),
    demo(
        "date-presets",
        "Preset Ranges",
        "Choose a named date or complete range.",
    ),
    demo(
        "date-footer",
        "Extra Footer",
        "Add supplementary content beneath the calendar.",
    ),
    demo(
        "date-size",
        "Three Sizes",
        "Small, medium and large inputs share the same calendar.",
    ),
    demo(
        "date-cell-render",
        "Customized Cell Rendering",
        "Decorate calendar cells with native content.",
    ),
    demo(
        "date-components",
        "Customize Panel",
        "Compose the calendar with native panel content.",
    ),
    demo(
        "date-external-panel",
        "External use panel",
        "Embed the same calendar directly in a view.",
    ),
    demo(
        "date-buddhist",
        "Buddhist Era",
        "Format displayed years with a custom native formatter.",
    ),
    demo(
        "date-status",
        "Status",
        "Show validation errors and warnings.",
    ),
    demo(
        "date-variant",
        "Variants",
        "Outlined, filled, borderless and underlined triggers.",
    ),
    demo(
        "date-style",
        "Custom semantic styling",
        "Refine the input and popup semantic slots.",
    ),
    demo(
        "date-placement",
        "Placement",
        "Open the calendar above or below either input edge.",
    ),
    demo(
        "date-suffix",
        "Prefix and Suffix",
        "Add a prefix and replace the calendar suffix icon.",
    ),
    demo(
        "date-controlled",
        "Controlled value",
        "Keep the selected date in the parent view.",
    ),
    demo(
        "date-locale",
        "Localization",
        "Switch calendar labels and week rules independently of layout direction.",
    ),
];

pub(super) const TIME_DEMOS: &[Demo] = &[
    demo(
        "time-basic",
        "Basic",
        "Select or enter an hour, minute and second.",
    ),
    demo(
        "time-value",
        "Under Control",
        "Keep the time value in the parent view.",
    ),
    demo(
        "time-size",
        "Three Sizes",
        "Small, medium and large time inputs.",
    ),
    demo(
        "time-confirm",
        "Need Confirm",
        "Confirm explicitly or commit when leaving the picker.",
    ),
    demo(
        "time-disabled",
        "Disabled",
        "A disabled input preserves its selected time.",
    ),
    demo(
        "time-hide-column",
        "Hour and minute",
        "Hide seconds when minute precision is enough.",
    ),
    demo(
        "time-interval",
        "Interval option",
        "Choose hours, minutes and seconds at configured intervals.",
    ),
    demo(
        "time-addon",
        "Addon",
        "Add native footer content beneath the time columns.",
    ),
    demo(
        "time-12hours",
        "12 hours",
        "Choose AM or PM with a zero-padded 12-hour clock.",
    ),
    demo(
        "time-scroll",
        "Change on scroll",
        "Select an enabled time option after scrolling settles.",
    ),
    demo(
        "time-range",
        "Time Range Picker",
        "Select and confirm the start and end times.",
    ),
    demo(
        "time-variant",
        "Variants",
        "Outlined, filled, borderless and underlined time inputs.",
    ),
    demo(
        "time-status",
        "Status",
        "Show error and warning validation states.",
    ),
    demo(
        "time-suffix",
        "Prefix and Suffix",
        "Add a prefix or replace the clock suffix icon.",
    ),
    demo(
        "time-style",
        "Custom semantic styling",
        "Refine the input and popup semantic slots.",
    ),
    demo(
        "time-millisecond",
        "Milliseconds",
        "Select milliseconds in a fourth time column.",
    ),
    demo(
        "time-disabled-options",
        "Disabled options",
        "Restrict hours and hide unavailable choices.",
    ),
    demo(
        "time-external-panel",
        "External use panel",
        "Embed time columns directly in a native view.",
    ),
];

type States<S> = BTreeMap<&'static str, Entity<PickerState<S>>>;

pub(super) struct PickerPage {
    pub time_page: bool,
    scope: ConfigScope,
    chinese_scope: ConfigScope,
    traditional_scope: ConfigScope,
    pub dates: States<DateSelection>,
    pub date_times: States<DateTimeSelection>,
    pub multiple: States<MultipleDateSelection>,
    pub ranges: States<DateRangeSelection>,
    pub date_time_ranges: States<DateTimeRangeSelection>,
    pub times: States<TimeSelection>,
    pub time_ranges: States<TimeRangeSelection>,
    pub controlled_date: Option<NaiveDate>,
    pub controlled_time: Option<NaiveTime>,
    pub switch_mode: PickerMode,
    anchors: Vec<ScrollAnchor>,
    _config: Subscription,
    _states: Vec<Subscription>,
}

fn date(day: u32) -> NaiveDate {
    NaiveDate::from_ymd_opt(2026, 10, day).unwrap()
}
fn time(hour: u32, minute: u32, second: u32) -> NaiveTime {
    NaiveTime::from_hms_opt(hour, minute, second).unwrap()
}
fn pattern(value: &'static str) -> PickerFormat {
    PickerFormat::new(value).expect("valid demo format")
}

fn make_states<S: PickerSelection>(
    keys: &[&'static str],
    window: &mut Window,
    cx: &mut Context<PickerPage>,
) -> States<S> {
    keys.iter()
        .map(|&key| (key, cx.new(|cx| PickerState::new(window, cx))))
        .collect()
}

#[inline(never)]
fn picker_locale_scope(
    scope: &ConfigScope,
    locale: Arc<Locale>,
    cx: &mut gpui::App,
) -> ConfigScope {
    scope
        .child(
            ConfigPatch {
                locale: LocalePatch {
                    locale: Some(locale),
                    ..Default::default()
                },
                ..Default::default()
            },
            cx,
        )
        .unwrap()
}

impl PickerPage {
    pub fn new(
        time_page: bool,
        scope: ConfigScope,
        anchors: Vec<ScrollAnchor>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        let dates = make_states(
            if time_page {
                &[]
            } else {
                &[
                    "date-basic",
                    "date-basic.week",
                    "date-basic.month",
                    "date-basic.quarter",
                    "date-basic.year",
                    "date-confirm",
                    "date-confirm.auto",
                    "date-switchable",
                    "date-format",
                    "date-format.alt",
                    "date-mask",
                    "date-limit",
                    "date-disabled",
                    "date-disabled-date",
                    "date-footer",
                    "date-size",
                    "date-size.small",
                    "date-size.large",
                    "date-cell-render",
                    "date-components",
                    "date-external-panel",
                    "date-buddhist",
                    "date-status",
                    "date-status.warning",
                    "date-variant",
                    "date-variant.filled",
                    "date-variant.borderless",
                    "date-variant.underlined",
                    "date-style",
                    "date-style.large",
                    "date-placement",
                    "date-placement.bottom-right",
                    "date-placement.top-left",
                    "date-placement.top-right",
                    "date-suffix",
                    "date-suffix.custom",
                    "date-controlled",
                    "date-locale",
                    "date-locale.cn",
                    "date-locale.tw",
                    "date-presets.single",
                ]
            },
            window,
            cx,
        );
        let date_times = make_states(
            if time_page {
                &[]
            } else {
                &["date-time", "date-disabled-time"]
            },
            window,
            cx,
        );
        let multiple = make_states(
            if time_page {
                &[]
            } else {
                &[
                    "date-multiple",
                    "date-multiple.week",
                    "date-multiple.month",
                    "date-multiple.quarter",
                    "date-multiple.year",
                    "date-multiple.custom",
                ]
            },
            window,
            cx,
        );
        let ranges = make_states(
            if time_page {
                &[]
            } else {
                &[
                    "date-range",
                    "date-range.week",
                    "date-range.month",
                    "date-range.quarter",
                    "date-range.year",
                    "date-disabled.range",
                    "date-allow-empty",
                    "date-select-range",
                    "date-presets",
                ]
            },
            window,
            cx,
        );
        let date_time_ranges = make_states(
            if time_page {
                &[]
            } else {
                &["date-range.time", "date-time.range"]
            },
            window,
            cx,
        );
        let times = make_states(
            if time_page {
                &[
                    "time-basic",
                    "time-value",
                    "time-size",
                    "time-size.small",
                    "time-size.large",
                    "time-confirm",
                    "time-disabled",
                    "time-hide-column",
                    "time-interval",
                    "time-addon",
                    "time-12hours",
                    "time-12hours.minute",
                    "time-scroll",
                    "time-variant",
                    "time-variant.filled",
                    "time-variant.borderless",
                    "time-variant.underlined",
                    "time-status",
                    "time-status.warning",
                    "time-suffix",
                    "time-suffix.custom",
                    "time-style",
                    "time-style.large",
                    "time-millisecond",
                    "time-disabled-options",
                    "time-external-panel",
                ]
            } else {
                &[]
            },
            window,
            cx,
        );
        let time_ranges = make_states(
            if time_page {
                &["time-range", "time-range.unordered"]
            } else {
                &[]
            },
            window,
            cx,
        );
        let mut observers = Vec::new();
        macro_rules! observe { ($($states:ident),+) => { $(
            observers.extend($states.values().map(|state| cx.observe(state, |_, _, cx| cx.notify())));
        )+ }; }
        observe!(
            dates,
            date_times,
            multiple,
            ranges,
            date_time_ranges,
            times,
            time_ranges
        );
        let chinese_scope = picker_locale_scope(&scope, Locale::zh_cn(), cx);
        let traditional_scope = picker_locale_scope(&scope, Locale::zh_tw(), cx);
        Self {
            _config: scope.observe(cx),
            _states: observers,
            time_page,
            scope,
            chinese_scope,
            traditional_scope,
            dates,
            date_times,
            multiple,
            ranges,
            date_time_ranges,
            times,
            time_ranges,
            controlled_date: Some(date(30)),
            controlled_time: Some(time(13, 30, 56)),
            switch_mode: PickerMode::Date,
            anchors,
        }
    }

    fn date(&self, key: &'static str) -> DatePicker {
        Picker::new(key, &self.dates[key]).scope(self.scope.clone())
    }
    fn date_time(&self, key: &'static str) -> DateTimePicker {
        Picker::new(key, &self.date_times[key]).scope(self.scope.clone())
    }
    fn range(&self, key: &'static str) -> RangePicker {
        Picker::new(key, &self.ranges[key]).scope(self.scope.clone())
    }
    fn date_time_range(&self, key: &'static str) -> DateTimeRangePicker {
        Picker::new(key, &self.date_time_ranges[key]).scope(self.scope.clone())
    }
    fn multiple(&self, key: &'static str) -> MultipleDatePicker {
        Picker::new(key, &self.multiple[key]).scope(self.scope.clone())
    }
    fn time(&self, key: &'static str) -> TimePicker {
        Picker::new(key, &self.times[key]).scope(self.scope.clone())
    }
    fn time_range(&self, key: &'static str) -> TimeRangePicker {
        Picker::new(key, &self.time_ranges[key]).scope(self.scope.clone())
    }

    // Separate concrete GPUI builders keep debug frames bounded on native Windows stacks.
    // A single large match otherwise reserves space for every demo's temporary elements.
    fn preview(&self, id: &'static str, cx: &mut Context<Self>) -> AnyElement {
        if self.time_page {
            return self.time_preview(id, cx);
        }
        match id {
            "date-basic" => self.preview_date_basic(id, cx),
            "date-range" => self.preview_date_range(id, cx),
            "date-multiple" => self.preview_date_multiple(id, cx),
            "date-confirm" => self.preview_date_confirm(id, cx),
            "date-switchable" => self.preview_date_switchable(id, cx),
            "date-format" => self.preview_date_format(id, cx),
            "date-time" => self.preview_date_time(id, cx),
            "date-mask" => self.preview_date_mask(id, cx),
            "date-limit" => self.preview_date_limit(id, cx),
            "date-disabled" => self.preview_date_disabled(id, cx),
            "date-disabled-date" => self.preview_date_disabled_date(id, cx),
            "date-allow-empty" => self.preview_date_allow_empty(id, cx),
            "date-select-range" => self.preview_date_select_range(id, cx),
            "date-presets" => self.preview_date_presets(id, cx),
            "date-footer" => self.preview_date_footer(id, cx),
            "date-size" => self.preview_date_size(id, cx),
            "date-cell-render" => self.preview_date_cell_render(id, cx),
            "date-components" => self.preview_date_components(id, cx),
            "date-external-panel" => self.preview_date_external_panel(id, cx),
            "date-buddhist" => self.preview_date_buddhist(id, cx),
            "date-status" => self.preview_date_status(id, cx),
            "date-variant" => self.preview_date_variant(id, cx),
            "date-style" => self.preview_date_style(id, cx),
            "date-placement" => self.preview_date_placement(id, cx),
            "date-suffix" => self.preview_date_suffix(id, cx),
            "date-controlled" => self.preview_date_controlled(id, cx),
            "date-locale" => self.preview_date_locale(id, cx),
            _ => unreachable!("registered date-picker demo"),
        }
    }

    fn time_preview(&self, id: &'static str, cx: &mut Context<Self>) -> AnyElement {
        match id {
            "time-basic" => self.preview_time_basic(id, cx),
            "time-value" => self.preview_time_value(id, cx),
            "time-size" => self.preview_time_size(id, cx),
            "time-confirm" => self.preview_time_confirm(id, cx),
            "time-disabled" => self.preview_time_disabled(id, cx),
            "time-hide-column" => self.preview_time_hide_column(id, cx),
            "time-interval" => self.preview_time_interval(id, cx),
            "time-addon" => self.preview_time_addon(id, cx),
            "time-12hours" => self.preview_time_12hours(id, cx),
            "time-scroll" => self.preview_time_scroll(id, cx),
            "time-range" => self.preview_time_range(id, cx),
            "time-variant" => self.preview_time_variant(id, cx),
            "time-status" => self.preview_time_status(id, cx),
            "time-suffix" => self.preview_time_suffix(id, cx),
            "time-style" => self.preview_time_style(id, cx),
            "time-millisecond" => self.preview_time_millisecond(id, cx),
            "time-disabled-options" => self.preview_time_disabled_options(id, cx),
            "time-external-panel" => self.preview_time_external_panel(id, cx),
            _ => unreachable!("registered time-picker demo"),
        }
    }

    #[inline(never)]
    fn preview_date_basic(&self, id: &'static str, _cx: &mut Context<Self>) -> AnyElement {
        column()
            .child(self.date(id))
            .child(self.date("date-basic.week").picker_mode(PickerMode::Week))
            .child(self.date("date-basic.month").picker_mode(PickerMode::Month))
            .child(
                self.date("date-basic.quarter")
                    .picker_mode(PickerMode::Quarter),
            )
            .child(self.date("date-basic.year").picker_mode(PickerMode::Year))
            .into_any_element()
    }

    #[inline(never)]
    fn preview_date_range(&self, id: &'static str, _cx: &mut Context<Self>) -> AnyElement {
        column()
            .child(self.range(id))
            .child(self.date_time_range("date-range.time"))
            .child(self.range("date-range.week").picker_mode(PickerMode::Week))
            .child(
                self.range("date-range.month")
                    .picker_mode(PickerMode::Month),
            )
            .child(
                self.range("date-range.quarter")
                    .picker_mode(PickerMode::Quarter),
            )
            .child(self.range("date-range.year").picker_mode(PickerMode::Year))
            .into_any_element()
    }

    #[inline(never)]
    fn preview_date_multiple(&self, id: &'static str, _cx: &mut Context<Self>) -> AnyElement {
        let tag_scope = self.scope.clone();
        column()
            .child(
                self.multiple(id)
                    .default_value(Arc::from([date(1), date(3), date(5)])),
            )
            .child(
                self.multiple("date-multiple.week")
                    .picker_mode(PickerMode::Week),
            )
            .child(
                self.multiple("date-multiple.month")
                    .picker_mode(PickerMode::Month),
            )
            .child(
                self.multiple("date-multiple.quarter")
                    .picker_mode(PickerMode::Quarter),
            )
            .child(
                self.multiple("date-multiple.year")
                    .picker_mode(PickerMode::Year),
            )
            .child(
                self.multiple("date-multiple.custom")
                    .default_value(Arc::from([date(1), date(3), date(5)]))
                    .tag_render(move |context, _, _, _| {
                        let command = context.clone();
                        Tag::new(gpui::SharedString::from(format!(
                            "custom-date.{}",
                            context.value
                        )))
                        .scope(tag_scope.clone())
                        .label(context.value.format("%d %b").to_string())
                        .color(ant_design_gpui::theme::PresetColor::Blue)
                        .disabled(context.disabled)
                        .closable(!context.disabled)
                        .on_close(move |event, window, cx| {
                            event.prevent_default();
                            command.remove(window, cx);
                        })
                    }),
            )
            .into_any_element()
    }

    #[inline(never)]
    fn preview_date_confirm(&self, id: &'static str, _cx: &mut Context<Self>) -> AnyElement {
        column()
            .child(self.date(id).need_confirm(true))
            .child(self.date("date-confirm.auto").need_confirm(false))
            .into_any_element()
    }

    #[inline(never)]
    fn preview_date_switchable(&self, id: &'static str, cx: &mut Context<Self>) -> AnyElement {
        column()
            .child(
                row(&self.scope, cx).children(
                    [
                        (PickerMode::Date, "Date"),
                        (PickerMode::Week, "Week"),
                        (PickerMode::Month, "Month"),
                        (PickerMode::Quarter, "Quarter"),
                        (PickerMode::Year, "Year"),
                    ]
                    .into_iter()
                    .map(|(mode, label)| {
                        Button::new(gpui::SharedString::from(format!("date-mode.{label}")))
                            .scope(self.scope.clone())
                            .label(label)
                            .button_type(if self.switch_mode == mode {
                                ButtonType::Primary
                            } else {
                                ButtonType::Default
                            })
                            .on_click(cx.listener(move |this, _, _, cx| {
                                this.switch_mode = mode;
                                cx.notify();
                            }))
                    }),
                ),
            )
            .child(self.date(id).picker_mode(self.switch_mode))
            .into_any_element()
    }

    #[inline(never)]
    fn preview_date_format(&self, id: &'static str, _cx: &mut Context<Self>) -> AnyElement {
        column()
            .child(
                self.date(id)
                    .default_value(Some(date(30)))
                    .format(pattern("%d/%m/%Y")),
            )
            .child(
                self.date("date-format.alt")
                    .default_value(Some(date(30)))
                    .format(
                        PickerFormat::patterns(["%Y/%m/%d", "%d-%m-%Y"])
                            .expect("valid alternative formats"),
                    ),
            )
            .into_any_element()
    }

    #[inline(never)]
    fn preview_date_time(&self, id: &'static str, _cx: &mut Context<Self>) -> AnyElement {
        column()
            .child(
                self.date_time(id)
                    .default_value(Some(date(30).and_time(time(13, 30, 56)))),
            )
            .child(
                self.date_time_range("date-time.range")
                    .format(pattern("%Y-%m-%d %H:%M"))
                    .time_options(TimeOptions {
                        show_second: false,
                        ..Default::default()
                    }),
            )
            .into_any_element()
    }

    #[inline(never)]
    fn preview_date_mask(&self, id: &'static str, _cx: &mut Context<Self>) -> AnyElement {
        self.date(id)
            .format(PickerFormat::mask("%Y-%m-%d").expect("valid mask"))
            .into_any_element()
    }

    #[inline(never)]
    fn preview_date_limit(&self, id: &'static str, _cx: &mut Context<Self>) -> AnyElement {
        self.date(id)
            .min_date(date(10))
            .max_date(date(25))
            .default_value(Some(date(15)))
            .into_any_element()
    }

    #[inline(never)]
    fn preview_date_disabled(&self, id: &'static str, _cx: &mut Context<Self>) -> AnyElement {
        column()
            .child(self.date(id).default_value(Some(date(30))).disabled(true))
            .child(
                self.range("date-disabled.range")
                    .default_value((date(10), date(20)).into())
                    .disabled_endpoints([true, false]),
            )
            .into_any_element()
    }

    #[inline(never)]
    fn preview_date_disabled_date(&self, id: &'static str, _cx: &mut Context<Self>) -> AnyElement {
        column()
            .child(
                self.date(id)
                    .disabled_date(|date, _| date.weekday().number_from_monday() >= 6),
            )
            .child(
                self.date_time("date-disabled-time")
                    .time_options(TimeOptions {
                        hour_step: 2,
                        minute_step: 15,
                        hide_disabled_options: true,
                        ..Default::default()
                    })
                    .disabled_time(|_, _| {
                        ant_design_gpui::date_picker::DisabledTime::new()
                            .hours(|| (0..9).chain(18..24).collect())
                            .minutes(|hour| if hour == 9 { vec![0, 15] } else { Vec::new() })
                    }),
            )
            .into_any_element()
    }

    #[inline(never)]
    fn preview_date_allow_empty(&self, id: &'static str, _cx: &mut Context<Self>) -> AnyElement {
        self.range(id)
            .allow_empty([false, true])
            .range_placeholders(["Start date".into(), "Till now".into()])
            .into_any_element()
    }

    #[inline(never)]
    fn preview_date_select_range(&self, id: &'static str, _cx: &mut Context<Self>) -> AnyElement {
        self.range(id)
            .disabled_date(|date, info| {
                info.from
                    .is_some_and(|from| date.signed_duration_since(from).num_days().abs() > 7)
            })
            .into_any_element()
    }

    #[inline(never)]
    fn preview_date_presets(&self, id: &'static str, _cx: &mut Context<Self>) -> AnyElement {
        column()
            .child(self.date("date-presets.single").presets(vec![
                PickerPreset::new("today", "Today", Some(date(30))),
                PickerPreset::new("next-week", "Next week", Some(date(30) + Duration::days(7))),
            ]))
            .child(self.range(id).presets(vec![
                PickerPreset::new("last-week", "Last 7 days", (date(24), date(30)).into()),
                PickerPreset::new("this-month", "This month", (date(1), date(31)).into()),
            ]))
            .into_any_element()
    }

    #[inline(never)]
    fn preview_date_footer(&self, id: &'static str, _cx: &mut Context<Self>) -> AnyElement {
        self.date(id)
            .extra_footer(|_, _| div().child("Extra footer").into_any_element())
            .into_any_element()
    }

    #[inline(never)]
    fn preview_date_size(&self, id: &'static str, _cx: &mut Context<Self>) -> AnyElement {
        column()
            .child(self.date("date-size.small").size(Size::Small))
            .child(self.date(id))
            .child(self.date("date-size.large").size(Size::Large))
            .into_any_element()
    }

    #[inline(never)]
    fn preview_date_cell_render(&self, id: &'static str, _cx: &mut Context<Self>) -> AnyElement {
        self.date(id)
            .cell_render(|context, origin, _, _| {
                div()
                    .relative()
                    .child(origin)
                    .when(context.date.is_some_and(|date| date.day() == 1), |cell| {
                        cell.child(
                            div()
                                .absolute()
                                .bottom(px(0.))
                                .left(px(10.))
                                .size(px(4.))
                                .rounded_full()
                                .bg(gpui::rgb(0x52c41a)),
                        )
                    })
                    .into_any_element()
            })
            .into_any_element()
    }

    #[inline(never)]
    fn preview_date_components(&self, id: &'static str, _cx: &mut Context<Self>) -> AnyElement {
        self.date(id)
            .input_render(|_, input, _, _| {
                input
                    .placeholder("Delivery date")
                    .accessibility_label("Delivery date")
            })
            .panel_render(|parts, window, cx| {
                column()
                    .child(
                        div()
                            .px(px(12.))
                            .pt(px(12.))
                            .child("Choose a delivery date"),
                    )
                    .child(parts.panel(window, cx))
            })
            .into_any_element()
    }

    #[inline(never)]
    fn preview_date_external_panel(&self, id: &'static str, _cx: &mut Context<Self>) -> AnyElement {
        DatePickerPanel::new(id, &self.dates[id])
            .scope(self.scope.clone())
            .into_any_element()
    }

    #[inline(never)]
    fn preview_date_buddhist(&self, id: &'static str, _cx: &mut Context<Self>) -> AnyElement {
        self.date(id)
            .default_value(Some(date(30)))
            .format_render(|value| {
                format!(
                    "{}-{:02}-{:02}",
                    value.year() + 543,
                    value.month(),
                    value.day()
                )
                .into()
            })
            .into_any_element()
    }

    #[inline(never)]
    fn preview_date_status(&self, id: &'static str, _cx: &mut Context<Self>) -> AnyElement {
        column()
            .child(self.date(id).status(ValidationStatus::Error))
            .child(
                self.date("date-status.warning")
                    .status(ValidationStatus::Warning),
            )
            .into_any_element()
    }

    #[inline(never)]
    fn preview_date_variant(&self, id: &'static str, _cx: &mut Context<Self>) -> AnyElement {
        column()
            .child(self.date(id))
            .child(self.date("date-variant.filled").variant(Variant::Filled))
            .child(
                self.date("date-variant.borderless")
                    .variant(Variant::Borderless),
            )
            .child(
                self.date("date-variant.underlined")
                    .variant(Variant::Underlined),
            )
            .into_any_element()
    }

    #[inline(never)]
    fn preview_date_style(&self, id: &'static str, _cx: &mut Context<Self>) -> AnyElement {
        column()
            .child(self.date(id).styles(PickerStyles {
                root: StyleRefinement::default().border_color(gpui::rgb(0x722ed1)),
                popup: StyleRefinement::default().rounded(px(12.)),
                ..Default::default()
            }))
            .child(
                self.date("date-style.large")
                    .size(Size::Large)
                    .styles_fn(|context, _, _| PickerStyles {
                        root: StyleRefinement::default().bg(if context.size == Size::Large {
                            gpui::rgb(0xf9f0ff)
                        } else {
                            gpui::rgb(0xffffff)
                        }),
                        ..Default::default()
                    }),
            )
            .into_any_element()
    }

    #[inline(never)]
    fn preview_date_placement(&self, id: &'static str, _cx: &mut Context<Self>) -> AnyElement {
        column()
            .child(
                self.date(id)
                    .placement(PopupPlacement::BottomLeft)
                    .placeholder("Bottom left"),
            )
            .child(
                self.date("date-placement.bottom-right")
                    .placement(PopupPlacement::BottomRight)
                    .placeholder("Bottom right"),
            )
            .child(
                self.date("date-placement.top-left")
                    .placement(PopupPlacement::TopLeft)
                    .placeholder("Top left"),
            )
            .child(
                self.date("date-placement.top-right")
                    .placement(PopupPlacement::TopRight)
                    .placeholder("Top right"),
            )
            .into_any_element()
    }

    #[inline(never)]
    fn preview_date_suffix(&self, id: &'static str, _cx: &mut Context<Self>) -> AnyElement {
        column()
            .child(self.date(id).prefix("Date"))
            .child(
                self.date("date-suffix.custom")
                    .suffix_icon(icon(IconName::SmileOutlined)),
            )
            .into_any_element()
    }

    #[inline(never)]
    fn preview_date_controlled(&self, id: &'static str, cx: &mut Context<Self>) -> AnyElement {
        column()
            .child(
                self.date(id)
                    .value(self.controlled_date)
                    .on_change(cx.listener(
                        |this, change: &PickerChange<Option<NaiveDate>>, _, cx| {
                            this.controlled_date = change.value;
                            cx.notify();
                        },
                    )),
            )
            .child(div().child(self.controlled_date.map_or_else(
                || "No date selected".to_owned(),
                |value| value.format("%Y-%m-%d").to_string(),
            )))
            .into_any_element()
    }

    #[inline(never)]
    fn preview_date_locale(&self, id: &'static str, _cx: &mut Context<Self>) -> AnyElement {
        column()
            .child(self.date(id))
            .child(
                self.date("date-locale.cn")
                    .scope(self.chinese_scope.clone()),
            )
            .child(
                self.date("date-locale.tw")
                    .scope(self.traditional_scope.clone()),
            )
            .into_any_element()
    }

    #[inline(never)]
    fn preview_time_basic(&self, id: &'static str, _cx: &mut Context<Self>) -> AnyElement {
        self.time(id)
            .default_value(Some(time(13, 30, 56)))
            .into_any_element()
    }

    #[inline(never)]
    fn preview_time_value(&self, id: &'static str, cx: &mut Context<Self>) -> AnyElement {
        column()
            .child(
                self.time(id)
                    .value(self.controlled_time)
                    .on_change(cx.listener(
                        |this, change: &PickerChange<Option<NaiveTime>>, _, cx| {
                            this.controlled_time = change.value;
                            cx.notify();
                        },
                    )),
            )
            .child(div().child(self.controlled_time.map_or_else(
                || "No time selected".to_owned(),
                |value| value.format("%H:%M:%S").to_string(),
            )))
            .into_any_element()
    }

    #[inline(never)]
    fn preview_time_size(&self, id: &'static str, _cx: &mut Context<Self>) -> AnyElement {
        column()
            .child(self.time("time-size.small").size(Size::Small))
            .child(self.time(id))
            .child(self.time("time-size.large").size(Size::Large))
            .into_any_element()
    }

    #[inline(never)]
    fn preview_time_confirm(&self, id: &'static str, _cx: &mut Context<Self>) -> AnyElement {
        self.time(id).need_confirm(false).into_any_element()
    }

    #[inline(never)]
    fn preview_time_disabled(&self, id: &'static str, _cx: &mut Context<Self>) -> AnyElement {
        self.time(id)
            .default_value(Some(time(13, 30, 56)))
            .disabled(true)
            .into_any_element()
    }

    #[inline(never)]
    fn preview_time_hide_column(&self, id: &'static str, _cx: &mut Context<Self>) -> AnyElement {
        self.time(id)
            .format(pattern("%H:%M"))
            .time_options(TimeOptions {
                show_second: false,
                ..Default::default()
            })
            .into_any_element()
    }

    #[inline(never)]
    fn preview_time_interval(&self, id: &'static str, _cx: &mut Context<Self>) -> AnyElement {
        self.time(id)
            .time_options(TimeOptions {
                minute_step: 15,
                second_step: 10,
                ..Default::default()
            })
            .into_any_element()
    }

    #[inline(never)]
    fn preview_time_addon(&self, id: &'static str, _cx: &mut Context<Self>) -> AnyElement {
        self.time(id)
            .extra_footer(|_, _| div().child("Choose an appointment time").into_any_element())
            .into_any_element()
    }

    #[inline(never)]
    fn preview_time_12hours(&self, id: &'static str, _cx: &mut Context<Self>) -> AnyElement {
        column()
            .child(self.time(id).time_options(TimeOptions {
                use_12_hours: true,
                ..Default::default()
            }))
            .child(
                self.time("time-12hours.minute")
                    .format(pattern("%I:%M %p"))
                    .time_options(TimeOptions {
                        use_12_hours: true,
                        show_second: false,
                        ..Default::default()
                    }),
            )
            .into_any_element()
    }

    #[inline(never)]
    fn preview_time_scroll(&self, id: &'static str, _cx: &mut Context<Self>) -> AnyElement {
        self.time(id)
            .time_options(TimeOptions {
                change_on_scroll: true,
                ..Default::default()
            })
            .into_any_element()
    }

    #[inline(never)]
    fn preview_time_range(&self, id: &'static str, _cx: &mut Context<Self>) -> AnyElement {
        column()
            .child(self.time_range(id))
            .child(
                self.time_range("time-range.unordered")
                    .order(false)
                    .default_value(PickerRange::new(Some(time(18, 0, 0)), Some(time(9, 0, 0)))),
            )
            .into_any_element()
    }

    #[inline(never)]
    fn preview_time_variant(&self, id: &'static str, _cx: &mut Context<Self>) -> AnyElement {
        column()
            .child(self.time(id))
            .child(self.time("time-variant.filled").variant(Variant::Filled))
            .child(
                self.time("time-variant.borderless")
                    .variant(Variant::Borderless),
            )
            .child(
                self.time("time-variant.underlined")
                    .variant(Variant::Underlined),
            )
            .into_any_element()
    }

    #[inline(never)]
    fn preview_time_status(&self, id: &'static str, _cx: &mut Context<Self>) -> AnyElement {
        column()
            .child(self.time(id).status(ValidationStatus::Error))
            .child(
                self.time("time-status.warning")
                    .status(ValidationStatus::Warning),
            )
            .into_any_element()
    }

    #[inline(never)]
    fn preview_time_suffix(&self, id: &'static str, _cx: &mut Context<Self>) -> AnyElement {
        column()
            .child(self.time(id).prefix("Time"))
            .child(
                self.time("time-suffix.custom")
                    .suffix_icon(icon(IconName::SmileOutlined)),
            )
            .into_any_element()
    }

    #[inline(never)]
    fn preview_time_style(&self, id: &'static str, _cx: &mut Context<Self>) -> AnyElement {
        column()
            .child(self.time(id).styles(PickerStyles {
                root: StyleRefinement::default().border_color(gpui::rgb(0x722ed1)),
                popup: StyleRefinement::default().rounded(px(12.)),
                ..Default::default()
            }))
            .child(
                self.time("time-style.large")
                    .size(Size::Large)
                    .styles_fn(|context, _, _| PickerStyles {
                        root: StyleRefinement::default().bg(if context.size == Size::Large {
                            gpui::rgb(0xf9f0ff)
                        } else {
                            gpui::rgb(0xffffff)
                        }),
                        ..Default::default()
                    }),
            )
            .into_any_element()
    }

    #[inline(never)]
    fn preview_time_millisecond(&self, id: &'static str, _cx: &mut Context<Self>) -> AnyElement {
        self.time(id)
            .format(pattern("%H:%M:%S%.3f"))
            .time_options(TimeOptions {
                show_millisecond: true,
                ..Default::default()
            })
            .into_any_element()
    }

    #[inline(never)]
    fn preview_time_disabled_options(
        &self,
        id: &'static str,
        _cx: &mut Context<Self>,
    ) -> AnyElement {
        self.time(id)
            .time_options(TimeOptions {
                hide_disabled_options: true,
                ..Default::default()
            })
            .disabled_time(|_, _| {
                ant_design_gpui::date_picker::DisabledTime::new()
                    .hours(|| (0..9).chain(18..24).collect())
            })
            .into_any_element()
    }

    #[inline(never)]
    fn preview_time_external_panel(&self, id: &'static str, _cx: &mut Context<Self>) -> AnyElement {
        PickerPanel::<TimeSelection>::new(id, &self.times[id])
            .scope(self.scope.clone())
            .into_any_element()
    }
}

impl Render for PickerPage {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let (name, demos) = if self.time_page {
            ("time-picker", TIME_DEMOS)
        } else {
            ("date-picker", DATE_DEMOS)
        };
        examples::grid(&self.scope, window, cx)
            .id(if self.time_page {
                "gallery.time-picker.examples"
            } else {
                "gallery.date-picker.examples"
            })
            .children(demos.iter().zip(&self.anchors).map(|(demo, anchor)| {
                examples::card(
                    name,
                    demo,
                    anchor.clone(),
                    self.preview(demo.id, cx),
                    &self.scope,
                    cx,
                )
            }))
    }
}
