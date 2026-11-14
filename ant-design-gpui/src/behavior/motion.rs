// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::time::Duration;

use crate::theme::CubicBezier;

/// Effective scoped motion settings. Application reduced motion always wins.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct MotionPolicy {
    pub enabled: bool,
    pub reduced: bool,
}

impl MotionPolicy {
    pub fn new(enabled: bool, reduced: bool) -> Self {
        Self { enabled, reduced }
    }

    pub fn allows_motion(self) -> bool {
        self.enabled && !self.reduced
    }

    /// `None` means render a static final state without scheduling an animation.
    ///
    /// GPUI additionally respects its application-wide reduced-motion setting, so a
    /// subsequent application preference change can stop an already mounted animation.
    pub fn animation(self, duration: Duration, curve: CubicBezier) -> Option<gpui::Animation> {
        (self.allows_motion() && !duration.is_zero()).then(|| {
            gpui::Animation::new(duration).with_easing(move |progress| {
                let value = evaluate_cubic_bezier(curve, f64::from(progress)) as f32;
                if value.is_finite() {
                    value
                } else {
                    progress.clamp(0., 1.)
                }
            })
        })
    }
}

/// Evaluate a CSS cubic-bezier curve by solving its x coordinate for elapsed progress.
///
/// The y coordinate may overshoot. Invalid x control points or nonfinite curves fall
/// back to linear motion. Bisection handles flat derivatives and keeps evaluation bounded.
pub fn evaluate_cubic_bezier(curve: CubicBezier, progress: f64) -> f64 {
    let progress = if progress.is_finite() {
        progress.clamp(0., 1.)
    } else {
        1.
    };
    if progress == 0. || progress == 1. {
        return progress;
    }
    if ![curve.x1, curve.y1, curve.x2, curve.y2]
        .into_iter()
        .all(f64::is_finite)
        || !(0. ..=1.).contains(&curve.x1)
        || !(0. ..=1.).contains(&curve.x2)
    {
        return progress;
    }
    if curve.x1 == curve.y1 && curve.x2 == curve.y2 {
        return progress;
    }
    let cx = 3. * curve.x1;
    let bx = 3. * (curve.x2 - curve.x1) - cx;
    let ax = 1. - cx - bx;
    let cy = 3. * curve.y1;
    let by = 3. * (curve.y2 - curve.y1) - cy;
    let ay = 1. - cy - by;
    if ![cy, by, ay].into_iter().all(f64::is_finite) {
        return progress;
    }
    let coordinate_x = |t: f64| ((ax * t + bx) * t + cx) * t;
    let coordinate_y = |t: f64| {
        let value = ((ay * t + by) * t + cy) * t;
        if value.is_finite() { value } else { progress }
    };
    let mut low = 0.;
    let mut high = 1.;
    let mut t = progress;
    // Ordinary token curves converge in a handful of Newton steps. Bound every
    // step so flat endpoint tangents cannot escape the monotone time interval.
    for _ in 0..8 {
        let difference = coordinate_x(t) - progress;
        if difference.abs() <= 1e-9 {
            return coordinate_y(t);
        }
        if difference < 0. {
            low = t;
        } else {
            high = t;
        }
        let derivative = (3. * ax * t + 2. * bx) * t + cx;
        let candidate = t - difference / derivative;
        t = if derivative > 1e-7 && candidate > low && candidate < high {
            candidate
        } else {
            (low + high) * 0.5
        };
    }
    for _ in 0..24 {
        let difference = coordinate_x(t) - progress;
        if difference.abs() <= 1e-9 {
            break;
        }
        if difference < 0. {
            low = t;
        } else {
            high = t;
        }
        t = (low + high) * 0.5;
    }
    coordinate_y(t)
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct TransitionSample {
    pub value: f64,
    pub finished: bool,
}

/// A small scalar interpolation state with interruption continuity and caller-owned time.
///
/// This is a value model, not a scheduler: controls request GPUI frames only while
/// `sample(...).finished` is false. Retargeting begins at the current sampled value.
#[derive(Clone, Copy, Debug)]
pub struct ScalarTransition {
    from: f64,
    target: f64,
    started_at: Duration,
    duration: Duration,
    curve: CubicBezier,
}

impl ScalarTransition {
    pub fn new(value: f64, curve: CubicBezier) -> Self {
        Self {
            from: value,
            target: value,
            started_at: Duration::ZERO,
            duration: Duration::ZERO,
            curve,
        }
    }

    pub fn target(&self) -> f64 {
        self.target
    }

    pub fn sample(&self, now: Duration) -> TransitionSample {
        let elapsed = now.saturating_sub(self.started_at);
        if self.duration.is_zero() || elapsed >= self.duration {
            return TransitionSample {
                value: self.target,
                finished: true,
            };
        }
        let progress = elapsed.as_secs_f64() / self.duration.as_secs_f64();
        TransitionSample {
            value: self.from
                + (self.target - self.from) * evaluate_cubic_bezier(self.curve, progress),
            finished: false,
        }
    }

    pub fn retarget(
        &mut self,
        target: f64,
        now: Duration,
        duration: Duration,
        policy: MotionPolicy,
    ) {
        if target == self.target && policy.allows_motion() && !duration.is_zero() {
            return;
        }
        self.from = self.sample(now).value;
        self.target = target;
        self.started_at = now;
        self.duration = if policy.allows_motion() && self.from != target {
            duration
        } else {
            Duration::ZERO
        };
    }
}
