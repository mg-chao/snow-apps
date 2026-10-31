// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Visual defaults and state rules adapted from Ant Design 6.6.7 (MIT).

//! Cached Menu tokens and pure visual resolution, independent of GPUI events.

use std::sync::Arc;

use crate::theme::{AliasTokens, Color, ComponentTheme, ResolvedTheme, ShadowList};

use super::MenuColorTheme;

/// Marker for Menu-specific token overrides and mapping algorithms.
pub struct MenuTheme;

macro_rules! valid_lengths {
    ($tokens:ident, $defaults:ident; $($field:ident),+ $(,)?) => { $(
        if !($tokens.$field as f32).is_finite() || $tokens.$field < 0. {
            $tokens.$field = $defaults.$field;
        }
    )+ };
}

macro_rules! menu_tokens {
    ($($field:ident: $ty:ty = $value:expr;)+) => {
        /// Ant Design Menu component tokens. Dimensions use logical pixels;
        /// `group_title_line_height` is a font-size multiplier.
        #[derive(Clone, Debug, PartialEq)]
        pub struct MenuTokens {
            $(pub $field: $ty,)+
            globals: Arc<AliasTokens>,
            font: gpui::Font,
            popup_shadows: Arc<[gpui::BoxShadow]>,
        }

        /// Optional overrides; absent fields inherit from the enclosing scope.
        #[derive(Clone, Debug, Default, PartialEq)]
        pub struct MenuTokenPatch { $(pub $field: Option<$ty>,)+ }

        impl ComponentTheme for MenuTheme {
            type Tokens = MenuTokens;
            type Patch = MenuTokenPatch;

            fn default_tokens(g: &AliasTokens) -> MenuTokens {
                MenuTokens {
                    $($field: ($value)(g),)+
                    globals: Arc::new(g.clone()),
                    font: g.font_family.to_gpui_font(),
                    popup_shadows: g.box_shadow_secondary.iter().map(|shadow| gpui::BoxShadow {
                        inset: shadow.inset,
                        color: shadow.color.to_hsla(),
                        offset: gpui::point(gpui::px(shadow.x as f32), gpui::px(shadow.y as f32)),
                        blur_radius: gpui::px(shadow.blur as f32),
                        spread_radius: gpui::px(shadow.spread as f32),
                    }).collect(),
                }
            }

            fn merge(parent: &MenuTokenPatch, local: &MenuTokenPatch) -> MenuTokenPatch {
                MenuTokenPatch { $($field: local.$field.or(parent.$field),)+ }
            }

            fn apply(tokens: &mut MenuTokens, patch: &MenuTokenPatch) {
                $(if let Some(value) = patch.$field { tokens.$field = value; })+
                // Non-finite/negative dimensions cannot enter native layout.
                let defaults = Self::default_tokens(&tokens.globals);
                valid_lengths!(tokens, defaults; dropdown_width, group_title_line_height, group_title_font_size,
                    item_border_radius, sub_menu_item_border_radius, active_bar_width,
                    active_bar_height, active_bar_border_width, item_margin_inline,
                    horizontal_item_border_radius, item_height, collapsed_width,
                    item_margin_block, item_padding_inline, horizontal_line_height,
                    icon_margin_inline_end, icon_size, collapsed_icon_size);
            }
        }
    };
}

