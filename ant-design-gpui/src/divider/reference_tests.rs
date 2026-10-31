// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Independent computed styles captured from the original Ant Design generators.

use gpui::px;
use serde::Deserialize;

use super::{DividerStyle, DividerTheme, DividerTokenPatch};
use crate::{
    Size,
    theme::{AlgorithmChain, Color, ComponentThemeConfig, ThemeConfig, TokenPatch},
};

#[derive(Deserialize)]
struct Reference {
    cases: Vec<ReferenceCase>,
}

#[derive(Deserialize)]
struct ReferenceCase {
    theme: String,
    direction: String,
    dividers: Vec<BrowserDivider>,
}

#[derive(Deserialize)]
struct BrowserDivider {
    options: Options,
    bounds: BrowserBounds,
    style: BrowserStyle,
    content: Option<BrowserElement>,
    rails: Vec<BrowserElement>,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct Options {
    id: String,
    size: Option<String>,
    #[serde(default)]
    orientation: String,
    #[serde(default)]
    plain: bool,
    #[serde(default)]
    custom: bool,
    resolved_placement: String,
    title_margin: Option<serde_json::Value>,
}

#[derive(Deserialize)]
struct BrowserElement {
    bounds: BrowserBounds,
    style: BrowserStyle,
}

#[derive(Deserialize)]
struct BrowserBounds {
    width: f32,
    height: f32,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct BrowserStyle {
    color: String,
    font_size: f32,
    font_weight: f32,
    line_height: f32,
    margin_top: f32,
    margin_right: f32,
    margin_bottom: f32,
    margin_left: f32,
    padding_left: f32,
    padding_right: f32,
    border_top_width: f32,
    border_top_color: String,
    border_left_width: f32,
    border_left_color: String,
    border_right_width: f32,
    border_right_color: String,
    top: f32,
}

fn css_color(value: &str) -> Color {
    let channels: Vec<f64> = value
        .trim_start_matches("rgba(")
        .trim_start_matches("rgb(")
        .trim_end_matches(')')
        .split(',')
        .map(|channel| channel.trim().parse().expect("browser RGB channel"))
        .collect();
    assert!(matches!(channels.len(), 3 | 4));
    Color {
        r: channels[0] as u8,
        g: channels[1] as u8,
        b: channels[2] as u8,
        a: channels.get(3).copied().unwrap_or(1.),
    }
}

fn compare_number(context: &str, property: &str, native: f32, browser: f32, tolerance: f32) {
    assert!(
        (native - browser).abs() <= tolerance,
        "{context}: {property}: native {native}, browser {browser}"
    );
}

fn compare_color(context: &str, property: &str, native: Color, browser: &str) {
    let browser = css_color(browser);
    assert!(
        native.r == browser.r
            && native.g == browser.g
            && native.b == browser.b
            && (native.a - browser.a).abs() < 0.001,
        "{context}: {property}: native {native:?}, browser {browser:?}"
    );
}

#[test]
fn divider_visual_metrics_match_original_browser_computed_styles() {
    let reference: Reference =
        serde_json::from_str(include_str!("../../fixtures/divider/reference.json"))
            .expect("independent browser-computed Divider fixture");
    assert_eq!(reference.cases.len(), 8);
    let mut compared = 0;
    for case in reference.cases {
        let algorithms = match case.theme.as_str() {
            "default" => AlgorithmChain::default(),
            "dark" => AlgorithmChain::dark(),
            "compact" => AlgorithmChain::compact(),
            "dark-compact" => AlgorithmChain::dark_compact(),
            name => panic!("unknown browser theme {name}"),
        };
        let mut configuration = ThemeConfig {
            algorithms: Some(algorithms),
            ..Default::default()
        };
        let theme = configuration.resolve();
        // The source customization demo overrides these global and component
        // tokens. Apply them through the component registry to also exercise
        // Divider-local global aliases without changing unrelated components.
        configuration.components.set_theme::<DividerTheme>(
            ComponentThemeConfig::new(DividerTokenPatch {
                text_padding_inline: Some(px(16.).into()),
                orientation_margin: Some(0.2),
                vertical_margin_inline: Some(16.),
            })
            .with_tokens(TokenPatch {
                margin: Some(24.),
                margin_lg: Some(48.),
                line_width: Some(5.),
                color_split: Some(Color::rgb(0x1677ff)),
                ..Default::default()
            }),
        );
        let custom_theme = configuration.resolve();
        assert_eq!(case.dividers.len(), 74);
        for divider in case.dividers {
            compared += 1;
            let context = format!(
                "{} / {} / {}",
                case.theme, case.direction, divider.options.id
            );
            let size = match divider.options.size.as_deref() {
                None => None,
                Some("small") => Some(Size::Small),
                Some("medium") => Some(Size::Medium),
                Some("large") => Some(Size::Large),
                Some(name) => panic!("unknown browser size {name}"),
            };
            let vertical = divider.options.orientation == "vertical";
            let native = DividerStyle::resolve(
                if divider.options.custom {
                    &custom_theme
                } else {
                    &theme
                },
                size,
                divider.content.is_some(),
                divider.options.plain,
            );
            for (property, native, browser) in [
                ("font size", native.font_size, divider.style.font_size),
                ("font weight", native.font_weight, divider.style.font_weight),
                (
                    "line height",
                    native.line_height * native.font_size,
                    divider.style.line_height,
                ),
            ] {
                compare_number(&context, property, native, browser, 0.001);
            }
            compare_color(
                &context,
                "text color",
                native.text_color,
                &divider.style.color,
            );
            if vertical {
                compare_number(
                    &context,
                    "vertical height",
                    native.vertical_height,
                    divider.bounds.height,
                    0.02,
                );
                compare_number(
                    &context,
                    "vertical offset",
                    native.vertical_offset,
                    divider.style.top,
                    0.001,
                );
                for (property, browser) in [
                    ("left margin", divider.style.margin_left),
                    ("right margin", divider.style.margin_right),
                ] {
                    compare_number(
                        &context,
                        property,
                        native.vertical_margin_inline,
                        browser,
                        0.001,
                    );
                }
                let (width, color) = if case.direction == "rtl" {
                    (
                        divider.style.border_right_width,
                        &divider.style.border_right_color,
                    )
                } else {
                    (
                        divider.style.border_left_width,
                        &divider.style.border_left_color,
                    )
                };
                compare_number(&context, "vertical stroke", native.line_width, width, 0.001);
                compare_color(&context, "vertical rail color", native.line_color, color);
            } else {
                compare_number(
                    &context,
                    "top margin",
                    native.margin_block,
                    divider.style.margin_top,
                    0.001,
                );
                compare_number(
                    &context,
                    "bottom margin",
                    native.margin_block,
                    divider.style.margin_bottom,
                    0.001,
                );
                if let Some(content) = &divider.content {
                    // The source intentionally removes the edge-side padding
                    // when its explicit orientationMargin prop is supplied.
                    if divider.options.title_margin.is_none() {
                        let padding = f32::from(
                            native
                                .text_padding_inline
                                .to_pixels(px(native.font_size).into(), px(16.)),
                        );
                        compare_number(
                            &context,
                            "left title padding",
                            padding,
                            content.style.padding_left,
                            0.001,
                        );
                        compare_number(
                            &context,
                            "right title padding",
                            padding,
                            content.style.padding_right,
                            0.001,
                        );
                    }
                    compare_number(
                        &context,
                        "title line box",
                        native.font_size * native.line_height,
                        content.bounds.height,
                        0.02,
                    );
                    for rail in &divider.rails {
                        compare_number(
                            &context,
                            "horizontal stroke",
                            native.line_width,
                            rail.style.border_top_width,
                            0.001,
                        );
                        compare_color(
                            &context,
                            "horizontal rail color",
                            native.line_color,
                            &rail.style.border_top_color,
                        );
                    }
                    if divider.options.resolved_placement != "center"
                        && divider.options.title_margin.is_none()
                        && divider.rails.len() == 2
                    {
                        let start = divider.rails[0].bounds.width;
                        let end = divider.rails[1].bounds.width;
                        // Normalize measured browser rail lengths. This compares
                        // the native token with observed layout, independently
                        // of the native placement or flex implementation.
                        if start + end > 0. {
                            compare_number(
                                &context,
                                "short rail fraction",
                                native.orientation_margin,
                                start.min(end) / (start + end),
                                0.0001,
                            );
                        }
                    }
                } else {
                    compare_number(
                        &context,
                        "horizontal stroke",
                        native.line_width,
                        divider.style.border_top_width,
                        0.001,
                    );
                    compare_color(
                        &context,
                        "horizontal rail color",
                        native.line_color,
                        &divider.style.border_top_color,
                    );
                }
            }
        }
    }
    assert_eq!(compared, 592);
}
