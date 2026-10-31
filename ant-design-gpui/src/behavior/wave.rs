// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::time::Duration;

use gpui::{Hsla, StyleRefinement};

use super::{MotionPolicy, evaluate_cubic_bezier};
use crate::theme::AliasTokens;

/// Ant's wave-quick class belongs to the circular Radio / Checkbox target,
/// not to the Radio component as a whole (button radios use the regular wave).
#[derive(Clone, Copy)]
pub(crate) enum WaveMotion {
    Quick,
    Regular,
}

#[derive(Clone, Copy, Debug)]
pub(crate) struct WaveSample {
    pub spread: f32,
    pub opacity: f32,
}

/// The reference samples the target color when a wave mounts. Reusing the
/// target's live transition color on every frame makes a fading border dim the
/// wave twice and can change its hue partway through the effect.
#[derive(Default)]
pub(crate) struct WaveState {
    waves: Vec<Wave>,
}

struct Wave {
    started_at: Duration,
    color: Option<Hsla>,
    sample: WaveSample,
}

impl WaveState {
    pub fn start(&mut self, now: Duration) {
        // useWave coalesces triggers before the next frame. Mounted waves keep
        // their own lifetime when another click creates a new holder.
        self.discard_pending();
        self.waves.push(Wave {
            started_at: now,
            color: None,
            sample: WaveSample {
                spread: 0.,
                opacity: 0.2,
            },
        });
    }

    pub fn clear(&mut self) {
        self.waves.clear();
    }

    pub fn discard_pending(&mut self) {
        self.waves.retain(|wave| wave.color.is_some());
    }

    pub fn sample(
        &mut self,
        now: Duration,
        kind: WaveMotion,
        tokens: &AliasTokens,
        motion: MotionPolicy,
        enabled: bool,
    ) -> bool {
        if !enabled || !motion.allows_motion() {
            self.clear();
        }
        self.waves.retain_mut(|wave| {
            if let Some(sample) = kind.sample(now.saturating_sub(wave.started_at), tokens) {
                wave.sample = sample;
                true
            } else {
                false
            }
        });
        !self.waves.is_empty()
    }

    pub fn paint_samples(&mut self, target: Hsla) -> Vec<(WaveSample, Hsla)> {
        self.waves
            .iter_mut()
            .map(|wave| (wave.sample, *wave.color.get_or_insert(target)))
            .collect()
    }
}

impl WaveMotion {
    pub fn sample(self, elapsed: Duration, tokens: &AliasTokens) -> Option<WaveSample> {
        // components/_util/wave/style.ts: regular expansion and opacity have
        // independent clocks; quick waves use the scoped slow duration for both.
        let (expansion, fade, curve) = match self {
            Self::Quick => (
                tokens.motion_duration_slow,
                tokens.motion_duration_slow,
                tokens.motion_ease_in_out,
            ),
            Self::Regular => (
                Duration::from_millis(400),
                Duration::from_secs(2),
                tokens.motion_ease_out_circ,
            ),
        };
        if fade.is_zero() || elapsed >= fade {
            return None;
        }
        let progress = |duration: Duration| {
            evaluate_cubic_bezier(curve, elapsed.as_secs_f64() / duration.as_secs_f64()) as f32
        };
        Some(WaveSample {
            spread: 6. * progress(expansion),
            opacity: 0.2 * (1. - progress(fade)),
        })
    }
}

