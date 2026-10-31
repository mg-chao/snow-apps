// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Visual formulas adapted from Ant Design 6.6.7 (MIT).

use std::sync::Arc;

use gpui::{Font, Hsla};

use super::ResultStatus;
use crate::{
    icons::IconPalette,
    theme::{AliasTokens, ComponentTheme, ResolvedTheme},
};

/// Physical top/right/bottom/left margins in logical pixels.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct ResultMargin {
    pub top: f64,
    pub right: f64,
    pub bottom: f64,
    pub left: f64,
}

impl ResultMargin {
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

impl From<f64> for ResultMargin {
    fn from(value: f64) -> Self {
        Self::all(value)
    }
}

pub struct ResultTheme;

#[derive(Clone, Debug, PartialEq)]
pub struct ResultTokens {
    pub title_font_size: f64,
    pub subtitle_font_size: f64,
    pub icon_font_size: f64,
    pub extra_margin: ResultMargin,
    globals: Arc<AliasTokens>,
    font: Font,
    icon_palette: IconPalette,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct ResultTokenPatch {
    pub title_font_size: Option<f64>,
    pub subtitle_font_size: Option<f64>,
    pub icon_font_size: Option<f64>,
    pub extra_margin: Option<ResultMargin>,
}

impl ComponentTheme for ResultTheme {
    type Tokens = ResultTokens;
    type Patch = ResultTokenPatch;

    fn default_tokens(global: &AliasTokens) -> Self::Tokens {
        ResultTokens {
            title_font_size: global.font_size_heading3,
            subtitle_font_size: global.font_size,
            icon_font_size: global.font_size_heading3 * 3.,
            extra_margin: ResultMargin::new(global.padding_lg, 0., 0., 0.),
            globals: Arc::new(global.clone()),
            font: global.font_family.to_gpui_font(),
            icon_palette: IconPalette::from_theme(global),
        }
    }

    fn merge(parent: &Self::Patch, local: &Self::Patch) -> Self::Patch {
        ResultTokenPatch {
            title_font_size: local.title_font_size.or(parent.title_font_size),
            subtitle_font_size: local.subtitle_font_size.or(parent.subtitle_font_size),
            icon_font_size: local.icon_font_size.or(parent.icon_font_size),
            extra_margin: local.extra_margin.or(parent.extra_margin),
        }
    }

    fn apply(tokens: &mut Self::Tokens, patch: &Self::Patch) {
        if let Some(value) = patch.title_font_size {
            tokens.title_font_size = value;
        }
        if let Some(value) = patch.subtitle_font_size {
            tokens.subtitle_font_size = value;
        }
        if let Some(value) = patch.icon_font_size {
            tokens.icon_font_size = value;
        }
        if let Some(value) = patch.extra_margin {
            tokens.extra_margin = value;
        }
    }
}

impl ResultTokens {
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

/// Resolved native geometry and colors. Line heights are unitless multipliers.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ResultStyle {
    pub padding_vertical: f64,
    pub padding_horizontal: f64,
    pub font_size: f64,
    pub line_height: f64,
    pub title_font_size: f64,
    pub title_line_height: f64,
    pub subtitle_font_size: f64,
    pub icon_font_size: f64,
    pub icon_margin_bottom: f64,
    pub title_margin: f64,
    pub extra_margin: ResultMargin,
    pub extra_gap: f64,
    pub body_margin_top: f64,
    pub body_padding_vertical: f64,
    pub body_padding_horizontal: f64,
    pub image_width: f64,
    pub image_height: f64,
    pub text_color: Hsla,
    pub title_color: Hsla,
    pub subtitle_color: Hsla,
    pub icon_color: Hsla,
    pub body_background: Hsla,
}

impl ResultStyle {
    pub fn resolve(theme: &ResolvedTheme, status: ResultStatus) -> Self {
        Self::from_tokens(&theme.component::<ResultTheme>(), status)
    }

    pub fn from_tokens(tokens: &ResultTokens, status: ResultStatus) -> Self {
        let g = tokens.globals();
        Self {
            padding_vertical: g.padding_lg * 2.,
            padding_horizontal: g.padding_xl,
            font_size: g.font_size,
            line_height: g.line_height,
            title_font_size: tokens.title_font_size,
            title_line_height: g.line_height_heading3,
            subtitle_font_size: tokens.subtitle_font_size,
            icon_font_size: tokens.icon_font_size,
            icon_margin_bottom: g.padding_lg,
            title_margin: g.margin_xs,
            extra_margin: tokens.extra_margin,
            extra_gap: g.padding_xs,
            body_margin_top: g.padding_lg,
            body_padding_vertical: g.padding_lg,
            body_padding_horizontal: g.padding * 2.5,
            image_width: 250.,
            image_height: 295.,
            text_color: g.color_text.into(),
            title_color: g.color_text_heading.into(),
            subtitle_color: g.color_text_description.into(),
            icon_color: match status {
                ResultStatus::Success => g.color_success,
                ResultStatus::Error => g.color_error,
                ResultStatus::Warning => g.color_warning,
                _ => g.color_info,
            }
            .into(),
            body_background: g.color_fill_alter.into(),
        }
    }
}
