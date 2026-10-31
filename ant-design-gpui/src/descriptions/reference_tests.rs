// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Independent expectations captured from the original Ant Design renderer.

use std::{collections::BTreeSet, sync::OnceLock};

use gpui::{Hsla, Styled};
use serde_json::Value;

use super::{
    DescriptionsBreakpoints, DescriptionsColumns, DescriptionsSpan, DescriptionsStyle,
    DescriptionsTheme, DescriptionsTokenPatch, geometry::plan_rows,
};
use crate::{
    Size,
    theme::{AlgorithmChain, Color, LineType, ResolvedTheme, ThemeConfig},
};

fn reference() -> &'static Value {
    static REFERENCE: OnceLock<Value> = OnceLock::new();
    REFERENCE.get_or_init(|| {
        serde_json::from_str(include_str!("../../fixtures/descriptions/reference.json"))
            .expect("original-source Descriptions browser measurements")
    })
}

fn scenes() -> &'static Value {
    static SCENES: OnceLock<Value> = OnceLock::new();
    SCENES.get_or_init(|| {
        serde_json::from_str(include_str!(
            "../../fixtures/descriptions/visual-cases.json"
        ))
        .expect("shared Descriptions source inputs")
    })
}

fn number(value: &Value) -> f64 {
    value.as_f64().expect("numeric browser metric")
}

fn text(value: &Value) -> &str {
    value.as_str().expect("textual browser metric")
}

fn near(native: f64, browser: f64, context: &str) {
    assert!(
        (native - browser).abs() <= 0.001,
        "{context}: native={native}, browser={browser}"
    );
}

fn pixels(value: &Value) -> f64 {
    text(value)
        .trim_end_matches("px")
        .parse()
        .expect("CSS pixel length")
}

fn color(native: Hsla, browser: &Value, context: &str) {
    let channels = text(browser)
        .trim_start_matches("rgba(")
        .trim_start_matches("rgb(")
        .trim_end_matches(')')
        .split(',')
        .map(|channel| channel.trim().parse::<f64>().expect("CSS color channel"))
        .collect::<Vec<_>>();
    assert!(matches!(channels.len(), 3 | 4));
    let native: gpui::Rgba = native.into();
    for (native, browser) in [native.r, native.g, native.b].into_iter().zip(&channels) {
        assert!(
            (f64::from(native) - browser / 255.).abs() < 0.0001,
            "{context}"
        );
    }
    assert!(
        (f64::from(native.a) - channels.get(3).copied().unwrap_or(1.)).abs() <= 0.0021,
        "{context}"
    );
}

fn theme(name: &str, input: &Value) -> ResolvedTheme {
    let mut config = ThemeConfig {
        algorithms: Some(match name {
            "default" => AlgorithmChain::default(),
            "dark" => AlgorithmChain::dark(),
            "compact" => AlgorithmChain::compact(),
            "dark-compact" => AlgorithmChain::dark_compact(),
            unknown => panic!("unknown reference algorithm {unknown}"),
        }),
        ..Default::default()
    };
    config.seed.line_width = input["globalTokens"]["lineWidth"].as_f64();
    config.seed.line_type = input["globalTokens"]["lineType"]
        .as_str()
        .map(|value| match value {
            "solid" => LineType::Solid,
            "dashed" => LineType::Dashed,
            "dotted" => LineType::Dotted,
            unknown => panic!("unknown reference line type {unknown}"),
        });
    let tokens = &input["tokens"];
    let mut patch = DescriptionsTokenPatch::default();
    macro_rules! colors {
        ($($field:ident => $key:literal),+ $(,)?) => {
            $(patch.$field = tokens[$key].as_str().map(|value| value.parse::<Color>().expect("source color"));)+
        };
    }
    colors!(label_bg => "labelBg", label_color => "labelColor", title_color => "titleColor",
        content_color => "contentColor", extra_color => "extraColor");
    macro_rules! lengths {
        ($($field:ident => $key:literal),+ $(,)?) => { $(patch.$field = tokens[$key].as_f64();)+ };
    }
    lengths!(title_margin_bottom => "titleMarginBottom", item_padding_bottom => "itemPaddingBottom",
        item_padding_end => "itemPaddingEnd", colon_margin_left => "colonMarginLeft",
        colon_margin_right => "colonMarginRight");
    config.components.set::<DescriptionsTheme>(patch);
    config.resolve()
}

fn size(input: &Value) -> Size {
    match input["size"].as_str() {
        None | Some("large") => Size::Large,
        Some("medium") => Size::Medium,
        Some("small") => Size::Small,
        unknown => panic!("unknown reference size {unknown:?}"),
    }
}

fn breakpoints(value: &Value) -> DescriptionsBreakpoints<usize> {
    let at = |key: &str| value[key].as_u64().map(|value| value as usize);
    DescriptionsBreakpoints {
        xs: at("xs"),
        sm: at("sm"),
        md: at("md"),
        lg: at("lg"),
        xl: at("xl"),
        xxl: at("xxl"),
        xxxl: at("xxxl"),
    }
}

