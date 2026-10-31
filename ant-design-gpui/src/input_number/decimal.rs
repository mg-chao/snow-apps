// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::{cmp::Ordering, fmt, str::FromStr};

use num_bigint::BigInt;
use num_traits::{Signed, Zero};

/// The maximum expanded decimal length/scale accepted by the numeric editor.
/// This bounds work for pasted exponents without routing exact values through f64.
pub const MAX_DECIMAL_DIGITS: usize = 100_000;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DecimalError {
    Invalid,
    TooLarge,
    NonFinite,
}

impl fmt::Display for DecimalError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            Self::Invalid => "Invalid decimal",
            Self::TooLarge => "Decimal exceeds the supported digit limit",
            Self::NonFinite => "Numeric values must be finite",
        })
    }
}
impl std::error::Error for DecimalError {}

/// A canonical signed integer coefficient times 10^(-scale). Equality and
/// arithmetic are exact, including integers larger than JavaScript's safe range.
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct ExactDecimal {
    coefficient: BigInt,
    scale: i32,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Rounding {
    HalfAwayFromZero,
    Floor,
    Ceiling,
}

impl Default for ExactDecimal {
    fn default() -> Self {
        Self {
            coefficient: BigInt::zero(),
            scale: 0,
        }
    }
}

impl From<i64> for ExactDecimal {
    fn from(value: i64) -> Self {
        Self::canonical(BigInt::from(value), 0)
    }
}

impl ExactDecimal {
    pub fn is_zero(&self) -> bool {
        self.coefficient.is_zero()
    }
    pub fn is_negative(&self) -> bool {
        self.coefficient.is_negative()
    }
    pub fn decimal_places(&self) -> u32 {
        self.scale.max(0) as u32
    }
    pub fn negate(&self) -> Self {
        Self {
            coefficient: -&self.coefficient,
            scale: self.scale,
        }
    }

    /// Converts a finite standard number using its shortest round-tripping decimal.
    pub fn from_number(number: f64) -> Result<Self, DecimalError> {
        if !number.is_finite() {
            return Err(DecimalError::NonFinite);
        }
        number.to_string().parse()
    }

    pub fn to_number(&self) -> Result<f64, DecimalError> {
        let value: f64 = format!("{}e{}", self.coefficient, -self.scale)
            .parse()
            .map_err(|_| DecimalError::NonFinite)?;
        if value.is_finite() {
            Ok(value)
        } else {
            Err(DecimalError::NonFinite)
        }
    }

    pub fn checked_add(&self, other: &Self) -> Result<Self, DecimalError> {
        let scale = self.scale.max(other.scale);
        let left = &self.coefficient * ten_pow((scale - self.scale) as u32);
        let right = &other.coefficient * ten_pow((scale - other.scale) as u32);
        Self::checked_parts(left + right, scale)
    }

    pub fn checked_multiply_integer(&self, factor: i64) -> Result<Self, DecimalError> {
        Self::checked_parts(&self.coefficient * factor, self.scale)
    }

    pub fn round(&self, precision: u32) -> Result<Self, DecimalError> {
        self.round_with(precision, Rounding::HalfAwayFromZero)
    }

    pub(crate) fn round_with(
        &self,
        precision: u32,
        rounding: Rounding,
    ) -> Result<Self, DecimalError> {
        if precision as usize > MAX_DECIMAL_DIGITS {
            return Err(DecimalError::TooLarge);
        }
        if self.scale <= precision as i32 {
            return Ok(self.clone());
        }
        let divisor = ten_pow((self.scale - precision as i32) as u32);
        let mut quotient = &self.coefficient / &divisor;
        let remainder = &self.coefficient % &divisor;
        let adjust = match rounding {
            Rounding::HalfAwayFromZero => remainder.abs() * 2 >= divisor,
            Rounding::Floor => self.is_negative() && !remainder.is_zero(),
            Rounding::Ceiling => !self.is_negative() && !remainder.is_zero(),
        };
        if adjust {
            quotient += if self.is_negative() { -1 } else { 1 };
        }
        Self::checked_parts(quotient, precision as i32)
    }

    /// Fixed-point output with half-away-from-zero rounding, without binary conversion.
    pub fn to_fixed(&self, precision: u32) -> Result<String, DecimalError> {
        let value = self.round(precision)?;
        let mut output = value.to_string();
        if precision > 0 {
            let present = output.find('.').map_or(0, |dot| output.len() - dot - 1);
            if !output.contains('.') {
                output.push('.');
            }
            output.extend(std::iter::repeat_n('0', precision as usize - present));
        }
        Ok(output)
    }

    fn canonical(coefficient: BigInt, scale: i32) -> Self {
        if coefficient.is_zero() {
            return Self::default();
        }
        let digits = coefficient.to_str_radix(10);
        let trimmed = digits.trim_end_matches('0');
        let zeros = digits.len() - trimmed.len();
        if zeros == 0 {
            Self { coefficient, scale }
        } else {
            Self {
                coefficient: BigInt::parse_bytes(trimmed.as_bytes(), 10).expect("integer digits"),
                scale: scale - zeros as i32,
            }
        }
    }

    fn checked_parts(coefficient: BigInt, scale: i32) -> Result<Self, DecimalError> {
        let value = Self::canonical(coefficient, scale);
        let digits = value
            .coefficient
            .to_str_radix(10)
            .trim_start_matches('-')
            .len();
        if value.scale.unsigned_abs() as usize > MAX_DECIMAL_DIGITS
            || digits + (-value.scale).max(0) as usize > MAX_DECIMAL_DIGITS
        {
            Err(DecimalError::TooLarge)
        } else {
            Ok(value)
        }
    }
}

fn ten_pow(exponent: u32) -> BigInt {
    BigInt::from(10u8).pow(exponent)
}

impl FromStr for ExactDecimal {
    type Err = DecimalError;
    fn from_str(source: &str) -> Result<Self, Self::Err> {
        let source = source.trim();
        if source.len() > MAX_DECIMAL_DIGITS + 32 {
            return Err(DecimalError::TooLarge);
        }
        let (mantissa, exponent) = match source.find(['e', 'E']) {
            Some(index) => (
                &source[..index],
                source[index + 1..]
                    .parse::<i32>()
                    .map_err(|_| DecimalError::Invalid)?,
            ),
            None => (source, 0),
        };
        if exponent.unsigned_abs() as usize > MAX_DECIMAL_DIGITS {
            return Err(DecimalError::TooLarge);
        }
        let negative = mantissa.starts_with('-');
        let mantissa = mantissa.strip_prefix(['-', '+']).unwrap_or(mantissa);
        let mut digits = String::with_capacity(mantissa.len());
        let mut after_decimal = None;
        for byte in mantissa.bytes() {
            match byte {
                b'0'..=b'9' => {
                    digits.push(byte as char);
                    if let Some(count) = &mut after_decimal {
                        *count += 1;
                    }
                }
                b'.' if after_decimal.is_none() => after_decimal = Some(0i32),
                _ => return Err(DecimalError::Invalid),
            }
        }
        if digits.is_empty() {
            return Err(DecimalError::Invalid);
        }
        let digits = digits.trim_start_matches('0');
        if digits.is_empty() {
            return Ok(Self::default());
        }
        let trimmed = digits.trim_end_matches('0');
        let scale = after_decimal.unwrap_or(0) - exponent - (digits.len() - trimmed.len()) as i32;
        let mut coefficient =
            BigInt::parse_bytes(trimmed.as_bytes(), 10).ok_or(DecimalError::Invalid)?;
        if negative {
            coefficient = -coefficient;
        }
        Self::checked_parts(coefficient, scale)
    }
}

impl fmt::Display for ExactDecimal {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if self.is_negative() {
            f.write_str("-")?;
        }
        let digits = self.coefficient.abs().to_str_radix(10);
        if self.scale <= 0 {
            f.write_str(&digits)?;
            for _ in 0..-self.scale {
                f.write_str("0")?;
            }
        } else if self.scale as usize >= digits.len() {
            f.write_str("0.")?;
            for _ in 0..self.scale as usize - digits.len() {
                f.write_str("0")?;
            }
            f.write_str(&digits)?;
        } else {
            let split = digits.len() - self.scale as usize;
            write!(f, "{}.{}", &digits[..split], &digits[split..])?;
        }
        Ok(())
    }
}

