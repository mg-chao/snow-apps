// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Compiled chrono formats and byte-addressed numeric input masks.

use super::{
    calendar::{WeekRules, date_from_week, days_in_month, week_year_number},
    value::{PickerMode, TIME_ANCHOR},
};
use chrono::{
    NaiveDateTime, NaiveTime,
    format::{Fixed, Item, Numeric, Parsed, StrftimeItems, parse},
};
use std::{
    fmt,
    fmt::Write,
    ops::Range,
    rc::Rc,
    sync::{Arc, LazyLock},
};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum PickerLanguage {
    #[default]
    EnUs,
    ZhCn,
    ZhTw,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PickerFormatContext {
    pub mode: PickerMode,
    pub has_date: bool,
    pub has_time: bool,
    pub use_12_hours: bool,
    pub week_rules: WeekRules,
    pub language: PickerLanguage,
}

impl Default for PickerFormatContext {
    fn default() -> Self {
        Self::new(PickerMode::Date, true, false)
    }
}

impl PickerFormatContext {
    pub const fn new(mode: PickerMode, has_date: bool, has_time: bool) -> Self {
        Self {
            mode,
            has_date,
            has_time,
            use_12_hours: false,
            week_rules: WeekRules::SUNDAY,
            language: PickerLanguage::EnUs,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum PickerFormatError {
    Empty,
    InvalidPattern(String),
    TimezonePattern(String),
    UnsupportedMask(String),
    DuplicateMaskField(PickerMaskField),
}

impl fmt::Display for PickerFormatError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Empty => {
                f.write_str("a picker format must contain at least one nonempty pattern")
            }
            Self::InvalidPattern(pattern) => write!(f, "invalid chrono picker format: {pattern}"),
            Self::TimezonePattern(pattern) => write!(
                f,
                "a civil picker format cannot contain a timezone: {pattern}"
            ),
            Self::UnsupportedMask(pattern) => write!(
                f,
                "a picker mask requires fixed numeric date/time fields: {pattern}"
            ),
            Self::DuplicateMaskField(field) => {
                write!(f, "a picker mask repeats the {field:?} field")
            }
        }
    }
}
impl std::error::Error for PickerFormatError {}

#[derive(Clone, Debug, PartialEq, Eq)]
struct Pattern {
    source: Arc<str>,
    items: Arc<[Item<'static>]>,
}

impl Pattern {
    fn new(source: &str) -> Result<Self, PickerFormatError> {
        if source.is_empty() {
            return Err(PickerFormatError::Empty);
        }
        let items = StrftimeItems::new(source)
            .parse_to_owned()
            .map_err(|_| PickerFormatError::InvalidPattern(source.into()))?;
        if items.iter().any(|item| {
            matches!(
                item,
                Item::Fixed(
                    Fixed::TimezoneName
                        | Fixed::TimezoneOffsetColon
                        | Fixed::TimezoneOffsetDoubleColon
                        | Fixed::TimezoneOffsetTripleColon
                        | Fixed::TimezoneOffsetColonZ
                        | Fixed::TimezoneOffset
                        | Fixed::TimezoneOffsetZ
                        | Fixed::RFC2822
                        | Fixed::RFC3339
                )
            )
        }) {
            return Err(PickerFormatError::TimezonePattern(source.into()));
        }
        // A naive value has no offset. Test formatting through fmt::Write,
        // which reports unsupported offset fields without Display's panic.
        if write!(
            &mut String::new(),
            "{}",
            TIME_ANCHOR
                .and_time(NaiveTime::MIN)
                .format_with_items(items.iter())
        )
        .is_err()
        {
            return Err(PickerFormatError::TimezonePattern(source.into()));
        }
        Ok(Self {
            source: source.into(),
            items: items.into(),
        })
    }

    fn parse(&self, text: &str, context: PickerFormatContext) -> Option<NaiveDateTime> {
        let mut parsed = Parsed::new();
        parse(&mut parsed, text.trim(), self.items.iter()).ok()?;

        let date = if context.has_date {
            if context.mode == PickerMode::Week
                && parsed.weekday().is_none()
                && (parsed.isoweek.is_some()
                    || parsed.week_from_mon.is_some()
                    || parsed.week_from_sun.is_some())
            {
                parsed.set_weekday(context.week_rules.first_weekday).ok()?;
            }
            if context.mode == PickerMode::Quarter
                && parsed.month().is_none()
                && let Some(quarter) = parsed.quarter()
            {
                parsed.set_month(i64::from((quarter - 1) * 3 + 1)).ok()?;
            }
            if matches!(
                context.mode,
                PickerMode::Month | PickerMode::Quarter | PickerMode::Year
            ) {
                if parsed.month().is_none() {
                    parsed.set_month(1).ok()?;
                }
                if parsed.day().is_none() {
                    parsed.set_day(1).ok()?;
                }
            }
            parsed.to_naive_date().ok()?
        } else {
            TIME_ANCHOR
        };
        let time = if context.has_time {
            if parsed.hour_div_12().is_none() && parsed.hour_mod_12().is_none() {
                parsed.set_hour(0).ok()?;
            }
            if parsed.minute().is_none() {
                parsed.set_minute(0).ok()?;
            }
            if parsed.second().is_none() {
                parsed.set_second(0).ok()?;
            }
            parsed.to_naive_time().ok()?
        } else {
            NaiveTime::MIN
        };
        Some(date.and_time(time))
    }
}

type FormatCallback = Rc<dyn Fn(NaiveDateTime, PickerFormatContext) -> String>;

#[derive(Clone, Default)]
enum FormatKind {
    #[default]
    Automatic,
    Patterns(Arc<[Pattern]>),
    Custom(FormatCallback),
}

/// Retain this immutable specification between renders. Patterns are compiled
/// once; the first renders the value and every pattern is accepted for parsing.
#[derive(Clone, Default)]
pub struct PickerFormat {
    kind: FormatKind,
    mask: Option<PickerMask>,
}

/// Time columns explicitly represented by the primary compiled chrono pattern.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PickerTimeFields {
    pub hour: bool,
    pub minute: bool,
    pub second: bool,
    pub millisecond: bool,
    pub use_12_hours: bool,
}

impl fmt::Debug for PickerFormat {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let mut value = f.debug_struct("PickerFormat");
        match &self.kind {
            FormatKind::Automatic => {
                value.field("automatic", &true);
            }
            FormatKind::Patterns(patterns) => {
                value.field("patterns", patterns);
            }
            FormatKind::Custom(_) => {
                value.field("custom", &true);
            }
        }
        value.field("mask", &self.mask).finish()
    }
}