fn columns(input: &Value) -> DescriptionsColumns {
    input["column"].as_u64().map_or_else(
        || DescriptionsColumns::Responsive(breakpoints(&input["column"])),
        |value| DescriptionsColumns::Fixed(value as usize),
    )
}

fn span(item: &Value) -> DescriptionsSpan {
    match &item["span"] {
        Value::String(value) if value == "filled" => DescriptionsSpan::Filled,
        Value::Number(value) => DescriptionsSpan::Fixed(value.as_u64().unwrap() as usize),
        Value::Object(_) => DescriptionsSpan::Responsive(breakpoints(&item["span"])),
        Value::Null => DescriptionsSpan::default(),
        unknown => panic!("unknown source span {unknown}"),
    }
}

#[test]
fn browser_fixture_covers_all_source_scenes_and_the_pinned_revision() {
    assert_eq!(reference()["schema_version"], 1);
    assert_eq!(
        reference()["provenance"]["revision"],
        "f8f5595ab5e891fff6a2a06c5b3b98af643d008e"
    );
    let covered = reference()["cases"]
        .as_array()
        .unwrap()
        .iter()
        .map(|case| text(&case["scene"]))
        .collect::<BTreeSet<_>>();
    assert_eq!(
        covered,
        scenes()
            .as_object()
            .unwrap()
            .keys()
            .map(String::as_str)
            .collect()
    );
    assert_eq!(covered.len(), 32);
    let algorithms = reference()["cases"]
        .as_array()
        .unwrap()
        .iter()
        .map(|case| text(&case["theme"]))
        .collect::<BTreeSet<_>>();
    assert_eq!(
        algorithms,
        BTreeSet::from(["default", "dark", "compact", "dark-compact"])
    );
}

#[test]
fn logical_columns_and_row_spans_match_actual_browser_cells_for_every_case() {
    for case in reference()["cases"].as_array().unwrap() {
        let scene = &scenes()[text(&case["scene"])];
        let viewport = number(&case["viewport"]["width"]);
        for (input, browser) in scene["components"]
            .as_array()
            .unwrap()
            .iter()
            .zip(case["components"].as_array().unwrap())
        {
            assert_eq!(input["id"], browser["id"]);
            let theme = theme(text(&case["theme"]), input);
            let count = columns(input).resolve(viewport, &theme.tokens);
            let bordered = input["bordered"].as_bool().unwrap_or(false);
            let vertical = input["layout"] == "vertical";
            let rows = plan_rows(
                count,
                input["items"]
                    .as_array()
                    .unwrap()
                    .iter()
                    .map(|item| span(item).resolve(viewport, &theme.tokens)),
            );
            let mut native = Vec::new();
            for row in rows {
                let mut first = Vec::new();
                let mut second = Vec::new();
                for item in row {
                    if vertical {
                        first.push((
                            if bordered { "label" } else { "item" },
                            item.item,
                            item.span,
                        ));
                        second.push((
                            if bordered { "content" } else { "item" },
                            item.item,
                            item.span,
                        ));
                    } else if bordered {
                        first.push(("label", item.item, 1));
                        first.push(("content", item.item, item.span * 2 - 1));
                    } else {
                        first.push(("item", item.item, item.span));
                    }
                }
                native.push(first);
                if vertical {
                    native.push(second);
                }
            }
            let browser_rows = browser["rows"].as_array().unwrap();
            let actual = browser_rows
                .iter()
                .map(|row| {
                    row["cells"]
                        .as_array()
                        .unwrap()
                        .iter()
                        .map(|cell| {
                            (
                                text(&cell["kind"]),
                                cell["item"].as_u64().unwrap() as usize,
                                cell["span"].as_u64().unwrap() as usize,
                            )
                        })
                        .collect::<Vec<_>>()
                })
                .collect::<Vec<_>>();
            assert_eq!(native, actual, "{} / {}", case["name"], input["id"]);
            if let Some(first) = actual.first() {
                let physical_columns = first.iter().map(|cell| cell.2).sum::<usize>();
                assert_eq!(
                    count * if bordered && !vertical { 2 } else { 1 },
                    physical_columns
                );
            }
        }
    }
}

