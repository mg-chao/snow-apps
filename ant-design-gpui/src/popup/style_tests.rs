// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::{PopupColor, PopupPlacement};
use crate::{
    popconfirm::{PopconfirmStyle, PopconfirmTheme, PopconfirmTokenPatch},
    popover::{PopoverStyle, PopoverTheme, PopoverTokenPatch},
    theme::{AlgorithmChain, Color, ComponentThemeConfig, ThemeConfig, TokenPatch},
    tooltip::{TooltipStyle, TooltipTheme, TooltipTokenPatch},
};
use std::sync::Arc;

#[test]
fn popup_default_dimensions_follow_the_reference_rules() {
    let theme = ThemeConfig::default().resolve();
    let tooltip = TooltipStyle::resolve(&theme, None, PopupPlacement::Top);
    assert_eq!(
        (tooltip.max_width, tooltip.min_width, tooltip.min_height),
        (250., 28., 32.)
    );
    assert_eq!(
        (
            tooltip.padding_inline,
            tooltip.padding_block,
            tooltip.radius
        ),
        (8., 6., 6.)
    );
    assert_eq!(
        (tooltip.font_size, tooltip.line_height * tooltip.font_size),
        (14., 22.)
    );
    let aligned = TooltipStyle::resolve(&theme, None, PopupPlacement::TopLeft);
    assert_eq!(aligned.min_width, 34.);
    let popover = PopoverStyle::resolve(&theme, None);
    assert_eq!(
        (
            popover.padding,
            popover.title_min_width,
            popover.title_margin_bottom
        ),
        (12., 177., 8.)
    );
    assert_eq!(
        (
            popover.radius,
            popover.title_weight,
            popover.content_padding_inline
        ),
        (8., 600., 0.)
    );
    assert_eq!(PopconfirmStyle::resolve(&theme, false).title_weight, 400.);
    let confirm = PopconfirmStyle::resolve(&theme, true);
    assert_eq!(
        (
            confirm.title_weight,
            confirm.icon_gap,
            confirm.description_margin_top,
            confirm.message_margin_bottom
        ),
        (600., 8., 4., 8.)
    );
    assert_eq!(theme.component::<TooltipTheme>().z_index_popup, 1070);
    assert_eq!(theme.component::<PopoverTheme>().z_index_popup, 1030);
    assert_eq!(theme.component::<PopconfirmTheme>().z_index_popup, 1060);
}

#[test]
fn popup_dark_and_compact_algorithms_preserve_component_specific_rules() {
    for algorithms in [
        AlgorithmChain::default(),
        AlgorithmChain::dark(),
        AlgorithmChain::compact(),
        AlgorithmChain::dark_compact(),
    ] {
        let theme = ThemeConfig {
            algorithms: Some(algorithms),
            ..Default::default()
        }
        .resolve();
        let g = &theme.tokens;
        let tooltip = TooltipStyle::resolve(&theme, None, PopupPlacement::Right);
        let popover = PopoverStyle::resolve(&theme, None);
        let confirm = PopconfirmStyle::resolve(&theme, true);
        assert_eq!(tooltip.background, g.color_bg_spotlight.into());
        assert_eq!(tooltip.foreground, g.color_text_light_solid.into());
        assert_eq!(popover.background, g.color_bg_elevated.into());
        assert_eq!(
            popover.padding, 12.,
            "literal reference padding remains 12 in compact mode"
        );
        assert_eq!(popover.title_margin_bottom, g.margin_xs);
        assert_eq!(tooltip.min_height, g.control_height);
        assert_eq!(confirm.font_size, g.font_size);
    }
}

#[test]
fn popup_custom_color_contrast_is_limited_to_tooltips() {
    let theme = ThemeConfig::default().resolve();
    for (color, foreground) in [
        (Color::rgb(0xf8edda), Color::BLACK),
        (Color::rgb(0x241b38), Color::WHITE),
    ] {
        let tooltip = TooltipStyle::resolve(&theme, Some(color.into()), PopupPlacement::Top);
        assert_eq!(tooltip.background, color.into());
        assert_eq!(tooltip.foreground, foreground.into());
        let popover = PopoverStyle::resolve(&theme, Some(PopupColor::Custom(color)));
        assert_eq!(popover.foreground, theme.tokens.color_text.into());
    }
    let palette = crate::theme::PresetColor::Blue;
    let tooltip = TooltipStyle::resolve(&theme, Some(palette.into()), PopupPlacement::Top);
    assert_eq!(
        tooltip.background,
        theme.tokens.palettes.get(palette).tones[5].into()
    );
}

#[test]
fn popup_wireframe_title_and_content_padding_follow_font_height() {
    let theme = ThemeConfig {
        tokens: TokenPatch {
            wireframe: Some(true),
            ..Default::default()
        },
        ..Default::default()
    }
    .resolve();
    let style = PopoverStyle::resolve(&theme, None);
    assert_eq!((style.padding, style.title_margin_bottom), (0., 0.));
    assert_eq!(
        (
            style.title_padding_top,
            style.title_padding_bottom,
            style.title_padding_inline
        ),
        (5., 4., 16.)
    );
    assert_eq!(
        (
            style.title_border_width,
            style.content_padding_inline,
            style.content_padding_block
        ),
        (1., 16., 12.)
    );
}

#[test]
fn popup_component_tokens_are_cached_and_do_not_leak_between_components() {
    let mut config = ThemeConfig::default();
    config.components.set_theme::<TooltipTheme>(
        ComponentThemeConfig::new(TooltipTokenPatch {
            max_width: Some(333.),
            z_index_popup: Some(6000),
        })
        .with_tokens(TokenPatch {
            border_radius: Some(20.),
            font_size: Some(18.),
            ..Default::default()
        }),
    );
    config.components.set::<PopoverTheme>(PopoverTokenPatch {
        title_min_width: Some(120.),
        ..Default::default()
    });
    config
        .components
        .set::<PopconfirmTheme>(PopconfirmTokenPatch {
            z_index_popup: Some(8000),
        });
    let theme = config.resolve();
    assert!(Arc::ptr_eq(
        &theme.component::<TooltipTheme>(),
        &theme.component::<TooltipTheme>()
    ));
    let horizontal = TooltipStyle::resolve(&theme, None, PopupPlacement::Left);
    let aligned = TooltipStyle::resolve(&theme, None, PopupPlacement::BottomLeft);
    assert_eq!(
        (horizontal.radius, aligned.radius, aligned.min_width),
        (8., 20., 58.)
    );
    assert_eq!((aligned.font_size, aligned.max_width), (18., 333.));
    let popover = PopoverStyle::resolve(&theme, None);
    assert_eq!(
        (popover.title_min_width, popover.radius, popover.font_size),
        (120., 8., 14.)
    );
    assert_eq!(theme.component::<PopconfirmTheme>().z_index_popup, 8000);
}
