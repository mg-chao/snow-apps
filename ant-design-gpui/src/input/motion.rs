// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::time::Duration;

use gpui::{BoxShadow, Hsla, Rgba, point, px};

use crate::{
    behavior::{MotionPolicy, ScalarTransition},
    theme::{CubicBezier, ShadowList},
};

pub(crate) const CSS_EASE: CubicBezier = CubicBezier::new(0.25, 0.1, 0.25, 1.);
pub(crate) const LINEAR: CubicBezier = CubicBezier::new(0., 0., 1., 1.);

#[cfg(test)]
pub(crate) mod reference_tests;

/// CSS color interpolation in premultiplied sRGB, retaining float precision.
fn color(from: Hsla, to: Hsla, progress: f32) -> Hsla {
    if progress <= 0. || from == to {
        return from;
    }
    if progress >= 1. {
        return to;
    }
    let from = Rgba::from(from);
    let to = Rgba::from(to);
    let alpha = from.a + (to.a - from.a) * progress;
    let channel = |a: f32, b: f32| {
        if alpha > f32::EPSILON {
            (a * from.a * (1. - progress) + b * to.a * progress) / alpha
        } else {
            0.
        }
    };
    Rgba {
        r: channel(from.r, to.r),
        g: channel(from.g, to.g),
        b: channel(from.b, to.b),
        a: alpha,
    }
    .into()
}

/// CSS transitions retain their reversing-adjusted start and shortening factor.
/// Reversing an unfinished transition uses the elapsed *eased* progress, so a
/// brief hover does not take a full duration to fade away. Time remains owned by
/// the mounted GPUI control; this value type schedules no work itself.
pub(crate) struct CssTransition<T> {
    from: Option<T>,
    target: Option<T>,
    reversing_start: Option<T>,
    shortening: f64,
    progress: Option<ScalarTransition>,
}
impl<T> Default for CssTransition<T> {
    fn default() -> Self {
        Self {
            from: None,
            target: None,
            reversing_start: None,
            shortening: 1.,
            progress: None,
        }
    }
}
impl<T: Clone + PartialEq> CssTransition<T> {
    fn sample(&self, now: Duration, interpolate: fn(&T, &T, f64) -> T) -> T {
        let target = self.target.as_ref().expect("initialized transition");
        interpolate(
            self.from.as_ref().unwrap_or(target),
            target,
            self.progress.as_ref().map_or(1., |p| p.sample(now).value),
        )
    }
    pub fn resolve(
        &mut self,
        target: T,
        now: Duration,
        duration: Duration,
        curve: CubicBezier,
        policy: MotionPolicy,
        interpolate: fn(&T, &T, f64) -> T,
    ) -> (T, bool) {
        if !policy.allows_motion() || duration.is_zero() || self.target.is_none() {
            self.from = Some(target.clone());
            self.target = Some(target.clone());
            self.reversing_start = Some(target.clone());
            self.shortening = 1.;
            self.progress = Some(ScalarTransition::new(1., curve));
            return (target, false);
        }
        if self.target.as_ref() != Some(&target) {
            let current = self.sample(now, interpolate);
            let old_progress = self
                .progress
                .as_ref()
                .expect("initialized progress")
                .sample(now);
            let reversing =
                !old_progress.finished && self.reversing_start.as_ref() == Some(&target);
            self.shortening = if reversing {
                (old_progress.value * self.shortening + (1. - self.shortening))
                    .abs()
                    .clamp(0., 1.)
            } else {
                1.
            };
            self.reversing_start = if reversing {
                self.target.clone()
            } else {
                Some(current.clone())
            };
            self.from = Some(current);
            self.target = Some(target);
            let mut progress = ScalarTransition::new(0., curve);
            progress.retarget(
                1.,
                now,
                if self.from == self.target {
                    Duration::ZERO
                } else {
                    duration.mul_f64(self.shortening)
                },
                policy,
            );
            self.progress = Some(progress);
        }
        let animating = !self
            .progress
            .as_ref()
            .expect("initialized progress")
            .sample(now)
            .finished;
        (self.sample(now, interpolate), animating)
    }
}

pub(crate) fn scalar(from: &f64, to: &f64, progress: f64) -> f64 {
    from + (to - from) * progress
}
fn interpolate_color(from: &Hsla, to: &Hsla, progress: f64) -> Hsla {
    color(*from, *to, progress as f32)
}

#[derive(Default)]
pub(crate) struct ColorMotion(CssTransition<Hsla>);
impl ColorMotion {
    #[cfg(test)]
    pub fn sample(&self, now: Duration) -> Hsla {
        self.0.sample(now, interpolate_color)
    }
    pub fn resolve(
        &mut self,
        target: Hsla,
        now: Duration,
        duration: Duration,
        curve: CubicBezier,
        motion: MotionPolicy,
    ) -> (Hsla, bool) {
        self.0
            .resolve(target, now, duration, curve, motion, interpolate_color)
    }
}

