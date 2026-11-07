// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Inventory adapted from Ant Design 6.6.7 theme interfaces (MIT).
// The declaration below is the single inventory for structs, patches and fixture export.

use super::values::{SeedValue, ValidateToken, clone_token, to_seed};
use super::{
    Color, CubicBezier, FontStack, LineType, PresetPalettes, PresetPalettesPatch, ShadowList,
    TextDecoration,
};
use std::time::Duration;

fn default_font_stack() -> FontStack {
    FontStack::new([
        "-apple-system",
        "BlinkMacSystemFont",
        "Segoe UI",
        "Roboto",
        "Helvetica Neue",
        "Arial",
        "Noto Sans",
        "sans-serif",
        "Apple Color Emoji",
        "Segoe UI Emoji",
        "Segoe UI Symbol",
        "Noto Color Emoji",
    ])
}
fn default_code_font_stack() -> FontStack {
    FontStack::new([
        "SFMono-Regular",
        "Consolas",
        "Liberation Mono",
        "Menlo",
        "Courier",
        "monospace",
    ])
}

macro_rules! declare_tokens {
    (seed { $($seed:ident: $seed_ty:ty => $resolved_ty:ty, $seed_name:literal = $default:expr;)* }
     map { $($map:ident: $map_ty:ty, $map_name:literal;)* }
     alias { $($alias:ident: $alias_ty:ty, $alias_name:literal;)* }) => {
        /// Designer input. `None` base colors select the algorithm's light/dark default.
        #[derive(Clone, Debug, PartialEq)]
        pub struct SeedTokens { $(pub $seed: $seed_ty,)* }
        impl Default for SeedTokens { fn default() -> Self { Self { $($seed: $default,)* } } }
        impl SeedTokens { pub fn validate(&self) -> Result<(), super::ThemeError> { $(self.$seed.validate_token($seed_name)?;)* Ok(()) } }
        /// Complete output of mapping algorithms, including native preset palettes.
        #[derive(Clone, Debug, PartialEq)]
        pub struct MapTokens { $(pub $seed: $resolved_ty,)* $(pub $map: $map_ty,)* pub palettes: PresetPalettes }
        impl MapTokens {
            pub fn validate(&self) -> Result<(), super::ThemeError> { $(self.$seed.validate_token($seed_name)?;)* $(self.$map.validate_token($map_name)?;)* self.palettes.validate_token("palettes") }
            pub fn from_seed(seed: &SeedTokens) -> Self { Self { $($seed: seed.$seed.resolved(),)* $($map: Default::default(),)* palettes: Default::default() } }
            pub(crate) fn alias_shell(&self) -> AliasTokens { AliasTokens { $($seed: clone_token(&self.$seed),)* $($map: clone_token(&self.$map),)* $($alias: Default::default(),)* palettes: self.palettes.clone() } }
        }
        /// Complete global tokens consumed by controls. All lengths are logical pixels.
        #[derive(Clone, Debug, PartialEq)]
        pub struct AliasTokens { $(pub $seed: $resolved_ty,)* $(pub $map: $map_ty,)* $(pub $alias: $alias_ty,)* pub palettes: PresetPalettes }
        impl AliasTokens {
            pub fn validate(&self) -> Result<(), super::ThemeError> { $(self.$seed.validate_token($seed_name)?;)* $(self.$map.validate_token($map_name)?;)* $(self.$alias.validate_token($alias_name)?;)* self.palettes.validate_token("palettes") }
            pub fn as_seed(&self) -> SeedTokens { SeedTokens { $($seed: to_seed(&self.$seed),)* } }
        }
        /// Sparse seed override. An outer `Some(None)` restores automatic base colors.
        #[derive(Clone, Debug, Default, PartialEq)]
        pub struct SeedTokenPatch { $(pub $seed: Option<$seed_ty>,)* }
        impl SeedTokenPatch {
            pub fn validate(&self) -> Result<(), super::ThemeError> { $(self.$seed.validate_token($seed_name)?;)* Ok(()) }
            pub fn merged(parent: &Self, local: &Self) -> Self { Self { $($seed: clone_token(&local.$seed).or_else(|| clone_token(&parent.$seed)),)* } }
            pub fn apply(&self, seed: &mut SeedTokens) { $(if let Some(value) = &self.$seed { seed.$seed = clone_token(value); })* }
            pub fn is_empty(&self) -> bool { true $(&& self.$seed.is_none())* }
        }
        /// Sparse global overrides. Seed, map and alias fields retain their upstream precedence.
        #[derive(Clone, Debug, Default, PartialEq)]
        pub struct TokenPatch { $(pub $seed: Option<$resolved_ty>,)* $(pub $map: Option<$map_ty>,)* $(pub $alias: Option<$alias_ty>,)* pub palettes: PresetPalettesPatch }
        impl TokenPatch {
            pub fn validate(&self) -> Result<(), super::ThemeError> { $(self.$seed.validate_token($seed_name)?;)* $(self.$map.validate_token($map_name)?;)* $(self.$alias.validate_token($alias_name)?;)* self.palettes.validate_token("palettes") }
            pub(crate) fn seed_patch(&self) -> SeedTokenPatch { SeedTokenPatch { $($seed: self.$seed.as_ref().map(to_seed),)* } }
            pub(crate) fn without_seed(&self) -> Self { let mut patch = self.clone(); $(patch.$seed = None;)* patch }
            pub fn merged(parent: &Self, local: &Self) -> Self { Self { $($seed: clone_token(&local.$seed).or_else(|| clone_token(&parent.$seed)),)* $($map: clone_token(&local.$map).or_else(|| clone_token(&parent.$map)),)* $($alias: clone_token(&local.$alias).or_else(|| clone_token(&parent.$alias)),)* palettes: PresetPalettesPatch::merged(&parent.palettes, &local.palettes) } }
            pub fn apply_seed(&self, seed: &mut SeedTokens) { $(if let Some(value) = &self.$seed { seed.$seed = to_seed(value); })* }
            pub(crate) fn apply_map(&self, map: &mut MapTokens) { $(if let Some(value) = &self.$map { map.$map = clone_token(value); })* self.palettes.apply(&mut map.palettes); }
            pub(crate) fn apply_non_seed(&self, alias: &mut AliasTokens) { $(if let Some(value) = &self.$map { alias.$map = clone_token(value); })* $(if let Some(value) = &self.$alias { alias.$alias = clone_token(value); })* self.palettes.apply(&mut alias.palettes); }
            pub(crate) fn apply_all(&self, alias: &mut AliasTokens) { $(if let Some(value) = &self.$seed { alias.$seed = clone_token(value); })* self.apply_non_seed(alias); }
            pub fn is_empty(&self) -> bool { true $(&& self.$seed.is_none())* $(&& self.$map.is_none())* $(&& self.$alias.is_none())* && self.palettes.is_empty() }
        }
        #[cfg(test)]
        impl SeedTokens { pub(crate) fn fixture(&self) -> serde_json::Value { use super::tests::FixtureValue; let mut object = serde_json::Map::new(); $(object.insert($seed_name.into(), self.$seed.fixture_value());)* object.into() } }
        #[cfg(test)]
        impl MapTokens { pub(crate) fn fixture(&self) -> serde_json::Value { use super::tests::FixtureValue; let mut object = serde_json::Map::new(); $(object.insert($seed_name.into(), self.$seed.fixture_value());)* $(object.insert($map_name.into(), self.$map.fixture_value());)* super::tests::palette_fixture(&self.palettes, &mut object); object.into() } }
        #[cfg(test)]
        impl AliasTokens { pub(crate) fn fixture(&self) -> serde_json::Value { use super::tests::FixtureValue; let mut object = serde_json::Map::new(); $(object.insert($seed_name.into(), self.$seed.fixture_value());)* $(object.insert($map_name.into(), self.$map.fixture_value());)* $(object.insert($alias_name.into(), self.$alias.fixture_value());)* super::tests::palette_fixture(&self.palettes, &mut object); object.into() } }
        #[cfg(test)]
        impl SeedTokenPatch { pub(crate) fn from_fixture(object: &serde_json::Value) -> Self { let mut patch = Self::default(); $(if let Some(value) = object.get($seed_name) { patch.$seed = Some(super::tests::parse_fixture(value)); })* patch } }
        #[cfg(test)]
        impl TokenPatch { pub(crate) fn from_fixture(object: &serde_json::Value) -> Self { let mut patch = Self::default(); $(if let Some(value) = object.get($seed_name) { patch.$seed = Some(super::tests::parse_fixture(value)); })* $(if let Some(value) = object.get($map_name) { patch.$map = Some(super::tests::parse_fixture(value)); })* $(if let Some(value) = object.get($alias_name) { patch.$alias = Some(super::tests::parse_fixture(value)); })* patch.palettes = super::tests::palette_patch_from_fixture(object); patch } }
    }
}

