// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Token defaults adapted from Ant Design 6.6.7 (MIT).

use crate::theme::{AliasTokens, Color, ComponentTheme};
use std::sync::Arc;

pub struct ProgressTheme;
#[derive(Clone, Debug, PartialEq)]
pub struct ProgressTokens {
    pub default_color: Color,
    pub remaining_color: Color,
    pub circle_text_color: Color,
    pub line_border_radius: f64,
    /// Relative to the circular body's font size (diameter * .15 + 6).
    pub circle_text_font_size: f64,
    pub circle_icon_font_size: f64,
    globals: Arc<AliasTokens>,
}
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ProgressTokenPatch {
    pub default_color: Option<Color>,
    pub remaining_color: Option<Color>,
    pub circle_text_color: Option<Color>,
    pub line_border_radius: Option<f64>,
    pub circle_text_font_size: Option<f64>,
    pub circle_icon_font_size: Option<f64>,
}
impl ComponentTheme for ProgressTheme {
    type Tokens = ProgressTokens;
    type Patch = ProgressTokenPatch;
    fn default_tokens(g: &AliasTokens) -> ProgressTokens {
        ProgressTokens {
            default_color: g.color_info,
            remaining_color: g.color_fill_secondary,
            circle_text_color: g.color_text,
            line_border_radius: 100.,
            circle_text_font_size: 1.,
            circle_icon_font_size: g.font_size / g.font_size_sm,
            globals: Arc::new(g.clone()),
        }
    }
    fn merge(parent: &Self::Patch, local: &Self::Patch) -> Self::Patch {
        Self::Patch {
            default_color: local.default_color.or(parent.default_color),
            remaining_color: local.remaining_color.or(parent.remaining_color),
            circle_text_color: local.circle_text_color.or(parent.circle_text_color),
            line_border_radius: local.line_border_radius.or(parent.line_border_radius),
            circle_text_font_size: local.circle_text_font_size.or(parent.circle_text_font_size),
            circle_icon_font_size: local.circle_icon_font_size.or(parent.circle_icon_font_size),
        }
    }
    fn apply(tokens: &mut Self::Tokens, patch: &Self::Patch) {
        macro_rules! color { ($($name:ident),+) => { $(if let Some(value) = patch.$name { tokens.$name = value; })+ }; }
        color!(default_color, remaining_color, circle_text_color);
        macro_rules! length { ($($name:ident),+) => { $(if let Some(value) = patch.$name.filter(|v| v.is_finite() && (*v as f32).is_finite() && *v >= 0.) { tokens.$name = value; })+ }; }
        length!(
            line_border_radius,
            circle_text_font_size,
            circle_icon_font_size
        );
    }
}
impl ProgressTokens {
    pub fn globals(&self) -> &AliasTokens {
        &self.globals
    }
}
