// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Independent browser measurements from the original Ant Design style generator.

use serde::Deserialize;

use super::{
    SegmentedShape, SegmentedStyle, SegmentedStyleOptions, SegmentedTheme, SegmentedTokenPatch,
};
use crate::{
    Size,
    theme::{AlgorithmChain, Color, ComponentThemeConfig, ShadowList, ThemeConfig, TokenPatch},
};

#[derive(Deserialize)]
struct Reference {
    cases: Vec<ReferenceCase>,
}

#[derive(Deserialize)]
struct ReferenceCase {
    theme: String,
    direction: String,
    segmented: Vec<BrowserSegmented>,
}

#[derive(Deserialize)]
struct BrowserSegmented {
    options: Options,
    state: String,
    bounds: BrowserBounds,
    style: BrowserStyle,
    items: Vec<BrowserItem>,
}

#[derive(Deserialize)]
struct Options {
    id: String,
    size: Option<String>,
    shape: Option<String>,
    orientation: Option<String>,
    #[serde(default)]
    block: bool,
    #[serde(default)]
    custom: bool,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct BrowserItem {
    selected: bool,
    disabled: bool,
    bounds: BrowserBounds,
    style: BrowserStyle,
    overlay: BrowserStyle,
    label: BrowserStyle,
    icon_gap: Option<f64>,
}

#[derive(Deserialize)]
struct BrowserBounds {
    width: f64,
    height: f64,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct BrowserStyle {
    color: String,
    background: String,
    font_size: f64,
    line_height: f64,
    padding_left: f64,
    padding_right: f64,
    padding_top: f64,
    radius: f64,
    outline_color: String,
    outline_width: f64,
    outline_offset: f64,
    box_shadow: String,
    opacity: f64,
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

fn number(context: &str, property: &str, native: f64, browser: f64) {
    assert!(
        (native - browser).abs() < 0.001,
        "{context}: {property}: native={native}, browser={browser}"
    );
}

fn color(context: &str, property: &str, native: Color, browser: &str) {
    let browser = css_color(browser);
    assert!(
        native.r == browser.r
            && native.g == browser.g
            && native.b == browser.b
            // Chromium quantizes color alpha to an 8-bit channel before its
            // CSSOM serialization (e.g. 0.006 becomes 0.008). Compare the
            // quantized values rather than rejecting that expected round trip.
            && (native.a * 255.).round() == (browser.a * 255.).round(),
        "{context}: {property}: native={native:?}, browser={browser:?}"
    );
}

fn shadows(context: &str, native: &ShadowList, browser: &str) {
    let browser: Vec<_> = browser.split("px, ").collect();
    assert_eq!(native.len(), browser.len(), "{context}: shadow count");
    for (native, browser) in native.iter().zip(browser) {
        let (rgba, geometry) = browser.split_once(") ").expect("browser shadow");
        color(context, "shadow color", native.color, &format!("{rgba})"));
        let geometry: Vec<f64> = geometry
            .split_whitespace()
            .map(|value| value.trim_end_matches("px").parse().expect("shadow length"))
            .collect();
        assert_eq!(geometry.len(), 4);
        for (property, native, browser) in [
            ("shadow x", native.x, geometry[0]),
            ("shadow y", native.y, geometry[1]),
            ("shadow blur", native.blur, geometry[2]),
            ("shadow spread", native.spread, geometry[3]),
        ] {
            number(context, property, native, browser);
        }
    }
}

#[test]
fn segmented_visuals_match_original_browser_computed_styles() {
    let reference: Reference =
        serde_json::from_str(include_str!("../../fixtures/segmented/reference.json"))
            .expect("independent browser-computed Segmented fixture");
    assert_eq!(reference.cases.len(), 8);
    let mut compared_items = 0;
    for case in reference.cases {
        let algorithms = match case.theme.as_str() {
            "default" => AlgorithmChain::default(),
            "dark" => AlgorithmChain::dark(),
            "compact" => AlgorithmChain::compact(),
            "dark-compact" => AlgorithmChain::dark_compact(),
            value => panic!("unknown reference theme {value}"),
        };
        let mut config = ThemeConfig {
            algorithms: Some(algorithms),
            ..Default::default()
        };
        let theme = config.resolve();
        config.components.set_theme(
            ComponentThemeConfig::<SegmentedTheme>::new(SegmentedTokenPatch {
                track_padding: Some(3.),
                track_bg: Some(Color::rgb(0xf0f8ff)),
                item_hover_color: Some(Color::rgb(0x654321)),
                item_selected_bg: Some(Color::rgb(0xd6e4ff)),
                ..Default::default()
            })
            .with_tokens(TokenPatch {
                control_height: Some(38.),
                control_padding_horizontal: Some(18.),
                line_width_focus: Some(4.),
                margin_sm: Some(16.),
                ..Default::default()
            }),
        );
        let custom_theme = config.resolve();
        assert_eq!(case.segmented.len(), 72);
        for browser in case.segmented {
            let context = format!(
                "{} / {} / {} / {}",
                case.theme, case.direction, browser.options.id, browser.state
            );
            let options = SegmentedStyleOptions {
                size: match browser.options.size.as_deref() {
                    None | Some("medium") => Size::Medium,
                    Some("small") => Size::Small,
                    Some("large") => Size::Large,
                    value => panic!("unknown reference size {value:?}"),
                },
                shape: match browser.options.shape.as_deref() {
                    None | Some("default") => SegmentedShape::Default,
                    Some("round") => SegmentedShape::Round,
                    value => panic!("unknown reference shape {value:?}"),
                },
                disabled: false,
            };
            let effective_theme = if browser.options.custom {
                &custom_theme
            } else {
                &theme
            };
            let style = SegmentedStyle::resolve(effective_theme, options);
            let m = style.metrics;
            for (property, native, browser) in [
                (
                    "track padding left",
                    m.track_padding,
                    browser.style.padding_left,
                ),
                (
                    "track padding right",
                    m.track_padding,
                    browser.style.padding_right,
                ),
                (
                    "track padding top",
                    m.track_padding,
                    browser.style.padding_top,
                ),
                ("track radius", m.track_radius, browser.style.radius),
            ] {
                number(&context, property, native, browser);
            }
            color(
                &context,
                "track background",
                style.track_background,
                &browser.style.background,
            );
            let vertical = browser.options.orientation.as_deref() == Some("vertical");
            let expected_height = if vertical {
                m.item_height * browser.items.len() as f64 + 2. * m.track_padding
            } else {
                m.height
            };
            number(
                &context,
                "track height",
                expected_height,
                browser.bounds.height,
            );
            if browser.options.block {
                number(&context, "block width", 480., browser.bounds.width);
            }
            for item in browser.items {
                compared_items += 1;
                let native = SegmentedStyle::resolve(
                    effective_theme,
                    SegmentedStyleOptions {
                        disabled: item.disabled,
                        ..options
                    },
                );
                let state = if item.selected {
                    native.selected
                } else {
                    match browser.state.as_str() {
                        "hover" => native.hover,
                        "active" => native.active,
                        "rest" | "focus" => native.rest,
                        value => panic!("unknown reference state {value}"),
                    }
                };
                color(&context, "item text", state.text, &item.style.color);
                color(&context, "label text", state.text, &item.label.color);
                let background = if item.selected {
                    &item.style.background
                } else {
                    &item.overlay.background
                };
                color(&context, "item background", state.background, background);
                for (property, native, browser) in [
                    ("item height", m.item_height, item.bounds.height),
                    ("item radius", m.item_radius, item.style.radius),
                    ("font size", m.font_size, item.label.font_size),
                    ("line height", m.line_height, item.label.line_height),
                    (
                        "label padding left",
                        m.padding_inline,
                        item.label.padding_left,
                    ),
                    (
                        "label padding right",
                        m.padding_inline,
                        item.label.padding_right,
                    ),
                ] {
                    number(&context, property, native, browser);
                }
                if browser.options.block && !vertical {
                    let width = (browser.bounds.width - 2. * m.track_padding) / 3.;
                    assert!(
                        (item.bounds.width - width).abs() < 0.02,
                        "{context}: equal block items"
                    );
                }
                if let Some(gap) = item.icon_gap {
                    number(&context, "icon gap", m.icon_gap, gap);
                }
                if item.selected {
                    shadows(&context, &native.selected_shadow, &item.style.box_shadow);
                } else {
                    assert_eq!(
                        item.style.box_shadow, "none",
                        "{context}: unselected shadow"
                    );
                    let visible =
                        !item.disabled && matches!(browser.state.as_str(), "hover" | "active");
                    number(
                        &context,
                        "overlay opacity",
                        f64::from(visible),
                        item.overlay.opacity,
                    );
                }
                if browser.state == "focus" && !item.disabled {
                    color(
                        &context,
                        "focus color",
                        native.focus_color,
                        &item.style.outline_color,
                    );
                    number(
                        &context,
                        "focus width",
                        native.focus_width,
                        item.style.outline_width,
                    );
                    number(
                        &context,
                        "focus offset",
                        native.focus_offset,
                        item.style.outline_offset,
                    );
                }
            }
        }
    }
    assert_eq!(compared_items, 1728);
}
