// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Token formulas adapted from Ant Design 6.6.7 (MIT).

use crate::{
    Size,
    theme::{AliasTokens, ComponentTheme, ResolvedTheme},
};
use gpui::{Font, Hsla};
use std::{sync::Arc, time::Duration};

pub struct SpinTheme;
#[derive(Clone, Debug, PartialEq)]
pub struct SpinTokens {
    /// Retained for reference token compatibility; current Spin does not constrain content height.
    pub content_height: f64,
    pub dot_size: f64,
    pub dot_size_sm: f64,
    pub dot_size_lg: f64,
    globals: Arc<AliasTokens>,
    font: Font,
}
#[derive(Clone, Debug, Default, PartialEq)]
pub struct SpinTokenPatch {
    pub content_height: Option<f64>,
    pub dot_size: Option<f64>,
    pub dot_size_sm: Option<f64>,
    pub dot_size_lg: Option<f64>,
}
impl ComponentTheme for SpinTheme {
    type Tokens = SpinTokens;
    type Patch = SpinTokenPatch;
    fn default_tokens(g: &AliasTokens) -> SpinTokens {
        SpinTokens {
            content_height: 400.,
            dot_size: g.control_height_lg / 2.,
            dot_size_sm: g.control_height_lg * 0.35,
            dot_size_lg: g.control_height,
            globals: Arc::new(g.clone()),
            font: g.font_family.to_gpui_font(),
        }
    }
    fn merge(parent: &SpinTokenPatch, local: &SpinTokenPatch) -> SpinTokenPatch {
        SpinTokenPatch {
            content_height: local.content_height.or(parent.content_height),
            dot_size: local.dot_size.or(parent.dot_size),
            dot_size_sm: local.dot_size_sm.or(parent.dot_size_sm),
            dot_size_lg: local.dot_size_lg.or(parent.dot_size_lg),
        }
    }
    fn apply(tokens: &mut SpinTokens, patch: &SpinTokenPatch) {
        let defaults = Self::default_tokens(&tokens.globals);
        macro_rules! length {
            ($name:ident) => {
                if let Some(value) = patch.$name {
                    tokens.$name = if value.is_finite() && (value as f32).is_finite() {
                        value.max(0.)
                    } else {
                        defaults.$name
                    };
                }
            };
        }
        length!(content_height);
        length!(dot_size);
        length!(dot_size_sm);
        length!(dot_size_lg);
    }
}
impl SpinTokens {
    pub fn globals(&self) -> &AliasTokens {
        &self.globals
    }
    pub fn font(&self) -> &Font {
        &self.font
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SpinStyle {
    pub dot_size: f64,
    pub dot_item_size: f64,
    pub gap: f64,
    pub font_size: f64,
    pub color: Hsla,
    pub description_color: Hsla,
    pub track_color: Hsla,
    pub container_color: Hsla,
    pub mask_color: Hsla,
    pub z_index: usize,
    pub motion_duration: Duration,
    pub fullscreen_duration: Duration,
}
impl SpinStyle {
    pub fn resolve(theme: &ResolvedTheme, size: Size, fullscreen: bool) -> Self {
        Self::from_tokens(&theme.component::<SpinTheme>(), size, fullscreen)
    }
    pub fn from_tokens(tokens: &SpinTokens, size: Size, fullscreen: bool) -> Self {
        let g = tokens.globals();
        let dot_size = match size {
            Size::Small => tokens.dot_size_sm,
            Size::Medium => tokens.dot_size,
            Size::Large => tokens.dot_size_lg,
        };
        Self {
            dot_size,
            dot_item_size: ((dot_size - g.margin_xxs / 2.) / 2.).max(0.),
            gap: g.padding_sm,
            font_size: g.font_size,
            color: if fullscreen {
                g.color_white
            } else {
                g.color_primary
            }
            .into(),
            description_color: if fullscreen {
                g.color_text_light_solid
            } else {
                g.color_primary
            }
            .into(),
            track_color: g.color_fill_secondary.into(),
            container_color: g.color_bg_container.into(),
            mask_color: g.color_bg_mask.into(),
            z_index: g.z_index_popup_base.max(0.) as usize,
            motion_duration: g.motion_duration_slow,
            fullscreen_duration: g.motion_duration_mid,
        }
    }
}
