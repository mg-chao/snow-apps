// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::{fmt, rc::Rc};

use super::{DecimalError, ExactDecimal, MAX_DECIMAL_DIGITS, decimal::Rounding};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum NumericMode {
    #[default]
    Number,
    ExactDecimal,
}

#[derive(Clone, Debug, PartialEq)]
pub enum NumericValue {
    Number(f64),
    Decimal(ExactDecimal),
}

impl NumericValue {
    pub fn decimal(&self) -> Result<ExactDecimal, DecimalError> {
        match self {
            Self::Number(number) => ExactDecimal::from_number(*number),
            Self::Decimal(decimal) => Ok(decimal.clone()),
        }
    }
    pub fn to_number(&self) -> Result<f64, DecimalError> {
        self.decimal()?.to_number()
    }
}
impl From<ExactDecimal> for NumericValue {
    fn from(value: ExactDecimal) -> Self {
        Self::Decimal(value)
    }
}
impl From<f64> for NumericValue {
    fn from(value: f64) -> Self {
        Self::Number(value)
    }
}
impl fmt::Display for NumericValue {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Number(value) => value.fmt(f),
            Self::Decimal(value) => value.fmt(f),
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum NumericDraft {
    Empty,
    Incomplete,
    Valid(ExactDecimal),
    Invalid,
    Composing,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum NumberError {
    Decimal(DecimalError),
    InvalidRange,
    InvalidStep,
}
impl From<DecimalError> for NumberError {
    fn from(error: DecimalError) -> Self {
        Self::Decimal(error)
    }
}
impl fmt::Display for NumberError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Decimal(error) => error.fmt(f),
            Self::InvalidRange => f.write_str("Minimum exceeds maximum"),
            Self::InvalidStep => f.write_str("Numeric step must be positive"),
        }
    }
}
impl std::error::Error for NumberError {}

pub type NumberParser = Rc<dyn Fn(&str) -> String>;
pub type NumberFormatter = Rc<dyn Fn(NumberFormatContext<'_>) -> String>;

/// `input` is the untouched draft while typing. Return it to defer formatting
/// until commit. `canonical` includes the configured precision on commit.
pub struct NumberFormatContext<'a> {
    pub value: Option<&'a NumericValue>,
    pub input: &'a str,
    pub canonical: &'a str,
    pub user_typing: bool,
}

/// Numeric behavior belongs to retained state, so replacing a builder does not
/// reset the draft, parser, accepted value, or stepping configuration.
#[derive(Clone)]
pub struct NumberOptions {
    pub mode: NumericMode,
    pub minimum: Option<ExactDecimal>,
    pub maximum: Option<ExactDecimal>,
    pub step: ExactDecimal,
    pub precision: Option<u32>,
    pub keyboard: bool,
    pub change_on_wheel: bool,
    pub change_on_blur: bool,
    pub parser: Option<NumberParser>,
    pub formatter: Option<NumberFormatter>,
}

impl Default for NumberOptions {
    fn default() -> Self {
        Self {
            mode: NumericMode::Number,
            minimum: None,
            maximum: None,
            step: 1.into(),
            precision: None,
            keyboard: true,
            change_on_wheel: false,
            change_on_blur: true,
            parser: None,
            formatter: None,
        }
    }
}

impl NumberOptions {
    pub fn exact() -> Self {
        Self {
            mode: NumericMode::ExactDecimal,
            ..Self::default()
        }
    }
    pub fn validate(&self) -> Result<(), NumberError> {
        if let (Some(minimum), Some(maximum)) = (&self.minimum, &self.maximum)
            && minimum > maximum
        {
            return Err(NumberError::InvalidRange);
        }
        if self.step <= ExactDecimal::default() {
            return Err(NumberError::InvalidStep);
        }
        if self
            .precision
            .is_some_and(|precision| precision as usize > MAX_DECIMAL_DIGITS)
        {
            return Err(DecimalError::TooLarge.into());
        }
        if self.mode == NumericMode::Number {
            for number in self
                .minimum
                .iter()
                .chain(self.maximum.iter())
                .chain(std::iter::once(&self.step))
            {
                number.to_number()?;
            }
            if self.step.to_number()? == 0. {
                return Err(NumberError::InvalidStep);
            }
            let (minimum, maximum) = self.number_bounds()?;
            if minimum > maximum || minimum == f64::INFINITY || maximum == f64::NEG_INFINITY {
                return Err(NumberError::InvalidRange);
            }
        }
        Ok(())
    }

