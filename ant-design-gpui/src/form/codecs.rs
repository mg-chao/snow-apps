// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Native control codecs preserve domain values and expose structural arrays to
//! nested-field rules, without a JSON or display-string round trip.

use std::sync::Arc;

use crate::{ColorPickerValue, PickerRange, SelectValue, slider::SliderValue};

use super::{FormCodec, FormTypeError, FormValue};

impl<T: Clone + PartialEq + 'static> FormCodec<Option<T>> {
    pub fn optional_native() -> Self {
        Self::new(
            |value: Option<T>| value.map_or_else(FormValue::null, FormValue::native),
            |value| value.native_ref::<T>().cloned().map(Some),
        )
    }
}

impl<T: Clone + PartialEq + 'static> FormCodec<PickerRange<T>> {
    pub fn range() -> Self {
        Self::new(
            |range: PickerRange<T>| {
                if range.is_empty() {
                    return FormValue::null();
                }
                FormValue::array(
                    [range.start, range.end]
                        .into_iter()
                        .map(|value| value.map_or_else(FormValue::null, FormValue::native)),
                )
            },
            |value| {
                let values =
                    value
                        .as_array()
                        .filter(|values| values.len() == 2)
                        .ok_or(FormTypeError {
                            expected: "two range endpoints",
                            actual: value.type_name(),
                        })?;
                let endpoint = |index| -> Result<Option<T>, FormTypeError> {
                    let value = values.get(index).expect("validated endpoint count");
                    if value.is_null() || value.is_undefined() {
                        Ok(None)
                    } else {
                        value.native_ref::<T>().cloned().map(Some)
                    }
                };
                Ok(PickerRange::new(endpoint(0)?, endpoint(1)?))
            },
        )
    }
}

impl<T: Clone + PartialEq + 'static> FormCodec<Arc<[T]>> {
    pub fn native_array() -> Self {
        Self::new(
            |values: Arc<[T]>| FormValue::array(values.iter().cloned().map(FormValue::native)),
            |value| {
                value
                    .as_array()
                    .ok_or(FormTypeError {
                        expected: "array",
                        actual: value.type_name(),
                    })?
                    .iter()
                    .map(|value| value.native_ref::<T>().cloned())
                    .collect::<Result<Arc<[T]>, _>>()
            },
        )
    }
}

impl<T: Clone + PartialEq + 'static> FormCodec<SelectValue<T>> {
    pub fn selection() -> Self {
        Self::native()
    }
}

impl<T: Clone + PartialEq + 'static> FormCodec<Vec<SelectValue<T>>> {
    pub fn selections() -> Self {
        FormCodec::<SelectValue<T>>::selection().array()
    }
}

impl FormCodec<ColorPickerValue> {
    pub fn color() -> Self {
        Self::new(
            |value| {
                if matches!(value, ColorPickerValue::Cleared) {
                    FormValue::null()
                } else {
                    FormValue::native(value)
                }
            },
            |value| value.native_ref::<ColorPickerValue>().cloned(),
        )
    }
}

impl FormCodec<SliderValue> {
    pub fn slider() -> Self {
        fn number(value: &FormValue) -> Result<f64, FormTypeError> {
            value
                .as_number()
                .and_then(|number| number.to_number().ok())
                .ok_or(FormTypeError {
                    expected: "finite slider number",
                    actual: value.type_name(),
                })
        }
        Self::new(
            |value| match value {
                SliderValue::Single(value) => FormValue::from(value),
                SliderValue::Range(values) => {
                    FormValue::array(values.into_iter().map(FormValue::from))
                }
            },
            |value| match value.as_array() {
                Some(values) => values
                    .iter()
                    .map(number)
                    .collect::<Result<Vec<_>, _>>()
                    .map(SliderValue::Range),
                None => number(value).map(SliderValue::Single),
            },
        )
    }
}
