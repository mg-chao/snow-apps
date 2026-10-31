// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::sync::Arc;

use gpui::{px, relative};

use super::*;
use crate::theme::{AlgorithmChain, Color, ComponentThemeConfig, ThemeConfig, TokenPatch};

#[test]
fn divider_tokens_merge_sparse_overrides_and_share_cached_results() {
    let mut parent = ThemeConfig::default();
    parent.components.set::<DividerTheme>(DividerTokenPatch {
        text_padding_inline: Some(px(12.).into()),
        orientation_margin: Some(0.2),
        vertical_margin_inline: None,
    });
    let mut child = ThemeConfig::default();
    child.components.set::<DividerTheme>(DividerTokenPatch {
        text_padding_inline: None,
        orientation_margin: Some(0.),
        vertical_margin_inline: Some(0.),
    });
    let theme = ThemeConfig::merged(&parent, &child).resolve();
    let tokens = theme.component::<DividerTheme>();
    assert!(Arc::ptr_eq(&tokens, &theme.component::<DividerTheme>()));
    assert_eq!(tokens.text_padding_inline, px(12.).into());
    assert_eq!(tokens.orientation_margin, 0.);
    assert_eq!(tokens.vertical_margin_inline, 0.);
    let default = ThemeConfig::default().resolve().component::<DividerTheme>();
    assert_eq!(default.text_padding_inline, relative(1.));
    assert_eq!(default.orientation_margin, 0.05);
    assert_eq!(default.vertical_margin_inline, 8.);
}

#[test]
fn divider_component_algorithms_and_global_overrides_remain_local() {
    let mut config = ThemeConfig::default();
    config.components.set_theme::<DividerTheme>(
        ComponentThemeConfig::default()
            .with_algorithms(AlgorithmChain::dark_compact())
            .with_tokens(TokenPatch {
                font_size: Some(20.),
                color_split: Some(Color::rgb(0x1677ff)),
                margin_lg: Some(37.),
                ..Default::default()
            }),
    );
    let theme = config.resolve();
    let mut expected = ThemeConfig {
        algorithms: Some(AlgorithmChain::dark_compact()),
        ..Default::default()
    };
    // Component algorithms begin with the resolved provider's seed values,
    // including its selected light/dark base colors.
    expected.seed.color_text_base = Some(Some(theme.tokens.color_text_base));
    expected.seed.color_bg_base = Some(Some(theme.tokens.color_bg_base));
    expected.tokens.font_size = Some(20.);
    expected.tokens.color_split = Some(Color::rgb(0x1677ff));
    expected.tokens.margin_lg = Some(37.);
    let expected = expected.resolve();
    let style = DividerStyle::resolve(&theme, None, false, false);
    assert_eq!(style, DividerStyle::resolve(&expected, None, false, false));
    assert_eq!(style.font_size, 16.);
    assert_eq!(style.margin_block, 37.);
    assert_eq!(theme.tokens.font_size, 14.);
    assert_ne!(theme.tokens.color_split, style.line_color);
}

#[test]
fn divider_component_length_and_ratio_patches_have_bounded_rendering_values() {
    for (ratio, expected) in [(-1., 0.), (2., 1.), (f32::NAN, 0.05), (f32::INFINITY, 0.05)] {
        let mut config = ThemeConfig::default();
        config.components.set::<DividerTheme>(DividerTokenPatch {
            orientation_margin: Some(ratio),
            vertical_margin_inline: Some(f32::NAN),
            ..Default::default()
        });
        let style = DividerStyle::resolve(&config.resolve(), None, true, false);
        assert_eq!(style.orientation_margin, expected);
        assert_eq!(style.vertical_margin_inline, 0.);
    }
}
