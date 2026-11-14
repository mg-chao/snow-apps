// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Visual token defaults and state rules adapted from Ant Design 6.6.7 (MIT).

//! Pure, cached Button token derivation and native visual states.

use std::sync::{Arc, OnceLock};

use crate::{
    Size,
    theme::{
        AliasTokens, Color, ComponentTheme, LineType, PresetColor, ResolvedTheme, Shadow,
        ShadowList,
    },
};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum ButtonType {
    #[default]
    Default,
    Primary,
    Dashed,
    Text,
    Link,
}

impl ButtonType {
    pub fn appearance(self) -> (ButtonColor, ButtonVariant) {
        match self {
            Self::Default => (ButtonColor::Default, ButtonVariant::Outlined),
            Self::Primary => (ButtonColor::Primary, ButtonVariant::Solid),
            Self::Dashed => (ButtonColor::Default, ButtonVariant::Dashed),
            Self::Text => (ButtonColor::Default, ButtonVariant::Text),
            Self::Link => (ButtonColor::Link, ButtonVariant::Link),
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum ButtonVariant {
    #[default]
    Outlined,
    Dashed,
    Solid,
    Filled,
    Text,
    Link,
}

impl ButtonVariant {
    pub const ALL: [Self; 6] = [
        Self::Outlined,
        Self::Dashed,
        Self::Solid,
        Self::Filled,
        Self::Text,
        Self::Link,
    ];

    pub fn is_unbordered(self) -> bool {
        matches!(self, Self::Text | Self::Link)
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum ButtonColor {
    #[default]
    Default,
    Primary,
    Danger,
    Preset(PresetColor),
    /// The legacy link type follows the independently customizable link palette.
    #[doc(hidden)]
    Link,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum ButtonShape {
    #[default]
    Default,
    /// Ant Design's square shape retains the standard control corner radius.
    Square,
    Circle,
    Round,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum IconPlacement {
    #[default]
    Start,
    End,
}

/// Marker for `ComponentThemes::set::<ButtonTheme>(...)`.
pub struct ButtonTheme;

macro_rules! button_tokens {
    ($($field:ident: $ty:ty = $value:expr;)+) => {
        /// Native Button component tokens. Lengths are logical pixels.
        #[derive(Clone, Debug, PartialEq)]
        pub struct ButtonTokens {
            $(pub $field: $ty,)+
            // Keeping the effective aliases with the component also preserves
            // component-local global overrides and algorithm chains.
            globals: Arc<AliasTokens>,
            font: gpui::Font,
        }

        #[derive(Clone, Debug, Default, PartialEq)]
        pub struct ButtonTokenPatch { $(pub $field: Option<$ty>,)+ }

        impl ComponentTheme for ButtonTheme {
            type Tokens = ButtonTokens;
            type Patch = ButtonTokenPatch;

            fn default_tokens(global: &AliasTokens) -> Self::Tokens {
                Self::Tokens { $($field: ($value)(global),)+ globals: Arc::new(global.clone()), font: global.font_family.to_gpui_font() }
            }

            fn merge(parent: &Self::Patch, local: &Self::Patch) -> Self::Patch {
                Self::Patch { $($field: clone_value(&local.$field).or_else(|| clone_value(&parent.$field)),)+ }
            }

            fn apply(tokens: &mut Self::Tokens, patch: &Self::Patch) {
                $(if let Some(value) = &patch.$field { tokens.$field = clone_value(value); })+
            }
        }
    };
}

fn clone_value<T: Clone>(value: &T) -> T {
    value.clone()
}

button_tokens! {
    font_weight: f64 = |_: &AliasTokens| 400.;
    icon_gap: f64 = |g: &AliasTokens| g.margin_xs;
    default_shadow: ShadowList = |g: &AliasTokens| shadow(g.control_outline_width, g.control_tmp_outline);
    primary_shadow: ShadowList = |g: &AliasTokens| shadow(g.control_outline_width, g.control_outline);
    danger_shadow: ShadowList = |g: &AliasTokens| shadow(g.control_outline_width, g.color_error_outline);
    primary_color: Color = |g: &AliasTokens| g.color_text_light_solid;
    danger_color: Color = |g: &AliasTokens| g.color_text_light_solid;
    default_color: Color = |g: &AliasTokens| g.color_text;
    default_bg: Color = |g: &AliasTokens| g.color_bg_container;
    default_border_color: Color = |g: &AliasTokens| g.color_border;
    default_hover_bg: Color = |g: &AliasTokens| g.color_bg_container;
    default_hover_color: Color = |g: &AliasTokens| g.color_primary_hover;
    default_hover_border_color: Color = |g: &AliasTokens| g.color_primary_hover;
    default_active_bg: Color = |g: &AliasTokens| g.color_bg_container;
    default_active_color: Color = |g: &AliasTokens| g.color_primary_active;
    default_active_border_color: Color = |g: &AliasTokens| g.color_primary_active;
    border_color_disabled: Color = |g: &AliasTokens| g.color_border_disabled;
    default_ghost_color: Color = |g: &AliasTokens| g.color_bg_container;
    ghost_bg: Color = |_: &AliasTokens| Color::TRANSPARENT;
    default_ghost_border_color: Color = |g: &AliasTokens| g.color_bg_container;
    solid_text_color: Color = |g: &AliasTokens| if is_bright(g.color_bg_solid) { Color::BLACK } else { Color::WHITE };
    text_text_color: Color = |g: &AliasTokens| g.color_text;
    text_text_hover_color: Color = |g: &AliasTokens| g.color_text;
    text_text_active_color: Color = |g: &AliasTokens| g.color_text;
    padding_inline: f64 = |g: &AliasTokens| g.padding_content_horizontal - g.line_width;
    padding_inline_lg: f64 = |g: &AliasTokens| g.padding_content_horizontal - g.line_width;
    padding_inline_sm: f64 = |g: &AliasTokens| 8. - g.line_width;
    // None inherits the content font size, as Ant's inherit icon-size token does.
    only_icon_size: Option<f64> = |_: &AliasTokens| None;
    only_icon_size_lg: Option<f64> = |_: &AliasTokens| None;
    only_icon_size_sm: Option<f64> = |_: &AliasTokens| None;
    link_hover_bg: Color = |_: &AliasTokens| Color::TRANSPARENT;
    text_hover_bg: Color = |g: &AliasTokens| g.color_fill_tertiary;
    content_font_size: f64 = |g: &AliasTokens| g.font_size;
    content_font_size_lg: f64 = |g: &AliasTokens| g.font_size_lg;
    content_font_size_sm: f64 = |g: &AliasTokens| g.font_size;
    default_bg_disabled: Color = |g: &AliasTokens| g.color_bg_container_disabled;
    dashed_bg_disabled: Color = |g: &AliasTokens| g.color_bg_container_disabled;
    preset_shadows: [ShadowList; 13] = |g: &AliasTokens| std::array::from_fn(|index| shadow(g.control_outline_width, g.palettes.get(PresetColor::ALL[index]).tones[0].alpha_over(g.color_bg_container)));
}

impl ButtonTokens {
    pub(crate) fn globals(&self) -> &AliasTokens {
        &self.globals
    }

    pub(crate) fn font(&self) -> &gpui::Font {
        &self.font
    }
}

fn shadow(y: f64, color: Color) -> ShadowList {
    Arc::from([Shadow {
        y,
        color,
        ..Default::default()
    }])
}

fn no_shadow() -> ShadowList {
    static EMPTY: OnceLock<ShadowList> = OnceLock::new();
    EMPTY.get_or_init(|| Arc::from([])).clone()
}

fn is_bright(color: Color) -> bool {
    if color.a <= 0.5 {
        let max = color.r.max(color.g).max(color.b);
        // The reference evaluates HSV after compositing to byte RGB on white.
        // Rounding matters at exactly half alpha: black becomes 128, not 127.5.
        (f64::from(max) * color.a + 255. * (1. - color.a)).round() / 255. > 0.5
    } else {
        f64::from(color.r) * 0.299 + f64::from(color.g) * 0.587 + f64::from(color.b) * 0.114 > 192.
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct ButtonStyleOptions {
    pub size: Size,
    pub color: ButtonColor,
    pub variant: ButtonVariant,
    pub shape: ButtonShape,
    pub ghost: bool,
    pub disabled: bool,
    pub icon_only: bool,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ButtonMetrics {
    pub height: f64,
    pub padding_inline: f64,
    pub font_size: f64,
    /// Absolute line height in logical pixels.
    pub line_height: f64,
    pub radius: f64,
    pub min_width: f64,
    pub icon_size: f64,
    pub icon_gap: f64,
    pub font_weight: f64,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ButtonStateStyle {
    pub background: Color,
    pub border: Color,
    pub text: Color,
}

#[derive(Clone, Debug, PartialEq)]
pub struct ButtonStyle {
    pub rest: ButtonStateStyle,
    pub hover: ButtonStateStyle,
    pub active: ButtonStateStyle,
    pub metrics: ButtonMetrics,
    pub line_width: f64,
    pub line_type: LineType,
    pub shadow: ShadowList,
    pub focus_color: Color,
    pub focus_width: f64,
    pub focus_offset: f64,
    pub loading_opacity: f64,
}

/// Component composition can refine settled states before Button animates them.
/// Keeping this as values avoids allocating a render callback for every control.
#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct ButtonStyleOverride {
    pub height: Option<f64>,
    pub backgrounds: Option<[Color; 3]>,
}

impl ButtonStyleOverride {
    pub(crate) fn apply(self, style: &mut ButtonStyle, icon_only: bool) {
        if let Some(height) = self.height {
            style.metrics.height = height;
            if icon_only {
                style.metrics.min_width = height;
            }
        }
        if let Some([rest, hover, active]) = self.backgrounds {
            style.rest.background = rest;
            style.hover.background = hover;
            style.active.background = active;
        }
    }
}

impl ButtonStyle {
    /// Resolve from cached component tokens without palette generation or global merging.
    pub fn resolve(theme: &ResolvedTheme, options: ButtonStyleOptions) -> Self {
        let tokens = theme.component::<ButtonTheme>();
        Self::from_tokens(&tokens, options)
    }

    pub fn from_tokens(tokens: &ButtonTokens, options: ButtonStyleOptions) -> Self {
        let g = tokens.globals.as_ref();
        let ButtonStyleOptions {
            size,
            color,
            mut variant,
            shape,
            ghost,
            disabled,
            icon_only,
        } = options;
        let ghost = ghost && !variant.is_unbordered();
        if ghost && variant == ButtonVariant::Solid {
            variant = ButtonVariant::Outlined;
        }
        let (height, padding_inline, font_size, radius, only_icon_size) = match size {
            Size::Small => (
                g.control_height_sm,
                tokens.padding_inline_sm,
                tokens.content_font_size_sm,
                g.border_radius_sm,
                tokens.only_icon_size_sm,
            ),
            Size::Medium => (
                g.control_height,
                tokens.padding_inline,
                tokens.content_font_size,
                g.border_radius,
                tokens.only_icon_size,
            ),
            Size::Large => (
                g.control_height_lg,
                tokens.padding_inline_lg,
                tokens.content_font_size_lg,
                g.border_radius_lg,
                tokens.only_icon_size_lg,
            ),
        };
        let metrics = ButtonMetrics {
            height,
            padding_inline: if icon_only || shape == ButtonShape::Circle {
                0.
            } else {
                padding_inline.max(0.)
            },
            font_size,
            line_height: font_size * g.line_height,
            radius: match shape {
                ButtonShape::Circle => height / 2.,
                ButtonShape::Round => height,
                _ => radius,
            },
            min_width: if icon_only || shape == ButtonShape::Circle {
                height
            } else {
                0.
            },
            icon_size: if icon_only {
                only_icon_size.unwrap_or(font_size)
            } else {
                font_size
            },
            icon_gap: tokens.icon_gap,
            font_weight: tokens.font_weight,
        };
        let (base, hover, active, light, light_hover, light_active, shadow) = match color {
            ButtonColor::Default => (
                tokens.default_border_color,
                tokens.default_hover_border_color,
                tokens.default_active_border_color,
                g.color_fill_tertiary,
                g.color_fill_secondary,
                g.color_fill,
                tokens.default_shadow.clone(),
            ),
            ButtonColor::Primary => (
                g.color_primary,
                g.color_primary_hover,
                g.color_primary_active,
                g.color_primary_bg,
                g.color_primary_bg_hover,
                g.color_primary_border,
                tokens.primary_shadow.clone(),
            ),
            ButtonColor::Danger => (
                g.color_error,
                g.color_error_hover,
                g.color_error_active,
                g.color_error_bg,
                g.color_error_bg_filled_hover,
                g.color_error_bg_active,
                tokens.danger_shadow.clone(),
            ),
            ButtonColor::Link => (
                g.color_link,
                g.color_link_hover,
                g.color_link_active,
                g.color_primary_bg,
                g.color_primary_bg_hover,
                g.color_primary_border,
                no_shadow(),
            ),
            ButtonColor::Preset(preset) => {
                let p = g.palettes.get(preset);
                (
                    p.tones[5],
                    p.hover,
                    p.active,
                    p.tones[0],
                    p.tones[1],
                    p.tones[2],
                    tokens.preset_shadows[preset as usize].clone(),
                )
            }
        };
        let transparent = Color::TRANSPARENT;
        let state = |background, border, text| ButtonStateStyle {
            background,
            border,
            text,
        };
        let mut states = match variant {
            ButtonVariant::Solid => {
                let (base, hover, active) = if color == ButtonColor::Default {
                    (
                        g.color_bg_solid,
                        g.color_bg_solid_hover,
                        g.color_bg_solid_active,
                    )
                } else {
                    (base, hover, active)
                };
                let text = match color {
                    ButtonColor::Default => tokens.solid_text_color,
                    ButtonColor::Primary => tokens.primary_color,
                    ButtonColor::Danger => tokens.danger_color,
                    _ => g.color_text_light_solid,
                };
                [
                    state(base, transparent, text),
                    state(hover, transparent, text),
                    state(active, transparent, text),
                ]
            }
            ButtonVariant::Outlined | ButtonVariant::Dashed => {
                if color == ButtonColor::Default {
                    [
                        state(tokens.default_bg, base, tokens.default_color),
                        state(tokens.default_hover_bg, hover, tokens.default_hover_color),
                        state(
                            tokens.default_active_bg,
                            active,
                            tokens.default_active_color,
                        ),
                    ]
                } else {
                    [
                        state(g.color_bg_container, base, base),
                        state(g.color_bg_container, hover, hover),
                        state(g.color_bg_container, active, active),
                    ]
                }
            }
            ButtonVariant::Filled => {
                let text = if color == ButtonColor::Default {
                    tokens.default_color
                } else {
                    base
                };
                [
                    state(light, transparent, text),
                    state(light_hover, transparent, text),
                    state(light_active, transparent, text),
                ]
            }
            ButtonVariant::Text => {
                if color == ButtonColor::Default {
                    [
                        state(transparent, transparent, tokens.text_text_color),
                        state(
                            tokens.text_hover_bg,
                            transparent,
                            tokens.text_text_hover_color,
                        ),
                        state(light_active, transparent, tokens.text_text_active_color),
                    ]
                } else {
                    [
                        state(transparent, transparent, base),
                        state(light, transparent, hover),
                        state(light_active, transparent, active),
                    ]
                }
            }
            ButtonVariant::Link => {
                if color == ButtonColor::Default {
                    [
                        state(transparent, transparent, tokens.default_color),
                        state(
                            tokens.link_hover_bg,
                            transparent,
                            tokens.default_hover_color,
                        ),
                        state(transparent, transparent, tokens.default_active_color),
                    ]
                } else {
                    [
                        state(transparent, transparent, base),
                        state(tokens.link_hover_bg, transparent, hover),
                        state(transparent, transparent, active),
                    ]
                }
            }
        };
        let mut shadow = if matches!(
            variant,
            ButtonVariant::Solid | ButtonVariant::Outlined | ButtonVariant::Dashed
        ) {
            shadow
        } else {
            no_shadow()
        };
        if ghost {
            for state in &mut states {
                state.background = tokens.ghost_bg;
            }
            if color == ButtonColor::Default
                && matches!(variant, ButtonVariant::Outlined | ButtonVariant::Dashed)
            {
                states[0].text = tokens.default_ghost_color;
                states[0].border = tokens.default_ghost_border_color;
            }
            shadow = no_shadow();
        }
        if disabled {
            let background = match variant {
                ButtonVariant::Text | ButtonVariant::Link => transparent,
                ButtonVariant::Outlined if color == ButtonColor::Default => {
                    tokens.default_bg_disabled
                }
                ButtonVariant::Dashed => tokens.dashed_bg_disabled,
                _ => g.color_bg_container_disabled,
            };
            let border = if variant.is_unbordered() {
                transparent
            } else {
                tokens.border_color_disabled
            };
            states = [state(background, border, g.color_text_disabled); 3];
            shadow = no_shadow();
        }
        Self {
            rest: states[0],
            hover: states[1],
            active: states[2],
            metrics,
            line_width: g.line_width,
            line_type: if variant == ButtonVariant::Dashed {
                LineType::Dashed
            } else {
                g.line_type
            },
            shadow,
            focus_color: g.color_primary_border,
            focus_width: if disabled { 0. } else { g.line_width_focus },
            focus_offset: 1.,
            loading_opacity: g.opacity_loading,
        }
    }
}

#[cfg(test)]
#[path = "style_tests.rs"]
mod tests;

#[cfg(test)]
#[path = "reference_tests.rs"]
mod reference_tests;
