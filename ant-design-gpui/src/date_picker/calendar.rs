// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Checked Gregorian arithmetic and bounded, rendering-independent panel models.

use super::value::{PickerMode, PickerPanelMode};
use chrono::{Datelike, Days, NaiveDate, Weekday};
use std::{
    collections::VecDeque,
    fmt,
    rc::Rc,
    sync::{Arc, LazyLock},
};

/// Locale week rules. `min_days_in_first_week` is always interpreted in 1..=7.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct WeekRules {
    pub first_weekday: Weekday,
    pub min_days_in_first_week: u8,
}

impl Default for WeekRules {
    fn default() -> Self {
        Self::SUNDAY
    }
}

impl WeekRules {
    pub const SUNDAY: Self = Self {
        first_weekday: Weekday::Sun,
        min_days_in_first_week: 1,
    };
    pub const ISO: Self = Self {
        first_weekday: Weekday::Mon,
        min_days_in_first_week: 4,
    };

    pub const fn new(first_weekday: Weekday, min_days_in_first_week: u8) -> Option<Self> {
        if min_days_in_first_week >= 1 && min_days_in_first_week <= 7 {
            Some(Self {
                first_weekday,
                min_days_in_first_week,
            })
        } else {
            None
        }
    }
}

pub fn start_of_week(date: NaiveDate, rules: WeekRules) -> Option<NaiveDate> {
    let offset = (date.weekday().num_days_from_monday() as i64
        - rules.first_weekday.num_days_from_monday() as i64)
        .rem_euclid(7);
    date.checked_sub_days(Days::new(offset as u64))
}

// Using absolute days avoids constructing an out-of-bounds adjacent year at
// chrono's minimum and maximum dates. Gregorian year zero is handled as well.
fn first_week_day(year: i32, rules: WeekRules) -> i64 {
    let prior = i64::from(year) - 1;
    let january_first =
        365 * prior + prior.div_euclid(4) - prior.div_euclid(100) + prior.div_euclid(400) + 1;
    let weekday = (january_first - 1).rem_euclid(7);
    let offset = (weekday - i64::from(rules.first_weekday.num_days_from_monday())).rem_euclid(7);
    let mut start = january_first - offset;
    if 7 - offset < i64::from(rules.min_days_in_first_week.clamp(1, 7)) {
        start += 7;
    }
    start
}

/// Return the locale week year and one-based week number (not calendar year).
pub fn week_year_number(date: NaiveDate, rules: WeekRules) -> (i32, u32) {
    let day = i64::from(date.num_days_from_ce());
    let mut year = date.year();
    if day < first_week_day(year, rules) {
        year -= 1;
    } else if day >= first_week_day(year + 1, rules) {
        year += 1;
    }
    (year, ((day - first_week_day(year, rules)) / 7 + 1) as u32)
}

/// First day of a locale week. Rejects a nonexistent week 53.
pub fn date_from_week(year: i32, week: u32, rules: WeekRules) -> Option<NaiveDate> {
    if !(1..=53).contains(&week) || year == i32::MAX {
        return None;
    }
    let day = first_week_day(year, rules).checked_add(i64::from(week - 1) * 7)?;
    if day >= first_week_day(year + 1, rules) {
        return None;
    }
    NaiveDate::from_num_days_from_ce_opt(i32::try_from(day).ok()?)
}

pub fn days_in_month(year: i32, month: u32) -> Option<u32> {
    let leap = year.rem_euclid(4) == 0 && (year.rem_euclid(100) != 0 || year.rem_euclid(400) == 0);
    match month {
        1 | 3 | 5 | 7 | 8 | 10 | 12 => Some(31),
        4 | 6 | 9 | 11 => Some(30),
        2 => Some(if leap { 29 } else { 28 }),
        _ => None,
    }
}

/// Month navigation keeps the day when possible, otherwise clamps to month end.
pub fn add_months(date: NaiveDate, months: i32) -> Option<NaiveDate> {
    let absolute = i64::from(date.year()) * 12 + i64::from(date.month0()) + i64::from(months);
    let year = i32::try_from(absolute.div_euclid(12)).ok()?;
    let month = absolute.rem_euclid(12) as u32 + 1;
    NaiveDate::from_ymd_opt(year, month, date.day().min(days_in_month(year, month)?))
}

