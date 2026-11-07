// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Visual defaults adapted from Ant Design 6.6.7 (MIT).

use super::{PickerMode, PickerPanelMode};
use crate::{
    Size, ValidationStatus, Variant,
    input::{InputInteraction, InputStyle, InputTheme, InputTokens},
    theme::{AliasTokens, Color, ComponentTheme, Shadow, ShadowList},
};
use gpui::{Refineable, StyleRefinement};
use std::sync::Arc;

pub struct DatePickerTheme;

fn input_padding(height: f64, font: f64, line_height: f64, border: f64, large: bool) -> f64 {
    let tenths = (height - font * line_height) * 5.;
    ((if large { tenths.ceil() } else { tenths.round() }) / 10. - border).max(0.)
}

fn focus_shadow(g: &AliasTokens, color: Color) -> ShadowList {
    Arc::from([Shadow {
        spread: g.control_outline_width,
        color,
        ..Default::default()
    }])
}

macro_rules! picker_tokens {
    ($($field:ident: $ty:ty = $value:expr;)+) => {
        #[derive(Clone, Debug, PartialEq)]
        pub struct DatePickerTokens {
            $(pub $field: $ty,)+
            globals: Arc<AliasTokens>,
            input: InputTokens,
        }
        #[derive(Clone, Debug, Default, PartialEq)]
        pub struct DatePickerTokenPatch { $(pub $field: Option<$ty>,)+ }
        impl ComponentTheme for DatePickerTheme {
            type Tokens = DatePickerTokens;
            type Patch = DatePickerTokenPatch;
            fn default_tokens(g: &AliasTokens) -> Self::Tokens {
                let input = InputTheme::default_tokens(g);
                DatePickerTokens { $($field: ($value)(g),)+ globals: input.globals.clone(), input }
            }
            fn merge(parent: &Self::Patch, local: &Self::Patch) -> Self::Patch {
                DatePickerTokenPatch { $($field: local.$field.clone().or_else(|| parent.$field.clone()),)+ }
            }
            fn apply(tokens: &mut Self::Tokens, patch: &Self::Patch) {
                $(if let Some(value) = &patch.$field { tokens.$field = value.clone(); })+
                let defaults = Self::default_tokens(&tokens.globals);
                tokens.sanitize_lengths(&defaults);
                tokens.derive_font_padding(patch);
                tokens.sync_input_tokens();
            }
        }
    };
}

