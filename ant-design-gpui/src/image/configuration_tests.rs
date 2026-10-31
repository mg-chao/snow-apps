// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
use super::*;
use crate::{
    icons::{IconColors, IconName},
    locale::{ImageMessagePatch, Locale, LocalePatch},
    theme::{Color, ComponentTheme, ThemeConfig},
};
use gpui::{Styled, px};
use std::sync::Arc;

#[test]
fn reference_tokens_and_component_patch_preserve_unspecified_values() {
    let global = ThemeConfig::default().resolve();
    let tokens = global.component::<ImageTheme>();
    assert_eq!(tokens.z_index_popup, 1080);
    assert_eq!(tokens.preview_operation_size, 18.);
    assert_eq!(
        tokens.preview_operation_color,
        Color::WHITE.with_alpha(0.65)
    );
    assert_eq!(tokens.preview_operation_hover_color.a, 0.85);
    assert_eq!(tokens.preview_operation_color_disabled.a, 0.25);
    let mut patched = (*tokens).clone();
    let patch = ImageTheme::merge(
        &ImageTokenPatch {
            preview_operation_size: Some(24.),
            ..Default::default()
        },
        &ImageTokenPatch {
            z_index_popup: Some(2000),
            ..Default::default()
        },
    );
    ImageTheme::apply(&mut patched, &patch);
    assert_eq!(patched.z_index_popup, 2000);
    assert_eq!(patched.preview_operation_size, 24.);
    assert_eq!(
        patched.preview_operation_color,
        tokens.preview_operation_color
    );
}
#[test]
fn scoped_image_patch_overrides_false_and_refines_styles_per_field() {
    let parent = ImageConfig {
        styles: ImageStyles {
            root: gpui::StyleRefinement::default().p(px(12.)),
            ..Default::default()
        },
        ..Default::default()
    };
    let result = ImageConfigPatch {
        mask: Some(false),
        wheel: Some(false),
        styles: ImageStyles {
            root: gpui::StyleRefinement::default().rounded(px(8.)),
            ..Default::default()
        },
        ..Default::default()
    }
    .resolve(&parent);
    assert!(!result.mask);
    assert!(!result.wheel);
    assert!(result.focus_trap);
    assert_eq!(result.styles.root.padding, parent.styles.root.padding);
    assert_ne!(
        result.styles.root.corner_radii,
        parent.styles.root.corner_radii
    );
    assert_eq!(ImageConfigPatch::default().resolve(&result), result);
}
#[test]
fn image_catalogs_are_complete_and_partial_overrides_reuse_catalogs() {
    for locale in [Locale::en_us(), Locale::zh_cn(), Locale::zh_tw()] {
        let m = &locale.image;
        for text in [
            &m.root_label,
            &m.preview,
            &m.loading,
            &m.load_error,
            &m.previous,
            &m.next,
            &m.flip_y,
            &m.flip_x,
            &m.rotate_left,
            &m.rotate_right,
            &m.zoom_out,
            &m.zoom_in,
            &m.reset,
        ] {
            assert!(!text.is_empty());
        }
        assert!(m.count.contains("{current}") && m.count.contains("{total}"));
    }
    let english = Locale::en_us();
    let identity = LocalePatch {
        image: ImageMessagePatch {
            preview: Some("Preview".into()),
            ..Default::default()
        },
        ..Default::default()
    };
    assert!(Arc::ptr_eq(&english, &identity.resolve(&english)));
    let patch = LocalePatch {
        image: ImageMessagePatch {
            preview: Some("Inspect photo".into()),
            ..Default::default()
        },
        ..Default::default()
    };
    let result = patch.resolve(&Locale::zh_tw());
    assert_eq!(result.image.preview.as_ref(), "Inspect photo");
    assert_eq!(result.image.zoom_in, Locale::zh_tw().image.zoom_in);
    assert!(Arc::ptr_eq(
        &result,
        &patch.resolve_with_previous(&Locale::zh_tw(), Some(&result))
    ));
}
#[test]
fn retained_icon_equality_preserves_palette_changes() {
    let a: crate::icons::IconRef = IconName::CloseOutlined.into();
    assert_eq!(a, a.clone());
    assert_ne!(
        a,
        a.clone()
            .with_colors(IconColors::primary(gpui::rgb(0xff0000)))
    );
    let options = PreviewOptions::default();
    assert!(options == options.clone());
    assert!(
        options
            != PreviewOptions {
                close_icon: IconName::DeleteOutlined.into(),
                ..Default::default()
            }
    );
}

#[test]
fn component_global_overrides_stay_local_to_image() {
    let mut config = ThemeConfig::default();
    config.components.set_theme::<ImageTheme>(
        crate::theme::ComponentThemeConfig::new(ImageTokenPatch::default()).with_tokens(
            crate::theme::TokenPatch {
                padding_sm: Some(4.),
                font_size: Some(20.),
                motion: Some(false),
                ..Default::default()
            },
        ),
    );
    let theme = config.resolve();
    let image = theme.component::<ImageTheme>();
    assert_eq!(image.globals().padding_sm, 4.);
    assert_eq!(image.globals().font_size, 20.);
    assert!(!image.globals().motion);
    assert_eq!(theme.tokens.padding_sm, 12.);
    assert_eq!(theme.tokens.font_size, 14.);
    assert!(theme.tokens.motion);
}