menu_tokens! {
    dropdown_width: f64 = |_: &AliasTokens| 160.;
    z_index_popup: usize = |g: &AliasTokens| g.z_index_popup_base as usize + 50;
    group_title_color: Color = |g: &AliasTokens| g.color_text_description;
    group_title_line_height: f64 = |g: &AliasTokens| g.line_height;
    group_title_font_size: f64 = |g: &AliasTokens| g.font_size;
    item_border_radius: f64 = |g: &AliasTokens| g.border_radius_lg;
    sub_menu_item_border_radius: f64 = |g: &AliasTokens| g.border_radius_sm;
    item_color: Color = |g: &AliasTokens| g.color_text;
    item_hover_color: Color = |g: &AliasTokens| g.color_text;
    horizontal_item_hover_color: Color = |g: &AliasTokens| g.color_primary;
    item_selected_color: Color = |g: &AliasTokens| g.color_primary;
    sub_menu_item_selected_color: Color = |g: &AliasTokens| g.color_primary;
    horizontal_item_selected_color: Color = |g: &AliasTokens| g.color_primary;
    item_disabled_color: Color = |g: &AliasTokens| g.color_text_disabled;
    danger_item_color: Color = |g: &AliasTokens| g.color_error;
    danger_item_hover_color: Color = |g: &AliasTokens| g.color_error;
    danger_item_selected_color: Color = |g: &AliasTokens| g.color_error;
    danger_item_active_bg: Color = |g: &AliasTokens| g.color_error_bg;
    danger_item_selected_bg: Color = |g: &AliasTokens| g.color_error_bg;
    item_bg: Color = |g: &AliasTokens| g.color_bg_container;
    item_hover_bg: Color = |g: &AliasTokens| g.color_bg_text_hover;
    sub_menu_item_bg: Color = |g: &AliasTokens| g.color_fill_alter;
    item_active_bg: Color = |g: &AliasTokens| g.control_item_bg_active;
    item_selected_bg: Color = |g: &AliasTokens| g.control_item_bg_active;
    horizontal_item_selected_bg: Color = |_: &AliasTokens| Color::TRANSPARENT;
    active_bar_width: f64 = |_: &AliasTokens| 0.;
    active_bar_height: f64 = |g: &AliasTokens| g.line_width_bold;
    active_bar_border_width: f64 = |g: &AliasTokens| g.line_width;
    item_margin_inline: f64 = |g: &AliasTokens| g.margin_xxs;
    horizontal_item_hover_bg: Color = |_: &AliasTokens| Color::TRANSPARENT;
    horizontal_item_border_radius: f64 = |_: &AliasTokens| 0.;
    item_height: f64 = |g: &AliasTokens| g.control_height_lg;
    collapsed_width: f64 = |g: &AliasTokens| g.control_height_lg * 2.;
    popup_bg: Color = |g: &AliasTokens| g.color_bg_elevated;
    item_margin_block: f64 = |g: &AliasTokens| g.margin_xxs;
    item_padding_inline: f64 = |g: &AliasTokens| g.padding;
    horizontal_line_height: f64 = |g: &AliasTokens| g.control_height_lg * 1.15;
    icon_margin_inline_end: f64 = |g: &AliasTokens| g.control_height_sm - g.font_size;
    icon_size: f64 = |g: &AliasTokens| g.font_size;
    collapsed_icon_size: f64 = |g: &AliasTokens| g.font_size_lg;
    dark_popup_bg: Color = |_: &AliasTokens| Color::rgb(0x001529);
    dark_item_color: Color = |g: &AliasTokens| g.color_text_light_solid.with_alpha(0.65);
    dark_danger_item_color: Color = |g: &AliasTokens| g.color_error;
    dark_item_bg: Color = |_: &AliasTokens| Color::rgb(0x001529);
    dark_sub_menu_item_bg: Color = |_: &AliasTokens| Color::rgb(0x000c17);
    dark_item_selected_color: Color = |g: &AliasTokens| g.color_text_light_solid;
    dark_item_selected_bg: Color = |g: &AliasTokens| g.color_primary;
    dark_item_hover_bg: Color = |_: &AliasTokens| Color::TRANSPARENT;
    dark_group_title_color: Color = |g: &AliasTokens| g.color_text_light_solid.with_alpha(0.65);
    dark_item_hover_color: Color = |g: &AliasTokens| g.color_text_light_solid;
    dark_item_disabled_color: Color = |g: &AliasTokens| g.color_text_light_solid.with_alpha(0.25);
    dark_danger_item_selected_bg: Color = |g: &AliasTokens| g.color_error;
    dark_danger_item_hover_color: Color = |g: &AliasTokens| g.color_error_hover;
    dark_danger_item_selected_color: Color = |g: &AliasTokens| g.color_text_light_solid;
    dark_danger_item_active_bg: Color = |g: &AliasTokens| g.color_error;
}

