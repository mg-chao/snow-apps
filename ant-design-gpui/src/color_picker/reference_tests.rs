// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
//! Compare native resolved values with executed upstream browser/color fixtures.

use super::{ColorPickerFormat, ColorPickerTheme, ColorPickerTokenPatch, PickerColor};
use crate::{
    Size,
    theme::{AlgorithmChain, Color, ComponentThemeConfig, ThemeConfig, TokenPatch},
};
use serde_json::Value;

fn near(actual: f64, expected: f64, name: &str, property: &str) {
    assert!(
        (actual - expected).abs() < 0.02,
        "{name}: {property}: native={actual}, reference={expected}"
    );
}
fn number(v: &Value) -> f64 {
    v.as_f64().unwrap()
}
fn length(v: &Value) -> f64 {
    v.as_str().unwrap().trim_end_matches("px").parse().unwrap()
}
fn matches_color(actual: Color, expected: &Value, name: &str, property: &str) {
    let expected = PickerColor::parse(expected.as_str().unwrap())
        .unwrap()
        .to_rgb();
    assert_eq!(
        (actual.r, actual.g, actual.b),
        (expected.0, expected.1, expected.2),
        "{name}: {property}"
    );
    assert!(
        (actual.a - expected.3).abs() < 1e-8,
        "{name}: {property} alpha {} != {}",
        actual.a,
        expected.3
    );
}
fn theme(case: &Value) -> crate::theme::ResolvedTheme {
    let mut theme = ThemeConfig {
        algorithms: Some(match case["theme"].as_str().unwrap() {
            "dark" => AlgorithmChain::dark(),
            "compact" => AlgorithmChain::compact(),
            "dark-compact" => AlgorithmChain::dark_compact(),
            _ => AlgorithmChain::default(),
        }),
        ..Default::default()
    };
    if case["rows"]
        .as_array()
        .unwrap()
        .iter()
        .any(|r| r["options"]["tokenCase"] == true)
    {
        theme.components.set_theme::<ColorPickerTheme>(
            ComponentThemeConfig::new(ColorPickerTokenPatch::default()).with_tokens(TokenPatch {
                color_primary: Some(Color::rgb(0x722ed1)),
                color_primary_hover: Some(Color::rgb(0x9254de)),
                color_border: Some(Color::rgb(0xb37feb)),
                control_outline: Some(Color::rgb(0x722ed1).with_alpha(0.15)),
                border_radius: Some(10.),
                ..Default::default()
            }),
        );
    }
    theme.resolve()
}
fn fixture() -> Value {
    serde_json::from_str(include_str!(
        "../../fixtures/color_picker/style-reference.json"
    ))
    .unwrap()
}

