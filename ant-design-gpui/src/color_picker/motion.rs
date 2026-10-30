// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::time::Duration;

use crate::{
    behavior::MotionPolicy,
    input::motion::{CSS_EASE, ColorMotion},
};
use gpui::Hsla;

#[derive(Clone, Copy)]
pub(super) struct TriggerColors {
    pub border: Hsla,
    /// Non-solid authored fills remain native and do not become solid colors.
    pub background: Option<Hsla>,
    pub halo: Hsla,
}

/// Retained CSS color transitions. The mounted control owns time and scheduling.
#[derive(Default)]
pub(super) struct TriggerMotion {
    border: ColorMotion,
    background: ColorMotion,
    halo: ColorMotion,
}
impl TriggerMotion {
    pub fn resolve(
        &mut self,
        target: TriggerColors,
        now: Duration,
        duration: Duration,
        policy: MotionPolicy,
    ) -> (TriggerColors, bool) {
        let (border, a) = self
            .border
            .resolve(target.border, now, duration, CSS_EASE, policy);
        let (background, b) = if let Some(color) = target.background {
            let (color, active) = self
                .background
                .resolve(color, now, duration, CSS_EASE, policy);
            (Some(color), active)
        } else {
            self.background = ColorMotion::default();
            (None, false)
        };
        let (halo, c) = self
            .halo
            .resolve(target.halo, now, duration, CSS_EASE, policy);
        (
            TriggerColors {
                border,
                background,
                halo,
            },
            a || b || c,
        )
    }
    pub fn cancel(&mut self) {
        *self = Self::default();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn colors(value: u32) -> TriggerColors {
        TriggerColors {
            border: gpui::rgb(value).into(),
            background: Some(gpui::rgb(value).into()),
            halo: gpui::hsla(0., 0., 0., 0.),
        }
    }
    #[test]
    fn trigger_color_transitions_start_static_and_reverse_continuously() {
        let mut m = TriggerMotion::default();
        let policy = MotionPolicy::new(true, false);
        let (initial, active) = m.resolve(
            colors(0),
            Duration::ZERO,
            Duration::from_millis(200),
            policy,
        );
        assert_eq!(initial.border, colors(0).border);
        assert!(!active);
        m.resolve(
            colors(0xffffff),
            Duration::ZERO,
            Duration::from_millis(200),
            policy,
        );
        let (middle, active) = m.resolve(
            colors(0xffffff),
            Duration::from_millis(80),
            Duration::from_millis(200),
            policy,
        );
        assert!(active);
        assert_ne!(middle.border, colors(0).border);
        assert_ne!(middle.border, colors(0xffffff).border);
        let (reversed, active) = m.resolve(
            colors(0),
            Duration::from_millis(80),
            Duration::from_millis(200),
            policy,
        );
        assert!(active);
        assert_eq!(middle.border, reversed.border);
        let (end, active) = m.resolve(
            colors(0),
            Duration::from_millis(400),
            Duration::from_millis(200),
            policy,
        );
        assert_eq!(end.border, colors(0).border);
        assert!(!active);
    }
    #[test]
    fn reduced_motion_and_cancellation_snap_without_animation() {
        let mut m = TriggerMotion::default();
        let duration = Duration::from_millis(200);
        m.resolve(
            colors(0),
            Duration::ZERO,
            duration,
            MotionPolicy::new(true, false),
        );
        let (value, active) = m.resolve(
            colors(0xffffff),
            Duration::from_millis(10),
            duration,
            MotionPolicy::new(true, true),
        );
        assert_eq!(value.border, colors(0xffffff).border);
        assert!(!active);
        m.cancel();
        let (value, active) = m.resolve(
            colors(0),
            Duration::from_millis(11),
            duration,
            MotionPolicy::new(true, false),
        );
        assert_eq!(value.border, colors(0).border);
        assert!(!active);
    }
}