    pub fn parse(&self, draft: &str) -> NumericDraft {
        let parsed = self
            .parser
            .as_ref()
            .map_or_else(|| draft.replace('。', "."), |parser| parser(draft));
        let parsed = parsed.trim();
        if parsed.is_empty() {
            return NumericDraft::Empty;
        }
        if matches!(parsed, "-" | "+" | "." | "-." | "+.") {
            return NumericDraft::Incomplete;
        }
        if let Some(index) = parsed.find(['e', 'E'])
            && matches!(&parsed[index + 1..], "" | "-" | "+")
            && parsed[..index].parse::<ExactDecimal>().is_ok()
        {
            return NumericDraft::Incomplete;
        }
        match parsed.parse::<ExactDecimal>() {
            Ok(decimal)
                if self.mode == NumericMode::ExactDecimal || decimal.to_number().is_ok() =>
            {
                NumericDraft::Valid(decimal)
            }
            _ => NumericDraft::Invalid,
        }
    }

    fn in_range(&self, value: &ExactDecimal) -> bool {
        self.minimum.as_ref().is_none_or(|minimum| value >= minimum)
            && self.maximum.as_ref().is_none_or(|maximum| value <= maximum)
    }

    fn clamp(&self, value: ExactDecimal) -> ExactDecimal {
        if let Some(minimum) = &self.minimum
            && value < *minimum
        {
            return minimum.clone();
        }
        if let Some(maximum) = &self.maximum
            && value > *maximum
        {
            return maximum.clone();
        }
        value
    }

    fn normalize(&self, value: ExactDecimal) -> Result<ExactDecimal, NumberError> {
        let value = self.clamp(value);
        let Some(precision) = self.precision else {
            return Ok(value);
        };
        let rounded = value.round(precision)?;
        if self.in_range(&rounded) {
            return Ok(rounded);
        }
        let inward = value.round_with(
            precision,
            if self
                .minimum
                .as_ref()
                .is_some_and(|minimum| rounded < *minimum)
            {
                Rounding::Ceiling
            } else {
                Rounding::Floor
            },
        )?;
        // A narrow interval may contain no value at the requested precision.
        // The range is authoritative; do not emit an out-of-range rounded value.
        Ok(if self.in_range(&inward) {
            inward
        } else {
            value
        })
    }

    fn value(&self, decimal: ExactDecimal) -> Result<NumericValue, NumberError> {
        Ok(match self.mode {
            NumericMode::Number => NumericValue::Number(decimal.to_number()?),
            NumericMode::ExactDecimal => NumericValue::Decimal(decimal),
        })
    }

    fn number_bounds(&self) -> Result<(f64, f64), NumberError> {
        let minimum = match &self.minimum {
            Some(decimal) => {
                let number = decimal.to_number()?;
                if ExactDecimal::from_number(number)? < *decimal {
                    number.next_up()
                } else {
                    number
                }
            }
            None => f64::NEG_INFINITY,
        };
        let maximum = match &self.maximum {
            Some(decimal) => {
                let number = decimal.to_number()?;
                if ExactDecimal::from_number(number)? > *decimal {
                    number.next_down()
                } else {
                    number
                }
            }
            None => f64::INFINITY,
        };
        Ok((minimum, maximum))
    }

    fn accepted_value(&self, decimal: ExactDecimal) -> Result<NumericValue, NumberError> {
        let value = self.value(decimal)?;
        match value {
            NumericValue::Number(number) => {
                // Conversion itself can cross an exact range boundary. Clamp to
                // the nearest representable value inside the configured interval.
                let (minimum, maximum) = self.number_bounds()?;
                Ok(NumericValue::Number(number.clamp(minimum, maximum)))
            }
            value => Ok(value),
        }
    }
}

pub(super) struct NumberUpdate {
    pub changed: bool,
    pub display: String,
    pub draft: NumericDraft,
}

pub(super) struct NumberModel {
    pub options: NumberOptions,
    pub value: Option<NumericValue>,
}

impl NumberModel {
    pub fn new(options: NumberOptions) -> Result<Self, NumberError> {
        options.validate()?;
        Ok(Self {
            options,
            value: None,
        })
    }

    pub fn set_value(&mut self, value: Option<NumericValue>) -> Result<String, NumberError> {
        // External values may intentionally exceed the range. They are reflected
        // without clamping and never synthesize a user change.
        let value = value
            .map(|value| self.options.value(value.decimal()?))
            .transpose()?;
        self.value = value;
        Ok(self.format("", false))
    }

    pub fn input(&mut self, input: &str, composing: bool) -> Result<NumberUpdate, NumberError> {
        if composing {
            return Ok(NumberUpdate {
                changed: false,
                display: input.to_owned(),
                draft: NumericDraft::Composing,
            });
        }
        let draft = self.options.parse(input);
        let next = match &draft {
            NumericDraft::Empty => Some(None),
            NumericDraft::Valid(value) if self.options.in_range(value) => {
                Some(Some(self.options.accepted_value(value.clone())?))
            }
            _ => None,
        };
        let changed = next.as_ref().is_some_and(|value| *value != self.value);
        if let Some(value) = next {
            self.value = value;
        }
        Ok(NumberUpdate {
            changed,
            display: self.format(input, true),
            draft,
        })
    }

