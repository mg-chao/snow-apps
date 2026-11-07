// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Behavioral reference: Ant Design 6.6.7 / @rc-component/slider 1.1.1 (MIT).

use std::{fmt, sync::Arc};

use crate::Direction;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum SliderOrientation {
    #[default]
    Horizontal,
    Vertical,
}

#[derive(Clone, Debug, PartialEq)]
pub enum SliderValue {
    Single(f64),
    Range(Vec<f64>),
}
impl From<f64> for SliderValue {
    fn from(value: f64) -> Self {
        Self::Single(value)
    }
}
impl<const N: usize> From<[f64; N]> for SliderValue {
    fn from(values: [f64; N]) -> Self {
        Self::Range(values.into())
    }
}
impl From<Vec<f64>> for SliderValue {
    fn from(values: Vec<f64>) -> Self {
        Self::Range(values)
    }
}
impl SliderValue {
    pub fn as_slice(&self) -> &[f64] {
        match self {
            Self::Single(value) => std::slice::from_ref(value),
            Self::Range(values) => values,
        }
    }
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct SliderRange {
    pub draggable_track: bool,
    pub editable: bool,
    pub min_count: usize,
    pub max_count: Option<usize>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum SliderDisabled {
    All(bool),
    Handles(Arc<[bool]>),
}
impl Default for SliderDisabled {
    fn default() -> Self {
        Self::All(false)
    }
}
impl From<bool> for SliderDisabled {
    fn from(value: bool) -> Self {
        Self::All(value)
    }
}
impl<const N: usize> From<[bool; N]> for SliderDisabled {
    fn from(value: [bool; N]) -> Self {
        Self::Handles(value.into())
    }
}
impl SliderDisabled {
    pub fn handle(&self, index: usize) -> bool {
        match self {
            Self::All(value) => *value,
            Self::Handles(handles) => handles.get(index).copied().unwrap_or(false),
        }
    }
    pub fn any(&self, count: usize) -> bool {
        (0..count).any(|index| self.handle(index))
    }
    pub fn all(&self, count: usize) -> bool {
        match self {
            Self::All(value) => *value,
            Self::Handles(_) => count != 0 && (0..count).all(|index| self.handle(index)),
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct SliderOptions {
    pub min: f64,
    pub max: f64,
    /// `None` permits only marks and the two endpoints.
    pub step: Option<f64>,
    pub marks: Vec<f64>,
    /// `None` is a single handle; `Some(Default::default())` is an ordinary range.
    pub range: Option<SliderRange>,
    pub orientation: SliderOrientation,
    pub reverse: bool,
    pub keyboard: bool,
    pub included: bool,
    pub dots: bool,
    pub disabled: SliderDisabled,
}
impl Default for SliderOptions {
    fn default() -> Self {
        Self {
            min: 0.,
            max: 100.,
            step: Some(1.),
            marks: Vec::new(),
            range: None,
            orientation: SliderOrientation::Horizontal,
            reverse: false,
            keyboard: true,
            included: true,
            dots: false,
            disabled: SliderDisabled::default(),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SliderError {
    InvalidBounds,
    InvalidStep,
    NonFiniteValue,
    ValueModeMismatch,
    ConflictingRangeModes,
    InvalidHandleCount,
}
impl fmt::Display for SliderError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            Self::InvalidBounds => "slider bounds must be finite, ordered, and have a finite span",
            Self::InvalidStep => "slider step must be finite and greater than zero",
            Self::NonFiniteValue => "slider values and marks must be finite",
            Self::ValueModeMismatch => "slider value must match its single or range mode",
            Self::ConflictingRangeModes => {
                "editable handles and draggable track are mutually exclusive"
            }
            Self::InvalidHandleCount => "maximum handle count must not be smaller than minimum",
        })
    }
}
impl std::error::Error for SliderError {}

impl SliderOptions {
    pub fn validate(&self) -> Result<(), SliderError> {
        if !self.min.is_finite()
            || !self.max.is_finite()
            || self.min > self.max
            || !(self.max - self.min).is_finite()
        {
            return Err(SliderError::InvalidBounds);
        }
        if self
            .step
            .is_some_and(|step| !step.is_finite() || step <= 0.)
        {
            return Err(SliderError::InvalidStep);
        }
        if self.marks.iter().any(|mark| !mark.is_finite()) {
            return Err(SliderError::NonFiniteValue);
        }
        if let Some(range) = &self.range {
            if range.editable && range.draggable_track {
                return Err(SliderError::ConflictingRangeModes);
            }
            if range.max_count.is_some_and(|max| max < range.min_count) {
                return Err(SliderError::InvalidHandleCount);
            }
        }
        Ok(())
    }
    pub fn default_value(&self) -> SliderValue {
        if self.range.is_some() {
            SliderValue::Range(vec![self.min; 2])
        } else {
            SliderValue::Single(self.min)
        }
    }
    /// Whether increasing values run toward the physical start of the rail.
    pub fn inverted(&self, direction: Direction) -> bool {
        self.reverse
            ^ match self.orientation {
                SliderOrientation::Horizontal => direction == Direction::Rtl,
                SliderOrientation::Vertical => true,
            }
    }
}

/// A validated, reusable numeric scale. Pointer movement does not enumerate steps.
#[derive(Clone, Debug)]
pub struct SliderScale {
    min: f64,
    max: f64,
    step: Option<f64>,
    marks: Arc<[f64]>,
    rounding_factor: f64,
}
impl SliderScale {
    pub fn new(options: &SliderOptions) -> Result<Self, SliderError> {
        options.validate()?;
        let mut marks: Vec<_> = options
            .marks
            .iter()
            .copied()
            .filter(|value| *value >= options.min && *value <= options.max)
            .collect();
        marks.sort_by(f64::total_cmp);
        marks.dedup();
        let decimals = [options.min, options.max, options.step.unwrap_or(1.)]
            .into_iter()
            .map(decimal_places)
            .max()
            .unwrap_or(0);
        Ok(Self {
            min: options.min,
            max: options.max,
            step: options.step,
            marks: marks.into(),
            rounding_factor: 10_f64.powi(decimals.min(308) as i32),
        })
    }
    pub fn marks(&self) -> &[f64] {
        &self.marks
    }
    pub fn fraction(&self, value: f64) -> f64 {
        if self.min == self.max || !value.is_finite() {
            return 0.;
        }
        ((value - self.min) / (self.max - self.min)).clamp(0., 1.)
    }
    pub fn from_fraction(&self, fraction: f64) -> Option<f64> {
        fraction
            .is_finite()
            .then(|| self.snap_finite(self.min + fraction.clamp(0., 1.) * (self.max - self.min)))
    }
    pub fn snap(&self, value: f64) -> Option<f64> {
        value.is_finite().then(|| self.snap_finite(value))
    }
    fn rounded(&self, value: f64) -> f64 {
        let scaled = value * self.rounding_factor;
        if scaled.is_finite() {
            scaled.round() / self.rounding_factor
        } else {
            value
        }
    }
    fn step_value(&self, value: f64) -> Option<f64> {
        let step = self.step?;
        let index = ((value.clamp(self.min, self.max) - self.min) / step).round();
        if !index.is_finite() {
            // The grid is finer than f64 can index. Every representable value is
            // already within less than one representable interval of a grid point.
            return Some(value.clamp(self.min, self.max));
        }
        let value = self.rounded(self.min + index * step);
        (value >= self.min && value <= self.max).then_some(value)
    }
    fn snap_finite(&self, value: f64) -> f64 {
        let target = value.clamp(self.min, self.max);
        let split = self.marks.partition_point(|mark| *mark <= target);
        let mut result = self.min;
        let mut distance = f64::INFINITY;
        // Preserve reference tie priority: ascending marks, grid, min, max.
        for candidate in [
            split.checked_sub(1).map(|i| self.marks[i]),
            self.marks.get(split).copied(),
            self.step_value(target),
            Some(self.min),
            Some(self.max),
        ]
        .into_iter()
        .flatten()
        {
            let delta = (candidate - target).abs();
            if delta <= distance {
                result = candidate;
                distance = delta;
            }
        }
        result
    }
    /// Move by valid points, including marks between grid points. Page keys use two units.
    pub fn offset(&self, value: f64, units: i32) -> Option<f64> {
        let mut result = self.snap(value)?;
        let forward = units > 0;
        for _ in 0..units.unsigned_abs() {
            let split = self.marks.partition_point(|mark| {
                if forward {
                    *mark <= result
                } else {
                    *mark < result
                }
            });
            let mark = if forward {
                self.marks.get(split).copied()
            } else {
                split.checked_sub(1).map(|i| self.marks[i])
            };
            let step = self
                .step
                .and_then(|step| self.step_value(result + if forward { step } else { -step }));
            let aligned = self.step_value(result);
            let endpoint = if forward { self.max } else { self.min };
            let mut closest = endpoint;
            for candidate in [mark, aligned, step].into_iter().flatten() {
                if (if forward {
                    candidate > result
                } else {
                    candidate < result
                }) && (candidate - result).abs() < (closest - result).abs()
                {
                    closest = candidate;
                }
            }
            if result == closest {
                break;
            }
            result = closest;
        }
        Some(result)
    }
    /// Dragging uses a directed displacement from the gesture's original value.
    /// Its tie order differs from click/value snapping in the reference: marks
    /// win ties before endpoints and grid points, and cannot reverse the drag.
    pub fn drag_offset(&self, origin: f64, delta: f64) -> Option<f64> {
        let origin = self.snap(origin)?;
        if !delta.is_finite() {
            return None;
        }
        let target = origin + delta;
        if !target.is_finite() {
            return Some(if delta < 0. { self.min } else { self.max });
        }
        let forward = delta >= 0.;
        let first = if forward {
            self.marks.partition_point(|mark| *mark < origin)
        } else {
            0
        };
        let end = if forward {
            self.marks.len()
        } else {
            self.marks.partition_point(|mark| *mark <= origin)
        };
        let split = self
            .marks
            .partition_point(|mark| *mark <= target)
            .clamp(first, end);
        let lower = (split > first).then(|| self.marks[split - 1]);
        let upper = (split < end).then(|| self.marks[split]);
        let mut result = if forward { self.max } else { self.min };
        let mut distance = f64::INFINITY;
        for candidate in [
            lower,
            upper,
            Some(self.min),
            Some(self.max),
            self.step_value(origin),
            self.step_value(target),
        ]
        .into_iter()
        .flatten()
        {
            if (forward && candidate < origin) || (!forward && candidate > origin) {
                continue;
            }
            let next_distance = (candidate - target).abs();
            if next_distance < distance {
                result = candidate;
                distance = next_distance;
            }
        }
        Some(result)
    }
}
fn decimal_places(value: f64) -> usize {
    let text = value.to_string();
    let (mantissa, exponent) = text.split_once('e').map_or((text.as_str(), 0), |(m, e)| {
        (m, e.parse::<i32>().unwrap_or(0))
    });
    let decimals = mantissa
        .split_once('.')
        .map_or(0, |(_, tail)| tail.len() as i32);
    (decimals - exponent).max(0) as usize
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub(super) struct HandleValue {
    pub id: u64,
    pub value: f64,
}

#[derive(Clone, Debug)]
pub(super) struct Proposal {
    pub handles: Vec<HandleValue>,
    pub active: Option<u64>,
}
impl Proposal {
    pub fn value(&self, range: bool) -> SliderValue {
        if range {
            SliderValue::Range(self.handles.iter().map(|h| h.value).collect())
        } else {
            SliderValue::Single(self.handles.first().map_or(0., |h| h.value))
        }
    }
}

pub(super) struct SliderModel {
    pub options: SliderOptions,
    pub scale: SliderScale,
    pub handles: Vec<HandleValue>,
    next_id: u64,
}
impl SliderModel {
    pub fn new(options: SliderOptions, value: SliderValue) -> Result<Self, SliderError> {
        let scale = SliderScale::new(&options)?;
        let mut model = Self {
            options,
            scale,
            handles: Vec::new(),
            next_id: 0,
        };
        model.set_value(value)?;
        Ok(model)
    }
    pub fn value(&self) -> SliderValue {
        if self.options.range.is_some() {
            SliderValue::Range(self.handles.iter().map(|h| h.value).collect())
        } else {
            SliderValue::Single(self.handles.first().map_or(self.options.min, |h| h.value))
        }
    }
    pub fn set_options(&mut self, options: SliderOptions) -> Result<(), SliderError> {
        let scale = SliderScale::new(&options)?;
        let value = if self.options.range.is_some() == options.range.is_some() {
            self.value()
        } else {
            options.default_value()
        };
        self.options = options;
        self.scale = scale;
        self.set_value(value)?;
        Ok(())
    }
    pub fn normalize(&self, value: SliderValue) -> Result<Vec<f64>, SliderError> {
        if matches!(value, SliderValue::Range(_)) != self.options.range.is_some() {
            return Err(SliderError::ValueModeMismatch);
        }
        let mut values: Vec<_> = value
            .as_slice()
            .iter()
            .map(|value| self.scale.snap(*value).ok_or(SliderError::NonFiniteValue))
            .collect::<Result<_, _>>()?;
        values.sort_by(f64::total_cmp);
        Ok(values)
    }
    pub fn set_value(&mut self, value: SliderValue) -> Result<bool, SliderError> {
        let values = self.normalize(value)?;
        if values
            .iter()
            .copied()
            .eq(self.handles.iter().map(|h| h.value))
        {
            return Ok(false);
        }
        let mut remaining = self.handles.clone();
        let mut next = Vec::with_capacity(values.len());
        // Reserve exact matches before assigning moved handles, including duplicate values.
        let mut matches = Vec::with_capacity(values.len());
        for value in &values {
            matches.push(
                remaining
                    .iter()
                    .position(|h| h.value == *value)
                    .map(|i| remaining.remove(i)),
            );
        }
        for (value, matched) in values.into_iter().zip(matches) {
            let mut handle = matched.unwrap_or_else(|| {
                if remaining.is_empty() {
                    self.allocate(value)
                } else {
                    remaining.remove(0)
                }
            });
            handle.value = value;
            next.push(handle);
        }
        self.handles = next;
        Ok(true)
    }
    fn allocate(&mut self, value: f64) -> HandleValue {
        let id = self.next_id;
        self.next_id += 1;
        HandleValue { id, value }
    }
    pub fn accept(&mut self, proposal: &Proposal) {
        self.handles.clone_from(&proposal.handles);
    }
    pub fn boundaries(&self, index: usize) -> (f64, f64) {
        let min = (0..index)
            .rev()
            .find(|i| self.options.disabled.handle(*i))
            .map_or(self.options.min, |i| self.handles[i].value);
        let max = (index + 1..self.handles.len())
            .find(|i| self.options.disabled.handle(*i))
            .map_or(self.options.max, |i| self.handles[i].value);
        (min, max)
    }
    pub fn nearest(&self, target: f64) -> Option<usize> {
        let mut result = None;
        let mut distance = f64::INFINITY;
        for (index, handle) in self.handles.iter().enumerate() {
            let (min, max) = self.boundaries(index);
            if !self.options.disabled.handle(index) && target >= min && target <= max {
                let delta = (handle.value - target).abs();
                if delta <= distance {
                    distance = delta;
                    result = Some(index);
                }
            }
        }
        result
    }
    pub fn move_handle(&self, id: u64, target: f64) -> Option<Proposal> {
        let index = self.handles.iter().position(|handle| handle.id == id)?;
        if self.options.disabled.handle(index) {
            return None;
        }
        let (min, max) = self.boundaries(index);
        let value = self.scale.snap(target)?.clamp(min, max);
        let mut handles = self.handles.clone();
        handles[index].value = value;
        handles.sort_by(|a, b| a.value.total_cmp(&b.value));
        Some(Proposal {
            handles,
            active: Some(id),
        })
    }
    pub fn drag_handle(&self, id: u64, origin: f64, delta: f64) -> Option<Proposal> {
        self.move_handle(id, self.scale.drag_offset(origin, delta)?)
    }
    pub fn editable(&self) -> bool {
        self.options.range.as_ref().is_some_and(|r| r.editable)
            && !self.options.disabled.any(self.handles.len())
            && !self.options.disabled.all(self.handles.len())
    }
    pub fn track_draggable(&self) -> bool {
        self.options
            .range
            .as_ref()
            .is_some_and(|r| r.draggable_track)
            && self.options.step.is_some()
            && self.handles.len() > 1
            && !self.options.disabled.any(self.handles.len())
    }
    pub fn click(&mut self, target: f64) -> Option<Proposal> {
        if self.options.disabled.all(self.handles.len()) {
            return None;
        }
        let target = self.scale.snap(target)?;
        let index = self.nearest(target);
        if !self.handles.is_empty() && index.is_none() {
            return None;
        }
        let can_insert = self.editable()
            && self.options.range.as_ref().is_some_and(|r| {
                r.max_count
                    .is_none_or(|max| max == 0 || self.handles.len() < max)
            })
            && index.is_none_or(|i| self.handles[i].value != target);
        if self.handles.is_empty() || can_insert {
            let mut handles = self.handles.clone();
            let active = self.allocate(target);
            handles.push(active);
            if self.options.range.is_some() && self.handles.is_empty() {
                handles.push(self.allocate(target));
            }
            handles.sort_by(|a, b| a.value.total_cmp(&b.value));
            Some(Proposal {
                handles,
                active: Some(active.id),
            })
        } else {
            self.move_handle(self.handles[index?].id, target)
        }
    }
    pub fn remove(&self, id: u64) -> Option<Proposal> {
        if !self.editable() || self.handles.len() <= self.options.range.as_ref()?.min_count {
            return None;
        }
        let index = self.handles.iter().position(|h| h.id == id)?;
        let mut handles = self.handles.clone();
        handles.remove(index);
        let active = handles.get(index.saturating_sub(1)).map(|h| h.id);
        Some(Proposal { handles, active })
    }
    pub fn shift_track(&self, origin: &[HandleValue], delta: f64) -> Option<Proposal> {
        if !self.track_draggable() || origin.is_empty() || !delta.is_finite() {
            return None;
        }
        let first = origin.first()?.value;
        let last = origin.last()?.value;
        let offset = delta.clamp(self.options.min - first, self.options.max - last);
        let offset = (self.scale.snap(first + offset)? - first)
            .clamp(self.options.min - first, self.options.max - last);
        Some(Proposal {
            handles: origin
                .iter()
                .map(|handle| HandleValue {
                    id: handle.id,
                    value: self.scale.rounded(handle.value + offset),
                })
                .collect(),
            active: None,
        })
    }
}
