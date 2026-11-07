// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Token formulas adapted from Ant Design 6.6.7 (MIT).

//! Cached component tokens and allocation-free native Alert styling.

use std::{sync::Arc, time::Duration};

use gpui::{Font, Hsla};

use super::{AlertType, AlertVariant};
use crate::{
    icons::IconPalette,
    theme::{AliasTokens, Color, ComponentTheme, CubicBezier, ResolvedTheme},
};

/// Physical top/right/bottom/left padding, in logical pixels.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct AlertPadding {
    pub top: f64,
    pub right: f64,
    pub bottom: f64,
    pub left: f64,
}

impl AlertPadding {
    pub const fn new(top: f64, right: f64, bottom: f64, left: f64) -> Self {
        Self {
            top,
            right,
            bottom,
            left,
        }
    }

    pub const fn symmetric(vertical: f64, horizontal: f64) -> Self {
        Self::new(vertical, horizontal, vertical, horizontal)
    }

    pub const fn all(value: f64) -> Self {
        Self::symmetric(value, value)
    }
}

impl From<f64> for AlertPadding {
    fn from(value: f64) -> Self {
        Self::all(value)
    }
}

/// Marker for scoped Alert token overrides and component algorithms.
pub struct AlertTheme;

#[derive(Clone, Debug, PartialEq)]
pub struct AlertTokens {
    pub radius: f64,
    pub default_padding: AlertPadding,
    pub with_description_padding: AlertPadding,
    pub with_description_icon_size: f64,
    globals: Arc<AliasTokens>,
    font: Font,
    icon_palette: IconPalette,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct AlertTokenPatch {
    pub radius: Option<f64>,
    pub default_padding: Option<AlertPadding>,
    pub with_description_padding: Option<AlertPadding>,
    pub with_description_icon_size: Option<f64>,
}

impl ComponentTheme for AlertTheme {
    type Tokens = AlertTokens;
    type Patch = AlertTokenPatch;

    fn default_tokens(global: &AliasTokens) -> Self::Tokens {
        AlertTokens {
            radius: global.border_radius_lg,
            // The upstream horizontal default is deliberately a fixed 12px.
            default_padding: AlertPadding::symmetric(global.padding_content_vertical_sm, 12.),
            with_description_padding: AlertPadding::symmetric(
                global.padding_md,
                global.padding_content_horizontal_lg,
            ),
            with_description_icon_size: global.font_size_heading3,
            globals: Arc::new(global.clone()),
            font: global.font_family.to_gpui_font(),
            icon_palette: IconPalette::from_theme(global),
        }
    }

    fn merge(parent: &Self::Patch, local: &Self::Patch) -> Self::Patch {
        AlertTokenPatch {
            radius: local.radius.or(parent.radius),
            default_padding: local.default_padding.or(parent.default_padding),
            with_description_padding: local
                .with_description_padding
                .or(parent.with_description_padding),
            with_description_icon_size: local
                .with_description_icon_size
                .or(parent.with_description_icon_size),
        }
    }

    fn apply(tokens: &mut Self::Tokens, patch: &Self::Patch) {
        if let Some(value) = patch.radius {
            tokens.radius = value;
        }
        if let Some(value) = patch.default_padding {
            tokens.default_padding = value;
        }
        if let Some(value) = patch.with_description_padding {
            tokens.with_description_padding = value;
        }
        if let Some(value) = patch.with_description_icon_size {
            tokens.with_description_icon_size = value;
        }
    }
}

impl AlertTokens {
    pub fn globals(&self) -> &AliasTokens {
        &self.globals
    }

    pub fn font(&self) -> &Font {
        &self.font
    }

    pub fn icon_palette(&self) -> IconPalette {
        self.icon_palette
    }
}

/// Token-derived geometry and colors. All lengths and line heights are logical pixels.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct AlertStyle {
    pub background: Hsla,
    pub border: Hsla,
    pub title_color: Hsla,
    pub description_color: Hsla,
    pub icon_color: Hsla,
    pub close_color: Hsla,
    pub close_hover_color: Hsla,
    pub close_disabled_color: Hsla,
    pub font_size: f64,
    pub title_font_size: f64,
    pub line_height: f64,
    pub title_line_height: f64,
    pub line_width: f64,
    pub radius: f64,
    pub padding: AlertPadding,
    pub icon_size: f64,
    pub icon_gap: f64,
    pub action_gap: f64,
    pub close_size: f64,
    pub close_gap: f64,
    pub title_gap: f64,
    pub focus_color: Hsla,
    pub focus_width: f64,
    pub motion_duration: Duration,
    pub motion_easing: CubicBezier,
    pub hover_duration: Duration,
}

impl AlertStyle {
    pub fn resolve(
        theme: &ResolvedTheme,
        kind: AlertType,
        variant: AlertVariant,
        description: bool,
        banner: bool,
    ) -> Self {
        Self::from_tokens(
            &theme.component::<AlertTheme>(),
            kind,
            variant,
            description,
            banner,
        )
    }

