// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Visual defaults adapted from Ant Design 6.6.7 (MIT).

use std::sync::Arc;

use gpui::{DefiniteLength, relative};

use crate::{
    Size,
    theme::{AliasTokens, Color, ComponentTheme, ResolvedTheme},
};

/// Marker for `ComponentThemes::set::<DividerTheme>(...)`.
pub struct DividerTheme;

#[derive(Clone, Debug, PartialEq)]
pub struct DividerTokens {
    /// Pixels/rems, or multiples of the active title font size (`relative(1.)` is 1em).
    pub text_padding_inline: DefiniteLength,
    /// Fraction of the remaining rail space allocated to the short rail.
    pub orientation_margin: f32,
    pub vertical_margin_inline: f32,
    globals: Arc<AliasTokens>,
    font: gpui::Font,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct DividerTokenPatch {
    pub text_padding_inline: Option<DefiniteLength>,
    pub orientation_margin: Option<f32>,
    pub vertical_margin_inline: Option<f32>,
}

impl ComponentTheme for DividerTheme {
    type Tokens = DividerTokens;
    type Patch = DividerTokenPatch;

    fn default_tokens(global: &AliasTokens) -> Self::Tokens {
        DividerTokens {
            text_padding_inline: relative(1.),
            orientation_margin: 0.05,
            vertical_margin_inline: global.margin_xs as f32,
            globals: Arc::new(global.clone()),
            font: global.font_family.to_gpui_font(),
        }
    }

    fn merge(parent: &Self::Patch, local: &Self::Patch) -> Self::Patch {
        DividerTokenPatch {
            text_padding_inline: local.text_padding_inline.or(parent.text_padding_inline),
            orientation_margin: local.orientation_margin.or(parent.orientation_margin),
            vertical_margin_inline: local
                .vertical_margin_inline
                .or(parent.vertical_margin_inline),
        }
    }

    fn apply(tokens: &mut Self::Tokens, patch: &Self::Patch) {
        if let Some(value) = patch.text_padding_inline {
            tokens.text_padding_inline = value;
        }
        if let Some(value) = patch.orientation_margin {
            tokens.orientation_margin = if value.is_finite() {
                value.clamp(0., 1.)
            } else {
                0.05
            };
        }
        if let Some(value) = patch.vertical_margin_inline {
            tokens.vertical_margin_inline = finite_length(value);
        }
    }
}

impl DividerTokens {
    pub(crate) fn font(&self) -> gpui::Font {
        self.font.clone()
    }
}

/// Pure visual metrics. Unset size preserves Ant's larger untitled spacing.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct DividerStyle {
    pub margin_block: f32,
    pub vertical_margin_inline: f32,
    pub vertical_height: f32,
    pub vertical_offset: f32,
    pub line_width: f32,
    pub line_color: Color,
    pub text_color: Color,
    pub font_size: f32,
    pub font_weight: f32,
    pub line_height: f32,
    pub text_padding_inline: DefiniteLength,
    pub orientation_margin: f32,
}

impl DividerStyle {
    pub fn resolve(theme: &ResolvedTheme, size: Option<Size>, titled: bool, plain: bool) -> Self {
        Self::from_tokens(&theme.component::<DividerTheme>(), size, titled, plain)
    }

    pub fn from_tokens(
        tokens: &DividerTokens,
        size: Option<Size>,
        titled: bool,
        plain: bool,
    ) -> Self {
        let global = &tokens.globals;
        let heading = titled && !plain;
        Self {
            margin_block: finite_length(match size {
                Some(Size::Small) => global.margin_xs,
                Some(Size::Medium) => global.margin,
                _ if titled => global.margin,
                _ => global.margin_lg,
            } as f32),
            vertical_margin_inline: tokens.vertical_margin_inline,
            vertical_height: finite_length(global.font_size as f32 * 0.9),
            vertical_offset: -finite_length(global.font_size as f32 * 0.06),
            line_width: finite_length(global.line_width as f32),
            line_color: global.color_split,
            text_color: if heading {
                global.color_text_heading
            } else {
                global.color_text
            },
            font_size: finite_length(if heading {
                global.font_size_lg
            } else {
                global.font_size
            } as f32),
            font_weight: if heading { 500. } else { 400. },
            line_height: global.line_height as f32,
            text_padding_inline: tokens.text_padding_inline,
            orientation_margin: tokens.orientation_margin,
        }
    }
}

pub(super) fn finite_length(value: f32) -> f32 {
    if value.is_finite() { value.max(0.) } else { 0. }
}
