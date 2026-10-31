// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Visual defaults and state rules adapted from Ant Design 6.6.7 (MIT).

//! Cached pagination tokens, size metrics, and pure item state styles.

use std::{sync::Arc, time::Duration};

use crate::{
    Size,
    icons::IconPalette,
    input::InputTokens,
    theme::{AliasTokens, Color, ComponentTheme, Shadow},
};

/// Marker for `ComponentThemes::set::<PaginationTheme>(...)`.
pub struct PaginationTheme;

macro_rules! pagination_tokens {
    ($($field:ident: $ty:ty = $default:expr;)+) => {
        #[derive(Clone, Debug, PartialEq)]
        pub struct PaginationTokens {
            $(pub $field: $ty,)+
            globals: Arc<AliasTokens>,
            font: gpui::Font,
            icon_palette: IconPalette,
        }

        #[derive(Clone, Debug, Default, PartialEq)]
        pub struct PaginationTokenPatch { $(pub $field: Option<$ty>,)+ }

        impl ComponentTheme for PaginationTheme {
            type Tokens = PaginationTokens;
            type Patch = PaginationTokenPatch;

            fn default_tokens(global: &AliasTokens) -> Self::Tokens {
                PaginationTokens {
                    $($field: ($default)(global),)+
                    globals: Arc::new(global.clone()),
                    font: browser_font(global.font_family.to_gpui_font()),
                    icon_palette: IconPalette::from_theme(global),
                }
            }

            fn merge(parent: &Self::Patch, local: &Self::Patch) -> Self::Patch {
                PaginationTokenPatch { $($field: local.$field.or(parent.$field),)+ }
            }

            fn apply(tokens: &mut Self::Tokens, patch: &Self::Patch) {
                $(if let Some(value) = patch.$field { tokens.$field = value; })+
                let defaults = Self::default_tokens(&tokens.globals);
                macro_rules! size {
                    ($name:ident) => {
                        if !(tokens.$name as f32).is_finite() || tokens.$name < 0. {
                            tokens.$name = defaults.$name;
                        }
                    };
                }
                size!(item_size);
                size!(item_size_sm);
                size!(item_size_lg);
                // This is a signed top offset, unlike the dimensions above.
                if !(tokens.mini_options_size_changer_top as f32).is_finite() {
                    tokens.mini_options_size_changer_top = defaults.mini_options_size_changer_top;
                }
            }
        }
    };
}

pagination_tokens! {
    item_bg: Color = |g: &AliasTokens| g.color_bg_container;
    item_size: f64 = |g: &AliasTokens| g.control_height;
    item_size_sm: f64 = |g: &AliasTokens| g.control_height_sm;
    item_size_lg: f64 = |g: &AliasTokens| g.control_height_lg;
    item_active_bg: Color = |g: &AliasTokens| g.color_bg_container;
    item_active_color: Color = |g: &AliasTokens| g.color_primary;
    item_active_color_hover: Color = |g: &AliasTokens| g.color_primary_hover;
    item_link_bg: Color = |g: &AliasTokens| g.color_bg_container;
    item_active_bg_disabled: Color = |g: &AliasTokens| g.control_item_bg_active_disabled;
    item_active_color_disabled: Color = |g: &AliasTokens| g.color_text_disabled;
    item_input_bg: Color = |g: &AliasTokens| g.color_bg_container;
    mini_options_size_changer_top: f64 = |_: &AliasTokens| 0.;
}

/// All lengths are logical pixels. Item width is a minimum so labels can grow.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct PaginationMetrics {
    pub item_size: f64,
    pub item_spacing: f64,
    pub font_size: f64,
    pub line_height: f64,
    pub border_width: f64,
    pub radius: f64,
    pub padding_inline: f64,
    pub icon_size: f64,
    pub ellipsis_size: f64,
    pub options_margin: f64,
    pub quick_jumper_margin: f64,
    pub jumper_width: f64,
    pub slash_margin: f64,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct PaginationStateStyle {
    pub background: Color,
    pub border: Color,
    pub text: Color,
    pub font_weight: f32,
}

impl PaginationTokens {
    pub fn globals(&self) -> &AliasTokens {
        &self.globals
    }

    pub(crate) fn font(&self) -> &gpui::Font {
        &self.font
    }

    pub(crate) fn icon_palette(&self) -> IconPalette {
        self.icon_palette
    }

