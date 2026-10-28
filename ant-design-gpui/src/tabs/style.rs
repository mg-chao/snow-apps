// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Token formulas adapted from Ant Design 6.6.7 (MIT).

use crate::{
    Size,
    theme::{AliasTokens, Color, ComponentTheme},
};
use std::sync::Arc;

/// Physical top/right/bottom/left spacing in logical pixels.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct TabsSpacing {
    pub top: f64,
    pub right: f64,
    pub bottom: f64,
    pub left: f64,
}
impl TabsSpacing {
    pub const fn new(top: f64, right: f64, bottom: f64, left: f64) -> Self {
        Self {
            top,
            right,
            bottom,
            left,
        }
    }
    pub const fn symmetric(vertical: f64, horizontal: f64) -> Self {
        Self::new(vertical, horizontal, vertical, horizontal)
    }
}

pub struct TabsTheme;

macro_rules! tokens {
    ($($field:ident: $ty:ty = $value:expr;)+) => {
        #[derive(Clone, Debug, PartialEq)]
        pub struct TabsTokens {
            $(pub $field: $ty,)+
            pub(super) globals: Arc<AliasTokens>,
        }
        #[derive(Clone, Debug, Default, PartialEq)]
        pub struct TabsTokenPatch { $(pub $field: Option<$ty>,)+ }
        impl ComponentTheme for TabsTheme {
            type Tokens = TabsTokens;
            type Patch = TabsTokenPatch;
            fn default_tokens(g: &AliasTokens) -> Self::Tokens {
                TabsTokens { $($field: ($value)(g),)+ globals: Arc::new(g.clone()) }
            }
            fn merge(parent: &Self::Patch, local: &Self::Patch) -> Self::Patch {
                TabsTokenPatch { $($field: local.$field.or(parent.$field),)+ }
            }
            fn apply(t: &mut Self::Tokens, p: &Self::Patch) {
                $(if let Some(value) = p.$field { t.$field = value; })+
                // Heights participate in derived padding, just as upstream's
                // prepareComponentToken does before explicit padding overrides.
                if p.card_height.is_some() && p.card_padding.is_none() {
                    t.card_padding.top = (t.card_height - t.globals.font_height) / 2. - t.globals.line_width;
                    t.card_padding.bottom = t.card_padding.top;
                }
                if p.card_height_sm.is_some() && p.card_padding_sm.is_none() {
                    t.card_padding_sm.top = (t.card_height_sm - t.globals.font_height) / 2. - t.globals.line_width;
                    t.card_padding_sm.bottom = t.card_padding_sm.top;
                }
                if p.card_height_lg.is_some() && p.card_padding_lg.is_none() {
                    t.card_padding_lg.top = (t.card_height_lg - t.globals.font_height_lg) / 2. - t.globals.line_width;
                    t.card_padding_lg.bottom = t.card_padding_lg.top;
                }
            }
        }
    };
}
tokens! {
    z_index_popup: usize = |g: &AliasTokens| (g.z_index_popup_base + 50.) as usize;
    card_bg: Color = |g: &AliasTokens| g.color_fill_alter;
    card_height: f64 = |g: &AliasTokens| g.control_height_lg;
    card_height_sm: f64 = |g: &AliasTokens| g.control_height;
    card_height_lg: f64 = |g: &AliasTokens| g.control_height_lg + 8.;
    card_padding: TabsSpacing = |g: &AliasTokens| TabsSpacing::symmetric((g.control_height_lg - g.font_height) / 2. - g.line_width, g.padding);
    card_padding_sm: TabsSpacing = |g: &AliasTokens| TabsSpacing::symmetric((g.control_height - g.font_height) / 2. - g.line_width, g.padding_xs);
    card_padding_lg: TabsSpacing = |g: &AliasTokens| TabsSpacing::symmetric((g.control_height_lg + 8. - g.font_height_lg) / 2. - g.line_width, g.padding);
    title_font_size: f64 = |g: &AliasTokens| g.font_size;
    title_font_size_sm: f64 = |g: &AliasTokens| g.font_size;
    title_font_size_lg: f64 = |g: &AliasTokens| g.font_size_lg;
    ink_bar_color: Color = |g: &AliasTokens| g.color_primary;
    horizontal_margin: TabsSpacing = |g: &AliasTokens| TabsSpacing::new(0., 0., g.margin, 0.);
    horizontal_item_gutter: f64 = |_: &AliasTokens| 32.;
    horizontal_item_margin: Option<TabsSpacing> = |_: &AliasTokens| None;
    horizontal_item_margin_rtl: Option<TabsSpacing> = |_: &AliasTokens| None;
    horizontal_item_padding: TabsSpacing = |g: &AliasTokens| TabsSpacing::symmetric(g.padding_sm, 0.);
    horizontal_item_padding_sm: TabsSpacing = |g: &AliasTokens| TabsSpacing::symmetric(g.padding_xs, 0.);
    horizontal_item_padding_lg: TabsSpacing = |g: &AliasTokens| TabsSpacing::symmetric(g.padding, 0.);
    vertical_item_padding: TabsSpacing = |g: &AliasTokens| TabsSpacing::symmetric(g.padding_xs, g.padding_lg);
    vertical_item_margin: TabsSpacing = |g: &AliasTokens| TabsSpacing::new(g.margin, 0., 0., 0.);
    item_color: Color = |g: &AliasTokens| g.color_text;
    item_selected_color: Color = |g: &AliasTokens| g.color_primary;
    item_hover_color: Color = |g: &AliasTokens| g.color_primary_hover;
    item_active_color: Color = |g: &AliasTokens| g.color_primary_active;
    card_gutter: f64 = |g: &AliasTokens| g.margin_xxs / 2.;
}
impl TabsTokens {
    pub(super) fn line_height(&self, size: Size) -> f64 {
        self.font_size(size)
            * if size == Size::Large {
                self.globals.line_height_lg
            } else {
                self.globals.line_height
            }
    }
    pub(super) fn card_radius(&self, size: Size) -> f64 {
        if size == Size::Small {
            self.globals.border_radius
        } else {
            self.globals.border_radius_lg
        }
    }
    pub(super) fn card_height_for(&self, size: Size) -> f64 {
        match size {
            Size::Small => self.card_height_sm,
            Size::Medium => self.card_height,
            Size::Large => self.card_height_lg,
        }
    }
    pub(super) fn label_color(
        &self,
        selected: bool,
        disabled: bool,
        hovered: bool,
        pressed: bool,
    ) -> Color {
        if disabled {
            self.globals.color_text_disabled
        } else if selected {
            self.item_selected_color
        } else if pressed {
            self.item_active_color
        } else if hovered {
            self.item_hover_color
        } else {
            self.item_color
        }
    }
    pub(super) fn font_size(&self, size: Size) -> f64 {
        match size {
            Size::Small => self.title_font_size_sm,
            Size::Medium => self.title_font_size,
            Size::Large => self.title_font_size_lg,
        }
    }
    pub(super) fn padding(&self, size: Size, card: bool, vertical: bool) -> TabsSpacing {
        if card {
            match size {
                Size::Small => self.card_padding_sm,
                Size::Medium => self.card_padding,
                Size::Large => self.card_padding_lg,
            }
        } else if vertical {
            self.vertical_item_padding
        } else {
            match size {
                Size::Small => self.horizontal_item_padding_sm,
                Size::Medium => self.horizontal_item_padding,
                Size::Large => self.horizontal_item_padding_lg,
            }
        }
    }
}