impl MenuTokens {
    pub fn globals(&self) -> &AliasTokens {
        &self.globals
    }

    pub fn font(&self) -> &gpui::Font {
        &self.font
    }

    pub fn popup_shadow(&self) -> &ShadowList {
        &self.globals.box_shadow_secondary
    }

    pub fn popup_shadows(&self) -> Arc<[gpui::BoxShadow]> {
        self.popup_shadows.clone()
    }

    /// The default arrow is a pair of rounded strokes derived from font size.
    pub fn arrow_size(&self) -> f64 {
        self.globals.font_size / 7. * 5.
    }

    pub fn popup_radius(&self) -> f64 {
        self.globals.border_radius_lg
    }

    pub fn popup_gap(&self) -> f64 {
        self.globals.padding_xs
    }

    pub fn popup_viewport_inset(&self) -> f64 {
        self.globals.control_height_lg * 2.5
    }

    pub fn colors(&self, color_theme: MenuColorTheme) -> MenuStyle {
        MenuStyle::from_tokens(self, color_theme)
    }
}

/// Effective colors after the explicit Menu light/dark theme is applied.
/// This choice is independent of the global dark mapping algorithm.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct MenuStyle {
    pub bg: Color,
    pub item_bg: Color,
    pub submenu_bg: Color,
    pub popup_bg: Color,
    pub text: Color,
    pub hover_text: Color,
    pub hover_bg: Color,
    pub active_bg: Color,
    pub selected_text: Color,
    pub selected_bg: Color,
    pub submenu_selected_text: Color,
    pub disabled_text: Color,
    pub danger_text: Color,
    pub danger_hover_text: Color,
    pub danger_hover_bg: Color,
    pub danger_active_bg: Color,
    pub danger_selected_text: Color,
    pub danger_selected_bg: Color,
    pub group_text: Color,
    pub split: Color,
    pub horizontal_selected_text: Color,
    pub horizontal_selected_bg: Color,
    pub horizontal_hover_bg: Color,
    pub active_bar_height: f64,
    pub active_bar_width: f64,
    pub active_bar_border_width: f64,
    pub focus_color: Color,
    pub focus_width: f64,
    pub focus_offset: f64,
}

/// Interactions needed to resolve a row without rebuilding theme aliases.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct MenuVisualState {
    pub horizontal: bool,
    pub submenu: bool,
    pub selected: bool,
    pub descendant_selected: bool,
    pub danger: bool,
    pub disabled: bool,
    pub hovered: bool,
    pub active: bool,
    pub open: bool,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct MenuRowStyle {
    pub text: Color,
    pub background: Color,
    pub indicator: Color,
}

impl MenuStyle {
    pub fn resolve(theme: &ResolvedTheme, color_theme: MenuColorTheme) -> Self {
        Self::from_tokens(&theme.component::<MenuTheme>(), color_theme)
    }

