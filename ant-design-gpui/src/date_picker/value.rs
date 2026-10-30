// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Civil values and selection shapes. No timezone conversion occurs in a picker.

use chrono::{NaiveDate, NaiveDateTime, NaiveTime};
use std::{fmt::Debug, sync::Arc};

/// The granularity selected by a date picker.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum PickerMode {
    #[default]
    Date,
    Week,
    Month,
    Quarter,
    Year,
}

/// The currently displayed panel, independently of the selected granularity.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum PickerPanelMode {
    #[default]
    Date,
    Month,
    Quarter,
    Year,
    Decade,
    Time,
}

impl PickerMode {
    pub const fn panel_mode(self) -> PickerPanelMode {
        match self {
            Self::Date | Self::Week => PickerPanelMode::Date,
            Self::Month => PickerPanelMode::Month,
            Self::Quarter => PickerPanelMode::Quarter,
            Self::Year => PickerPanelMode::Year,
        }
    }
}

impl From<PickerMode> for PickerPanelMode {
    fn from(mode: PickerMode) -> Self {
        mode.panel_mode()
    }
}

/// A range may be cleared or open at either end. `allow_empty` controls which
/// partial ranges the interactive picker may commit.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct PickerRange<T> {
    pub start: Option<T>,
    pub end: Option<T>,
}

impl<T> Default for PickerRange<T> {
    fn default() -> Self {
        Self {
            start: None,
            end: None,
        }
    }
}

impl<T> PickerRange<T> {
    pub const fn new(start: Option<T>, end: Option<T>) -> Self {
        Self { start, end }
    }

    pub fn is_empty(&self) -> bool {
        self.start.is_none() && self.end.is_none()
    }
}

impl<T> From<(T, T)> for PickerRange<T> {
    fn from((start, end): (T, T)) -> Self {
        Self::new(Some(start), Some(end))
    }
}

/// Time-only controls use a stable internal date, never the date of opening.
pub const TIME_ANCHOR: NaiveDate = match NaiveDate::from_ymd_opt(1970, 1, 1) {
    Some(date) => date,
    None => panic!("the time anchor is a valid Gregorian date"),
};

mod sealed {
    pub trait Sealed {}
}

/// A supported value shape. The sealed implementations prevent time-only,
/// date-time and multiple-date values being accidentally interchanged.
pub trait PickerSelection: sealed::Sealed + Clone + Copy + Debug + Default + 'static {
    type Value: Clone + Debug + PartialEq + Default + 'static;
    const HAS_DATE: bool;
    const HAS_TIME: bool;
    const MULTIPLE: bool;
    const RANGE: bool;

    fn to_slots(value: &Self::Value) -> Vec<Option<NaiveDateTime>>;
    fn from_slots(slots: &[Option<NaiveDateTime>]) -> Self::Value;
    fn empty() -> Self::Value {
        Self::Value::default()
    }
}

/// Date-only selections can choose date, week, month, quarter or year granularity.
/// The sealed parent trait keeps this capability tied to supported value shapes.
pub trait DateGranularitySelection: PickerSelection {}

/// Time and date-time selections provide native time-column configuration.
pub trait TimePickerSelection: PickerSelection {}

macro_rules! marker {
    ($name:ident) => {
        #[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
        pub struct $name;
        impl sealed::Sealed for $name {}
    };
}

marker!(DateSelection);
marker!(DateTimeSelection);
marker!(MultipleDateSelection);
marker!(DateRangeSelection);
marker!(DateTimeRangeSelection);
marker!(TimeSelection);
marker!(TimeRangeSelection);

impl DateGranularitySelection for DateSelection {}
impl DateGranularitySelection for MultipleDateSelection {}
impl DateGranularitySelection for DateRangeSelection {}
impl TimePickerSelection for DateTimeSelection {}
impl TimePickerSelection for DateTimeRangeSelection {}
impl TimePickerSelection for TimeSelection {}
impl TimePickerSelection for TimeRangeSelection {}

fn first(slots: &[Option<NaiveDateTime>]) -> Option<NaiveDateTime> {
    slots.first().copied().flatten()
}

impl PickerSelection for DateSelection {
    type Value = Option<NaiveDate>;
    const HAS_DATE: bool = true;
    const HAS_TIME: bool = false;
    const MULTIPLE: bool = false;
    const RANGE: bool = false;

    fn to_slots(value: &Self::Value) -> Vec<Option<NaiveDateTime>> {
        vec![value.map(|date| date.and_time(NaiveTime::MIN))]
    }
    fn from_slots(slots: &[Option<NaiveDateTime>]) -> Self::Value {
        first(slots).map(|date| date.date())
    }
}

impl PickerSelection for DateTimeSelection {
    type Value = Option<NaiveDateTime>;
    const HAS_DATE: bool = true;
    const HAS_TIME: bool = true;
    const MULTIPLE: bool = false;
    const RANGE: bool = false;

