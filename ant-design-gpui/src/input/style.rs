// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Token defaults and visual rules adapted from Ant Design 6.6.7 (MIT).

use std::sync::Arc;

use gpui::Hsla;

use crate::{
    Size, ValidationStatus, Variant,
    theme::{AliasTokens, Color, ComponentTheme, Shadow, ShadowList},
};

pub struct InputTheme;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct InputInteraction {
    pub disabled: bool,
    pub hovered: bool,
    pub focused: bool,
    pub keyboard_focus: bool,
}

macro_rules! input_tokens {
    ($($name:ident: $ty:ty),* $(,)?) => {
        #[derive(Clone, Debug, PartialEq)]
        pub struct InputTokens {
            $(pub $name: $ty,)*
            pub(crate) globals: Arc<AliasTokens>,
            pub(crate) font: gpui::Font,
        }

        #[derive(Clone, Debug, Default, PartialEq)]
        pub struct InputTokenPatch { $(pub $name: Option<$ty>,)* }

        impl ComponentTheme for InputTheme {
            type Tokens = InputTokens;
            type Patch = InputTokenPatch;

            fn default_tokens(g: &AliasTokens) -> Self::Tokens {
                let shadow = |color: Color| -> ShadowList { Arc::from([Shadow {
                    spread: g.control_outline_width, color, ..Default::default()
                }]) };
                InputTokens {
                    padding_inline: g.padding_sm - g.line_width,
                    padding_inline_sm: g.control_padding_horizontal_sm - g.line_width,
                    padding_inline_lg: g.control_padding_horizontal - g.line_width,
                    padding_block: (((g.control_height - g.font_size * g.line_height) / 2. * 10.).round() / 10. - g.line_width).max(0.),
                    padding_block_sm: (((g.control_height_sm - g.font_size * g.line_height) / 2. * 10.).round() / 10. - g.line_width).max(0.),
                    padding_block_lg: (((g.control_height_lg - g.font_size_lg * g.line_height_lg) / 2. * 10.).ceil() / 10. - g.line_width).max(0.),
                    addon_bg: g.color_fill_alter,
                    active_border_color: g.color_primary,
                    hover_border_color: g.color_primary_hover,
                    active_shadow: shadow(g.control_outline),
                    error_active_shadow: shadow(g.color_error_outline),
                    warning_active_shadow: shadow(g.color_warning_outline),
                    active_bg: g.color_bg_container,
                    hover_bg: g.color_bg_container,
                    input_font_size: g.font_size,
                    input_font_size_sm: g.font_size,
                    input_font_size_lg: g.font_size_lg,
                    globals: Arc::new(g.clone()), font: g.font_family.to_gpui_font(),
                }
            }

            fn merge(parent: &Self::Patch, local: &Self::Patch) -> Self::Patch {
                InputTokenPatch { $($name: local.$name.clone().or_else(|| parent.$name.clone()),)* }
            }

            fn apply(tokens: &mut Self::Tokens, patch: &Self::Patch) {
                $(if let Some(value) = &patch.$name { tokens.$name = value.clone(); })*
                let defaults = Self::default_tokens(&tokens.globals);
                macro_rules! sanitize { ($field:ident) => {
                    if !(tokens.$field as f32).is_finite() || tokens.$field < 0. {
                        tokens.$field = defaults.$field;
                    }
                }; }
                sanitize!(padding_inline); sanitize!(padding_inline_sm); sanitize!(padding_inline_lg);
                sanitize!(padding_block); sanitize!(padding_block_sm); sanitize!(padding_block_lg);
                sanitize!(input_font_size); sanitize!(input_font_size_sm); sanitize!(input_font_size_lg);
                derive_font_padding(tokens, patch);
            }
        }
    };
}

input_tokens! {
    padding_inline: f64, padding_inline_sm: f64, padding_inline_lg: f64,
    padding_block: f64, padding_block_sm: f64, padding_block_lg: f64,
    addon_bg: Color, active_border_color: Color, hover_border_color: Color,
    active_shadow: ShadowList, error_active_shadow: ShadowList, warning_active_shadow: ShadowList,
    active_bg: Color, hover_bg: Color,
    input_font_size: f64, input_font_size_sm: f64, input_font_size_lg: f64,
}

