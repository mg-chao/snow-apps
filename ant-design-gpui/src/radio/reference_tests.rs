// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Independent browser-computed values from the original Radio style generators.

use gpui::Hsla;
use serde::Deserialize;

use super::{
    RadioButtonStyle, RadioButtonVisualStyle, RadioStateStyle, RadioStyle, RadioTheme,
    RadioTokenPatch,
};
use crate::{
    Size,
    theme::{AlgorithmChain, Color, ComponentThemeConfig, ResolvedTheme, ThemeConfig, TokenPatch},
};

#[derive(Deserialize)]
struct Reference {
    cases: Vec<ReferenceCase>,
}

#[derive(Deserialize)]
struct ReferenceCase {
    theme: String,
    state: String,
    controls: Vec<BrowserControl>,
    groups: Vec<BrowserGroup>,
}

#[derive(Default, Deserialize)]
#[serde(default, rename_all = "camelCase")]
struct Options {
    index: usize,
    direction: String,
    variant: String,
    checked: bool,
    disabled: bool,
    mode: String,
    button: bool,
    size: String,
    button_style: String,
    vertical: bool,
    block: bool,
}

#[derive(Deserialize)]
struct BrowserBounds {
    x: f64,
    y: f64,
    width: f64,
    height: f64,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct BrowserControl {
    options: Option<Options>,
    bounds: BrowserBounds,
    icon_bounds: BrowserBounds,
    label_bounds: Option<BrowserBounds>,
    background: String,
    border_color: String,
    border_width: f64,
    border_style: String,
    corners: [f64; 4],
    color: String,
    font_size: f64,
    line_height: f64,
    font_weight: String,
    padding_start: f64,
    padding_end: f64,
    margin_end: f64,
    margin_bottom: f64,
    label_padding_start: Option<f64>,
    label_padding_end: Option<f64>,
    dot_width: f64,
    dot_height: f64,
    dot_color: String,
    dot_opacity: f64,
    dot_transform: String,
    outline_color: String,
    outline_width: f64,
    outline_offset: f64,
    outline_style: String,
    z_index: String,
    cursor: String,
    direction: String,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct BrowserGroup {
    options: Options,
    bounds: BrowserBounds,
    row_gap: f64,
    children: Vec<BrowserControl>,
}

fn css_color(value: &str) -> Hsla {
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

fn theme(name: &str, variant: &str) -> ResolvedTheme {
    let algorithms = match name {
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
    match variant {
        "default" => {}
        "wireframe" => configuration.components.set_theme::<RadioTheme>(
            ComponentThemeConfig::default().with_tokens(TokenPatch {
                wireframe: Some(true),
                ..Default::default()
            }),
        ),
        "custom" => configuration.components.set::<RadioTheme>(RadioTokenPatch {
            radio_size: Some(20.),
            dot_size: Some(10.),
            dot_color_disabled: Some(Color::rgb(0x808080)),
            button_bg: Some(Color::rgb(0xf6ffed)),
            button_checked_bg: Some(Color::rgb(0xd9f7be)),
            button_color: Some(Color::rgb(0xfaad14)),
            button_padding_inline: Some(20.),
            button_checked_bg_disabled: Some(Color::rgb(0xfffbe6)),
            button_checked_color_disabled: Some(Color::rgb(0xffe58f)),
            button_solid_checked_color: Some(Color::rgb(0xffa39e)),
            wrapper_margin_inline_end: Some(20.),
            ..Default::default()
        }),
        variant => panic!("unknown browser variant {variant}"),
    }
    configuration.resolve()
}

fn size(value: &str) -> Size {
    match value {
        "" | "medium" => Size::Medium,
        "small" => Size::Small,
        "large" => Size::Large,
        value => panic!("unknown browser size {value}"),
    }
}

fn appearance(value: &str) -> RadioButtonStyle {
    match value {
        "" | "outline" => RadioButtonStyle::Outline,
        "solid" => RadioButtonStyle::Solid,
        value => panic!("unknown browser button style {value}"),
    }
}

fn compare_colors(native: RadioStateStyle, browser: &BrowserControl, context: &str) {
    assert_eq!(
        native.background,
        css_color(&browser.background),
        "{context}"
    );
    assert_eq!(native.border, css_color(&browser.border_color), "{context}");
    assert_eq!(native.label, css_color(&browser.color), "{context}");
    assert_eq!(browser.border_style, "solid", "{context}");
    assert_eq!(browser.font_weight, "400", "{context}");
}

fn compare_focus(
    browser: &BrowserControl,
    color: Hsla,
    width: f64,
    state: &str,
    disabled: bool,
    context: &str,
) {
    if state == "focus" && !disabled {
        assert_eq!(browser.outline_style, "solid", "{context}");
        assert_eq!(color, css_color(&browser.outline_color), "{context}");
        near(width, browser.outline_width, 0.001, context);
        near(1., browser.outline_offset, 0.001, context);
    } else {
        assert_eq!(browser.outline_style, "none", "{context}");
    }
    assert_eq!(
        browser.cursor,
        if disabled { "not-allowed" } else { "pointer" },
        "{context}"
    );
}

fn compare_circle(
    browser: &BrowserControl,
    style: RadioStyle,
    state: &str,
    options: &Options,
    context: &str,
) {
    let colors = if matches!(state, "hover" | "active") {
        style.hover
    } else {
        style.rest
    };
    compare_colors(colors, browser, context);
    assert_eq!(colors.mark, css_color(&browser.dot_color), "{context}");
    assert_eq!(browser.corners, [50.; 4], "circular radius: {context}");
    for (native, measured) in [
        (style.size, browser.icon_bounds.width),
        (style.size, browser.icon_bounds.height),
        (style.dot_size, browser.dot_width),
        (style.dot_size, browser.dot_height),
        (style.line_width, browser.border_width),
        (style.font_size, browser.font_size),
        (style.line_height, browser.line_height),
        (
            (browser.bounds.height - style.size) / 2.,
            browser.icon_bounds.y,
        ),
    ] {
        near(native, measured, 0.001, context);
    }
    near(
        if options.checked { 1. } else { 0. },
        browser.dot_opacity,
        0.001,
        context,
    );
    let matrix: Vec<f64> = browser
        .dot_transform
        .strip_prefix("matrix(")
        .expect("circle dot has a transform")
        .trim_end_matches(')')
        .split(',')
        .map(|part| part.trim().parse().expect("browser transform coefficient"))
        .collect();
    assert_eq!(matrix.len(), 6);
    let scale = if options.checked { 1. } else { 0. };
    for (native, browser) in [
        scale,
        0.,
        0.,
        scale,
        -style.dot_size / 2.,
        -style.dot_size / 2.,
    ]
    .into_iter()
    .zip(matrix)
    {
        near(native, browser, 0.001, context);
    }
    if options.mode == "empty" {
        assert!(browser.label_bounds.is_none());
        near(style.line_height, browser.bounds.height, 0.001, context);
    } else {
        let label = browser.label_bounds.as_ref().expect("Radio label");
        near(
            style.label_padding,
            browser.label_padding_start.unwrap(),
            0.001,
            context,
        );
        near(
            style.label_padding,
            browser.label_padding_end.unwrap(),
            0.001,
            context,
        );
        near(browser.bounds.height, label.height, 0.001, context);
        if !options.block && !options.vertical {
            let (icon_x, label_x) = if options.direction == "rtl" {
                (browser.bounds.width - style.size, 0.)
            } else {
                (0., style.size)
            };
            near(icon_x, browser.icon_bounds.x, 0.001, context);
            near(label_x, label.x, 0.001, context);
        }
    }
    compare_focus(
        browser,
        style.focus_color,
        style.focus_width,
        state,
        options.disabled,
        context,
    );
}

fn compare_button(
    browser: &BrowserControl,
    style: RadioButtonVisualStyle,
    state: &str,
    disabled: bool,
    context: &str,
) {
    let colors = match state {
        "hover" => style.hover,
        "active" => style.active,
        _ => style.rest,
    };
    compare_colors(colors, browser, context);
    for (native, measured) in [
        (style.height, browser.bounds.height),
        (style.font_size, browser.font_size),
        (style.line_height, browser.line_height),
        (style.padding_inline, browser.padding_start),
        (style.padding_inline, browser.padding_end),
        (style.line_width, browser.border_width),
    ] {
        near(native, measured, 0.001, context);
    }
    compare_focus(
        browser,
        style.focus_color,
        style.focus_width,
        state,
        disabled,
        context,
    );
}

#[test]
fn radio_matches_original_browser_states_geometry_tokens_labels_and_focus() {
    let reference: Reference =
        serde_json::from_str(include_str!("../../fixtures/radio/reference.json"))
            .expect("independent browser-computed Radio fixture");
    assert_eq!(reference.cases.len(), 16);
    let mut compared = 0;
    for case in reference.cases {
        let themes = [
            theme(&case.theme, "default"),
            theme(&case.theme, "wireframe"),
            theme(&case.theme, "custom"),
        ];
        assert_eq!(case.controls.len(), 104);
        assert_eq!(case.groups.len(), 32);
        for browser in &case.controls {
            let options = browser.options.as_ref().expect("standalone options");
            let context = format!("{} {} #{}", case.theme, case.state, options.index);
            let theme = &themes[match options.variant.as_str() {
                "default" => 0,
                "wireframe" => 1,
                "custom" => 2,
                variant => panic!("unknown variant {variant}"),
            }];
            assert_eq!(browser.direction, options.direction, "{context}");
            if options.button {
                let style = RadioButtonVisualStyle::resolve(
                    theme,
                    size(&options.size),
                    appearance(&options.button_style),
                    options.checked,
                    options.disabled,
                );
                compare_button(browser, style, &case.state, options.disabled, &context);
                assert_eq!(browser.corners, [style.radius; 4], "{context}");
            } else {
                compare_circle(
                    browser,
                    RadioStyle::resolve(theme, options.checked, options.disabled),
                    &case.state,
                    options,
                    &context,
                );
            }
            compared += 1;
        }
        for group in &case.groups {
            let options = &group.options;
            let context = format!(
                "{} {} {} {} {} vertical={} block={}",
                case.theme,
                case.state,
                options.direction,
                options.size,
                options.button_style,
                options.vertical,
                options.block
            );
            assert_eq!(group.children.len(), 3);
            let circle = RadioStyle::resolve(&themes[0], false, false);
            near(
                if options.vertical && !options.button {
                    circle.vertical_gap
                } else {
                    0.
                },
                group.row_gap,
                0.001,
                &context,
            );
            for (index, browser) in group.children.iter().enumerate() {
                let checked = index == 1;
                let disabled = index == 2;
                if options.button {
                    let style = RadioButtonVisualStyle::resolve(
                        &themes[0],
                        size(&options.size),
                        appearance(&options.button_style),
                        checked,
                        disabled,
                    );
                    compare_button(browser, style, &case.state, disabled, &context);
                    let radius = style.radius;
                    let expected_corners = if options.vertical {
                        match index {
                            0 => [radius, radius, 0., 0.],
                            1 => [0.; 4],
                            _ => [0., 0., radius, radius],
                        }
                    } else {
                        match (index, options.direction.as_str()) {
                            (0, "ltr") | (2, "rtl") => [radius, 0., 0., radius],
                            (0, "rtl") | (2, "ltr") => [0., radius, radius, 0.],
                            _ => [0.; 4],
                        }
                    };
                    assert_eq!(browser.corners, expected_corners, "{context}");
                    near(
                        if index < 2 && !options.vertical {
                            -style.line_width
                        } else {
                            0.
                        },
                        browser.margin_end,
                        0.001,
                        &context,
                    );
                    near(
                        if index < 2 && options.vertical {
                            -style.line_width
                        } else {
                            0.
                        },
                        browser.margin_bottom,
                        0.001,
                        &context,
                    );
                    assert_eq!(
                        browser.z_index,
                        if checked { "1" } else { "auto" },
                        "{context}"
                    );
                } else {
                    let child_options = Options {
                        checked,
                        disabled,
                        mode: "label".into(),
                        direction: options.direction.clone(),
                        block: options.block,
                        vertical: options.vertical,
                        ..Default::default()
                    };
                    compare_circle(
                        browser,
                        RadioStyle::resolve(&themes[0], checked, disabled),
                        &case.state,
                        &child_options,
                        &context,
                    );
                    near(
                        if index < 2 && !options.vertical {
                            circle.group_gap
                        } else {
                            0.
                        },
                        browser.margin_end,
                        0.001,
                        &context,
                    );
                    if options.block && !options.vertical {
                        near(
                            group.children[0].bounds.width,
                            browser.bounds.width,
                            0.02,
                            &context,
                        );
                    }
                }
                if index > 0 {
                    let previous = &group.children[index - 1].bounds;
                    let current = &browser.bounds;
                    let measured_gap = if options.vertical {
                        current.y - previous.y - previous.height
                    } else if options.direction == "rtl" {
                        previous.x - current.x - current.width
                    } else {
                        current.x - previous.x - previous.width
                    };
                    near(
                        if options.button {
                            -circle.line_width
                        } else if options.vertical {
                            circle.vertical_gap
                        } else {
                            circle.group_gap
                        },
                        measured_gap,
                        0.02,
                        &context,
                    );
                }
                compared += 1;
            }
            if options.block {
                near(360., group.bounds.width, 0.001, &context);
            }
        }
    }
    assert_eq!(compared, 3200);
}