impl Ord for ExactDecimal {
    fn cmp(&self, other: &Self) -> Ordering {
        if self.scale == other.scale {
            return self.coefficient.cmp(&other.coefficient);
        }
        let scale = self.scale.max(other.scale);
        (&self.coefficient * ten_pow((scale - self.scale) as u32))
            .cmp(&(&other.coefficient * ten_pow((scale - other.scale) as u32)))
    }
}
impl PartialOrd for ExactDecimal {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn decimal(value: &str) -> ExactDecimal {
        value.parse().unwrap()
    }

    #[test]
    fn canonical_decimal_parsing_preserves_exact_values_and_exponents() {
        for (input, expected) in [
            ("0001.2300", "1.23"),
            ("-0.000", "0"),
            ("+.5", "0.5"),
            ("1.", "1"),
            ("1.2e3", "1200"),
            ("-23e-6", "-0.000023"),
            (
                "9007199254740993.000000000000000001",
                "9007199254740993.000000000000000001",
            ),
        ] {
            assert_eq!(decimal(input).to_string(), expected, "{input}");
        }
        assert_eq!(decimal("1000"), decimal("1e3"));
        for input in [
            "", "-", ".", "1e", "1e-", "1e2e3", "1.2.3", "NaN", "Infinity", "1 2", "１２",
        ] {
            assert!(input.parse::<ExactDecimal>().is_err(), "{input}");
        }
    }