declare_tokens! {
    seed {
        blue: Color => Color, "blue" = Color::rgb(0x1677ff);
        purple: Color => Color, "purple" = Color::rgb(0x722ed1);
        cyan: Color => Color, "cyan" = Color::rgb(0x13c2c2);
        green: Color => Color, "green" = Color::rgb(0x52c41a);
        magenta: Color => Color, "magenta" = Color::rgb(0xeb2f96);
        pink: Color => Color, "pink" = Color::rgb(0xeb2f96);
        red: Color => Color, "red" = Color::rgb(0xf5222d);
        orange: Color => Color, "orange" = Color::rgb(0xfa8c16);
        yellow: Color => Color, "yellow" = Color::rgb(0xfadb14);
        volcano: Color => Color, "volcano" = Color::rgb(0xfa541c);
        geekblue: Color => Color, "geekblue" = Color::rgb(0x2f54eb);
        gold: Color => Color, "gold" = Color::rgb(0xfaad14);
        lime: Color => Color, "lime" = Color::rgb(0xa0d911);
        color_primary: Color => Color, "colorPrimary" = Color::rgb(0x1677ff);
        color_success: Color => Color, "colorSuccess" = Color::rgb(0x52c41a);
        color_warning: Color => Color, "colorWarning" = Color::rgb(0xfaad14);
        color_error: Color => Color, "colorError" = Color::rgb(0xff4d4f);
        color_info: Color => Color, "colorInfo" = Color::rgb(0x1677ff);
        color_text_base: Option<Color> => Color, "colorTextBase" = None;
        color_bg_base: Option<Color> => Color, "colorBgBase" = None;
        color_link: Option<Color> => Color, "colorLink" = None;
        font_family: FontStack => FontStack, "fontFamily" = default_font_stack();
        font_family_code: FontStack => FontStack, "fontFamilyCode" = default_code_font_stack();
        font_size: f64 => f64, "fontSize" = 14.0;
        line_width: f64 => f64, "lineWidth" = 1.0;
        line_type: LineType => LineType, "lineType" = LineType::Solid;
        border_radius: f64 => f64, "borderRadius" = 6.0;
        size_unit: f64 => f64, "sizeUnit" = 4.0;
        size_step: f64 => f64, "sizeStep" = 4.0;
        size_popup_arrow: f64 => f64, "sizePopupArrow" = 16.0;
        control_height: f64 => f64, "controlHeight" = 32.0;
        z_index_base: f64 => f64, "zIndexBase" = 0.0;
        z_index_popup_base: f64 => f64, "zIndexPopupBase" = 1000.0;
        opacity_image: f64 => f64, "opacityImage" = 1.0;
        motion_unit: f64 => f64, "motionUnit" = 0.1;
        motion_base: f64 => f64, "motionBase" = 0.0;
        motion_ease_out_circ: CubicBezier => CubicBezier, "motionEaseOutCirc" = CubicBezier::new(0.08, 0.82, 0.17, 1.0);
        motion_ease_in_out_circ: CubicBezier => CubicBezier, "motionEaseInOutCirc" = CubicBezier::new(0.78, 0.14, 0.15, 0.86);
        motion_ease_in_out: CubicBezier => CubicBezier, "motionEaseInOut" = CubicBezier::new(0.645, 0.045, 0.355, 1.0);
        motion_ease_out_back: CubicBezier => CubicBezier, "motionEaseOutBack" = CubicBezier::new(0.12, 0.4, 0.29, 1.46);
        motion_ease_in_back: CubicBezier => CubicBezier, "motionEaseInBack" = CubicBezier::new(0.71, -0.46, 0.88, 0.6);
        motion_ease_in_quint: CubicBezier => CubicBezier, "motionEaseInQuint" = CubicBezier::new(0.755, 0.05, 0.855, 0.06);
        motion_ease_out_quint: CubicBezier => CubicBezier, "motionEaseOutQuint" = CubicBezier::new(0.23, 1.0, 0.32, 1.0);
        motion_ease_out: CubicBezier => CubicBezier, "motionEaseOut" = CubicBezier::new(0.215, 0.61, 0.355, 1.0);
        wireframe: bool => bool, "wireframe" = false;
        focus_outline: bool => bool, "focusOutline" = true;
        motion: bool => bool, "motion" = true;
    }
    map {
        color_shadow: Color, "colorShadow";
        color_text: Color, "colorText";
        color_text_secondary: Color, "colorTextSecondary";
        color_text_tertiary: Color, "colorTextTertiary";
        color_text_quaternary: Color, "colorTextQuaternary";
        color_border: Color, "colorBorder";
        color_border_secondary: Color, "colorBorderSecondary";
        color_border_disabled: Color, "colorBorderDisabled";
        color_fill: Color, "colorFill";
        color_fill_secondary: Color, "colorFillSecondary";
        color_fill_tertiary: Color, "colorFillTertiary";
        color_fill_quaternary: Color, "colorFillQuaternary";
        color_bg_layout: Color, "colorBgLayout";
        color_bg_container: Color, "colorBgContainer";
        color_bg_elevated: Color, "colorBgElevated";
        color_bg_spotlight: Color, "colorBgSpotlight";
        color_bg_blur: Color, "colorBgBlur";
        color_bg_solid: Color, "colorBgSolid";
        color_bg_solid_active: Color, "colorBgSolidActive";
        color_bg_solid_hover: Color, "colorBgSolidHover";
        color_primary_bg: Color, "colorPrimaryBg";
        color_primary_bg_hover: Color, "colorPrimaryBgHover";
        color_primary_border: Color, "colorPrimaryBorder";
        color_primary_border_hover: Color, "colorPrimaryBorderHover";
        color_primary_hover: Color, "colorPrimaryHover";
        color_primary_active: Color, "colorPrimaryActive";
        color_primary_text_hover: Color, "colorPrimaryTextHover";
        color_primary_text: Color, "colorPrimaryText";
        color_primary_text_active: Color, "colorPrimaryTextActive";
        color_success_bg: Color, "colorSuccessBg";
        color_success_bg_hover: Color, "colorSuccessBgHover";
        color_success_border: Color, "colorSuccessBorder";
        color_success_border_hover: Color, "colorSuccessBorderHover";
        color_success_hover: Color, "colorSuccessHover";
        color_success_active: Color, "colorSuccessActive";
        color_success_text_hover: Color, "colorSuccessTextHover";
        color_success_text: Color, "colorSuccessText";
        color_success_text_active: Color, "colorSuccessTextActive";
        color_warning_bg: Color, "colorWarningBg";
        color_warning_bg_hover: Color, "colorWarningBgHover";
        color_warning_border: Color, "colorWarningBorder";
        color_warning_border_hover: Color, "colorWarningBorderHover";
        color_warning_hover: Color, "colorWarningHover";
        color_warning_active: Color, "colorWarningActive";
        color_warning_text_hover: Color, "colorWarningTextHover";
        color_warning_text: Color, "colorWarningText";
        color_warning_text_active: Color, "colorWarningTextActive";
        color_info_bg: Color, "colorInfoBg";
        color_info_bg_hover: Color, "colorInfoBgHover";
        color_info_border: Color, "colorInfoBorder";
        color_info_border_hover: Color, "colorInfoBorderHover";
        color_info_hover: Color, "colorInfoHover";
        color_info_active: Color, "colorInfoActive";
        color_info_text_hover: Color, "colorInfoTextHover";
        color_info_text: Color, "colorInfoText";
        color_info_text_active: Color, "colorInfoTextActive";
        color_error_bg: Color, "colorErrorBg";
        color_error_bg_hover: Color, "colorErrorBgHover";
        color_error_bg_filled_hover: Color, "colorErrorBgFilledHover";
        color_error_bg_active: Color, "colorErrorBgActive";
        color_error_border: Color, "colorErrorBorder";
        color_error_border_hover: Color, "colorErrorBorderHover";
        color_error_hover: Color, "colorErrorHover";
        color_error_active: Color, "colorErrorActive";
        color_error_text_hover: Color, "colorErrorTextHover";
        color_error_text: Color, "colorErrorText";
        color_error_text_active: Color, "colorErrorTextActive";
        color_link_hover: Color, "colorLinkHover";
        color_link_active: Color, "colorLinkActive";
        color_white: Color, "colorWhite";
        color_bg_mask: Color, "colorBgMask";
        font_size_sm: f64, "fontSizeSM";
        font_size_lg: f64, "fontSizeLG";
        font_size_xl: f64, "fontSizeXL";
        font_size_heading1: f64, "fontSizeHeading1";
        font_size_heading2: f64, "fontSizeHeading2";
        font_size_heading3: f64, "fontSizeHeading3";
        font_size_heading4: f64, "fontSizeHeading4";
        font_size_heading5: f64, "fontSizeHeading5";
        line_height: f64, "lineHeight";
        line_height_lg: f64, "lineHeightLG";
        line_height_sm: f64, "lineHeightSM";
        font_height: f64, "fontHeight";
        font_height_sm: f64, "fontHeightSM";
        font_height_lg: f64, "fontHeightLG";
        line_height_heading1: f64, "lineHeightHeading1";
        line_height_heading2: f64, "lineHeightHeading2";
        line_height_heading3: f64, "lineHeightHeading3";
        line_height_heading4: f64, "lineHeightHeading4";
        line_height_heading5: f64, "lineHeightHeading5";
        size_xxl: f64, "sizeXXL";
        size_xl: f64, "sizeXL";
        size_lg: f64, "sizeLG";
        size_md: f64, "sizeMD";
        size_ms: f64, "sizeMS";
        size: f64, "size";
        size_sm: f64, "sizeSM";
        size_xs: f64, "sizeXS";
        size_xxs: f64, "sizeXXS";
        control_height_xs: f64, "controlHeightXS";
        control_height_sm: f64, "controlHeightSM";
        control_height_lg: f64, "controlHeightLG";
        line_width_bold: f64, "lineWidthBold";
        border_radius_xs: f64, "borderRadiusXS";
        border_radius_sm: f64, "borderRadiusSM";
        border_radius_lg: f64, "borderRadiusLG";
        border_radius_outer: f64, "borderRadiusOuter";
        motion_duration_fast: Duration, "motionDurationFast";
        motion_duration_mid: Duration, "motionDurationMid";
        motion_duration_slow: Duration, "motionDurationSlow";
    }
    alias {
        color_fill_content_hover: Color, "colorFillContentHover";
        color_fill_alter: Color, "colorFillAlter";
        color_fill_content: Color, "colorFillContent";
        color_bg_container_disabled: Color, "colorBgContainerDisabled";
        color_bg_text_hover: Color, "colorBgTextHover";
        color_bg_text_active: Color, "colorBgTextActive";
        color_border_bg: Color, "colorBorderBg";
        color_split: Color, "colorSplit";
        color_text_placeholder: Color, "colorTextPlaceholder";
        color_text_disabled: Color, "colorTextDisabled";
        color_text_heading: Color, "colorTextHeading";
        color_text_label: Color, "colorTextLabel";
        color_text_description: Color, "colorTextDescription";
        color_text_light_solid: Color, "colorTextLightSolid";
        color_icon: Color, "colorIcon";
        color_icon_hover: Color, "colorIconHover";
        color_highlight: Color, "colorHighlight";
        control_outline: Color, "controlOutline";
        color_warning_outline: Color, "colorWarningOutline";
        color_error_outline: Color, "colorErrorOutline";
        color_error_affix: Color, "colorErrorAffix";
        color_warning_affix: Color, "colorWarningAffix";
        font_size_icon: f64, "fontSizeIcon";
        font_weight_strong: f64, "fontWeightStrong";
        control_outline_width: f64, "controlOutlineWidth";
        control_item_bg_hover: Color, "controlItemBgHover";
        control_item_bg_active: Color, "controlItemBgActive";
        control_item_bg_active_hover: Color, "controlItemBgActiveHover";
        control_interactive_size: f64, "controlInteractiveSize";
        control_item_bg_active_disabled: Color, "controlItemBgActiveDisabled";
        line_width_focus: f64, "lineWidthFocus";
        padding_xxs: f64, "paddingXXS";
        padding_xs: f64, "paddingXS";
        padding_sm: f64, "paddingSM";
        padding: f64, "padding";
        padding_md: f64, "paddingMD";
        padding_lg: f64, "paddingLG";
        padding_xl: f64, "paddingXL";
        padding_content_horizontal_lg: f64, "paddingContentHorizontalLG";
        padding_content_horizontal: f64, "paddingContentHorizontal";
        padding_content_horizontal_sm: f64, "paddingContentHorizontalSM";
        padding_content_vertical_lg: f64, "paddingContentVerticalLG";
        padding_content_vertical: f64, "paddingContentVertical";
        padding_content_vertical_sm: f64, "paddingContentVerticalSM";
        margin_xxs: f64, "marginXXS";
        margin_xs: f64, "marginXS";
        margin_sm: f64, "marginSM";
        margin: f64, "margin";
        margin_md: f64, "marginMD";
        margin_lg: f64, "marginLG";
        margin_xl: f64, "marginXL";
        margin_xxl: f64, "marginXXL";
        opacity_loading: f64, "opacityLoading";
        box_shadow: ShadowList, "boxShadow";
        box_shadow_secondary: ShadowList, "boxShadowSecondary";
        box_shadow_tertiary: ShadowList, "boxShadowTertiary";
        link_decoration: TextDecoration, "linkDecoration";
        link_hover_decoration: TextDecoration, "linkHoverDecoration";
        link_focus_decoration: TextDecoration, "linkFocusDecoration";
        control_padding_horizontal: f64, "controlPaddingHorizontal";
        control_padding_horizontal_sm: f64, "controlPaddingHorizontalSM";
        screen_xs: f64, "screenXS";
        screen_xs_min: f64, "screenXSMin";
        screen_xs_max: f64, "screenXSMax";
        screen_sm: f64, "screenSM";
        screen_sm_min: f64, "screenSMMin";
        screen_sm_max: f64, "screenSMMax";
        screen_md: f64, "screenMD";
        screen_md_min: f64, "screenMDMin";
        screen_md_max: f64, "screenMDMax";
        screen_lg: f64, "screenLG";
        screen_lg_min: f64, "screenLGMin";
        screen_lg_max: f64, "screenLGMax";
        screen_xl: f64, "screenXL";
        screen_xl_min: f64, "screenXLMin";
        screen_xl_max: f64, "screenXLMax";
        screen_xxl: f64, "screenXXL";
        screen_xxl_min: f64, "screenXXLMin";
        screen_xxl_max: f64, "screenXXLMax";
        screen_xxxl: f64, "screenXXXL";
        screen_xxxl_min: f64, "screenXXXLMin";
        control_tmp_outline: Color, "controlTmpOutline";
        box_shadow_popover_arrow: ShadowList, "boxShadowPopoverArrow";
        drop_shadow_popover: ShadowList, "dropShadowPopover";
        box_shadow_card: ShadowList, "boxShadowCard";
        box_shadow_drawer_right: ShadowList, "boxShadowDrawerRight";
        box_shadow_drawer_left: ShadowList, "boxShadowDrawerLeft";
        box_shadow_drawer_up: ShadowList, "boxShadowDrawerUp";
        box_shadow_drawer_down: ShadowList, "boxShadowDrawerDown";
        box_shadow_tabs_overflow_left: ShadowList, "boxShadowTabsOverflowLeft";
        box_shadow_tabs_overflow_right: ShadowList, "boxShadowTabsOverflowRight";
        box_shadow_tabs_overflow_top: ShadowList, "boxShadowTabsOverflowTop";
        box_shadow_tabs_overflow_bottom: ShadowList, "boxShadowTabsOverflowBottom";
    }
}

pub type GlobalTokenPatch = TokenPatch;
pub type SeedTokensPatch = SeedTokenPatch;
