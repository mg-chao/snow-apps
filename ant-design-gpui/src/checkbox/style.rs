// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Visual state rules adapted from Ant Design 6.6.7 (MIT).

//! Cached native Checkbox tokens and pure visual state resolution.

use std::sync::Arc;

use gpui::Hsla;

use crate::theme::{AliasTokens, ComponentTheme, ResolvedTheme};

/// Marker for component-local global overrides and algorithms.
pub struct CheckboxTheme;

/// Ant Design Checkbox has no independent component tokens. Customize its
/// effective aliases through `ComponentThemeConfig::<CheckboxTheme>::with_tokens`.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct CheckboxTokenPatch;

#[derive(Clone, Debug, PartialEq)]
pub struct CheckboxTokens {
    globals: Arc<AliasTokens>,
    font: gpui::Font,
}

impl ComponentTheme for CheckboxTheme {
    type Tokens = CheckboxTokens;
    type Patch = CheckboxTokenPatch;

    fn default_tokens(global: &AliasTokens) -> Self::Tokens {
        CheckboxTokens {
            globals: Arc::new(global.clone()),
            font: global.font_family.to_gpui_font(),
        }
    }

    fn merge(_: &Self::Patch, _: &Self::Patch) -> Self::Patch {
        CheckboxTokenPatch
    }

    fn apply(_: &mut Self::Tokens, _: &Self::Patch) {}
}

impl CheckboxTokens {
    pub(crate) fn globals(&self) -> &AliasTokens {
        &self.globals
    }

    pub(crate) fn font(&self) -> &gpui::Font {
        &self.font
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct CheckboxStateStyle {
    pub background: Hsla,
    pub border: Hsla,
    pub mark: Hsla,
    pub label: Hsla,
}

/// Dimensions are logical pixels. Size follows `controlInteractiveSize` rather
/// than the provider's Button/input size, as in the reference Checkbox.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct CheckboxStyle {
    pub rest: CheckboxStateStyle,
    pub hover: CheckboxStateStyle,
    pub size: f64,
    pub radius: f64,
    pub line_width: f64,
    pub mark_width: f64,
    pub indeterminate_size: f64,
    pub font_size: f64,
    pub line_height: f64,
    pub label_padding: f64,
    pub group_gap: f64,
    pub focus_color: Hsla,
    pub focus_width: f64,
}

impl CheckboxStyle {
    pub fn resolve(
        theme: &ResolvedTheme,
        checked: bool,
        indeterminate: bool,
        disabled: bool,
    ) -> Self {
        Self::from_tokens(
            &theme.component::<CheckboxTheme>(),
            checked,
            indeterminate,
            disabled,
        )
    }

