// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Visual defaults adapted from Ant Design 6.6.7 (MIT).

use std::sync::Arc;

use gpui::Hsla;

use crate::{
    Size,
    theme::{AliasTokens, Color, ComponentTheme, ResolvedTheme, Shadow, ShadowList},
};

/// Marker for Switch-specific aliases, algorithms and component tokens.
pub struct SwitchTheme;

fn cloned<T: Clone>(value: &T) -> T {
    value.clone()
}

macro_rules! switch_tokens {
    ($($field:ident: $ty:ty),* $(,)?) => {
        /// Logical-pixel geometry and colors, derived once per resolved theme.
        #[derive(Clone, Debug, PartialEq)]
        pub struct SwitchTokens {
            $(pub $field: $ty,)*
            globals: Arc<AliasTokens>,
            font: gpui::Font,
        }

        /// Optional component overrides. Unset fields inherit from the parent scope.
        #[derive(Clone, Debug, Default, PartialEq)]
        pub struct SwitchTokenPatch { $(pub $field: Option<$ty>,)* }

        impl ComponentTheme for SwitchTheme {
            type Tokens = SwitchTokens;
            type Patch = SwitchTokenPatch;

            fn default_tokens(g: &AliasTokens) -> Self::Tokens {
                let height = g.font_size * g.line_height;
                let height_sm = g.control_height / 2.;
                let padding = 2.;
                let handle = height - padding * 2.;
                let handle_sm = height_sm - padding * 2.;
                SwitchTokens {
                    track_height: height,
                    track_height_sm: height_sm,
                    track_min_width: handle * 2. + padding * 4.,
                    track_min_width_sm: handle_sm * 2. + padding * 2.,
                    track_padding: padding,
                    handle_bg: g.color_white,
                    handle_shadow: Arc::from([Shadow {
                        y: 2., blur: 4., color: Color::rgb(0x00230b).with_alpha(0.2),
                        ..Default::default()
                    }]),
                    handle_size: handle,
                    handle_size_sm: handle_sm,
                    inner_min_margin: handle / 2.,
                    inner_max_margin: handle + padding * 3.,
                    inner_min_margin_sm: handle_sm / 2.,
                    inner_max_margin_sm: handle_sm + padding * 3.,
                    globals: Arc::new(g.clone()),
                    font: g.font_family.to_gpui_font(),
                }
            }

            fn merge(parent: &Self::Patch, local: &Self::Patch) -> Self::Patch {
                SwitchTokenPatch { $($field: cloned(&local.$field).or_else(|| cloned(&parent.$field)),)* }
            }

            fn apply(tokens: &mut Self::Tokens, patch: &Self::Patch) {
                $(if let Some(value) = &patch.$field { tokens.$field = cloned(value); })*
                // Invalid lengths cannot enter native layout. Negative padding is
                // intentional: Ant's token demo uses it for an oversized handle.
                let defaults = Self::default_tokens(&tokens.globals);
                macro_rules! length {
                    ($name:ident) => {
                        if !(tokens.$name as f32).is_finite() { tokens.$name = defaults.$name; }
                    };
                }
                length!(track_height); length!(track_height_sm);
                length!(track_min_width); length!(track_min_width_sm);
                length!(track_padding); length!(handle_size); length!(handle_size_sm);
                length!(inner_min_margin); length!(inner_max_margin);
                length!(inner_min_margin_sm); length!(inner_max_margin_sm);
            }
        }
    };
}

switch_tokens! {
    track_height: f64,
    track_height_sm: f64,
    track_min_width: f64,
    track_min_width_sm: f64,
    track_padding: f64,
    handle_bg: Color,
    handle_shadow: ShadowList,
    handle_size: f64,
    handle_size_sm: f64,
    inner_min_margin: f64,
    inner_max_margin: f64,
    inner_min_margin_sm: f64,
    inner_max_margin_sm: f64,
}

impl SwitchTokens {
    pub(crate) fn globals(&self) -> &AliasTokens {
        &self.globals
    }
    pub(crate) fn font(&self) -> &gpui::Font {
        &self.font
    }
}

/// Pure visual resolution, shared by rendering and independent browser comparisons.
#[derive(Clone, Debug, PartialEq)]
pub struct SwitchStyle {
    pub track_height: f64,
    pub track_min_width: f64,
    pub track_padding: f64,
    pub handle_size: f64,
    pub inner_min_margin: f64,
    pub inner_max_margin: f64,
    pub background: Hsla,
    pub hover_background: Hsla,
    pub handle_background: Hsla,
    pub handle_shadow: ShadowList,
    pub content_color: Hsla,
    pub font_size: f64,
    pub loading_icon_size: f64,
    pub loading_icon_color: Hsla,
    pub opacity: f64,
    pub focus_color: Hsla,
    pub focus_width: f64,
    pub active_content_offset: f64,
}

impl SwitchStyle {
    pub fn resolve(
        theme: &ResolvedTheme,
        size: Size,
        checked: bool,
        disabled: bool,
        loading: bool,
    ) -> Self {
        Self::from_tokens(
            &theme.component::<SwitchTheme>(),
            size,
            checked,
            disabled,
            loading,
        )
    }

    pub fn from_tokens(
        tokens: &SwitchTokens,
        size: Size,
        checked: bool,
        disabled: bool,
        loading: bool,
    ) -> Self {
        let g = tokens.globals();
        let small = size == Size::Small;
        let disabled = disabled || loading;
        let background = if checked {
            g.color_primary
        } else {
            g.color_text_quaternary
        };
        Self {
            track_height: if small {
                tokens.track_height_sm
            } else {
                tokens.track_height
            }
            .max(0.),
            track_min_width: if small {
                tokens.track_min_width_sm
            } else {
                tokens.track_min_width
            }
            .max(0.),
            track_padding: tokens.track_padding,
            handle_size: if small {
                tokens.handle_size_sm
            } else {
                tokens.handle_size
            }
            .max(0.),
            inner_min_margin: if small {
                tokens.inner_min_margin_sm
            } else {
                tokens.inner_min_margin
            },
            inner_max_margin: if small {
                tokens.inner_max_margin_sm
            } else {
                tokens.inner_max_margin
            },
            background: background.into(),
            hover_background: if disabled {
                background
            } else if checked {
                g.color_primary_hover
            } else {
                g.color_text_tertiary
            }
            .into(),
            handle_background: tokens.handle_bg.into(),
            // The source's disabled descendant rule does not select ::before,
            // which owns the handle shadow. Preserve the browser's cascade.
            handle_shadow: tokens.handle_shadow.clone(),
            content_color: g.color_text_light_solid.into(),
            font_size: g.font_size_sm,
            loading_icon_size: if small {
                g.font_size_icon * 0.75
            } else {
                g.font_size
            },
            loading_icon_color: if checked {
                g.color_primary
            } else {
                Color::BLACK.with_alpha(g.opacity_loading)
            }
            .into(),
            opacity: if disabled { g.opacity_loading } else { 1. },
            focus_color: g.color_primary_border.into(),
            focus_width: if disabled || !g.focus_outline {
                0.
            } else {
                g.line_width_focus
            },
            active_content_offset: if small {
                g.margin_xxs / 2.
            } else {
                tokens.track_padding * 2.
            },
        }
    }
}
