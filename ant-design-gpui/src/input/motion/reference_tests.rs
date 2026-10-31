// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;
use crate::{
    Size, ValidationStatus, Variant,
    input::{InputInteraction, InputStyle, InputTheme},
    theme::{AlgorithmChain, ResolvedTheme, ThemeConfig},
};
use serde_json::Value;

pub(crate) fn fixtures() -> Vec<Value> {
    serde_json::from_str(include_str!("../../../fixtures/input/motion.json")).unwrap()
}
pub(crate) fn theme(name: &str) -> ResolvedTheme {
    ThemeConfig {
        algorithms: Some(match name {
            "default" => AlgorithmChain::default(),
            "dark" => AlgorithmChain::dark(),
            "compact" => AlgorithmChain::compact(),
            "dark-compact" => AlgorithmChain::dark_compact(),
            _ => panic!("unknown reference theme"),
        }),
        ..Default::default()
    }
    .resolve()
}
pub(crate) fn variant(name: &str) -> Variant {
    match name {
        "outlined" => Variant::Outlined,
        "filled" => Variant::Filled,
        "borderless" => Variant::Borderless,
        "underlined" => Variant::Underlined,
        _ => panic!("unknown reference variant"),
    }
}
pub(crate) fn number(value: &Value) -> f64 {
    value
        .as_str()
        .unwrap()
        .trim_end_matches("px")
        .parse()
        .unwrap()
}
pub(crate) fn near(actual: f64, expected: f64, label: &str) {
    assert!(
        (actual - expected).abs() < 0.025,
        "{label}: {actual} != {expected}"
    );
}
pub(crate) fn equal_color(actual: Hsla, css: &str, label: &str) {
    let fields: Vec<f32> = css
        .split_once('(')
        .unwrap()
        .1
        .trim_end_matches(')')
        .split(',')
        .map(|v| v.trim().parse().unwrap())
        .collect();
    let expected = Rgba {
        r: fields[0] / 255.,
        g: fields[1] / 255.,
        b: fields[2] / 255.,
        a: *fields.get(3).unwrap_or(&1.),
    };
    let actual = Rgba::from(actual);
    for (a, b) in [
        (actual.a, expected.a),
        (actual.r * actual.a, expected.r * expected.a),
        (actual.g * actual.a, expected.g * expected.a),
        (actual.b * actual.a, expected.b * expected.a),
    ] {
        assert!((a - b).abs() < 0.005, "{label}: {actual:?} != {css}");
    }
}
fn equal_shadow(actual: &[BoxShadow], css: &str, label: &str) {
    if css == "none" {
        assert!(
            actual
                .iter()
                .all(|s| s.color.a == 0. || s.spread_radius == px(0.)),
            "{label}: unexpected shadow {actual:?}"
        );
        return;
    }
    let (color, lengths) = css.split_once(") ").unwrap();
    let lengths: Vec<f64> = lengths
        .split_whitespace()
        .map(|v| v.trim_end_matches("px").parse().unwrap())
        .collect();
    let zero = BoxShadow {
        inset: false,
        color: Hsla::transparent_black(),
        offset: point(px(0.), px(0.)),
        blur_radius: px(0.),
        spread_radius: px(0.),
    };
    let shadow = actual.first().unwrap_or(&zero);
    equal_color(shadow.color, &format!("{color})"), label);
    for (actual, expected) in [
        f32::from(shadow.offset.x),
        f32::from(shadow.offset.y),
        f32::from(shadow.blur_radius),
        f32::from(shadow.spread_radius),
    ]
    .into_iter()
    .zip(lengths)
    {
        near(actual.into(), expected, label);
    }
}

#[test]
fn input_surface_and_action_motion_match_original_browser_timing() {
    let policy = MotionPolicy::new(true, false);
    let mut count = 0;
    for page in fixtures() {
        let theme = theme(page["theme"].as_str().unwrap());
        let tokens = theme.component::<InputTheme>();
        for case in page["cases"].as_array().unwrap() {
            let name = case["name"].as_str().unwrap();
            if name.starts_with("numeric-") {
                continue;
            }
            for reverse in [false, true] {
                let samples = case[if reverse { "reverseSamples" } else { "samples" }]
                    .as_array()
                    .unwrap();
                let label = format!("{} {name} reverse={reverse}", page["theme"]);
                if name.starts_with("input-") {
                    let variant = variant(name.split('-').nth(1).unwrap());
                    let duration = if variant == Variant::Borderless {
                        Duration::ZERO
                    } else {
                        tokens.globals.motion_duration_mid
                    };
                    let state = |active| {
                        InputStyle::from_tokens(
                            &tokens,
                            Size::Medium,
                            variant,
                            ValidationStatus::None,
                            InputInteraction {
                                disabled: false,
                                hovered: active && name.ends_with("hover"),
                                focused: active && name.ends_with("focus"),
                                keyboard_focus: false,
                            },
                        )
                    };
                    let mut motion = SurfaceMotion::default();
                    let mut update = |active, now| {
                        let mut style = state(active);
                        let (shadow, _) =
                            motion.resolve(&mut style, now, duration, duration, CSS_EASE, policy);
                        (style, shadow)
                    };
                    update(false, Duration::ZERO);
                    update(true, Duration::ZERO);
                    if reverse {
                        update(false, Duration::from_millis(50));
                    }
                    for sample in samples {
                        let time = sample["time"].as_u64().unwrap();
                        let (style, shadow) = update(
                            !reverse,
                            Duration::from_millis(time + if reverse { 50 } else { 0 }),
                        );
                        let css = &sample["style"];
                        if style.border_width > 0. {
                            equal_color(
                                style.border,
                                css["borderBottomColor"].as_str().unwrap(),
                                &label,
                            );
                        }
                        equal_color(
                            style.background,
                            css["backgroundColor"].as_str().unwrap(),
                            &label,
                        );
                        equal_color(style.text, css["color"].as_str().unwrap(), &label);
                        equal_shadow(&shadow, css["boxShadow"].as_str().unwrap(), &label);
                        count += 1;
                    }
                } else {
                    let g = &tokens.globals;
                    let clear = name.starts_with("clear-");
                    let from = if clear {
                        g.color_text_quaternary
                    } else {
                        g.color_icon
                    }
                    .to_hsla();
                    let to = if name.ends_with("active") {
                        g.color_text
                    } else if clear {
                        g.color_icon
                    } else {
                        g.color_icon_hover
                    }
                    .to_hsla();
                    let mut motion = ColorMotion::default();
                    let duration = g.motion_duration_slow;
                    let enter_duration = if name.ends_with("focus-visible") {
                        Duration::ZERO
                    } else {
                        duration
                    };
                    motion.resolve(from, Duration::ZERO, duration, CSS_EASE, policy);
                    motion.resolve(to, Duration::ZERO, enter_duration, CSS_EASE, policy);
                    if reverse {
                        motion.resolve(from, Duration::from_millis(50), duration, CSS_EASE, policy);
                    }
                    for sample in samples {
                        let time = sample["time"].as_u64().unwrap() + if reverse { 50 } else { 0 };
                        let (color, _) = motion.resolve(
                            if reverse { from } else { to },
                            Duration::from_millis(time),
                            if reverse { duration } else { enter_duration },
                            CSS_EASE,
                            policy,
                        );
                        equal_color(color, sample["style"]["color"].as_str().unwrap(), &label);
                        count += 1;
                    }
                }
            }
        }
    }
    assert_eq!(count, 720);
}
