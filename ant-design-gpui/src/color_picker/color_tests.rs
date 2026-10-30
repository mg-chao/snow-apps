// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;

fn color(s: &str) -> PickerColor {
    s.parse().unwrap()
}

#[test]
fn parses_all_hex_lengths_and_optional_prefix() {
    for (source, expected) in [
        ("#123", "#112233"),
        ("1234", "#11223344"),
        ("ABCDEF", "#abcdef"),
        (" #1677ff80 ", "#1677ff80"),
    ] {
        assert_eq!(color(source).to_hex_string(), expected);
    }
    for bad in [
        "#12",
        "#12345",
        "#1234567",
        "#123456789",
        "#gggggg",
        "#１２３",
        "##fff",
    ] {
        assert!(PickerColor::parse(bad).is_err(), "accepted {bad}");
    }
}

#[test]
fn parses_css_names_without_silently_accepting_unknown_names() {
    assert_eq!(color("RebeccaPurple").to_hex_string(), "#663399");
    assert_eq!(color("LightGoldenrodYellow").to_hex_string(), "#fafad2");
    assert_eq!(color("TRANSPARENT"), PickerColor::TRANSPARENT);
    assert!(PickerColor::parse("chartreuse-ish").is_err());
}

#[test]
fn parses_rgb_legacy_and_modern_notation() {
    assert_eq!(color("rgb(22, 119, 255)").to_hex_string(), "#1677ff");
    assert_eq!(color("RGB(100% 50% 0% / 50%)").to_rgb(), (255, 128, 0, 0.5));
    assert_eq!(color("rgba(255,0,0,.25)").to_rgb(), (255, 0, 0, 0.25));
    assert!((color("rgb(12.5 0 0)").to_rgba().r as f64 - 12.5 / 255.).abs() < 1e-7);
}

#[test]
fn parses_hsl_hsv_hsb_and_hue_units() {
    assert_eq!(color("hsl(120 100% 50%)").to_hex_string(), "#00ff00");
    assert_eq!(
        color("hsl(.5turn 100% 50% / 25%)").to_rgb(),
        (0, 255, 255, 0.25)
    );
    assert_eq!(color("hsb(240,100%,100%)").to_rgb(), (0, 0, 255, 1.));
    assert_eq!(color("hsv(400grad 100% 100%)").to_rgb(), (255, 0, 0, 1.));
    assert_eq!(color("hsb(-120deg 100% 100%)").to_rgb(), (0, 0, 255, 1.));
    assert_eq!(
        color("hsva(3.141592653589793rad 100% 100% / .5)").to_rgb(),
        (0, 255, 255, 0.5)
    );
}

#[test]
fn rejects_malformed_unsupported_or_out_of_range_function_values() {
    for bad in [
        "",
        "rgb()",
        "rgb(1,2)",
        "rgb(1 2 3 4)",
        "rgb(1,2,3 / .5)",
        "rgb(1 2 3 / / .5)",
        "rgb(1 2 3)junk",
        "rgb(256 0 0)",
        "rgb(-1 0 0)",
        "rgba(1,2,3,2)",
        "rgb(NaN 0 0)",
        "hsl(10 101% 50%)",
        "hsv(0 100% -1%)",
        "oklch(50% .2 20)",
        "var(--brand)",
        "rgb(1px,2,3)",
    ] {
        assert!(PickerColor::parse(bad).is_err(), "accepted {bad}");
    }
}

#[test]
fn constructors_reject_non_finite_and_out_of_range_channels() {
    assert_eq!(
        PickerColor::new(f64::NAN, 0., 0., 1.),
        Err(ColorPickerError::NonFiniteChannel)
    );
    assert_eq!(
        PickerColor::new(20., -0.1, 0., 1.),
        Err(ColorPickerError::ChannelOutOfRange)
    );
    assert!(PickerColor::new(361., 1., 1., 1.).is_err());
    assert!(PickerColor::from_rgb(f64::INFINITY, 0., 0., 1.).is_err());
    assert!(PickerColor::from_rgb(256., 0., 0., 1.).is_err());
}

#[test]
fn hsb_precision_survives_black_and_gray_edits() {
    let original =
        PickerColor::new(217.123456789, 0.73123456789, 0.9123456789, 0.417123456789).unwrap();
    let black = original.with_brightness(0.);
    assert_eq!(black.to_rgb(), (0, 0, 0, original.to_hsb().3));
    assert_eq!(black.with_brightness(original.to_hsb().2), original);
    assert_eq!(
        original
            .with_saturation(0.)
            .with_saturation(original.to_hsb().1),
        original
    );
    assert_eq!(original.with_hue(f64::NAN), original);
    assert_eq!(original.with_alpha(4.).to_hsb().3, 1.);
    assert_eq!(original.with_brightness(-1.).to_hsb().2, 0.);
}

