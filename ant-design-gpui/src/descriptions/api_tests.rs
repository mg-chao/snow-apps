// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::sync::Arc;

use gpui::{ParentElement, StyleRefinement, Styled, TestAppContext, div, px, rgb};

use super::*;
use crate::{
    ConfigPatch,
    theme::{Color, ComponentTheme, ThemeConfig},
};

#[test]
fn responsive_columns_follow_boundaries_and_default_fallback() {
    let theme = ThemeConfig::default().resolve();
    let g = &theme.tokens;
    let columns = DescriptionsColumns::default();
    for (width, expected) in [
        (0., 1),
        (g.screen_xs_max, 1),
        (g.screen_sm, 2),
        (g.screen_md - 0.01, 2),
        (g.screen_md, 3),
        (g.screen_xxl, 3),
        (g.screen_xxxl - 0.01, 3),
        (g.screen_xxxl, 4),
    ] {
        assert_eq!(columns.resolve(width, g), expected, "viewport {width}");
    }
    let sparse = DescriptionsColumns::Responsive(DescriptionsBreakpoints {
        md: Some(5),
        ..Default::default()
    });
    assert_eq!(sparse.resolve(g.screen_sm, g), 2);
    assert_eq!(sparse.resolve(g.screen_md, g), 5);
    assert_eq!(sparse.resolve(g.screen_xxxl, g), 5);
    assert_eq!(DescriptionsColumns::Fixed(0).resolve(800., g), 1);
    let zero = DescriptionsColumns::Responsive(DescriptionsBreakpoints {
        md: Some(0),
        ..Default::default()
    });
    assert_eq!(zero.resolve(g.screen_md, g), 1);
}

#[test]
fn responsive_spans_cascade_without_carrying_xs_to_larger_viewports() {
    let theme = ThemeConfig::default().resolve();
    let g = &theme.tokens;
    let values = DescriptionsBreakpoints {
        xs: Some(2),
        md: Some(3),
        xl: Some(4),
        ..Default::default()
    };
    assert_eq!(values.resolve(g.screen_xs_max, g), Some(2));
    assert_eq!(values.resolve(g.screen_sm, g), None);
    assert_eq!(values.resolve(g.screen_lg, g), Some(3));
    assert_eq!(values.resolve(g.screen_xxxl, g), Some(4));
    let span = DescriptionsSpan::Responsive(values);
    assert_eq!(span.resolve(g.screen_sm, g), Some(1));
    assert_eq!(DescriptionsSpan::Fixed(0).resolve(800., g), Some(1));
    assert_eq!(DescriptionsSpan::Filled.resolve(800., g), None);
}

#[test]
fn responsive_resolution_uses_overridden_alias_breakpoints() {
    let theme = ThemeConfig::default().resolve();
    let mut g = (*theme.tokens).clone();
    g.screen_xs_max = 399.;
    g.screen_sm = 400.;
    g.screen_md = 600.;
    assert_eq!(DescriptionsColumns::default().resolve(399., &g), 1);
    assert_eq!(DescriptionsColumns::default().resolve(400., &g), 2);
    assert_eq!(DescriptionsColumns::default().resolve(600., &g), 3);
}

#[test]
fn tokens_match_reference_defaults_and_size_specific_overrides() {
    let theme = ThemeConfig::default().resolve();
    let t = theme.component::<DescriptionsTheme>();
    assert_eq!(t.item_padding_bottom, 16.);
    assert_eq!(t.item_padding_end, 16.);
    assert!((t.title_margin_bottom - 20.).abs() < 0.00001);
    assert_eq!(t.colon_margin_left, 2.);
    assert_eq!(t.colon_margin_right, 8.);
    assert_eq!(t.label_bg, theme.tokens.color_fill_alter);
    assert_eq!(t.label_color, theme.tokens.color_text_tertiary);
    for (size, vertical, horizontal) in [
        (Size::Large, 16., 24.),
        (Size::Medium, 12., 24.),
        (Size::Small, 8., 16.),
    ] {
        let plain = DescriptionsStyle::from_tokens(&t, size, false);
        let bordered = DescriptionsStyle::from_tokens(&t, size, true);
        assert_eq!(plain.item_padding_bottom, vertical);
        assert_eq!(bordered.cell_padding_vertical, vertical);
        assert_eq!(bordered.cell_padding_horizontal, horizontal);
        assert_eq!(plain.label_color, t.label_color.into());
        assert_eq!(
            bordered.label_color,
            theme.tokens.color_text_secondary.into()
        );
    }
}

