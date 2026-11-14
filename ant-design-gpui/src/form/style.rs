// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Visual formulas adapted from Ant Design 6.6.7 (MIT).

use std::sync::Arc;

use gpui::{Font, Hsla};

use crate::{
    Size,
    theme::{AliasTokens, Color, ComponentTheme, FontStack, ResolvedTheme},
};

pub struct FormTheme;

#[derive(Clone, Debug, PartialEq)]
pub struct FormTokens {
    pub label_required_mark_color: Color,
    pub label_color: Color,
    pub label_font_size: f64,
    pub label_height: f64,
    /// `None` is the reference's intrinsic `auto` vertical label height.
    pub vertical_label_height: Option<f64>,
    pub label_colon_margin_inline_start: f64,
    pub label_colon_margin_inline_end: f64,
    pub item_margin_bottom: f64,
    pub inline_item_margin_bottom: f64,
    pub vertical_label_padding: f64,
    pub vertical_label_margin: f64,
    globals: Arc<AliasTokens>,
    font: Font,
    required_mark_font: Font,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct FormTokenPatch {
    pub label_required_mark_color: Option<Color>,
    pub label_color: Option<Color>,
    pub label_font_size: Option<f64>,
    pub label_height: Option<f64>,
    pub vertical_label_height: Option<f64>,
    pub label_colon_margin_inline_start: Option<f64>,
    pub label_colon_margin_inline_end: Option<f64>,
    pub item_margin_bottom: Option<f64>,
    pub inline_item_margin_bottom: Option<f64>,
    pub vertical_label_padding: Option<f64>,
    pub vertical_label_margin: Option<f64>,
}

impl ComponentTheme for FormTheme {
    type Tokens = FormTokens;
    type Patch = FormTokenPatch;

    fn default_tokens(g: &AliasTokens) -> FormTokens {
        let mut font = g.font_family.to_gpui_font();
        let mut features = font.features.tag_value_list().to_vec();
        if !features.iter().any(|(tag, _)| tag == "kern") {
            features.push(("kern".into(), 1));
        }
        font.features = gpui::FontFeatures(Arc::new(features));
        FormTokens {
            label_required_mark_color: g.color_error,
            label_color: g.color_text_heading,
            label_font_size: g.font_size,
            label_height: g.control_height,
            vertical_label_height: None,
            label_colon_margin_inline_start: g.margin_xxs / 2.,
            label_colon_margin_inline_end: g.margin_xs,
            item_margin_bottom: g.margin_lg,
            inline_item_margin_bottom: 0.,
            vertical_label_padding: g.padding_xs,
            vertical_label_margin: 0.,
            globals: Arc::new(g.clone()),
            font,
            required_mark_font: FontStack::new(["sans-serif"]).to_gpui_font(),
        }
    }

    fn merge(parent: &FormTokenPatch, local: &FormTokenPatch) -> FormTokenPatch {
        macro_rules! merge { ($($field:ident),+ $(,)?) => { FormTokenPatch {
            $($field: local.$field.or(parent.$field),)+
        } }; }
        merge!(
            label_required_mark_color,
            label_color,
            label_font_size,
            label_height,
            vertical_label_height,
            label_colon_margin_inline_start,
            label_colon_margin_inline_end,
            item_margin_bottom,
            inline_item_margin_bottom,
            vertical_label_padding,
            vertical_label_margin
        )
    }

    fn apply(t: &mut FormTokens, p: &FormTokenPatch) {
        if let Some(value) = p.label_required_mark_color {
            t.label_required_mark_color = value;
        }
        if let Some(value) = p.label_color {
            t.label_color = value;
        }
        macro_rules! lengths { ($($field:ident),+ $(,)?) => { $(
            if let Some(value) = p.$field.filter(|v| v.is_finite() && (*v as f32).is_finite() && *v >= 0.) {
                t.$field = value;
            }
        )+ }; }
        lengths!(label_font_size, label_height, vertical_label_padding);
        if let Some(value) = p
            .vertical_label_height
            .filter(|v| v.is_finite() && (*v as f32).is_finite() && *v >= 0.)
        {
            t.vertical_label_height = Some(value);
        }
        // CSS margins, unlike dimensions and padding, may be negative.
        macro_rules! margins { ($($field:ident),+ $(,)?) => { $(
            if let Some(value) = p.$field.filter(|v| v.is_finite() && (*v as f32).is_finite()) {
                t.$field = value;
            }
        )+ }; }
        margins!(
            label_colon_margin_inline_start,
            label_colon_margin_inline_end,
            item_margin_bottom,
            inline_item_margin_bottom,
            vertical_label_margin
        );
    }
}

impl FormTokens {
    pub fn globals(&self) -> &AliasTokens {
        &self.globals
    }
    pub fn font(&self) -> &Font {
        &self.font
    }
    pub fn required_mark_font(&self) -> &Font {
        &self.required_mark_font
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct FormStyle {
    pub font_size: f64,
    pub line_height: f64,
    pub label_font_size: f64,
    pub label_height: f64,
    pub vertical_label_height: Option<f64>,
    pub control_height: f64,
    pub extra_min_height: f64,
    pub item_margin_bottom: f64,
    pub inline_item_margin_bottom: f64,
    pub inline_item_margin_end: f64,
    pub vertical_label_padding: f64,
    pub vertical_label_margin: f64,
    pub colon_margin_start: f64,
    pub colon_margin_end: f64,
    pub mark_margin: f64,
    pub label_color: Hsla,
    pub text_color: Hsla,
    pub description_color: Hsla,
    pub required_color: Hsla,
    pub error_color: Hsla,
    pub warning_color: Hsla,
    pub success_color: Hsla,
    pub primary_color: Hsla,
}

impl FormStyle {
    pub fn resolve(theme: &ResolvedTheme, size: Size) -> Self {
        Self::from_tokens(&theme.component::<FormTheme>(), size)
    }
    pub fn from_tokens(t: &FormTokens, size: Size) -> Self {
        let g = t.globals();
        let control_height = match size {
            Size::Small => g.control_height_sm,
            Size::Medium => g.control_height,
            Size::Large => g.control_height_lg,
        };
        Self {
            font_size: g.font_size,
            line_height: g.line_height,
            label_font_size: t.label_font_size,
            label_height: if size == Size::Medium {
                t.label_height
            } else {
                control_height
            },
            vertical_label_height: t.vertical_label_height,
            control_height,
            extra_min_height: g.control_height_sm,
            item_margin_bottom: t.item_margin_bottom,
            inline_item_margin_bottom: t.inline_item_margin_bottom,
            inline_item_margin_end: g.margin,
            vertical_label_padding: t.vertical_label_padding,
            vertical_label_margin: t.vertical_label_margin,
            colon_margin_start: t.label_colon_margin_inline_start,
            colon_margin_end: t.label_colon_margin_inline_end,
            mark_margin: g.margin_xxs,
            label_color: t.label_color.into(),
            text_color: g.color_text.into(),
            description_color: g.color_text_description.into(),
            required_color: t.label_required_mark_color.into(),
            error_color: g.color_error.into(),
            warning_color: g.color_warning.into(),
            success_color: g.color_success.into(),
            primary_color: g.color_primary.into(),
        }
    }
}