    #[test]
    fn exact_steps_and_ordering_do_not_pass_through_binary_floats() {
        assert_eq!(
            decimal("0.1").checked_add(&decimal("0.2")).unwrap(),
            decimal("0.3")
        );
        assert_eq!(
            decimal("9007199254740993")
                .checked_add(&decimal("0.000000000000000001"))
                .unwrap()
                .to_string(),
            "9007199254740993.000000000000000001"
        );
        assert!(decimal("-0.01") < decimal("0"));
        assert!(decimal("9007199254740993") > decimal("9007199254740992"));
        assert_eq!(
            decimal("1.25").checked_multiply_integer(-10).unwrap(),
            decimal("-12.5")
        );
        let mut value = decimal("0");
        for _ in 0..1000 {
            value = value.checked_add(&decimal("0.001")).unwrap();
        }
        assert_eq!(value, decimal("1"));
    }

    #[test]
    fn rounding_handles_ties_signs_and_carry_without_losing_precision() {
        for (input, precision, expected) in [
            ("1.005", 2, "1.01"),
            ("-1.005", 2, "-1.01"),
            ("9.999", 2, "10.00"),
            ("-0.0001", 2, "0.00"),
            ("10", 3, "10.000"),
        ] {
            assert_eq!(decimal(input).to_fixed(precision).unwrap(), expected);
        }
        assert_eq!(
            decimal("-1.01").round_with(0, Rounding::Floor).unwrap(),
            decimal("-2")
        );
        assert_eq!(
            decimal("-1.01").round_with(0, Rounding::Ceiling).unwrap(),
            decimal("-1")
        );
    }

    #[test]
    fn standard_conversion_rejects_nonfinite_and_resource_limits_bound_exponents() {
        assert_eq!(
            ExactDecimal::from_number(f64::INFINITY),
            Err(DecimalError::NonFinite)
        );
        assert_eq!(
            ExactDecimal::from_number(f64::NAN),
            Err(DecimalError::NonFinite)
        );
        assert_eq!(ExactDecimal::from_number(0.1).unwrap(), decimal("0.1"));
        assert_eq!(decimal("1e400").to_number(), Err(DecimalError::NonFinite));
        assert_eq!(
            "1e100001".parse::<ExactDecimal>(),
            Err(DecimalError::TooLarge)
        );
        assert_eq!(
            "1e-100001".parse::<ExactDecimal>(),
            Err(DecimalError::TooLarge)
        );
        assert!(decimal("1").to_fixed(100001).is_err());
    }
}