    pub fn metrics(&self, size: Size) -> PaginationMetrics {
        let g = &self.globals;
        let (item_size, item_spacing, options_margin, jumper_width) = match size {
            Size::Small => (
                self.item_size_sm,
                g.margin_xxs,
                g.margin_xxs / 2.,
                g.control_height_lg * 1.1,
            ),
            Size::Medium => (
                self.item_size,
                g.margin_xs,
                g.margin,
                g.control_height_lg * 1.25,
            ),
            Size::Large => (
                self.item_size_lg,
                g.margin_sm,
                g.margin,
                g.control_height_lg * 1.25,
            ),
        };
        PaginationMetrics {
            item_size,
            item_spacing,
            font_size: g.font_size,
            // The reference reserves two pixels independently of lineWidth.
            line_height: (item_size - 2.).max(0.),
            border_width: g.line_width,
            radius: g.border_radius,
            padding_inline: g.margin_xxs * 1.5,
            icon_size: g.font_size_sm,
            ellipsis_size: g.size_lg,
            options_margin,
            quick_jumper_margin: g.margin_xs,
            jumper_width,
            slash_margin: g.margin_sm,
        }
    }

    /// Resolve number and previous/next surfaces. Jumpers use their own icons.
    /// A disabled boundary arrow uses `disabled` without disabling other items.
    pub fn surface(
        &self,
        selected: bool,
        navigation: bool,
        disabled: bool,
        hovered: bool,
        pressed: bool,
    ) -> PaginationStateStyle {
        let g = &self.globals;
        let selected = selected && !navigation;
        let transparent = Color::TRANSPARENT;
        let font_weight = if selected {
            g.font_weight_strong as f32
        } else {
            400.
        };
        if disabled {
            return PaginationStateStyle {
                background: if selected {
                    self.item_active_bg_disabled
                } else if g.wireframe {
                    g.color_bg_container_disabled
                } else {
                    transparent
                },
                border: if selected || g.wireframe {
                    g.color_border
                } else {
                    transparent
                },
                text: if selected {
                    self.item_active_color_disabled
                } else {
                    g.color_text_disabled
                },
                font_weight,
            };
        }
        if selected {
            return PaginationStateStyle {
                background: self.item_active_bg,
                border: if hovered {
                    g.color_primary_hover
                } else {
                    g.color_primary
                },
                text: if hovered {
                    self.item_active_color_hover
                } else {
                    self.item_active_color
                },
                font_weight,
            };
        }
        if g.wireframe {
            return PaginationStateStyle {
                background: if navigation && !hovered {
                    self.item_link_bg
                } else {
                    self.item_bg
                },
                border: if hovered {
                    g.color_primary
                } else {
                    g.color_border
                },
                text: if hovered {
                    g.color_primary
                } else {
                    g.color_text
                },
                font_weight,
            };
        }
        PaginationStateStyle {
            background: if pressed {
                g.color_bg_text_active
            } else if hovered {
                g.color_bg_text_hover
            } else if navigation {
                transparent
            } else {
                self.item_bg
            },
            border: transparent,
            text: g.color_text,
            font_weight,
        }
    }

    pub fn focus_color(&self) -> Color {
        self.globals.color_primary_border
    }

    pub fn focus_width(&self) -> f64 {
        self.globals.line_width_focus
    }

    pub fn focus_offset(&self) -> f64 {
        1.
    }

    pub fn motion_duration(&self) -> Duration {
        self.globals.motion_duration_mid
    }

    /// Adapt the retained native input to the reference pagination-owned input.
    /// The view supplies the explicit height and centers simple-mode text.
    pub fn input_tokens(&self, base: &InputTokens, _size: Size, simple: bool) -> InputTokens {
        let g = &self.globals;
        let mut tokens = base.clone();
        let mut globals = (**g).clone();
        if simple {
            globals.color_bg_container = self.item_input_bg;
            globals.border_radius_sm = g.border_radius;
            globals.border_radius_lg = g.border_radius;
            // The reference's simple input keeps the browser's form-control
            // font rather than inheriting Pagination typography.
            globals.line_height = 1.15;
            globals.line_height_lg = globals.line_height;
            tokens.padding_inline = g.margin_xxs * 1.5;
            tokens.padding_inline_sm = tokens.padding_inline;
            tokens.padding_inline_lg = tokens.padding_inline;
            tokens.padding_block = 0.;
            tokens.padding_block_sm = 0.;
            tokens.padding_block_lg = 0.;
            tokens.input_font_size = 40. / 3.;
            tokens.input_font_size_sm = tokens.input_font_size;
            tokens.input_font_size_lg = tokens.input_font_size;
            tokens.hover_border_color = g.color_primary;
            tokens.active_border_color = g.color_primary_hover;
            tokens.active_bg = self.item_input_bg;
            tokens.hover_bg = self.item_input_bg;
            tokens.active_shadow = Arc::from([Shadow {
                color: g.control_outline,
                spread: g.control_outline_width,
                ..Default::default()
            }]);
        }
        tokens.font = if simple {
            browser_font(gpui::font("Arial"))
        } else {
            self.font.clone()
        };
        tokens.globals = Arc::new(globals);
        tokens
    }
}

