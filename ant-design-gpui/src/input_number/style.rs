// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Visual defaults adapted from Ant Design 6.6.7 (MIT).

use crate::{
    input::{InputTheme, InputTokenPatch, InputTokens},
    theme::{AliasTokens, Color, ComponentTheme},
};

pub struct InputNumberTheme;

macro_rules! number_tokens {
    ($($field:ident: $ty:ty),* $(,)?) => {
        #[derive(Clone, Debug, PartialEq)]
        pub struct InputNumberTokens { pub input: InputTokens, $(pub $field: $ty,)* }
        #[derive(Clone, Debug, Default, PartialEq)]
        pub struct InputNumberTokenPatch { pub input: InputTokenPatch, $(pub $field: Option<$ty>,)* }
        impl ComponentTheme for InputNumberTheme {
            type Tokens = InputNumberTokens;
            type Patch = InputNumberTokenPatch;
            fn default_tokens(g: &AliasTokens) -> Self::Tokens {
                let foreground = g.color_fill_secondary;
                let background = g.color_bg_container;
                let channel = |front: u8, back: u8| (f64::from(front) * foreground.a + f64::from(back) * (1. - foreground.a)).round() as u8;
                Self::Tokens {
                    input: InputTheme::default_tokens(g), control_width: 90., handle_width: g.control_height_sm - g.line_width * 2.,
                    handle_font_size: g.font_size / 2., handle_visible: false, handle_bg: g.color_bg_container,
                    handle_active_bg: g.color_fill_alter, handle_hover_color: g.color_primary, handle_border_color: g.color_border,
                    filled_handle_bg: Color { r: channel(foreground.r, background.r), g: channel(foreground.g, background.g), b: channel(foreground.b, background.b), a: 1. },
                }
            }
            fn merge(parent: &Self::Patch, local: &Self::Patch) -> Self::Patch {
                Self::Patch { input: InputTheme::merge(&parent.input, &local.input), $($field: local.$field.or(parent.$field),)* }
            }
            fn apply(tokens: &mut Self::Tokens, patch: &Self::Patch) {
                InputTheme::apply(&mut tokens.input, &patch.input);
                $(if let Some(value) = patch.$field { tokens.$field = value; })*
                let defaults = Self::default_tokens(&tokens.input.globals);
                macro_rules! length { ($name:ident) => { if !(tokens.$name as f32).is_finite() || tokens.$name < 0. { tokens.$name = defaults.$name; } }; }
                length!(control_width); length!(handle_width); length!(handle_font_size);
            }
        }
    };
}
number_tokens! {
    control_width: f64, handle_width: f64, handle_font_size: f64, handle_visible: bool,
    handle_bg: Color, handle_active_bg: Color, handle_hover_color: Color, handle_border_color: Color, filled_handle_bg: Color,
}
