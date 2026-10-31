// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Visual token formulas adapted from Ant Design 6.6.7 (MIT).

use crate::theme::{AliasTokens, Color, ComponentTheme};
use std::{sync::Arc, time::Duration};

pub struct ImageTheme;
#[derive(Clone, Debug, PartialEq)]
pub struct ImageTokens {
    globals: Arc<AliasTokens>,
    pub z_index_popup: usize,
    pub preview_operation_size: f32,
    pub preview_operation_color: Color,
    pub preview_operation_hover_color: Color,
    pub preview_operation_color_disabled: Color,
    pub progress_animation_duration: Duration,
}
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ImageTokenPatch {
    pub z_index_popup: Option<usize>,
    pub preview_operation_size: Option<f32>,
    pub preview_operation_color: Option<Color>,
    pub preview_operation_hover_color: Option<Color>,
    pub preview_operation_color_disabled: Option<Color>,
    pub progress_animation_duration: Option<Duration>,
}
impl ComponentTheme for ImageTheme {
    type Tokens = ImageTokens;
    type Patch = ImageTokenPatch;
    fn default_tokens(g: &AliasTokens) -> ImageTokens {
        ImageTokens {
            globals: Arc::new(g.clone()),
            z_index_popup: g.z_index_popup_base as usize + 80,
            preview_operation_size: g.font_size_icon as f32 * 1.5,
            preview_operation_color: g.color_text_light_solid.with_alpha(0.65),
            preview_operation_hover_color: g.color_text_light_solid.with_alpha(0.85),
            preview_operation_color_disabled: g.color_text_light_solid.with_alpha(0.25),
            progress_animation_duration: Duration::from_secs(3),
        }
    }
    fn merge(p: &ImageTokenPatch, l: &ImageTokenPatch) -> ImageTokenPatch {
        ImageTokenPatch {
            z_index_popup: l.z_index_popup.or(p.z_index_popup),
            preview_operation_size: l.preview_operation_size.or(p.preview_operation_size),
            preview_operation_color: l.preview_operation_color.or(p.preview_operation_color),
            preview_operation_hover_color: l
                .preview_operation_hover_color
                .or(p.preview_operation_hover_color),
            preview_operation_color_disabled: l
                .preview_operation_color_disabled
                .or(p.preview_operation_color_disabled),
            progress_animation_duration: l
                .progress_animation_duration
                .or(p.progress_animation_duration),
        }
    }
    fn apply(t: &mut ImageTokens, p: &ImageTokenPatch) {
        macro_rules! apply { ($($field:ident),+) => { $(if let Some(value)=p.$field {t.$field=value;})+ }; }
        apply!(
            z_index_popup,
            preview_operation_size,
            preview_operation_color,
            preview_operation_hover_color,
            preview_operation_color_disabled,
            progress_animation_duration
        );
    }
}
impl ImageTokens {
    pub fn globals(&self) -> &AliasTokens {
        &self.globals
    }
}
