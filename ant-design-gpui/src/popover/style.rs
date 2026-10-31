// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Visual rules adapted from Ant Design 6.6.7 (MIT).

use crate::{
    popup::PopupColor,
    theme::{AliasTokens, ComponentTheme, ResolvedTheme},
};
use gpui::{Font, Hsla};
use std::sync::Arc;

pub struct PopoverTheme;
#[derive(Clone, Debug, PartialEq)]
pub struct PopoverTokens {
    pub title_min_width: f64,
    pub z_index_popup: usize,
    pub inner_padding: f64,
    pub title_margin_bottom: f64,
    globals: Arc<AliasTokens>,
    font: Font,
}
#[derive(Clone, Debug, Default, PartialEq)]
pub struct PopoverTokenPatch {
    pub title_min_width: Option<f64>,
    pub z_index_popup: Option<usize>,
    pub inner_padding: Option<f64>,
    pub title_margin_bottom: Option<f64>,
}
impl ComponentTheme for PopoverTheme {
    type Tokens = PopoverTokens;
    type Patch = PopoverTokenPatch;
    fn default_tokens(g: &AliasTokens) -> PopoverTokens {
        PopoverTokens {
            title_min_width: 177.,
            z_index_popup: g.z_index_popup_base as usize + 30,
            inner_padding: if g.wireframe { 0. } else { 12. },
            title_margin_bottom: if g.wireframe { 0. } else { g.margin_xs },
            globals: Arc::new(g.clone()),
            font: g.font_family.to_gpui_font(),
        }
    }
    fn merge(parent: &Self::Patch, local: &Self::Patch) -> Self::Patch {
        PopoverTokenPatch {
            title_min_width: local.title_min_width.or(parent.title_min_width),
            z_index_popup: local.z_index_popup.or(parent.z_index_popup),
            inner_padding: local.inner_padding.or(parent.inner_padding),
            title_margin_bottom: local.title_margin_bottom.or(parent.title_margin_bottom),
        }
    }
    fn apply(tokens: &mut Self::Tokens, patch: &Self::Patch) {
        for (target, value) in [
            (&mut tokens.title_min_width, patch.title_min_width),
            (&mut tokens.inner_padding, patch.inner_padding),
            (&mut tokens.title_margin_bottom, patch.title_margin_bottom),
        ] {
            if let Some(value) = value.filter(|value| value.is_finite()) {
                *target = value.max(0.);
            }
        }
        if let Some(value) = patch.z_index_popup {
            tokens.z_index_popup = value;
        }
    }
}
impl PopoverTokens {
    pub(crate) fn globals(&self) -> &AliasTokens {
        &self.globals
    }
    pub(crate) fn font(&self) -> &Font {
        &self.font
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct PopoverStyle {
    pub background: Hsla,
    pub foreground: Hsla,
    pub title_color: Hsla,
    pub radius: f64,
    pub padding: f64,
    pub title_min_width: f64,
    pub title_margin_bottom: f64,
    pub title_padding_top: f64,
    pub title_padding_bottom: f64,
    pub title_padding_inline: f64,
    pub title_border_width: f64,
    pub title_border_color: Hsla,
    pub content_padding_inline: f64,
    pub content_padding_block: f64,
    pub font_size: f64,
    pub line_height: f64,
    pub title_weight: f64,
}
impl PopoverStyle {
    pub fn resolve(theme: &ResolvedTheme, color: Option<PopupColor>) -> Self {
        Self::from_tokens(&theme.component::<PopoverTheme>(), color)
    }
    pub fn from_tokens(tokens: &PopoverTokens, color: Option<PopupColor>) -> Self {
        let g = tokens.globals();
        let title_padding_top = if g.wireframe {
            (g.control_height - g.font_height) / 2.
        } else {
            0.
        };
        Self {
            background: color
                .map_or(g.color_bg_elevated, |color| color.resolve(g))
                .into(),
            foreground: g.color_text.into(),
            title_color: g.color_text_heading.into(),
            radius: g.border_radius_lg,
            padding: tokens.inner_padding,
            title_min_width: tokens.title_min_width,
            title_margin_bottom: tokens.title_margin_bottom,
            title_padding_top,
            title_padding_bottom: if g.wireframe {
                (title_padding_top - g.line_width).max(0.)
            } else {
                0.
            },
            title_padding_inline: if g.wireframe { g.padding } else { 0. },
            title_border_width: if g.wireframe { g.line_width } else { 0. },
            title_border_color: g.color_split.into(),
            content_padding_inline: if g.wireframe { g.padding } else { 0. },
            content_padding_block: if g.wireframe { g.padding_sm } else { 0. },
            font_size: g.font_size,
            line_height: g.line_height,
            title_weight: g.font_weight_strong,
        }
    }
}