    pub fn from_tokens(t: &MenuTokens, color_theme: MenuColorTheme) -> Self {
        let g = t.globals();
        let dark = color_theme == MenuColorTheme::Dark;
        let themed = |light, dark_value| if dark { dark_value } else { light };
        Self {
            bg: themed(t.item_bg, t.dark_item_bg),
            item_bg: themed(t.item_bg, t.dark_item_bg),
            submenu_bg: themed(t.sub_menu_item_bg, t.dark_sub_menu_item_bg),
            popup_bg: themed(t.popup_bg, t.dark_popup_bg),
            text: themed(t.item_color, t.dark_item_color),
            hover_text: themed(t.item_hover_color, t.dark_item_hover_color),
            hover_bg: themed(t.item_hover_bg, t.dark_item_hover_bg),
            active_bg: themed(t.item_active_bg, Color::TRANSPARENT),
            selected_text: themed(t.item_selected_color, t.dark_item_selected_color),
            selected_bg: themed(t.item_selected_bg, t.dark_item_selected_bg),
            submenu_selected_text: themed(
                t.sub_menu_item_selected_color,
                t.dark_item_selected_color,
            ),
            disabled_text: themed(t.item_disabled_color, t.dark_item_disabled_color),
            danger_text: themed(t.danger_item_color, t.dark_danger_item_color),
            danger_hover_text: themed(t.danger_item_hover_color, t.dark_danger_item_hover_color),
            danger_hover_bg: themed(t.item_hover_bg, t.dark_item_hover_bg),
            danger_active_bg: themed(t.danger_item_active_bg, t.dark_danger_item_active_bg),
            danger_selected_text: themed(
                t.danger_item_selected_color,
                t.dark_danger_item_selected_color,
            ),
            danger_selected_bg: themed(t.danger_item_selected_bg, t.dark_danger_item_selected_bg),
            group_text: themed(t.group_title_color, t.dark_group_title_color),
            split: g.color_split,
            horizontal_selected_text: themed(
                t.horizontal_item_selected_color,
                t.dark_item_selected_color,
            ),
            horizontal_selected_bg: themed(t.horizontal_item_selected_bg, t.dark_item_selected_bg),
            horizontal_hover_bg: t.horizontal_item_hover_bg,
            active_bar_height: if dark { 0. } else { t.active_bar_height },
            active_bar_width: t.active_bar_width,
            active_bar_border_width: if dark { 0. } else { t.active_bar_border_width },
            focus_color: g.color_primary_border,
            focus_width: if g.focus_outline {
                g.line_width_focus
            } else {
                0.
            },
            focus_offset: 1.,
        }
    }

    /// Preserve the original selector precedence, including the distinct
    /// horizontal danger/selection rules and disabled selected rows.
    pub fn row(self, state: MenuVisualState) -> MenuRowStyle {
        let transparent = Color::TRANSPARENT;
        if state.disabled {
            return MenuRowStyle {
                text: self.disabled_text,
                background: transparent,
                indicator: transparent,
            };
        }
        let selected = state.selected && !state.submenu;
        let text = if state.submenu && state.descendant_selected {
            self.submenu_selected_text
        } else if selected && state.horizontal {
            self.horizontal_selected_text
        } else if selected && state.danger {
            self.danger_selected_text
        } else if selected {
            self.selected_text
        } else if state.hovered && state.danger {
            self.danger_hover_text
        } else if state.hovered {
            self.hover_text
        } else if state.danger {
            self.danger_text
        } else {
            self.text
        };
        let background = if state.horizontal {
            if state.active && state.danger {
                self.danger_active_bg
            } else if selected {
                self.horizontal_selected_bg
            } else if state.hovered {
                self.horizontal_hover_bg
            } else {
                transparent
            }
        } else if selected && state.danger {
            self.danger_selected_bg
        } else if selected {
            self.selected_bg
        } else if state.active {
            self.active_bg
        } else if state.hovered {
            self.hover_bg
        } else {
            transparent
        };
        let indicator = if state.horizontal {
            if selected || state.descendant_selected || state.hovered || state.open {
                self.horizontal_selected_text
            } else {
                transparent
            }
        } else if selected {
            if state.danger {
                self.danger_selected_text
            } else {
                self.selected_text
            }
        } else {
            transparent
        };
        MenuRowStyle {
            text,
            background,
            indicator,
        }
    }
}

#[cfg(test)]
#[path = "style_tests.rs"]
mod tests;

#[cfg(test)]
#[path = "reference_tests.rs"]
mod reference_tests;