/// Match getTargetWaveColor: border, then background, then the primary token.
/// Opaque white and fully transparent colors cannot supply a visible wave.
pub(crate) fn wave_color(style: &StyleRefinement, fallback: Hsla) -> Hsla {
    [
        style.border_color,
        style
            .background
            .as_ref()
            .and_then(gpui::Fill::color)
            .and_then(|c| c.as_solid()),
    ]
    .into_iter()
    .flatten()
    .find(|color| color.a > 0. && !(color.l == 1. && color.a == 1.))
    .unwrap_or(fallback)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::theme::{Color, ResolvedTheme};
    use gpui::Styled;

    #[test]
    fn wave_reference_clocks_and_curves() {
        let tokens = &ResolvedTheme::default().tokens;
        let quick = WaveMotion::Quick
            .sample(Duration::from_millis(150), tokens)
            .unwrap();
        // Independent CSS cubic-bezier samples from the reference's seed tokens.
        assert!((quick.spread - 3.101_25).abs() < 0.000_01);
        assert!((quick.opacity - 0.096_625).abs() < 0.000_01);
        assert!(
            WaveMotion::Quick
                .sample(Duration::from_millis(300), tokens)
                .is_none()
        );
        let regular = WaveMotion::Regular
            .sample(Duration::from_millis(400), tokens)
            .unwrap();
        assert_eq!(regular.spread, 6.);
        assert!((regular.opacity - 0.042_833_667).abs() < 0.000_01);
        assert!(
            WaveMotion::Regular
                .sample(Duration::from_millis(1999), tokens)
                .is_some()
        );
        assert!(
            WaveMotion::Regular
                .sample(Duration::from_secs(2), tokens)
                .is_none()
        );
    }

    #[test]
    fn wave_color_skips_white_and_transparent_surfaces() {
        let primary = Color::rgb(0x1677ff).to_hsla();
        let custom = Color::rgb(0x722ed1).to_hsla();
        let style = StyleRefinement::default()
            .border_color(Color::WHITE)
            .bg(custom);
        assert_eq!(wave_color(&style, primary), custom);
        let style = StyleRefinement::default()
            .border_color(Color::TRANSPARENT)
            .bg(Color::WHITE);
        assert_eq!(wave_color(&style, primary), primary);
        let translucent = Color::WHITE.to_hsla().opacity(0.5);
        assert_eq!(
            wave_color(&StyleRefinement::default().bg(translucent), primary),
            translucent
        );
    }

    #[test]
    fn wave_quick_uses_theme_timing_and_regular_keeps_its_independent_clocks() {
        let mut tokens = (*ResolvedTheme::default().tokens).clone();
        tokens.motion_duration_slow = Duration::from_millis(600);
        tokens.motion_ease_in_out = crate::theme::CubicBezier::new(0., 0., 1., 1.);
        tokens.motion_ease_out_circ = tokens.motion_ease_in_out;
        let quick = WaveMotion::Quick
            .sample(Duration::from_millis(300), &tokens)
            .unwrap();
        assert_eq!(quick.spread, 3.);
        assert_eq!(quick.opacity, 0.1);
        tokens.motion_duration_slow = Duration::ZERO;
        assert!(WaveMotion::Quick.sample(Duration::ZERO, &tokens).is_none());
        let regular = WaveMotion::Regular
            .sample(Duration::from_millis(200), &tokens)
            .unwrap();
        assert_eq!(regular.spread, 3.);
        assert!((regular.opacity - 0.18).abs() < 0.000_01);
    }

    #[test]
    fn wave_coalesces_pending_triggers_without_removing_mounted_waves() {
        let mut wave = WaveState::default();
        let color = Color::rgb(0x1677ff).to_hsla();
        wave.start(Duration::ZERO);
        wave.start(Duration::from_millis(1));
        assert_eq!(wave.paint_samples(color).len(), 1);
        wave.start(Duration::from_millis(20));
        wave.discard_pending();
        assert_eq!(wave.paint_samples(color).len(), 1);
    }

    #[test]
    fn wave_color_is_captured_per_click_and_cancelled_waves_do_not_resume() {
        let tokens = &ResolvedTheme::default().tokens;
        let enabled = MotionPolicy::new(true, false);
        let mut wave = WaveState::default();
        let color = Color::rgb(0x1677ff).to_hsla();
        wave.start(Duration::ZERO);
        assert!(wave.sample(Duration::ZERO, WaveMotion::Quick, tokens, enabled, true));
        assert_eq!(wave.paint_samples(color)[0].1, color);
        assert_eq!(wave.paint_samples(color.opacity(0.1))[0].1, color);
        wave.start(Duration::from_millis(100));
        let painted = wave.paint_samples(color.opacity(0.5));
        assert_eq!(painted.len(), 2);
        assert_eq!(painted[0].1, color);
        assert_eq!(painted[1].1, color.opacity(0.5));
        assert!(wave.sample(
            Duration::from_millis(300),
            WaveMotion::Quick,
            tokens,
            enabled,
            true
        ));
        assert_eq!(wave.paint_samples(color).len(), 1);
        assert!(!wave.sample(
            Duration::from_millis(400),
            WaveMotion::Quick,
            tokens,
            enabled,
            true
        ));
        for (policy, allowed) in [
            (enabled, false),
            (MotionPolicy::new(false, false), true),
            (MotionPolicy::new(true, true), true),
        ] {
            wave.start(Duration::ZERO);
            assert!(!wave.sample(
                Duration::from_millis(10),
                WaveMotion::Regular,
                tokens,
                policy,
                allowed
            ));
            assert!(!wave.sample(
                Duration::from_millis(20),
                WaveMotion::Regular,
                tokens,
                enabled,
                true
            ));
        }
    }
}
