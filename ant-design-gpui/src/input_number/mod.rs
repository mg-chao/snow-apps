// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Exact decimal values and native numeric editing controls.

mod decimal;
mod model;
mod motion;
mod state;
mod style;
#[cfg(test)]
mod tests;
mod view;
pub use decimal::{DecimalError, ExactDecimal, MAX_DECIMAL_DIGITS};
pub use model::{
    NumberError, NumberFormatContext, NumberFormatter, NumberOptions, NumberParser, NumericDraft,
    NumericMode, NumericValue,
};
pub use state::{
    InputNumberEvent, InputNumberState, NumericChange, NumericChangeSource, NumericCommit,
    NumericInput, NumericStep, StepDirection, StepSource,
};
pub use style::{InputNumberTheme, InputNumberTokenPatch, InputNumberTokens};
pub use view::InputNumber;