picker_tokens! {
    padding_inline: f64 = |g: &AliasTokens| g.padding_sm - g.line_width;
    padding_inline_sm: f64 = |g: &AliasTokens| g.control_padding_horizontal_sm - g.line_width;
    padding_inline_lg: f64 = |g: &AliasTokens| g.control_padding_horizontal - g.line_width;
    padding_block: f64 = |g: &AliasTokens| input_padding(g.control_height, g.font_size, g.line_height, g.line_width, false);
    padding_block_sm: f64 = |g: &AliasTokens| input_padding(g.control_height_sm, g.font_size, g.line_height, g.line_width, false);
    padding_block_lg: f64 = |g: &AliasTokens| input_padding(g.control_height_lg, g.font_size_lg, g.line_height_lg, g.line_width, true);
    input_font_size: f64 = |g: &AliasTokens| g.font_size;
    input_font_size_sm: f64 = |g: &AliasTokens| g.font_size;
    input_font_size_lg: f64 = |g: &AliasTokens| g.font_size_lg;
    active_border_color: Color = |g: &AliasTokens| g.color_primary;
    hover_border_color: Color = |g: &AliasTokens| g.color_primary_hover;
    active_bg: Color = |g: &AliasTokens| g.color_bg_container;
    hover_bg: Color = |g: &AliasTokens| g.color_bg_container;
    active_shadow: ShadowList = |g: &AliasTokens| focus_shadow(g, g.control_outline);
    error_active_shadow: ShadowList = |g: &AliasTokens| focus_shadow(g, g.color_error_outline);
    warning_active_shadow: ShadowList = |g: &AliasTokens| focus_shadow(g, g.color_warning_outline);
    cell_hover_bg: Color = |g: &AliasTokens| g.control_item_bg_hover;
    cell_active_with_range_bg: Color = |g: &AliasTokens| g.control_item_bg_active;
    cell_hover_with_range_bg: Color = |g: &AliasTokens| g.color_primary.adjust_lightness(0.35);
    cell_range_border_color: Color = |g: &AliasTokens| g.color_primary.adjust_lightness(0.2);
    cell_bg_disabled: Color = |g: &AliasTokens| g.color_bg_container_disabled;
    time_column_width: f64 = |g: &AliasTokens| g.control_height_lg * 1.4;
    time_column_height: f64 = |_: &AliasTokens| 224.;
    time_cell_height: f64 = |_: &AliasTokens| 28.;
    cell_width: f64 = |g: &AliasTokens| g.control_height_sm * 1.5;
    cell_height: f64 = |g: &AliasTokens| g.control_height_sm;
    text_height: f64 = |g: &AliasTokens| g.control_height_lg;
    without_time_cell_height: f64 = |g: &AliasTokens| g.control_height_lg * 1.65;
    presets_width: f64 = |_: &AliasTokens| 120.;
    presets_max_width: f64 = |_: &AliasTokens| 200.;
    z_index_popup: f64 = |g: &AliasTokens| g.z_index_popup_base + 50.;
    multiple_item_height: f64 = |g: &AliasTokens| (g.control_height - 2. * g.padding_xxs).min(g.control_height - 2. * g.line_width).max(0.);
    multiple_item_height_sm: f64 = |g: &AliasTokens| (g.control_height_sm - 2. * g.padding_xxs).min(g.control_height_sm - 2. * g.line_width).max(0.);
    multiple_item_height_lg: f64 = |g: &AliasTokens| (g.control_height_lg - 2. * g.padding_xxs).min(g.control_height_lg - 2. * g.line_width).max(0.);
    multiple_item_bg: Color = |g: &AliasTokens| g.color_fill_secondary;
    multiple_item_border_color: Color = |_: &AliasTokens| Color::TRANSPARENT;
    multiple_selector_bg_disabled: Color = |g: &AliasTokens| g.color_bg_container_disabled;
    multiple_item_color_disabled: Color = |g: &AliasTokens| g.color_text_disabled;
    multiple_item_border_color_disabled: Color = |_: &AliasTokens| Color::TRANSPARENT;
}

impl DatePickerTokens {
    pub fn globals(&self) -> &AliasTokens {
        &self.globals
    }

    pub fn panel_width(&self) -> f64 {
        self.cell_width * 7. + self.date_panel_padding() * 2.
    }

    pub fn date_panel_padding(&self) -> f64 {
        self.globals.padding + self.globals.padding_xxs / 2.
    }

    pub fn cell_padding(&self) -> f64 {
        self.globals.padding_xxs * 1.5
    }

    pub fn header_height(&self) -> f64 {
        self.text_height + self.globals.line_width
    }

    pub fn footer_height(&self) -> f64 {
        (self.text_height - self.globals.line_width).max(0.)
    }

    pub fn quarter_content_height(&self) -> f64 {
        self.globals.control_height_lg * 1.4
    }

    pub fn year_month_cell_width(&self) -> f64 {
        self.globals.control_height_lg * 1.5
    }

