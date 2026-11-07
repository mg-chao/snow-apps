// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::time::Duration;

use gpui::Hsla;

use super::{InputNumberTokens, StepDirection};
use crate::{
    behavior::MotionPolicy,
    input::motion::{CSS_EASE, ColorMotion, CssTransition, LINEAR, scalar},
    theme::CubicBezier,
};

#[derive(Default)]
pub(super) struct HandleMotion {
    width: CssTransition<f64>,
    opacity: CssTransition<f64>,
    fraction: CssTransition<f64>,
    background: ColorMotion,
    border: ColorMotion,
    colors: [ColorMotion; 2],
    pressed: [ColorMotion; 2],
}

pub(super) struct HandleTarget {
    pub shown: bool,
    pub background: Hsla,
    pub hovered: Option<StepDirection>,
    pub pressed: Option<StepDirection>,
    pub enabled: [bool; 2],
}

pub(super) struct HandleStyle {
    pub width: f32,
    pub opacity: f32,
    pub fraction: f32,
    pub background: Hsla,
    pub border: Hsla,
    pub colors: [Hsla; 2],
    pub pressed: [Hsla; 2],
}

impl HandleMotion {
    pub fn resolve(
        &mut self,
        target: HandleTarget,
        tokens: &InputNumberTokens,
        now: Duration,
        policy: MotionPolicy,
    ) -> (HandleStyle, bool) {
        let duration = tokens.input.globals.motion_duration_mid;
        let resolve_scalar = |value: &mut CssTransition<f64>, target, curve: CubicBezier| {
            let (value, active) = value.resolve(target, now, duration, curve, policy, scalar);
            (value as f32, active)
        };
        let (width, mut animating) = resolve_scalar(
            &mut self.width,
            if target.shown {
                tokens.handle_width
            } else {
                0.
            },
            CSS_EASE,
        );
        let (opacity, moving) = resolve_scalar(
            &mut self.opacity,
            if target.shown { 1. } else { 0. },
            CSS_EASE,
        );
        animating |= moving;
        let (fraction, moving) = resolve_scalar(
            &mut self.fraction,
            match target.hovered {
                Some(StepDirection::Up) if target.enabled[0] => 0.6,
                Some(StepDirection::Down) if target.enabled[1] => 0.4,
                _ => 0.5,
            },
            LINEAR,
        );
        animating |= moving;
        let (background, moving) =
            self.background
                .resolve(target.background, now, duration, CSS_EASE, policy);
        animating |= moving;
        let (border, moving) = self.border.resolve(
            tokens.handle_border_color.into(),
            now,
            duration,
            LINEAR,
            policy,
        );
        animating |= moving;
        let mut colors = [Hsla::transparent_black(); 2];
        let mut pressed = colors;
        for (index, direction) in [StepDirection::Up, StepDirection::Down]
            .into_iter()
            .enumerate()
        {
            let color = if !target.enabled[index] {
                tokens.input.globals.color_text_disabled
            } else if target.hovered == Some(direction) {
                tokens.handle_hover_color
            } else {
                tokens.input.globals.color_icon
            };
            let (color, moving) =
                self.colors[index].resolve(color.into(), now, duration, LINEAR, policy);
            colors[index] = color;
            animating |= moving;
            let color = if target.pressed == Some(direction) && target.enabled[index] {
                tokens.handle_active_bg.into()
            } else {
                Hsla::transparent_black()
            };
            let (color, moving) = self.pressed[index].resolve(color, now, duration, LINEAR, policy);
            pressed[index] = color;
            animating |= moving;
        }
        (
            HandleStyle {
                width,
                opacity,
                fraction,
                background,
                border,
                colors,
                pressed,
            },
            animating,
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{input_number::InputNumberTheme, theme::ThemeConfig};

    #[test]
    fn numeric_motion_matches_original_browser_timing() {
        use crate::input::motion::reference_tests::{equal_color, fixtures, near, number, theme};
        let policy = MotionPolicy::new(true, false);
        let mut count = 0;
        for page in fixtures() {
            let theme = theme(page["theme"].as_str().unwrap());
            let tokens = theme.component::<InputNumberTheme>();
            for case in page["cases"].as_array().unwrap() {
                let name = case["name"].as_str().unwrap();
                if !name.starts_with("numeric-") {
                    continue;
                }
                let background = if name.contains("filled") {
                    tokens.filled_handle_bg.into()
                } else if name.contains("borderless") {
                    Hsla::transparent_black()
                } else {
                    tokens.handle_bg.into()
                };
                let reveal = name.ends_with("reveal");
                let pressed = name.ends_with("active");
                let target = |active| HandleTarget {
                    shown: !reveal || active,
                    background,
                    hovered: ((!reveal && active) || pressed).then_some(StepDirection::Up),
                    pressed: (pressed && active).then_some(StepDirection::Up),
                    enabled: [true; 2],
                };
                let input = &tokens.input;
                let height = input.padding_block * 2.
                    + input.input_font_size * input.globals.line_height
                    + if name.contains("borderless") {
                        2. * input.globals.line_width
                    } else {
                        0.
                    };
                for reverse in [false, true] {
                    let label = format!("{} {name} reverse={reverse}", page["theme"]);
                    let mut motion = HandleMotion::default();
                    motion.resolve(target(false), &tokens, Duration::ZERO, policy);
                    motion.resolve(target(true), &tokens, Duration::ZERO, policy);
                    if reverse {
                        motion.resolve(target(false), &tokens, Duration::from_millis(50), policy);
                    }
                    for sample in case[if reverse { "reverseSamples" } else { "samples" }]
                        .as_array()
                        .unwrap()
                    {
                        let time = sample["time"].as_u64().unwrap() + if reverse { 50 } else { 0 };
                        let (style, _) = motion.resolve(
                            target(!reverse),
                            &tokens,
                            Duration::from_millis(time),
                            policy,
                        );
                        let css = &sample["style"];
                        if reveal {
                            near(style.width.into(), number(&css["width"]), &label);
                            near(style.opacity.into(), number(&css["opacity"]), &label);
                            near(height, number(&css["height"]), &label);
                            equal_color(
                                style.background,
                                css["backgroundColor"].as_str().unwrap(),
                                &label,
                            );
                        } else {
                            near(
                                f64::from(style.fraction) * height,
                                number(&css["height"]),
                                &label,
                            );
                            equal_color(style.colors[0], css["color"].as_str().unwrap(), &label);
                            equal_color(
                                style.pressed[0],
                                css["backgroundColor"].as_str().unwrap(),
                                &label,
                            );
                        }
                        count += 1;
                    }
                }
            }
        }
        assert_eq!(count, 720);
    }

    #[test]
    fn numeric_handle_height_is_linear_and_reverses_at_the_painted_fraction() {
        let theme = ThemeConfig::default().resolve();
        let tokens = theme.component::<InputNumberTheme>();
        let policy = MotionPolicy::new(true, false);
        let target = |hovered, enabled| HandleTarget {
            shown: true,
            background: tokens.handle_bg.into(),
            hovered,
            pressed: hovered,
            enabled,
        };
        let mut motion = HandleMotion::default();
        motion.resolve(target(None, [true; 2]), &tokens, Duration::ZERO, policy);
        motion.resolve(
            target(Some(StepDirection::Up), [true; 2]),
            &tokens,
            Duration::ZERO,
            policy,
        );
        let half = tokens.input.globals.motion_duration_mid / 2;
        let (mid, active) = motion.resolve(
            target(Some(StepDirection::Up), [true; 2]),
            &tokens,
            half,
            policy,
        );
        assert!(active);
        assert!((mid.fraction - 0.55).abs() < 0.0001);
        assert!((mid.pressed[0].a - tokens.handle_active_bg.a as f32 / 2.).abs() < 0.0001);
        let (reverse, _) = motion.resolve(
            target(Some(StepDirection::Down), [true; 2]),
            &tokens,
            half,
            policy,
        );
        assert_eq!(reverse.fraction, mid.fraction);
        let (reverse_mid, _) = motion.resolve(
            target(Some(StepDirection::Down), [true; 2]),
            &tokens,
            half * 2,
            policy,
        );
        assert!((reverse_mid.fraction - 0.475).abs() < 0.0001);
        let (disabled, active) = motion.resolve(
            target(Some(StepDirection::Down), [true, false]),
            &tokens,
            half * 2,
            MotionPolicy::new(false, false),
        );
        assert!(!active);
        assert_eq!(disabled.fraction, 0.5);
        assert_eq!(
            disabled.colors[1],
            tokens.input.globals.color_text_disabled.to_hsla()
        );
        assert_eq!(disabled.pressed[1].a, 0.);
    }
}
