// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Visual rules adapted from Ant Design 6.6.7 (MIT).

use crate::{
    Size, ValidationStatus, Variant,
    input::{InputInteraction, InputStyle},
    theme::{AliasTokens, Color, ComponentTheme, Shadow},
};
use std::sync::Arc;

pub struct SelectTheme;
macro_rules! lengths {
    ($tokens:ident, $defaults:ident; $($field:ident),+) => { $(
        if !($tokens.$field as f32).is_finite() || $tokens.$field < 0. {
            $tokens.$field = $defaults.$field;
        }
    )+ };
}
macro_rules! tokens {
    ($($name:ident: $ty:ty = $default:expr;)+) => {
        #[derive(Clone, Debug, PartialEq)]
        pub struct SelectTokens { $(pub $name: $ty,)+ pub(crate) globals: Arc<AliasTokens> }
        #[derive(Clone, Debug, Default, PartialEq)]
        pub struct SelectTokenPatch { $(pub $name: Option<$ty>,)+ }
        impl ComponentTheme for SelectTheme {
            type Tokens = SelectTokens;
            type Patch = SelectTokenPatch;
            fn default_tokens(g: &AliasTokens) -> SelectTokens {
                SelectTokens { $($name: ($default)(g),)+ globals: Arc::new(g.clone()) }
            }
            fn merge(parent: &Self::Patch, local: &Self::Patch) -> Self::Patch {
                SelectTokenPatch { $($name: local.$name.or(parent.$name),)+ }
            }
            fn apply(tokens: &mut Self::Tokens, patch: &Self::Patch) {
                $(if let Some(value) = patch.$name { tokens.$name = value; })+
                let defaults = Self::default_tokens(&tokens.globals);
                lengths!(tokens, defaults; option_font_size, option_line_height, option_height, option_padding_inline,
                    option_padding_block, single_item_height_lg, multiple_item_height,
                    multiple_item_height_sm, multiple_item_height_lg, show_arrow_padding_inline_end);
            }
        }
    };
}
tokens! {
    z_index_popup: usize = |g: &AliasTokens| g.z_index_popup_base as usize + 50;
    option_selected_color: Color = |g: &AliasTokens| g.color_text;
    option_selected_font_weight: f32 = |g: &AliasTokens| g.font_weight_strong as f32;
    option_selected_bg: Color = |g: &AliasTokens| g.control_item_bg_active;
    option_active_bg: Color = |g: &AliasTokens| g.control_item_bg_hover;
    option_padding_inline: f64 = |g: &AliasTokens| g.control_padding_horizontal;
    option_padding_block: f64 = |g: &AliasTokens| (g.control_height - g.font_size * g.line_height) / 2.;
    option_font_size: f64 = |g: &AliasTokens| g.font_size;
    option_line_height: f64 = |g: &AliasTokens| g.line_height;
    option_height: f64 = |g: &AliasTokens| g.control_height;
    selector_bg: Color = |g: &AliasTokens| g.color_bg_container;
    clear_bg: Color = |g: &AliasTokens| g.color_bg_container;
    single_item_height_lg: f64 = |g: &AliasTokens| g.control_height_lg;
    multiple_item_bg: Color = |g: &AliasTokens| g.color_fill_secondary;
    multiple_item_border_color: Color = |_: &AliasTokens| Color::rgba(0);
    multiple_item_height: f64 = |g: &AliasTokens| (g.control_height - 2. * g.padding_xxs).min(g.control_height - 2. * g.line_width);
    multiple_item_height_sm: f64 = |g: &AliasTokens| (g.control_height_sm - 2. * g.padding_xxs).min(g.control_height_sm - 2. * g.line_width);
    multiple_item_height_lg: f64 = |g: &AliasTokens| (g.control_height_lg - 2. * g.padding_xxs).min(g.control_height_lg - 2. * g.line_width);
    multiple_selector_bg_disabled: Color = |g: &AliasTokens| g.color_bg_container_disabled;
    multiple_item_color_disabled: Color = |g: &AliasTokens| g.color_text_disabled;
    multiple_item_border_color_disabled: Color = |_: &AliasTokens| Color::rgba(0);
    show_arrow_padding_inline_end: f64 = |g: &AliasTokens| (g.font_size * 1.25).ceil();
    hover_border_color: Color = |g: &AliasTokens| g.color_primary_hover;
    active_border_color: Color = |g: &AliasTokens| g.color_primary;
    active_outline_color: Color = |g: &AliasTokens| g.control_outline;
}
impl SelectTokens {
    pub fn globals(&self) -> &AliasTokens {
        &self.globals
    }
    pub fn height(&self, size: Size) -> f64 {
        match size {
            Size::Small => self.globals.control_height_sm,
            Size::Medium => self.globals.control_height,
            Size::Large => self.globals.control_height_lg,
        }
    }
    pub fn tag_height(&self, size: Size) -> f64 {
        match size {
            Size::Small => self.multiple_item_height_sm,
            Size::Medium => self.multiple_item_height,
            Size::Large => self.multiple_item_height_lg,
        }
    }
    pub fn tag_radius(&self, size: Size) -> f64 {
        match size {
            Size::Small => self.globals.border_radius_xs,
            Size::Medium => self.globals.border_radius_sm,
            Size::Large => self.globals.border_radius,
        }
    }
    pub fn surface(
        &self,
        size: Size,
        variant: Variant,
        status: ValidationStatus,
        multiple: bool,
        interaction: InputInteraction,
    ) -> InputStyle {
        let g = &self.globals;
        let InputInteraction {
            disabled,
            hovered,
            focused,
            keyboard_focus,
        } = interaction;
        let (font_size, line_height, radius, padding) = match size {
            Size::Small => (g.font_size, g.line_height, g.border_radius_sm, g.padding_xs),
            Size::Medium => (g.font_size, g.line_height, g.border_radius, g.padding_sm),
            Size::Large => (
                g.font_size_lg,
                g.line_height_lg,
                g.border_radius_lg,
                g.padding_sm,
            ),
        };
        let transparent = Color::rgba(0);
        let (normal, hover, active, outline, filled, filled_hover, affix) = match status {
            ValidationStatus::Error => (
                g.color_error,
                g.color_error_border_hover,
                g.color_error,
                g.color_error_outline,
                g.color_error_bg,
                g.color_error_bg_hover,
                g.color_error_affix,
            ),
            ValidationStatus::Warning => (
                g.color_warning,
                g.color_warning_hover,
                g.color_warning,
                g.color_warning_outline,
                g.color_warning_bg,
                g.color_warning_bg_hover,
                g.color_warning_affix,
            ),
            _ => (
                g.color_border,
                self.hover_border_color,
                self.active_border_color,
                self.active_outline_color,
                g.color_fill_tertiary,
                g.color_fill_secondary,
                g.color_text,
            ),
        };
        let mut border = if focused {
            active
        } else if hovered {
            hover
        } else {
            normal
        };
        let mut background = self.selector_bg;
        let mut shadow_color = outline;
        match variant {
            Variant::Outlined => {}
            Variant::Filled => {
                background = if focused {
                    g.color_bg_container
                } else if hovered {
                    filled_hover
                } else {
                    filled
                };
                if !focused {
                    border = transparent;
                }
                shadow_color = transparent;
            }
            Variant::Borderless => {
                border = transparent;
                background = transparent;
                shadow_color = transparent;
            }
            Variant::Underlined => {
                shadow_color = transparent;
            }
        }
        if disabled {
            border = match variant {
                Variant::Borderless => transparent,
                Variant::Underlined => normal,
                _ => g.color_border_disabled,
            };
            background = if multiple {
                self.multiple_selector_bg_disabled
            } else {
                g.color_bg_container_disabled
            };
        }
        let line_height = font_size * line_height;
        let padding_block = if multiple {
            (self.height(size) - self.tag_height(size)) / 2.
                - (g.padding_xxs / 2.).floor()
                - g.line_width
        } else {
            (self.height(size) - line_height) / 2. - g.line_width
        };
        InputStyle {
            padding_inline: (padding - g.line_width).max(0.),
            padding_block: padding_block.max(0.),
            font_size,
            line_height,
            radius: if variant == Variant::Underlined {
                0.
            } else {
                radius
            },
            border_width: g.line_width,
            border: border.to_hsla(),
            background: background.to_hsla(),
            text: if disabled {
                g.color_text_disabled
            } else {
                g.color_text
            }
            .to_hsla(),
            placeholder: g.color_text_placeholder.to_hsla(),
            affix: if variant == Variant::Outlined {
                affix
            } else {
                g.color_text
            }
            .to_hsla(),
            shadow: if focused && !disabled && shadow_color.a != 0. {
                Arc::from([Shadow {
                    spread: g.control_outline_width,
                    color: shadow_color,
                    ..Default::default()
                }])
            } else {
                Arc::from([])
            },
            underline_only: variant == Variant::Underlined,
            keyboard_outline: variant == Variant::Borderless
                && focused
                && keyboard_focus
                && !disabled
                && g.line_width_focus > 0.,
        }
    }
}
