// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Visual token defaults and state rules adapted from Ant Design 6.6.7 (MIT).

//! Cached Radio tokens and pure native visual state resolution.

use std::sync::Arc;

use gpui::Hsla;

use super::RadioButtonStyle;
use crate::{
    Size,
    theme::{AliasTokens, Color, ComponentTheme, ResolvedTheme},
};

/// Marker for component-local Radio tokens, global aliases and algorithms.
pub struct RadioTheme;

macro_rules! radio_tokens {
    ($($field:ident: $ty:ty = $value:expr;)+) => {
        /// Radio component tokens. Dimensions are logical pixels.
        #[derive(Clone, Debug, PartialEq)]
        pub struct RadioTokens {
            $(pub $field: $ty,)+
            globals: Arc<AliasTokens>,
            font: gpui::Font,
        }

        /// Sparse component token overrides, merged field by field across scopes.
        #[derive(Clone, Debug, Default, PartialEq)]
        pub struct RadioTokenPatch { $(pub $field: Option<$ty>,)+ }

        impl ComponentTheme for RadioTheme {
            type Tokens = RadioTokens;
            type Patch = RadioTokenPatch;

            fn default_tokens(global: &AliasTokens) -> Self::Tokens {
                RadioTokens {
                    $($field: ($value)(global),)+
                    globals: Arc::new(global.clone()),
                    font: global.font_family.to_gpui_font(),
                }
            }

            fn merge(parent: &Self::Patch, local: &Self::Patch) -> Self::Patch {
                RadioTokenPatch { $($field: local.$field.or(parent.$field),)+ }
            }

            fn apply(tokens: &mut Self::Tokens, patch: &Self::Patch) {
                $(if let Some(value) = patch.$field { tokens.$field = value; })+
            }
        }
    };
}

radio_tokens! {
    radio_size: f64 = |g: &AliasTokens| g.font_size_lg;
    dot_size: f64 = |g: &AliasTokens| g.font_size_lg - if g.wireframe { 8. } else { (4. + g.line_width) * 2. };
    dot_color_disabled: Color = |g: &AliasTokens| g.color_text_disabled;
    button_bg: Color = |g: &AliasTokens| g.color_bg_container;
    button_checked_bg: Color = |g: &AliasTokens| g.color_bg_container;
    button_color: Color = |g: &AliasTokens| g.color_text;
    button_padding_inline: f64 = |g: &AliasTokens| g.padding - g.line_width;
    button_checked_bg_disabled: Color = |g: &AliasTokens| g.control_item_bg_active_disabled;
    button_checked_color_disabled: Color = |g: &AliasTokens| g.color_text_disabled;
    button_solid_checked_color: Color = |g: &AliasTokens| g.color_text_light_solid;
    button_solid_checked_bg: Color = |g: &AliasTokens| g.color_primary;
    button_solid_checked_hover_bg: Color = |g: &AliasTokens| g.color_primary_hover;
    button_solid_checked_active_bg: Color = |g: &AliasTokens| g.color_primary_active;
    wrapper_margin_inline_end: f64 = |g: &AliasTokens| g.margin_xs;
    radio_color: Color = |g: &AliasTokens| if g.wireframe { g.color_primary } else { g.color_white };
    radio_bg_color: Color = |g: &AliasTokens| if g.wireframe { g.color_bg_container } else { g.color_primary };
}

impl RadioTokens {
    pub(crate) fn globals(&self) -> &AliasTokens {
        &self.globals
    }

    pub(crate) fn font(&self) -> &gpui::Font {
        &self.font
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct RadioStateStyle {
    pub background: Hsla,
    pub border: Hsla,
    pub mark: Hsla,
    pub label: Hsla,
}

/// Pure visual metrics for a circular Radio, independent of provider input size.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct RadioStyle {
    pub rest: RadioStateStyle,
    pub hover: RadioStateStyle,
    pub size: f64,
    pub dot_size: f64,
    pub line_width: f64,
    pub font_size: f64,
    /// Absolute label line height in logical pixels.
    pub line_height: f64,
    pub label_padding: f64,
    pub group_gap: f64,
    pub vertical_gap: f64,
    pub focus_color: Hsla,
    pub focus_width: f64,
}

impl RadioStyle {
    pub fn resolve(theme: &ResolvedTheme, checked: bool, disabled: bool) -> Self {
        Self::from_tokens(&theme.component::<RadioTheme>(), checked, disabled)
    }

