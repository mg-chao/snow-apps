// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use chrono::{NaiveTime, Timelike};
use std::{fmt, rc::Rc, sync::Arc};

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum TimeUnit {
    Hour,
    Minute,
    Second,
    Millisecond,
    Meridiem,
}

/// Time-column configuration. Steps constrain popup choices, not typed input.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TimeOptions {
    pub use_12_hours: bool,
    pub show_hour: bool,
    pub show_minute: bool,
    pub show_second: bool,
    pub show_millisecond: bool,
    pub hour_step: u32,
    pub minute_step: u32,
    pub second_step: u32,
    pub millisecond_step: u32,
    pub hide_disabled_options: bool,
    pub change_on_scroll: bool,
    pub default_open_value: Option<NaiveTime>,
}
impl Default for TimeOptions {
    fn default() -> Self {
        Self {
            use_12_hours: false,
            show_hour: true,
            show_minute: true,
            show_second: true,
            show_millisecond: false,
            hour_step: 1,
            minute_step: 1,
            second_step: 1,
            millisecond_step: 100,
            hide_disabled_options: false,
            change_on_scroll: false,
            default_open_value: None,
        }
    }
}

type Hours = Rc<dyn Fn() -> Vec<u32>>;
type Minutes = Rc<dyn Fn(u32) -> Vec<u32>>;
type Seconds = Rc<dyn Fn(u32, u32) -> Vec<u32>>;
type Milliseconds = Rc<dyn Fn(u32, u32, u32) -> Vec<u32>>;

/// Cascading disabled-unit callbacks. These never turn step intervals into validation rules.
#[derive(Clone, Default)]
pub struct DisabledTime {
    hours: Option<Hours>,
    minutes: Option<Minutes>,
    seconds: Option<Seconds>,
    milliseconds: Option<Milliseconds>,
}

/// One unit evaluated against one set of more significant time fields. Keeping
/// this snapshot local to an operation lets dynamic callbacks stay live without
/// allocating their result again for every candidate or visible row.
#[derive(Clone, Copy)]
pub(super) struct DisabledTimeSnapshot {
    bits: [u64; 16],
}

impl DisabledTimeSnapshot {
    fn from_values(values: impl IntoIterator<Item = u32>) -> Self {
        let mut snapshot = Self { bits: [0; 16] };
        for value in values {
            if let Some(word) = snapshot.bits.get_mut(value as usize / 64) {
                *word |= 1 << (value % 64);
            }
        }
        snapshot
    }

    pub(super) fn disabled(&self, value: u32) -> bool {
        self.bits
            .get(value as usize / 64)
            .is_some_and(|word| word & (1 << (value % 64)) != 0)
    }
}
impl fmt::Debug for DisabledTime {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("DisabledTime")
            .field("hours", &self.hours.is_some())
            .field("minutes", &self.minutes.is_some())
            .field("seconds", &self.seconds.is_some())
            .field("milliseconds", &self.milliseconds.is_some())
            .finish()
    }
}
impl DisabledTime {
    pub fn new() -> Self {
        Self::default()
    }
    pub fn hours(mut self, f: impl Fn() -> Vec<u32> + 'static) -> Self {
        self.hours = Some(Rc::new(f));
        self
    }
    pub fn minutes(mut self, f: impl Fn(u32) -> Vec<u32> + 'static) -> Self {
        self.minutes = Some(Rc::new(f));
        self
    }
    pub fn seconds(mut self, f: impl Fn(u32, u32) -> Vec<u32> + 'static) -> Self {
        self.seconds = Some(Rc::new(f));
        self
    }
    pub fn milliseconds(mut self, f: impl Fn(u32, u32, u32) -> Vec<u32> + 'static) -> Self {
        self.milliseconds = Some(Rc::new(f));
        self
    }
    pub fn disabled(&self, unit: TimeUnit, value: u32, time: NaiveTime) -> bool {
        self.snapshot(unit, time).disabled(value)
    }
    pub(super) fn snapshot(&self, unit: TimeUnit, time: NaiveTime) -> DisabledTimeSnapshot {
        let values = match unit {
            TimeUnit::Hour => self.hours.as_ref().map(|f| f()).unwrap_or_default(),
            TimeUnit::Minute => self
                .minutes
                .as_ref()
                .map(|f| f(time.hour()))
                .unwrap_or_default(),
            TimeUnit::Second => self
                .seconds
                .as_ref()
                .map(|f| f(time.hour(), time.minute()))
                .unwrap_or_default(),
            TimeUnit::Millisecond => self
                .milliseconds
                .as_ref()
                .map(|f| f(time.hour(), time.minute(), time.second()))
                .unwrap_or_default(),
            TimeUnit::Meridiem => {
                let hours = self.snapshot(TimeUnit::Hour, time);
                return DisabledTimeSnapshot::from_values(
                    (0..2).filter(|value| (value * 12..value * 12 + 12).all(|h| hours.disabled(h))),
                );
            }
        };
        DisabledTimeSnapshot::from_values(values.into_iter().filter(|v| *v <= unit.maximum()))
    }
    pub fn valid(&self, time: NaiveTime) -> bool {
        [
            TimeUnit::Hour,
            TimeUnit::Minute,
            TimeUnit::Second,
            TimeUnit::Millisecond,
        ]
        .into_iter()
        .all(|unit| !self.disabled(unit, unit.value(time), time))
    }
}