#[test]
fn component_patches_merge_inherit_and_keep_size_rules() {
    let mut parent = ThemeConfig::default();
    parent
        .components
        .set::<DescriptionsTheme>(DescriptionsTokenPatch {
            label_bg: Some(Color::rgb(0x123456)),
            label_color: Some(Color::rgb(0x654321)),
            item_padding_bottom: Some(27.),
            item_padding_end: Some(29.),
            ..Default::default()
        });
    let mut local = ThemeConfig::default();
    local
        .components
        .set::<DescriptionsTheme>(DescriptionsTokenPatch {
            label_bg: Some(Color::rgb(0xabcdef)),
            title_margin_bottom: Some(0.),
            ..Default::default()
        });
    let theme = ThemeConfig::merged(&parent, &local).resolve();
    let tokens = theme.component::<DescriptionsTheme>();
    assert_eq!(tokens.label_bg, Color::rgb(0xabcdef));
    assert_eq!(tokens.label_color, Color::rgb(0x654321));
    assert_eq!(tokens.item_padding_end, 29.);
    assert_eq!(tokens.title_margin_bottom, 0.);
    assert_eq!(
        DescriptionsStyle::from_tokens(&tokens, Size::Large, false).item_padding_bottom,
        27.
    );
    assert_eq!(
        DescriptionsStyle::from_tokens(&tokens, Size::Medium, false).item_padding_bottom,
        12.
    );
    assert_eq!(
        DescriptionsStyle::from_tokens(&tokens, Size::Large, true).cell_padding_vertical,
        16.
    );
}

#[test]
fn invalid_component_lengths_do_not_enter_native_geometry() {
    let globals = ThemeConfig::default().resolve();
    let mut tokens = DescriptionsTheme::default_tokens(&globals.tokens);
    DescriptionsTheme::apply(
        &mut tokens,
        &DescriptionsTokenPatch {
            title_margin_bottom: Some(f64::NAN),
            item_padding_bottom: Some(f64::INFINITY),
            item_padding_end: Some(-1.),
            colon_margin_left: Some(f64::MAX),
            colon_margin_right: Some(0.),
            ..Default::default()
        },
    );
    assert!((tokens.title_margin_bottom - 20.).abs() < 0.00001);
    assert_eq!(tokens.item_padding_bottom, 16.);
    assert_eq!(tokens.item_padding_end, 16.);
    assert_eq!(tokens.colon_margin_left, 2.);
    assert_eq!(tokens.colon_margin_right, 0.);
    DescriptionsTheme::apply(
        &mut tokens,
        &DescriptionsTokenPatch {
            title_margin_bottom: Some(-2.),
            colon_margin_left: Some(-1.),
            ..Default::default()
        },
    );
    assert_eq!(tokens.title_margin_bottom, -2.);
    assert_eq!(tokens.colon_margin_left, -1.);
}

#[test]
fn semantic_style_patches_merge_properties_and_preserve_resolver_identity() {
    let resolver = DescriptionsStyleResolver::new(|_, _, _| DescriptionsStyles::default());
    let parent = DescriptionsConfig {
        styles: DescriptionsStyles {
            label: StyleRefinement::default().bg(rgb(0x123456)).px(px(12.)),
            title: StyleRefinement::default().text_color(rgb(0x234567)),
            ..Default::default()
        },
        styles_fn: Some(resolver.clone()),
    };
    let local = DescriptionsConfigPatch {
        styles: DescriptionsStyles {
            label: StyleRefinement::default().bg(rgb(0xabcdef)),
            ..Default::default()
        },
        ..Default::default()
    };
    let merged = local.resolve(&parent);
    assert_eq!(
        merged.styles.label.background,
        StyleRefinement::default().bg(rgb(0xabcdef)).background
    );
    assert_eq!(merged.styles.label.padding, parent.styles.label.padding);
    assert_eq!(merged.styles.title, parent.styles.title);
    assert_eq!(merged.styles_fn, Some(resolver));
}