// Ant's default-token factory receives component overrides before explicit
// padding is overlaid. A custom base font also supplies the small-size default.
fn derive_font_padding(tokens: &mut InputTokens, patch: &InputTokenPatch) {
    let g = &tokens.globals;
    let nonzero = |value: f64, fallback: f64| if value == 0. { fallback } else { value };
    let medium = nonzero(tokens.input_font_size, g.font_size);
    if patch.input_font_size.is_some() && patch.input_font_size_sm.is_none() {
        tokens.input_font_size_sm = medium;
    }
    let small = nonzero(tokens.input_font_size_sm, medium);
    let large = nonzero(tokens.input_font_size_lg, g.font_size_lg);
    let padding = |height, font, line_height, ceil: bool| {
        let tenths: f64 = (height - font * line_height) * 5.;
        let rounded = if ceil { tenths.ceil() } else { tenths.round() };
        (rounded / 10. - g.line_width).max(0.)
    };
    if patch.input_font_size.is_some() && patch.padding_block.is_none() {
        tokens.padding_block = padding(g.control_height, medium, g.line_height, false);
    }
    if (patch.input_font_size.is_some() || patch.input_font_size_sm.is_some())
        && patch.padding_block_sm.is_none()
    {
        tokens.padding_block_sm = padding(g.control_height_sm, small, g.line_height, false);
    }
    if patch.input_font_size_lg.is_some() && patch.padding_block_lg.is_none() {
        tokens.padding_block_lg = padding(g.control_height_lg, large, g.line_height_lg, true);
    }
}

pub(crate) fn search_button_style(
    tokens: &InputTokens,
    size: Size,
    variant: Variant,
    primary: bool,
    disabled: bool,
) -> (
    crate::button::ButtonColor,
    crate::button::ButtonVariant,
    crate::button::ButtonStyleOverride,
) {
    use crate::button::{ButtonColor, ButtonStyleOverride, ButtonVariant};
    let g = &tokens.globals;
    let height = match size {
        Size::Small => g.control_height_sm.max(
            tokens.input_font_size_sm * g.line_height
                + 2. * tokens.padding_block_sm
                + 2. * g.line_width,
        ),
        Size::Medium => g.control_height,
        Size::Large => g.control_height_lg,
    };
    (
        if primary {
            ButtonColor::Primary
        } else {
            ButtonColor::Default
        },
        if variant != Variant::Outlined {
            ButtonVariant::Text
        } else if primary {
            ButtonVariant::Solid
        } else {
            ButtonVariant::Outlined
        },
        ButtonStyleOverride {
            height: Some(height),
            backgrounds: (variant == Variant::Filled && !disabled).then_some([
                g.color_fill_tertiary,
                g.color_fill_secondary,
                g.color_fill,
            ]),
        },
    )
}

/// Settled visual values, shared by native rendering and independent reference tests.
#[derive(Clone, Debug)]
pub struct InputStyle {
    pub padding_inline: f64,
    pub padding_block: f64,
    pub font_size: f64,
    pub line_height: f64,
    pub radius: f64,
    pub border_width: f64,
    pub border: Hsla,
    pub background: Hsla,
    pub text: Hsla,
    pub placeholder: Hsla,
    pub affix: Hsla,
    pub shadow: ShadowList,
    pub underline_only: bool,
    pub keyboard_outline: bool,
}