impl PartialEq for PickerFormat {
    fn eq(&self, other: &Self) -> bool {
        self.mask == other.mask
            && match (&self.kind, &other.kind) {
                (FormatKind::Automatic, FormatKind::Automatic) => true,
                (FormatKind::Patterns(a), FormatKind::Patterns(b)) => Arc::ptr_eq(a, b) || a == b,
                (FormatKind::Custom(a), FormatKind::Custom(b)) => Rc::ptr_eq(a, b),
                _ => false,
            }
    }
}
impl Eq for PickerFormat {}

impl PickerFormat {
    pub fn new(pattern: impl AsRef<str>) -> Result<Self, PickerFormatError> {
        Self::patterns([pattern])
    }

    pub fn patterns<I, S>(patterns: I) -> Result<Self, PickerFormatError>
    where
        I: IntoIterator<Item = S>,
        S: AsRef<str>,
    {
        let patterns: Vec<_> = patterns
            .into_iter()
            .map(|pattern| Pattern::new(pattern.as_ref()))
            .collect::<Result<_, _>>()?;
        if patterns.is_empty() {
            return Err(PickerFormatError::Empty);
        }
        Ok(Self {
            kind: FormatKind::Patterns(patterns.into()),
            mask: None,
        })
    }

    pub fn mask(pattern: impl AsRef<str>) -> Result<Self, PickerFormatError> {
        let mut format = Self::new(pattern.as_ref())?;
        format.mask = Some(PickerMask::new(pattern.as_ref())?);
        Ok(format)
    }

    /// A display-only callback makes the native editor read-only. Supply chrono
    /// patterns when the user should also be able to type values.
    pub fn custom(format: impl Fn(NaiveDateTime, PickerFormatContext) -> String + 'static) -> Self {
        Self {
            kind: FormatKind::Custom(Rc::new(format)),
            mask: None,
        }
    }

