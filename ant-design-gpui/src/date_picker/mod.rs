// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Native civil date and time selection with retained editing and popup state.

mod calendar;
mod config;
mod control;
mod format;
mod label;
mod state;
mod style;
mod time;
mod time_list;
mod value;
mod view;

pub use calendar::{
    CalendarCell, CalendarConstraints, CalendarDisabledInfo, DisabledDatePredicate, WeekRules,
    add_months, add_years, date_from_week, days_in_month, panel_cells, period_bounds, same_period,
    start_of_week, week_year_number,
};
pub use chrono::{NaiveDate, NaiveDateTime, NaiveTime};
pub use config::{PickerConfig, PickerConfigPatch, PickerStyleResolver};
pub use format::{
    PickerFormat, PickerFormatContext, PickerFormatError, PickerLanguage, PickerMask,
    PickerMaskEdit, PickerMaskField, PickerMaskSegment, PickerTimeFields,
};
pub use state::{
    DisabledDateInfo, PickerCellGeometry, PickerChange, PickerChangeSource, PickerEndpoint,
    PickerEvent, PickerGeometry, PickerOptions, PickerState,
};
pub use state::{PickerFooterActionGeometry, PickerScrollbarGeometry, PickerTagGeometry};
pub use style::{
    DatePickerTheme, DatePickerTokenPatch, DatePickerTokens, PickerStyleContext, PickerStyles,
    PickerTriggerMetrics,
};
pub use time::{DisabledTime, TimeOptions, TimeUnit};
pub use value::{
    DateGranularitySelection, DateRangeSelection, DateSelection, DateTimeRangeSelection,
    DateTimeSelection, MultipleDateSelection, PickerMode, PickerPanelMode, PickerRange,
    PickerSelection, TIME_ANCHOR, TimePickerSelection, TimeRangeSelection, TimeSelection,
};
pub use view::{
    Picker, PickerCellContext, PickerPanel, PickerPanelParts, PickerPreset, PickerTagContext,
};

pub type DatePicker = Picker<DateSelection>;
pub type DateTimePicker = Picker<DateTimeSelection>;
pub type MultipleDatePicker = Picker<MultipleDateSelection>;
pub type RangePicker = Picker<DateRangeSelection>;
pub type DateTimeRangePicker = Picker<DateTimeRangeSelection>;
pub type TimePicker = Picker<TimeSelection>;
pub type TimeRangePicker = Picker<TimeRangeSelection>;
pub type DatePickerState = PickerState<DateSelection>;
pub type DateTimePickerState = PickerState<DateTimeSelection>;
pub type MultipleDatePickerState = PickerState<MultipleDateSelection>;
pub type RangePickerState = PickerState<DateRangeSelection>;
pub type DateTimeRangePickerState = PickerState<DateTimeRangeSelection>;
pub type TimePickerState = PickerState<TimeSelection>;
pub type TimeRangePickerState = PickerState<TimeRangeSelection>;
pub type DatePickerPanel = PickerPanel<DateSelection>;
pub type TimePickerPanel = PickerPanel<TimeSelection>;

#[cfg(test)]
mod interaction_tests;
#[cfg(test)]
mod reference_tests;
