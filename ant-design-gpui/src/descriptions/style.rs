// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Visual formulas adapted from Ant Design 6.6.7 (MIT).

use std::sync::Arc;

use gpui::{Font, Hsla};

use crate::{
    Size,
    theme::{AliasTokens, Color, ComponentTheme, LineType, ResolvedTheme},
};

pub struct DescriptionsTheme;

#[derive(Clone, Debug, PartialEq)]
pub struct DescriptionsTokens {
    pub label_bg: Color,
    pub label_color: Color,
    pub title_color: Color,
    pub title_margin_bottom: f64,
    pub item_padding_bottom: f64,
    pub item_padding_end: f64,
    pub colon_margin_right: f64,
    pub colon_margin_left: f64,
    pub content_color: Color,
    pub extra_color: Color,
    globals: Arc<AliasTokens>,
    font: Font,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct DescriptionsTokenPatch {
    pub label_bg: Option<Color>,
    pub label_color: Option<Color>,
    pub title_color: Option<Color>,
    pub title_margin_bottom: Option<f64>,
    pub item_padding_bottom: Option<f64>,
    pub item_padding_end: Option<f64>,
    pub colon_margin_right: Option<f64>,
    pub colon_margin_left: Option<f64>,
    pub content_color: Option<Color>,
    pub extra_color: Option<Color>,
}

impl ComponentTheme for DescriptionsTheme {
    type Tokens = DescriptionsTokens;
    type Patch = DescriptionsTokenPatch;

    fn default_tokens(g: &AliasTokens) -> Self::Tokens {
        let mut font = g.font_family.to_gpui_font();
        let mut features = font.features.tag_value_list().to_vec();
        if !features.iter().any(|(tag, _)| tag == "kern") {
            features.push(("kern".into(), 1));
        }
        font.features = gpui::FontFeatures(Arc::new(features));
        DescriptionsTokens {
            label_bg: g.color_fill_alter,
            label_color: g.color_text_tertiary,
            title_color: g.color_text,
            title_margin_bottom: g.font_size_sm * g.line_height_sm,
            item_padding_bottom: g.padding,
            item_padding_end: g.padding,
            colon_margin_right: g.margin_xs,
            colon_margin_left: g.margin_xxs / 2.,
            content_color: g.color_text,
            extra_color: g.color_text,
            globals: Arc::new(g.clone()),
            font,
        }
    }

    fn merge(parent: &Self::Patch, local: &Self::Patch) -> Self::Patch {
        DescriptionsTokenPatch {
            label_bg: local.label_bg.or(parent.label_bg),
            label_color: local.label_color.or(parent.label_color),
            title_color: local.title_color.or(parent.title_color),
            title_margin_bottom: local.title_margin_bottom.or(parent.title_margin_bottom),
            item_padding_bottom: local.item_padding_bottom.or(parent.item_padding_bottom),
            item_padding_end: local.item_padding_end.or(parent.item_padding_end),
            colon_margin_right: local.colon_margin_right.or(parent.colon_margin_right),
            colon_margin_left: local.colon_margin_left.or(parent.colon_margin_left),
            content_color: local.content_color.or(parent.content_color),
            extra_color: local.extra_color.or(parent.extra_color),
        }
    }

    fn apply(tokens: &mut Self::Tokens, patch: &Self::Patch) {
        macro_rules! colors {
            ($($name:ident),+ $(,)?) => {
                $(if let Some(value) = patch.$name { tokens.$name = value; })+
            };
        }
        colors!(
            label_bg,
            label_color,
            title_color,
            content_color,
            extra_color
        );
        macro_rules! lengths {
            ($($name:ident),+ $(,)?) => {
                $(if let Some(value) = patch.$name.filter(|value| {
                    value.is_finite() && (*value as f32).is_finite() && *value >= 0.
                }) { tokens.$name = value; })+
            };
        }
        lengths!(item_padding_bottom, item_padding_end);
        // CSS margins can be negative; padding cannot. Retain that distinction
        // when resolving component overrides into native geometry.
        for (target, value) in [
            (&mut tokens.title_margin_bottom, patch.title_margin_bottom),
            (&mut tokens.colon_margin_right, patch.colon_margin_right),
            (&mut tokens.colon_margin_left, patch.colon_margin_left),
        ] {
            if let Some(value) =
                value.filter(|value| value.is_finite() && (*value as f32).is_finite())
            {
                *target = value;
            }
        }
    }
}

impl DescriptionsTokens {
    pub fn globals(&self) -> &AliasTokens {
        &self.globals
    }

    pub fn font(&self) -> &Font {
        &self.font
    }
}

/// Resolved logical-pixel metrics and native colors. Line heights are multipliers.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct DescriptionsStyle {
    pub font_size: f64,
    pub line_height: f64,
    pub title_font_size: f64,
    pub title_line_height: f64,
    pub title_font_weight: f64,
    pub title_margin_bottom: f64,
    pub item_padding_bottom: f64,
    pub item_padding_end: f64,
    pub cell_padding_vertical: f64,
    pub cell_padding_horizontal: f64,
    pub colon_margin_left: f64,
    pub colon_margin_right: f64,
    pub colon_offset_y: f64,
    pub border_width: f64,
    pub border_radius: f64,
    pub border_line_type: LineType,
    pub border_color: Hsla,
    pub label_background: Hsla,
    pub label_color: Hsla,
    pub content_color: Hsla,
    pub title_color: Hsla,
    pub extra_color: Hsla,
}

impl DescriptionsStyle {
    pub fn resolve(theme: &ResolvedTheme, size: Size, bordered: bool) -> Self {
        Self::from_tokens(&theme.component::<DescriptionsTheme>(), size, bordered)
    }

    pub fn from_tokens(tokens: &DescriptionsTokens, size: Size, bordered: bool) -> Self {
        let g = tokens.globals();
        Self {
            font_size: g.font_size,
            line_height: g.line_height,
            title_font_size: g.font_size_lg,
            title_line_height: g.line_height_lg,
            title_font_weight: g.font_weight_strong,
            title_margin_bottom: tokens.title_margin_bottom,
            item_padding_bottom: match size {
                Size::Large => tokens.item_padding_bottom,
                Size::Medium => g.padding_sm,
                Size::Small => g.padding_xs,
            },
            item_padding_end: tokens.item_padding_end,
            cell_padding_vertical: match size {
                Size::Large => g.padding,
                Size::Medium => g.padding_sm,
                Size::Small => g.padding_xs,
            },
            cell_padding_horizontal: if size == Size::Small {
                g.padding
            } else {
                g.padding_lg
            },
            colon_margin_left: tokens.colon_margin_left,
            colon_margin_right: tokens.colon_margin_right,
            colon_offset_y: -0.5,
            border_width: g.line_width,
            border_radius: g.border_radius_lg,
            border_line_type: g.line_type,
            border_color: g.color_split.into(),
            label_background: tokens.label_bg.into(),
            label_color: if bordered {
                g.color_text_secondary
            } else {
                tokens.label_color
            }
            .into(),
            content_color: tokens.content_color.into(),
            title_color: tokens.title_color.into(),
            extra_color: tokens.extra_color.into(),
        }
    }
}
