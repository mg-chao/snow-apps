// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Browser-computed CSS is independent of native visual state resolution.

use gpui::{Bounds, point, px, size};
use serde::Deserialize;

use super::{CheckboxStyle, paint::checkmark_points};
use crate::theme::{AlgorithmChain, Color, ThemeConfig};

#[derive(Deserialize)]
struct Reference {
    cases: Vec<ReferenceCase>,
}

#[derive(Deserialize)]
struct ReferenceCase {
    theme: String,
    state: String,
    checkboxes: Vec<BrowserCheckbox>,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct BrowserCheckbox {
    index: usize,
    checked: bool,
    indeterminate: bool,
    disabled: bool,
    direction: String,
    mode: String,
    label: String,
    width: f64,
    height: f64,
    background: String,
    border_color: String,
    border_width: String,
    border_style: String,
    radius: String,
    label_color: String,
    font_size: String,
    line_height: String,
    font_weight: String,
    label_padding_start: Option<String>,
    label_padding_end: Option<String>,
    wrapper_height: f64,
    label_height: Option<f64>,
    icon_offset_top: f64,
    icon_direction: String,
    mark_width: String,
    mark_height: String,
    mark_top: String,
    mark_left: String,
    mark_border_width: String,
    mark_border_color: String,
    mark_background: String,
    mark_opacity: String,
    mark_transform: String,
    mark_transform_origin: String,
    outline_color: String,
    outline_width: String,
    outline_offset: String,
    outline_style: String,
    group_gap: String,
}

fn css_pixels(value: &str) -> f64 {
    value
        .strip_suffix("px")
        .unwrap_or(value)
        .parse()
        .unwrap_or_else(|_| panic!("expected browser length: {value}"))
}

fn css_color(value: &str) -> gpui::Hsla {
    let channels: Vec<f64> = value
        .trim_start_matches("rgba(")
        .trim_start_matches("rgb(")
        .trim_end_matches(')')
        .split(',')
        .map(|part| part.trim().parse().expect("browser RGB channel"))
        .collect();
    assert!(matches!(channels.len(), 3 | 4));
    Color {
        r: channels[0] as u8,
        g: channels[1] as u8,
        b: channels[2] as u8,
        a: channels.get(3).copied().unwrap_or(1.),
    }
    .to_hsla()
}

fn near(native: f64, browser: f64, tolerance: f64, context: &str) {
    assert!(
        (native - browser).abs() < tolerance,
        "{context}: native={native}, browser={browser}"
    );
}

fn compare_tick(browser: &BrowserCheckbox, style: CheckboxStyle, context: &str) {
    let width = css_pixels(&browser.mark_width);
    let height = css_pixels(&browser.mark_height);
    near(style.size * 5. / 14., width, 0.016, context);
    near(style.size * 8. / 14., height, 0.016, context);
    near(
        style.mark_width,
        css_pixels(&browser.mark_border_width),
        0.001,
        context,
    );
    if !browser.checked {
        return;
    }
    let matrix: Vec<f64> = browser
        .mark_transform
        .strip_prefix("matrix(")
        .expect("checked mark has a CSS transform")
        .trim_end_matches(')')
        .split(',')
        .map(|part| part.trim().parse().expect("matrix component"))
        .collect();
    assert_eq!(matrix.len(), 6);
    let origin: Vec<f64> = browser
        .mark_transform_origin
        .split_whitespace()
        .map(css_pixels)
        .collect();
    assert_eq!(origin.len(), 2);
    let native = checkmark_points(
        Bounds::new(
            point(px(0.), px(0.)),
            size(px(style.size as f32), px(style.size as f32)),
        ),
        px(style.mark_width as f32),
        1.,
    )
    .expect("native tick geometry");
    let stroke = css_pixels(&browser.mark_border_width);
    let left = css_pixels(&browser.mark_left) + css_pixels(&browser.border_width);
    let top = css_pixels(&browser.mark_top) + css_pixels(&browser.border_width);
    let polygon = [
        (width - stroke, 0.),
        (width, 0.),
        (width, height),
        (0., height),
        (0., height - stroke),
        (width - stroke, height - stroke),
    ];
    for (native, (x, y)) in native.into_iter().zip(polygon) {
        // Evaluate the recorded browser transform, including transform-origin,
        // independently of the native shape's mathematical construction.
        let x = x - origin[0];
        let y = y - origin[1];
        let browser_x = left + origin[0] + matrix[0] * x + matrix[2] * y + matrix[4];
        let browser_y = top + origin[1] + matrix[1] * x + matrix[3] * y + matrix[5];
        near(f64::from(f32::from(native.x)), browser_x, 0.03, context);
        near(f64::from(f32::from(native.y)), browser_y, 0.03, context);
    }
}

#[test]
fn checkbox_matches_original_browser_states_geometry_labels_and_focus() {
    let reference: Reference =
        serde_json::from_str(include_str!("../../fixtures/checkbox/reference.json"))
            .expect("valid independent Checkbox browser fixture");
    assert_eq!(reference.cases.len(), 12);
    let mut compared = 0;
    for case in reference.cases {
        let algorithms = match case.theme.as_str() {
            "default" => AlgorithmChain::default(),
            "dark" => AlgorithmChain::dark(),
            "compact" => AlgorithmChain::compact(),
            "dark-compact" => AlgorithmChain::dark_compact(),
            theme => panic!("unexpected source theme: {theme}"),
        };
        let theme = ThemeConfig {
            algorithms: Some(algorithms),
            ..Default::default()
        }
        .resolve();
        assert_eq!(case.checkboxes.len(), 48);
        for browser in case.checkboxes {
            let context = format!("{} {} #{}", case.theme, case.state, browser.index);
            let style = CheckboxStyle::resolve(
                &theme,
                browser.checked,
                browser.indeterminate,
                browser.disabled,
            );
            let colors = if case.state == "hover" {
                style.hover
            } else {
                style.rest
            };
            assert_eq!(
                colors.background,
                css_color(&browser.background),
                "{context}"
            );
            assert_eq!(colors.border, css_color(&browser.border_color), "{context}");
            assert_eq!(
                browser.icon_direction, "ltr",
                "the tick never mirrors: {context}"
            );
            assert!(matches!(browser.direction.as_str(), "ltr" | "rtl"));
            near(style.size, browser.width, 0.001, &context);
            near(style.size, browser.height, 0.001, &context);
            near(
                style.line_width,
                css_pixels(&browser.border_width),
                0.001,
                &context,
            );
            near(style.radius, css_pixels(&browser.radius), 0.001, &context);
            near(
                style.font_size,
                css_pixels(&browser.font_size),
                0.001,
                &context,
            );
            near(
                style.line_height,
                css_pixels(&browser.line_height),
                0.001,
                &context,
            );
            near(
                style.group_gap,
                css_pixels(&browser.group_gap),
                0.001,
                &context,
            );
            assert_eq!(browser.border_style, "solid");
            assert_eq!(browser.font_weight, "400");
            near(
                (browser.wrapper_height - style.size) / 2.,
                browser.icon_offset_top,
                0.001,
                &context,
            );
            if browser.mode == "empty" {
                assert!(browser.label.is_empty());
                assert!(browser.label_height.is_none());
                near(style.line_height, browser.wrapper_height, 0.001, &context);
            } else {
                assert!(!browser.label.is_empty());
                assert_eq!(colors.label, css_color(&browser.label_color), "{context}");
                near(
                    style.label_padding,
                    css_pixels(browser.label_padding_start.as_ref().unwrap()),
                    0.001,
                    &context,
                );
                near(
                    style.label_padding,
                    css_pixels(browser.label_padding_end.as_ref().unwrap()),
                    0.001,
                    &context,
                );
                near(
                    browser.label_height.unwrap(),
                    browser.wrapper_height,
                    0.001,
                    &context,
                );
            }
            if browser.indeterminate {
                near(
                    style.indeterminate_size,
                    css_pixels(&browser.mark_width),
                    0.001,
                    &context,
                );
                near(
                    style.indeterminate_size,
                    css_pixels(&browser.mark_height),
                    0.001,
                    &context,
                );
                assert_eq!(
                    colors.mark,
                    css_color(&browser.mark_background),
                    "{context}"
                );
                assert_eq!(browser.mark_border_width, "0px");
            } else {
                assert_eq!(
                    colors.mark,
                    css_color(&browser.mark_border_color),
                    "{context}"
                );
                compare_tick(&browser, style, &context);
            }
            assert_eq!(
                browser.mark_opacity,
                if browser.checked || browser.indeterminate {
                    "1"
                } else {
                    "0"
                }
            );
            if case.state == "focus" && !browser.disabled {
                assert_eq!(browser.outline_style, "solid");
                assert_eq!(
                    style.focus_color,
                    css_color(&browser.outline_color),
                    "{context}"
                );
                near(
                    style.focus_width,
                    css_pixels(&browser.outline_width),
                    0.001,
                    &context,
                );
                near(1., css_pixels(&browser.outline_offset), 0.001, &context);
            } else {
                assert_eq!(browser.outline_style, "none");
            }
            compared += 1;
        }
    }
    assert_eq!(compared, 576);
}