#[test]
fn resolved_tokens_sizes_typography_padding_and_colors_match_browser_css() {
    let mut checked = 0;
    for case in reference()["cases"].as_array().unwrap() {
        let scene = &scenes()[text(&case["scene"])];
        for (input, browser) in scene["components"]
            .as_array()
            .unwrap()
            .iter()
            .zip(case["components"].as_array().unwrap())
        {
            // Semantic refinements are separately checked; they intentionally
            // replace component defaults on individual browser cells.
            if input.get("styles").is_some() {
                continue;
            }
            let theme = theme(text(&case["theme"]), input);
            let bordered = input["bordered"].as_bool().unwrap_or(false);
            let metrics = DescriptionsStyle::resolve(&theme, size(input), bordered);
            let context = format!("{} / {}", case["name"], input["id"]);
            near(
                metrics.font_size,
                number(&browser["root"]["fontSize"]),
                &context,
            );
            near(
                metrics.font_size * metrics.line_height,
                number(&browser["root"]["lineHeight"]),
                &context,
            );
            if browser["header"].is_object() {
                near(
                    metrics.title_margin_bottom,
                    number(&browser["header"]["margin"][2]),
                    &context,
                );
            }
            if browser["title"].is_object() {
                near(
                    metrics.title_font_size,
                    number(&browser["title"]["fontSize"]),
                    &context,
                );
                near(
                    metrics.title_font_size * metrics.title_line_height,
                    number(&browser["title"]["lineHeight"]),
                    &context,
                );
                near(
                    metrics.title_font_weight,
                    text(&browser["title"]["fontWeight"]).parse().unwrap(),
                    &context,
                );
                color(metrics.title_color, &browser["title"]["color"], &context);
            }
            if browser["extra"].is_object() {
                color(metrics.extra_color, &browser["extra"]["color"], &context);
            }
            near(
                metrics.border_radius,
                pixels(&browser["view"]["borderRadius"]),
                &context,
            );
            if bordered {
                near(
                    metrics.border_width,
                    number(&browser["view"]["border"][0]),
                    &context,
                );
                color(
                    metrics.border_color,
                    &browser["view"]["borderColor"],
                    &context,
                );
            }
            let rows = browser["rows"].as_array().unwrap();
            for (row_index, row) in rows.iter().enumerate() {
                let cells = row["cells"].as_array().unwrap();
                for (cell_index, cell) in cells.iter().enumerate() {
                    let item_index = cell["item"].as_u64().unwrap() as usize;
                    if input["items"][item_index].get("styles").is_some() {
                        continue;
                    }
                    let padding = &cell["bounds"]["padding"];
                    if bordered {
                        for (index, expected) in [
                            metrics.cell_padding_vertical,
                            metrics.cell_padding_horizontal,
                            metrics.cell_padding_vertical,
                            metrics.cell_padding_horizontal,
                        ]
                        .into_iter()
                        .enumerate()
                        {
                            near(expected, number(&padding[index]), &context);
                        }
                    } else {
                        near(1., number(&padding[0]), &context);
                        near(
                            if row_index + 1 == rows.len() {
                                0.
                            } else {
                                metrics.item_padding_bottom
                            },
                            number(&padding[2]),
                            &context,
                        );
                        let (start, end) = if case["direction"] == "rtl" {
                            (1, 3)
                        } else {
                            (3, 1)
                        };
                        near(1., number(&padding[start]), &context);
                        near(
                            if cell_index + 1 == cells.len() {
                                0.
                            } else {
                                metrics.item_padding_end
                            },
                            number(&padding[end]),
                            &context,
                        );
                    }
                    if cell["label"].is_object() {
                        color(metrics.label_color, &cell["label"]["color"], &context);
                    }
                    if cell["content"].is_object() {
                        color(metrics.content_color, &cell["content"]["color"], &context);
                    }
                    if bordered && cell["kind"] == "label" {
                        color(
                            metrics.label_background,
                            &cell["bounds"]["background"],
                            &context,
                        );
                    }
                    if cell["colon"].is_object() {
                        let (left, right) = if case["direction"] == "rtl" {
                            (metrics.colon_margin_right, metrics.colon_margin_left)
                        } else {
                            (metrics.colon_margin_left, metrics.colon_margin_right)
                        };
                        near(left, pixels(&cell["colon"]["marginLeft"]), &context);
                        near(right, pixels(&cell["colon"]["marginRight"]), &context);
                        near(
                            metrics.colon_offset_y,
                            pixels(&cell["colon"]["top"]),
                            &context,
                        );
                    }
                    checked += 1;
                }
            }
        }
    }
    assert!(checked > 250, "exercise the full browser metrics matrix");
}

#[test]
fn captured_semantic_overrides_apply_to_cells_and_keep_item_precedence() {
    let case = reference()["cases"]
        .as_array()
        .unwrap()
        .iter()
        .find(|case| case["scene"] == "semantic")
        .unwrap();
    let browser = &case["components"][0];
    let cells = browser["rows"][0]["cells"].as_array().unwrap();
    let slot = |label, content| super::DescriptionsStyles {
        label: gpui::StyleRefinement::default().text_color(Color::rgb(label)),
        content: gpui::StyleRefinement::default().text_color(Color::rgb(content)),
        ..Default::default()
    };
    let mut native = slot(0x000000, 0x1677ff);
    let item = slot(0x389e0d, 0xad6800);
    native.refine(&item);
    color(
        native.label.text.color.unwrap(),
        &cells[2]["bounds"]["color"],
        "per-item label color",
    );
    color(
        native.content.text.color.unwrap(),
        &cells[3]["bounds"]["color"],
        "per-item content color",
    );
    assert_eq!(
        cells[2]["bounds"]["padding"],
        serde_json::json!([12, 12, 12, 12])
    );
    color(
        Color::rgb(0xfff1f0).into(),
        &cells[0]["bounds"]["background"],
        "component label background",
    );
    color(
        Color::rgb(0xf6ffed).into(),
        &cells[2]["bounds"]["background"],
        "item label background",
    );
}
