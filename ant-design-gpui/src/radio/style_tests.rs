// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;
use crate::theme::{AlgorithmChain, ComponentThemeConfig, ThemeConfig, TokenPatch};

#[test]
fn radio_style_matches_reference_geometry_and_state_colors() {
    let theme = ResolvedTheme::default();
    let empty = RadioStyle::resolve(&theme, false, false);
    assert_eq!(empty.size, 16.);
    assert_eq!(empty.dot_size, 6.);
    assert_eq!(empty.line_width, 1.);
    assert_eq!(empty.font_size, 14.);
    assert_eq!(empty.line_height, 22.);
    assert_eq!(empty.label_padding, 8.);
    assert_eq!(empty.group_gap, 8.);
    assert_eq!(empty.vertical_gap, 8.);
    assert_eq!(empty.focus_width, 3.);
    assert_eq!(empty.rest.background, Color::WHITE.to_hsla());
    assert_eq!(empty.rest.border, Color::rgb(0xd9d9d9).to_hsla());
    assert_eq!(empty.hover.border, Color::rgb(0x1677ff).to_hsla());
    let checked = RadioStyle::resolve(&theme, true, false);
    assert_eq!(checked.rest.background, empty.hover.border);
    assert_eq!(checked.rest.border, checked.rest.background);
    assert_eq!(checked.rest.mark, Color::WHITE.to_hsla());
    assert_eq!(checked.hover.background, Color::rgb(0x4096ff).to_hsla());
    assert_eq!(checked.hover.border.a, 0.);
    for checked in [false, true] {
        let disabled = RadioStyle::resolve(&theme, checked, true);
        assert_eq!(disabled.rest, disabled.hover);
        assert_eq!(
            disabled.rest.background,
            Color::BLACK.with_alpha(0.04).to_hsla()
        );
        assert_eq!(disabled.rest.border, empty.rest.border);
        assert_eq!(disabled.rest.mark, Color::BLACK.with_alpha(0.25).to_hsla());
        assert_eq!(disabled.rest.label, disabled.rest.mark);
        assert_eq!(disabled.focus_width, 0.);
    }
}

#[test]
fn radio_button_geometry_and_all_interaction_states_match_reference() {
    let theme = ResolvedTheme::default();
    for (size, height, font, padding, radius) in [
        (Size::Small, 24., 14., 7., 4.),
        (Size::Medium, 32., 14., 15., 6.),
        (Size::Large, 40., 16., 15., 8.),
    ] {
        let style =
            RadioButtonVisualStyle::resolve(&theme, size, RadioButtonStyle::Outline, false, false);
        assert_eq!(style.height, height);
        assert_eq!(style.line_height, height - 2.);
        assert_eq!(style.font_size, font);
        assert_eq!(style.padding_inline, padding);
        assert_eq!(style.radius, radius);
        assert_eq!(style.rest.background, Color::WHITE.to_hsla());
        assert_eq!(style.rest.border, Color::rgb(0xd9d9d9).to_hsla());
        assert_eq!(style.hover.label, Color::rgb(0x1677ff).to_hsla());
        assert_eq!(style.hover.border, style.rest.border);
    }
    let outline = RadioButtonVisualStyle::resolve(
        &theme,
        Size::Medium,
        RadioButtonStyle::Outline,
        true,
        false,
    );
    assert_eq!(outline.rest.background, Color::WHITE.to_hsla());
    for (state, color) in [
        (outline.rest, Color::rgb(0x1677ff)),
        (outline.hover, Color::rgb(0x4096ff)),
        (outline.active, Color::rgb(0x0958d9)),
    ] {
        assert_eq!(state.label, color.to_hsla());
        assert_eq!(state.border, state.label);
        assert_eq!(state.background, outline.rest.background);
    }
    let solid =
        RadioButtonVisualStyle::resolve(&theme, Size::Medium, RadioButtonStyle::Solid, true, false);
    for (state, color) in [
        (solid.rest, Color::rgb(0x1677ff)),
        (solid.hover, Color::rgb(0x4096ff)),
        (solid.active, Color::rgb(0x0958d9)),
    ] {
        assert_eq!(state.background, color.to_hsla());
        assert_eq!(state.border, state.background);
        assert_eq!(state.label, Color::WHITE.to_hsla());
    }
    for checked in [false, true] {
        for appearance in [RadioButtonStyle::Outline, RadioButtonStyle::Solid] {
            let disabled =
                RadioButtonVisualStyle::resolve(&theme, Size::Medium, appearance, checked, true);
            assert_eq!(disabled.rest, disabled.hover);
            assert_eq!(disabled.rest, disabled.active);
            assert_eq!(
                disabled.rest.background,
                Color::BLACK
                    .with_alpha(if checked { 0.15 } else { 0.04 })
                    .to_hsla()
            );
            assert_eq!(disabled.rest.label, Color::BLACK.with_alpha(0.25).to_hsla());
            assert_eq!(disabled.rest.border, Color::rgb(0xd9d9d9).to_hsla());
            assert_eq!(disabled.focus_width, 0.);
        }
    }
}

