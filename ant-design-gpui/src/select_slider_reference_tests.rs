// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
//! Independently captured styles from the executable Ant Design components.
use crate::{
    Size, ValidationStatus, Variant,
    input::InputInteraction,
    select::SelectTheme,
    slider::SliderTheme,
    theme::{AlgorithmChain, ThemeConfig},
};
use serde_json::Value;

fn theme(case: &Value) -> crate::theme::ResolvedTheme {
    ThemeConfig {
        algorithms: Some(match case["scenario"]["theme"].as_str().unwrap() {
            "dark" => AlgorithmChain::dark(),
            "compact" => AlgorithmChain::compact(),
            "dark-compact" => AlgorithmChain::dark_compact(),
            _ => AlgorithmChain::default(),
        }),
        ..Default::default()
    }
    .resolve()
}
fn near(actual: f64, expected: f64, case: &str, property: &str) {
    assert!(
        (actual - expected).abs() < 0.02,
        "{case}: {property}: native={actual}, browser={expected}"
    );
}
fn px(value: &Value) -> f64 {
    value
        .as_str()
        .unwrap()
        .trim_end_matches("px")
        .parse()
        .unwrap()
}
fn color(actual: gpui::Hsla, value: &Value, case: &str, property: &str) {
    let css = value.as_str().unwrap();
    let channels: Vec<f32> = css
        .split_once('(')
        .unwrap()
        .1
        .trim_end_matches(')')
        .split(',')
        .map(|s| s.trim().parse().unwrap())
        .collect();
    let rgb = actual.to_rgb();
    for (a, e) in [rgb.r, rgb.g, rgb.b, rgb.a].into_iter().zip([
        channels[0] / 255.,
        channels[1] / 255.,
        channels[2] / 255.,
        channels.get(3).copied().unwrap_or(1.),
    ]) {
        assert!(
            (a - e).abs() < 0.002,
            "{case}: {property}: native={actual:?}, browser={css}"
        );
    }
}
fn elements<'a>(case: &'a Value, selector: &str) -> Vec<&'a Value> {
    case["metrics"]["elements"]
        .as_array()
        .unwrap()
        .iter()
        .filter(|e| e["selector"] == selector)
        .collect()
}
#[test]
fn select_tokens_and_selector_geometry_match_executable_browser_reference() {
    let data: Value =
        serde_json::from_str(include_str!("../fixtures/select/visual-reference.json")).unwrap();
    assert_eq!(
        data["provenance"]["revision"],
        "f8f5595ab5e891fff6a2a06c5b3b98af643d008e"
    );
    for case in data["cases"].as_array().unwrap() {
        let theme = theme(case);
        let tokens = theme.component::<SelectTheme>();
        let name = case["scenario"]["name"].as_str().unwrap();
        let multiple = case["scenario"]["kind"] == "multiple";
        let surface = tokens
            .surface(
                Size::Medium,
                Variant::Outlined,
                ValidationStatus::None,
                multiple,
                InputInteraction {
                    disabled: case["scenario"]["kind"] == "disabled",
                    hovered: false,
                    focused: false,
                    keyboard_focus: false,
                },
            )
            .at_scale(case["metrics"]["scale"].as_f64().unwrap() as f32);
        let root = elements(case, ".ant-select")[0];
        near(surface.font_size, px(&root["fontSize"]), name, "font size");
        near(
            surface.line_height,
            px(&root["lineHeight"]),
            name,
            "line height",
        );
        near(
            surface.border_width,
            px(&root["borderWidth"]),
            name,
            "physical border",
        );
        near(
            tokens.height(Size::Medium) - 2. * (tokens.globals().line_width - surface.border_width),
            root["h"].as_f64().unwrap(),
            name,
            "selector height",
        );
        near(surface.radius, px(&root["radius"]), name, "radius");
        color(surface.background, &root["background"], name, "background");
        color(surface.border, &root["border"], name, "border");
        color(surface.text, &root["color"], name, "text");
        for option in elements(case, ".ant-select-item-option") {
            near(
                tokens.option_font_size,
                px(&option["fontSize"]),
                name,
                "option font",
            );
            near(
                tokens.option_font_size * tokens.option_line_height,
                px(&option["lineHeight"]),
                name,
                "option line height",
            );
            near(
                tokens.option_height,
                option["h"].as_f64().unwrap(),
                name,
                "option height",
            );
        }
    }
}
#[test]
fn slider_rail_and_handle_tokens_match_executable_browser_reference() {
    let data: Value =
        serde_json::from_str(include_str!("../fixtures/slider/visual-reference.json")).unwrap();
    for case in data["cases"].as_array().unwrap() {
        let theme = theme(case);
        let tokens = theme.component::<SliderTheme>();
        let name = case["scenario"]["name"].as_str().unwrap();
        let vertical = case["scenario"]["kind"] == "vertical";
        let rail = elements(case, ".ant-slider-rail")[0];
        near(
            tokens.rail_size,
            rail[if vertical { "w" } else { "h" }].as_f64().unwrap(),
            name,
            "rail thickness",
        );
        color(
            tokens.rail_bg.into(),
            &rail["background"],
            name,
            "rail background",
        );
        for handle in elements(case, ".ant-slider-handle") {
            near(
                tokens.handle_size,
                handle["w"].as_f64().unwrap(),
                name,
                "handle width",
            );
            near(
                tokens.handle_size,
                handle["h"].as_f64().unwrap(),
                name,
                "handle height",
            );
            near(
                tokens.handle_size,
                px(&handle["after"]["w"]),
                name,
                "handle face width",
            );
            color(
                tokens.globals().color_bg_elevated.into(),
                &handle["after"]["background"],
                name,
                "handle fill",
            );
        }
    }
}