    pub fn primary_pattern(&self) -> Option<&str> {
        match &self.kind {
            FormatKind::Patterns(patterns) => Some(patterns[0].source.as_ref()),
            _ => None,
        }
    }
    pub fn time_fields(&self) -> Option<PickerTimeFields> {
        let FormatKind::Patterns(patterns) = &self.kind else {
            return None;
        };
        let mut fields = PickerTimeFields::default();
        for item in patterns[0].items.iter() {
            match item {
                Item::Numeric(Numeric::Hour, _) => fields.hour = true,
                Item::Numeric(Numeric::Hour12, _) => {
                    fields.hour = true;
                    fields.use_12_hours = true;
                }
                Item::Numeric(Numeric::Minute, _) => fields.minute = true,
                Item::Numeric(Numeric::Second, _) => fields.second = true,
                Item::Numeric(Numeric::Nanosecond, _)
                | Item::Fixed(
                    Fixed::Nanosecond
                    | Fixed::Nanosecond3
                    | Fixed::Nanosecond6
                    | Fixed::Nanosecond9
                    | Fixed::Internal(_),
                ) => fields.millisecond = true,
                Item::Fixed(Fixed::LowerAmPm | Fixed::UpperAmPm) => fields.use_12_hours = true,
                _ => {}
            }
        }
        Some(fields)
    }

    pub fn mask_format(&self) -> Option<&PickerMask> {
        self.mask.as_ref()
    }

    pub fn is_read_only(&self) -> bool {
        matches!(self.kind, FormatKind::Custom(_))
    }
    pub fn is_automatic(&self) -> bool {
        matches!(self.kind, FormatKind::Automatic)
    }

    pub fn format(&self, value: NaiveDateTime, context: PickerFormatContext) -> String {
        match &self.kind {
            FormatKind::Automatic => default_format(value, context),
            FormatKind::Patterns(patterns) => value
                .format_with_items(patterns[0].items.iter())
                .to_string(),
            FormatKind::Custom(format) => format(value, context),
        }
    }

    pub fn parse(&self, text: &str, context: PickerFormatContext) -> Option<NaiveDateTime> {
        match &self.kind {
            FormatKind::Automatic => default_parse(text, context),
            FormatKind::Patterns(patterns) => patterns
                .iter()
                .find_map(|pattern| pattern.parse(text, context)),
            FormatKind::Custom(_) => None,
        }
    }
}

fn default_pattern(context: PickerFormatContext) -> &'static Pattern {
    static PATTERNS: LazyLock<[Pattern; 8]> = LazyLock::new(|| {
        [
            "%H:%M:%S",
            "%I:%M:%S %p",
            "%Y-%m-%d %H:%M:%S",
            "%Y-%m-%d %I:%M:%S %p",
            "%Y-%m-%d",
            "%Y-%m",
            "%Y-Q%q",
            "%Y",
        ]
        .map(|pattern| Pattern::new(pattern).expect("built-in picker format is valid"))
    });
    let index = if !context.has_date {
        if context.use_12_hours { 1 } else { 0 }
    } else if context.has_time {
        if context.use_12_hours { 3 } else { 2 }
    } else {
        match context.mode {
            PickerMode::Date | PickerMode::Week => 4,
            PickerMode::Month => 5,
            PickerMode::Quarter => 6,
            PickerMode::Year => 7,
        }
    };
    &PATTERNS[index]
}

fn ordinal_suffix(number: u32) -> &'static str {
    if (11..=13).contains(&(number % 100)) {
        return "th";
    }
    match number % 10 {
        1 => "st",
        2 => "nd",
        3 => "rd",
        _ => "th",
    }
}

fn default_format(value: NaiveDateTime, context: PickerFormatContext) -> String {
    if context.has_date && !context.has_time && context.mode == PickerMode::Week {
        let (year, week) = week_year_number(value.date(), context.week_rules);
        let suffix = match context.language {
            PickerLanguage::EnUs => ordinal_suffix(week),
            PickerLanguage::ZhCn => "周",
            PickerLanguage::ZhTw => "週",
        };
        format!("{year:04}-{week}{suffix}")
    } else {
        value
            .format_with_items(default_pattern(context).items.iter())
            .to_string()
    }
}