#[gpui::test]
fn scoped_resolver_no_op_updates_preserve_snapshot_and_theme(cx: &mut TestAppContext) {
    let resolver = DescriptionsStyleResolver::new(|_, _, _| DescriptionsStyles::default());
    let (scope, child) = cx.update(|cx| {
        let scope = ConfigScope::new_window(cx);
        scope
            .update_patch(cx, |patch| {
                patch.descriptions.styles_fn = Some(resolver.clone());
                patch.descriptions.styles.title = StyleRefinement::default().mt(px(10.));
            })
            .unwrap();
        let child = scope.child(ConfigPatch::default(), cx).unwrap();
        (scope, child)
    });
    cx.update(|cx| {
        let before = scope.snapshot(cx);
        assert!(
            !scope
                .update_patch(cx, |patch| {
                    patch.descriptions.styles_fn = Some(resolver.clone());
                })
                .unwrap()
        );
        let after = scope.snapshot(cx);
        assert!(Arc::ptr_eq(&before, &after));
        assert_eq!(
            child.snapshot(cx).descriptions.styles_fn,
            Some(resolver.clone())
        );
        let replacement = DescriptionsStyleResolver::new(|_, _, _| DescriptionsStyles::default());
        assert!(
            scope
                .update_patch(cx, |patch| {
                    patch.descriptions.styles_fn = Some(replacement);
                })
                .unwrap()
        );
        assert!(Arc::ptr_eq(&before.theme, &scope.snapshot(cx).theme));
    });
}

#[gpui::test]
fn unset_provider_size_preserves_large_default_and_explicit_medium(cx: &mut TestAppContext) {
    let (scope, child) = cx.update(|cx| {
        let scope = ConfigScope::new_window(cx);
        let child = scope.child(ConfigPatch::default(), cx).unwrap();
        assert_eq!(
            child.snapshot(cx).configured_size().unwrap_or(Size::Large),
            Size::Large
        );
        scope
            .update_patch(cx, |patch| patch.size = Some(Size::Medium))
            .unwrap();
        (scope, child)
    });
    cx.update(|cx| {
        assert_eq!(child.snapshot(cx).configured_size(), Some(Size::Medium));
        scope.update_patch(cx, |patch| patch.size = None).unwrap();
    });
    cx.update(|cx| {
        assert_eq!(
            child.snapshot(cx).configured_size().unwrap_or(Size::Large),
            Size::Large
        );
    });
}

#[test]
fn builder_normalizes_primitive_empty_content_and_retains_rich_slots() {
    let details = Descriptions::new("details")
        .columns(3)
        .title_content("")
        .extra(" ")
        .item(DescriptionItem::new("first").label("0").content(""))
        .item(
            DescriptionItem::new("second")
                .label_content("")
                .content_element(div())
                .span(2),
        );
    assert!(details.title.is_none());
    assert!(details.extra.is_some());
    assert_eq!(details.items.len(), 2);
    assert!(details.items[0].label.is_some());
    assert!(details.items[0].content.is_none());
    assert!(details.items[1].label.is_none());
    assert!(details.items[1].content.is_some());
}

#[test]
fn item_accessible_names_use_explicit_then_plain_label_without_flattening_values() {
    let plain = DescriptionItem::new("plain")
        .label("Department")
        .content("Research");
    assert_eq!(
        plain.resolved_accessibility_label().as_deref(),
        Some("Department")
    );
    let overridden = DescriptionItem::new("override")
        .label("Dept.")
        .accessibility_label("Department")
        .content("Research");
    assert_eq!(
        overridden.resolved_accessibility_label().as_deref(),
        Some("Department")
    );
    let rich = DescriptionItem::new("rich")
        .label_content(div().child("Custom label"))
        .accessibility_label("Account alias")
        .content_element(div().child("Value"));
    assert_eq!(
        rich.resolved_accessibility_label().as_deref(),
        Some("Account alias")
    );
    let unnamed_rich = DescriptionItem::new("unnamed")
        .label_content(div().child("Custom label"))
        .content("Never flatten this value into the name");
    assert_eq!(unnamed_rich.resolved_accessibility_label(), None);
}
