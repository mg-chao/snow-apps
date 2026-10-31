// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Visual rules adapted from Ant Design 6.6.7 (MIT).

use crate::theme::{AliasTokens, ComponentTheme, ResolvedTheme};
use gpui::{Font, Hsla};
use std::sync::Arc;

pub struct PopconfirmTheme;
#[derive(Clone, Debug, PartialEq)]
pub struct PopconfirmTokens {
    pub z_index_popup: usize,
    globals: Arc<AliasTokens>,
    font: Font,
}
#[derive(Clone, Debug, Default, PartialEq)]
pub struct PopconfirmTokenPatch {
    pub z_index_popup: Option<usize>,
}
impl ComponentTheme for PopconfirmTheme {
    type Tokens = PopconfirmTokens;
    type Patch = PopconfirmTokenPatch;
    fn default_tokens(g: &AliasTokens) -> PopconfirmTokens {
        PopconfirmTokens {
            z_index_popup: g.z_index_popup_base as usize + 60,
            globals: Arc::new(g.clone()),
            font: g.font_family.to_gpui_font(),
        }
    }
    fn merge(parent: &Self::Patch, local: &Self::Patch) -> Self::Patch {
        PopconfirmTokenPatch {
            z_index_popup: local.z_index_popup.or(parent.z_index_popup),
        }
    }
    fn apply(tokens: &mut Self::Tokens, patch: &Self::Patch) {
        if let Some(value) = patch.z_index_popup {
            tokens.z_index_popup = value;
        }
    }
}
impl PopconfirmTokens {
    pub(crate) fn globals(&self) -> &AliasTokens {
        &self.globals
    }
    pub(crate) fn font(&self) -> &Font {
        &self.font
    }
}
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct PopconfirmStyle {
    pub icon_color: Hsla,
    pub title_color: Hsla,
    pub description_color: Hsla,
    pub font_size: f64,
    pub line_height: f64,
    pub title_weight: f64,
    pub icon_gap: f64,
    pub message_margin_bottom: f64,
    pub description_margin_top: f64,
    pub button_margin_start: f64,
}
impl PopconfirmStyle {
    pub fn resolve(theme: &ResolvedTheme, has_description: bool) -> Self {
        Self::from_tokens(&theme.component::<PopconfirmTheme>(), has_description)
    }
    pub fn from_tokens(tokens: &PopconfirmTokens, has_description: bool) -> Self {
        let g = tokens.globals();
        Self {
            icon_color: g.color_warning.into(),
            title_color: g.color_text_heading.into(),
            description_color: g.color_text.into(),
            font_size: g.font_size,
            line_height: g.line_height,
            title_weight: if has_description {
                g.font_weight_strong
            } else {
                400.
            },
            icon_gap: g.margin_xs,
            message_margin_bottom: g.margin_xs,
            description_margin_top: g.margin_xxs,
            button_margin_start: g.margin_xs,
        }
    }
}
