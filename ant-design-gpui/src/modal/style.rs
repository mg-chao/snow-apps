// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Token formulas adapted from Ant Design 6.6.7 (MIT).
use super::{ModalKind, ModalLength, ModalWidth};
use crate::theme::{AliasTokens, Color, ComponentTheme};

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct ModalPadding {
    pub vertical: f64,
    pub horizontal: f64,
}
impl ModalPadding {
    pub const fn new(vertical: f64, horizontal: f64) -> Self {
        Self {
            vertical,
            horizontal,
        }
    }
}
pub struct ModalTheme;

macro_rules! tokens {
    ($($name:ident: $ty:ty = $value:expr;)+) => {
        #[derive(Clone, Debug, PartialEq)] pub struct ModalTokens { $(pub $name: $ty,)+ }
        #[derive(Clone, Debug, Default, PartialEq)] pub struct ModalTokenPatch { $(pub $name: Option<$ty>,)+ }
        impl ComponentTheme for ModalTheme {
            type Tokens = ModalTokens; type Patch = ModalTokenPatch;
            fn default_tokens(g: &AliasTokens) -> ModalTokens { ModalTokens { $($name: ($value)(g),)+ } }
            fn merge(p: &ModalTokenPatch, l: &ModalTokenPatch) -> ModalTokenPatch {
                ModalTokenPatch { $($name: l.$name.or(p.$name),)+ }
            }
            fn apply(t: &mut ModalTokens, p: &ModalTokenPatch) { $(if let Some(v) = &p.$name { t.$name = *v; })+ }
        }
    }
}
tokens! {
    header_bg: Color = |_: &AliasTokens| Color::TRANSPARENT;
    content_bg: Color = |g: &AliasTokens| g.color_bg_elevated;
    footer_bg: Color = |_: &AliasTokens| Color::TRANSPARENT;
    title_color: Color = |g: &AliasTokens| g.color_text_heading;
    title_font_size: f64 = |g: &AliasTokens| g.font_size_heading5;
    title_line_height: f64 = |g: &AliasTokens| g.line_height_heading5;
    content_padding: ModalPadding = |g: &AliasTokens| if g.wireframe { ModalPadding::default() } else { ModalPadding::new(g.padding_md, g.padding_content_horizontal_lg) };
    header_padding: ModalPadding = |g: &AliasTokens| if g.wireframe { ModalPadding::new(g.padding, g.padding_lg) } else { ModalPadding::default() };
    header_border_width: f64 = |g: &AliasTokens| if g.wireframe { g.line_width } else { 0. };
    header_margin_bottom: f64 = |g: &AliasTokens| if g.wireframe { 0. } else { g.margin_xs };
    body_padding: f64 = |g: &AliasTokens| if g.wireframe { g.padding_lg } else { 0. };
    footer_padding: ModalPadding = |g: &AliasTokens| if g.wireframe { ModalPadding::new(g.padding_xs, g.padding) } else { ModalPadding::default() };
    footer_border_width: f64 = |g: &AliasTokens| if g.wireframe { g.line_width } else { 0. };
    footer_margin_top: f64 = |g: &AliasTokens| if g.wireframe { 0. } else { g.margin_sm };
    confirm_body_padding: ModalPadding = |g: &AliasTokens| if g.wireframe { ModalPadding::new(g.padding * 2., g.padding * 2.) } else { ModalPadding::default() };
    confirm_body_padding_bottom: f64 = |g: &AliasTokens| if g.wireframe { g.padding_lg } else { 0. };
    confirm_icon_margin: f64 = |g: &AliasTokens| if g.wireframe { g.margin } else { g.margin_sm };
    confirm_buttons_margin_top: f64 = |g: &AliasTokens| if g.wireframe { g.margin_lg } else { g.margin_sm };
}

pub(super) fn width(
    width: Option<&ModalWidth>,
    kind: ModalKind,
    viewport: f32,
    g: &AliasTokens,
) -> f32 {
    let default = ModalLength::Pixels(if kind == ModalKind::Standard {
        520.
    } else {
        416.
    });
    let chosen = match width {
        None => default,
        Some(ModalWidth::Fixed(value)) => *value,
        Some(ModalWidth::Responsive(values)) => {
            let breaks = [
                0.,
                g.screen_sm,
                g.screen_md,
                g.screen_lg,
                g.screen_xl,
                g.screen_xxl,
            ];
            values
                .iter()
                .zip(breaks)
                .filter(|(_, b)| f64::from(viewport) >= *b)
                .filter_map(|(v, _)| *v)
                .next_back()
                .unwrap_or(default)
        }
    };
    let value = match chosen {
        ModalLength::Pixels(v) => v,
        ModalLength::Fraction(v) => viewport * v,
    };
    let gutter = if f64::from(viewport) <= g.screen_sm_max {
        16.
    } else {
        (g.margin * 2.) as f32
    };
    let maximum = (viewport - gutter).max(0.);
    if value.is_finite() {
        value.max(0.).min(maximum)
    } else {
        maximum.min(520.)
    }
}
