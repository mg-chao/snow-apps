// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Motion and progress formulas adapted from Ant Design 6.6.7 (MIT).

use crate::Direction;
use std::time::Duration;

pub(super) fn finite_percent(value: f64) -> f64 {
    if value.is_finite() { value } else { 0. }
}

pub(super) fn auto_percent(ticks: u64) -> f64 {
    // Saturation occurs at tick 235. Bounded work also makes resuming after days cheap.
    let mut percent = 0.;
    for _ in 0..ticks.min(235) {
        let rate = if percent <= 30. {
            0.05
        } else if percent <= 70. {
            0.03
        } else if percent <= 96. {
            0.01
        } else {
            break;
        };
        percent += (100. - percent) * rate;
    }
    percent
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub(super) struct Dot {
    pub x: f32,
    pub y: f32,
    pub diameter: f32,
    pub opacity: f32,
}

pub(super) fn dots(
    holder: f32,
    item: f32,
    elapsed: Duration,
    direction: Direction,
    motion: bool,
) -> [Dot; 4] {
    let seconds = if motion { elapsed.as_secs_f64() } else { 0. };
    let angle = std::f64::consts::FRAC_PI_4 + (seconds / 1.2).fract() * std::f64::consts::TAU;
    let (sin, cos) = angle.sin_cos();
    let half = f64::from(holder / 2.);
    let item_half = f64::from(item / 2.);
    std::array::from_fn(|index| {
        let (right, bottom) = match index {
            0 => (false, false),
            1 => (true, false),
            2 => (true, true),
            _ => (false, true),
        };
        let right = right ^ (direction == Direction::Rtl);
        let x = if right {
            f64::from(holder) - item_half
        } else {
            item_half
        } - half;
        let y = if bottom {
            f64::from(holder) - item_half
        } else {
            item_half
        } - half;
        let local = seconds - index as f64 * 0.4;
        let opacity = if local <= 0. {
            0.3
        } else {
            let phase = local % 2.;
            0.3 + 0.7 * if phase <= 1. { phase } else { 2. - phase }
        };
        Dot {
            x: (half + x * cos - y * sin) as f32,
            y: (half + x * sin + y * cos) as f32,
            diameter: item * 0.75,
            opacity: opacity as f32,
        }
    })
}