fn interpolate_shadows(from: &[BoxShadow], to: &[BoxShadow], progress: f64) -> Vec<BoxShadow> {
    let progress = progress as f32;
    if progress >= 1. {
        return to.to_vec();
    }
    // CSS shadow lists are discrete if corresponding inset flags differ.
    if from.iter().zip(to).any(|(a, b)| a.inset != b.inset) {
        return to.to_vec();
    }
    (0..from.len().max(to.len()))
        .map(|index| {
            let missing = BoxShadow {
                inset: from
                    .get(index)
                    .or_else(|| to.get(index))
                    .is_some_and(|s| s.inset),
                color: Hsla::transparent_black(),
                offset: point(px(0.), px(0.)),
                blur_radius: px(0.),
                spread_radius: px(0.),
            };
            let a = from.get(index).unwrap_or(&missing);
            let b = to.get(index).unwrap_or(&missing);
            BoxShadow {
                inset: b.inset,
                color: color(a.color, b.color, progress),
                offset: a.offset + (b.offset - a.offset) * progress,
                blur_radius: a.blur_radius + (b.blur_radius - a.blur_radius) * progress,
                spread_radius: a.spread_radius + (b.spread_radius - a.spread_radius) * progress,
            }
        })
        .collect()
}
#[derive(Default)]
struct ShadowMotion(CssTransition<Vec<BoxShadow>>);
impl ShadowMotion {
    #[cfg(test)]
    fn sample(&self, now: Duration) -> Vec<BoxShadow> {
        self.0.sample(now, |a, b, p| interpolate_shadows(a, b, p))
    }
    fn resolve(
        &mut self,
        target: &ShadowList,
        now: Duration,
        duration: Duration,
        motion: MotionPolicy,
    ) -> (Vec<BoxShadow>, bool) {
        let target = target
            .iter()
            .map(|s| BoxShadow {
                inset: s.inset,
                color: s.color.into(),
                offset: point(px(s.x as f32), px(s.y as f32)),
                blur_radius: px(s.blur as f32),
                spread_radius: px(s.spread as f32),
            })
            .collect();
        self.0
            .resolve(target, now, duration, CSS_EASE, motion, |a, b, p| {
                interpolate_shadows(a, b, p)
            })
    }
}

#[derive(Default)]
pub(crate) struct SurfaceMotion {
    border: ColorMotion,
    background: ColorMotion,
    text: ColorMotion,
    affix: ColorMotion,
    shadow: ShadowMotion,
}
impl SurfaceMotion {
    pub fn resolve(
        &mut self,
        style: &mut super::InputStyle,
        now: Duration,
        duration: Duration,
        text_duration: Duration,
        text_curve: CubicBezier,
        motion: MotionPolicy,
    ) -> (Vec<BoxShadow>, bool) {
        let (border, a) = self
            .border
            .resolve(style.border, now, duration, CSS_EASE, motion);
        let (background, b) =
            self.background
                .resolve(style.background, now, duration, CSS_EASE, motion);
        let (text, c) = self
            .text
            .resolve(style.text, now, text_duration, text_curve, motion);
        let (affix, d) = self
            .affix
            .resolve(style.affix, now, duration, CSS_EASE, motion);
        let (shadow, e) = self.shadow.resolve(&style.shadow, now, duration, motion);
        style.border = border;
        style.background = background;
        style.text = text;
        style.affix = affix;
        (shadow, a || b || c || d || e)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::theme::{Color, Shadow};
    use std::sync::Arc;

    #[test]
    fn focus_shadow_interpolates_from_none_and_reverses_without_jumping() {
        let none: ShadowList = Arc::from([]);
        let focused: ShadowList = Arc::from([Shadow {
            spread: 2.,
            color: Color::rgb(0x1677ff).with_alpha(0.2),
            ..Default::default()
        }]);
        let policy = MotionPolicy::new(true, false);
        let mut motion = ShadowMotion::default();
        let duration = Duration::from_millis(200);
        motion.resolve(&none, Duration::ZERO, duration, policy);
        motion.resolve(&focused, Duration::ZERO, duration, policy);
        let mid = Duration::from_millis(100);
        let sample = motion.sample(mid);
        assert!(sample[0].spread_radius > px(1.) && sample[0].spread_radius < px(2.));
        assert!(sample[0].color.a > 0.1 && sample[0].color.a < 0.2);
        let (reversed, active) = motion.resolve(&none, mid, duration, policy);
        assert!(active);
        assert_eq!(sample, reversed);
        let (settled, active) = motion.resolve(&none, mid, duration, MotionPolicy::new(true, true));
        assert!(!active && settled.is_empty());
        assert!(motion.sample(Duration::from_secs(1)).is_empty());
    }

    #[test]
    fn removing_transition_duration_snaps_an_in_flight_color() {
        let mut motion = ColorMotion::default();
        let policy = MotionPolicy::new(true, false);
        let duration = Duration::from_millis(200);
        motion.resolve(gpui::black(), Duration::ZERO, duration, CSS_EASE, policy);
        motion.resolve(gpui::white(), Duration::ZERO, duration, CSS_EASE, policy);
        let now = Duration::from_millis(50);
        let (color, active) = motion.resolve(gpui::white(), now, Duration::ZERO, CSS_EASE, policy);
        assert!(!active);
        assert_eq!(color, gpui::white());
    }
}
