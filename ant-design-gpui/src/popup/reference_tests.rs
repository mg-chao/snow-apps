// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::{PopupPlacement, shape::ArrowShape};
use crate::{
    popconfirm::PopconfirmStyle,
    popover::PopoverStyle,
    theme::{AlgorithmChain, PresetColor, ThemeConfig, TokenPatch},
    tooltip::TooltipStyle,
};
use serde_json::Value;

fn number(value: &Value, property: &str) -> f64 {
    value[property]
        .as_str()
        .unwrap()
        .trim_end_matches("px")
        .parse()
        .unwrap()
}
fn near(actual: f64, expected: f64, case: &str, property: &str) {
    assert!(
        (actual - expected).abs() < 0.002,
        "{case}: {property}: native={actual}, browser={expected}"
    );
}
fn color(actual: gpui::Hsla, node: &Value, property: &str, case: &str) {
    let css = node[property].as_str().unwrap();
    let channels: Vec<f32> = css
        .split_once('(')
        .unwrap()
        .1
        .trim_end_matches(')')
        .split(',')
        .map(|channel| channel.trim().parse().unwrap())
        .collect();
    let actual = actual.to_rgb();
    let expected = [
        channels[0] / 255.,
        channels[1] / 255.,
        channels[2] / 255.,
        channels.get(3).copied().unwrap_or(1.),
    ];
    for (channel, (actual, expected)) in [actual.r, actual.g, actual.b, actual.a]
        .into_iter()
        .zip(expected)
        .enumerate()
    {
        assert!(
            (actual - expected).abs() < 0.002,
            "{case}: {property} channel {channel}: native={actual}, browser={expected}"
        );
    }
}
fn path_numbers(path: &str) -> Vec<f32> {
    path.split(|c: char| !(c.is_ascii_digit() || c == '.' || c == '-' || c == 'e'))
        .filter_map(|word| word.parse().ok())
        .collect()
}

#[test]
fn popup_styles_match_independent_upstream_browser_metrics() {
    let fixture: Value =
        serde_json::from_str(include_str!("../../fixtures/popup/reference.json")).unwrap();
    assert_eq!(
        fixture["provenance"]["upstream"]["revision"],
        "f8f5595ab5e891fff6a2a06c5b3b98af643d008e"
    );
    let placements = [
        "top",
        "topLeft",
        "topRight",
        "bottom",
        "bottomLeft",
        "bottomRight",
        "left",
        "leftTop",
        "leftBottom",
        "right",
        "rightTop",
        "rightBottom",
    ];
    let mut count = 0;
    for group in fixture["cases"].as_array().unwrap() {
        let name = group["theme"].as_str().unwrap();
        let algorithm = match name {
            "dark" => AlgorithmChain::dark(),
            "compact" => AlgorithmChain::compact(),
            "dark-compact" => AlgorithmChain::dark_compact(),
            _ => AlgorithmChain::default(),
        };
        let theme = ThemeConfig {
            algorithms: Some(algorithm),
            tokens: TokenPatch {
                wireframe: Some(group["wireframe"].as_bool().unwrap()),
                ..Default::default()
            },
            ..Default::default()
        }
        .resolve();
        for case in group["popups"].as_array().unwrap() {
            count += 1;
            let label = format!(
                "{name}/wireframe={}/{}-{}-{}-{}",
                group["wireframe"],
                case["component"],
                case["placement"],
                case["direction"],
                case["variant"]
            );
            let placement = PopupPlacement::ALL[placements
                .iter()
                .position(|name| *name == case["placement"])
                .unwrap()];
            let preset = (case["variant"] == "preset").then_some(PresetColor::Blue.into());
            let container = &case["container"];
            let root = &case["root"];
            if case["component"] == "tooltip" {
                let style = TooltipStyle::resolve(&theme, preset, placement);
                for (native, prop, node) in [
                    (style.max_width, "maxWidth", root),
                    (style.min_height, "minHeight", container),
                    (style.radius, "borderRadius", container),
                    (style.padding_inline, "paddingLeft", container),
                    (style.padding_block, "paddingTop", container),
                    (style.font_size, "fontSize", container),
                    (style.font_size * style.line_height, "lineHeight", container),
                ] {
                    near(native, number(node, prop), &label, prop);
                }
                near(
                    style.min_width,
                    number(container, "minWidth").max(number(root, "minWidth")),
                    &label,
                    "minWidth",
                );
                color(style.background, container, "backgroundColor", &label);
                color(style.foreground, container, "color", &label);
            } else {
                let style = PopoverStyle::resolve(&theme, preset);
                for (native, prop) in [
                    (style.radius, "borderRadius"),
                    (style.padding, "paddingTop"),
                    (style.font_size, "fontSize"),
                    (style.font_size * style.line_height, "lineHeight"),
                ] {
                    near(native, number(container, prop), &label, prop);
                }
                color(style.background, container, "backgroundColor", &label);
                if case["component"] == "popover" {
                    let title = &case["title"];
                    for (native, prop) in [
                        (style.title_min_width, "minWidth"),
                        (style.title_margin_bottom, "marginBottom"),
                        (style.title_padding_top, "paddingTop"),
                        (style.title_padding_bottom, "paddingBottom"),
                        (style.title_padding_inline, "paddingLeft"),
                        (style.title_border_width, "borderBottomWidth"),
                        (style.title_weight, "fontWeight"),
                    ] {
                        near(native, number(title, prop), &label, prop);
                    }
                    color(style.title_color, title, "color", &label);
                    color(style.foreground, &case["content"], "color", &label);
                    near(
                        style.content_padding_block,
                        number(&case["content"], "paddingTop"),
                        &label,
                        "content block padding",
                    );
                    near(
                        style.content_padding_inline,
                        number(&case["content"], "paddingLeft"),
                        &label,
                        "content inline padding",
                    );
                } else {
                    let style = PopconfirmStyle::resolve(&theme, case["variant"] != "title-only");
                    near(
                        style.title_weight,
                        number(&case["title"], "fontWeight"),
                        &label,
                        "title weight",
                    );
                    near(
                        style.icon_gap,
                        number(&case["icon"], "marginInlineEnd"),
                        &label,
                        "icon gap",
                    );
                    near(
                        style.message_margin_bottom,
                        number(&case["message"], "marginBottom"),
                        &label,
                        "message gap",
                    );
                    near(
                        style.button_margin_start,
                        number(&case["button"], "marginInlineStart"),
                        &label,
                        "button margin",
                    );
                    color(style.icon_color, &case["icon"], "color", &label);
                    color(style.title_color, &case["title"], "color", &label);
                    if !case["content"].is_null() {
                        near(
                            style.description_margin_top,
                            number(&case["content"], "marginTop"),
                            &label,
                            "description gap",
                        );
                    }
                }
            }
            let g = &theme.tokens;
            let shape = ArrowShape::new(
                gpui::px(g.size_popup_arrow as f32),
                gpui::px(g.border_radius_xs as f32),
                gpui::px(g.border_radius_outer.min(4.) as f32),
            );
            let actual = path_numbers(&shape.svg_path());
            let expected = path_numbers(case["arrowShape"]["clipPath"].as_str().unwrap());
            assert_eq!(actual.len(), expected.len(), "{label}");
            for (actual, expected) in actual.iter().zip(expected) {
                near(
                    f64::from(*actual),
                    f64::from(expected),
                    &label,
                    "rounded arrow",
                );
            }
        }
    }
    assert_eq!(count, 600);
}
