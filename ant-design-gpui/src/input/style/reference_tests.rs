// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;
use crate::theme::{AlgorithmChain, ThemeConfig};
use serde::Deserialize;

#[derive(Deserialize)]
struct Reference {
    scale: f32,
    #[serde(default)]
    profile: String,
    options: Vec<Options>,
    styles: Vec<BrowserStyle>,
    cases: Vec<Case>,
}
#[derive(Deserialize, Debug)]
struct Options {
    kind: String,
    size: String,
    variant: String,
    status: String,
    availability: String,
    direction: String,
}
#[derive(Deserialize)]
struct Case {
    theme: String,
    state: String,
    controls: Vec<[Option<usize>; 3]>,
}
#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct BrowserStyle {
    height: f64,
    padding_top: f64,
    padding_left: f64,
    font_size: f64,
    line_height: f64,
    border_bottom_width: f64,
    border_top_left_radius: f64,
    background_color: String,
    border_bottom_color: String,
    color: String,
}
pub(super) fn css_color(value: &str) -> Hsla {
    let c: Vec<f64> = value
        .trim_start_matches("rgba(")
        .trim_start_matches("rgb(")
        .trim_end_matches(')')
        .split(',')
        .map(|part| part.trim().parse().unwrap())
        .collect();
    Color {
        r: c[0] as u8,
        g: c[1] as u8,
        b: c[2] as u8,
        a: c.get(3).copied().unwrap_or(1.),
    }
    .to_hsla()
}
pub(super) fn color_equal(left: Hsla, right: Hsla) -> bool {
    let left = gpui::Rgba::from(left);
    let right = gpui::Rgba::from(right);
    (left.a - right.a).abs() < 0.005
        && ((left.r * left.a - right.r * right.a).abs() < 0.005)
        && ((left.g * left.a - right.g * right.a).abs() < 0.005)
        && ((left.b * left.a - right.b * right.a).abs() < 0.005)
}
#[test]
fn input_styles_match_original_browser_matrix() {
    compare_matrix(include_str!("../../../fixtures/input/reference.json"));
}

#[test]
fn input_custom_tokens_match_original_browser_matrices() {
    for fixture in [
        include_str!("../../../fixtures/input/typography/reference.json"),
        include_str!("../../../fixtures/input/geometry/reference.json"),
        include_str!("../../../fixtures/input/colors/reference.json"),
    ] {
        compare_matrix(fixture);
    }
}

