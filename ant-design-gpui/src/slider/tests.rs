// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::{model::SliderModel, *};
use crate::Direction;

#[test]
fn fractional_grid_and_marks_snap_without_binary_drift() {
    let scale = SliderScale::new(&SliderOptions {
        min: -1.,
        max: 1.,
        step: Some(0.1),
        marks: vec![0.35],
        ..Default::default()
    })
    .unwrap();
    assert_eq!(scale.snap(0.30000000000000004), Some(0.3));
    assert_eq!(scale.snap(0.349), Some(0.35));
    assert_eq!(scale.offset(0.3, 1), Some(0.35));
    assert_eq!(scale.offset(0.35, 1), Some(0.4));
    assert_eq!(scale.offset(0.3, 2), Some(0.4));
    assert_eq!(scale.offset(0.4, -2), Some(0.3));
    assert_eq!(scale.snap(f64::NAN), None);
    assert_eq!(scale.from_fraction(f64::INFINITY), None);
}

#[test]
fn marks_only_includes_endpoints_and_ignores_out_of_range_marks() {
    let scale = SliderScale::new(&SliderOptions {
        min: 10.,
        max: 90.,
        step: None,
        marks: vec![100., 40., 20., 40., -5.],
        ..Default::default()
    })
    .unwrap();
    assert_eq!(scale.marks(), &[20., 40.]);
    assert_eq!(scale.snap(30.), Some(40.));
    assert_eq!(scale.offset(40., 1), Some(90.));
    assert_eq!(scale.offset(20., -1), Some(10.));
    assert_eq!(scale.offset(90., 2), Some(90.));
}

#[test]
fn very_small_steps_and_degenerate_ranges_are_finite() {
    let scale = SliderScale::new(&SliderOptions {
        max: 1e-18,
        step: Some(1e-20),
        ..Default::default()
    })
    .unwrap();
    assert_eq!(scale.snap(3.01e-20), Some(3e-20));
    let fixed = SliderScale::new(&SliderOptions {
        min: 7.,
        max: 7.,
        ..Default::default()
    })
    .unwrap();
    assert_eq!(fixed.fraction(7.), 0.);
    assert_eq!(fixed.from_fraction(0.5), Some(7.));
    assert_eq!(fixed.offset(7., 1), Some(7.));
}

#[test]
fn invalid_configuration_is_rejected_before_state_changes() {
    for options in [
        SliderOptions {
            min: 2.,
            max: 1.,
            ..Default::default()
        },
        SliderOptions {
            max: f64::INFINITY,
            ..Default::default()
        },
        SliderOptions {
            min: -f64::MAX,
            max: f64::MAX,
            ..Default::default()
        },
    ] {
        assert_eq!(options.validate(), Err(SliderError::InvalidBounds));
    }
    assert_eq!(
        SliderOptions {
            step: Some(0.),
            ..Default::default()
        }
        .validate(),
        Err(SliderError::InvalidStep)
    );
    let mut model = SliderModel::new(SliderOptions::default(), 10.0.into()).unwrap();
    assert_eq!(
        model.set_value(f64::NAN.into()),
        Err(SliderError::NonFiniteValue)
    );
    assert_eq!(model.value(), SliderValue::Single(10.));
    assert_eq!(
        model.set_value([1., 2.].into()),
        Err(SliderError::ValueModeMismatch)
    );
}

#[test]
fn direction_and_reverse_follow_physical_axis() {
    let mut options = SliderOptions::default();
    assert!(!options.inverted(Direction::Ltr));
    assert!(options.inverted(Direction::Rtl));
    options.reverse = true;
    assert!(!options.inverted(Direction::Rtl));
    options.orientation = SliderOrientation::Vertical;
    assert!(!options.inverted(Direction::Rtl));
    options.reverse = false;
    assert!(options.inverted(Direction::Ltr));
}

fn range(values: &[f64], config: SliderRange) -> SliderModel {
    SliderModel::new(
        SliderOptions {
            range: Some(config),
            ..Default::default()
        },
        values.to_vec().into(),
    )
    .unwrap()
}

#[test]
fn crossing_reorders_values_without_changing_active_identity() {
    let mut model = range(&[20., 80.], SliderRange::default());
    let id = model.handles[0].id;
    let proposal = model.move_handle(id, 90.).unwrap();
    assert_eq!(
        model.value(),
        [20., 80.].into(),
        "proposals do not change accepted values"
    );
    assert_eq!(proposal.value(true), [80., 90.].into());
    assert_eq!(proposal.active, Some(id));
    model.accept(&proposal);
    assert_eq!(model.handles[1].id, id);
    assert_eq!(
        model.move_handle(id, 10.).unwrap().value(true),
        [10., 80.].into()
    );
}

