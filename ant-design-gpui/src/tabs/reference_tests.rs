// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;
use crate::{
    Size,
    theme::{AlgorithmChain, Color, ComponentThemeConfig, ThemeConfig, TokenPatch},
};
use serde_json::Value;

fn css_color(value: &Value) -> Color {
    let value = value.as_str().unwrap();
    let channels: Vec<f64> = value
        .trim_start_matches("rgba(")
        .trim_start_matches("rgb(")
        .trim_end_matches(')')
        .split(',')
        .map(|value| value.trim().parse().unwrap())
        .collect();
    Color {
        r: channels[0] as u8,
        g: channels[1] as u8,
        b: channels[2] as u8,
        a: channels.get(3).copied().unwrap_or(1.),
    }
}
fn near(context: &str, property: &str, actual: f64, expected: &Value) {
    let expected = expected.as_f64().unwrap();
    assert!(
        (actual - expected).abs() < 0.02,
        "{context} {property}: native {actual}, reference {expected}"
    );
}
#[test]
fn tabs_tokens_and_states_match_independent_browser_measurements() {
    let reference: Value =
        serde_json::from_str(include_str!("../../fixtures/tabs/reference.json")).unwrap();
    let measurement = |id: &Value| &reference["measurements"][id.as_u64().unwrap() as usize];
    let mut compared = 0;
    for case in reference["cases"].as_array().unwrap() {
        let config = ThemeConfig {
            algorithms: Some(match case["theme"].as_str().unwrap() {
                "default" => AlgorithmChain::default(),
                "dark" => AlgorithmChain::dark(),
                "compact" => AlgorithmChain::compact(),
                "dark-compact" => AlgorithmChain::dark_compact(),
                other => panic!("unknown theme {other}"),
            }),
            ..Default::default()
        };
        for sample in case["tabs"].as_array().unwrap() {
            let mut config = config.clone();
            let options = &sample["options"];
            if options["custom"] == true {
                config.components.set_theme(
                    ComponentThemeConfig::<TabsTheme>::new(TabsTokenPatch {
                        card_height: Some(52.),
                        horizontal_item_gutter: Some(18.),
                        ink_bar_color: Some(Color::rgb(0xeb2f96)),
                        item_selected_color: Some(Color::rgb(0x722ed1)),
                        ..Default::default()
                    })
                    .with_tokens(TokenPatch {
                        font_size: Some(16.),
                        font_height: Some(24.),
                        line_height: Some(1.5),
                        line_width: Some(2.),
                        ..Default::default()
                    }),
                );
            }
            let theme = config.resolve();
            let tokens = theme.component::<TabsTheme>();
            let size = match options["size"].as_str() {
                Some("small") => Size::Small,
                Some("large") => Size::Large,
                _ => Size::Medium,
            };
            let card = matches!(options["type"].as_str(), Some("card" | "editable-card"));
            let vertical = matches!(options["placement"].as_str(), Some("start" | "end"));
            let context = format!(
                "{}/{}/{}/{}",
                case["theme"], case["state"], options["id"], options["direction"]
            );
            let padding = tokens.padding(size, card, vertical);
            for item in sample["items"].as_array().unwrap() {
                let b = measurement(&item["box"]);
                let label = measurement(&item["label"]);
                near(
                    &context,
                    "font-size",
                    tokens.font_size(size),
                    &label["fontSize"],
                );
                near(
                    &context,
                    "line-height",
                    tokens.line_height(size),
                    &label["lineHeight"],
                );
                for (field, actual) in [
                    ("paddingTop", padding.top),
                    ("paddingRight", padding.right),
                    ("paddingBottom", padding.bottom),
                    ("paddingLeft", padding.left),
                ] {
                    near(&context, field, actual, &b[field]);
                }
                let selected = item["selected"] == true;
                let disabled = item["disabled"] == true;
                let color = tokens.label_color(
                    selected,
                    disabled,
                    case["state"] == "hover",
                    case["state"] == "active",
                );
                assert_eq!(color, css_color(&label["color"]), "{context} label color");
                let background = if card {
                    if selected {
                        tokens.globals.color_bg_container
                    } else {
                        tokens.card_bg
                    }
                } else {
                    Color::TRANSPARENT
                };
                assert_eq!(
                    background,
                    css_color(&b["background"]),
                    "{context} background"
                );
                compared += 1;
            }
            assert_eq!(
                tokens.ink_bar_color,
                css_color(&measurement(&sample["indicator"])["background"]),
                "{context} indicator"
            );
        }
    }
    assert_eq!(compared, 3840);
}
