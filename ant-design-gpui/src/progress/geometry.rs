// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Geometry rules adapted from Ant Design 6.6.7 and rc-progress 1.0.3 (MIT).

use super::*;

pub(super) fn percent(value: f64) -> f64 {
    if value.is_nan() {
        0.
    } else {
        value.clamp(0., 100.)
    }
}
pub(super) fn length(value: f32, fallback: f32) -> f32 {
    if value.is_finite() {
        value.max(0.)
    } else {
        fallback
    }
}
pub(super) fn status(progress: &Progress) -> ProgressStatus {
    progress.status.unwrap_or_else(|| {
        if percent(progress.success.map_or(progress.percent, |s| s.percent)) >= 100. {
            ProgressStatus::Success
        } else {
            ProgressStatus::Normal
        }
    })
}
pub(super) fn dimensions(size: &ProgressSize, kind: ProgressKind) -> (Option<Length>, f32) {
    if kind != ProgressKind::Line {
        let diameter = match size {
            ProgressSize::Small => 60.,
            ProgressSize::Medium => 120.,
            ProgressSize::Pixels(value) => length(*value, 120.),
            ProgressSize::Dimensions {
                width:
                    Some(gpui::Length::Definite(gpui::DefiniteLength::Absolute(
                        gpui::AbsoluteLength::Pixels(width),
                    ))),
                ..
            } => length(f32::from(*width), 120.),
            ProgressSize::Dimensions { height, .. } => length(*height, 120.),
        };
        return (Some(gpui::px(diameter).into()), diameter);
    }
    match size {
        ProgressSize::Small => (None, 6.),
        ProgressSize::Medium => (None, 8.),
        ProgressSize::Pixels(value) => {
            let v = length(*value, 8.);
            (Some(gpui::px(v).into()), v)
        }
        ProgressSize::Dimensions { width, height } => (*width, length(*height, 8.)),
    }
}
pub(super) fn step_size(size: &ProgressSize) -> (Option<Length>, f32) {
    match size {
        ProgressSize::Small => (Some(gpui::px(2.).into()), 8.),
        ProgressSize::Medium => (Some(gpui::px(14.).into()), 8.),
        _ => dimensions(size, ProgressKind::Line),
    }
}
pub(super) fn gap(progress: &Progress) -> f32 {
    length(
        progress
            .gap_degree
            .unwrap_or(if progress.kind == ProgressKind::Dashboard {
                75.
            } else {
                0.
            }),
        0.,
    )
    .min(295.)
}
pub(super) fn start_angle(gap: f32, placement: ProgressGapPlacement, direction: Direction) -> f32 {
    if gap == 0. {
        return -std::f32::consts::FRAC_PI_2;
    }
    let shift = match placement {
        ProgressGapPlacement::Bottom => 0.,
        ProgressGapPlacement::Top => 180.,
        ProgressGapPlacement::Start => {
            if direction == Direction::Ltr {
                90.
            } else {
                -90.
            }
        }
        ProgressGapPlacement::End => {
            if direction == Direction::Ltr {
                -90.
            } else {
                90.
            }
        }
    };
    (90. + gap / 2. + shift).to_radians()
}
pub(super) fn active_steps(
    count: usize,
    value: f64,
    rounding: Option<&dyn Fn(f64) -> f64>,
) -> usize {
    let value = count as f64 * percent(value) / 100.;
    let rounded = rounding.map_or_else(|| value.round(), |f| f(value));
    if rounded.is_finite() {
        rounded.clamp(0., count as f64) as usize
    } else {
        0
    }
}