fn default_parse(text: &str, context: PickerFormatContext) -> Option<NaiveDateTime> {
    if context.has_date && !context.has_time && context.mode == PickerMode::Week {
        let (year, rest) = text.trim().rsplit_once('-')?;
        let year = year.parse::<i32>().ok()?;
        let number_end = rest.bytes().take_while(u8::is_ascii_digit).count();
        let week = rest.get(..number_end)?.parse::<u32>().ok()?;
        let suffix = rest.get(number_end..)?;
        let expected = match context.language {
            PickerLanguage::EnUs => ordinal_suffix(week),
            PickerLanguage::ZhCn => "周",
            PickerLanguage::ZhTw => "週",
        };
        if suffix != expected {
            return None;
        }
        return Some(date_from_week(year, week, context.week_rules)?.and_time(NaiveTime::MIN));
    }
    default_pattern(context).parse(text, context)
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum PickerMaskField {
    Year,
    Month,
    Day,
    Hour,
    Minute,
    Second,
    Millisecond,
}

impl PickerMaskField {
    const fn placeholder(self) -> &'static str {
        match self {
            Self::Year => "YYYY",
            Self::Month => "MM",
            Self::Day => "DD",
            Self::Hour => "HH",
            Self::Minute => "mm",
            Self::Second => "ss",
            Self::Millisecond => "SSS",
        }
    }
    const fn limits(self) -> (u32, u32) {
        match self {
            Self::Year => (1, 9999),
            Self::Month => (1, 12),
            Self::Day => (1, 31),
            Self::Hour => (0, 23),
            Self::Minute | Self::Second => (0, 59),
            Self::Millisecond => (0, 999),
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PickerMaskSegment {
    pub field: PickerMaskField,
    /// UTF-8 byte offsets, matching InputState's native text selection model.
    pub range: Range<usize>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PickerMask {
    template: Arc<str>,
    segments: Arc<[PickerMaskSegment]>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PickerMaskEdit {
    pub text: String,
    pub segment: usize,
    pub pending_digits: String,
}

impl PickerMask {
    pub fn new(pattern: &str) -> Result<Self, PickerFormatError> {
        Pattern::new(pattern)?;
        let mut template = String::new();
        let mut segments = Vec::new();
        Self::append_pattern(pattern, &mut template, &mut segments)?;
        if segments.is_empty() {
            return Err(PickerFormatError::UnsupportedMask(pattern.into()));
        }
        Ok(Self {
            template: template.into(),
            segments: segments.into(),
        })
    }

    fn append_pattern(
        pattern: &str,
        template: &mut String,
        segments: &mut Vec<PickerMaskSegment>,
    ) -> Result<(), PickerFormatError> {
        let mut rest = pattern;
        while !rest.is_empty() {
            let Some(percent) = rest.find('%') else {
                template.push_str(rest);
                break;
            };
            template.push_str(&rest[..percent]);
            rest = &rest[percent..];
            if let Some(next) = rest.strip_prefix("%%") {
                template.push('%');
                rest = next;
                continue;
            }
            let mut matched = false;
            for (token, expansion) in [("%F", "%Y-%m-%d"), ("%T", "%H:%M:%S"), ("%R", "%H:%M")] {
                if let Some(next) = rest.strip_prefix(token) {
                    Self::append_pattern(expansion, template, segments)?;
                    rest = next;
                    matched = true;
                    break;
                }
            }
            if matched {
                continue;
            }
            let mut found = None;
            for (token, field, dot) in [
                ("%.3f", PickerMaskField::Millisecond, true),
                ("%3f", PickerMaskField::Millisecond, false),
                ("%Y", PickerMaskField::Year, false),
                ("%m", PickerMaskField::Month, false),
                ("%d", PickerMaskField::Day, false),
                ("%H", PickerMaskField::Hour, false),
                ("%M", PickerMaskField::Minute, false),
                ("%S", PickerMaskField::Second, false),
            ] {
                if rest.starts_with(token) {
                    found = Some((token, field, dot));
                    break;
                }
            }
            let Some((token, field, dot)) = found else {
                return Err(PickerFormatError::UnsupportedMask(pattern.into()));
            };
            if segments.iter().any(|segment| segment.field == field) {
                return Err(PickerFormatError::DuplicateMaskField(field));
            }
            if dot {
                template.push('.');
            }
            let start = template.len();
            template.push_str(field.placeholder());
            segments.push(PickerMaskSegment {
                field,
                range: start..template.len(),
            });
            rest = &rest[token.len()..];
        }
        Ok(())
    }

    pub fn template(&self) -> &str {
        &self.template
    }
    pub fn segments(&self) -> &[PickerMaskSegment] {
        &self.segments
    }
    pub fn selection(&self, segment: usize) -> Option<Range<usize>> {
        self.segments
            .get(segment)
            .map(|segment| segment.range.clone())
    }

    pub fn segment_at(&self, byte_offset: usize) -> usize {
        self.segments
            .iter()
            .enumerate()
            .min_by_key(|(_, segment)| {
                if byte_offset < segment.range.start {
                    segment.range.start - byte_offset
                } else {
                    byte_offset.saturating_sub(segment.range.end)
                }
            })
            .map_or(0, |(index, _)| index)
    }

    pub fn matches(&self, text: &str) -> bool {
        if text.len() != self.template.len() {
            return false;
        }
        let mut cursor = 0;
        for segment in self.segments.iter() {
            if text.get(cursor..segment.range.start)
                != self.template.get(cursor..segment.range.start)
            {
                return false;
            }
            let Some(content) = text.get(segment.range.clone()) else {
                return false;
            };
            if content != segment.field.placeholder()
                && !content.bytes().all(|byte| byte.is_ascii_digit())
            {
                return false;
            }
            cursor = segment.range.end;
        }
        text.get(cursor..) == self.template.get(cursor..)
    }

    pub fn is_complete(&self, text: &str) -> bool {
        self.matches(text)
            && self.segments.iter().all(|segment| {
                text[segment.range.clone()]
                    .bytes()
                    .all(|byte| byte.is_ascii_digit())
            })
    }

    pub fn set_segment(&self, text: &str, segment: usize, digits: &str) -> Option<String> {
        let selected = self.segments.get(segment)?;
        if digits.is_empty() {
            return self.delete(text, segment);
        }
        if digits.len() > selected.range.len() || !digits.bytes().all(|byte| byte.is_ascii_digit())
        {
            return None;
        }
        let mut value = if self.matches(text) {
            text.to_owned()
        } else {
            self.template.to_string()
        };
        value.replace_range(
            selected.range.clone(),
            &format!("{digits:0>width$}", width = selected.range.len()),
        );
        Some(value)
    }

    pub fn delete(&self, text: &str, segment: usize) -> Option<String> {
        let selected = self.segments.get(segment)?;
        let mut value = if self.matches(text) {
            text.to_owned()
        } else {
            self.template.to_string()
        };
        value.replace_range(selected.range.clone(), selected.field.placeholder());
        Some(value)
    }

    pub fn step(&self, text: &str, segment: usize, delta: i32) -> Option<String> {
        let selected = self.segments.get(segment)?;
        let (minimum, maximum) = selected.field.limits();
        let current = text
            .get(selected.range.clone())
            .and_then(|text| text.parse::<u32>().ok());
        let next = match current {
            Some(current) => {
                (i64::from(current) - i64::from(minimum) + i64::from(delta))
                    .rem_euclid(i64::from(maximum - minimum + 1))
                    + i64::from(minimum)
            }
            None => i64::from(if delta >= 0 { minimum } else { maximum }),
        };
        self.set_segment(text, segment, &next.to_string())
    }

    pub fn input_digit(
        &self,
        text: &str,
        segment: usize,
        digit: char,
        pending: &str,
    ) -> Option<PickerMaskEdit> {
        let selected = self.segments.get(segment)?;
        if !digit.is_ascii_digit() {
            return None;
        }
        let mut pending_digits = if pending.len() < selected.range.len()
            && pending.bytes().all(|byte| byte.is_ascii_digit())
        {
            pending.to_owned()
        } else {
            String::new()
        };
        pending_digits.push(digit);
        let text = self.set_segment(text, segment, &pending_digits)?;
        let advance = pending_digits.len() == selected.range.len();
        Some(PickerMaskEdit {
            text,
            segment: if advance {
                (segment + 1).min(self.segments.len() - 1)
            } else {
                segment
            },
            pending_digits: if advance {
                String::new()
            } else {
                pending_digits
            },
        })
    }

    /// Normalize a completely filled mask on blur. Incomplete masks remain
    /// invalid; they never acquire a fabricated date or time.
    pub fn repair(&self, text: &str) -> Option<String> {
        if !self.is_complete(text) {
            return None;
        }
        let mut values: Vec<_> = self
            .segments
            .iter()
            .map(|segment| {
                let (min, max) = segment.field.limits();
                text[segment.range.clone()]
                    .parse::<u32>()
                    .ok()
                    .map(|value| value.clamp(min, max))
            })
            .collect::<Option<_>>()?;
        let field = |kind| {
            self.segments
                .iter()
                .position(|segment| segment.field == kind)
        };
        if let (Some(year), Some(month), Some(day)) = (
            field(PickerMaskField::Year),
            field(PickerMaskField::Month),
            field(PickerMaskField::Day),
        ) {
            values[day] = values[day].min(days_in_month(values[year] as i32, values[month])?);
        }
        let mut repaired = text.to_owned();
        for (index, value) in values.into_iter().enumerate() {
            repaired = self.set_segment(&repaired, index, &value.to_string())?;
        }
        Some(repaired)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use chrono::{Datelike, NaiveDate};

    fn value() -> NaiveDateTime {
        NaiveDate::from_ymd_opt(2024, 2, 29)
            .unwrap()
            .and_hms_milli_opt(23, 7, 5, 123)
            .unwrap()
    }

    #[test]
    fn picker_format_patterns_compile_once_and_parse_all_alternatives() {
        let format = PickerFormat::patterns(["%Y/%m/%d", "%d-%m-%Y"]).unwrap();
        assert_eq!(format.primary_pattern(), Some("%Y/%m/%d"));
        assert_eq!(
            format.format(value(), PickerFormatContext::default()),
            "2024/02/29"
        );
        assert_eq!(
            format
                .parse("29-02-2024", PickerFormatContext::default())
                .unwrap()
                .date(),
            value().date()
        );
        assert!(
            format
                .parse("29-02-2023", PickerFormatContext::default())
                .is_none()
        );
        assert!(
            format
                .parse("2024/02/29 trailing", PickerFormatContext::default())
                .is_none()
        );
        assert!(PickerFormat::patterns::<[&str; 0], &str>([]).is_err());
        assert!(PickerFormat::new("%invalid").is_err());
        assert!(PickerFormat::new("%Y-%m-%d %z").is_err());
    }

    #[test]
    fn picker_format_civil_time_preserves_meridiem_and_milliseconds() {
        let context = PickerFormatContext::new(PickerMode::Date, true, true);
        let format = PickerFormat::new("%Y-%m-%d %I:%M:%S%.3f %p").unwrap();
        assert_eq!(
            format.format(value(), context),
            "2024-02-29 11:07:05.123 PM"
        );
        assert_eq!(
            format.parse("2024-02-29 11:07:05.123 PM", context),
            Some(value())
        );
        let time_context = PickerFormatContext::new(PickerMode::Date, false, true);
        let time = PickerFormat::new("%H:%M")
            .unwrap()
            .parse("12:08", time_context)
            .unwrap();
        assert_eq!(time.date(), TIME_ANCHOR);
        assert_eq!(time.time(), NaiveTime::from_hms_opt(12, 8, 0).unwrap());
        assert!(
            PickerFormat::new("%I:%M")
                .unwrap()
                .parse("11:08", time_context)
                .is_none()
        );
    }

    #[test]
    fn picker_format_defaults_cover_year_month_quarter_and_localized_week() {
        let format = PickerFormat::default();
        for (mode, text) in [
            (PickerMode::Year, "2024"),
            (PickerMode::Month, "2024-02"),
            (PickerMode::Quarter, "2024-Q1"),
        ] {
            let context = PickerFormatContext::new(mode, true, false);
            assert_eq!(format.format(value(), context), text);
            assert!(format.parse(text, context).is_some());
        }
        let edge = NaiveDate::from_ymd_opt(2021, 1, 1)
            .unwrap()
            .and_time(NaiveTime::MIN);
        let mut context = PickerFormatContext::new(PickerMode::Week, true, false);
        assert_eq!(format.format(edge, context), "2021-1st");
        context.week_rules = WeekRules::ISO;
        context.language = PickerLanguage::ZhCn;
        assert_eq!(format.format(edge, context), "2020-53周");
        assert_eq!(
            format.parse("2020-53周", context).unwrap().date(),
            NaiveDate::from_ymd_opt(2020, 12, 28).unwrap()
        );
        assert!(format.parse("2021-53周", context).is_none());
        context.language = PickerLanguage::ZhTw;
        context.week_rules = WeekRules::SUNDAY;
        assert_eq!(format.format(edge, context), "2021-1週");
    }

    #[test]
    fn picker_format_custom_display_is_read_only_and_clone_stable() {
        let format = PickerFormat::custom(|date, _| format!("Day {}", date.day()));
        assert!(format.is_read_only());
        assert_eq!(format, format.clone());
        assert_eq!(
            format.format(value(), PickerFormatContext::default()),
            "Day 29"
        );
        assert!(
            format
                .parse("Day 29", PickerFormatContext::default())
                .is_none()
        );
    }

    #[test]
    fn picker_format_column_inference_uses_tokens_not_literal_substrings() {
        let fields = PickerFormat::new("%%H %M off")
            .unwrap()
            .time_fields()
            .unwrap();
        assert_eq!(
            fields,
            PickerTimeFields {
                minute: true,
                ..Default::default()
            }
        );
        let fields = PickerFormat::new("%r%.3f").unwrap().time_fields().unwrap();
        assert_eq!(
            fields,
            PickerTimeFields {
                hour: true,
                minute: true,
                second: true,
                millisecond: true,
                use_12_hours: true
            }
        );
        assert_eq!(PickerFormat::default().time_fields(), None);
        assert!(PickerFormat::new("%#z").is_err());
    }

    #[test]
    fn picker_mask_segments_use_utf8_byte_selection_and_literal_escapes() {
        let mask = PickerMask::new("%Y年%m月%d日 %H:%M:%S%.3f %%").unwrap();
        assert_eq!(mask.template(), "YYYY年MM月DD日 HH:mm:ss.SSS %");
        assert_eq!(mask.selection(1), Some(7..9));
        assert_eq!(mask.segment_at(8), 1);
        assert!(mask.matches("2024年02月29日 23:07:05.123 %"));
        assert!(mask.matches(mask.template()));
        assert!(!mask.is_complete(mask.template()));
        assert!(!mask.matches("2024年02月29日 23-07:05.123 %"));
        assert_eq!(
            PickerMask::new("%F %T").unwrap().template(),
            "YYYY-MM-DD HH:mm:ss"
        );
        assert!(PickerMask::new("%Y-%B-%d").is_err());
        assert!(PickerMask::new("%Y %Y").is_err());
    }

    #[test]
    fn picker_mask_digit_entry_advances_deletes_and_wraps_segments() {
        let mask = PickerMask::new("%Y-%m-%d").unwrap();
        let one = mask.input_digit("", 0, '2', "").unwrap();
        assert_eq!(one.text, "0002-MM-DD");
        let two = mask
            .input_digit(&one.text, one.segment, '0', &one.pending_digits)
            .unwrap();
        let three = mask
            .input_digit(&two.text, two.segment, '2', &two.pending_digits)
            .unwrap();
        let four = mask
            .input_digit(&three.text, three.segment, '4', &three.pending_digits)
            .unwrap();
        assert_eq!(four.text, "2024-MM-DD");
        assert_eq!(four.segment, 1);
        assert!(four.pending_digits.is_empty());
        assert_eq!(mask.step("2024-12-31", 1, 1).as_deref(), Some("2024-01-31"));
        assert_eq!(
            mask.step("2024-01-31", 1, -1).as_deref(),
            Some("2024-12-31")
        );
        assert_eq!(mask.delete("2024-12-31", 2).as_deref(), Some("2024-12-DD"));
        assert!(mask.input_digit("", 0, '日', "").is_none());
    }

    #[test]
    fn picker_mask_blur_repairs_complete_dates_and_rejects_incomplete_input() {
        let mask = PickerMask::new("%Y-%m-%d %H:%M:%S.%3f").unwrap();
        assert_eq!(
            mask.repair("2024-02-31 25:99:99.999").as_deref(),
            Some("2024-02-29 23:59:59.999")
        );
        assert_eq!(
            mask.repair("2023-02-29 00:00:00.001").as_deref(),
            Some("2023-02-28 00:00:00.001")
        );
        assert!(mask.repair("2024-02-DD 00:00:00.001").is_none());
        let format = PickerFormat::mask("%F %T%.3f").unwrap();
        let context = PickerFormatContext::new(PickerMode::Date, true, true);
        let repaired = format
            .mask_format()
            .unwrap()
            .repair("2024-02-31 23:07:05.123")
            .unwrap();
        assert_eq!(format.parse(&repaired, context), Some(value()));
    }
}