impl TimeUnit {
    pub fn value(self, time: NaiveTime) -> u32 {
        match self {
            Self::Hour => time.hour(),
            Self::Minute => time.minute(),
            Self::Second => time.second(),
            Self::Millisecond => time.nanosecond() / 1_000_000,
            Self::Meridiem => time.hour() / 12,
        }
    }
    pub fn maximum(self) -> u32 {
        match self {
            Self::Hour => 23,
            Self::Minute | Self::Second => 59,
            Self::Millisecond => 999,
            Self::Meridiem => 1,
        }
    }
    pub fn set(self, time: NaiveTime, value: u32) -> Option<NaiveTime> {
        match self {
            Self::Hour => time.with_hour(value),
            Self::Minute => time.with_minute(value),
            Self::Second => time.with_second(value),
            Self::Millisecond => time.with_nanosecond(value.checked_mul(1_000_000)?),
            Self::Meridiem => time.with_hour(time.hour() % 12 + value * 12),
        }
    }
}
impl TimeOptions {
    pub fn units(&self) -> Vec<TimeUnit> {
        [
            (self.show_hour, TimeUnit::Hour),
            (self.show_minute, TimeUnit::Minute),
            (self.show_second, TimeUnit::Second),
            (self.show_millisecond, TimeUnit::Millisecond),
            (self.use_12_hours, TimeUnit::Meridiem),
        ]
        .into_iter()
        .filter_map(|(show, unit)| show.then_some(unit))
        .collect()
    }
    fn value_step(&self, unit: TimeUnit) -> usize {
        match unit {
            TimeUnit::Hour => self.hour_step,
            TimeUnit::Minute => self.minute_step,
            TimeUnit::Second => self.second_step,
            TimeUnit::Millisecond => self.millisecond_step,
            TimeUnit::Meridiem => 1,
        }
        .clamp(1, unit.maximum() + 1) as usize
    }
    pub fn values(&self, unit: TimeUnit) -> Vec<u32> {
        (0..=unit.maximum())
            .step_by(self.value_step(unit))
            .collect()
    }
    pub(super) fn visible_values(
        &self,
        unit: TimeUnit,
        time: NaiveTime,
        disabled: &DisabledTimeSnapshot,
    ) -> Arc<[u32]> {
        (0..=unit.maximum())
            .step_by(self.value_step(unit))
            .filter(|value| !self.hide_disabled_options || !disabled.disabled(*value))
            .filter(|value| {
                unit != TimeUnit::Hour || !self.use_12_hours || *value / 12 == time.hour() / 12
            })
            .collect()
    }
    /// Align popup edits from the most significant unit downwards.
    pub fn align(&self, mut time: NaiveTime, disabled: &DisabledTime) -> Option<NaiveTime> {
        for unit in [
            TimeUnit::Hour,
            TimeUnit::Minute,
            TimeUnit::Second,
            TimeUnit::Millisecond,
        ] {
            let current = unit.value(time);
            let values = self.values(unit);
            let snapshot = disabled.snapshot(unit, time);
            let enabled = |value: &&u32| !snapshot.disabled(**value);
            let value = values
                .iter()
                .filter(enabled)
                .copied()
                .rfind(|v| *v <= current)
                .or_else(|| values.iter().filter(enabled).copied().next())?;
            time = unit.set(time, value)?;
        }
        Some(time)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::{Cell, RefCell};

    #[test]
    fn date_picker_time_alignment_snapshots_each_dependency_once() {
        let calls = Rc::new(RefCell::new(Vec::new()));
        let hours = calls.clone();
        let minutes = calls.clone();
        let seconds = calls.clone();
        let milliseconds = calls.clone();
        let disabled = DisabledTime::new()
            .hours(move || {
                hours.borrow_mut().push(vec![]);
                vec![13]
            })
            .minutes(move |h| {
                minutes.borrow_mut().push(vec![h]);
                vec![24]
            })
            .seconds(move |h, m| {
                seconds.borrow_mut().push(vec![h, m]);
                vec![35]
            })
            .milliseconds(move |h, m, s| {
                milliseconds.borrow_mut().push(vec![h, m, s]);
                vec![123]
            });
        let options = TimeOptions {
            millisecond_step: 1,
            ..Default::default()
        };
        let time = NaiveTime::from_hms_milli_opt(13, 24, 35, 123).unwrap();
        assert_eq!(
            options.align(time, &disabled),
            NaiveTime::from_hms_milli_opt(12, 23, 34, 122)
        );
        assert_eq!(
            *calls.borrow(),
            vec![vec![], vec![12], vec![12, 23], vec![12, 23, 34]]
        );
    }

    #[test]
    fn date_picker_time_hidden_millisecond_rows_share_one_snapshot() {
        let calls = Rc::new(Cell::new(0));
        let output = calls.clone();
        let disabled = DisabledTime::new().milliseconds(move |h, m, s| {
            assert_eq!((h, m, s), (13, 24, 35));
            output.set(output.get() + 1);
            (0..1000).filter(|value| value % 2 == 0).collect()
        });
        let time = NaiveTime::from_hms_opt(13, 24, 35).unwrap();
        let snapshot = disabled.snapshot(TimeUnit::Millisecond, time);
        let options = TimeOptions {
            hide_disabled_options: true,
            millisecond_step: 1,
            ..Default::default()
        };
        let values = options.visible_values(TimeUnit::Millisecond, time, &snapshot);
        assert_eq!(values.len(), 500);
        assert_eq!(values[0], 1);
        assert_eq!(values[499], 999);
        assert!(values.iter().all(|value| !snapshot.disabled(*value)));
        assert_eq!(calls.get(), 1);
    }

    #[test]
    fn date_picker_time_meridiem_snapshots_hours_once() {
        let calls = Rc::new(Cell::new(0));
        let output = calls.clone();
        let disabled = DisabledTime::new().hours(move || {
            output.set(output.get() + 1);
            (0..12).collect()
        });
        let snapshot = disabled.snapshot(TimeUnit::Meridiem, NaiveTime::MIN);
        assert!(snapshot.disabled(0));
        assert!(!snapshot.disabled(1));
        assert_eq!(calls.get(), 1);
    }

    #[test]
    fn date_picker_time_steps_only_restrict_popup_choices() {
        let options = TimeOptions {
            minute_step: 15,
            ..Default::default()
        };
        let time = NaiveTime::from_hms_opt(13, 24, 35).unwrap();
        assert!(DisabledTime::new().valid(time));
        assert_eq!(
            options.align(time, &DisabledTime::new()).unwrap().minute(),
            15
        );
        assert_eq!(options.values(TimeUnit::Minute), vec![0, 15, 30, 45]);
    }
    #[test]
    fn date_picker_time_dependencies_recompute_after_each_alignment() {
        let disabled = DisabledTime::new()
            .hours(|| vec![13])
            .minutes(|h| if h == 12 { vec![24] } else { vec![] });
        let time = NaiveTime::from_hms_opt(13, 24, 35).unwrap();
        let aligned = TimeOptions::default().align(time, &disabled).unwrap();
        assert_eq!(aligned, NaiveTime::from_hms_opt(12, 23, 35).unwrap());
        assert!(disabled.valid(aligned));
    }
    #[test]
    fn date_picker_time_all_disabled_does_not_fabricate_selection() {
        let disabled = DisabledTime::new().hours(|| (0..24).collect());
        assert!(
            TimeOptions::default()
                .align(NaiveTime::MIN, &disabled)
                .is_none()
        );
    }
    #[test]
    fn date_picker_time_zero_steps_and_milliseconds_are_bounded() {
        let options = TimeOptions {
            hour_step: 0,
            millisecond_step: 1,
            ..Default::default()
        };
        assert_eq!(options.values(TimeUnit::Hour).len(), 24);
        assert_eq!(options.values(TimeUnit::Millisecond).len(), 1000);
        assert_eq!(
            TimeUnit::Meridiem.set(NaiveTime::MIN, 1).unwrap().hour(),
            12
        );
    }
}
