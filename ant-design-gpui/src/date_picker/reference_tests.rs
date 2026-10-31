// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;
use crate::theme::{AlgorithmChain, ThemeConfig};
use serde_json::Value;

fn reference() -> Value {
    serde_json::from_str(include_str!(
        "../../fixtures/date_time_picker/style-reference.json"
    ))
    .unwrap()
}
fn case<'a>(data: &'a Value, scene: &str, theme: &str) -> &'a Value {
    let name = format!("{scene}-{theme}-ltr-100");
    data["cases"]
        .as_array()
        .unwrap()
        .iter()
        .find(|c| c["name"] == name)
        .unwrap()
}
fn close(actual: f64, expected: &Value) {
    assert!(
        (actual - expected.as_f64().unwrap()).abs() < 0.02,
        "{actual} != {expected}"
    );
}

#[test]
fn picker_tokens_match_independent_browser_panel_geometry() {
    let data = reference();
    for (name, algorithm) in [
        ("default", AlgorithmChain::default()),
        ("dark", AlgorithmChain::dark()),
        ("compact", AlgorithmChain::compact()),
        ("dark-compact", AlgorithmChain::dark_compact()),
    ] {
        let theme = ThemeConfig {
            algorithms: Some(algorithm),
            ..Default::default()
        }
        .resolve();
        let tokens = theme.component::<DatePickerTheme>();
        let date = case(&data, "date-selected", name);
        close(
            tokens.panel_width(),
            &date["geometry"]["panels"][0]["width"],
        );
        close(
            tokens.header_height(),
            &date["geometry"]["headers"][0]["height"],
        );
        close(
            tokens.cell_height,
            &date["geometry"]["cells"][0]["inner"]["height"],
        );
        close(
            tokens.cell_width,
            &date["geometry"]["cells"][0]["rect"]["width"],
        );
        let time = case(&data, "time", name);
        close(
            tokens.time_column_width,
            &time["geometry"]["timeColumns"][0]["width"],
        );
        close(
            tokens.time_cell_height,
            &time["geometry"]["timeCells"][0]["rect"]["height"],
        );
    }
}

#[test]
fn picker_default_formats_match_independent_browser_inputs() {
    let data = reference();
    for (scene, mode, date, time, twelve) in [
        ("date-selected", PickerMode::Date, true, false, false),
        ("month", PickerMode::Month, true, false, false),
        ("quarter", PickerMode::Quarter, true, false, false),
        ("year", PickerMode::Year, true, false, false),
        ("week", PickerMode::Week, true, false, false),
        ("datetime", PickerMode::Date, true, true, false),
        ("time", PickerMode::Date, false, true, false),
        ("time-12-hour", PickerMode::Date, false, true, true),
    ] {
        let mut context = PickerFormatContext::new(mode, date, time);
        context.use_12_hours = twelve;
        let expected = case(&data, scene, "default");
        let raw = expected["options"]["value"].as_str().unwrap();
        let value = NaiveDateTime::parse_from_str(raw, "%Y-%m-%dT%H:%M:%S%.f")
            .or_else(|_| {
                NaiveDate::parse_from_str(raw, "%Y-%m-%d").map(|d| d.and_time(NaiveTime::MIN))
            })
            .unwrap();
        assert_eq!(
            PickerFormat::default().format(value, context),
            expected["geometry"]["inputs"][0]["value"].as_str().unwrap(),
            "{scene}"
        );
    }
}