    fn to_slots(value: &Self::Value) -> Vec<Option<NaiveDateTime>> {
        vec![*value]
    }
    fn from_slots(slots: &[Option<NaiveDateTime>]) -> Self::Value {
        first(slots)
    }
}

impl PickerSelection for MultipleDateSelection {
    type Value = Arc<[NaiveDate]>;
    const HAS_DATE: bool = true;
    const HAS_TIME: bool = false;
    const MULTIPLE: bool = true;
    const RANGE: bool = false;

    fn to_slots(value: &Self::Value) -> Vec<Option<NaiveDateTime>> {
        value
            .iter()
            .map(|date| Some(date.and_time(NaiveTime::MIN)))
            .collect()
    }
    fn from_slots(slots: &[Option<NaiveDateTime>]) -> Self::Value {
        slots.iter().flatten().map(|date| date.date()).collect()
    }
}

impl PickerSelection for TimeSelection {
    type Value = Option<NaiveTime>;
    const HAS_DATE: bool = false;
    const HAS_TIME: bool = true;
    const MULTIPLE: bool = false;
    const RANGE: bool = false;

    fn to_slots(value: &Self::Value) -> Vec<Option<NaiveDateTime>> {
        vec![value.map(|time| TIME_ANCHOR.and_time(time))]
    }
    fn from_slots(slots: &[Option<NaiveDateTime>]) -> Self::Value {
        first(slots).map(|date| date.time())
    }
}

macro_rules! range_selection {
    ($name:ident, $item:ty, $has_date:expr, $has_time:expr, $to:expr, $from:expr) => {
        impl PickerSelection for $name {
            type Value = PickerRange<$item>;
            const HAS_DATE: bool = $has_date;
            const HAS_TIME: bool = $has_time;
            const MULTIPLE: bool = false;
            const RANGE: bool = true;

            fn to_slots(value: &Self::Value) -> Vec<Option<NaiveDateTime>> {
                vec![value.start.map($to), value.end.map($to)]
            }
            fn from_slots(slots: &[Option<NaiveDateTime>]) -> Self::Value {
                PickerRange::new(
                    first(slots).map($from),
                    slots.get(1).copied().flatten().map($from),
                )
            }
        }
    };
}

range_selection!(
    DateRangeSelection,
    NaiveDate,
    true,
    false,
    |date: NaiveDate| date.and_time(NaiveTime::MIN),
    |date: NaiveDateTime| date.date()
);
range_selection!(
    DateTimeRangeSelection,
    NaiveDateTime,
    true,
    true,
    |date: NaiveDateTime| date,
    |date: NaiveDateTime| date
);
range_selection!(
    TimeRangeSelection,
    NaiveTime,
    false,
    true,
    |time: NaiveTime| TIME_ANCHOR.and_time(time),
    |date: NaiveDateTime| date.time()
);

#[cfg(test)]
mod tests {
    use super::*;

    fn date() -> NaiveDate {
        NaiveDate::from_ymd_opt(2024, 2, 29).unwrap()
    }
    fn time() -> NaiveTime {
        NaiveTime::from_hms_milli_opt(23, 58, 12, 345).unwrap()
    }

    fn roundtrip<S: PickerSelection>(value: S::Value) {
        assert_eq!(S::from_slots(&S::to_slots(&value)), value);
    }

    #[test]
    fn picker_values_roundtrip_without_timezone_or_precision_loss() {
        roundtrip::<DateSelection>(Some(date()));
        roundtrip::<DateTimeSelection>(Some(date().and_time(time())));
        roundtrip::<TimeSelection>(Some(time()));
        roundtrip::<MultipleDateSelection>(Arc::from([date(), date().succ_opt().unwrap()]));
        roundtrip::<DateRangeSelection>((date(), date().succ_opt().unwrap()).into());
        roundtrip::<DateTimeRangeSelection>(
            (date().and_time(time()), date().and_time(time())).into(),
        );
        roundtrip::<TimeRangeSelection>((time(), time()).into());
    }

    #[test]
    fn cleared_and_open_ranges_have_distinct_stable_slots() {
        assert_eq!(DateSelection::to_slots(&None), [None]);
        assert!(MultipleDateSelection::to_slots(&Arc::from([])).is_empty());
        assert_eq!(
            DateRangeSelection::to_slots(&PickerRange::default()),
            [None, None]
        );
        let open = PickerRange::new(None, Some(date()));
        roundtrip::<DateRangeSelection>(open);
        assert_eq!(DateRangeSelection::from_slots(&[]), PickerRange::default());
        assert_eq!(DateSelection::from_slots(&[]), None);
    }

    #[test]
    fn date_and_time_slots_normalize_only_the_unused_dimension() {
        let slot = DateSelection::to_slots(&Some(date()))[0].unwrap();
        assert_eq!(slot.time(), NaiveTime::MIN);
        let slot = TimeSelection::to_slots(&Some(time()))[0].unwrap();
        assert_eq!(slot.date(), TIME_ANCHOR);
        assert_eq!(slot.time(), time());
        assert_eq!(PickerMode::Week.panel_mode(), PickerPanelMode::Date);
    }
}
