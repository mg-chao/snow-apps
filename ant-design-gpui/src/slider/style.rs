// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Visual defaults adapted from Ant Design 6.6.7 (MIT).

use crate::theme::{AliasTokens, Color, ComponentTheme};
use std::sync::Arc;

pub struct SliderTheme;
macro_rules! lengths {
    ($tokens:ident, $defaults:ident; $($field:ident),+) => { $(
        if !$tokens.$field.is_finite() || $tokens.$field < 0. || !($tokens.$field as f32).is_finite() {
            $tokens.$field = $defaults.$field;
        }
    )+ };
}
macro_rules! tokens {
    ($($name:ident: $ty:ty = $default:expr;)+) => {
        #[derive(Clone, Debug, PartialEq)]
        pub struct SliderTokens { $(pub $name: $ty,)+ globals: Arc<AliasTokens> }
        #[derive(Clone, Debug, Default, PartialEq)]
        pub struct SliderTokenPatch { $(pub $name: Option<$ty>,)+ }
        impl ComponentTheme for SliderTheme {
            type Tokens = SliderTokens;
            type Patch = SliderTokenPatch;
            fn default_tokens(g: &AliasTokens) -> SliderTokens {
                SliderTokens { $($name: ($default)(g),)+ globals: Arc::new(g.clone()) }
            }
            fn merge(parent: &Self::Patch, local: &Self::Patch) -> Self::Patch {
                SliderTokenPatch { $($name: local.$name.or(parent.$name),)+ }
            }
            fn apply(tokens: &mut Self::Tokens, patch: &Self::Patch) {
                $(if let Some(value) = patch.$name { tokens.$name = value; })+
                let defaults = Self::default_tokens(&tokens.globals);
                lengths!(tokens, defaults; control_size, rail_size, handle_size, handle_size_hover, dot_size,
                    handle_line_width, handle_line_width_hover);
            }
        }
    };
}
tokens! {
    control_size: f64 = |g: &AliasTokens| g.control_height_lg / 4.;
    rail_size: f64 = |_: &AliasTokens| 4.;
    handle_size: f64 = |g: &AliasTokens| g.control_height_lg / 4.;
    handle_size_hover: f64 = |g: &AliasTokens| g.control_height_sm / 2.;
    dot_size: f64 = |_: &AliasTokens| 8.;
    handle_line_width: f64 = |g: &AliasTokens| g.line_width + 1.;
    handle_line_width_hover: f64 = |g: &AliasTokens| g.line_width + 1.5;
    rail_bg: Color = |g: &AliasTokens| g.color_fill_tertiary;
    rail_hover_bg: Color = |g: &AliasTokens| g.color_fill_secondary;
    track_bg: Color = |g: &AliasTokens| g.color_primary_border;
    track_hover_bg: Color = |g: &AliasTokens| g.color_primary_border_hover;
    handle_color: Color = |g: &AliasTokens| g.color_primary_border;
    handle_active_color: Color = |g: &AliasTokens| g.color_primary;
    handle_active_outline_color: Color = |g: &AliasTokens| g.color_primary.with_alpha(0.2);
    handle_color_disabled: Color = |g: &AliasTokens| composite(g.color_text_disabled, g.color_bg_container);
    dot_border_color: Color = |g: &AliasTokens| g.color_border_secondary;
    dot_active_border_color: Color = |g: &AliasTokens| g.color_primary_border;
    track_bg_disabled: Color = |g: &AliasTokens| g.color_bg_container_disabled;
}
impl SliderTokens {
    pub fn globals(&self) -> &AliasTokens {
        &self.globals
    }
    pub fn margin_cross(&self) -> f64 {
        (self.globals.control_height - self.control_size) / 2.
    }
    pub fn margin_axis(&self) -> f64 {
        self.control_size / 2.
    }
    pub fn margin_with_marks(&self) -> f64 {
        self.globals.control_height_lg - self.control_size
    }
}
fn composite(front: Color, back: Color) -> Color {
    let channel =
        |a: u8, b: u8| (f64::from(a) * front.a + f64::from(b) * (1. - front.a)).round() as u8;
    Color {
        r: channel(front.r, back.r),
        g: channel(front.g, back.g),
        b: channel(front.b, back.b),
        a: 1.,
    }
}