#[test]
fn color_picker_theme_and_trigger_metrics_match_independent_browser() {
    let data = fixture();
    assert_eq!(
        data["upstream"]["revision"],
        "f8f5595ab5e891fff6a2a06c5b3b98af643d008e"
    );
    let scenes: Value = serde_json::from_str(include_str!(
        "../../fixtures/color_picker/visual-cases.json"
    ))
    .unwrap();
    assert_eq!(
        data["cases"].as_array().unwrap().len(),
        scenes.as_object().unwrap().len() * 8
    );
    for case in data["cases"].as_array().unwrap() {
        let theme = theme(case);
        let tokens = theme.component::<ColorPickerTheme>();
        let g = tokens.globals();
        let name = format!("{}/{}/{}", case["scene"], case["theme"], case["direction"]);
        let reference = &case["tokens"];
        for (key, actual) in [
            ("fontSize", theme.tokens.font_size),
            ("fontSizeSM", theme.tokens.font_size_sm),
            ("controlHeight", g.control_height),
            ("controlHeightSM", g.control_height_sm),
            ("controlHeightLG", g.control_height_lg),
            ("marginSM", g.margin_sm),
            ("marginXS", g.margin_xs),
        ] {
            near(actual, number(&reference[key]), &name, key);
        }
        for (key, actual) in [
            ("colorBgContainer", theme.tokens.color_bg_container),
            ("colorBgElevated", theme.tokens.color_bg_elevated),
            ("colorFillSecondary", theme.tokens.color_fill_secondary),
            ("colorBorder", theme.tokens.color_border),
            ("colorPrimary", theme.tokens.color_primary),
            ("colorPrimaryHover", theme.tokens.color_primary_hover),
            ("controlOutline", theme.tokens.control_outline),
        ] {
            matches_color(actual, &reference[key], &name, key);
        }
        for row in case["rows"].as_array().unwrap() {
            // Custom triggers retain the native Button's own metrics and colors;
            // only the default ColorPicker trigger owns a swatch and these tokens.
            if row["trigger"].is_null() || row["options"]["customTrigger"] == true {
                continue;
            }
            let options = &row["options"];
            let size = match options["size"].as_str() {
                Some("small") => Size::Small,
                Some("large") => Size::Large,
                _ => Size::Medium,
            };
            let metrics = tokens.trigger_metrics(size);
            let semantic = options["semantic"] == true;
            let swatch_size = if semantic { 40. } else { metrics.swatch_size };
            let trigger = &row["trigger"];
            let swatch = &row["swatch"];
            near(
                metrics.radius,
                length(&trigger["borderRadius"]),
                &name,
                "trigger radius",
            );
            near(swatch_size, number(&swatch["width"]), &name, "swatch width");
            near(
                swatch_size,
                number(&swatch["height"]),
                &name,
                "swatch height",
            );
            near(
                if semantic { 12. } else { metrics.swatch_radius },
                length(&swatch["borderRadius"]),
                &name,
                "swatch radius",
            );
            if semantic {
                matches_color(
                    Color::rgb(0x00ff00),
                    &swatch["background"],
                    &name,
                    "semantic body background",
                );
                matches_color(
                    Color::rgb(0x800080),
                    &swatch["borderColor"],
                    &name,
                    "semantic body border",
                );
                let content = &row["content"];
                near(
                    20.,
                    number(&content["width"]),
                    &name,
                    "semantic content width",
                );
                near(
                    20.,
                    number(&content["height"]),
                    &name,
                    "semantic content height",
                );
                near(
                    5.,
                    length(&content["borderRadius"]),
                    &name,
                    "semantic content radius",
                );
                matches_color(
                    Color::rgb(0xff0000),
                    &content["background"],
                    &name,
                    "semantic content background",
                );
                matches_color(
                    Color::rgb(0x0000ff),
                    &content["borderColor"],
                    &name,
                    "semantic content border",
                );
            }
            let has_text = options["showText"] == true
                || matches!(options["textRender"].as_str(), Some("custom" | "icon"));
            let height = metrics
                .height
                .max(swatch_size + 2. * (metrics.padding + g.line_width));
            near(
                height,
                number(&trigger["height"]),
                &name,
                "actual trigger height",
            );
            if !has_text {
                near(
                    height,
                    number(&trigger["width"]),
                    &name,
                    "square trigger width",
                );
            }
            matches_color(
                if options["disabled"] == true {
                    g.color_bg_container_disabled
                } else {
                    g.color_bg_elevated
                },
                &trigger["background"],
                &name,
                "trigger background",
            );
            let border = match options["status"].as_str() {
                Some("error") => {
                    if options["hover"] == true {
                        g.color_error_hover
                    } else {
                        g.color_error
                    }
                }
                Some("warning") => {
                    if options["hover"] == true {
                        g.color_warning_hover
                    } else {
                        g.color_warning
                    }
                }
                _ if options["open"] == true => g.color_primary,
                _ if options["hover"] == true => g.color_primary_hover,
                _ => g.color_border,
            };
            matches_color(border, &trigger["borderColor"], &name, "trigger border");
        }
    }
}