#[test]
fn radio_component_patches_merge_and_apply_to_both_appearances() {
    let mut parent = ThemeConfig::default();
    parent.components.set::<RadioTheme>(RadioTokenPatch {
        radio_size: Some(20.),
        dot_size: Some(10.),
        dot_color_disabled: Some(Color::rgb(0x888888)),
        button_padding_inline: Some(21.),
        button_solid_checked_bg: Some(Color::rgb(0x52c41a)),
        ..Default::default()
    });
    let mut local = ThemeConfig::default();
    local.components.set::<RadioTheme>(RadioTokenPatch {
        radio_size: Some(24.),
        wrapper_margin_inline_end: Some(20.),
        button_checked_bg: Some(Color::rgb(0xd9f7be)),
        button_checked_bg_disabled: Some(Color::rgb(0xfffbe6)),
        button_checked_color_disabled: Some(Color::rgb(0xffe58f)),
        ..Default::default()
    });
    let theme = ThemeConfig::merged(&parent, &local).resolve();
    let tokens = theme.component::<RadioTheme>();
    assert!(Arc::ptr_eq(&tokens, &theme.component::<RadioTheme>()));
    assert_eq!(tokens.font(), &tokens.globals().font_family.to_gpui_font());
    let radio = RadioStyle::from_tokens(&tokens, true, true);
    assert_eq!(radio.size, 24.);
    assert_eq!(radio.dot_size, 10.);
    assert_eq!(radio.rest.mark, Color::rgb(0x888888).to_hsla());
    assert_eq!(radio.group_gap, 20.);
    assert_eq!(radio.vertical_gap, 8.);
    let button = RadioButtonVisualStyle::from_tokens(
        &tokens,
        Size::Medium,
        RadioButtonStyle::Outline,
        true,
        false,
    );
    assert_eq!(button.padding_inline, 21.);
    assert_eq!(button.rest.background, Color::rgb(0xd9f7be).to_hsla());
    let small = RadioButtonVisualStyle::from_tokens(
        &tokens,
        Size::Small,
        RadioButtonStyle::Solid,
        true,
        false,
    );
    assert_eq!(small.padding_inline, 7.);
    assert_eq!(small.rest.background, Color::rgb(0x52c41a).to_hsla());
    let disabled = RadioButtonVisualStyle::from_tokens(
        &tokens,
        Size::Large,
        RadioButtonStyle::Solid,
        true,
        true,
    );
    assert_eq!(disabled.rest.background, Color::rgb(0xfffbe6).to_hsla());
    assert_eq!(disabled.rest.label, Color::rgb(0xffe58f).to_hsla());
}

#[test]
fn radio_local_aliases_algorithms_and_wireframe_follow_the_reference() {
    for algorithms in [
        AlgorithmChain::dark(),
        AlgorithmChain::compact(),
        AlgorithmChain::dark_compact(),
    ] {
        let mut config = ThemeConfig::default();
        config.components.set_theme(
            ComponentThemeConfig::<RadioTheme>::default()
                .with_algorithms(algorithms)
                .with_tokens(TokenPatch {
                    color_bg_base: Some(Color::BLACK),
                    font_size: Some(19.),
                    padding_xs: Some(11.),
                    focus_outline: Some(false),
                    wireframe: Some(true),
                    ..Default::default()
                }),
        );
        let theme = config.resolve();
        let tokens = theme.component::<RadioTheme>();
        let globals = tokens.globals();
        let style = RadioStyle::from_tokens(&tokens, true, false);
        assert_eq!(style.size, globals.font_size_lg);
        assert_eq!(style.dot_size, style.size - 8.);
        assert_eq!(style.rest.background, globals.color_bg_container.to_hsla());
        assert_eq!(style.rest.mark, globals.color_primary.to_hsla());
        assert_eq!(
            style.hover.background,
            globals.color_primary_hover.to_hsla()
        );
        assert_eq!(style.hover.border.a, 0.);
        assert_eq!(style.label_padding, 11.);
        assert_eq!(style.focus_width, 0.);
        assert_eq!(theme.tokens.font_size, 14.);
        let button = RadioButtonVisualStyle::from_tokens(
            &tokens,
            Size::Large,
            RadioButtonStyle::Solid,
            true,
            false,
        );
        assert_eq!(button.height, globals.control_height_lg);
        assert_eq!(button.rest.background, globals.color_primary.to_hsla());
        assert_eq!(button.font_size, globals.font_size_lg);
        assert_eq!(button.focus_width, 0.);
    }
}

#[test]
fn invalid_component_dimensions_never_reach_native_paint_geometry() {
    let mut config = ThemeConfig::default();
    config.components.set::<RadioTheme>(RadioTokenPatch {
        radio_size: Some(f64::NAN),
        dot_size: Some(-2.),
        button_padding_inline: Some(f64::INFINITY),
        wrapper_margin_inline_end: Some(f64::NEG_INFINITY),
        ..Default::default()
    });
    let theme = config.resolve();
    let radio = RadioStyle::resolve(&theme, true, false);
    assert_eq!(radio.size, 0.);
    assert_eq!(radio.dot_size, 0.);
    assert_eq!(radio.group_gap, 0.);
    let button =
        RadioButtonVisualStyle::resolve(&theme, Size::Medium, RadioButtonStyle::Solid, true, false);
    assert_eq!(button.padding_inline, 0.);
}