    pub fn trigger_metrics(&self, size: Size) -> PickerTriggerMetrics {
        let g = self.globals();
        let (height, padding_inline, padding_block, font_size, line_height, radius, tag_height) =
            match size {
                Size::Small => (
                    g.control_height_sm,
                    self.padding_inline_sm,
                    self.padding_block_sm,
                    self.input_font_size_sm,
                    g.line_height,
                    g.border_radius_sm,
                    self.multiple_item_height_sm,
                ),
                Size::Medium => (
                    g.control_height,
                    self.padding_inline,
                    self.padding_block,
                    self.input_font_size,
                    g.line_height,
                    g.border_radius,
                    self.multiple_item_height,
                ),
                Size::Large => (
                    g.control_height_lg,
                    self.padding_inline_lg,
                    self.padding_block_lg,
                    self.input_font_size_lg,
                    g.line_height_lg,
                    g.border_radius_lg,
                    self.multiple_item_height_lg,
                ),
            };
        PickerTriggerMetrics {
            height,
            padding_inline,
            padding_block,
            font_size,
            line_height: font_size * line_height,
            radius,
            suffix_gap: g.padding_xs / 2.,
            tag_height,
        }
    }

    pub fn input_style(
        &self,
        size: Size,
        variant: Variant,
        status: ValidationStatus,
        interaction: InputInteraction,
    ) -> InputStyle {
        InputStyle::from_tokens(&self.input, size, variant, status, interaction)
    }

    fn sync_input_tokens(&mut self) {
        macro_rules! copy { ($($field:ident),+ $(,)?) => { $(self.input.$field = self.$field.clone();)+ }; }
        copy!(
            padding_inline,
            padding_inline_sm,
            padding_inline_lg,
            padding_block,
            padding_block_sm,
            padding_block_lg,
            input_font_size,
            input_font_size_sm,
            input_font_size_lg,
            active_border_color,
            hover_border_color,
            active_bg,
            hover_bg,
            active_shadow,
            error_active_shadow,
            warning_active_shadow
        );
    }

    fn sanitize_lengths(&mut self, defaults: &Self) {
        macro_rules! sanitize { ($($field:ident),+ $(,)?) => { $(
            if !self.$field.is_finite() || !(self.$field as f32).is_finite() {
                self.$field = defaults.$field;
            } else { self.$field = self.$field.max(0.); }
        )+ }; }
        sanitize!(
            padding_inline,
            padding_inline_sm,
            padding_inline_lg,
            padding_block,
            padding_block_sm,
            padding_block_lg,
            input_font_size,
            input_font_size_sm,
            input_font_size_lg,
            time_column_width,
            time_column_height,
            time_cell_height,
            cell_width,
            cell_height,
            text_height,
            without_time_cell_height,
            presets_width,
            presets_max_width,
            z_index_popup,
            multiple_item_height,
            multiple_item_height_sm,
            multiple_item_height_lg
        );
    }