#[test]
fn color_picker_panel_rails_editors_and_thumbs_match_independent_browser() {
    let data = fixture();
    for case in data["cases"].as_array().unwrap() {
        let geometry = &case["geometry"];
        if geometry["panel"].is_null() {
            continue;
        }
        let theme = theme(case);
        let t = theme.component::<ColorPickerTheme>();
        let g = t.globals();
        let name = format!("{}/{}/{}", case["scene"], case["theme"], case["direction"]);
        let panel = &geometry["panel"];
        let palette = &geometry["palette"];
        let hue = &geometry["hue"];
        let alpha = &geometry["alpha"];
        let preview = &geometry["preview"];
        let standalone = case["scene"] == "standalone";
        let padding = if standalone { g.padding_sm } else { 12. };
        let content_width = if case["scene"] == "split-panel" {
            t.width * 2. + 25.
        } else {
            t.width
        };
        near(
            content_width + padding * 2.,
            number(&panel["width"]),
            &name,
            "popup width including12px padding",
        );
        matches_color(
            g.color_bg_elevated,
            &panel["background"],
            &name,
            "popup background",
        );
        near(
            g.border_radius_lg,
            length(&panel["borderRadius"]),
            &name,
            "popup radius",
        );
        near(t.width, number(&palette["width"]), &name, "palette width");
        near(
            t.palette_height(),
            number(&palette["height"]),
            &name,
            "palette height",
        );
        near(
            g.border_radius_sm,
            length(&palette["borderRadius"]),
            &name,
            "palette radius",
        );
        near(
            t.preview_size,
            number(&preview["width"]),
            &name,
            "preview width",
        );
        near(
            t.preview_size,
            number(&preview["height"]),
            &name,
            "preview height",
        );
        near(
            t.width - t.preview_size - g.margin_sm,
            number(&hue["width"]),
            &name,
            "hue width",
        );
        near(t.slider_height, number(&hue["height"]), &name, "hue height");
        near(
            t.slider_height / 2.,
            length(&hue["borderRadius"]),
            &name,
            "rail radius",
        );
        let row_y = number(&palette["y"]) + number(&palette["height"]) + g.margin_sm;
        near(row_y, number(&preview["y"]), &name, "slider row spacing");
        near(
            row_y
                + if alpha.is_null() {
                    (t.preview_size - t.slider_height) / 2.
                } else {
                    0.
                },
            number(&hue["y"]),
            &name,
            "hue centering when alpha disabled",
        );
        if !alpha.is_null() {
            near(
                row_y + t.preview_size - t.slider_height,
                number(&alpha["y"]),
                &name,
                "alpha position",
            );
            near(
                number(&hue["width"]),
                number(&alpha["width"]),
                &name,
                "equal rail widths",
            );
        }
        for editor in geometry["editors"].as_array().unwrap() {
            let format = match case["rows"][0]["options"]["format"].as_str() {
                Some("rgb") => ColorPickerFormat::Rgb,
                Some("hsb") => ColorPickerFormat::Hsb,
                _ => ColorPickerFormat::Hex,
            };
            near(
                t.editor_height(format),
                number(&editor["height"]),
                &name,
                "editor height including compact text linebox",
            );
            near(
                row_y + t.preview_size + g.margin_sm,
                number(&editor["y"]),
                &name,
                "editor row spacing",
            );
        }
        if case["scene"] == "alpha-disabled" {
            assert!(alpha.is_null());
            let editors = geometry["editors"].as_array().unwrap();
            assert_eq!(editors.len(), 1);
            near(
                t.width,
                number(&editors[0]["width"]),
                &name,
                "full-width HEX with alpha/format disabled",
            );
        }
        let thumb = &case["controls"]["paletteThumb"];
        near(
            t.handler_size,
            number(&thumb["width"]),
            &name,
            "palette thumb size",
        );
        near(
            g.line_width_bold,
            length(&thumb["borderWidth"]),
            &name,
            "palette thumb rim",
        );
        for thumb in case["controls"]["railThumbs"].as_array().unwrap() {
            near(
                t.handler_size_sm,
                number(&thumb["outerWidth"]),
                &name,
                "rail thumb outer size",
            );
            near(
                t.handler_size_sm - 2. * g.line_width_bold,
                number(&thumb["width"]),
                &name,
                "rail thumb inner size",
            );
            near(
                g.line_width_bold,
                number(&thumb["borderWidth"]),
                &name,
                "rail thumb rim",
            );
        }
        for preset in geometry["presets"].as_array().unwrap() {
            near(
                t.preset_color_size,
                number(&preset["width"]),
                &name,
                "preset width",
            );
            near(
                t.preset_color_size,
                number(&preset["height"]),
                &name,
                "preset height",
            );
        }
    }
}

#[test]
fn color_picker_conversions_match_executed_pinned_color_reference() {
    let data: Value = serde_json::from_str(include_str!(
        "../../fixtures/color_picker/color-reference.json"
    ))
    .unwrap();
    assert_eq!(data["upstream"]["rc_color_picker"], "3.1.1");
    assert_eq!(data["upstream"]["fast_color"], "3.0.1");
    assert!(data["cases"].as_array().unwrap().len() >= 20);
    for case in data["cases"].as_array().unwrap() {
        let source = case["input"].as_str().unwrap();
        let color: PickerColor = source.parse().unwrap();
        let (r, g, b, a) = color.to_rgb();
        assert_eq!(
            (r, g, b),
            (
                number(&case["rgb"]["r"]) as u8,
                number(&case["rgb"]["g"]) as u8,
                number(&case["rgb"]["b"]) as u8
            ),
            "{source}"
        );
        assert!(
            (a - number(&case["rgb"]["a"])).abs() < 1e-12,
            "{source}: alpha"
        );
        assert_eq!(
            color.to_hex_string(),
            case["hex"].as_str().unwrap(),
            "{source}: hex"
        );
        assert_eq!(
            color.to_hsb_string(),
            case["hsb_string"].as_str().unwrap(),
            "{source}: HSB display"
        );
    }
}
