// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Compare native tokens against actual upstream Alert markup and computed CSS.

use gpui::Hsla;
use serde::Deserialize;

use super::{AlertPadding, AlertStyle, AlertTheme, AlertTokenPatch, AlertType, AlertVariant};
use crate::theme::{AlgorithmChain, ThemeConfig};

#[derive(Deserialize)]
struct Reference {
    cases: Vec<ReferenceCase>,
}

#[derive(Deserialize)]
struct ReferenceCase {
    theme: String,
    state: String,
    alerts: Vec<BrowserAlert>,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct Options {
    #[serde(rename = "type")]
    kind: Option<String>,
    variant: Option<String>,
    mode: String,
    direction: String,
    #[serde(default)]
    banner: bool,
    #[serde(default)]
    close_disabled: bool,
    #[serde(default)]
    close_text: bool,
    #[serde(default)]
    first_line: bool,
    #[serde(default)]
    token_case: bool,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct BrowserPart {
    x: f64,
    width: f64,
    height: f64,
    color: String,
    font_size: String,
    line_height: String,
    margin_start: String,
    margin_end: String,
    margin_top: String,
    margin_bottom: String,
    outline_color: String,
    outline_width: String,
    outline_offset: String,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct BrowserAlert {
    index: usize,
    options: Options,
    x: f64,
    width: f64,
    height: f64,
    background: String,
    border_color: String,
    border_width: String,
    radius: String,
    font_size: String,
    line_height: String,
    font_weight: String,
    padding: [String; 4],
    direction: String,
    align_items: String,
    title: Option<BrowserPart>,
    description: Option<BrowserPart>,
    icon: Option<BrowserPart>,
    icon_glyph: Option<BrowserPart>,
    section: BrowserPart,
    actions: Option<BrowserPart>,
    close: Option<BrowserPart>,
    close_glyph: Option<BrowserPart>,
    role: String,
    close_disabled: bool,
    close_label: Option<String>,
}

fn pixels(value: &str) -> f64 {
    value
        .strip_suffix("px")
        .unwrap_or(value)
        .parse()
        .unwrap_or_else(|_| panic!("browser pixels {value}"))
}

fn near(native: f64, browser: f64, tolerance: f64, context: &str) {
    assert!(
        (native - browser).abs() < tolerance,
        "{context}: native={native}, browser={browser}"
    );
}

fn color(native: Hsla, browser: &str, context: &str) {
    let channels: Vec<f64> = browser
        .trim_start_matches("rgba(")
        .trim_start_matches("rgb(")
        .trim_end_matches(')')
        .split(',')
        .map(|channel| channel.trim().parse().expect("browser RGB channel"))
        .collect();
    assert!(matches!(channels.len(), 3 | 4), "{context}: {browser}");
    let native: gpui::Rgba = native.into();
    for (native, browser) in [native.r, native.g, native.b].into_iter().zip(&channels) {
        near(f64::from(native), browser / 255., 0.0001, context);
    }
    near(
        f64::from(native.a),
        channels.get(3).copied().unwrap_or(1.),
        0.0021,
        context,
    );
}

#[test]
fn alert_matches_actual_upstream_browser_styles_and_geometry() {
    let fixture: Reference =
        serde_json::from_str(include_str!("../../fixtures/alert/reference.json"))
            .expect("actual upstream React Alert browser fixture");
    assert_eq!(fixture.cases.len(), 12);
    let mut compared = 0;
    for case in fixture.cases {
        let algorithms = match case.theme.as_str() {
            "default" => AlgorithmChain::default(),
            "dark" => AlgorithmChain::dark(),
            "compact" => AlgorithmChain::compact(),
            "dark-compact" => AlgorithmChain::dark_compact(),
            value => panic!("reference theme {value}"),
        };
        let base_config = ThemeConfig {
            algorithms: Some(algorithms),
            ..Default::default()
        };
        let theme = base_config.resolve();
        for browser in case.alerts {
            let options = &browser.options;
            let context = format!(
                "{} {} #{} {} {}",
                case.theme, case.state, browser.index, options.direction, options.mode
            );
            let kind = match options.kind.as_deref() {
                Some("success") => AlertType::Success,
                Some("info") => AlertType::Info,
                Some("warning") => AlertType::Warning,
                Some("error") => AlertType::Error,
                None if options.banner => AlertType::Warning,
                None => AlertType::Info,
                value => panic!("reference type {value:?}"),
            };
            let variant = match options.variant.as_deref() {
                None | Some("outlined") => AlertVariant::Outlined,
                Some("filled") => AlertVariant::Filled,
                value => panic!("reference variant {value:?}"),
            };
            let mut token_theme = theme.clone();
            if options.token_case {
                let mut config = base_config.clone();
                config.components.set::<AlertTheme>(AlertTokenPatch {
                    radius: Some(5.),
                    default_padding: Some(AlertPadding::symmetric(10., 18.)),
                    with_description_padding: Some(AlertPadding::symmetric(14., 18.)),
                    with_description_icon_size: Some(32.),
                });
                token_theme = config.resolve();
            }
            let style = AlertStyle::resolve(
                &token_theme,
                kind,
                variant,
                browser.description.is_some(),
                options.banner,
            );
            color(style.background, &browser.background, &context);
            // Banner's border is absent; browsers retain the computed status color.
            if !options.banner {
                color(style.border, &browser.border_color, &context);
            }
            for (native, browser) in [
                (style.line_width, &browser.border_width),
                (style.radius, &browser.radius),
                (style.font_size, &browser.font_size),
                (style.line_height, &browser.line_height),
                (style.padding.top, &browser.padding[0]),
                (style.padding.right, &browser.padding[1]),
                (style.padding.bottom, &browser.padding[2]),
                (style.padding.left, &browser.padding[3]),
            ] {
                near(native, pixels(browser), 0.001, &context);
            }
            assert_eq!(browser.direction, options.direction, "{context}");
            assert_eq!(browser.font_weight, "400", "{context}");
            assert_eq!(browser.role, "alert", "{context}");
            let has_description = browser.description.is_some();
            assert_eq!(
                browser.align_items,
                if has_description || options.first_line {
                    "flex-start"
                } else {
                    "center"
                },
                "{context}"
            );
            if let Some(title) = &browser.title {
                color(style.title_color, &title.color, &context);
                near(
                    style.title_font_size,
                    pixels(&title.font_size),
                    0.001,
                    &context,
                );
                near(
                    style.title_line_height,
                    pixels(&title.line_height),
                    0.001,
                    &context,
                );
                near(
                    style.title_gap,
                    pixels(&title.margin_bottom),
                    0.001,
                    &context,
                );
            }
            if let Some(description) = &browser.description {
                color(style.description_color, &description.color, &context);
                near(
                    style.font_size,
                    pixels(&description.font_size),
                    0.001,
                    &context,
                );
                near(
                    style.line_height,
                    pixels(&description.line_height),
                    0.001,
                    &context,
                );
            }
            if let Some(icon) = &browser.icon {
                color(style.icon_color, &icon.color, &context);
                near(style.icon_size, pixels(&icon.font_size), 0.001, &context);
                near(style.icon_gap, pixels(&icon.margin_end), 0.001, &context);
                let glyph = browser.icon_glyph.as_ref().expect("actual upstream icon");
                near(style.icon_size, glyph.width, 0.02, &context);
                near(style.icon_size, glyph.height, 0.02, &context);
                near(
                    if options.first_line { 4. } else { 0. },
                    pixels(&icon.margin_top),
                    0.001,
                    &context,
                );
            }
            if let Some(action) = &browser.actions {
                near(
                    style.action_gap,
                    pixels(&action.margin_start),
                    0.001,
                    &context,
                );
            }
            if let Some(close) = &browser.close {
                near(style.close_size, pixels(&close.font_size), 0.001, &context);
                near(
                    style.close_gap,
                    pixels(&close.margin_start),
                    0.001,
                    &context,
                );
                assert_eq!(browser.close_disabled, options.close_disabled, "{context}");
                assert_eq!(
                    browser.close_label.as_deref(),
                    Some("Close alert"),
                    "{context}"
                );
                if let Some(glyph) = &browser.close_glyph {
                    let expected = if options.close_disabled {
                        style.close_disabled_color
                    } else if case.state == "hover" {
                        style.close_hover_color
                    } else {
                        style.close_color
                    };
                    color(expected, &glyph.color, &context);
                    near(style.close_size, glyph.width, 0.02, &context);
                    near(style.close_size, glyph.height, 0.02, &context);
                } else {
                    assert!(options.close_text, "{context}");
                }
                if case.state == "focus" {
                    near(
                        style.focus_width,
                        pixels(&close.outline_width),
                        0.001,
                        &context,
                    );
                    near(1., pixels(&close.outline_offset), 0.001, &context);
                    color(style.focus_color, &close.outline_color, &context);
                }
            }
            // Reconstruct the flex row from independently measured child bounds.
            let mut used = browser.section.width;
            if browser.icon.is_some() {
                used += style.icon_size + style.icon_gap;
            }
            if let Some(action) = &browser.actions {
                used += action.width + style.action_gap;
            }
            if let Some(close) = &browser.close {
                used += close.width + style.close_gap;
            }
            near(
                browser.width,
                used + style.padding.left + style.padding.right + 2. * style.line_width,
                0.03,
                &context,
            );
            assert!(
                browser.height >= style.padding.top + style.padding.bottom,
                "{context}"
            );
            assert!(browser.section.x >= browser.x, "{context}");
            compared += 1;
        }
    }
    assert_eq!(compared, 2304);
}

#[test]
fn alert_motion_tokens_match_actual_mounted_upstream_close() {
    let theme = ThemeConfig::default().resolve();
    let style = AlertStyle::resolve(
        &theme,
        AlertType::Warning,
        AlertVariant::Outlined,
        true,
        false,
    );
    for (mode, source) in [
        (
            "description",
            include_str!("../../fixtures/alert/lifecycle.json"),
        ),
        (
            "title",
            include_str!("../../fixtures/alert/lifecycle-title.json"),
        ),
        (
            "styled",
            include_str!("../../fixtures/alert/lifecycle-styled.json"),
        ),
    ] {
        let reference: serde_json::Value = serde_json::from_str(source).unwrap();
        let trace = &reference["trace"];
        assert_eq!(trace["mode"], mode);
        let events = trace["events"].as_array().unwrap();
        assert_eq!(events.len(), 2, "mounted upstream callback count");
        assert_eq!(events[0]["kind"], "onClose");
        assert_eq!(events[1]["kind"], "afterClose");
        assert!(events[0]["time"].as_f64().unwrap() < events[1]["time"].as_f64().unwrap());
        let samples = trace["samples"].as_array().unwrap();
        let initial = samples
            .iter()
            .find_map(|sample| sample["maxHeight"].as_str())
            .unwrap();
        let initial = pixels(initial);
        near(
            initial,
            trace["beforeHeight"].as_f64().unwrap().round(),
            0.001,
            mode,
        );
        let mut intermediate = 0;
        for sample in samples {
            let Some(duration) = sample["transitionDuration"].as_str() else {
                continue;
            };
            for duration in duration.split(',') {
                near(
                    style.motion_duration.as_secs_f64(),
                    duration.trim().strip_suffix('s').unwrap().parse().unwrap(),
                    0.0001,
                    mode,
                );
            }
            let curve = style.motion_easing;
            let expected = format!(
                "cubic-bezier({}, {}, {}, {})",
                curve.x1, curve.y1, curve.x2, curve.y2
            );
            assert!(
                sample["transitionTimingFunction"]
                    .as_str()
                    .unwrap()
                    .contains(&expected)
            );
            let ratio: f64 = sample["opacity"].as_str().unwrap().parse().unwrap();
            let height = pixels(sample["maxHeight"].as_str().unwrap());
            near(height, initial * ratio, 0.002, mode);
            if ratio > 0. && ratio < 1. {
                intermediate += 1;
                let expected_padding = match mode {
                    "description" => style.padding.top,
                    "styled" => 14.,
                    _ => 8. * ratio,
                };
                near(
                    pixels(sample["paddingTop"].as_str().unwrap()),
                    expected_padding,
                    0.001,
                    mode,
                );
            }
        }
        assert!(
            intermediate > 3,
            "real mounted upstream must expose intermediate frames"
        );
        assert_eq!(samples.last().unwrap()["classes"], serde_json::Value::Null);
    }
}
