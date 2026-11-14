// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Original TypeScript and browser CSS measurements, independent of native style code.

use gpui::Hsla;
use serde::Deserialize;

use super::{TagColor, TagStyle, TagVariant};
use crate::theme::{AlgorithmChain, Color, PresetColor, ThemeConfig};

#[derive(Deserialize)]
struct Reference {
    cases: Vec<ReferenceCase>,
}

#[derive(Deserialize)]
struct ReferenceCase {
    theme: String,
    state: String,
    tags: Vec<BrowserTag>,
}

#[derive(Deserialize)]
struct Options {
    variant: String,
    color: String,
    disabled: bool,
    checked: Option<bool>,
    mode: String,
    direction: String,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct BrowserTag {
    index: usize,
    options: Options,
    width: f64,
    height: f64,
    background: String,
    border_color: String,
    color: String,
    font_size: String,
    line_height: String,
    font_weight: String,
    padding_start: String,
    padding_end: String,
    border_width: String,
    radius: String,
    direction: String,
    icon_size: Option<String>,
    icon_gap: Option<String>,
    close_size: Option<String>,
    close_gap: Option<String>,
    close_color: Option<String>,
    group_gap: String,
}

fn css_pixels(value: &str) -> f64 {
    value
        .strip_suffix("px")
        .unwrap_or(value)
        .parse()
        .unwrap_or_else(|_| panic!("browser length: {value}"))
}

fn near(native: f64, browser: f64, tolerance: f64, context: &str) {
    assert!(
        (native - browser).abs() < tolerance,
        "{context}: native={native}, browser={browser}"
    );
}

fn compare_color(native: Hsla, browser: &str, context: &str) {
    let channels: Vec<f64> = browser
        .trim_start_matches("rgba(")
        .trim_start_matches("rgb(")
        .trim_end_matches(')')
        .split(',')
        .map(|channel| channel.trim().parse().expect("browser RGB channel"))
        .collect();
    assert!(matches!(channels.len(), 3 | 4));
    let native: gpui::Rgba = native.into();
    for (value, channel) in [native.r, native.g, native.b].into_iter().zip(&channels) {
        near(f64::from(value), channel / 255., 0.0001, context);
    }
    // Chromium rounds byte-alpha CSS colors to at most three decimals and may
    // serialize 128/255 as 0.5. The native color keeps the byte's exact alpha.
    near(
        f64::from(native.a),
        channels.get(3).copied().unwrap_or(1.),
        0.0021,
        context,
    );
}

fn color(value: &str) -> TagColor {
    match value {
        "default" => TagColor::Default,
        "success" => TagColor::Success,
        "processing" => TagColor::Processing,
        "error" => TagColor::Error,
        "warning" => TagColor::Warning,
        value if value.starts_with('#') => TagColor::Custom(value.parse::<Color>().unwrap()),
        value => TagColor::Preset(
            PresetColor::ALL
                .into_iter()
                .find(|color| color.name() == value)
                .unwrap_or_else(|| panic!("original preset color {value}")),
        ),
    }
}

#[test]
fn tag_matches_original_browser_colors_states_and_geometry() {
    let reference: Reference =
        serde_json::from_str(include_str!("../../fixtures/tag/reference.json"))
            .expect("independent original Tag fixture");
    assert_eq!(reference.cases.len(), 12);
    let mut compared = 0;
    for case in reference.cases {
        let algorithms = match case.theme.as_str() {
            "default" => AlgorithmChain::default(),
            "dark" => AlgorithmChain::dark(),
            "compact" => AlgorithmChain::compact(),
            "dark-compact" => AlgorithmChain::dark_compact(),
            theme => panic!("original theme {theme}"),
        };
        let theme = ThemeConfig {
            algorithms: Some(algorithms),
            ..Default::default()
        }
        .resolve();
        assert_eq!(case.tags.len(), 294);
        let text_width = case
            .tags
            .iter()
            .find(|tag| tag.options.mode == "text")
            .expect("plain text measurement");
        let label_width = text_width.width
            - 2. * css_pixels(&text_width.padding_start)
            - 2. * css_pixels(&text_width.border_width);
        for browser in case.tags {
            let context = format!(
                "{} {} #{} {} {} {}",
                case.theme,
                case.state,
                browser.index,
                browser.options.direction,
                browser.options.variant,
                browser.options.color
            );
            let variant = match browser.options.variant.as_str() {
                "filled" => TagVariant::Filled,
                "solid" => TagVariant::Solid,
                "outlined" => TagVariant::Outlined,
                value => panic!("original variant {value}"),
            };
            let style = TagStyle::resolve(
                &theme,
                color(&browser.options.color),
                variant,
                browser.options.checked,
                browser.options.disabled,
            );
            let colors = match case.state.as_str() {
                "rest" => style.rest,
                "hover" => style.hover,
                "active" => style.active,
                state => panic!("original interaction {state}"),
            };
            compare_color(colors.background, &browser.background, &context);
            compare_color(colors.border, &browser.border_color, &context);
            compare_color(colors.foreground, &browser.color, &context);
            if let Some(close) = &browser.close_color {
                compare_color(colors.close, close, &context);
            }
            for (native, browser) in [
                (style.font_size, &browser.font_size),
                (style.line_height, &browser.line_height),
                (style.line_width, &browser.border_width),
                (style.padding_inline, &browser.padding_start),
                (style.padding_inline, &browser.padding_end),
                (style.radius, &browser.radius),
                (style.group_gap, &browser.group_gap),
            ] {
                near(native, css_pixels(browser), 0.001, &context);
            }
            assert_eq!(browser.font_weight, "400", "{context}");
            assert_eq!(browser.direction, browser.options.direction, "{context}");
            let mut width = 2. * (style.padding_inline + style.line_width);
            if browser.options.mode != "empty" && browser.options.mode != "icon-only" {
                width += label_width;
            }
            if let Some(size) = &browser.icon_size {
                near(style.icon_size, css_pixels(size), 0.001, &context);
                width += style.icon_size;
            }
            if let Some(gap) = &browser.icon_gap {
                near(style.padding_inline, css_pixels(gap), 0.001, &context);
                width += style.padding_inline;
            }
            if let Some(size) = &browser.close_size {
                near(style.close_icon_size, css_pixels(size), 0.001, &context);
                width += style.close_icon_size;
            }
            if let Some(gap) = &browser.close_gap {
                let native = if browser.options.mode == "icon-close" {
                    style.padding_inline
                } else {
                    style.close_gap
                };
                near(native, css_pixels(gap), 0.001, &context);
                width += native;
            }
            near(width, browser.width, 0.02, &context);
            let height = 2. * style.line_width
                + if browser.options.mode == "empty" {
                    0.
                } else {
                    style.line_height
                };
            near(height, browser.height, 0.02, &context);
            compared += 1;
        }
    }
    assert_eq!(compared, 3528);
}
