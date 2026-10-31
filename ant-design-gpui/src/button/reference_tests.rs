// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Browser-computed fixtures execute upstream styles independently of native tokens.

use super::*;
use crate::theme::{AlgorithmChain, ThemeConfig};
use serde::Deserialize;

#[derive(Deserialize)]
struct Reference {
    cases: Vec<ReferenceCase>,
}

#[derive(Deserialize)]
struct ReferenceCase {
    theme: String,
    state: String,
    buttons: Vec<BrowserButton>,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct BrowserButton {
    index: usize,
    id: String,
    label: String,
    classes: String,
    disabled: bool,
    width: f64,
    height: f64,
    background: String,
    color: String,
    border_color: String,
    border_width: String,
    border_style: String,
    radius: String,
    gap: String,
    padding: String,
    font_size: String,
    font_weight: String,
    line_height: String,
    opacity: String,
    shadow: String,
    icon_font_size: Option<String>,
    icon_width: Option<f64>,
    icon_height: Option<f64>,
}

impl BrowserButton {
    fn has(&self, class: &str) -> bool {
        self.classes.split_whitespace().any(|value| value == class)
    }

    fn options(&self) -> ButtonStyleOptions {
        let suffix = |prefix| {
            self.classes
                .split_whitespace()
                .find_map(|class| class.strip_prefix(prefix))
                .expect("source Button contains its appearance classes")
        };
        let variant = match suffix("ant-btn-variant-") {
            "outlined" => ButtonVariant::Outlined,
            "dashed" => ButtonVariant::Dashed,
            "solid" => ButtonVariant::Solid,
            "filled" => ButtonVariant::Filled,
            "text" => ButtonVariant::Text,
            "link" => ButtonVariant::Link,
            variant => panic!("unknown upstream variant: {variant}"),
        };
        let color = match suffix("ant-btn-color-") {
            "default" => ButtonColor::Default,
            "primary" => ButtonColor::Primary,
            "dangerous" => ButtonColor::Danger,
            "link" => ButtonColor::Link,
            name => ButtonColor::Preset(
                PresetColor::ALL
                    .into_iter()
                    .find(|color| color.name() == name)
                    .unwrap_or_else(|| panic!("unknown upstream preset: {name}")),
            ),
        };
        ButtonStyleOptions {
            size: if self.has("ant-btn-sm") {
                Size::Small
            } else if self.has("ant-btn-lg") {
                Size::Large
            } else {
                Size::Medium
            },
            shape: if self.has("ant-btn-circle") {
                ButtonShape::Circle
            } else if self.has("ant-btn-round") {
                ButtonShape::Round
            } else if self.has("ant-btn-square") {
                ButtonShape::Square
            } else {
                ButtonShape::Default
            },
            color,
            variant,
            ghost: self.has("ant-btn-background-ghost"),
            disabled: self.disabled,
            icon_only: self.has("ant-btn-icon-only"),
        }
    }
}

fn css_pixels(value: &str) -> f64 {
    value
        .strip_suffix("px")
        .unwrap_or(value)
        .parse()
        .unwrap_or_else(|_| panic!("expected concrete CSS length: {value}"))
}

fn css_color(value: &str) -> Color {
    let values: Vec<f64> = value
        .trim_start_matches("rgba(")
        .trim_start_matches("rgb(")
        .trim_end_matches(')')
        .split(',')
        .map(|value| value.trim().parse().expect("browser RGB channel"))
        .collect();
    assert!(
        matches!(values.len(), 3 | 4),
        "expected browser RGB: {value}"
    );
    Color {
        r: values[0] as u8,
        g: values[1] as u8,
        b: values[2] as u8,
        a: values.get(3).copied().unwrap_or(1.),
    }
}

fn css_shadows(value: &str) -> Vec<Shadow> {
    if value == "none" {
        return Vec::new();
    }
    let mut depth = 0;
    let mut start = 0;
    let mut parts = Vec::new();
    for (index, character) in value.char_indices() {
        match character {
            '(' => depth += 1,
            ')' => depth -= 1,
            ',' if depth == 0 => {
                parts.push(&value[start..index]);
                start = index + 1;
            }
            _ => {}
        }
    }
    parts.push(&value[start..]);
    parts
        .into_iter()
        .map(|shadow| {
            let color_start = shadow.find("rgb").expect("browser shadow color");
            let color_end = shadow[color_start..]
                .find(')')
                .expect("closed shadow RGB color")
                + color_start
                + 1;
            let lengths = format!("{} {}", &shadow[..color_start], &shadow[color_end..]);
            let lengths: Vec<_> = lengths
                .split_whitespace()
                .filter(|value| *value != "inset")
                .map(css_pixels)
                .collect();
            assert_eq!(lengths.len(), 4, "browser shadow dimensions");
            Shadow {
                x: lengths[0],
                y: lengths[1],
                blur: lengths[2],
                spread: lengths[3],
                color: css_color(&shadow[color_start..color_end]),
                inset: shadow.contains("inset"),
            }
        })
        .collect()
}

fn compare_number(
    failures: &mut Vec<String>,
    context: &str,
    property: &str,
    native: f64,
    browser: f64,
) {
    // Chromium rounds serialized computed line heights to four decimal places.
    if (native - browser).abs() > 0.001 {
        failures.push(format!(
            "{context}: {property}: native {native}, browser {browser}"
        ));
    }
}

fn compare_color(
    failures: &mut Vec<String>,
    context: &str,
    property: &str,
    native: Color,
    browser: Color,
) {
    if native.r != browser.r
        || native.g != browser.g
        || native.b != browser.b
        || (native.a - browser.a).abs() > 0.001
    {
        failures.push(format!(
            "{context}: {property}: native {native:?}, browser {browser:?}"
        ));
    }
}

#[test]
fn button_visual_tokens_match_all_original_browser_computed_styles() {
    let reference: Reference =
        serde_json::from_str(include_str!("../../fixtures/button/reference.json"))
            .expect("independent browser-computed Button fixture");
    assert_eq!(reference.cases.len(), 12);
    let mut failures = Vec::new();
    let mut compared = 0;
    for case in reference.cases {
        let algorithms = match case.theme.as_str() {
            "default" => AlgorithmChain::default(),
            "dark" => AlgorithmChain::dark(),
            "compact" => AlgorithmChain::compact(),
            "dark-compact" => AlgorithmChain::dark_compact(),
            theme => panic!("unknown reference theme: {theme}"),
        };
        let theme = ThemeConfig {
            algorithms: Some(algorithms),
            ..Default::default()
        }
        .resolve();
        assert_eq!(case.buttons.len(), 184);
        for button in case.buttons {
            compared += 1;
            let options = button.options();
            let style = ButtonStyle::resolve(&theme, options);
            let visual = match case.state.as_str() {
                "rest" => style.rest,
                "hover" => style.hover,
                "active" => style.active,
                state => panic!("unknown reference state: {state}"),
            };
            let context = format!(
                "{} / {} / {} {} {:?}",
                case.theme, case.state, button.index, button.id, button.label
            );
            for (property, native, browser) in [
                ("height", style.metrics.height, button.height),
                (
                    "font size",
                    style.metrics.font_size,
                    css_pixels(&button.font_size),
                ),
                (
                    "line height",
                    style.metrics.line_height,
                    css_pixels(&button.line_height),
                ),
                (
                    "font weight",
                    style.metrics.font_weight,
                    css_pixels(&button.font_weight),
                ),
                ("icon gap", style.metrics.icon_gap, css_pixels(&button.gap)),
                (
                    "border width",
                    style.line_width,
                    css_pixels(&button.border_width),
                ),
                (
                    "corner radius",
                    style.metrics.radius,
                    if button.radius == "50%" {
                        button.height / 2.
                    } else {
                        css_pixels(&button.radius)
                    },
                ),
            ] {
                compare_number(&mut failures, &context, property, native, browser);
            }
            let padding: Vec<_> = button.padding.split_whitespace().map(css_pixels).collect();
            let padding_inline = if padding.len() == 1 {
                padding[0]
            } else {
                padding[1]
            };
            compare_number(
                &mut failures,
                &context,
                "horizontal padding",
                style.metrics.padding_inline,
                padding_inline,
            );
            if options.icon_only {
                compare_number(
                    &mut failures,
                    &context,
                    "icon-only width",
                    style.metrics.min_width,
                    button.width,
                );
            }
            if let Some(icon_font_size) = &button.icon_font_size {
                for (property, browser) in [
                    ("icon font size", css_pixels(icon_font_size)),
                    ("icon width", button.icon_width.expect("browser icon width")),
                    (
                        "icon height",
                        button.icon_height.expect("browser icon height"),
                    ),
                ] {
                    compare_number(
                        &mut failures,
                        &context,
                        property,
                        style.metrics.icon_size,
                        browser,
                    );
                }
            } else {
                assert!(button.icon_width.is_none() && button.icon_height.is_none());
            }
            for (property, native, browser) in [
                (
                    "background",
                    visual.background,
                    css_color(&button.background),
                ),
                ("text color", visual.text, css_color(&button.color)),
                (
                    "border color",
                    visual.border,
                    css_color(&button.border_color),
                ),
            ] {
                compare_color(&mut failures, &context, property, native, browser);
            }
            let expected_line_type = match button.border_style.as_str() {
                "solid" => LineType::Solid,
                "dashed" => LineType::Dashed,
                line => panic!("unexpected browser border style: {line}"),
            };
            if style.line_type != expected_line_type {
                failures.push(format!("{context}: border type differs"));
            }
            compare_number(
                &mut failures,
                &context,
                "loading opacity",
                if button.has("ant-btn-loading") {
                    style.loading_opacity
                } else {
                    1.
                },
                button.opacity.parse().expect("browser opacity"),
            );
            let shadows = css_shadows(&button.shadow);
            if shadows.len() != style.shadow.len() {
                failures.push(format!(
                    "{context}: shadow count: native {}, browser {}",
                    style.shadow.len(),
                    shadows.len()
                ));
            }
            for (native, browser) in style.shadow.iter().zip(shadows) {
                for (property, native, browser) in [
                    ("shadow x", native.x, browser.x),
                    ("shadow y", native.y, browser.y),
                    ("shadow blur", native.blur, browser.blur),
                    ("shadow spread", native.spread, browser.spread),
                ] {
                    compare_number(&mut failures, &context, property, native, browser);
                }
                compare_color(
                    &mut failures,
                    &context,
                    "shadow color",
                    native.color,
                    browser.color,
                );
                if native.inset != browser.inset {
                    failures.push(format!("{context}: shadow inset differs"));
                }
            }
        }
    }
    assert_eq!(compared, 2208);
    assert!(
        failures.is_empty(),
        "{} upstream browser mismatches:\n{}",
        failures.len(),
        failures
            .iter()
            .take(60)
            .cloned()
            .collect::<Vec<_>>()
            .join("\n")
    );
}
