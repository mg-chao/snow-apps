// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Visual defaults adapted from Ant Design 6.6.7 (MIT).

use super::{ColorPickerFormat, ColorPickerMode, ColorPickerValue};
use crate::{
    Size,
    theme::{AliasTokens, ComponentTheme},
};
use gpui::{Refineable, StyleRefinement};
use std::sync::Arc;

pub struct ColorPickerTheme;

macro_rules! tokens {
    ($($field:ident = $value:expr;)+) => {
        #[derive(Clone, Debug, PartialEq)]
        pub struct ColorPickerTokens {
            $(pub $field: f64,)+
            globals: Arc<AliasTokens>,
        }
        #[derive(Clone, Debug, Default, PartialEq)]
        pub struct ColorPickerTokenPatch { $(pub $field: Option<f64>,)+ }
        impl ComponentTheme for ColorPickerTheme {
            type Tokens = ColorPickerTokens;
            type Patch = ColorPickerTokenPatch;
            fn default_tokens(g: &AliasTokens) -> ColorPickerTokens {
                ColorPickerTokens { $($field: ($value)(g),)+ globals: Arc::new(g.clone()) }
            }
            fn merge(parent: &Self::Patch, local: &Self::Patch) -> Self::Patch {
                ColorPickerTokenPatch { $($field: local.$field.or(parent.$field),)+ }
            }
            fn apply(tokens: &mut ColorPickerTokens, patch: &Self::Patch) {
                let defaults = Self::default_tokens(&tokens.globals);
                $(if let Some(value) = patch.$field {
                    tokens.$field = if value.is_finite() && (value as f32).is_finite() {
                        value.max(0.)
                    } else { defaults.$field };
                })+
            }
        }
    };
}
tokens! {
    width = |_: &AliasTokens| 234.;
    handler_size = |_: &AliasTokens| 16.;
    handler_size_sm = |_: &AliasTokens| 12.;
    slider_height = |_: &AliasTokens| 8.;
    preview_size = |g: &AliasTokens| 16. + g.margin_sm;
    alpha_input_width = |_: &AliasTokens| 44.;
    input_number_handle_width = |_: &AliasTokens| 16.;
    preset_color_size = |_: &AliasTokens| 24.;
}

impl ColorPickerTokens {
    pub fn globals(&self) -> &AliasTokens {
        &self.globals
    }
    pub fn palette_height(&self) -> f64 {
        self.globals.control_height_lg * 4.
    }
    /// The small editors retain their text line box even when compact density
    /// makes the nominal control height shorter (21px versus a 22px line box).
    pub fn editor_height(&self, format: ColorPickerFormat) -> f64 {
        if format == ColorPickerFormat::Hex {
            self.globals
                .control_height_sm
                .max(self.globals.font_height + 2. * self.globals.line_width)
        } else {
            self.globals.control_height_sm
        }
    }
    pub fn trigger_metrics(&self, size: Size) -> ColorPickerTriggerMetrics {
        let g = self.globals();
        let (height, swatch_size, radius, swatch_radius, font_size) = match size {
            Size::Small => (
                g.control_height_sm,
                g.control_height_xs,
                g.border_radius_sm,
                g.border_radius_xs,
                g.font_size,
            ),
            Size::Medium => (
                g.control_height,
                g.control_height_sm,
                g.border_radius,
                g.border_radius_sm,
                g.font_size,
            ),
            Size::Large => (
                g.control_height_lg,
                g.control_height,
                g.border_radius_lg,
                g.border_radius,
                g.font_size_lg,
            ),
        };
        ColorPickerTriggerMetrics {
            height,
            swatch_size,
            radius,
            swatch_radius,
            font_size,
            padding: (g.padding_xxs - g.line_width).max(0.),
            text_gap: g.margin_xs,
            text_end_margin: (g.margin_xs - g.padding_xxs + g.line_width).max(0.),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ColorPickerTriggerMetrics {
    pub height: f64,
    pub swatch_size: f64,
    pub radius: f64,
    pub swatch_radius: f64,
    pub font_size: f64,
    pub padding: f64,
    pub text_gap: f64,
    pub text_end_margin: f64,
}

/// Native refinements for the reference's semantic slots and interactive controls.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ColorPickerStyles {
    pub root: StyleRefinement,
    pub body: StyleRefinement,
    pub content: StyleRefinement,
    pub description: StyleRefinement,
    pub popup: StyleRefinement,
    pub palette: StyleRefinement,
    pub slider: StyleRefinement,
    pub input: StyleRefinement,
    pub presets: StyleRefinement,
}
impl ColorPickerStyles {
    pub(crate) fn refine(&mut self, other: &Self) {
        self.root.refine(&other.root);
        self.body.refine(&other.body);
        self.content.refine(&other.content);
        self.description.refine(&other.description);
        self.popup.refine(&other.popup);
        self.palette.refine(&other.palette);
        self.slider.refine(&other.slider);
        self.input.refine(&other.input);
        self.presets.refine(&other.presets);
    }
}

#[derive(Clone, Debug)]
pub struct ColorPickerStyleContext {
    pub value: ColorPickerValue,
    pub format: ColorPickerFormat,
    pub mode: ColorPickerMode,
    pub size: Size,
    pub disabled: bool,
    pub open: bool,
    pub dragging: bool,
    pub hovered: bool,
    pub focused: bool,
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::theme::{AlgorithmChain, ThemeConfig};

    #[test]
    fn color_picker_reference_metrics_follow_global_density() {
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
            let t = theme.component::<ColorPickerTheme>();
            let g = t.globals();
            assert_eq!(
                (t.width, t.handler_size, t.handler_size_sm, t.slider_height),
                (234., 16., 12., 8.)
            );
            assert_eq!(t.preview_size, 16. + g.margin_sm);
            assert_eq!(t.palette_height(), 4. * g.control_height_lg);
            assert_eq!(
                t.trigger_metrics(Size::Small).swatch_size,
                g.control_height_xs
            );
            assert_eq!(t.trigger_metrics(Size::Medium).height, g.control_height);
            assert_eq!(t.trigger_metrics(Size::Large).radius, g.border_radius_lg);
        }
    }

    #[test]
    fn color_picker_token_patches_merge_and_sanitize_lengths() {
        let theme = ThemeConfig::default().resolve();
        let mut t = ColorPickerTheme::default_tokens(&theme.tokens);
        let patch = ColorPickerTheme::merge(
            &ColorPickerTokenPatch {
                width: Some(300.),
                handler_size: Some(20.),
                ..Default::default()
            },
            &ColorPickerTokenPatch {
                handler_size: Some(18.),
                slider_height: Some(f64::NAN),
                alpha_input_width: Some(-1.),
                ..Default::default()
            },
        );
        ColorPickerTheme::apply(&mut t, &patch);
        assert_eq!(
            (
                t.width,
                t.handler_size,
                t.slider_height,
                t.alpha_input_width
            ),
            (300., 18., 8., 0.)
        );
    }
}