    pub fn from_tokens(tokens: &RadioTokens, checked: bool, disabled: bool) -> Self {
        let g = tokens.globals();
        let mut rest = RadioStateStyle {
            background: if checked {
                tokens.radio_bg_color
            } else {
                g.color_bg_container
            }
            .into(),
            border: if checked {
                g.color_primary
            } else {
                g.color_border
            }
            .into(),
            mark: tokens.radio_color.into(),
            label: g.color_text.into(),
        };
        let mut hover = rest;
        hover.border = g.color_primary.into();
        if checked {
            hover.background = g.color_primary_hover.into();
            hover.border = Color::TRANSPARENT.into();
        }
        if disabled {
            rest.background = g.color_bg_container_disabled.into();
            rest.border = g.color_border.into();
            rest.mark = tokens.dot_color_disabled.into();
            rest.label = g.color_text_disabled.into();
            hover = rest;
        }
        Self {
            rest,
            hover,
            size: length(tokens.radio_size),
            dot_size: length(tokens.dot_size),
            line_width: length(g.line_width),
            font_size: g.font_size,
            line_height: g.font_size * g.line_height,
            label_padding: g.padding_xs,
            group_gap: length(tokens.wrapper_margin_inline_end),
            vertical_gap: g.margin_xs,
            focus_color: g.color_primary_border.into(),
            focus_width: focus_width(g, disabled),
        }
    }
}

/// Pure visual metrics for a connected Radio button.
///
/// Its group owns shared borders and corner placement; these metrics describe
/// a complete single button before adjoining edges are overlapped.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct RadioButtonVisualStyle {
    pub rest: RadioStateStyle,
    pub hover: RadioStateStyle,
    pub active: RadioStateStyle,
    pub height: f64,
    pub padding_inline: f64,
    pub font_size: f64,
    pub line_height: f64,
    pub radius: f64,
    pub line_width: f64,
    pub focus_color: Hsla,
    pub focus_width: f64,
}

impl RadioButtonVisualStyle {
    pub fn resolve(
        theme: &ResolvedTheme,
        size: Size,
        button_style: RadioButtonStyle,
        checked: bool,
        disabled: bool,
    ) -> Self {
        Self::from_tokens(
            &theme.component::<RadioTheme>(),
            size,
            button_style,
            checked,
            disabled,
        )
    }

    pub fn from_tokens(
        tokens: &RadioTokens,
        size: Size,
        button_style: RadioButtonStyle,
        checked: bool,
        disabled: bool,
    ) -> Self {
        let g = tokens.globals();
        let (height, font_size, radius, padding_inline) = match size {
            Size::Small => (
                g.control_height_sm,
                g.font_size,
                g.border_radius_sm,
                g.padding_xs - g.line_width,
            ),
            Size::Medium => (
                g.control_height,
                g.font_size,
                g.border_radius,
                tokens.button_padding_inline,
            ),
            Size::Large => (
                g.control_height_lg,
                g.font_size_lg,
                g.border_radius_lg,
                tokens.button_padding_inline,
            ),
        };
        let mut rest = RadioStateStyle {
            background: tokens.button_bg.into(),
            border: g.color_border.into(),
            mark: Color::TRANSPARENT.into(),
            label: tokens.button_color.into(),
        };
        let mut hover = rest;
        hover.label = g.color_primary.into();
        let mut active = hover;
        if checked {
            rest.background = tokens.button_checked_bg.into();
            rest.border = g.color_primary.into();
            rest.label = g.color_primary.into();
            hover = rest;
            hover.border = g.color_primary_hover.into();
            hover.label = g.color_primary_hover.into();
            active = rest;
            active.border = g.color_primary_active.into();
            active.label = g.color_primary_active.into();
            if button_style == RadioButtonStyle::Solid {
                rest.background = tokens.button_solid_checked_bg.into();
                rest.border = rest.background;
                rest.label = tokens.button_solid_checked_color.into();
                hover = rest;
                hover.background = tokens.button_solid_checked_hover_bg.into();
                hover.border = hover.background;
                active = rest;
                active.background = tokens.button_solid_checked_active_bg.into();
                active.border = active.background;
            }
        }
        if disabled {
            rest.background = if checked {
                tokens.button_checked_bg_disabled
            } else {
                g.color_bg_container_disabled
            }
            .into();
            rest.border = g.color_border.into();
            rest.label = if checked {
                tokens.button_checked_color_disabled
            } else {
                g.color_text_disabled
            }
            .into();
            hover = rest;
            active = rest;
        }
        Self {
            rest,
            hover,
            active,
            height: length(height),
            padding_inline: length(padding_inline),
            font_size,
            line_height: length(height - g.line_width * 2.),
            radius: length(radius),
            line_width: length(g.line_width),
            focus_color: g.color_primary_border.into(),
            focus_width: focus_width(g, disabled),
        }
    }
}

fn focus_width(tokens: &AliasTokens, disabled: bool) -> f64 {
    if disabled || !tokens.focus_outline {
        0.
    } else {
        tokens.line_width_focus
    }
}

fn length(value: f64) -> f64 {
    if value.is_finite() { value.max(0.) } else { 0. }
}

#[cfg(test)]
#[path = "style_tests.rs"]
mod tests;
