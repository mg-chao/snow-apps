// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;
use crate::theme::{AlgorithmChain, ComponentThemeConfig, FontStack, ThemeConfig, TokenPatch};

fn resolve(options: SegmentedStyleOptions) -> SegmentedStyle {
    SegmentedStyle::resolve(&ResolvedTheme::default(), options)
}

#[test]
fn segmented_style_matches_ant_design_track_and_item_states() {
    let style = resolve(Default::default());
    assert_eq!(style.track_background, Color::rgb(0xf5f5f5));
    assert_eq!(style.rest.background, Color::TRANSPARENT);
    assert_eq!(style.rest.text, Color::BLACK.with_alpha(0.65));
    assert_eq!(style.hover.background, Color::BLACK.with_alpha(0.06));
    assert_eq!(style.hover.text, Color::BLACK.with_alpha(0.88));
    assert_eq!(style.active.background, Color::BLACK.with_alpha(0.15));
    assert_eq!(style.active.text, style.hover.text);
    assert_eq!(style.selected.background, Color::WHITE);
    assert_eq!(style.selected.text, style.hover.text);
    assert_eq!(style.selected_shadow.len(), 3);
    assert_eq!(style.selected_shadow[0].y, 1.);
    assert_eq!(style.selected_shadow[0].blur, 2.);
    assert_eq!(
        style.selected_shadow[0].color,
        Color::BLACK.with_alpha(0.05)
    );
    assert_eq!(style.focus_color, Color::rgb(0x91caff));
    assert_eq!(style.focus_width, 3.);
    assert_eq!(style.focus_offset, 1.);
}

#[test]
fn segmented_style_sizes_and_round_shape_match_ant_design() {
    for (size, height, item_height, font_size, padding, track_radius, item_radius) in [
        (Size::Small, 24., 20., 14., 7., 4., 2.),
        (Size::Medium, 32., 28., 14., 11., 6., 4.),
        (Size::Large, 40., 36., 16., 11., 8., 6.),
    ] {
        let options = SegmentedStyleOptions {
            size,
            ..Default::default()
        };
        let metrics = resolve(options).metrics;
        assert_eq!(metrics.height, height);
        assert_eq!(metrics.item_height, item_height);
        assert_eq!(metrics.line_height, item_height);
        assert_eq!(metrics.font_size, font_size);
        assert_eq!(metrics.icon_size, font_size);
        assert_eq!(metrics.icon_gap, 6.);
        assert_eq!(metrics.track_padding, 2.);
        assert_eq!(metrics.padding_inline, padding);
        assert_eq!(metrics.track_radius, track_radius);
        assert_eq!(metrics.item_radius, item_radius);
        let round = resolve(SegmentedStyleOptions {
            shape: SegmentedShape::Round,
            ..options
        });
        assert_eq!(round.metrics.track_radius, 9999.);
        assert_eq!(round.metrics.item_radius, 9999.);
        assert_eq!(round.metrics.padding_inline, metrics.padding_inline);
        assert_eq!(round.metrics.height, metrics.height);
    }
}

#[test]
fn segmented_style_dark_and_compact_follow_effective_algorithms() {
    for (algorithms, dark, compact) in [
        (AlgorithmChain::default(), false, false),
        (AlgorithmChain::dark(), true, false),
        (AlgorithmChain::compact(), false, true),
        (AlgorithmChain::dark_compact(), true, true),
    ] {
        let theme = ThemeConfig {
            algorithms: Some(algorithms),
            ..Default::default()
        }
        .resolve();
        for (size, height, item_height, font_size) in if compact {
            [
                (Size::Small, 21., 17., 12.),
                (Size::Medium, 28., 24., 12.),
                (Size::Large, 35., 31., 14.),
            ]
        } else {
            [
                (Size::Small, 24., 20., 14.),
                (Size::Medium, 32., 28., 14.),
                (Size::Large, 40., 36., 16.),
            ]
        } {
            let style = SegmentedStyle::resolve(
                &theme,
                SegmentedStyleOptions {
                    size,
                    ..Default::default()
                },
            );
            assert_eq!(style.metrics.height, height);
            assert_eq!(style.metrics.item_height, item_height);
            assert_eq!(style.metrics.font_size, font_size);
            assert_eq!(
                style.track_background,
                Color::rgb(if dark { 0x000000 } else { 0xf5f5f5 })
            );
            assert_eq!(
                style.selected.background,
                Color::rgb(if dark { 0x1f1f1f } else { 0xffffff })
            );
            assert_eq!(
                style.rest.text,
                if dark {
                    Color::WHITE.with_alpha(0.65)
                } else {
                    Color::BLACK.with_alpha(0.65)
                }
            );
        }
    }
}

