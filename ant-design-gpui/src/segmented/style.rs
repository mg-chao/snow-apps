// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Visual token defaults and state rules adapted from Ant Design 6.6.7 (MIT).

//! Cached component tokens and pure native Segmented visual states.

use std::sync::Arc;

use crate::{
    Size,
    icons::IconPalette,
    theme::{AliasTokens, Color, ComponentTheme, ResolvedTheme, ShadowList},
};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum SegmentedShape {
    #[default]
    Default,
    Round,
}

/// Marker for `ComponentThemes::set::<SegmentedTheme>(...)`.
pub struct SegmentedTheme;

macro_rules! segmented_tokens {
    ($($field:ident: $ty:ty = $value:expr;)+) => {
        /// Ant Design Segmented component tokens. Lengths are logical pixels.
        #[derive(Clone, Debug, PartialEq)]
        pub struct SegmentedTokens {
            $(pub $field: $ty,)+
            // Effective aliases preserve component-specific global overrides and
            // algorithms without resolving fonts or theme data during rendering.
            globals: Arc<AliasTokens>,
            font: gpui::Font,
            icon_palette: IconPalette,
            shadows: Arc<[gpui::BoxShadow]>,
        }

        #[derive(Clone, Debug, Default, PartialEq)]
        pub struct SegmentedTokenPatch { $(pub $field: Option<$ty>,)+ }

        impl ComponentTheme for SegmentedTheme {
            type Tokens = SegmentedTokens;
            type Patch = SegmentedTokenPatch;

            fn default_tokens(global: &AliasTokens) -> Self::Tokens {
                Self::Tokens {
                    $($field: ($value)(global),)+
                    globals: Arc::new(global.clone()),
                    font: global.font_family.to_gpui_font(),
                    icon_palette: IconPalette::from_theme(global),
                    shadows: global.box_shadow_tertiary.iter().map(|shadow| gpui::BoxShadow {
                        inset: shadow.inset,
                        color: shadow.color.to_hsla(),
                        offset: gpui::point(gpui::px(shadow.x as f32), gpui::px(shadow.y as f32)),
                        blur_radius: gpui::px(shadow.blur as f32),
                        spread_radius: gpui::px(shadow.spread as f32),
                    }).collect(),
                }
            }

            fn merge(parent: &Self::Patch, local: &Self::Patch) -> Self::Patch {
                Self::Patch { $($field: local.$field.or(parent.$field),)+ }
            }

            fn apply(tokens: &mut Self::Tokens, patch: &Self::Patch) {
                $(if let Some(value) = patch.$field { tokens.$field = value; })+
            }
        }
    };
}

segmented_tokens! {
    item_color: Color = |g: &AliasTokens| g.color_text_label;
    item_hover_color: Color = |g: &AliasTokens| g.color_text;
    item_hover_bg: Color = |g: &AliasTokens| g.color_fill_secondary;
    item_active_bg: Color = |g: &AliasTokens| g.color_fill;
    item_selected_bg: Color = |g: &AliasTokens| g.color_bg_elevated;
    item_selected_color: Color = |g: &AliasTokens| g.color_text;
    track_padding: f64 = |g: &AliasTokens| g.line_width_bold;
    track_bg: Color = |g: &AliasTokens| g.color_bg_layout;
}

impl SegmentedTokens {
    pub(crate) fn globals(&self) -> &AliasTokens {
        &self.globals
    }

    pub(crate) fn font(&self) -> &gpui::Font {
        &self.font
    }

    pub(crate) fn icon_palette(&self) -> IconPalette {
        self.icon_palette
    }

    pub(crate) fn shadows(&self) -> &Arc<[gpui::BoxShadow]> {
        &self.shadows
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SegmentedStyleOptions {
    pub size: Size,
    pub shape: SegmentedShape,
    pub disabled: bool,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SegmentedMetrics {
    /// Minimum overall height, including the track's padding.
    pub height: f64,
    /// Minimum item height. Custom content may increase it.
    pub item_height: f64,
    pub track_padding: f64,
    pub padding_inline: f64,
    pub font_size: f64,
    /// Absolute label line height in logical pixels.
    pub line_height: f64,
    pub track_radius: f64,
    pub item_radius: f64,
    pub icon_size: f64,
    pub icon_gap: f64,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SegmentedStateStyle {
    pub background: Color,
    pub text: Color,
}

#[derive(Clone, Debug, PartialEq)]
pub struct SegmentedStyle {
    pub metrics: SegmentedMetrics,
    pub track_background: Color,
    pub rest: SegmentedStateStyle,
    pub hover: SegmentedStateStyle,
    pub active: SegmentedStateStyle,
    pub selected: SegmentedStateStyle,
    pub disabled_text: Color,
    /// Selected items retain their surface and shadow when disabled.
    pub selected_shadow: ShadowList,
    pub focus_color: Color,
    pub focus_width: f64,
    pub focus_offset: f64,
}

impl SegmentedStyle {
    /// Resolve cached component tokens without regenerating global aliases.
    pub fn resolve(theme: &ResolvedTheme, options: SegmentedStyleOptions) -> Self {
        Self::from_tokens(&theme.component::<SegmentedTheme>(), options)
    }

    pub fn from_tokens(tokens: &SegmentedTokens, options: SegmentedStyleOptions) -> Self {
        let g = tokens.globals.as_ref();
        let (height, padding_inline, font_size, track_radius, item_radius) = match options.size {
            Size::Small => (
                g.control_height_sm,
                g.control_padding_horizontal_sm - g.line_width,
                g.font_size,
                g.border_radius_sm,
                g.border_radius_xs,
            ),
            Size::Medium => (
                g.control_height,
                g.control_padding_horizontal - g.line_width,
                g.font_size,
                g.border_radius,
                g.border_radius_sm,
            ),
            Size::Large => (
                g.control_height_lg,
                g.control_padding_horizontal - g.line_width,
                g.font_size_lg,
                g.border_radius_lg,
                g.border_radius,
            ),
        };
        let track_padding = tokens.track_padding.max(0.);
        let item_height = (height - track_padding * 2.).max(0.);
        let state = |background, text| SegmentedStateStyle { background, text };
        let text = |color| {
            if options.disabled {
                g.color_text_disabled
            } else {
                color
            }
        };
        let rest = state(Color::TRANSPARENT, text(tokens.item_color));
        Self {
            metrics: SegmentedMetrics {
                height: item_height + track_padding * 2.,
                item_height,
                track_padding,
                padding_inline: padding_inline.max(0.),
                font_size,
                line_height: item_height,
                track_radius: if options.shape == SegmentedShape::Round {
                    9999.
                } else {
                    track_radius
                },
                item_radius: if options.shape == SegmentedShape::Round {
                    9999.
                } else {
                    item_radius
                },
                icon_size: font_size,
                icon_gap: g.margin_sm / 2.,
            },
            track_background: tokens.track_bg,
            rest,
            hover: if options.disabled {
                rest
            } else {
                state(tokens.item_hover_bg, tokens.item_hover_color)
            },
            active: if options.disabled {
                rest
            } else {
                state(tokens.item_active_bg, tokens.item_hover_color)
            },
            selected: state(tokens.item_selected_bg, text(tokens.item_selected_color)),
            disabled_text: g.color_text_disabled,
            selected_shadow: g.box_shadow_tertiary.clone(),
            focus_color: g.color_primary_border,
            focus_width: if options.disabled {
                0.
            } else {
                g.line_width_focus
            },
            focus_offset: 1.,
        }
    }
}

#[cfg(test)]
#[path = "style_tests.rs"]
mod tests;