/// Browser typography uses normal pair kerning. Request it explicitly because
/// the pinned native DirectWrite backend leaves this feature off by default.
pub(super) fn browser_font(mut font: gpui::Font) -> gpui::Font {
    let mut features = font.features.tag_value_list().to_vec();
    features.retain(|(tag, _)| tag != "kern");
    features.push(("kern".into(), 1));
    font.features = gpui::FontFeatures(Arc::new(features));
    font
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::theme::{BuiltinAlgorithm, ThemeConfig};

    #[test]
    fn pagination_metrics_match_all_reference_sizes() {
        let theme = ThemeConfig::default().resolve();
        let tokens = theme.component::<PaginationTheme>();
        for (size, height, gap, width, options) in [
            (Size::Small, 24., 4., 44., 2.),
            (Size::Medium, 32., 8., 50., 16.),
            (Size::Large, 40., 12., 50., 16.),
        ] {
            let metrics = tokens.metrics(size);
            assert_eq!(metrics.item_size, height);
            assert_eq!(metrics.item_spacing, gap);
            assert!((metrics.jumper_width - width).abs() < 1e-9);
            assert_eq!(metrics.options_margin, options);
            assert_eq!(metrics.font_size, 14.);
            assert_eq!(metrics.icon_size, 12.);
            assert_eq!(metrics.line_height, height - 2.);
            assert_eq!(metrics.padding_inline, 6.);
        }
    }

    #[test]
    fn pagination_selected_disabled_and_wireframe_surfaces_match_reference() {
        let theme = ThemeConfig::default().resolve();
        let tokens = theme.component::<PaginationTheme>();
        let g = tokens.globals();
        let selected = tokens.surface(true, false, false, false, false);
        assert_eq!(selected.text, g.color_primary);
        assert_eq!(selected.border, g.color_primary);
        assert_eq!(selected.font_weight, 600.);
        let selected_hover = tokens.surface(true, false, false, true, true);
        assert_eq!(selected_hover.background, selected.background);
        assert_eq!(selected_hover.text, g.color_primary_hover);
        assert_eq!(
            tokens.surface(false, false, false, true, true).background,
            g.color_bg_text_active
        );
        let disabled = tokens.surface(true, false, true, true, true);
        assert_eq!(disabled.background, g.control_item_bg_active_disabled);
        assert_eq!(disabled.border, g.color_border);
        assert_eq!(disabled.text, g.color_text_disabled);
        assert_eq!(
            tokens.surface(false, true, true, true, true).background,
            Color::TRANSPARENT
        );

        let mut aliases = (*theme.tokens).clone();
        aliases.wireframe = true;
        let wireframe = PaginationTheme::default_tokens(&aliases);
        let rest = wireframe.surface(false, false, false, false, false);
        assert_eq!(rest.border, aliases.color_border);
        let hover = wireframe.surface(false, false, false, true, false);
        assert_eq!(hover.border, aliases.color_primary);
        assert_eq!(hover.text, aliases.color_primary);
        let disabled = wireframe.surface(false, true, true, true, true);
        assert_eq!(disabled.background, aliases.color_bg_container_disabled);
    }

    #[test]
    fn pagination_theme_patches_merge_and_sanitize_lengths() {
        let parent = PaginationTokenPatch {
            item_size: Some(36.),
            item_active_color: Some(Color::rgb(0xff0000)),
            ..Default::default()
        };
        let local = PaginationTokenPatch {
            item_size_sm: Some(20.),
            mini_options_size_changer_top: Some(-2.),
            ..Default::default()
        };
        let merged = PaginationTheme::merge(&parent, &local);
        let aliases = ThemeConfig::default().resolve().tokens;
        let mut tokens = PaginationTheme::default_tokens(&aliases);
        PaginationTheme::apply(&mut tokens, &merged);
        assert_eq!(tokens.item_size, 36.);
        assert_eq!(tokens.item_size_sm, 20.);
        assert_eq!(tokens.mini_options_size_changer_top, -2.);
        assert_eq!(tokens.item_active_color, Color::rgb(0xff0000));
        PaginationTheme::apply(
            &mut tokens,
            &PaginationTokenPatch {
                item_size: Some(f64::NAN),
                item_size_sm: Some(-1.),
                item_size_lg: Some(f64::MAX),
                ..Default::default()
            },
        );
        assert_eq!(tokens.item_size, aliases.control_height);
        assert_eq!(tokens.item_size_sm, aliases.control_height_sm);
        assert_eq!(tokens.item_size_lg, aliases.control_height_lg);
    }

    #[test]
    fn pagination_tokens_follow_dark_and_compact_algorithms() {
        for algorithm in [BuiltinAlgorithm::Dark, BuiltinAlgorithm::Compact] {
            let config = ThemeConfig {
                algorithms: Some(crate::theme::AlgorithmChain::new([algorithm])),
                ..Default::default()
            };
            let theme = config.resolve();
            let tokens = theme.component::<PaginationTheme>();
            assert_eq!(tokens.item_bg, theme.tokens.color_bg_container);
            assert_eq!(tokens.item_size, theme.tokens.control_height);
            assert_eq!(
                tokens.surface(false, false, false, true, false).background,
                theme.tokens.color_bg_text_hover
            );
        }
    }
}
