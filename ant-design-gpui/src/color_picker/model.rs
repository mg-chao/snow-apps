// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Behavioral reference: Ant Design 6.6.7 (MIT). Native stable stop identities
// replace index-based drag bookkeeping.

use super::color::{ColorPickerMode, ColorPickerValue, ColorStop, PickerColor, validate_percent};

#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct PickerStop {
    pub id: u64,
    pub color: PickerColor,
    pub percent: f64,
}

/// Pure editing model. GPUI state owns accepted/controlled values and gesture
/// snapshots; this model is cheap to clone and contains no window or callbacks.
#[derive(Clone, Debug)]
pub(crate) struct PickerModel {
    value: ColorPickerValue,
    mode: ColorPickerMode,
    stops: Vec<PickerStop>,
    active_index: usize,
    next_id: u64,
    cached_gradient: Option<(ColorPickerValue, Vec<PickerStop>)>,
}
impl PickerModel {
    pub fn new(value: ColorPickerValue) -> Self {
        let mode = if value.is_gradient() {
            ColorPickerMode::Gradient
        } else {
            ColorPickerMode::Single
        };
        let mut model = Self {
            value: ColorPickerValue::Cleared,
            mode,
            stops: Vec::new(),
            active_index: 0,
            next_id: 1,
            cached_gradient: None,
        };
        model.set_value(value);
        model
    }
    pub fn value(&self) -> &ColorPickerValue {
        &self.value
    }
    pub fn set_value(&mut self, value: ColorPickerValue) {
        let mut used = vec![false; self.stops.len()];
        let mut stops = Vec::new();
        if let ColorPickerValue::Gradient(values) = &value {
            for stop in values.iter() {
                if let Some(index) = self.stops.iter().enumerate().position(|(index, s)| {
                    !used[index] && s.color == stop.color && s.percent == stop.percent
                }) {
                    used[index] = true;
                    stops.push(self.stops[index]);
                } else {
                    stops.push(PickerStop {
                        id: self.allocate_id(),
                        color: stop.color,
                        percent: stop.percent,
                    });
                }
            }
            stops.sort_by(|a, b| a.percent.total_cmp(&b.percent));
        }
        self.stops = stops;
        self.value = if value.is_cleared() {
            ColorPickerValue::Cleared
        } else {
            value
        };
        if self.value.is_gradient() {
            self.mode = ColorPickerMode::Gradient;
            self.sync_value();
        } else if !self.value.is_cleared() {
            self.mode = ColorPickerMode::Single;
        }
        self.active_index = self.active_index.min(self.stops.len().saturating_sub(1));
        self.cached_gradient = None;
    }
    pub fn active_index(&self) -> usize {
        self.active_index
    }
    pub fn set_active_index(&mut self, index: usize) {
        self.active_index = index.min(self.stops.len().saturating_sub(1));
    }
    pub fn active_color(&self) -> PickerColor {
        self.value.active_color(self.active_index)
    }
    /// Normalize an edit with alpha disabled without replacing its stable stop
    /// identities or resetting the active drag/index and remembered hue.
    pub fn force_opaque_alpha(&mut self) -> bool {
        if self.value.is_cleared() {
            return false;
        }
        let mut changed = false;
        for stop in &mut self.stops {
            if stop.color.to_hsb().3 != 1. {
                stop.color = stop.color.with_alpha(1.);
                changed = true;
            }
        }
        match &mut self.value {
            ColorPickerValue::Single(color) => {
                if color.to_hsb().3 != 1. {
                    *color = color.with_alpha(1.);
                    changed = true;
                }
            }
            ColorPickerValue::Gradient(_) if changed => self.sync_value(),
            _ => {}
        }
        if let Some((value, stops)) = &mut self.cached_gradient {
            *value = value.with_opaque_alpha();
            for stop in stops {
                stop.color = stop.color.with_alpha(1.);
            }
        }
        changed
    }
    pub fn mode(&self) -> ColorPickerMode {
        self.mode
    }
    /// Apply a configuration constraint without changing the supplied value or
    /// emitting a color change. Cleared gradients expose two virtual handles;
    /// their first actual edit materializes a gradient value.
    pub fn prepare_mode(&mut self, mode: ColorPickerMode) {
        self.mode = mode;
        if mode == ColorPickerMode::Gradient {
            self.ensure_gradient_points(self.value.active_color(0));
        } else {
            self.active_index = 0;
        }
    }
    pub fn stops(&self) -> &[PickerStop] {
        &self.stops
    }
    pub fn index_for_id(&self, id: u64) -> Option<usize> {
        self.stops.iter().position(|s| s.id == id)
    }
    pub fn set_active_color(&mut self, color: PickerColor) -> bool {
        self.cached_gradient = None;
        if self.mode == ColorPickerMode::Single {
            let next = ColorPickerValue::Single(color);
            let changed = self.value != next;
            self.value = next;
            self.stops.clear();
            self.active_index = 0;
            return changed;
        }
        self.ensure_gradient_points(PickerColor::TRANSPARENT);
        let changed = self.stops[self.active_index].color != color || self.value.is_cleared();
        self.stops[self.active_index].color = color;
        self.sync_value();
        changed
    }
    pub fn set_mode(&mut self, mode: ColorPickerMode) -> bool {
        if self.mode == mode {
            return false;
        }
        self.mode = mode;
        self.active_index = 0;
        if mode == ColorPickerMode::Single {
            let color = self.value.active_color(0);
            if self.value.is_gradient() {
                self.cached_gradient = Some((self.value.clone(), self.stops.clone()));
            }
            self.stops.clear();
            self.value = ColorPickerValue::Single(color);
        } else if let Some((value, stops)) = self.cached_gradient.take() {
            self.value = value;
            self.stops = stops;
        } else {
            let base = self.value.active_color(0).with_alpha(1.);
            self.stops.clear();
            self.ensure_gradient_points(base);
            self.sync_value();
        }
        true
    }
    pub fn clear(&mut self) -> bool {
        if self.value.is_cleared() {
            return false;
        }
        self.value = ColorPickerValue::Cleared;
        self.stops.clear();
        self.active_index = 0;
        self.cached_gradient = None;
        true
    }
    pub fn add_stop(&mut self, percent: f64) -> Option<u64> {
        if validate_percent(percent).is_err() || self.mode != ColorPickerMode::Gradient {
            return None;
        }
        let base = self.value.active_color(0);
        self.ensure_gradient_points(base);
        let color = interpolate(&self.stops, percent);
        let id = self.allocate_id();
        self.stops.push(PickerStop { id, color, percent });
        self.sort_and_activate(id);
        self.cached_gradient = None;
        self.sync_value();
        Some(id)
    }
    pub fn move_stop(&mut self, id: u64, percent: f64) -> bool {
        if validate_percent(percent).is_err() {
            return false;
        }
        let Some(index) = self.index_for_id(id) else {
            return false;
        };
        if self.stops[index].percent == percent {
            return false;
        }
        self.stops[index].percent = percent;
        self.sort_and_activate(id);
        self.cached_gradient = None;
        self.sync_value();
        true
    }
    pub fn remove_stop(&mut self, id: u64) -> bool {
        if self.stops.len() <= 2 {
            return false;
        }
        let Some(index) = self.index_for_id(id) else {
            return false;
        };
        let active_id = self.stops.get(self.active_index).map(|s| s.id);
        self.stops.remove(index);
        self.active_index = active_id
            .and_then(|id| self.index_for_id(id))
            .unwrap_or_else(|| index.min(self.stops.len() - 1));
        self.cached_gradient = None;
        self.sync_value();
        true
    }
    fn allocate_id(&mut self) -> u64 {
        let id = self.next_id;
        self.next_id = self
            .next_id
            .checked_add(1)
            .expect("color picker stop identity exhausted");
        id
    }
    fn ensure_gradient_points(&mut self, base: PickerColor) {
        if !self.stops.is_empty() {
            return;
        }
        let first = self.allocate_id();
        let last = self.allocate_id();
        self.stops = vec![
            PickerStop {
                id: first,
                color: base,
                percent: 0.,
            },
            PickerStop {
                id: last,
                color: base,
                percent: 100.,
            },
        ];
        self.active_index = 0;
    }
    fn sort_and_activate(&mut self, id: u64) {
        self.stops.sort_by(|a, b| a.percent.total_cmp(&b.percent));
        self.active_index = self.index_for_id(id).unwrap_or(0);
    }
    fn sync_value(&mut self) {
        self.value = if self.stops.is_empty() {
            ColorPickerValue::Cleared
        } else {
            ColorPickerValue::Gradient(
                self.stops
                    .iter()
                    .map(|s| ColorStop {
                        color: s.color,
                        percent: s.percent,
                    })
                    .collect(),
            )
        };
    }
}

fn interpolate(stops: &[PickerStop], percent: f64) -> PickerColor {
    let Some(first) = stops.first() else {
        return PickerColor::TRANSPARENT;
    };
    if percent <= first.percent {
        return first.color;
    }
    for pair in stops.windows(2) {
        let (a, b) = (pair[0], pair[1]);
        if percent >= a.percent && percent <= b.percent {
            let span = b.percent - a.percent;
            return if span == 0. {
                a.color
            } else {
                a.color.mix(b.color, (percent - a.percent) / span)
            };
        }
    }
    stops.last().unwrap().color
}

#[cfg(test)]
#[path = "model_tests.rs"]
mod tests;
