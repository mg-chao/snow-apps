// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Token derivation adapted from Ant Design 6.6.7 (MIT).

use std::{fmt, sync::Arc, time::Duration};

use super::palette::generate_palette;
use super::{AliasTokens, Color, MapTokens, Palette, PresetColor, SeedTokens, Shadow, ShadowList};

/// A pure mapping step. Custom algorithms must depend only on their arguments.
///
/// `previous` contains the preceding step's output. A first step should produce
/// a complete map (usually by starting with `BuiltinAlgorithm::Default`).
pub trait MappingAlgorithm: Send + Sync + 'static {
    fn derive(&self, seed: &SeedTokens, previous: Option<&MapTokens>) -> MapTokens;
}

impl<F> MappingAlgorithm for F
where
    F: Fn(&SeedTokens, Option<&MapTokens>) -> MapTokens + Send + Sync + 'static,
{
    fn derive(&self, seed: &SeedTokens, previous: Option<&MapTokens>) -> MapTokens {
        self(seed, previous)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BuiltinAlgorithm {
    Default,
    Dark,
    Compact,
}
impl MappingAlgorithm for BuiltinAlgorithm {
    fn derive(&self, seed: &SeedTokens, previous: Option<&MapTokens>) -> MapTokens {
        match self {
            Self::Default => derive_default(seed),
            Self::Dark => {
                let mut map = previous.cloned().unwrap_or_else(|| derive_default(seed));
                derive_colors(seed, &mut map, true);
                map.color_primary_bg = map.color_primary_border;
                map.color_primary_bg_hover = map.color_primary_border_hover;
                map
            }
            Self::Compact => {
                let mut map = previous.cloned().unwrap_or_else(|| derive_default(seed));
                let font_size = map.font_size_sm;
                let height = map.control_height - 4.0;
                derive_sizes(&mut map, true);
                derive_fonts(&mut map, font_size);
                derive_heights(&mut map, height);
                map
            }
        }
    }
}

/// Ordered mapping algorithms. Custom steps compare by `Arc` identity.
#[derive(Clone)]
pub struct AlgorithmChain(Arc<[AlgorithmStep]>);
#[derive(Clone)]
enum AlgorithmStep {
    Builtin(BuiltinAlgorithm),
    Custom(Arc<dyn MappingAlgorithm>),
}
impl PartialEq for AlgorithmStep {
    fn eq(&self, other: &Self) -> bool {
        match (self, other) {
            (Self::Builtin(left), Self::Builtin(right)) => left == right,
            (Self::Custom(left), Self::Custom(right)) => Arc::ptr_eq(left, right),
            _ => false,
        }
    }
}
impl PartialEq for AlgorithmChain {
    fn eq(&self, other: &Self) -> bool {
        self.0 == other.0
    }
}
impl fmt::Debug for AlgorithmChain {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_list()
            .entries(self.0.iter().map(|step| match step {
                AlgorithmStep::Builtin(step) => format!("{step:?}"),
                AlgorithmStep::Custom(_) => "Custom".into(),
            }))
            .finish()
    }
}
impl Default for AlgorithmChain {
    fn default() -> Self {
        Self::new([BuiltinAlgorithm::Default])
    }
}
impl AlgorithmChain {
    pub fn new(steps: impl IntoIterator<Item = BuiltinAlgorithm>) -> Self {
        Self(steps.into_iter().map(AlgorithmStep::Builtin).collect())
    }
    pub fn dark() -> Self {
        Self::new([BuiltinAlgorithm::Dark])
    }
    pub fn compact() -> Self {
        Self::new([BuiltinAlgorithm::Compact])
    }
    pub fn dark_compact() -> Self {
        Self::new([BuiltinAlgorithm::Dark, BuiltinAlgorithm::Compact])
    }
    pub fn then(mut self, step: BuiltinAlgorithm) -> Self {
        let mut steps = self.0.to_vec();
        steps.push(AlgorithmStep::Builtin(step));
        self.0 = steps.into();
        self
    }
    pub fn then_custom(mut self, algorithm: Arc<dyn MappingAlgorithm>) -> Self {
        let mut steps = self.0.to_vec();
        steps.push(AlgorithmStep::Custom(algorithm));
        self.0 = steps.into();
        self
    }
    pub fn derive(&self, seed: &SeedTokens) -> MapTokens {
        let mut result = None;
        for step in self.0.iter() {
            result = Some(match step {
                AlgorithmStep::Builtin(algorithm) => algorithm.derive(seed, result.as_ref()),
                AlgorithmStep::Custom(algorithm) => algorithm.derive(seed, result.as_ref()),
            });
        }
        result.unwrap_or_else(|| derive_default(seed))
    }
}

fn derive_default(seed: &SeedTokens) -> MapTokens {
    let mut map = MapTokens::from_seed(seed);
    derive_colors(seed, &mut map, false);
    derive_fonts(&mut map, seed.font_size);
    derive_sizes(&mut map, false);
    derive_heights(&mut map, seed.control_height);
    map.line_width_bold = seed.line_width + 1.0;
    derive_radii(&mut map, seed.border_radius);
    map.motion_duration_fast = duration(seed.motion_base + seed.motion_unit);
    map.motion_duration_mid = duration(seed.motion_base + seed.motion_unit * 2.0);
    map.motion_duration_slow = duration(seed.motion_base + seed.motion_unit * 3.0);
    map
}

fn duration(seconds: f64) -> Duration {
    // JS toFixed(1) rounds the exact binary value, with decimal ties away from
    // zero. Scaling in floating point first loses information (e.g. 0.35).
    let bits = seconds.to_bits();
    let exponent = ((bits >> 52) & 0x7ff) as i32 - 1023 - 52;
    let mantissa = (bits & ((1u64 << 52) - 1)) | (1u64 << 52);
    let numerator = u128::from(mantissa) * 10;
    let tenths = if seconds == 0.0 {
        0
    } else if exponent >= 0 {
        numerator.checked_shl(exponent as u32).unwrap_or(u128::MAX)
    } else {
        let shift = (-exponent) as u32;
        if shift >= 128 {
            0
        } else {
            let half = 1u128 << (shift - 1);
            (numerator + half) >> shift
        }
    };
    Duration::from_secs_f64(tenths as f64 / 10.0)
}

fn derive_colors(seed: &SeedTokens, map: &mut MapTokens, dark: bool) {
    let presets = [
        seed.blue,
        seed.purple,
        seed.cyan,
        seed.green,
        seed.magenta,
        seed.pink,
        seed.red,
        seed.orange,
        seed.yellow,
        seed.volcano,
        seed.geekblue,
        seed.gold,
        seed.lime,
    ];
    for (name, color) in PresetColor::ALL.into_iter().zip(presets) {
        let tones = generate_palette(color, dark);
        *map.palettes.get_mut(name) = Palette {
            hover: tones[4],
            active: tones[6],
            tones,
        };
    }
    let semantic = |color| {
        let tones = generate_palette(color, dark);
        if dark {
            [
                tones[0], tones[1], tones[2], tones[3], tones[6], tones[5], tones[4], tones[6],
                tones[5], tones[4],
            ]
        } else {
            [
                tones[0], tones[1], tones[2], tones[3], tones[4], tones[5], tones[6], tones[4],
                tones[5], tones[6],
            ]
        }
    };
    macro_rules! semantic_color {
        ($source:expr, $bg:ident, $bg_hover:ident, $border:ident, $border_hover:ident, $hover:ident, $base:ident, $active:ident, $text_hover:ident, $text:ident, $text_active:ident, $hover_index:expr) => {{
            let tones = semantic($source);
            map.$bg = tones[0];
            map.$bg_hover = tones[1];
            map.$border = tones[2];
            map.$border_hover = tones[3];
            map.$hover = tones[$hover_index];
            map.$base = tones[5];
            map.$active = tones[6];
            map.$text_hover = tones[7];
            map.$text = tones[8];
            map.$text_active = tones[9];
        }};
    }
    semantic_color!(
        seed.color_primary,
        color_primary_bg,
        color_primary_bg_hover,
        color_primary_border,
        color_primary_border_hover,
        color_primary_hover,
        color_primary,
        color_primary_active,
        color_primary_text_hover,
        color_primary_text,
        color_primary_text_active,
        4
    );
    semantic_color!(
        seed.color_success,
        color_success_bg,
        color_success_bg_hover,
        color_success_border,
        color_success_border_hover,
        color_success_hover,
        color_success,
        color_success_active,
        color_success_text_hover,
        color_success_text,
        color_success_text_active,
        3
    );
    semantic_color!(
        seed.color_warning,
        color_warning_bg,
        color_warning_bg_hover,
        color_warning_border,
        color_warning_border_hover,
        color_warning_hover,
        color_warning,
        color_warning_active,
        color_warning_text_hover,
        color_warning_text,
        color_warning_text_active,
        3
    );
    semantic_color!(
        seed.color_error,
        color_error_bg,
        color_error_bg_hover,
        color_error_border,
        color_error_border_hover,
        color_error_hover,
        color_error,
        color_error_active,
        color_error_text_hover,
        color_error_text,
        color_error_text_active,
        4
    );
    semantic_color!(
        seed.color_info,
        color_info_bg,
        color_info_bg_hover,
        color_info_border,
        color_info_border_hover,
        color_info_hover,
        color_info,
        color_info_active,
        color_info_text_hover,
        color_info_text,
        color_info_text_active,
        3
    );
    let errors = semantic(seed.color_error);
    map.color_error_bg_filled_hover = errors[0].mix(errors[2], 0.5).hex_roundtrip();
    map.color_error_bg_active = errors[2];
    let links = semantic(seed.color_link.unwrap_or(seed.color_info));
    map.color_link_hover = links[3];
    map.color_link = links[5];
    map.color_link_active = links[6];

    let background = seed
        .color_bg_base
        .unwrap_or(if dark { Color::BLACK } else { Color::WHITE });
    let text = seed
        .color_text_base
        .unwrap_or(if dark { Color::WHITE } else { Color::BLACK });
    let solid = |amount: f64| background.adjust_lightness(if dark { amount } else { -amount });
    map.color_bg_base = background;
    map.color_text_base = text;
    map.color_shadow = if dark {
        Color::WHITE.with_alpha(0.2)
    } else {
        Color::BLACK
    };
    map.color_text = text.with_alpha(if dark { 0.85 } else { 0.88 });
    map.color_text_secondary = text.with_alpha(0.65);
    map.color_text_tertiary = text.with_alpha(0.45);
    map.color_text_quaternary = text.with_alpha(0.25);
    map.color_fill = text.with_alpha(if dark { 0.18 } else { 0.15 });
    map.color_fill_secondary = text.with_alpha(if dark { 0.12 } else { 0.06 });
    map.color_fill_tertiary = text.with_alpha(if dark { 0.08 } else { 0.04 });
    map.color_fill_quaternary = text.with_alpha(if dark { 0.04 } else { 0.02 });
    map.color_bg_solid = text.with_alpha(if dark { 0.95 } else { 1.0 });
    map.color_bg_solid_hover = text.with_alpha(if dark { 1.0 } else { 0.75 });
    map.color_bg_solid_active = text.with_alpha(if dark { 0.9 } else { 0.95 });
    map.color_bg_layout = solid(if dark { 0.0 } else { 0.04 });
    map.color_bg_container = solid(if dark { 0.08 } else { 0.0 });
    map.color_bg_elevated = solid(if dark { 0.12 } else { 0.0 });
    map.color_bg_spotlight = if dark {
        solid(0.26)
    } else {
        text.with_alpha(0.85)
    };
    map.color_bg_blur = if dark {
        text.with_alpha(0.04)
    } else {
        Color::TRANSPARENT
    };
    map.color_border = solid(if dark { 0.26 } else { 0.15 });
    map.color_border_disabled = map.color_border;
    map.color_border_secondary = solid(if dark { 0.19 } else { 0.06 });
    map.color_bg_mask = Color::BLACK.with_alpha(0.45);
    map.color_white = Color::WHITE;
}

fn derive_fonts(map: &mut MapTokens, base: f64) {
    let sizes: [f64; 10] = std::array::from_fn(|index| {
        if index == 1 {
            return base;
        }
        let size = base * ((index as f64 - 1.0) / 5.0).exp();
        let integer = if index > 1 { size.floor() } else { size.ceil() };
        (integer / 2.0).floor() * 2.0
    });
    let height = |index: usize| (sizes[index] + 8.0) / sizes[index];
    map.font_size = base;
    map.font_size_sm = sizes[0];
    map.font_size_lg = sizes[2];
    map.font_size_xl = sizes[3];
    map.font_size_heading1 = sizes[6];
    map.font_size_heading2 = sizes[5];
    map.font_size_heading3 = sizes[4];
    map.font_size_heading4 = sizes[3];
    map.font_size_heading5 = sizes[2];
    map.line_height = height(1);
    map.line_height_sm = height(0);
    map.line_height_lg = height(2);
    map.font_height = (height(1) * base).round();
    map.font_height_sm = (height(0) * sizes[0]).round();
    map.font_height_lg = (height(2) * sizes[2]).round();
    map.line_height_heading1 = height(6);
    map.line_height_heading2 = height(5);
    map.line_height_heading3 = height(4);
    map.line_height_heading4 = height(3);
    map.line_height_heading5 = height(2);
}

fn derive_sizes(map: &mut MapTokens, compact: bool) {
    let unit = map.size_unit;
    let step = map.size_step - if compact { 2.0 } else { 0.0 };
    map.size_xxl = unit * (step + if compact { 10.0 } else { 8.0 });
    map.size_xl = unit * (step + if compact { 6.0 } else { 4.0 });
    map.size_lg = unit * (step + 2.0);
    map.size_md = unit * (step + if compact { 2.0 } else { 1.0 });
    map.size_ms = unit * (step + if compact { 1.0 } else { 0.0 });
    map.size = unit * step;
    map.size_sm = unit * (step - if compact { 0.0 } else { 1.0 });
    map.size_xs = unit * (step - if compact { 1.0 } else { 2.0 });
    map.size_xxs = unit * (step - if compact { 1.0 } else { 3.0 });
}

fn derive_heights(map: &mut MapTokens, height: f64) {
    map.control_height = height;
    map.control_height_xs = height * 0.5;
    map.control_height_sm = height * 0.75;
    map.control_height_lg = height * 1.25;
}

fn derive_radii(map: &mut MapTokens, radius: f64) {
    map.border_radius_lg = if radius >= 16.0 {
        16.0
    } else if radius >= 6.0 {
        radius + 2.0
    } else if radius >= 5.0 {
        radius + 1.0
    } else {
        radius
    };
    map.border_radius_sm = if radius >= 16.0 {
        8.0
    } else if radius >= 14.0 {
        7.0
    } else if radius >= 8.0 {
        6.0
    } else if radius >= 7.0 {
        5.0
    } else if radius >= 5.0 {
        4.0
    } else {
        radius
    };
    map.border_radius_xs = if radius >= 6.0 {
        2.0
    } else if radius >= 2.0 {
        1.0
    } else {
        radius
    };
    map.border_radius_outer = if radius >= 8.0 {
        6.0
    } else if radius > 4.0 {
        4.0
    } else {
        radius
    };
}

pub(crate) fn derive_alias(map: &MapTokens) -> AliasTokens {
    let mut alias = map.alias_shell();
    if !map.motion {
        alias.motion_duration_fast = Duration::ZERO;
        alias.motion_duration_mid = Duration::ZERO;
        alias.motion_duration_slow = Duration::ZERO;
    }
    alias.color_fill_content = map.color_fill_secondary;
    alias.color_fill_content_hover = map.color_fill;
    alias.color_fill_alter = map.color_fill_quaternary;
    alias.color_bg_container_disabled = map.color_fill_tertiary;
    alias.color_border_bg = map.color_bg_container;
    alias.color_split = map
        .color_border_secondary
        .alpha_over(map.color_bg_container);
    alias.color_text_placeholder = map.color_text_quaternary;
    alias.color_text_disabled = map.color_text_quaternary;
    alias.color_text_heading = map.color_text;
    alias.color_text_label = map.color_text_secondary;
    alias.color_text_description = map.color_text_tertiary;
    alias.color_text_light_solid = map.color_white;
    alias.color_highlight = map.color_error;
    alias.color_bg_text_hover = map.color_fill_secondary;
    alias.color_bg_text_active = map.color_fill;
    alias.color_icon = map.color_text_tertiary;
    alias.color_icon_hover = map.color_text;
    alias.color_error_outline = map.color_error_bg.alpha_over(map.color_bg_container);
    alias.color_warning_outline = map.color_warning_bg.alpha_over(map.color_bg_container);
    alias.color_error_affix = map.color_error;
    alias.color_warning_affix = map.color_warning;
    alias.font_height = (map.font_size * map.line_height).round();
    alias.font_height_sm = (map.font_size_sm * map.line_height_sm).round();
    alias.font_height_lg = (map.font_size_lg * map.line_height_lg).round();
    alias.font_size_icon = map.font_size_sm;
    alias.line_width_focus = if map.focus_outline {
        map.line_width * 3.0
    } else {
        0.0
    };
    alias.control_outline_width = map.line_width * 2.0;
    alias.control_interactive_size = map.control_height / 2.0;
    alias.control_item_bg_hover = map.color_fill_tertiary;
    alias.control_item_bg_active = map.color_primary_bg;
    alias.control_item_bg_active_hover = map.color_primary_bg_hover;
    alias.control_item_bg_active_disabled = map.color_fill;
    alias.control_tmp_outline = map.color_fill_quaternary;
    alias.control_outline = map.color_primary_bg.alpha_over(map.color_bg_container);
    alias.font_weight_strong = 600.0;
    alias.opacity_loading = 0.65;
    alias.control_padding_horizontal = 12.0;
    alias.control_padding_horizontal_sm = 8.0;
    alias.padding_xxs = map.size_xxs;
    alias.padding_xs = map.size_xs;
    alias.padding_sm = map.size_sm;
    alias.padding = map.size;
    alias.padding_md = map.size_md;
    alias.padding_lg = map.size_lg;
    alias.padding_xl = map.size_xl;
    alias.padding_content_horizontal_lg = map.size_lg;
    alias.padding_content_vertical_lg = map.size_ms;
    alias.padding_content_horizontal = map.size_ms;
    alias.padding_content_vertical = map.size_sm;
    alias.padding_content_horizontal_sm = map.size;
    alias.padding_content_vertical_sm = map.size_xs;
    alias.margin_xxs = map.size_xxs;
    alias.margin_xs = map.size_xs;
    alias.margin_sm = map.size_sm;
    alias.margin = map.size;
    alias.margin_md = map.size_md;
    alias.margin_lg = map.size_lg;
    alias.margin_xl = map.size_xl;
    alias.margin_xxl = map.size_xxl;
    alias.screen_xs = 480.0;
    alias.screen_xs_min = 480.0;
    alias.screen_xs_max = 575.0;
    alias.screen_sm = 576.0;
    alias.screen_sm_min = 576.0;
    alias.screen_sm_max = 767.0;
    alias.screen_md = 768.0;
    alias.screen_md_min = 768.0;
    alias.screen_md_max = 991.0;
    alias.screen_lg = 992.0;
    alias.screen_lg_min = 992.0;
    alias.screen_lg_max = 1199.0;
    alias.screen_xl = 1200.0;
    alias.screen_xl_min = 1200.0;
    alias.screen_xl_max = 1599.0;
    alias.screen_xxl = 1600.0;
    alias.screen_xxl_min = 1600.0;
    alias.screen_xxl_max = 1919.0;
    alias.screen_xxxl = 1920.0;
    alias.screen_xxxl_min = 1920.0;

    let shadow = |x, y, blur, spread, alpha, inset| Shadow {
        x,
        y,
        blur,
        spread,
        color: map.color_shadow.with_alpha(map.color_shadow.a * alpha),
        inset,
    };
    let standard: ShadowList = Arc::from([
        shadow(0.0, 6.0, 16.0, 0.0, 0.08, false),
        shadow(0.0, 3.0, 6.0, -4.0, 0.12, false),
        shadow(0.0, 9.0, 28.0, 8.0, 0.05, false),
    ]);
    alias.box_shadow = standard.clone();
    alias.box_shadow_secondary = standard;
    alias.box_shadow_tertiary = Arc::from([
        shadow(0.0, 1.0, 2.0, 0.0, 0.05, false),
        shadow(0.0, 1.0, 6.0, -1.0, 0.03, false),
        shadow(0.0, 2.0, 4.0, 0.0, 0.03, false),
    ]);
    alias.box_shadow_popover_arrow = Arc::from([shadow(2.0, 2.0, 5.0, 0.0, 0.05, false)]);
    alias.drop_shadow_popover = Arc::from([
        shadow(0.0, 6.0, 16.0, 0.0, 0.08, false),
        shadow(0.0, 3.0, 6.0, 0.0, 0.12, false),
        shadow(0.0, 9.0, 28.0, 0.0, 0.05, false),
    ]);
    alias.box_shadow_card = Arc::from([
        shadow(0.0, 1.0, 2.0, -2.0, 0.16, false),
        shadow(0.0, 3.0, 6.0, 0.0, 0.12, false),
        shadow(0.0, 5.0, 12.0, 4.0, 0.09, false),
    ]);
    alias.box_shadow_drawer_right = Arc::from([
        shadow(-6.0, 0.0, 16.0, 0.0, 0.08, false),
        shadow(-3.0, 0.0, 6.0, -4.0, 0.12, false),
        shadow(-9.0, 0.0, 28.0, 8.0, 0.05, false),
    ]);
    alias.box_shadow_drawer_left = Arc::from([
        shadow(6.0, 0.0, 16.0, 0.0, 0.08, false),
        shadow(3.0, 0.0, 6.0, -4.0, 0.12, false),
        shadow(9.0, 0.0, 28.0, 8.0, 0.05, false),
    ]);
    alias.box_shadow_drawer_down = Arc::from([
        shadow(0.0, -6.0, 16.0, 0.0, 0.08, false),
        shadow(0.0, -3.0, 6.0, -4.0, 0.12, false),
        shadow(0.0, -9.0, 28.0, 8.0, 0.05, false),
    ]);
    alias.box_shadow_drawer_up = alias.box_shadow.clone();
    alias.box_shadow_tabs_overflow_left = Arc::from([shadow(10.0, 0.0, 8.0, -8.0, 0.08, true)]);
    alias.box_shadow_tabs_overflow_right = Arc::from([shadow(-10.0, 0.0, 8.0, -8.0, 0.08, true)]);
    alias.box_shadow_tabs_overflow_top = Arc::from([shadow(0.0, 10.0, 8.0, -8.0, 0.08, true)]);
    alias.box_shadow_tabs_overflow_bottom = Arc::from([shadow(0.0, -10.0, 8.0, -8.0, 0.08, true)]);
    alias
}