pub fn add_years(date: NaiveDate, years: i32) -> Option<NaiveDate> {
    let year = date.year().checked_add(years)?;
    NaiveDate::from_ymd_opt(
        year,
        date.month(),
        date.day().min(days_in_month(year, date.month())?),
    )
}

pub fn period_bounds(
    date: NaiveDate,
    mode: PickerMode,
    rules: WeekRules,
) -> Option<(NaiveDate, NaiveDate)> {
    let (first_month, last_month) = match mode {
        PickerMode::Date => return Some((date, date)),
        PickerMode::Week => {
            let start = start_of_week(date, rules)?;
            return Some((start, start.checked_add_days(Days::new(6))?));
        }
        PickerMode::Month => (date.month(), date.month()),
        PickerMode::Quarter => {
            let first = date.month0() / 3 * 3 + 1;
            (first, first + 2)
        }
        PickerMode::Year => (1, 12),
    };
    Some((
        NaiveDate::from_ymd_opt(date.year(), first_month, 1)?,
        NaiveDate::from_ymd_opt(
            date.year(),
            last_month,
            days_in_month(date.year(), last_month)?,
        )?,
    ))
}

pub fn same_period(left: NaiveDate, right: NaiveDate, mode: PickerMode, rules: WeekRules) -> bool {
    match mode {
        PickerMode::Date => left == right,
        PickerMode::Week => week_year_number(left, rules) == week_year_number(right, rules),
        PickerMode::Month => left.year() == right.year() && left.month() == right.month(),
        PickerMode::Quarter => {
            left.year() == right.year() && left.month0() / 3 == right.month0() / 3
        }
        PickerMode::Year => left.year() == right.year(),
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CalendarCell {
    /// Representative value used for navigation and selecting this cell.
    pub date: NaiveDate,
    pub period_start: NaiveDate,
    pub period_end: NaiveDate,
    pub in_view: bool,
}

impl CalendarCell {
    pub fn contains(&self, date: NaiveDate) -> bool {
        self.period_start <= date && date <= self.period_end
    }
}

pub(super) fn cell_for_date(
    date: NaiveDate,
    mode: PickerPanelMode,
    rules: WeekRules,
) -> CalendarCell {
    let (start, end) = if mode == PickerPanelMode::Decade {
        let year = date.year().div_euclid(10) * 10;
        (
            NaiveDate::from_ymd_opt(year, 1, 1).unwrap_or(NaiveDate::MIN),
            NaiveDate::from_ymd_opt(year + 9, 12, 31).unwrap_or(NaiveDate::MAX),
        )
    } else {
        let picker = match mode {
            PickerPanelMode::Month => PickerMode::Month,
            PickerPanelMode::Quarter => PickerMode::Quarter,
            PickerPanelMode::Year => PickerMode::Year,
            _ => PickerMode::Date,
        };
        period_bounds(date, picker, rules).unwrap_or((date, date))
    };
    CalendarCell {
        date: start,
        period_start: start,
        period_end: end,
        in_view: true,
    }
}

pub(super) fn callback_disables_cell(
    cell: &CalendarCell,
    mode: PickerPanelMode,
    mut disabled: impl FnMut(NaiveDate) -> bool,
) -> bool {
    match mode {
        PickerPanelMode::Month | PickerPanelMode::Year | PickerPanelMode::Decade => {
            disabled(cell.period_start) && disabled(cell.period_end)
        }
        _ => disabled(cell.date),
    }
}

/// At most 42 dates, 12 months/years/decades or four quarters are materialized.
/// Cells beyond chrono's representable range are omitted rather than panicking.
pub fn panel_cells(date: NaiveDate, mode: PickerPanelMode, rules: WeekRules) -> Vec<CalendarCell> {
    match mode {
        PickerPanelMode::Date => {
            let Some(first) = date
                .with_day(1)
                .and_then(|first| start_of_week(first, rules))
            else {
                return Vec::new();
            };
            (0..42)
                .filter_map(|index| {
                    let cell = first.checked_add_days(Days::new(index))?;
                    Some(CalendarCell {
                        date: cell,
                        period_start: cell,
                        period_end: cell,
                        in_view: cell.year() == date.year() && cell.month() == date.month(),
                    })
                })
                .collect()
        }
        PickerPanelMode::Month | PickerPanelMode::Quarter => {
            let quarter = mode == PickerPanelMode::Quarter;
            (0..if quarter { 4 } else { 12 })
                .filter_map(|index| {
                    let month = index * if quarter { 3 } else { 1 } + 1;
                    let cell = NaiveDate::from_ymd_opt(date.year(), month, 1)?;
                    let (period_start, period_end) = period_bounds(
                        cell,
                        if quarter {
                            PickerMode::Quarter
                        } else {
                            PickerMode::Month
                        },
                        rules,
                    )?;
                    Some(CalendarCell {
                        date: cell,
                        period_start,
                        period_end,
                        in_view: true,
                    })
                })
                .collect()
        }
        PickerPanelMode::Year | PickerPanelMode::Decade => {
            let decade = mode == PickerPanelMode::Decade;
            let interval = if decade { 10 } else { 1 };
            let span = interval * 10;
            let base = date.year().div_euclid(span) * span;
            (0..12)
                .filter_map(|index| {
                    let year = base.checked_add((index - 1) * interval)?;
                    let cell = NaiveDate::from_ymd_opt(year, 1, 1)?;
                    let end = NaiveDate::from_ymd_opt(year.checked_add(interval - 1)?, 12, 31)?;
                    Some(CalendarCell {
                        date: cell,
                        period_start: cell,
                        period_end: end,
                        in_view: (1..=10).contains(&index),
                    })
                })
                .collect()
        }
        PickerPanelMode::Time => Vec::new(),
    }
}

const CALENDAR_CACHE_CAPACITY: usize = 4;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct CalendarKey {
    year: i32,
    month: u32,
    mode: PickerPanelMode,
    rules: WeekRules,
}

impl CalendarKey {
    fn new(date: NaiveDate, mode: PickerPanelMode, rules: WeekRules) -> Self {
        let (year, month) = match mode {
            PickerPanelMode::Date => (date.year(), date.month()),
            PickerPanelMode::Month | PickerPanelMode::Quarter => (date.year(), 0),
            PickerPanelMode::Year => (date.year().div_euclid(10) * 10, 0),
            PickerPanelMode::Decade => (date.year().div_euclid(100) * 100, 0),
            PickerPanelMode::Time => (0, 0),
        };
        Self {
            year,
            month,
            mode,
            rules,
        }
    }
}

/// Four canonical period grids, retained independently of selection, constraints
/// and localized labels. Empty/closed controllers allocate no cache storage.
#[derive(Debug, Default)]
pub(super) struct CalendarCache {
    entries: VecDeque<(CalendarKey, Arc<[CalendarCell]>)>,
}

impl CalendarCache {
    pub(super) fn cells(
        &mut self,
        date: NaiveDate,
        mode: PickerPanelMode,
        rules: WeekRules,
    ) -> Arc<[CalendarCell]> {
        if mode == PickerPanelMode::Time {
            static EMPTY: LazyLock<Arc<[CalendarCell]>> = LazyLock::new(|| Arc::from([]));
            return EMPTY.clone();
        }
        let key = CalendarKey::new(date, mode, rules);
        if let Some(index) = self
            .entries
            .iter()
            .position(|(existing, _)| *existing == key)
        {
            let entry = self
                .entries
                .remove(index)
                .expect("the matching cache entry exists");
            let cells = entry.1.clone();
            self.entries.push_back(entry);
            return cells;
        }
        let cells: Arc<[CalendarCell]> = panel_cells(date, mode, rules).into();
        if self.entries.capacity() == 0 {
            self.entries.reserve_exact(CALENDAR_CACHE_CAPACITY);
        }
        if self.entries.len() == CALENDAR_CACHE_CAPACITY {
            self.entries.pop_front();
        }
        self.entries.push_back((key, cells.clone()));
        cells
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CalendarDisabledInfo {
    pub mode: PickerPanelMode,
    /// The first date selected in the current range interaction.
    pub from: Option<NaiveDate>,
}

pub type DisabledDatePredicate = Rc<dyn Fn(NaiveDate, CalendarDisabledInfo) -> bool>;

#[derive(Clone, Default)]
pub struct CalendarConstraints {
    pub min: Option<NaiveDate>,
    pub max: Option<NaiveDate>,
    pub disabled: Option<DisabledDatePredicate>,
}

impl fmt::Debug for CalendarConstraints {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("CalendarConstraints")
            .field("min", &self.min)
            .field("max", &self.max)
            .field("disabled", &self.disabled.as_ref().map(|_| "predicate"))
            .finish()
    }
}

impl CalendarConstraints {
    pub fn is_disabled(
        &self,
        date: NaiveDate,
        mode: PickerPanelMode,
        from: Option<NaiveDate>,
    ) -> bool {
        self.min.zip(self.max).is_some_and(|(min, max)| min > max)
            || self.min.is_some_and(|min| date < min)
            || self.max.is_some_and(|max| date > max)
            || self
                .disabled
                .as_ref()
                .is_some_and(|disabled| disabled(date, CalendarDisabledInfo { mode, from }))
    }

    /// Bounds disable a coarse cell only when its entire period is out of range.
    /// Month, year and decade callbacks sample both period endpoints; date and
    /// quarter callbacks receive the cell representative, matching panel behavior.
    pub fn cell_disabled(
        &self,
        cell: &CalendarCell,
        mode: PickerPanelMode,
        from: Option<NaiveDate>,
    ) -> bool {
        self.min.zip(self.max).is_some_and(|(min, max)| min > max)
            || self.min.is_some_and(|min| cell.period_end < min)
            || self.max.is_some_and(|max| cell.period_start > max)
            || self.disabled.as_ref().is_some_and(|disabled| {
                callback_disables_cell(cell, mode, |date| {
                    disabled(date, CalendarDisabledInfo { mode, from })
                })
            })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::{Cell, RefCell};

    fn date(year: i32, month: u32, day: u32) -> NaiveDate {
        NaiveDate::from_ymd_opt(year, month, day).unwrap()
    }

    #[test]
    fn picker_calendar_month_arithmetic_clamps_and_checks_boundaries() {
        assert_eq!(add_months(date(2024, 1, 31), 1), Some(date(2024, 2, 29)));
        assert_eq!(add_months(date(2023, 3, 31), -1), Some(date(2023, 2, 28)));
        assert_eq!(add_years(date(2024, 2, 29), 1), Some(date(2025, 2, 28)));
        assert_eq!(add_months(date(1, 1, 31), -1), Some(date(0, 12, 31)));
        assert_eq!(add_years(NaiveDate::MAX, 1), None);
        assert_eq!(add_months(NaiveDate::MIN, -1), None);
        assert_eq!(add_months(date(2024, 1, 1), i32::MAX), None);
    }

    #[test]
    fn picker_calendar_locale_week_years_match_sunday_and_iso_rules() {
        assert_eq!(
            week_year_number(date(2021, 1, 1), WeekRules::ISO),
            (2020, 53)
        );
        assert_eq!(
            week_year_number(date(2021, 1, 1), WeekRules::SUNDAY),
            (2021, 1)
        );
        assert_eq!(
            week_year_number(date(2023, 12, 31), WeekRules::SUNDAY),
            (2024, 1)
        );
        assert_eq!(
            week_year_number(date(2023, 12, 31), WeekRules::ISO),
            (2023, 52)
        );
        assert_eq!(
            date_from_week(2020, 53, WeekRules::ISO),
            Some(date(2020, 12, 28))
        );
        assert_eq!(date_from_week(2021, 53, WeekRules::ISO), None);
        assert_eq!(
            date_from_week(2024, 1, WeekRules::SUNDAY),
            Some(date(2023, 12, 31))
        );
        assert!(WeekRules::new(Weekday::Mon, 0).is_none());
    }

    #[test]
    fn picker_calendar_iso_weeks_agree_with_chrono_across_centuries() {
        for year in [0, 1, 1900, 2000, 2020, 2024, 2100] {
            let mut current = date(year, 1, 1);
            for _ in 0..365 {
                let actual = current.iso_week();
                assert_eq!(
                    week_year_number(current, WeekRules::ISO),
                    (actual.year(), actual.week())
                );
                current = current.succ_opt().unwrap();
            }
        }
        for edge in [NaiveDate::MIN, NaiveDate::MAX] {
            assert!((1..=53).contains(&week_year_number(edge, WeekRules::ISO).1));
        }
    }

    #[test]
    fn picker_calendar_grids_cover_complete_periods_and_adjacent_dates() {
        let sunday = panel_cells(date(2024, 9, 20), PickerPanelMode::Date, WeekRules::SUNDAY);
        assert_eq!(sunday.len(), 42);
        assert_eq!(sunday[0].date, date(2024, 9, 1));
        assert_eq!(sunday[41].date, date(2024, 10, 12));
        let monday = panel_cells(date(2024, 9, 20), PickerPanelMode::Date, WeekRules::ISO);
        assert_eq!(monday[0].date, date(2024, 8, 26));
        assert_eq!(monday.iter().filter(|cell| cell.in_view).count(), 30);
        let months = panel_cells(date(2024, 1, 1), PickerPanelMode::Month, WeekRules::ISO);
        assert_eq!(months.len(), 12);
        assert_eq!(months[1].period_end, date(2024, 2, 29));
        let quarters = panel_cells(date(2024, 1, 1), PickerPanelMode::Quarter, WeekRules::ISO);
        assert_eq!(quarters.len(), 4);
        assert_eq!(quarters[0].period_end, date(2024, 3, 31));
        let years = panel_cells(date(2024, 1, 1), PickerPanelMode::Year, WeekRules::ISO);
        assert_eq!(years.len(), 12);
        assert_eq!(years[0].date, date(2019, 1, 1));
        assert!(!years[0].in_view && years[1].in_view && !years[11].in_view);
        let decades = panel_cells(date(2024, 1, 1), PickerPanelMode::Decade, WeekRules::ISO);
        assert_eq!(decades[0].period_start, date(1990, 1, 1));
        assert_eq!(decades[11].period_end, date(2109, 12, 31));
    }

    #[test]
    fn picker_calendar_constraints_keep_partially_available_coarse_cells_enabled() {
        let from = date(2024, 2, 15);
        let constraints = CalendarConstraints {
            min: Some(from),
            max: Some(date(2024, 3, 2)),
            disabled: Some(Rc::new(move |date, info| {
                assert_eq!(info.from, Some(from));
                info.mode == PickerPanelMode::Date && date.weekday() == Weekday::Sun
            })),
        };
        let months = panel_cells(from, PickerPanelMode::Month, WeekRules::SUNDAY);
        assert!(constraints.cell_disabled(&months[0], PickerPanelMode::Month, Some(from)));
        assert!(!constraints.cell_disabled(&months[1], PickerPanelMode::Month, Some(from)));
        assert!(!constraints.cell_disabled(&months[2], PickerPanelMode::Month, Some(from)));
        assert!(constraints.cell_disabled(&months[3], PickerPanelMode::Month, Some(from)));
        assert!(constraints.is_disabled(date(2024, 2, 18), PickerPanelMode::Date, Some(from)));
        assert!(!constraints.is_disabled(from, PickerPanelMode::Date, Some(from)));
        let impossible = CalendarConstraints {
            min: Some(date(2024, 2, 20)),
            max: Some(date(2024, 2, 10)),
            disabled: None,
        };
        assert!(impossible.cell_disabled(&months[1], PickerPanelMode::Month, None));
    }

    #[test]
    fn picker_calendar_coarse_callbacks_match_reference_endpoint_sampling() {
        let from = date(2024, 2, 7);
        for mode in [
            PickerPanelMode::Month,
            PickerPanelMode::Quarter,
            PickerPanelMode::Year,
            PickerPanelMode::Decade,
        ] {
            let cell = cell_for_date(date(2024, 5, 20), mode, WeekRules::SUNDAY);
            let flags = Rc::new(Cell::new((true, false)));
            let calls = Rc::new(RefCell::new(Vec::new()));
            let disabled = flags.clone();
            let output = calls.clone();
            let constraints = CalendarConstraints {
                disabled: Some(Rc::new(move |value, info| {
                    assert_eq!(
                        info,
                        CalendarDisabledInfo {
                            mode,
                            from: Some(from)
                        }
                    );
                    output.borrow_mut().push(value);
                    if value == cell.period_start {
                        disabled.get().0
                    } else {
                        assert_eq!(value, cell.period_end);
                        disabled.get().1
                    }
                })),
                ..Default::default()
            };
            let direct = mode == PickerPanelMode::Quarter;
            assert_eq!(constraints.cell_disabled(&cell, mode, Some(from)), direct);
            assert_eq!(
                *calls.borrow(),
                if direct {
                    vec![cell.date]
                } else {
                    vec![cell.period_start, cell.period_end]
                }
            );
            flags.set((true, true));
            calls.borrow_mut().clear();
            assert!(constraints.cell_disabled(&cell, mode, Some(from)));
            assert_eq!(calls.borrow().len(), if direct { 1 } else { 2 });
            flags.set((false, true));
            calls.borrow_mut().clear();
            assert!(!constraints.cell_disabled(&cell, mode, Some(from)));
            assert_eq!(*calls.borrow(), vec![cell.period_start]);
        }
        let decade = cell_for_date(date(2024, 5, 20), PickerPanelMode::Decade, WeekRules::ISO);
        assert_eq!(decade.date, date(2020, 1, 1));
        assert_eq!(decade.period_end, date(2029, 12, 31));
    }

    #[test]
    fn picker_calendar_cache_reuses_canonical_periods_and_invalidates_rules_and_modes() {
        let mut cache = CalendarCache::default();
        assert_eq!(
            cache.entries.capacity(),
            0,
            "closed controllers do not allocate grid storage"
        );
        let february = cache.cells(date(2024, 2, 1), PickerPanelMode::Date, WeekRules::SUNDAY);
        let same = cache.cells(date(2024, 2, 29), PickerPanelMode::Date, WeekRules::SUNDAY);
        assert!(Arc::ptr_eq(&february, &same));
        let monday = cache.cells(date(2024, 2, 29), PickerPanelMode::Date, WeekRules::ISO);
        assert!(!Arc::ptr_eq(&february, &monday));
        assert_ne!(february[0].date, monday[0].date);
        let months = cache.cells(date(2024, 2, 29), PickerPanelMode::Month, WeekRules::SUNDAY);
        assert!(Arc::ptr_eq(
            &months,
            &cache.cells(date(2024, 12, 1), PickerPanelMode::Month, WeekRules::SUNDAY)
        ));
        let quarters = cache.cells(
            date(2024, 2, 29),
            PickerPanelMode::Quarter,
            WeekRules::SUNDAY,
        );
        assert!(Arc::ptr_eq(
            &quarters,
            &cache.cells(
                date(2024, 12, 1),
                PickerPanelMode::Quarter,
                WeekRules::SUNDAY
            )
        ));
        let years = cache.cells(date(2021, 2, 1), PickerPanelMode::Year, WeekRules::SUNDAY);
        assert!(Arc::ptr_eq(
            &years,
            &cache.cells(date(2029, 12, 31), PickerPanelMode::Year, WeekRules::SUNDAY)
        ));
        let decades = cache.cells(date(2024, 2, 1), PickerPanelMode::Decade, WeekRules::SUNDAY);
        assert!(Arc::ptr_eq(
            &decades,
            &cache.cells(
                date(2099, 12, 31),
                PickerPanelMode::Decade,
                WeekRules::SUNDAY
            )
        ));
    }

    #[test]
    fn picker_calendar_cache_is_bounded_lru_and_time_uses_shared_empty_storage() {
        let mut cache = CalendarCache::default();
        let january = cache.cells(date(2024, 1, 1), PickerPanelMode::Date, WeekRules::SUNDAY);
        let february = cache.cells(date(2024, 2, 1), PickerPanelMode::Date, WeekRules::SUNDAY);
        for month in 3..=4 {
            cache.cells(
                date(2024, month, 1),
                PickerPanelMode::Date,
                WeekRules::SUNDAY,
            );
        }
        assert!(Arc::ptr_eq(
            &january,
            &cache.cells(date(2024, 1, 31), PickerPanelMode::Date, WeekRules::SUNDAY)
        ));
        cache.cells(date(2024, 5, 1), PickerPanelMode::Date, WeekRules::SUNDAY);
        assert_eq!(cache.entries.len(), 4);
        assert!(Arc::ptr_eq(
            &january,
            &cache.cells(date(2024, 1, 15), PickerPanelMode::Date, WeekRules::SUNDAY)
        ));
        assert!(!Arc::ptr_eq(
            &february,
            &cache.cells(date(2024, 2, 15), PickerPanelMode::Date, WeekRules::SUNDAY)
        ));
        for month in 1..=12 {
            cache.cells(
                date(2025, month, 1),
                PickerPanelMode::Date,
                WeekRules::SUNDAY,
            );
            assert_eq!(cache.entries.len(), 4);
        }
        let empty = cache.cells(date(2024, 2, 1), PickerPanelMode::Time, WeekRules::SUNDAY);
        assert!(empty.is_empty());
        assert!(Arc::ptr_eq(
            &empty,
            &cache.cells(date(2025, 12, 1), PickerPanelMode::Time, WeekRules::ISO)
        ));
        assert_eq!(
            cache.entries.len(),
            4,
            "time panels do not evict a real calendar grid"
        );
    }
}
