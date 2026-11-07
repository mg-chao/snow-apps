// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;
fn color(s: &str) -> PickerColor {
    s.parse().unwrap()
}
fn gradient() -> ColorPickerValue {
    ColorPickerValue::gradient([
        ColorStop {
            color: color("red"),
            percent: 0.,
        },
        ColorStop {
            color: color("blue"),
            percent: 100.,
        },
    ])
    .unwrap()
}

#[test]
fn stop_crossing_preserves_identity_color_and_active_selection() {
    let mut m = PickerModel::new(gradient());
    let id = m.stops()[0].id;
    let added = m.add_stop(50.).unwrap();
    assert_eq!(m.active_index(), 1);
    assert_eq!(m.active_color().to_rgb(), (128, 0, 128, 1.));
    assert!(m.move_stop(id, 75.));
    assert_eq!(m.active_index(), 1);
    assert_eq!(m.stops()[1].id, id);
    assert_eq!(m.active_color(), color("red"));
    assert_eq!(m.index_for_id(added), Some(0));
}

#[test]
fn interpolated_stops_include_alpha_and_extend_endpoint_colors() {
    let value = ColorPickerValue::gradient([
        ColorStop {
            color: color("#f000"),
            percent: 20.,
        },
        ColorStop {
            color: color("blue"),
            percent: 80.,
        },
    ])
    .unwrap();
    let mut m = PickerModel::new(value);
    m.add_stop(50.);
    assert_eq!(m.active_color().to_rgb(), (128, 0, 128, 0.5));
    m.add_stop(0.);
    assert_eq!(m.active_color(), color("#f000"));
    m.add_stop(100.);
    assert_eq!(m.active_color(), color("blue"));
}

#[test]
fn deletion_preserves_active_stop_and_stops_at_two() {
    let mut m = PickerModel::new(gradient());
    let extra = m.add_stop(30.).unwrap();
    let last = m.stops()[2].id;
    assert!(m.remove_stop(last));
    assert_eq!(m.stops()[m.active_index()].id, extra);
    assert!(!m.remove_stop(extra));
    assert!(!m.remove_stop(u64::MAX));
}

#[test]
fn switching_modes_restores_cached_gradient_until_single_color_is_edited() {
    let mut m = PickerModel::new(gradient());
    let original = m.stops().to_vec();
    assert!(m.set_mode(ColorPickerMode::Single));
    assert_eq!(m.value(), &ColorPickerValue::Single(color("red")));
    assert!(m.set_mode(ColorPickerMode::Gradient));
    assert_eq!(m.stops(), original);
    m.set_mode(ColorPickerMode::Single);
    m.set_active_color(color("lime"));
    m.set_mode(ColorPickerMode::Gradient);
    assert_eq!(m.stops().len(), 2);
    assert!(m.stops().iter().all(|s| s.color == color("lime")));
    assert_eq!(m.stops()[0].percent, 0.);
    assert_eq!(m.stops()[1].percent, 100.);
}

#[test]
fn external_replacement_preserves_matching_ids_and_replaces_mode_cache() {
    let mut m = PickerModel::new(gradient());
    let old = m.stops().to_vec();
    m.set_value(gradient());
    assert_eq!(m.stops(), old);
    m.set_mode(ColorPickerMode::Single);
    m.set_value(ColorPickerValue::Single(color("lime")));
    m.set_mode(ColorPickerMode::Gradient);
    assert!(m.stops().iter().all(|s| s.color == color("lime")));
}

#[test]
fn cleared_gradient_recovery_edits_only_selected_stop() {
    let mut m = PickerModel::new(gradient());
    assert!(m.clear());
    assert!(!m.clear());
    assert_eq!(m.mode(), ColorPickerMode::Gradient);
    assert_eq!(m.active_color(), PickerColor::TRANSPARENT);
    assert!(m.set_active_color(color("lime")));
    assert_eq!(m.stops().len(), 2);
    assert_eq!(m.stops()[0].color, color("lime"));
    assert_eq!(m.stops()[1].color, PickerColor::TRANSPARENT);
}

#[test]
fn one_stop_external_gradient_is_not_padded_and_can_gain_second_stop() {
    let mut m = PickerModel::new(
        ColorPickerValue::gradient([ColorStop {
            color: color("red"),
            percent: 50.,
        }])
        .unwrap(),
    );
    assert_eq!(m.stops().len(), 1);
    assert!(!m.remove_stop(m.stops()[0].id));
    m.add_stop(75.);
    assert_eq!(m.stops().len(), 2);
    assert_eq!(m.active_color(), color("red"));
}