    fn derive_font_padding(&mut self, patch: &DatePickerTokenPatch) {
        let g = self.globals();
        let padding = |height: f64, font: f64, line: f64, large: bool| {
            let tenths = (height - font * line) * 5.;
            ((if large { tenths.ceil() } else { tenths.round() }) / 10. - g.line_width).max(0.)
        };
        let medium = if self.input_font_size > 0. {
            self.input_font_size
        } else {
            g.font_size
        };
        let small = if patch.input_font_size.is_some() && patch.input_font_size_sm.is_none() {
            medium
        } else {
            self.input_font_size_sm
        };
        let small = if small > 0. { small } else { medium };
        let large = if self.input_font_size_lg > 0. {
            self.input_font_size_lg
        } else {
            g.font_size_lg
        };
        let block = padding(g.control_height, medium, g.line_height, false);
        let block_sm = padding(g.control_height_sm, small, g.line_height, false);
        let block_lg = padding(g.control_height_lg, large, g.line_height_lg, true);
        if patch.input_font_size.is_some() && patch.input_font_size_sm.is_none() {
            self.input_font_size_sm = medium;
        }
        if patch.input_font_size.is_some() && patch.padding_block.is_none() {
            self.padding_block = block;
        }
        if (patch.input_font_size.is_some() || patch.input_font_size_sm.is_some())
            && patch.padding_block_sm.is_none()
        {
            self.padding_block_sm = block_sm;
        }
        if patch.input_font_size_lg.is_some() && patch.padding_block_lg.is_none() {
            self.padding_block_lg = block_lg;
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct PickerTriggerMetrics {
    pub height: f64,
    pub padding_inline: f64,
    pub padding_block: f64,
    pub font_size: f64,
    pub line_height: f64,
    pub radius: f64,
    pub suffix_gap: f64,
    pub tag_height: f64,
}

/// Native semantic slots. Styles refine provider values before local values.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct PickerStyles {
    pub root: StyleRefinement,
    pub prefix: StyleRefinement,
    pub input: StyleRefinement,
    pub suffix: StyleRefinement,
    pub popup: StyleRefinement,
    pub container: StyleRefinement,
    pub content: StyleRefinement,
    pub body: StyleRefinement,
    pub header: StyleRefinement,
    pub item: StyleRefinement,
    pub footer: StyleRefinement,
    pub presets: StyleRefinement,
    pub time_column: StyleRefinement,
}

impl PickerStyles {
    pub(crate) fn refine(&mut self, other: &Self) {
        macro_rules! slots { ($($slot:ident),+) => { $(self.$slot.refine(&other.$slot);)+ }; }
        slots!(
            root,
            prefix,
            input,
            suffix,
            popup,
            container,
            content,
            body,
            header,
            item,
            footer,
            presets,
            time_column
        );
    }
}

#[derive(Clone, Debug)]
pub struct PickerStyleContext {
    pub picker: PickerMode,
    pub panel_mode: PickerPanelMode,
    pub size: Size,
    pub variant: Variant,
    pub status: ValidationStatus,
    pub disabled: bool,
    pub open: bool,
    pub hovered: bool,
    pub focused: bool,
    pub range: bool,
    pub multiple: bool,
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::theme::{AlgorithmChain, ThemeConfig};

    #[test]
    fn picker_reference_geometry_and_density_follow_tokens() {
        let t = ThemeConfig::default()
            .resolve()
            .component::<DatePickerTheme>();
        assert_eq!(
            (t.panel_width(), t.header_height(), t.footer_height()),
            (288., 41., 39.)
        );
        assert_eq!(
            (t.cell_height, t.cell_width, t.cell_padding()),
            (24., 36., 6.)
        );
        assert_eq!(
            (
                t.time_column_width,
                t.time_column_height,
                t.time_cell_height
            ),
            (56., 224., 28.)
        );
        assert_eq!(t.without_time_cell_height * 4., 264.);
        assert_eq!(t.year_month_cell_width(), 60.);
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
            let t = theme.component::<DatePickerTheme>();
            assert_eq!(t.time_column_width, t.globals().control_height_lg * 1.4);
            assert_eq!(
                t.cell_active_with_range_bg,
                t.globals().control_item_bg_active
            );
            assert_eq!(
                t.trigger_metrics(Size::Small).height,
                t.globals().control_height_sm
            );
        }
    }

    #[test]
    fn picker_compact_year_month_cell_width_matches_reference() {
        let compact = ThemeConfig {
            algorithms: Some(AlgorithmChain::compact()),
            ..Default::default()
        }
        .resolve();
        assert_eq!(
            compact
                .component::<DatePickerTheme>()
                .year_month_cell_width(),
            52.5
        );
    }

    #[test]
    fn picker_token_overrides_merge_and_sanitize() {
        let theme = ThemeConfig::default().resolve();
        let mut t = DatePickerTheme::default_tokens(&theme.tokens);
        let patch = DatePickerTheme::merge(
            &DatePickerTokenPatch {
                time_column_width: Some(72.),
                cell_height: Some(30.),
                ..Default::default()
            },
            &DatePickerTokenPatch {
                cell_height: Some(28.),
                time_column_height: Some(f64::INFINITY),
                cell_width: Some(-1.),
                ..Default::default()
            },
        );
        DatePickerTheme::apply(&mut t, &patch);
        assert_eq!(
            (
                t.time_column_width,
                t.cell_height,
                t.time_column_height,
                t.cell_width
            ),
            (72., 28., 224., 0.)
        );
    }
}