fn compare_matrix(json: &str) {
    let fixture: Reference = serde_json::from_str(json).unwrap();
    let mut failures = Vec::new();
    let mut compared = 0;
    for case in fixture.cases {
        let theme = ThemeConfig {
            algorithms: Some(match case.theme.as_str() {
                "default" => AlgorithmChain::default(),
                "dark" => AlgorithmChain::dark(),
                "compact" => AlgorithmChain::compact(),
                "dark-compact" => AlgorithmChain::dark_compact(),
                _ => panic!(),
            }),
            ..Default::default()
        }
        .resolve();
        let base_tokens = theme.component::<InputTheme>();
        for (options, row) in fixture.options.iter().zip(case.controls) {
            compared += 1;
            let browser = &fixture.styles[row[if options.kind == "otp" { 1 } else { 0 }].unwrap()];
            let browser_text = if options.kind == "numeric" {
                &fixture.styles[row[1].unwrap()]
            } else {
                browser
            };
            let mut tokens = if options.kind == "numeric" {
                theme
                    .component::<crate::input_number::InputNumberTheme>()
                    .input
                    .clone()
            } else {
                (*base_tokens).clone()
            };
            let patch = match fixture.profile.as_str() {
                "typography" => InputTokenPatch {
                    input_font_size: Some(18.),
                    input_font_size_lg: Some(21.),
                    ..Default::default()
                },
                "geometry" => InputTokenPatch {
                    input_font_size: Some(15.5),
                    input_font_size_sm: Some(12.),
                    input_font_size_lg: Some(19.),
                    padding_block: Some(7.),
                    padding_inline: Some(17.),
                    padding_block_sm: Some(2.5),
                    padding_inline_sm: Some(9.),
                    padding_block_lg: Some(9.),
                    padding_inline_lg: Some(21.),
                    ..Default::default()
                },
                "colors" => InputTokenPatch {
                    active_bg: Some(Color::rgb(0xe6fffb)),
                    hover_bg: Some(Color::rgb(0xf0f5ff)),
                    active_border_color: Some(Color::rgb(0x531dab)),
                    hover_border_color: Some(Color::rgb(0x08979c)),
                    active_shadow: Some(Arc::from([Shadow {
                        spread: 3.,
                        color: Color {
                            r: 83,
                            g: 29,
                            b: 171,
                            a: 0.2,
                        },
                        ..Default::default()
                    }])),
                    ..Default::default()
                },
                "" | "default" => InputTokenPatch::default(),
                other => panic!("Unknown reference profile {other}"),
            };
            InputTheme::apply(&mut tokens, &patch);
            if options.kind == "otp" {
                let otp = theme.component::<crate::input::otp::OtpTheme>();
                tokens.padding_inline = otp.padding_inline;
                tokens.padding_inline_sm = otp.padding_inline_sm;
                tokens.padding_inline_lg = otp.padding_inline_lg;
            }
            let variant = match options.variant.as_str() {
                "outlined" => Variant::Outlined,
                "filled" => Variant::Filled,
                "borderless" => Variant::Borderless,
                "underlined" => Variant::Underlined,
                _ => panic!(),
            };
            let mut style = InputStyle::from_tokens(
                &tokens,
                match options.size.as_str() {
                    "small" => Size::Small,
                    "medium" => Size::Medium,
                    "large" => Size::Large,
                    _ => panic!(),
                },
                variant,
                match options.status.as_str() {
                    "none" => ValidationStatus::None,
                    "error" => ValidationStatus::Error,
                    "warning" => ValidationStatus::Warning,
                    _ => panic!(),
                },
                InputInteraction {
                    disabled: options.availability == "disabled",
                    hovered: case.state == "hover",
                    focused: case.state == "focus" && options.availability != "disabled",
                    keyboard_focus: case.state == "focus",
                },
            )
            .at_scale(fixture.scale);
            if options.kind == "textarea" {
                style = style.multiline(&tokens);
            }
            if options.kind == "numeric" {
                style = style.numeric(
                    &tokens,
                    match options.size.as_str() {
                        "small" => Size::Small,
                        "medium" => Size::Medium,
                        "large" => Size::Large,
                        _ => panic!(),
                    },
                    variant,
                    match options.status.as_str() {
                        "none" => ValidationStatus::None,
                        "error" => ValidationStatus::Error,
                        "warning" => ValidationStatus::Warning,
                        _ => panic!(),
                    },
                    options.availability == "disabled",
                );
            }
            let context = format!(
                "{}/{}/{}/{}/{}/{}/{}/{}",
                case.theme,
                case.state,
                options.kind,
                options.size,
                options.variant,
                options.status,
                options.availability,
                options.direction
            );
            for (label, native, web) in [
                (
                    "height",
                    style.line_height * if options.kind == "textarea" { 2. } else { 1. }
                        + 2. * style.padding_block
                        + if variant == Variant::Borderless {
                            0.
                        } else {
                            2. * style.border_width
                        },
                    browser.height,
                ),
                ("padding", style.padding_block, browser_text.padding_top),
                ("inline", style.padding_inline, browser.padding_left),
                ("font", style.font_size, browser_text.font_size),
                ("line", style.line_height, browser_text.line_height),
                ("border", style.border_width, browser.border_bottom_width),
                ("radius", style.radius, browser.border_top_left_radius),
            ] {
                if (native - web).abs() > 0.05 {
                    failures.push(format!("{context} {label}: native={native}, browser={web}"));
                }
            }
            for (label, native, web) in [
                ("background", style.background, &browser.background_color),
                ("text", style.text, &browser_text.color),
            ] {
                if !color_equal(native, css_color(web)) {
                    failures.push(format!(
                        "{context} {label}: native={native:?}, browser={web}"
                    ));
                }
            }
            if variant != Variant::Borderless
                && !color_equal(style.border, css_color(&browser.border_bottom_color))
            {
                failures.push(format!(
                    "{context} border color: native={:?}, browser={}",
                    style.border, browser.border_bottom_color
                ));
            }
        }
    }
    assert_eq!(compared, 8064);
    assert!(
        failures.is_empty(),
        "{} mismatches:\n{}",
        failures.len(),
        failures
            .iter()
            .take(45)
            .cloned()
            .collect::<Vec<_>>()
            .join("\n")
    );
}