    pub fn commit(&mut self, input: &str) -> Result<NumberUpdate, NumberError> {
        let draft = self.options.parse(input);
        let decimal = match &draft {
            NumericDraft::Empty => None,
            NumericDraft::Valid(value) => Some(value.clone()),
            _ => self.value.as_ref().map(NumericValue::decimal).transpose()?,
        };
        let next = decimal
            .map(|value| self.options.accepted_value(self.options.normalize(value)?))
            .transpose()?;
        let changed = next != self.value;
        self.value = next;
        Ok(NumberUpdate {
            changed,
            display: self.format(input, false),
            draft,
        })
    }

    pub fn can_step(&self, up: bool) -> bool {
        let Some(value) = self.value.as_ref().and_then(|value| value.decimal().ok()) else {
            return true;
        };
        if up {
            self.options
                .maximum
                .as_ref()
                .is_none_or(|maximum| value < *maximum)
        } else {
            self.options
                .minimum
                .as_ref()
                .is_none_or(|minimum| value > *minimum)
        }
    }

    pub fn step(
        &mut self,
        up: bool,
        accelerated: bool,
    ) -> Result<Option<NumberUpdate>, NumberError> {
        if !self.can_step(up) {
            return Ok(None);
        }
        let base = self
            .value
            .as_ref()
            .map(NumericValue::decimal)
            .transpose()?
            .unwrap_or_default();
        let offset = self
            .options
            .step
            .checked_multiply_integer(if accelerated { 10 } else { 1 } * if up { 1 } else { -1 })?;
        let value = self.options.normalize(base.checked_add(&offset)?)?;
        let next = Some(self.options.accepted_value(value)?);
        let changed = next != self.value;
        self.value = next;
        Ok(Some(NumberUpdate {
            changed,
            display: self.format("", false),
            draft: self
                .value
                .as_ref()
                .map(|value| value.decimal().map(NumericDraft::Valid))
                .transpose()?
                .unwrap_or(NumericDraft::Empty),
        }))
    }

