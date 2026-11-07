// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;
use crate::theme::{AlgorithmChain, ComponentThemeConfig, ThemeConfig, TokenPatch};

#[test]
fn menu_default_geometry_and_colors_match_ant_design() {
    let theme = ThemeConfig::default().resolve();
    let t = theme.component::<MenuTheme>();
    assert_eq!(t.item_height, 40.);
    assert_eq!(t.horizontal_line_height, 46.);
    assert_eq!(t.collapsed_width, 80.);
    assert_eq!(t.dropdown_width, 160.);
    assert_eq!(t.item_margin_inline, 4.);
    assert_eq!(t.item_margin_block, 4.);
    assert_eq!(t.item_padding_inline, 16.);
    assert_eq!(t.icon_size, 14.);
    assert_eq!(t.collapsed_icon_size, 16.);
    assert_eq!(t.icon_margin_inline_end, 10.);
    assert_eq!(t.item_border_radius, 8.);
    assert_eq!(t.sub_menu_item_border_radius, 4.);
    assert_eq!(t.popup_radius(), 8.);
    assert_eq!(t.popup_gap(), 8.);
    assert_eq!(t.popup_viewport_inset(), 100.);
    assert_eq!(t.arrow_size(), 10.);
    assert_eq!(t.active_bar_height, 2.);
    assert_eq!(t.active_bar_width, 0.);
    assert_eq!(t.active_bar_border_width, 1.);
    assert_eq!(t.z_index_popup, 1050);
    assert_eq!(t.group_title_font_size * t.group_title_line_height, 22.);
    let light = t.colors(MenuColorTheme::Light);
    assert_eq!(light.bg, Color::WHITE);
    assert_eq!(light.hover_bg, Color::BLACK.with_alpha(0.06));
    assert_eq!(light.submenu_bg, Color::BLACK.with_alpha(0.02));
    assert_eq!(light.selected_text, Color::rgb(0x1677ff));
    assert_eq!(light.selected_bg, Color::rgb(0xe6f4ff));
    assert_eq!(light.danger_selected_bg, Color::rgb(0xfff2f0));
    assert_eq!(t.popup_shadow().len(), 3);
    assert_eq!(t.popup_shadows().len(), 3);
    assert_eq!(t.popup_shadow()[1].spread, -4.);
}

#[test]
fn menu_dark_appearance_is_distinct_from_global_dark_algorithm() {
    for algorithms in [AlgorithmChain::default(), AlgorithmChain::dark()] {
        let theme = ThemeConfig {
            algorithms: Some(algorithms),
            ..Default::default()
        }
        .resolve();
        let t = theme.component::<MenuTheme>();
        let dark = t.colors(MenuColorTheme::Dark);
        assert_eq!(dark.bg, Color::rgb(0x001529));
        assert_eq!(dark.popup_bg, dark.bg);
        assert_eq!(dark.submenu_bg, Color::rgb(0x000c17));
        assert_eq!(dark.text, Color::WHITE.with_alpha(0.65));
        assert_eq!(dark.hover_text, Color::WHITE);
        assert_eq!(dark.selected_text, Color::WHITE);
        assert_eq!(dark.disabled_text, Color::WHITE.with_alpha(0.25));
        assert_eq!(dark.active_bg, Color::TRANSPARENT);
        assert_eq!(dark.active_bar_height, 0.);
        assert_eq!(dark.active_bar_border_width, 0.);
        assert_eq!(dark.selected_bg, t.globals().color_primary);
        assert_eq!(dark.danger_active_bg, t.globals().color_error);
    }
}

#[test]
fn menu_component_overrides_preserve_unset_fields_and_effective_aliases() {
    let parent = MenuTokenPatch {
        item_height: Some(48.),
        item_selected_bg: Some(Color::rgb(0xfff7e6)),
        ..Default::default()
    };
    let local = MenuTokenPatch {
        item_height: Some(44.),
        dark_popup_bg: Some(Color::rgb(0x141414)),
        ..Default::default()
    };
    let merged = MenuTheme::merge(&parent, &local);
    assert_eq!(merged.item_height, Some(44.));
    assert_eq!(merged.item_selected_bg, parent.item_selected_bg);
    let mut config = ThemeConfig::default();
    config
        .components
        .set_theme(
            ComponentThemeConfig::<MenuTheme>::new(merged).with_tokens(TokenPatch {
                font_size: Some(16.),
                line_width_focus: Some(4.),
                box_shadow_secondary: Some(Arc::from([])),
                ..Default::default()
            }),
        );
    let theme = config.resolve();
    let t = theme.component::<MenuTheme>();
    assert_eq!(t.item_height, 44.);
    assert_eq!(t.icon_size, 16.);
    assert_eq!(t.arrow_size(), 16. / 7. * 5.);
    assert!(t.popup_shadows().is_empty());
    assert_eq!(t.colors(MenuColorTheme::Light).focus_width, 4.);
    assert_eq!(
        t.colors(MenuColorTheme::Dark).popup_bg,
        Color::rgb(0x141414)
    );
}

#[test]
fn menu_compact_metrics_follow_component_mapping_algorithm() {
    let mut config = ThemeConfig::default();
    config.components.set_theme(
        ComponentThemeConfig::<MenuTheme>::default().with_algorithms(AlgorithmChain::compact()),
    );
    let theme = config.resolve();
    let t = theme.component::<MenuTheme>();
    assert_eq!(t.item_height, 35.);
    assert_eq!(t.collapsed_width, 70.);
    assert_eq!(t.icon_size, 12.);
    assert_eq!(t.group_title_font_size, 12.);
    assert_eq!(theme.tokens.font_size, 14.);
}

#[test]
fn menu_invalid_lengths_fall_back_and_zero_is_a_valid_override() {
    let theme = ThemeConfig::default().resolve();
    let mut t = (*theme.component::<MenuTheme>()).clone();
    MenuTheme::apply(
        &mut t,
        &MenuTokenPatch {
            item_height: Some(f64::NAN),
            dropdown_width: Some(f64::INFINITY),
            collapsed_width: Some(f64::MAX),
            icon_size: Some(-1.),
            item_margin_block: Some(0.),
            ..Default::default()
        },
    );
    assert_eq!(t.item_height, 40.);
    assert_eq!(t.dropdown_width, 160.);
    assert_eq!(t.collapsed_width, 80.);
    assert_eq!(t.icon_size, 14.);
    assert_eq!(t.item_margin_block, 0.);
}
