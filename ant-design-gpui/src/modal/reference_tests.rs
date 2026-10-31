// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
use super::*;
use crate::theme::{AlgorithmChain, Color, ThemeConfig};
use serde_json::Value;

fn lengths(value: &Value) -> Vec<f64> {
    value
        .as_str()
        .unwrap()
        .split_whitespace()
        .map(|s| s.trim_end_matches("px").parse().unwrap())
        .collect()
}
fn near(a: f64, b: f64, case: &str, part: &str) {
    assert!(
        (a - b).abs() <= 1.,
        "{case} {part}: native {a}, browser {b}"
    );
}
fn color(a: Color, value: &Value, case: &str) {
    let css = value.as_str().unwrap();
    let channels = css
        .split_once('(')
        .unwrap()
        .1
        .trim_end_matches(')')
        .split(',')
        .map(|s| s.trim().parse::<f64>().unwrap())
        .collect::<Vec<_>>();
    for (a, b) in [a.r as f64, a.g as f64, a.b as f64, a.a * 255.]
        .into_iter()
        .zip([
            channels[0],
            channels[1],
            channels[2],
            channels.get(3).copied().unwrap_or(1.) * 255.,
        ])
    {
        near(a, b, case, "color");
    }
}
#[test]
fn modal_tokens_match_original_browser_component() {
    let reference: Value =
        serde_json::from_str(include_str!("../../fixtures/modal/reference.json")).unwrap();
    assert_eq!(
        reference["provenance"]["revision"],
        "f8f5595ab5e891fff6a2a06c5b3b98af643d008e"
    );
    assert_eq!(
        reference["provenance"]["packages"]["@rc-component/dialog"],
        "1.10.0"
    );
    for case in reference["cases"].as_array().unwrap() {
        let scenario = &case["scenario"];
        let name = scenario["name"].as_str().unwrap();
        let mut config = ThemeConfig {
            algorithms: Some(match scenario["theme"].as_str().unwrap_or("default") {
                "dark" => AlgorithmChain::dark(),
                "compact" => AlgorithmChain::compact(),
                "dark-compact" => AlgorithmChain::dark_compact(),
                _ => AlgorithmChain::default(),
            }),
            ..Default::default()
        };
        config.tokens.wireframe = Some(scenario["wireframe"] == true);
        if scenario["variant"] == "custom-token" {
            config.components.set::<ModalTheme>(ModalTokenPatch {
                content_bg: Some(Color::rgb(0xfffbe6)),
                title_color: Some(Color::rgb(0xad6800)),
                title_font_size: Some(20.),
                header_margin_bottom: Some(16.),
                footer_margin_top: Some(20.),
                ..Default::default()
            });
        }
        let theme = config.resolve();
        let t = theme.component::<ModalTheme>();
        let g = &theme.tokens;
        let elements = case["metrics"]["elements"].as_array().unwrap();
        let node = |selector: &str| elements.iter().find(|e| e["selector"] == selector);
        let container = node(".ant-modal-container").unwrap();
        color(t.content_bg, &container["background"], name);
        let padding = lengths(&container["padding"]);
        near(
            t.content_padding.vertical,
            padding[0],
            name,
            "vertical padding",
        );
        near(
            t.content_padding.horizontal,
            *padding.get(1).unwrap_or(&padding[0]),
            name,
            "horizontal padding",
        );
        near(
            g.border_radius_lg,
            lengths(&container["radius"])[0],
            name,
            "corner radius",
        );
        let title = node(".ant-modal-title").or_else(|| node(".ant-modal-confirm-title"));
        if let Some(title) = title {
            near(
                t.title_font_size,
                lengths(&title["fontSize"])[0],
                name,
                "title size",
            );
            near(
                t.title_font_size * t.title_line_height,
                lengths(&title["lineHeight"])[0],
                name,
                "title line height",
            );
            color(t.title_color, &title["color"], name);
        }
        if let Some(close) = node(".ant-modal-close") {
            near(
                g.control_height,
                close["w"].as_f64().unwrap(),
                name,
                "close width",
            );
            near(
                g.control_height,
                close["h"].as_f64().unwrap(),
                name,
                "close height",
            );
        }
        if let Some(header) = node(".ant-modal-header") {
            let margin = lengths(&header["margin"]);
            near(
                t.header_margin_bottom,
                *margin.get(2).unwrap_or(&margin[0]),
                name,
                "header gap",
            );
        }
        color(
            g.color_bg_mask,
            &node(".ant-modal-mask").unwrap()["background"],
            name,
        );
    }
}