    fn format(&self, input: &str, user_typing: bool) -> String {
        let canonical = self
            .value
            .as_ref()
            .map(|value| {
                let decimal = value.decimal().expect("accepted numeric value");
                let precision = self.options.precision.unwrap_or_else(|| {
                    decimal
                        .decimal_places()
                        .max(self.options.step.decimal_places())
                });
                if !user_typing
                    && decimal
                        .round(precision)
                        .is_ok_and(|rounded| rounded == decimal)
                {
                    decimal.to_fixed(precision).expect("validated precision")
                } else {
                    decimal.to_string()
                }
            })
            .unwrap_or_default();
        match &self.options.formatter {
            Some(formatter) => formatter(NumberFormatContext {
                value: self.value.as_ref(),
                input,
                canonical: &canonical,
                user_typing,
            }),
            None if user_typing => input.to_owned(),
            None => canonical,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn decimal(value: &str) -> ExactDecimal {
        value.parse().unwrap()
    }
    fn model() -> NumberModel {
        NumberModel::new(NumberOptions::exact()).unwrap()
    }
    fn text(model: &NumberModel) -> Option<String> {
        model.value.as_ref().map(ToString::to_string)
    }

    #[test]
    fn drafts_keep_intermediate_and_out_of_range_text_until_commit() {
        let mut model = NumberModel::new(NumberOptions {
            minimum: Some(decimal("-10")),
            maximum: Some(decimal("10")),
            ..NumberOptions::exact()
        })
        .unwrap();
        assert!(model.input("2", false).unwrap().changed);
        for input in ["-", "+", ".", "1e-", "invalid", "99"] {
            let update = model.input(input, false).unwrap();
            assert_eq!(update.display, input);
            assert!(!update.changed);
            assert_eq!(text(&model).as_deref(), Some("2"));
        }
        assert_eq!(model.commit("99").unwrap().display, "10");
        assert_eq!(model.commit("invalid").unwrap().display, "10");
        assert_eq!(model.commit("").unwrap().display, "");
        assert_eq!(text(&model), None);
    }

    #[test]
    fn trailing_decimal_zeroes_and_exponents_are_not_rewritten_while_typing() {
        let mut model = model();
        for (input, value) in [
            ("1.", "1"),
            ("1.20", "1.2"),
            ("1e3", "1000"),
            ("1。5", "1.5"),
        ] {
            assert_eq!(model.input(input, false).unwrap().display, input);
            assert_eq!(text(&model).as_deref(), Some(value));
        }
        assert_eq!(model.commit("1。5").unwrap().display, "1.5");
    }

    #[test]
    fn precision_is_deferred_and_ranges_win_when_rounding_crosses_a_boundary() {
        let mut model = NumberModel::new(NumberOptions {
            precision: Some(2),
            ..NumberOptions::exact()
        })
        .unwrap();
        model.input("1.005", false).unwrap();
        assert_eq!(text(&model).as_deref(), Some("1.005"));
        assert_eq!(model.commit("1.005").unwrap().display, "1.01");
        model.options.precision = Some(0);
        model.options.maximum = Some(decimal("3.8"));
        assert_eq!(model.commit("4").unwrap().display, "3");
        model.options.minimum = Some(decimal("3.5"));
        assert_eq!(model.commit("4").unwrap().display, "3.8");
        assert_eq!(model.commit("3").unwrap().display, "3.5");
    }

    #[test]
    fn exact_steps_support_empty_acceleration_limits_and_external_outliers() {
        let mut model = NumberModel::new(NumberOptions {
            step: decimal("0.1"),
            minimum: Some(decimal("-1")),
            maximum: Some(decimal("1")),
            ..NumberOptions::exact()
        })
        .unwrap();
        assert_eq!(model.step(true, false).unwrap().unwrap().display, "0.1");
        assert_eq!(model.step(true, true).unwrap().unwrap().display, "1.0");
        assert!(model.step(true, false).unwrap().is_none());
        assert_eq!(model.step(false, true).unwrap().unwrap().display, "0.0");
        assert_eq!(model.set_value(Some(decimal("99").into())).unwrap(), "99.0");
        assert!(!model.can_step(true));
        assert_eq!(model.step(false, false).unwrap().unwrap().display, "1.0");
    }

    #[test]
    fn custom_parser_and_formatter_receive_typing_context() {
        let mut model = NumberModel::new(NumberOptions {
            parser: Some(Rc::new(|input| input.replace(['$', ','], ""))),
            formatter: Some(Rc::new(|context| {
                if context.user_typing {
                    context.input.to_owned()
                } else if context.value.is_some() {
                    format!("$ {}", context.canonical)
                } else {
                    String::new()
                }
            })),
            precision: Some(2),
            ..NumberOptions::exact()
        })
        .unwrap();
        assert_eq!(
            model.input("$ 1,234.50", false).unwrap().display,
            "$ 1,234.50"
        );
        assert_eq!(text(&model).as_deref(), Some("1234.5"));
        assert_eq!(model.commit("$ 1,234.50").unwrap().display, "$ 1234.50");
        assert_eq!(model.commit("").unwrap().display, "");
    }

    #[test]
    fn composition_and_invalid_standard_numbers_do_not_change_accepted_values() {
        let mut model = NumberModel::new(NumberOptions::default()).unwrap();
        model.input("2", false).unwrap();
        assert_eq!(
            model.input("30", true).unwrap().draft,
            NumericDraft::Composing
        );
        assert_eq!(model.value, Some(NumericValue::Number(2.)));
        assert_eq!(
            model.input("1e400", false).unwrap().draft,
            NumericDraft::Invalid
        );
        assert!(
            model
                .set_value(Some(NumericValue::Number(f64::NAN)))
                .is_err()
        );
        assert_eq!(model.value, Some(NumericValue::Number(2.)));
        model.input("0.1", false).unwrap();
        model.options.step = decimal("0.2");
        assert_eq!(model.step(true, false).unwrap().unwrap().display, "0.3");
    }

    #[test]
    fn invalid_configuration_is_rejected_before_editing() {
        assert!(matches!(
            NumberModel::new(NumberOptions {
                step: decimal("0"),
                ..Default::default()
            }),
            Err(NumberError::InvalidStep)
        ));
        assert!(matches!(
            NumberModel::new(NumberOptions {
                minimum: Some(decimal("2")),
                maximum: Some(decimal("1")),
                ..Default::default()
            }),
            Err(NumberError::InvalidRange)
        ));
    }

    #[test]
    fn standard_conversion_respects_exact_bounds_and_rejects_unrepresentable_intervals() {
        let mut model = NumberModel::new(NumberOptions {
            minimum: Some(decimal("9007199254740993")),
            maximum: Some(decimal("9007199254740997")),
            ..Default::default()
        })
        .unwrap();
        model.input("9007199254740993", false).unwrap();
        assert_eq!(text(&model).as_deref(), Some("9007199254740994"));
        assert_eq!(
            model.commit("9007199254740998").unwrap().display,
            "9007199254740996"
        );
        assert!(matches!(
            NumberModel::new(NumberOptions {
                minimum: Some(decimal("9007199254740993")),
                maximum: Some(decimal("9007199254740993")),
                ..Default::default()
            }),
            Err(NumberError::InvalidRange)
        ));
        assert!(matches!(
            NumberModel::new(NumberOptions {
                step: decimal("1e-400"),
                ..Default::default()
            }),
            Err(NumberError::InvalidStep)
        ));
    }
}
