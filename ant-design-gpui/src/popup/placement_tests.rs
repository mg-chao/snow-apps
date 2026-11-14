// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::{PopupArrow, PopupGeometry, PopupPlacement};
use gpui::{Bounds, point, px, size};
use serde_json::Value;

const NAMES: [&str; 12] = [
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
fn number(value: &Value) -> f32 {
    value.as_f64().expect("reference number") as f32
}

#[test]
fn popup_geometry_matches_live_reference_trigger_alignment() {
    let fixture: Value =
        serde_json::from_str(include_str!("../../fixtures/popup/placement.json")).unwrap();
    let cases = fixture["cases"].as_array().unwrap();
    assert_eq!(cases.len(), 4560, "complete live browser matrix");
    let mut mismatches = Vec::new();
    for case in cases {
        let requested = NAMES
            .iter()
            .position(|name| Some(*name) == case["placement"].as_str())
            .unwrap();
        let a = &case["anchorBounds"];
        let geometry = PopupGeometry {
            anchor: Bounds::new(
                point(px(number(&a["x"])), px(number(&a["y"]))),
                size(px(number(&a["width"])), px(number(&a["height"]))),
            ),
            popup_size: size(
                px(number(&case["popupSize"][0])),
                px(number(&case["popupSize"][1])),
            ),
            viewport: Bounds::new(
                point(px(0.), px(0.)),
                size(
                    px(number(&case["viewport"]["width"])),
                    px(number(&case["viewport"]["height"])),
                ),
            ),
            placement: PopupPlacement::ALL[requested],
            arrow: PopupArrow {
                visible: case["arrow"] != "hidden",
                point_at_center: case["arrow"] == "center",
            },
            arrow_size: px(number(&case["arrowSize"])),
            gap: px(4.),
            radius: px(number(&case["radius"])),
            offset: point(px(0.), px(0.)),
            auto_adjust_overflow: case["overflow"].as_bool().unwrap(),
        };
        let actual = geometry.resolve();
        let expected = &case["bounds"];
        let error_x = (f32::from(actual.bounds.origin.x) - number(&expected["x"])).abs();
        let error_y = (f32::from(actual.bounds.origin.y) - number(&expected["y"])).abs();
        let actual_name = NAMES[PopupPlacement::ALL
            .iter()
            .position(|placement| *placement == actual.placement)
            .unwrap()];
        // rc-trigger positions centered arrows at the midpoint of the actual
        // overlap. Edge-aligned arrows are positioned by Ant's CSS instead.
        let arrow_error = if actual.placement.is_centered() && geometry.arrow.visible {
            let (coordinate, property) = if actual.placement.is_vertical() {
                (actual.arrow.unwrap().x, "left")
            } else {
                (actual.arrow.unwrap().y, "top")
            };
            let expected: f32 = case["arrowOffset"][property]
                .as_str()
                .unwrap()
                .trim_end_matches("px")
                .parse()
                .unwrap();
            (f32::from(coordinate) - expected).abs()
        } else {
            0.
        };
        if error_x > 0.51
            || error_y > 0.51
            || arrow_error > 0.51
            || Some(actual_name) != case["effectivePlacement"].as_str()
        {
            mismatches.push(format!("case {} {} {} arrow={} size={} radius={}: native {:?} at {:?}, browser {} at ({}, {}), error=({error_x}, {error_y}), arrow_error={arrow_error}", case["id"], case["anchor"], case["placement"], case["arrow"], case["popupSize"], case["radius"], actual.placement, actual.bounds.origin, case["effectivePlacement"], expected["x"], expected["y"]));
        }
    }
    assert!(
        mismatches.is_empty(),
        "{} / {} live placement mismatches:\n{}",
        mismatches.len(),
        cases.len(),
        mismatches
            .iter()
            .take(24)
            .cloned()
            .collect::<Vec<_>>()
            .join("\n")
    );
}
