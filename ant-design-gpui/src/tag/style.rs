// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Visual state rules adapted from Ant Design 6.6.7 (MIT).

//! Cached native Tag tokens and allocation-free visual state resolution.

use std::sync::Arc;

use gpui::Hsla;

use super::{TagColor, TagVariant};
use crate::{
    icons::IconPalette,
    theme::{AliasTokens, Color, ComponentTheme, ResolvedTheme},
};

/// Marker for component-local token overrides and algorithms.
pub struct TagTheme;

#[derive(Clone, Debug, PartialEq)]
pub struct TagTokens {
    pub default_bg: Color,
    pub default_color: Color,
    pub solid_text_color: Color,
    globals: Arc<AliasTokens>,
    font: gpui::Font,
    icon_palette: IconPalette,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct TagTokenPatch {
    pub default_bg: Option<Color>,
    pub default_color: Option<Color>,
    pub solid_text_color: Option<Color>,
}

impl ComponentTheme for TagTheme {
    type Tokens = TagTokens;
    type Patch = TagTokenPatch;

    fn default_tokens(global: &AliasTokens) -> Self::Tokens {
        TagTokens {
            default_bg: composite(global.color_fill_tertiary, global.color_bg_container),
            default_color: global.color_text,
            solid_text_color: if is_bright(global.color_bg_solid) {
                Color::BLACK
            } else {
                Color::WHITE
            },
            globals: Arc::new(global.clone()),
            font: global.font_family.to_gpui_font(),
            icon_palette: IconPalette::from_theme(global),
        }
    }

    fn merge(parent: &Self::Patch, local: &Self::Patch) -> Self::Patch {
        TagTokenPatch {
            default_bg: local.default_bg.or(parent.default_bg),
            default_color: local.default_color.or(parent.default_color),
            solid_text_color: local.solid_text_color.or(parent.solid_text_color),
        }
    }

    fn apply(tokens: &mut Self::Tokens, patch: &Self::Patch) {
        if let Some(value) = patch.default_bg {
            tokens.default_bg = value;
        }
        if let Some(value) = patch.default_color {
            tokens.default_color = value;
        }
        if let Some(value) = patch.solid_text_color {
            tokens.solid_text_color = value;
        }
    }
}

impl TagTokens {
    pub(crate) fn globals(&self) -> &AliasTokens {
        &self.globals
    }

    pub(crate) fn font(&self) -> &gpui::Font {
        &self.font
    }

    pub(crate) fn icon_palette(&self) -> IconPalette {
        self.icon_palette
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct TagStateStyle {
    pub background: Hsla,
    pub border: Hsla,
    pub foreground: Hsla,
    pub close: Hsla,
}

/// Logical-pixel dimensions. A transparent border preserves identical geometry
/// across all variants, selected states, and disabled states.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct TagStyle {
    pub rest: TagStateStyle,
    pub hover: TagStateStyle,
    pub active: TagStateStyle,
    pub font_size: f64,
    pub line_height: f64,
    pub line_width: f64,
    pub radius: f64,
    pub padding_inline: f64,
    pub icon_size: f64,
    pub close_icon_size: f64,
    pub close_gap: f64,
    pub group_gap: f64,
    pub focus_color: Hsla,
    pub focus_width: f64,
}

impl TagStyle {
    pub fn resolve(
        theme: &ResolvedTheme,
        color: TagColor,
        variant: TagVariant,
        checkable: Option<bool>,
        disabled: bool,
    ) -> Self {
        Self::from_tokens(
            &theme.component::<TagTheme>(),
            color,
            variant,
            checkable,
            disabled,
        )
    }