#[test]
fn disabled_handles_are_fixed_boundaries_and_disable_editing_and_track_dragging() {
    let mut model = range(
        &[20., 50., 80.],
        SliderRange {
            draggable_track: true,
            ..Default::default()
        },
    );
    model.options.disabled = [true, false, true].into();
    let middle = model.handles[1].id;
    assert_eq!(
        model.move_handle(middle, 0.).unwrap().value(true),
        [20., 20., 80.].into()
    );
    assert_eq!(
        model.move_handle(middle, 100.).unwrap().value(true),
        [20., 80., 80.].into()
    );
    assert!(model.move_handle(model.handles[0].id, 40.).is_none());
    assert!(model.click(10.).is_none());
    assert!(!model.track_draggable());
    model.options.range = Some(SliderRange {
        editable: true,
        ..Default::default()
    });
    assert!(!model.editable());
}

#[test]
fn editable_range_limits_insertion_and_deletion() {
    let mut model = range(
        &[20., 80.],
        SliderRange {
            editable: true,
            min_count: 2,
            max_count: Some(3),
            ..Default::default()
        },
    );
    assert!(model.remove(model.handles[0].id).is_none());
    let inserted = model.click(50.).unwrap();
    model.accept(&inserted);
    assert_eq!(model.value(), [20., 50., 80.].into());
    let at_limit = model.click(60.).unwrap();
    assert_eq!(at_limit.value(true), [20., 60., 80.].into());
    let removed = model.remove(inserted.active.unwrap()).unwrap();
    model.accept(&removed);
    assert_eq!(model.value(), [20., 80.].into());
}

#[test]
fn dragging_a_track_preserves_span_and_clamps_at_both_ends() {
    let model = range(
        &[20., 50., 80.],
        SliderRange {
            draggable_track: true,
            ..Default::default()
        },
    );
    assert_eq!(
        model.shift_track(&model.handles, 50.).unwrap().value(true),
        [40., 70., 100.].into()
    );
    assert_eq!(
        model.shift_track(&model.handles, -50.).unwrap().value(true),
        [0., 30., 60.].into()
    );
}

#[test]
fn value_updates_preserve_duplicate_handle_identity() {
    let mut model = range(&[20., 20., 80.], SliderRange::default());
    let ids: Vec<_> = model.handles.iter().map(|h| h.id).collect();
    model.set_value([20., 30., 80.].into()).unwrap();
    assert_eq!(model.handles.iter().map(|h| h.id).collect::<Vec<_>>(), ids);
}

#[test]
fn numeric_behavior_matches_executed_upstream_reference() {
    let reference: serde_json::Value =
        serde_json::from_str(include_str!("../../fixtures/slider/model.json")).unwrap();
    let mut failures = Vec::new();
    for case in reference["cases"].as_array().unwrap() {
        let scale = reference["scales"]
            .as_array()
            .unwrap()
            .iter()
            .find(|s| s["name"] == case["scale"])
            .unwrap();
        let options = SliderOptions {
            min: scale["min"].as_f64().unwrap(),
            max: scale["max"].as_f64().unwrap(),
            step: scale["step"].as_f64(),
            marks: scale["marks"]
                .as_array()
                .unwrap()
                .iter()
                .map(|v| v.as_f64().unwrap())
                .collect(),
            ..Default::default()
        };
        let input = case["input"].as_f64().unwrap();
        let model = SliderModel::new(options, input.into()).unwrap();
        let actual = match case["kind"].as_str().unwrap() {
            "snap" => model.scale.snap(input).unwrap(),
            "offset" => model
                .scale
                .offset(input, case["units"].as_i64().unwrap() as i32)
                .unwrap(),
            "drag" => {
                model
                    .drag_handle(
                        model.handles[0].id,
                        input,
                        case["target"].as_f64().unwrap() - input,
                    )
                    .unwrap()
                    .handles[0]
                    .value
            }
            other => panic!("Unknown reference operation {other}"),
        };
        let expected = case["expected"].as_f64().unwrap();
        if (actual - expected).abs() > 1e-12 {
            failures.push(format!("{case}: native={actual}"));
        }
    }
    for case in reference["anchors"].as_array().unwrap() {
        let values: Vec<_> = case["values"]
            .as_array()
            .unwrap()
            .iter()
            .map(|v| v.as_f64().unwrap())
            .collect();
        let mut model = range(&values, SliderRange::default());
        model.options.disabled = SliderDisabled::Handles(
            case["disabled"]
                .as_array()
                .unwrap()
                .iter()
                .map(|v| v.as_bool().unwrap())
                .collect(),
        );
        let actual = model
            .nearest(case["target"].as_f64().unwrap())
            .map_or(-1, |i| i as i64);
        if actual != case["expected"].as_i64().unwrap() {
            failures.push(format!("{case}: native index={actual}"));
        }
    }
    assert!(
        failures.is_empty(),
        "{} reference mismatches:\n{}",
        failures.len(),
        failures.join("\n")
    );
}
