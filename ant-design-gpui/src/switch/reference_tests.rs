// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Independently executed browser styles provide the visual reference.

use serde::Deserialize;

use super::{SwitchStyle, SwitchTheme, SwitchTokenPatch};
use crate::{
    Direction, Size,
    theme::{AlgorithmChain, Color, Shadow, ThemeConfig},
};

#[derive(Deserialize)]
struct Reference {
    cases: Vec<ReferenceCase>,
}

#[derive(Deserialize)]
struct ReferenceCase {
    theme: String,
    state: String,
    switches: Vec<BrowserSwitch>,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct BrowserSwitch {
    index: usize,
    checked: bool,
    disabled: bool,
    loading: bool,
    small: bool,
    direction: String,
    content: String,
    custom: bool,
    width: f64,
    height: f64,
    min_width: String,
    background: String,
    opacity: String,
    border_width: String,
    radius: String,
    cursor: String,
    handle_size: String,
    handle_height: String,
    handle_top: f64,
    handle_left: f64,
    handle_background: String,
    handle_shadow: String,
    handle_paint_width: String,
    handle_inset_start: String,
    handle_inset_end: String,
    content_color: String,
    content_font_size: String,
    content_padding_start: String,
    content_padding_end: String,
    checked_content_left: f64,
    checked_content_width: f64,
    unchecked_content_left: f64,
    unchecked_content_width: f64,
    checked_natural_width: f64,
    unchecked_natural_width: f64,
    loading_icon_size: Option<String>,
    loading_icon_color: Option<String>,
    outline_color: String,
    outline_width: String,
    outline_offset: String,
    outline_style: String,
}

fn pixels(value: &str) -> f64 {
    value
        .strip_suffix("px")
        .unwrap_or(value)
        .parse()
        .unwrap_or_else(|_| panic!("expected browser length: {value}"))
}

fn color(value: &str) -> gpui::Hsla {
    let values: Vec<f64> = value
        .trim_start_matches("rgba(")
        .trim_start_matches("rgb(")
        .trim_end_matches(')')
        .split(',')
        .map(|part| part.trim().parse().expect("browser color channel"))
        .collect();
    assert!(matches!(values.len(), 3 | 4));
    Color {
        r: values[0] as u8,
        g: values[1] as u8,
        b: values[2] as u8,
        a: values.get(3).copied().unwrap_or(1.),
    }
    .to_hsla()
}

fn near(native: f64, browser: f64, tolerance: f64, context: &str) {
    assert!(
        (native - browser).abs() <= tolerance,
        "{context}: native={native}, browser={browser}"
    );
}

fn custom_patch() -> SwitchTokenPatch {
    SwitchTokenPatch {
        track_height: Some(14.),
        track_height_sm: Some(12.),
        track_min_width: Some(32.),
        track_min_width_sm: Some(24.),
        track_padding: Some(-3.),
        handle_bg: Some(Color::rgb(0x1976d2)),
        handle_size: Some(20.),
        handle_size_sm: Some(16.),
        handle_shadow: Some(
            vec![
                Shadow {
                    y: 2.,
                    blur: 1.,
                    spread: -1.,
                    color: Color::BLACK.with_alpha(0.2),
                    ..Default::default()
                },
                Shadow {
                    y: 1.,
                    blur: 1.,
                    color: Color::BLACK.with_alpha(0.14),
                    ..Default::default()
                },
            ]
            .into(),
        ),
        inner_min_margin: Some(10.),
        inner_max_margin: Some(28.),
        inner_min_margin_sm: Some(8.),
        inner_max_margin_sm: Some(24.),
    }
}

fn compare_shadows(native: &[Shadow], browser: &str, context: &str) {
    let mut rest = browser;
    let mut index = 0;
    while !rest.is_empty() {
        let end = rest.find(')').expect("browser shadow color");
        let shadow_color = color(&rest[..=end]);
        let tail = rest[end + 1..].trim_start();
        let (dimensions, remaining) = tail.split_once(", ").unwrap_or((tail, ""));
        let values: Vec<f64> = dimensions.split_whitespace().map(pixels).collect();
        assert_eq!(values.len(), 4, "{context}");
        let shadow = native.get(index).expect("native shadow");
        assert_eq!(shadow.color.to_hsla(), shadow_color, "{context}");
        assert!(!shadow.inset, "{context}");
        for (native, browser) in [shadow.x, shadow.y, shadow.blur, shadow.spread]
            .into_iter()
            .zip(values)
        {
            near(native, browser, 0.001, context);
        }
        rest = remaining;
        index += 1;
    }
    assert_eq!(native.len(), index, "{context}");
}

#[test]
fn switch_matches_original_browser_colors_geometry_content_loading_and_focus() {
    let fixture: Reference =
        serde_json::from_str(include_str!("../../fixtures/switch/reference.json"))
            .expect("independent Switch browser fixture");
    assert_eq!(fixture.cases.len(), 16);
    let mut count = 0;
    for case in fixture.cases {
        let algorithms = match case.theme.as_str() {
            "default" => AlgorithmChain::default(),
            "dark" => AlgorithmChain::dark(),
            "compact" => AlgorithmChain::compact(),
            "dark-compact" => AlgorithmChain::dark_compact(),
            other => panic!("unknown browser theme {other}"),
        };
        let config = ThemeConfig {
            algorithms: Some(algorithms),
            ..Default::default()
        };
        let default_theme = config.resolve();
        let mut custom_config = config;
        custom_config.components.set::<SwitchTheme>(custom_patch());
        let custom_theme = custom_config.resolve();
        assert_eq!(case.switches.len(), 104);
        for browser in case.switches {
            let context = format!("{} {} #{}", case.theme, case.state, browser.index);
            let theme = if browser.custom {
                &custom_theme
            } else {
                &default_theme
            };
            let style = SwitchStyle::resolve(
                theme,
                if browser.small {
                    Size::Small
                } else {
                    Size::Medium
                },
                browser.checked,
                browser.disabled,
                browser.loading,
            );
            let active = case.state == "active" && !browser.disabled;
            let hovered = matches!(case.state.as_str(), "hover" | "active");
            let background = if hovered {
                style.hover_background
            } else {
                style.background
            };
            assert_eq!(background, color(&browser.background), "{context}");
            assert_eq!(
                style.handle_background,
                color(&browser.handle_background),
                "{context}"
            );
            assert_eq!(
                style.content_color,
                color(&browser.content_color),
                "{context}"
            );
            near(style.opacity, pixels(&browser.opacity), 0.001, &context);
            near(style.track_height, browser.height, 0.001, &context);
            near(
                style.track_min_width,
                pixels(&browser.min_width),
                0.001,
                &context,
            );
            near(
                style.handle_size,
                pixels(&browser.handle_size),
                0.001,
                &context,
            );
            near(
                style.handle_size,
                pixels(&browser.handle_height),
                0.001,
                &context,
            );
            near(style.track_padding, browser.handle_top, 0.001, &context);
            near(
                style.font_size,
                pixels(&browser.content_font_size),
                0.001,
                &context,
            );
            near(pixels(&browser.border_width), 0., 0.001, &context);
            near(pixels(&browser.radius), 100., 0.001, &context);
            assert_eq!(
                browser.cursor,
                if browser.disabled {
                    "not-allowed"
                } else {
                    "pointer"
                },
                "{context}"
            );
            compare_shadows(&style.handle_shadow, &browser.handle_shadow, &context);

            let direction = if browser.direction == "rtl" {
                Direction::Rtl
            } else {
                Direction::Ltr
            };
            let geometry = style.geometry(
                browser.width,
                f64::from(browser.checked),
                f64::from(active),
                direction,
            );
            near(
                geometry.indicator_left,
                browser.handle_left,
                0.016,
                &context,
            );
            near(geometry.indicator_top, browser.handle_top, 0.001, &context);
            near(
                geometry.indicator_size,
                pixels(&browser.handle_size),
                0.001,
                &context,
            );
            near(geometry.handle_top, browser.handle_top, 0.001, &context);
            near(
                geometry.handle_height,
                pixels(&browser.handle_height),
                0.001,
                &context,
            );
            near(
                geometry.handle_width,
                pixels(&browser.handle_paint_width),
                0.016,
                &context,
            );
            let physical_left_inset = if direction == Direction::Rtl {
                pixels(&browser.handle_inset_end)
            } else {
                pixels(&browser.handle_inset_start)
            };
            near(
                geometry.handle_left,
                browser.handle_left + physical_left_inset,
                0.016,
                &context,
            );
            let (start, end) = if browser.checked {
                (style.inner_min_margin, style.inner_max_margin)
            } else {
                (style.inner_max_margin, style.inner_min_margin)
            };
            near(
                start,
                pixels(&browser.content_padding_start),
                0.001,
                &context,
            );
            near(end, pixels(&browser.content_padding_end), 0.001, &context);
            let natural_width = browser
                .checked_natural_width
                .max(browser.unchecked_natural_width);
            near(
                style.track_min_width.max(start + end + natural_width),
                browser.width,
                0.016,
                &context,
            );
            near(
                geometry.content_width,
                browser.checked_content_width,
                0.016,
                &context,
            );
            near(
                geometry.content_width,
                browser.unchecked_content_width,
                0.016,
                &context,
            );
            near(
                geometry.checked_content_left,
                browser.checked_content_left,
                0.016,
                &context,
            );
            near(
                geometry.unchecked_content_left,
                browser.unchecked_content_left,
                0.016,
                &context,
            );
            if let Some(icon_size) = browser.loading_icon_size {
                near(style.loading_icon_size, pixels(&icon_size), 0.001, &context);
                assert_eq!(
                    style.loading_icon_color,
                    color(browser.loading_icon_color.as_deref().unwrap()),
                    "{context}"
                );
            }
            if case.state == "focus" && !browser.disabled {
                assert_eq!(
                    style.focus_color,
                    color(&browser.outline_color),
                    "{context}"
                );
                near(
                    style.focus_width,
                    pixels(&browser.outline_width),
                    0.001,
                    &context,
                );
                near(pixels(&browser.outline_offset), 1., 0.001, &context);
                assert_eq!(browser.outline_style, "solid", "{context}");
            }
            assert!(matches!(
                browser.content.as_str(),
                "empty" | "text" | "asymmetric" | "icon"
            ));
            count += 1;
        }
    }
    assert_eq!(count, 1664);
}
