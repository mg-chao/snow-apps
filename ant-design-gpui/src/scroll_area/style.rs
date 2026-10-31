// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use crate::theme::{AliasTokens, Color, ComponentTheme};

pub struct ScrollAreaTheme;

/// Logical pixel lengths. The hit target remains generous while the visual thumb
/// expands without changing the content's available space.
#[derive(Clone, Debug, PartialEq)]
pub struct ScrollAreaTokens {
    pub thickness: f64,
    pub collapsed_thickness: f64,
    pub inset: f64,
    pub min_thumb_length: f64,
    pub thumb_color: Color,
    pub thumb_hover_color: Color,
    pub thumb_active_color: Color,
    pub track_color: Color,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct ScrollAreaTokenPatch {
    pub thickness: Option<f64>,
    pub collapsed_thickness: Option<f64>,
    pub inset: Option<f64>,
    pub min_thumb_length: Option<f64>,
    pub thumb_color: Option<Color>,
    pub thumb_hover_color: Option<Color>,
    pub thumb_active_color: Option<Color>,
    pub track_color: Option<Color>,
}

impl ComponentTheme for ScrollAreaTheme {
    type Tokens = ScrollAreaTokens;
    type Patch = ScrollAreaTokenPatch;

    fn default_tokens(global: &AliasTokens) -> Self::Tokens {
        ScrollAreaTokens {
            thickness: 8.,
            collapsed_thickness: 3.,
            inset: 2.,
            min_thumb_length: 24.,
            thumb_color: global.color_text_quaternary,
            thumb_hover_color: global.color_text_tertiary,
            thumb_active_color: global.color_text_secondary,
            track_color: global.color_fill_quaternary,
        }
    }

    fn merge(parent: &Self::Patch, local: &Self::Patch) -> Self::Patch {
        macro_rules! merge { ($($field:ident),+) => { ScrollAreaTokenPatch { $($field: local.$field.or(parent.$field),)+ } }; }
        merge!(
            thickness,
            collapsed_thickness,
            inset,
            min_thumb_length,
            thumb_color,
            thumb_hover_color,
            thumb_active_color,
            track_color
        )
    }

    fn apply(tokens: &mut Self::Tokens, patch: &Self::Patch) {
        macro_rules! lengths { ($($field:ident),+) => { $(if let Some(value) = patch.$field
            && value.is_finite() && (0. ..=f32::MAX as f64).contains(&value) {
                tokens.$field = value;
            })+ }; }
        lengths!(thickness, collapsed_thickness, inset, min_thumb_length);
        macro_rules! colors { ($($field:ident),+) => { $(if let Some(value) = patch.$field { tokens.$field = value; })+ }; }
        colors!(
            thumb_color,
            thumb_hover_color,
            thumb_active_color,
            track_color
        );
        tokens.thickness = tokens.thickness.max(1.);
        tokens.collapsed_thickness = tokens.collapsed_thickness.min(tokens.thickness);
    }
}