#[test]
fn invalid_drag_coordinates_and_stale_ids_do_not_change_value() {
    let mut m = PickerModel::new(gradient());
    let original = m.value().clone();
    let id = m.stops()[0].id;
    for percent in [f64::NAN, f64::INFINITY, -1., 101.] {
        assert_eq!(m.add_stop(percent), None);
        assert!(!m.move_stop(id, percent));
    }
    assert!(!m.move_stop(u64::MAX, 20.));
    assert_eq!(m.value(), &original);
}

#[test]
fn snapshot_restoration_rolls_back_preview_without_reassigning_ids() {
    let mut m = PickerModel::new(gradient());
    let accepted = m.clone();
    let id = m.stops()[0].id;
    m.move_stop(id, 80.);
    m.set_active_color(color("lime"));
    assert_ne!(m.value(), accepted.value());
    m = accepted;
    assert_eq!(m.stops()[0].id, id);
    assert_eq!(m.stops()[0].percent, 0.);
    assert_eq!(m.stops()[0].color, color("red"));
}

#[test]
fn forced_gradient_mode_prepares_handles_without_changing_supplied_value() {
    for value in [
        ColorPickerValue::Cleared,
        ColorPickerValue::Single(color("#1677ff80")),
    ] {
        let mut m = PickerModel::new(value.clone());
        m.prepare_mode(ColorPickerMode::Gradient);
        assert_eq!(m.value(), &value);
        assert_eq!(m.mode(), ColorPickerMode::Gradient);
        assert_eq!(m.stops().len(), 2);
        let ids = m.stops().iter().map(|s| s.id).collect::<Vec<_>>();
        m.prepare_mode(ColorPickerMode::Gradient);
        assert_eq!(m.stops().iter().map(|s| s.id).collect::<Vec<_>>(), ids);
        m.add_stop(50.);
        assert!(m.value().is_gradient());
    }
}

#[test]
fn alpha_disabled_normalization_preserves_active_stop_while_crossing() {
    let mut m = PickerModel::new(
        ColorPickerValue::gradient([
            ColorStop {
                color: color("#f008"),
                percent: 0.,
            },
            ColorStop {
                color: color("#00f4"),
                percent: 50.,
            },
            ColorStop {
                color: color("#0f00"),
                percent: 100.,
            },
        ])
        .unwrap(),
    );
    let id = m.stops()[1].id;
    m.set_active_index(1);
    m.set_active_color(color("#ff08"));
    assert!(m.force_opaque_alpha());
    assert_eq!(m.stops()[m.active_index()].id, id);
    assert!(m.stops().iter().all(|s| s.color.to_hsb().3 == 1.));
    assert!(m.move_stop(id, 100.));
    assert_eq!(m.stops()[m.active_index()].id, id);
    assert_eq!(m.active_color().to_hex_string(), "#ffff00");
    assert!(!m.force_opaque_alpha());
    assert!(m.move_stop(id, 25.));
    assert_eq!(m.stops()[m.active_index()].id, id);
}

#[test]
fn opaque_normalization_preserves_cleared_marker_and_hidden_hue() {
    let mut m = PickerModel::new(ColorPickerValue::Cleared);
    m.prepare_mode(ColorPickerMode::Gradient);
    let stops = m.stops().to_vec();
    assert!(!m.force_opaque_alpha());
    assert!(m.value().is_cleared());
    assert_eq!(m.stops(), stops);
    let color = PickerColor::new(237.1234, 0.5, 0., 0.25).unwrap();
    m.set_value(ColorPickerValue::Single(color));
    assert!(m.force_opaque_alpha());
    assert_eq!(m.active_color(), color.with_alpha(1.));
}

#[test]
fn editing_locked_single_external_gradient_discards_old_stops() {
    let mut m = PickerModel::new(gradient());
    m.prepare_mode(ColorPickerMode::Single);
    m.set_active_color(color("lime"));
    assert!(m.stops().is_empty());
    m.prepare_mode(ColorPickerMode::Gradient);
    assert_eq!(m.stops().len(), 2);
    assert!(m.stops().iter().all(|stop| stop.color == color("lime")));
    m.set_active_index(1);
    m.set_active_color(color("yellow"));
    assert_eq!(m.stops()[0].color, color("lime"));
    assert_eq!(m.stops()[1].color, color("yellow"));
}
