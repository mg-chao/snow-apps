// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use crate::{
    behavior::{MotionPolicy, ScalarTransition},
    theme::CubicBezier,
};
use gpui::{Bounds, Pixels, point, px, size};
use std::time::Duration;

/// A single timeline for the shared background's four coordinates. Content is
/// measured and registered once at its final location, independently of this
/// decorative surface, just as in the reference unique container.
pub(super) struct BoundsTransition {
    from: Bounds<Pixels>,
    target: Bounds<Pixels>,
    progress: ScalarTransition,
}

impl BoundsTransition {
    pub fn new(bounds: Bounds<Pixels>) -> Self {
        Self {
            from: bounds,
            target: bounds,
            progress: ScalarTransition::new(1., curve()),
        }
    }

    pub fn retarget(
        &mut self,
        target: Bounds<Pixels>,
        now: Duration,
        duration: Duration,
        policy: MotionPolicy,
    ) {
        if target != self.target {
            self.from = self.sample(now).0;
            self.target = target;
            self.progress = ScalarTransition::new(0., curve());
            self.progress.retarget(1., now, duration, policy);
        } else if !policy.allows_motion() {
            self.progress.retarget(1., now, Duration::ZERO, policy);
        }
    }

    pub fn sample(&self, now: Duration) -> (Bounds<Pixels>, bool) {
        let sample = self.progress.sample(now);
        let mix = |from: Pixels, to: Pixels| from + (to - from) * sample.value as f32;
        (
            Bounds::new(
                point(
                    mix(self.from.left(), self.target.left()),
                    mix(self.from.top(), self.target.top()),
                ),
                size(
                    mix(self.from.size.width, self.target.size.width).max(px(0.)),
                    mix(self.from.size.height, self.target.size.height).max(px(0.)),
                ),
            ),
            sample.finished,
        )
    }
}

fn curve() -> CubicBezier {
    // CSS's implicit `ease` in `transition: all motionDurationSlow`.
    CubicBezier::new(0.25, 0.1, 0.25, 1.)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn bounds_handoff_preserves_interruption_continuity_and_reduced_motion() {
        let a = Bounds::new(point(px(10.), px(20.)), size(px(60.), px(32.)));
        let b = Bounds::new(point(px(200.), px(50.)), size(px(180.), px(64.)));
        let mut transition = BoundsTransition::new(a);
        let policy = MotionPolicy::new(true, false);
        transition.retarget(b, Duration::ZERO, Duration::from_millis(300), policy);
        assert_eq!(transition.sample(Duration::ZERO).0, a);
        let middle = transition.sample(Duration::from_millis(100)).0;
        assert!(middle.left() > a.left() && middle.left() < b.left());
        assert!(middle.size.height > a.size.height && middle.size.height < b.size.height);
        transition.retarget(
            a,
            Duration::from_millis(100),
            Duration::from_millis(300),
            policy,
        );
        assert_eq!(transition.sample(Duration::from_millis(100)).0, middle);
        transition.retarget(
            a,
            Duration::from_millis(120),
            Duration::from_millis(300),
            MotionPolicy::new(true, true),
        );
        assert_eq!(transition.sample(Duration::from_millis(120)), (a, true));
    }
    #[test]
    fn unique_background_matches_original_browser_css_interpolation() {
        let fixture: serde_json::Value =
            serde_json::from_str(include_str!("../../fixtures/popup/motion.json")).unwrap();
        let motion = &fixture["motion"];
        assert_eq!(motion["duration"], "0.3s");
        assert_eq!(motion["easing"], "ease");
        let bounds = |value: &serde_json::Value| {
            let n = |key| px(value[key].as_f64().unwrap() as f32);
            Bounds::new(point(n("x"), n("y")), size(n("width"), n("height")))
        };
        let mut transition = BoundsTransition::new(bounds(&motion["from"]));
        transition.retarget(
            bounds(&motion["to"]),
            Duration::ZERO,
            Duration::from_millis(300),
            MotionPolicy::new(true, false),
        );
        for sample in motion["samples"].as_array().unwrap() {
            let now = Duration::from_millis(sample["time"].as_u64().unwrap());
            let actual = transition.sample(now).0;
            let expected = bounds(&sample["bounds"]);
            for (a, b) in [
                (actual.left(), expected.left()),
                (actual.top(), expected.top()),
                (actual.size.width, expected.size.width),
                (actual.size.height, expected.size.height),
            ] {
                assert!(
                    f32::from(a - b).abs() < 0.025,
                    "CSS sample {now:?}: native {actual:?}, browser {expected:?}"
                );
            }
        }
    }
}