impl InputStyle {
    pub fn from_tokens(
        t: &InputTokens,
        size: Size,
        variant: Variant,
        status: ValidationStatus,
        interaction: InputInteraction,
    ) -> Self {
        let InputInteraction {
            disabled,
            hovered,
            focused,
            keyboard_focus,
        } = interaction;
        let g = &t.globals;
        let (padding_inline, mut padding_block, font_size, line_height, mut radius) = match size {
            Size::Small => (
                t.padding_inline_sm,
                t.padding_block_sm,
                t.input_font_size_sm,
                g.line_height,
                g.border_radius_sm,
            ),
            Size::Medium => (
                t.padding_inline,
                t.padding_block,
                t.input_font_size,
                g.line_height,
                g.border_radius,
            ),
            Size::Large => (
                t.padding_inline_lg,
                t.padding_block_lg,
                t.input_font_size_lg,
                g.line_height_lg,
                g.border_radius_lg,
            ),
        };
        let (
            status_border,
            status_hover,
            status_bg,
            status_bg_hover,
            status_text,
            affix,
            status_shadow,
        ) = match status {
            ValidationStatus::Error => (
                g.color_error,
                g.color_error_border_hover,
                g.color_error_bg,
                g.color_error_bg_hover,
                g.color_error_text,
                g.color_error_affix,
                t.error_active_shadow.clone(),
            ),
            ValidationStatus::Warning => (
                g.color_warning,
                g.color_warning_border_hover,
                g.color_warning_bg,
                g.color_warning_bg_hover,
                g.color_warning_text,
                g.color_warning_affix,
                t.warning_active_shadow.clone(),
            ),
            _ => (
                t.active_border_color,
                t.hover_border_color,
                g.color_fill_tertiary,
                g.color_fill_secondary,
                g.color_text,
                g.color_text,
                t.active_shadow.clone(),
            ),
        };
        let has_status = matches!(status, ValidationStatus::Error | ValidationStatus::Warning);
        let transparent = Hsla::transparent_black();
        let mut border_width = g.line_width;
        let mut border = if has_status {
            status_border
        } else {
            g.color_border
        }
        .to_hsla();
        let mut background = g.color_bg_container.to_hsla();
        let mut text = g.color_text.to_hsla();
        let mut shadow: ShadowList = Arc::from([]);
        let mut keyboard_outline = false;
        match variant {
            Variant::Outlined => {
                if focused && !disabled {
                    border = status_border.to_hsla();
                    background = t.active_bg.to_hsla();
                    shadow = status_shadow;
                } else if hovered && !disabled {
                    border = status_hover.to_hsla();
                    background = t.hover_bg.to_hsla();
                }
                if disabled {
                    background = g.color_bg_container_disabled.to_hsla();
                    border = if has_status {
                        status_border
                    } else {
                        g.color_border_disabled
                    }
                    .to_hsla();
                }
            }
            Variant::Filled => {
                border = transparent;
                background = status_bg.to_hsla();
                text = status_text.to_hsla();
                if focused && !disabled {
                    border = status_border.to_hsla();
                    background = t.active_bg.to_hsla();
                } else if hovered && !disabled {
                    background = status_bg_hover.to_hsla();
                }
                if disabled {
                    background = g.color_bg_container_disabled.to_hsla();
                    border = g.color_border_disabled.to_hsla();
                }
            }
            Variant::Borderless => {
                border_width = 0.;
                padding_block += g.line_width;
                border = transparent;
                background = transparent;
                if has_status {
                    text = status_border.to_hsla();
                }
                keyboard_outline =
                    focused && keyboard_focus && !disabled && g.line_width_focus > 0.;
                if keyboard_outline {
                    border = status_border.to_hsla();
                }
            }
            Variant::Underlined => {
                radius = 0.;
                if focused && !disabled {
                    border = status_border.to_hsla();
                    background = t.active_bg.to_hsla();
                } else if hovered {
                    // The disabled underlined rule restores the border below,
                    // while its base hover background still applies.
                    border = status_hover.to_hsla();
                    background = t.hover_bg.to_hsla();
                }
                if disabled {
                    border = if has_status {
                        status_border
                    } else {
                        g.color_border
                    }
                    .to_hsla();
                }
            }
        }
        if disabled && !(variant == Variant::Borderless && has_status) {
            text = g.color_text_disabled.to_hsla();
        }
        Self {
            padding_inline,
            padding_block,
            font_size,
            line_height: font_size * line_height,
            radius,
            border_width,
            border,
            background,
            text,
            placeholder: g.color_text_placeholder.to_hsla(),
            affix: if disabled {
                g.color_text_disabled
            } else {
                affix
            }
            .to_hsla(),
            shadow,
            underline_only: variant == Variant::Underlined,
            keyboard_outline,
        }
    }

    /// CSS snaps positive border widths down to physical pixels (at least one).
    /// Inputs derive their height from their contents and borders, so use the
    /// same width for native layout and painting at fractional display scales.
    pub(crate) fn at_scale(mut self, scale: f32) -> Self {
        if self.border_width > 0. && scale.is_finite() && scale > 0. {
            self.border_width =
                (self.border_width * f64::from(scale)).floor().max(1.) / f64::from(scale);
        }
        self
    }

    pub(crate) fn multiline(mut self, tokens: &InputTokens) -> Self {
        self.line_height = self.font_size * tokens.globals.line_height;
        self
    }

    pub(crate) fn affix(
        mut self,
        tokens: &InputTokens,
        size: Size,
        variant: Variant,
        multiline: bool,
    ) -> Self {
        // The wrapper's shared padding rule follows the medium borderless
        // rule. Sized selectors have higher specificity. Wrapped textareas
        // instead take padding from their unbordered inner textarea.
        if variant == Variant::Borderless && (size == Size::Medium || multiline) {
            self.padding_block = (self.padding_block - tokens.globals.line_width).max(0.);
        }
        self
    }

    pub(crate) fn numeric(
        mut self,
        tokens: &InputTokens,
        size: Size,
        variant: Variant,
        status: ValidationStatus,
        disabled: bool,
    ) -> Self {
        // InputNumber's small-size rule follows the base variant rule. Enabled
        // validation selectors have greater specificity and retain square ends.
        if size == Size::Small
            && variant == Variant::Underlined
            && (status == ValidationStatus::None || disabled)
        {
            self.radius = tokens.globals.border_radius_sm;
        }
        self
    }
}

#[cfg(test)]
mod reference_tests;

#[cfg(test)]
mod composite_tests;
