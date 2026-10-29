// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;
use crate::{
    locale::{CarouselMessagePatch, Locale, LocalePatch},
    theme::{ComponentTheme, ThemeConfig},
};

#[test]
fn all_component_tokens_match_independently_executed_reference_generators() {
    use crate::theme::AlgorithmChain;
    let reference: serde_json::Value =
        serde_json::from_str(include_str!("../../fixtures/carousel/reference.json")).unwrap();
    for (name, algorithms) in [
        ("default", AlgorithmChain::default()),
        ("dark", AlgorithmChain::dark()),
        ("compact", AlgorithmChain::compact()),
        ("dark-compact", AlgorithmChain::dark_compact()),
    ] {
        let theme = ThemeConfig {
            algorithms: Some(algorithms),
            ..Default::default()
        }
        .resolve();
        let native = theme.component::<CarouselTheme>();
        let expected = &reference["componentDefaults"][name]["defaults"];
        for (key, value) in [
            ("dotWidth", native.dot_width),
            ("dotHeight", native.dot_height),
            ("dotActiveWidth", native.dot_active_width),
            ("dotGap", native.dot_gap),
            ("dotOffset", native.dot_offset),
            ("arrowSize", native.arrow_size),
            ("arrowOffset", native.arrow_offset),
        ] {
            assert_eq!(value, expected[key].as_f64().unwrap(), "{name}/{key}");
        }
    }
}

#[test]
fn reference_tokens_and_invalid_overrides_are_finite() {
    let theme = ThemeConfig::default().resolve();
    let mut tokens = CarouselTheme::default_tokens(&theme.tokens);
    assert_eq!(
        (tokens.dot_width, tokens.dot_height, tokens.dot_active_width),
        (16., 3., 24.)
    );
    assert_eq!(
        (
            tokens.dot_gap,
            tokens.dot_offset,
            tokens.arrow_size,
            tokens.arrow_offset
        ),
        (4., 12., 16., 8.)
    );
    CarouselTheme::apply(
        &mut tokens,
        &CarouselTokenPatch {
            dot_width: Some(f64::NAN),
            dot_height: Some(-2.),
            arrow_size: Some(f64::INFINITY),
            ..Default::default()
        },
    );
    assert_eq!(
        (tokens.dot_width, tokens.dot_height, tokens.arrow_size),
        (16., 0., 16.)
    );
}

#[test]
fn carousel_locales_patch_independently_and_preserve_shared_no_op() {
    let english = Locale::en_us();
    assert_eq!(english.carousel.prev_slide.as_ref(), "Previous slide");
    assert_eq!(Locale::zh_cn().carousel.next_slide.as_ref(), "下一张");
    assert_eq!(Locale::zh_tw().carousel.next_slide.as_ref(), "下一張");
    let no_op = LocalePatch {
        carousel: CarouselMessagePatch {
            prev_slide: Some("Previous slide".into()),
            ..Default::default()
        },
        ..Default::default()
    };
    assert!(std::sync::Arc::ptr_eq(&english, &no_op.resolve(&english)));
    let patch = LocalePatch {
        carousel: CarouselMessagePatch {
            root_label: Some("Photos".into()),
            ..Default::default()
        },
        ..Default::default()
    };
    let result = patch.resolve(&english);
    assert_eq!(result.carousel.root_label.as_ref(), "Photos");
    assert_eq!(result.carousel.next_slide, english.carousel.next_slide);
    assert_eq!(result.pagination, english.pagination);
    assert!(std::sync::Arc::ptr_eq(
        &result,
        &patch.resolve_with_previous(&english, Some(&result))
    ));
}