    pub fn from_tokens(
        tokens: &TagTokens,
        color: TagColor,
        variant: TagVariant,
        checkable: Option<bool>,
        disabled: bool,
    ) -> Self {
        let g = tokens.globals();
        let transparent = Color::TRANSPARENT.to_hsla();
        let mut rest = TagStateStyle {
            background: tokens.default_bg.into(),
            border: if variant == TagVariant::Outlined {
                g.color_border.into()
            } else {
                transparent
            },
            foreground: tokens.default_color.into(),
            close: g.color_icon.into(),
        };
        if variant == TagVariant::Solid {
            rest.background = g.color_bg_solid.into();
            rest.foreground = tokens.solid_text_color.into();
        }
        match color {
            TagColor::Default => {}
            TagColor::Custom(custom) => {
                if variant == TagVariant::Solid {
                    rest.background = custom.into();
                    rest.foreground = g.color_text_light_solid.into();
                } else {
                    rest.background = custom_background(custom).into();
                    rest.foreground = custom.into();
                    if variant == TagVariant::Outlined {
                        rest.border = custom.into();
                    }
                }
            }
            _ => {
                let (background, border, foreground, solid) = match color {
                    TagColor::Preset(preset) => {
                        let p = g.palettes.get(preset);
                        (p.tones[0], p.tones[2], p.tones[6], p.tones[5])
                    }
                    TagColor::Success => (
                        g.color_success_bg,
                        g.color_success_border,
                        g.color_success,
                        g.color_success,
                    ),
                    TagColor::Processing => (
                        g.color_info_bg,
                        g.color_info_border,
                        g.color_info,
                        g.color_info,
                    ),
                    TagColor::Error => (
                        g.color_error_bg,
                        g.color_error_border,
                        g.color_error,
                        g.color_error,
                    ),
                    TagColor::Warning => (
                        g.color_warning_bg,
                        g.color_warning_border,
                        g.color_warning,
                        g.color_warning,
                    ),
                    TagColor::Default | TagColor::Custom(_) => unreachable!(),
                };
                if variant == TagVariant::Solid {
                    rest.background = solid.into();
                    rest.border = solid.into();
                    rest.foreground = g.color_text_light_solid.into();
                } else {
                    rest.background = background.into();
                    rest.foreground = foreground.into();
                    if variant == TagVariant::Outlined {
                        rest.border = border.into();
                    }
                }
            }
        }
        let mut hover = rest;
        hover.close = g.color_text_heading.into();
        let mut active = rest;

        if let Some(checked) = checkable {
            rest.background = if checked {
                g.color_primary.into()
            } else {
                transparent
            };
            rest.border = transparent;
            rest.foreground = if checked {
                g.color_text_light_solid.into()
            } else {
                tokens.default_color.into()
            };
            hover = rest;
            hover.background = if checked {
                g.color_primary_hover.into()
            } else {
                g.color_fill_secondary.into()
            };
            if !checked {
                hover.foreground = g.color_primary.into();
            }
            active = rest;
            active.background = g.color_primary_active.into();
            active.foreground = g.color_text_light_solid.into();
        }

        if disabled {
            rest.background = if checkable == Some(false) {
                transparent
            } else {
                g.color_bg_container_disabled.into()
            };
            rest.border = if checkable.is_none() && variant == TagVariant::Outlined {
                g.color_border_disabled.into()
            } else {
                transparent
            };
            rest.foreground = g.color_text_disabled.into();
            rest.close = g.color_text_disabled.into();
            hover = rest;
            active = rest;
            // An unchecked disabled Tag stays transparent on hover, but the
            // reference still gives its pressed state the disabled fill.
            if checkable == Some(false) {
                active.background = g.color_bg_container_disabled.into();
            }
        }

        Self {
            rest,
            hover,
            active,
            font_size: g.font_size_sm,
            line_height: g.line_height_sm * g.font_size_sm,
            line_width: g.line_width,
            radius: g.border_radius_sm,
            padding_inline: (8. - g.line_width).max(0.),
            icon_size: g.font_size_sm,
            close_icon_size: (g.font_size_icon - 2. * g.line_width).max(0.),
            close_gap: (g.padding_xxs - g.line_width).max(0.),
            group_gap: g.padding_xs,
            focus_color: g.color_primary_border.into(),
            focus_width: if disabled || !g.focus_outline {
                0.
            } else {
                g.line_width_focus
            },
        }
    }
}

fn composite(foreground: Color, background: Color) -> Color {
    let alpha = foreground.a + background.a * (1. - foreground.a);
    if alpha == 0. {
        return Color::TRANSPARENT;
    }
    let channel = |front: u8, back: u8| {
        ((f64::from(front) * foreground.a + f64::from(back) * background.a * (1. - foreground.a))
            / alpha)
            .round() as u8
    };
    Color {
        r: channel(foreground.r, background.r),
        g: channel(foreground.g, background.g),
        b: channel(foreground.b, background.b),
        a: (alpha * 255.).round() / 255.,
    }
}

fn is_bright(color: Color) -> bool {
    if color.a <= 0.5 {
        let on_white = composite(color, Color::WHITE);
        f64::from(on_white.r.max(on_white.g).max(on_white.b)) / 255. > 0.5
    } else {
        f64::from(color.r) * 0.299 + f64::from(color.g) * 0.587 + f64::from(color.b) * 0.114 > 192.
    }
}

/// Ant's custom-color fill keeps its rounded HSL hue/saturation, sets lightness
/// to 95%, and quantizes alpha when serializing to a hex color. Double precision
/// keeps half-channel rounding identical to the reference's color conversion.
fn custom_background(color: Color) -> Color {
    let max = f64::from(color.r.max(color.g).max(color.b));
    let min = f64::from(color.r.min(color.g).min(color.b));
    let delta = max - min;
    let original_lightness = (max + min) / 510.;
    let saturation = if delta == 0. {
        0.
    } else {
        delta / 255. / (1. - (2. * original_lightness - 1.).abs())
    };
    let hue = color.hsv().0 / 60.;
    let lightness = 0.95;
    let chroma = (1. - (2_f64 * lightness - 1.).abs()) * saturation;
    let second = chroma * (1. - ((hue % 2.) - 1.).abs());
    let (r, g, b) = match hue.floor() as u32 {
        0 => (chroma, second, 0.),
        1 => (second, chroma, 0.),
        2 => (0., chroma, second),
        3 => (0., second, chroma),
        4 => (second, 0., chroma),
        _ => (chroma, 0., second),
    };
    let modification = lightness - chroma / 2.;
    Color {
        r: ((r + modification) * 255.).round() as u8,
        g: ((g + modification) * 255.).round() as u8,
        b: ((b + modification) * 255.).round() as u8,
        a: (color.a * 255.).round() / 255.,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::theme::{
        AlgorithmChain, ComponentThemeConfig, PresetColor, ThemeConfig, TokenPatch,
    };

    #[test]
    fn tag_default_metrics_and_variants_match_reference() {
        let theme = ResolvedTheme::default();
        let filled = TagStyle::resolve(&theme, TagColor::Default, TagVariant::Filled, None, false);
        assert_eq!(filled.font_size, 12.);
        assert_eq!(filled.line_height, 20.);
        assert_eq!(filled.line_width, 1.);
        assert_eq!(filled.radius, 4.);
        assert_eq!(filled.padding_inline, 7.);
        assert_eq!(filled.icon_size, 12.);
        assert_eq!(filled.close_icon_size, 10.);
        assert_eq!(filled.close_gap, 3.);
        assert_eq!(filled.group_gap, 8.);
        assert_eq!(filled.rest.background, Color::rgb(0xf5f5f5).to_hsla());
        assert_eq!(filled.rest.border.a, 0.);
        assert_eq!(filled.rest.foreground, theme.tokens.color_text.to_hsla());
        assert_eq!(filled.rest.close, theme.tokens.color_icon.to_hsla());
        assert_eq!(
            filled.hover.close,
            theme.tokens.color_text_heading.to_hsla()
        );
        let outlined =
            TagStyle::resolve(&theme, TagColor::Default, TagVariant::Outlined, None, false);
        assert_eq!(outlined.rest.border, Color::rgb(0xd9d9d9).to_hsla());
        assert_eq!(outlined.rest.background, filled.rest.background);
        let solid = TagStyle::resolve(&theme, TagColor::Default, TagVariant::Solid, None, false);
        assert_eq!(solid.rest.background, theme.tokens.color_bg_solid.to_hsla());
        assert_eq!(solid.rest.foreground, Color::WHITE.to_hsla());
        assert_eq!(solid.rest.border.a, 0.);
    }

    #[test]
    fn tag_preset_and_status_colors_follow_effective_theme() {
        for algorithms in [
            AlgorithmChain::default(),
            AlgorithmChain::dark(),
            AlgorithmChain::compact(),
            AlgorithmChain::dark_compact(),
        ] {
            let theme = ThemeConfig {
                algorithms: Some(algorithms),
                ..Default::default()
            }
            .resolve();
            for color in PresetColor::ALL {
                let palette = theme.tokens.palettes.get(color);
                let outlined = TagStyle::resolve(
                    &theme,
                    TagColor::Preset(color),
                    TagVariant::Outlined,
                    None,
                    false,
                );
                assert_eq!(outlined.rest.background, palette.tones[0].to_hsla());
                assert_eq!(outlined.rest.border, palette.tones[2].to_hsla());
                assert_eq!(outlined.rest.foreground, palette.tones[6].to_hsla());
                let solid = TagStyle::resolve(
                    &theme,
                    TagColor::Preset(color),
                    TagVariant::Solid,
                    None,
                    false,
                );
                assert_eq!(solid.rest.background, palette.tones[5].to_hsla());
                assert_eq!(solid.rest.border, palette.tones[5].to_hsla());
                assert_eq!(
                    solid.rest.foreground,
                    theme.tokens.color_text_light_solid.to_hsla()
                );
            }
            for (color, background, border, foreground) in [
                (
                    TagColor::Success,
                    theme.tokens.color_success_bg,
                    theme.tokens.color_success_border,
                    theme.tokens.color_success,
                ),
                (
                    TagColor::Processing,
                    theme.tokens.color_info_bg,
                    theme.tokens.color_info_border,
                    theme.tokens.color_info,
                ),
                (
                    TagColor::Error,
                    theme.tokens.color_error_bg,
                    theme.tokens.color_error_border,
                    theme.tokens.color_error,
                ),
                (
                    TagColor::Warning,
                    theme.tokens.color_warning_bg,
                    theme.tokens.color_warning_border,
                    theme.tokens.color_warning,
                ),
            ] {
                let outlined = TagStyle::resolve(&theme, color, TagVariant::Outlined, None, false);
                assert_eq!(outlined.rest.background, background.to_hsla());
                assert_eq!(outlined.rest.border, border.to_hsla());
                assert_eq!(outlined.rest.foreground, foreground.to_hsla());
                let solid = TagStyle::resolve(&theme, color, TagVariant::Solid, None, false);
                assert_eq!(solid.rest.background, foreground.to_hsla());
                assert_eq!(solid.rest.border, foreground.to_hsla());
            }
        }
    }

    #[test]
    fn tag_custom_colors_keep_hue_and_use_pale_hsl_fill() {
        let theme = ResolvedTheme::default();
        for (custom, pale) in [
            (0xff5500, 0xffeee5),
            (0x2db7f5, 0xe7f6fe),
            (0x000000, 0xf2f2f2),
        ] {
            let color = Color::rgb(custom);
            let filled = TagStyle::resolve(
                &theme,
                TagColor::Custom(color),
                TagVariant::Filled,
                None,
                false,
            );
            assert_eq!(filled.rest.background, Color::rgb(pale).to_hsla());
            assert_eq!(filled.rest.foreground, color.to_hsla());
            assert_eq!(filled.rest.border.a, 0.);
            let outlined = TagStyle::resolve(
                &theme,
                TagColor::Custom(color),
                TagVariant::Outlined,
                None,
                false,
            );
            assert_eq!(outlined.rest.border, color.to_hsla());
            let solid = TagStyle::resolve(
                &theme,
                TagColor::Custom(color),
                TagVariant::Solid,
                None,
                false,
            );
            assert_eq!(solid.rest.background, color.to_hsla());
            assert_eq!(solid.rest.foreground, Color::WHITE.to_hsla());
        }
    }

    #[test]
    fn tag_checkable_states_and_disabled_precedence_match_reference() {
        let theme = ResolvedTheme::default();
        let g = &theme.tokens;
        let unchecked = TagStyle::resolve(
            &theme,
            TagColor::Error,
            TagVariant::Solid,
            Some(false),
            false,
        );
        assert_eq!(unchecked.rest.background.a, 0.);
        assert_eq!(unchecked.rest.border.a, 0.);
        assert_eq!(unchecked.hover.foreground, g.color_primary.to_hsla());
        assert_eq!(unchecked.hover.background, g.color_fill_secondary.to_hsla());
        assert_eq!(
            unchecked.active.background,
            g.color_primary_active.to_hsla()
        );
        let checked = TagStyle::resolve(
            &theme,
            TagColor::Default,
            TagVariant::Filled,
            Some(true),
            false,
        );
        assert_eq!(checked.rest.background, g.color_primary.to_hsla());
        assert_eq!(checked.hover.background, g.color_primary_hover.to_hsla());
        assert_eq!(checked.rest.foreground, g.color_text_light_solid.to_hsla());
        for color in [
            TagColor::Default,
            TagColor::Error,
            TagColor::Preset(PresetColor::Blue),
            TagColor::Custom(Color::rgb(0xff5500)),
        ] {
            for variant in [TagVariant::Filled, TagVariant::Solid, TagVariant::Outlined] {
                let disabled = TagStyle::resolve(&theme, color, variant, None, true);
                assert_eq!(
                    disabled.rest.background,
                    g.color_bg_container_disabled.to_hsla()
                );
                assert_eq!(disabled.rest.foreground, g.color_text_disabled.to_hsla());
                assert_eq!(disabled.rest.close, disabled.rest.foreground);
                assert_eq!(disabled.rest, disabled.hover);
                assert_eq!(disabled.rest, disabled.active);
                assert_eq!(disabled.focus_width, 0.);
            }
        }
        let unchecked_disabled = TagStyle::resolve(
            &theme,
            TagColor::Default,
            TagVariant::Filled,
            Some(false),
            true,
        );
        assert_eq!(unchecked_disabled.rest.background.a, 0.);
        assert_eq!(unchecked_disabled.rest, unchecked_disabled.hover);
        let checked_disabled = TagStyle::resolve(
            &theme,
            TagColor::Default,
            TagVariant::Filled,
            Some(true),
            true,
        );
        assert_eq!(
            checked_disabled.rest.background,
            g.color_bg_container_disabled.to_hsla()
        );
    }

    #[test]
    fn tag_component_tokens_are_cached_and_inherit_local_aliases() {
        let mut config = ThemeConfig::default();
        config.components.set_theme(
            ComponentThemeConfig::<TagTheme>::new(TagTokenPatch {
                default_bg: Some(Color::rgb(0x123456)),
                default_color: Some(Color::rgb(0x654321)),
                ..Default::default()
            })
            .with_tokens(TokenPatch {
                font_size_sm: Some(16.),
                line_height_sm: Some(1.5),
                line_width: Some(2.),
                ..Default::default()
            }),
        );
        let theme = config.resolve();
        let tokens = theme.component::<TagTheme>();
        assert!(Arc::ptr_eq(&tokens, &theme.component::<TagTheme>()));
        assert_eq!(tokens.font(), &tokens.globals().font_family.to_gpui_font());
        let style =
            TagStyle::from_tokens(&tokens, TagColor::Default, TagVariant::Filled, None, false);
        assert_eq!(style.rest.background, Color::rgb(0x123456).to_hsla());
        assert_eq!(style.rest.foreground, Color::rgb(0x654321).to_hsla());
        assert_eq!(style.font_size, 16.);
        assert_eq!(style.line_height, 24.);
        assert_eq!(style.padding_inline, 6.);
        assert_eq!(style.close_gap, 2.);
        let dark = ThemeConfig {
            algorithms: Some(AlgorithmChain::dark()),
            ..Default::default()
        }
        .resolve();
        let style = TagStyle::resolve(&dark, TagColor::Default, TagVariant::Solid, None, false);
        assert_eq!(style.rest.foreground, Color::BLACK.to_hsla());
    }
}