#[test]
fn rgba_hex_and_css_conversions_preserve_color_and_opacity() {
    let c = color("#2ddcb480");
    assert_eq!(c.to_hex(), "2ddcb480");
    assert_eq!(c.to_rgb(), (45, 220, 180, 128. / 255.));
    assert_eq!(color(&c.to_css_string()).to_rgb(), c.to_rgb());
    assert_eq!(c.to_theme_color().a, 128. / 255.);
    let rgba = c.to_rgba();
    assert!((rgba.r - 45. / 255.).abs() < 1e-6);
    assert_eq!(color("red").to_hsb_string(), "hsb(0, 100%, 100%)");
    assert_eq!(PickerColor::TRANSPARENT.to_rgb_string(), "rgba(0,0,0,0)");
}

#[test]
fn every_rgb_byte_survives_hsb_conversion_at_many_channel_combinations() {
    for r in 0..=255 {
        for (g, b) in [(0, 0), (255, 255), (23, 187), (r, 255 - r)] {
            let c = PickerColor::from_rgb(f64::from(r), f64::from(g), f64::from(b), 1.).unwrap();
            assert_eq!(c.to_rgb(), (r, g, b, 1.));
        }
    }
}

#[test]
fn gradients_validate_and_sort_stably_without_requiring_two_stops() {
    assert_eq!(
        ColorPickerValue::gradient([]).unwrap(),
        ColorPickerValue::Cleared
    );
    let one = ColorPickerValue::gradient([ColorStop {
        color: color("red"),
        percent: 20.,
    }])
    .unwrap();
    assert!(one.is_gradient());
    let value = ColorPickerValue::gradient([
        ColorStop {
            color: color("blue"),
            percent: 80.,
        },
        ColorStop {
            color: color("red"),
            percent: 20.,
        },
        ColorStop {
            color: color("lime"),
            percent: 20.,
        },
    ])
    .unwrap();
    let ColorPickerValue::Gradient(stops) = value else {
        panic!()
    };
    assert_eq!(stops[0].color, color("red"));
    assert_eq!(stops[1].color, color("lime"));
    for percent in [f64::NAN, f64::INFINITY, -1., 101.] {
        assert_eq!(
            ColorPickerValue::gradient([ColorStop {
                color: color("red"),
                percent
            }]),
            Err(ColorPickerError::InvalidStopPercent)
        );
    }
}

#[test]
fn empty_clear_and_alpha_zero_remain_distinct() {
    assert!(ColorPickerValue::default().is_cleared());
    assert_eq!(
        " ".parse::<ColorPickerValue>().unwrap(),
        ColorPickerValue::Cleared
    );
    let transparent = ColorPickerValue::Single(PickerColor::TRANSPARENT);
    assert!(!transparent.is_cleared());
    assert!(!transparent.preset_eq(&ColorPickerValue::Cleared));
    assert_eq!(
        ColorPickerValue::Cleared.active_color(99),
        PickerColor::TRANSPARENT
    );
    assert!(ColorPickerValue::Gradient(Arc::from([])).visual_eq(&ColorPickerValue::Cleared));
}

#[test]
fn opaque_normalization_maps_all_gradient_stops_and_preserves_clear() {
    let value = ColorPickerValue::gradient([
        ColorStop {
            color: color("#f008"),
            percent: 0.,
        },
        ColorStop {
            color: color("#00f0"),
            percent: 100.,
        },
    ])
    .unwrap();
    let opaque = value.with_opaque_alpha();
    assert!(opaque.is_gradient());
    assert_eq!(opaque.active_color(0).to_hsb().3, 1.);
    assert_eq!(opaque.active_color(1).to_hsb().3, 1.);
    assert_eq!(
        ColorPickerValue::Cleared.with_opaque_alpha(),
        ColorPickerValue::Cleared
    );
}

#[test]
fn preset_equality_uses_visible_color_and_all_gradient_stops() {
    let a = PickerColor::new(123., 0., 0.5, 1.).unwrap();
    let b = PickerColor::new(217., 0., 0.5, 1.).unwrap();
    assert_ne!(a, b);
    assert!(ColorPickerValue::Single(a).preset_eq(&ColorPickerValue::Single(b)));
    assert!(!ColorPickerValue::Single(a).preset_eq(&ColorPickerValue::Single(b.with_alpha(0.5))));
}

#[test]
fn preset_checkmark_contrast_uses_actual_background_and_reference_threshold() {
    let light = crate::theme::Color::rgb(0xffffff);
    let dark = crate::theme::Color::rgb(0x1f1f1f);
    assert!(color("#0000001a").is_bright_on(light));
    assert!(!color("#0000001a").is_bright_on(dark));
    assert!(!color("#aaaaaa").is_bright_on(light));
    assert!(!color("#c0c0c0").is_bright_on(light));
    assert!(color("#c1c1c1").is_bright_on(light));
    assert!(color("#ffffff").is_bright_on(dark));
    assert!(
        PickerColor::from_rgb(255., 255., 255., 0.5)
            .unwrap()
            .is_bright_on(crate::theme::Color::BLACK)
    );
}
