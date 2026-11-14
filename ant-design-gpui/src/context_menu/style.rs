// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use crate::theme::{AliasTokens, Color, ComponentTheme};

pub struct ContextMenuTheme;
macro_rules! tokens {
    ($($name:ident: $ty:ty = $default:expr;)+) => {
        #[derive(Clone,Debug,PartialEq)]
        pub struct ContextMenuTokens {$(pub $name:$ty,)+}
        #[derive(Clone,Debug,Default,PartialEq)]
        pub struct ContextMenuTokenPatch {$(pub $name:Option<$ty>,)+}
        impl ComponentTheme for ContextMenuTheme {
            type Tokens=ContextMenuTokens; type Patch=ContextMenuTokenPatch;
            fn default_tokens(g:&AliasTokens)->Self::Tokens {ContextMenuTokens {$($name:($default)(g),)+}}
            fn merge(parent:&Self::Patch,local:&Self::Patch)->Self::Patch {ContextMenuTokenPatch {$($name:local.$name.or(parent.$name),)+}}
            fn apply(tokens:&mut Self::Tokens,patch:&Self::Patch) {$(if let Some(value)=patch.$name {tokens.$name=value;})+}
        }
    }
}
tokens! {
    item_height:f64 = |g:&AliasTokens| g.control_height;
    padding:f64 = |g:&AliasTokens|g.padding_xxs;
    padding_inline:f64 = |g:&AliasTokens|g.padding_sm;
    icon_size:f64 = |g:&AliasTokens|g.font_size;
    gap:f64 = |g:&AliasTokens|g.margin_xs;
    radius:f64 = |g:&AliasTokens|g.border_radius_lg;
    item_radius:f64 = |g:&AliasTokens|g.border_radius_sm;
    minimum_width:f64 = |_:&AliasTokens|160.;
    background:Color = |g:&AliasTokens|g.color_bg_elevated;
    text:Color = |g:&AliasTokens|g.color_text;
    secondary_text:Color = |g:&AliasTokens|g.color_text_description;
    disabled_text:Color = |g:&AliasTokens|g.color_text_disabled;
    hover_background:Color = |g:&AliasTokens|g.control_item_bg_hover;
    danger_text:Color = |g:&AliasTokens|g.color_error;
    danger_hover_background:Color = |g:&AliasTokens|g.color_error_bg;
    divider:Color = |g:&AliasTokens|g.color_split;
    checkmark:Color = |g:&AliasTokens|g.color_primary;
}
pub(super) fn metric(value: f64, fallback: f32) -> gpui::Pixels {
    gpui::px(if value.is_finite() && value >= 0. {
        value.min(10000.) as f32
    } else {
        fallback
    })
}
