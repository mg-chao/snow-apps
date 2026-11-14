// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Visual rules adapted from Ant Design 6.6.7 (MIT).

use crate::{
    popup::{PopupColor, PopupPlacement},
    theme::{AliasTokens, Color, ComponentTheme, ResolvedTheme},
};
use gpui::{Font, Hsla};
use std::sync::Arc;

pub struct TooltipTheme;

#[derive(Clone, Debug, PartialEq)]
pub struct TooltipTokens {
    pub max_width: f64,
    pub z_index_popup: usize,
    globals: Arc<AliasTokens>,
    font: Font,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct TooltipTokenPatch {
    pub max_width: Option<f64>,
    pub z_index_popup: Option<usize>,
}

impl ComponentTheme for TooltipTheme {
    type Tokens = TooltipTokens;
    type Patch = TooltipTokenPatch;
    fn default_tokens(g: &AliasTokens) -> TooltipTokens {
        TooltipTokens {
            max_width: 250.,
            z_index_popup: g.z_index_popup_base as usize + 70,
            globals: Arc::new(g.clone()),
            font: g.font_family.to_gpui_font(),
        }
    }
    fn merge(parent: &Self::Patch, local: &Self::Patch) -> Self::Patch {
        TooltipTokenPatch {
            max_width: local.max_width.or(parent.max_width),
            z_index_popup: local.z_index_popup.or(parent.z_index_popup),
        }
    }
    fn apply(tokens: &mut Self::Tokens, patch: &Self::Patch) {
        if let Some(value) = patch.max_width {
            tokens.max_width = if value.is_finite() {
                value.max(0.)
            } else {
                tokens.max_width
            };
        }
        if let Some(value) = patch.z_index_popup {
            tokens.z_index_popup = value;
        }
    }
}
impl TooltipTokens {
    pub(crate) fn globals(&self) -> &AliasTokens {
        &self.globals
    }
    pub(crate) fn font(&self) -> &Font {
        &self.font
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct TooltipStyle {
    pub background: Hsla,
    pub foreground: Hsla,
    pub radius: f64,
    pub max_width: f64,
    pub min_width: f64,
    pub min_height: f64,
    pub padding_inline: f64,
    pub padding_block: f64,
    pub font_size: f64,
    pub line_height: f64,
}
impl TooltipStyle {
    pub fn resolve(
        theme: &ResolvedTheme,
        color: Option<PopupColor>,
        placement: PopupPlacement,
    ) -> Self {
        Self::from_tokens(&theme.component::<TooltipTheme>(), color, placement)
    }
    pub fn from_tokens(
        tokens: &TooltipTokens,
        color: Option<PopupColor>,
        placement: PopupPlacement,
    ) -> Self {
        let g = tokens.globals();
        let background = color.map_or(g.color_bg_spotlight, |color| color.resolve(g));
        let foreground = if matches!(color, Some(PopupColor::Custom(_))) {
            if (0.299 * f64::from(background.r)
                + 0.587 * f64::from(background.g)
                + 0.114 * f64::from(background.b))
                / 255.
                < 0.5
            {
                Color::WHITE
            } else {
                Color::BLACK
            }
        } else {
            g.color_text_light_solid
        };
        let radius = if placement.is_vertical() {
            g.border_radius
        } else {
            g.border_radius.min(8.)
        };
        let min_width = if placement.is_vertical() && !placement.is_centered() {
            g.border_radius
                + g.size_popup_arrow
                + if g.border_radius > 12. {
                    g.border_radius + 2.
                } else {
                    12.
                }
        } else {
            2. * g.border_radius + g.size_popup_arrow
        };
        Self {
            background: background.into(),
            foreground: foreground.into(),
            radius,
            max_width: tokens.max_width,
            min_width,
            min_height: g.control_height,
            padding_inline: g.padding_xs,
            padding_block: g.padding_sm / 2.,
            font_size: g.font_size,
            line_height: g.line_height,
        }
    }
}
