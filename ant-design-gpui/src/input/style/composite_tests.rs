// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::collections::HashMap;

use serde::Deserialize;
use serde_json::Value;

use super::{
    reference_tests::{color_equal, css_color},
    *,
};
use crate::{
    button::{ButtonStyle, ButtonStyleOptions, ButtonTheme},
    input::otp::OtpTheme,
    theme::{AlgorithmChain, ThemeConfig},
};

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
    controls: Vec<HashMap<String, usize>>,
}
#[derive(Deserialize)]
struct Reference {
    scale: f32,
    profile: String,
    options: Vec<Options>,
    styles: Vec<Value>,
    cases: Vec<Case>,
}

#[test]
fn input_composite_styles_match_original_browser_matrices() {
    let mut failures = Vec::new();
    let mut compared = 0;
    for json in [
        include_str!("../../../fixtures/input/composite-default/reference.json"),
        include_str!("../../../fixtures/input/composite-typography/reference.json"),
    ] {
        let fixture: Reference = serde_json::from_str(json).unwrap();
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
            let mut tokens = (*theme.component::<InputTheme>()).clone();
            if fixture.profile == "typography" {
                InputTheme::apply(
                    &mut tokens,
                    &InputTokenPatch {
                        input_font_size: Some(18.),
                        input_font_size_lg: Some(21.),
                        ..Default::default()
                    },
                );
            }
            for (options, parts) in fixture.options.iter().zip(case.controls) {
                compared += 1;
                let context = format!(
                    "{}/{}/{} {options:?}",
                    fixture.profile, case.theme, case.state
                );
                let size = match options.size.as_str() {
                    "small" => Size::Small,
                    "medium" => Size::Medium,
                    "large" => Size::Large,
                    _ => panic!(),
                };
                let variant = match options.variant.as_str() {
                    "outlined" => Variant::Outlined,
                    "filled" => Variant::Filled,
                    "borderless" => Variant::Borderless,
                    "underlined" => Variant::Underlined,
                    _ => panic!(),
                };
                let status = match options.status.as_str() {
                    "none" => ValidationStatus::None,
                    "error" => ValidationStatus::Error,
                    "warning" => ValidationStatus::Warning,
                    _ => panic!(),
                };
                let disabled = options.availability == "disabled";
                let interaction = InputInteraction {
                    disabled,
                    hovered: case.state == "hover",
                    focused: case.state == "focus" && !disabled,
                    keyboard_focus: case.state == "focus",
                };
                let mut style =
                    InputStyle::from_tokens(&tokens, size, variant, status, interaction)
                        .at_scale(fixture.scale);
                if matches!(
                    options.kind.as_str(),
                    "affix" | "password" | "textarea-count"
                ) {
                    style = style.affix(&tokens, size, variant, options.kind == "textarea-count");
                }
                let part = |name: &str| &fixture.styles[parts[name]];
                let mut numeric = Vec::new();
                let mut colors = Vec::new();
                let mut height = style.line_height
                    + 2. * style.padding_block
                    + if variant == Variant::Borderless {
                        0.
                    } else {
                        2. * style.border_width
                    };
                let frame = match options.kind.as_str() {
                    "search-icon" | "search-label" => {
                        let icon = options.kind == "search-icon";
                        let (color, button_variant, refinement) =
                            search_button_style(&tokens, size, variant, !icon, disabled);
                        let mut button = ButtonStyle::from_tokens(
                            &theme.component::<ButtonTheme>(),
                            ButtonStyleOptions {
                                size,
                                color,
                                variant: button_variant,
                                disabled,
                                icon_only: icon,
                                ..Default::default()
                            },
                        );
                        refinement.apply(&mut button, icon);
                        let state = if case.state == "hover" && !disabled {
                            button.hover
                        } else {
                            button.rest
                        };
                        numeric.extend([
                            (
                                "button height",
                                button.metrics.height,
                                part("button")["height"].as_f64().unwrap(),
                            ),
                            (
                                "button font",
                                button.metrics.font_size,
                                part("button")["fontSize"].as_f64().unwrap(),
                            ),
                            (
                                "button padding",
                                button.metrics.padding_inline,
                                part("button")["paddingLeft"].as_f64().unwrap(),
                            ),
                        ]);
                        if icon {
                            numeric.push((
                                "button width",
                                button.metrics.height,
                                part("button")["width"].as_f64().unwrap(),
                            ));
                        }
                        colors.extend([
                            (
                                "button background",
                                state.background.to_hsla(),
                                part("button")["backgroundColor"].as_str().unwrap(),
                            ),
                            (
                                "button border",
                                state.border.to_hsla(),
                                part("button")["borderBottomColor"].as_str().unwrap(),
                            ),
                            (
                                "button text",
                                state.text.to_hsla(),
                                part("button")["color"].as_str().unwrap(),
                            ),
                        ]);
                        height = height.max(button.metrics.height);
                        part("field")
                    }
                    "textarea-count" => {
                        style = style.multiline(&tokens);
                        height = 2. * (style.line_height + style.padding_block)
                            + if variant == Variant::Borderless {
                                0.
                            } else {
                                2. * style.border_width
                            };
                        part("frame")
                    }
                    "masked-otp" => {
                        let otp = theme.component::<OtpTheme>();
                        numeric.push((
                            "mask font",
                            otp.globals.font_size,
                            part("mask-0")["fontSize"].as_f64().unwrap(),
                        ));
                        colors.push((
                            "mask color",
                            otp.globals.color_text.to_hsla(),
                            part("mask-0")["color"].as_str().unwrap(),
                        ));
                        part("cell-0")
                    }
                    "group" => part("first"),
                    "affix" | "password" => {
                        let g = &tokens.globals;
                        let (start, end) = if options.direction == "ltr" {
                            ("marginLeft", "marginRight")
                        } else {
                            ("marginRight", "marginLeft")
                        };
                        numeric.extend([
                            (
                                "suffix gap",
                                g.padding_xxs,
                                part("suffix")[start].as_f64().unwrap(),
                            ),
                            (
                                "clear leading gap",
                                g.padding_xxs,
                                part("clear")[start].as_f64().unwrap(),
                            ),
                            (
                                "clear trailing gap",
                                g.padding_xs,
                                part("clear")[end].as_f64().unwrap(),
                            ),
                        ]);
                        if options.kind == "password" {
                            colors.push((
                                "visibility color",
                                if case.state == "hover" && !disabled {
                                    g.color_icon_hover
                                } else {
                                    g.color_icon
                                }
                                .to_hsla(),
                                part("visibility")["color"].as_str().unwrap(),
                            ));
                        }
                        part("frame")
                    }
                    _ => panic!(),
                };
                numeric.extend([
                    ("frame height", height, frame["height"].as_f64().unwrap()),
                    (
                        "frame font",
                        style.font_size,
                        frame["fontSize"].as_f64().unwrap(),
                    ),
                    (
                        "frame border",
                        style.border_width,
                        frame["borderBottomWidth"].as_f64().unwrap(),
                    ),
                ]);
                colors.push((
                    "frame background",
                    style.background,
                    frame["backgroundColor"].as_str().unwrap(),
                ));
                if variant != Variant::Borderless {
                    colors.push((
                        "frame border",
                        style.border,
                        frame["borderBottomColor"].as_str().unwrap(),
                    ));
                }
                for (label, native, browser) in numeric {
                    if (native - browser).abs() > 0.05 {
                        failures.push(format!(
                            "{context} {label}: native={native}, browser={browser}"
                        ));
                    }
                }
                for (label, native, browser) in colors {
                    if !color_equal(native, css_color(browser)) {
                        failures.push(format!(
                            "{context} {label}: native={native:?}, browser={browser}"
                        ));
                    }
                }
            }
        }
    }
    assert_eq!(compared, 20_160);
    let mut samples = std::collections::BTreeMap::new();
    for failure in &failures {
        let kind = failure
            .split("kind: ")
            .nth(1)
            .unwrap()
            .split(',')
            .next()
            .unwrap();
        let label = failure
            .split("} ")
            .nth(1)
            .unwrap()
            .split(':')
            .next()
            .unwrap();
        samples.entry(format!("{kind}/{label}")).or_insert(failure);
    }
    assert!(
        failures.is_empty(),
        "{} mismatches:\n{}",
        failures.len(),
        samples
            .values()
            .map(|v| v.as_str())
            .collect::<Vec<_>>()
            .join("\n")
    );
}