    pub fn from_tokens(
        tokens: &CheckboxTokens,
        checked: bool,
        indeterminate: bool,
        disabled: bool,
    ) -> Self {
        let g = tokens.globals();
        let mut rest = CheckboxStateStyle {
            background: g.color_bg_container.into(),
            border: g.color_border.into(),
            mark: if indeterminate {
                g.color_primary.into()
            } else {
                g.color_white.into()
            },
            label: g.color_text.into(),
        };
        if checked && !indeterminate {
            rest.background = g.color_primary.into();
            rest.border = g.color_primary.into();
        }
        let mut hover = rest;
        hover.border = g.color_primary.into();
        // The wrapper's checked-hover selector is more specific than the
        // indeterminate container rule, including when both flags are set.
        if checked {
            hover.background = g.color_primary_hover.into();
            hover.border = crate::theme::Color::TRANSPARENT.into();
        }
        if disabled {
            rest.background = g.color_bg_container_disabled.into();
            rest.border = g.color_border.into();
            rest.mark = g.color_text_disabled.into();
            rest.label = g.color_text_disabled.into();
            hover = rest;
        }
        Self {
            rest,
            hover,
            size: g.control_interactive_size,
            radius: g.border_radius_sm,
            line_width: g.line_width,
            mark_width: g.line_width_bold,
            indeterminate_size: g.font_size_lg / 2.,
            font_size: g.font_size,
            line_height: g.font_size * g.line_height,
            label_padding: g.padding_xs,
            group_gap: g.margin_xs,
            focus_color: g.color_primary_border.into(),
            focus_width: if disabled || !g.focus_outline {
                0.
            } else {
                g.line_width_focus
            },
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::theme::{AlgorithmChain, Color, ComponentThemeConfig, ThemeConfig, TokenPatch};

    #[test]
    fn checkbox_style_matches_reference_dimensions_and_states() {
        let theme = ResolvedTheme::default();
        let empty = CheckboxStyle::resolve(&theme, false, false, false);
        assert_eq!(empty.size, 16.);
        assert_eq!(empty.radius, 4.);
        assert_eq!(empty.line_width, 1.);
        assert_eq!(empty.mark_width, 2.);
        assert_eq!(empty.indeterminate_size, 8.);
        assert_eq!(empty.font_size, 14.);
        assert_eq!(empty.line_height, 22.);
        assert_eq!(empty.label_padding, 8.);
        assert_eq!(empty.group_gap, 8.);
        assert_eq!(empty.rest.background, Color::WHITE.to_hsla());
        assert_eq!(empty.rest.border, Color::rgb(0xd9d9d9).to_hsla());
        assert_eq!(empty.hover.border, Color::rgb(0x1677ff).to_hsla());
        let checked = CheckboxStyle::resolve(&theme, true, false, false);
        assert_eq!(checked.rest.background, empty.hover.border);
        assert_eq!(checked.rest.border, empty.hover.border);
        assert_eq!(checked.hover.background, Color::rgb(0x4096ff).to_hsla());
        assert_eq!(checked.hover.border.a, 0.);
        let mixed = CheckboxStyle::resolve(&theme, false, true, false);
        assert_eq!(mixed.rest.background, empty.rest.background);
        assert_eq!(mixed.rest.border, empty.rest.border);
        assert_eq!(mixed.rest.mark, empty.hover.border);
        assert_eq!(mixed.hover.background, mixed.rest.background);
        for (checked, mixed) in [(false, false), (true, false), (false, true), (true, true)] {
            let disabled = CheckboxStyle::resolve(&theme, checked, mixed, true);
            assert_eq!(disabled.rest, disabled.hover);
            assert_eq!(
                disabled.rest.background,
                Color::BLACK.with_alpha(0.04).to_hsla()
            );
            assert_eq!(disabled.rest.border, empty.rest.border);
            assert_eq!(disabled.rest.mark, Color::BLACK.with_alpha(0.25).to_hsla());
            assert_eq!(disabled.rest.label, disabled.rest.mark);
            assert_eq!(disabled.focus_width, 0.);
        }
    }

    #[test]
    fn checkbox_style_uses_complete_component_local_aliases_and_cached_fonts() {
        let mut config = ThemeConfig::default();
        config.components.set_theme(
            ComponentThemeConfig::<CheckboxTheme>::default()
                .with_algorithms(AlgorithmChain::dark_compact())
                .with_tokens(TokenPatch {
                    control_interactive_size: Some(23.),
                    padding_xs: Some(11.),
                    color_primary: Some(Color::rgb(0x722ed1)),
                    // A component algorithm inherits the parent's resolved base;
                    // explicitly select a dark base for this local override.
                    color_bg_base: Some(Color::BLACK),
                    font_size: Some(19.),
                    ..Default::default()
                }),
        );
        let theme = config.resolve();
        let tokens = theme.component::<CheckboxTheme>();
        assert!(Arc::ptr_eq(&tokens, &theme.component::<CheckboxTheme>()));
        assert_eq!(tokens.font(), &tokens.globals().font_family.to_gpui_font());
        let style = CheckboxStyle::from_tokens(&tokens, true, false, false);
        assert_eq!(style.size, 23.);
        assert_eq!(style.label_padding, 11.);
        // Component algorithms consume the seed override before deriving aliases;
        // compact scales the customized 19px seed to its 16px body size.
        assert_eq!(style.font_size, 16.);
        assert_eq!(tokens.globals().color_primary, Color::rgb(0x642ab5));
        assert_eq!(style.rest.background, Color::rgb(0x642ab5).to_hsla());
        assert_eq!(tokens.globals().color_bg_container, Color::rgb(0x141414));
        assert_eq!(theme.tokens.control_interactive_size, 16.);
    }

    #[test]
    fn checkbox_compact_and_dark_geometry_follow_upstream_aliases() {
        for algorithms in [
            AlgorithmChain::dark(),
            AlgorithmChain::compact(),
            AlgorithmChain::dark_compact(),
        ] {
            let theme = ThemeConfig {
                algorithms: Some(algorithms),
                ..Default::default()
            }
            .resolve();
            let style = CheckboxStyle::resolve(&theme, false, true, false);
            assert_eq!(style.size, theme.tokens.control_height / 2.);
            assert_eq!(style.indeterminate_size, theme.tokens.font_size_lg / 2.);
            assert_eq!(style.radius, theme.tokens.border_radius_sm);
            assert_eq!(
                style.rest.background,
                theme.tokens.color_bg_container.to_hsla()
            );
            assert_eq!(style.rest.mark, theme.tokens.color_primary.to_hsla());
        }
    }
}