#[test]
fn segmented_style_disabled_preserves_selected_surface_without_hover() {
    let enabled = resolve(Default::default());
    let disabled = resolve(SegmentedStyleOptions {
        disabled: true,
        ..Default::default()
    });
    assert_eq!(disabled.rest, disabled.hover);
    assert_eq!(disabled.rest, disabled.active);
    assert_eq!(disabled.rest.text, Color::BLACK.with_alpha(0.25));
    assert_eq!(disabled.selected.text, disabled.rest.text);
    assert_eq!(disabled.selected.background, enabled.selected.background);
    assert_eq!(disabled.selected_shadow, enabled.selected_shadow);
    assert_eq!(disabled.track_background, enabled.track_background);
    assert_eq!(disabled.focus_width, 0.);
}

#[test]
fn segmented_style_component_overrides_inherit_and_cache_effective_globals() {
    let mut parent = ThemeConfig::default();
    parent
        .components
        .set::<SegmentedTheme>(SegmentedTokenPatch {
            track_padding: Some(3.),
            item_hover_color: Some(Color::rgb(0x123456)),
            ..Default::default()
        });
    let mut local = ThemeConfig::default();
    let font_family = FontStack::new(["Consolas"]);
    local.components.set_theme(
        ComponentThemeConfig::<SegmentedTheme>::new(SegmentedTokenPatch {
            track_bg: Some(Color::rgb(0xf0f8ff)),
            item_hover_color: Some(Color::rgb(0x654321)),
            ..Default::default()
        })
        .with_tokens(TokenPatch {
            control_height: Some(38.),
            control_padding_horizontal: Some(18.),
            font_family: Some(font_family.clone()),
            line_width_focus: Some(4.),
            ..Default::default()
        }),
    );
    let theme = ThemeConfig::merged(&parent, &local).resolve();
    let tokens = theme.component::<SegmentedTheme>();
    assert!(Arc::ptr_eq(&tokens, &theme.component::<SegmentedTheme>()));
    assert_eq!(tokens.font(), &font_family.to_gpui_font());
    assert_eq!(tokens.globals().control_height, 38.);
    let style = SegmentedStyle::from_tokens(&tokens, Default::default());
    assert_eq!(style.metrics.height, 38.);
    assert_eq!(style.metrics.item_height, 32.);
    assert_eq!(style.metrics.track_padding, 3.);
    assert_eq!(style.metrics.padding_inline, 17.);
    assert_eq!(style.track_background, Color::rgb(0xf0f8ff));
    assert_eq!(style.hover.text, Color::rgb(0x654321));
    assert_eq!(style.focus_width, 4.);
    assert_eq!(theme.tokens.control_height, 32.);
    assert_ne!(theme.tokens.font_family, font_family);
}

#[test]
fn segmented_style_component_algorithms_control_geometry_and_state_palette() {
    let mut config = ThemeConfig::default();
    config.components.set_theme(
        ComponentThemeConfig::<SegmentedTheme>::default()
            .with_algorithms(AlgorithmChain::dark_compact()),
    );
    let theme = config.resolve();
    let style = SegmentedStyle::resolve(&theme, Default::default());
    assert_eq!(style.metrics.height, 28.);
    assert_eq!(style.metrics.item_height, 24.);
    assert_eq!(style.metrics.font_size, 12.);
    // Component algorithms retain the parent's explicitly resolved text base.
    assert_eq!(style.rest.text, Color::BLACK.with_alpha(0.65));
    assert_eq!(theme.tokens.control_height, 32.);
}

#[test]
fn segmented_style_caches_paint_resources_from_component_effective_aliases() {
    use crate::theme::Shadow;

    let primary = Color::rgb(0x722ed1);
    let shadow = Shadow {
        x: -1.,
        y: 3.,
        blur: 5.,
        spread: -2.,
        color: primary.with_alpha(0.2),
        inset: true,
    };
    let mut config = ThemeConfig::default();
    config.components.set_theme(
        ComponentThemeConfig::<SegmentedTheme>::default().with_tokens(TokenPatch {
            color_primary: Some(primary),
            box_shadow_tertiary: Some(vec![shadow].into()),
            ..Default::default()
        }),
    );
    let theme = config.resolve();
    let tokens = theme.component::<SegmentedTheme>();
    let cached = theme.component::<SegmentedTheme>();
    assert!(Arc::ptr_eq(tokens.shadows(), cached.shadows()));
    assert_eq!(tokens.icon_palette().primary, primary.to_rgba());
    assert_eq!(
        tokens.icon_palette().secondary,
        crate::theme::generate_palette(primary, false)[0].to_rgba()
    );
    let native_shadow = &tokens.shadows()[0];
    assert_eq!(native_shadow.inset, shadow.inset);
    assert_eq!(native_shadow.color, shadow.color.to_hsla());
    assert_eq!(
        native_shadow.offset,
        gpui::point(gpui::px(-1.), gpui::px(3.))
    );
    assert_eq!(native_shadow.blur_radius, gpui::px(5.));
    assert_eq!(native_shadow.spread_radius, gpui::px(-2.));
    assert_ne!(theme.tokens.color_primary, primary);
}