    pub fn from_tokens(
        tokens: &AlertTokens,
        kind: AlertType,
        variant: AlertVariant,
        description: bool,
        banner: bool,
    ) -> Self {
        let g = tokens.globals();
        let (background, border, icon) = match kind {
            AlertType::Success => (g.color_success_bg, g.color_success_border, g.color_success),
            AlertType::Info => (g.color_info_bg, g.color_info_border, g.color_info),
            AlertType::Warning => (g.color_warning_bg, g.color_warning_border, g.color_warning),
            AlertType::Error => (g.color_error_bg, g.color_error_border, g.color_error),
        };
        let title_font_size = if description {
            g.font_size_lg
        } else {
            g.font_size
        };
        Self {
            background: background.into(),
            border: if variant == AlertVariant::Filled || banner {
                Color::TRANSPARENT.into()
            } else {
                border.into()
            },
            title_color: g.color_text_heading.into(),
            description_color: g.color_text.into(),
            icon_color: icon.into(),
            close_color: g.color_icon.into(),
            close_hover_color: g.color_icon_hover.into(),
            close_disabled_color: g.color_text_disabled.into(),
            font_size: g.font_size,
            title_font_size,
            line_height: g.font_size * g.line_height,
            // Alert titles inherit the base unitless line-height, including when enlarged.
            title_line_height: title_font_size * g.line_height,
            line_width: if banner { 0. } else { g.line_width },
            radius: if banner { 0. } else { tokens.radius },
            padding: if description {
                tokens.with_description_padding
            } else {
                tokens.default_padding
            },
            icon_size: if description {
                tokens.with_description_icon_size
            } else {
                g.font_size
            },
            icon_gap: if description {
                g.margin_sm
            } else {
                g.margin_xs
            },
            action_gap: g.margin_xs,
            close_size: g.font_size_icon,
            close_gap: g.margin_xs,
            title_gap: if description { g.margin_xs } else { 0. },
            focus_color: g.color_primary_border.into(),
            focus_width: g.line_width_focus,
            motion_duration: g.motion_duration_slow,
            motion_easing: g.motion_ease_in_out_circ,
            hover_duration: g.motion_duration_mid,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::theme::{AlgorithmChain, ComponentThemeConfig, ThemeConfig};

    #[test]
    fn alert_default_tokens_follow_reference_geometry_in_all_theme_algorithms() {
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
            let t = theme.component::<AlertTheme>();
            let g = t.globals();
            for kind in [
                AlertType::Success,
                AlertType::Info,
                AlertType::Warning,
                AlertType::Error,
            ] {
                let basic = AlertStyle::from_tokens(&t, kind, AlertVariant::Outlined, false, false);
                let rich = AlertStyle::from_tokens(&t, kind, AlertVariant::Outlined, true, false);
                assert_eq!(
                    basic.padding,
                    AlertPadding::symmetric(g.padding_content_vertical_sm, 12.)
                );
                assert_eq!(
                    rich.padding,
                    AlertPadding::symmetric(g.padding_md, g.padding_content_horizontal_lg)
                );
                assert_eq!(rich.title_line_height, g.font_size_lg * g.line_height);
                assert_eq!(rich.icon_size, g.font_size_heading3);
                assert_eq!(basic.icon_size, g.font_size);
                assert_eq!(basic.close_size, g.font_size_icon);
                let filled = AlertStyle::from_tokens(&t, kind, AlertVariant::Filled, false, false);
                assert_eq!(filled.border, Color::TRANSPARENT.to_hsla());
                assert_eq!(filled.line_width, basic.line_width);
                assert_eq!(filled.padding, basic.padding);
                let banner = AlertStyle::from_tokens(&t, kind, AlertVariant::Outlined, true, true);
                assert_eq!(banner.line_width, 0.);
                assert_eq!(banner.radius, 0.);
                assert_eq!(banner.padding, rich.padding);
            }
        }
    }

    #[test]
    fn alert_component_patches_merge_and_cache_without_changing_global_tokens() {
        let mut parent = ThemeConfig::default();
        parent.components.set::<AlertTheme>(AlertTokenPatch {
            radius: Some(3.),
            default_padding: Some(AlertPadding::symmetric(5., 9.)),
            ..Default::default()
        });
        let mut child = ThemeConfig::default();
        child.components.set_theme::<AlertTheme>(
            ComponentThemeConfig::new(AlertTokenPatch {
                with_description_padding: Some(AlertPadding::new(10., 11., 12., 13.)),
                with_description_icon_size: Some(30.),
                ..Default::default()
            })
            .with_tokens(crate::theme::TokenPatch {
                font_size: Some(18.),
                ..Default::default()
            }),
        );
        let theme = ThemeConfig::merged(&parent, &child).resolve();
        let tokens = theme.component::<AlertTheme>();
        assert!(Arc::ptr_eq(&tokens, &theme.component::<AlertTheme>()));
        assert_eq!(tokens.radius, 3.);
        assert_eq!(tokens.default_padding, AlertPadding::symmetric(5., 9.));
        assert_eq!(
            tokens.with_description_padding,
            AlertPadding::new(10., 11., 12., 13.)
        );
        assert_eq!(tokens.with_description_icon_size, 30.);
        assert_eq!(tokens.globals().font_size, 18.);
        assert_eq!(theme.tokens.font_size, 14.);
    }

    #[test]
    fn alert_focus_outline_uses_resolved_alias_and_component_override() {
        let mut config = ThemeConfig {
            seed: crate::theme::SeedTokenPatch {
                focus_outline: Some(false),
                ..Default::default()
            },
            ..Default::default()
        };
        let style = AlertStyle::resolve(
            &config.resolve(),
            AlertType::Info,
            AlertVariant::Outlined,
            false,
            false,
        );
        assert_eq!(style.focus_width, 0.);
        config
            .components
            .set_theme::<AlertTheme>(ComponentThemeConfig::default().with_tokens(
                crate::theme::TokenPatch {
                    line_width_focus: Some(7.),
                    ..Default::default()
                },
            ));
        let theme = config.resolve();
        let style = AlertStyle::resolve(
            &theme,
            AlertType::Info,
            AlertVariant::Outlined,
            false,
            false,
        );
        assert!(!theme.tokens.focus_outline);
        assert_eq!(theme.tokens.line_width_focus, 0.);
        assert_eq!(style.focus_width, 7.);
    }
}
