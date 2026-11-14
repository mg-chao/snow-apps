// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Independent original Ant Design token execution and browser measurements.

use serde_json::Value;

use super::*;
use crate::theme::{AlgorithmChain, ComponentThemeConfig, ThemeConfig, TokenPatch};

fn fixture() -> Value {
    serde_json::from_str(include_str!("../../fixtures/menu/reference.json"))
        .expect("independent original Menu reference fixture")
}

fn algorithms(name: &str) -> AlgorithmChain {
    match name {
        "default" => AlgorithmChain::default(),
        "dark" => AlgorithmChain::dark(),
        "compact" => AlgorithmChain::compact(),
        "dark-compact" => AlgorithmChain::dark_compact(),
        name => panic!("unexpected fixture theme {name}"),
    }
}

fn number(context: &str, field: &str, native: f64, browser: &Value) {
    let browser = browser.as_f64().unwrap_or_else(|| {
        browser
            .as_str()
            .expect("CSS numeric value")
            .trim_end_matches("px")
            .parse()
            .expect("CSS length")
    });
    assert!(
        (native - browser).abs() < 0.001,
        "{context} {field}: native={native}, browser={browser}"
    );
}

fn color(context: &str, field: &str, native: Color, browser: &Value) {
    let css = browser.as_str().expect("CSS color");
    let browser = if css == "transparent" {
        Color::TRANSPARENT
    } else if css.starts_with('#') {
        css.parse().expect("hex CSS color")
    } else {
        let channels: Vec<f64> = css
            .trim_start_matches("rgba(")
            .trim_start_matches("rgb(")
            .trim_end_matches(')')
            .split(',')
            .map(|value| value.trim().parse().expect("RGB channel"))
            .collect();
        Color {
            r: channels[0] as u8,
            g: channels[1] as u8,
            b: channels[2] as u8,
            a: channels.get(3).copied().unwrap_or(1.),
        }
    };
    assert!(
        native.r == browser.r
            && native.g == browser.g
            && native.b == browser.b
            && (native.a * 255.).round() == (browser.a * 255.).round(),
        "{context} {field}: native={native:?}, browser={browser:?}"
    );
}

fn config(name: &str, custom: bool) -> ThemeConfig {
    let mut config = ThemeConfig {
        algorithms: Some(algorithms(name)),
        ..Default::default()
    };
    if custom {
        config.components.set_theme(
            ComponentThemeConfig::<MenuTheme>::new(MenuTokenPatch {
                item_height: Some(44.),
                item_margin_inline: Some(6.),
                item_margin_block: Some(3.),
                item_border_radius: Some(10.),
                sub_menu_item_border_radius: Some(6.),
                item_padding_inline: Some(20.),
                icon_size: Some(18.),
                collapsed_icon_size: Some(22.),
                icon_margin_inline_end: Some(12.),
                collapsed_width: Some(96.),
                horizontal_line_height: Some(52.),
                horizontal_item_border_radius: Some(4.),
                item_hover_bg: Some(Color::rgb(0xe6fffb)),
                item_selected_bg: Some(Color::rgb(0xfff7e6)),
                item_selected_color: Some(Color::rgb(0xd46b08)),
                sub_menu_item_selected_color: Some(Color::rgb(0x531dab)),
                dark_item_selected_bg: Some(Color::rgb(0x531dab)),
                active_bar_width: Some(3.),
                active_bar_height: Some(4.),
                active_bar_border_width: Some(2.),
                ..Default::default()
            })
            .with_tokens(TokenPatch {
                font_size: Some(16.),
                border_radius_lg: Some(10.),
                padding_xs: Some(10.),
                line_width_focus: Some(4.),
                ..Default::default()
            }),
        );
    }
    config
}

#[test]
fn menu_all_modern_component_tokens_match_original_execution() {
    let reference = fixture();
    let cases = reference["cases"].as_array().expect("reference cases");
    assert_eq!(cases.len(), 4);
    for case in cases {
        let name = case["theme"].as_str().unwrap();
        let theme = config(name, false).resolve();
        let t = theme.component::<MenuTheme>();
        let values = &case["tokens"];
        macro_rules! numbers {
            ($($field:ident => $key:literal),+ $(,)?) => { $(number(name,$key,t.$field,&values[$key]);)+ };
        }
        macro_rules! colors {
            ($($field:ident => $key:literal),+ $(,)?) => { $(color(name,$key,t.$field,&values[$key]);)+ };
        }
        numbers! {
            dropdown_width => "dropdownWidth", group_title_line_height => "groupTitleLineHeight",
            group_title_font_size => "groupTitleFontSize", item_border_radius => "itemBorderRadius",
            sub_menu_item_border_radius => "subMenuItemBorderRadius", active_bar_width => "activeBarWidth",
            active_bar_height => "activeBarHeight", active_bar_border_width => "activeBarBorderWidth",
            item_margin_inline => "itemMarginInline", horizontal_item_border_radius => "horizontalItemBorderRadius",
            item_height => "itemHeight", collapsed_width => "collapsedWidth", item_margin_block => "itemMarginBlock",
            item_padding_inline => "itemPaddingInline", horizontal_line_height => "horizontalLineHeight",
            icon_margin_inline_end => "iconMarginInlineEnd", icon_size => "iconSize", collapsed_icon_size => "collapsedIconSize",
        }
        number(
            name,
            "zIndexPopup",
            t.z_index_popup as f64,
            &values["zIndexPopup"],
        );
        colors! {
            group_title_color => "groupTitleColor", item_color => "itemColor", item_hover_color => "itemHoverColor",
            horizontal_item_hover_color => "horizontalItemHoverColor", item_selected_color => "itemSelectedColor",
            sub_menu_item_selected_color => "subMenuItemSelectedColor", horizontal_item_selected_color => "horizontalItemSelectedColor",
            item_disabled_color => "itemDisabledColor", danger_item_color => "dangerItemColor", danger_item_hover_color => "dangerItemHoverColor",
            danger_item_selected_color => "dangerItemSelectedColor", danger_item_active_bg => "dangerItemActiveBg",
            danger_item_selected_bg => "dangerItemSelectedBg", item_bg => "itemBg", item_hover_bg => "itemHoverBg",
            sub_menu_item_bg => "subMenuItemBg", item_active_bg => "itemActiveBg", item_selected_bg => "itemSelectedBg",
            horizontal_item_selected_bg => "horizontalItemSelectedBg", horizontal_item_hover_bg => "horizontalItemHoverBg",
            popup_bg => "popupBg", dark_popup_bg => "darkPopupBg", dark_item_color => "darkItemColor",
            dark_danger_item_color => "darkDangerItemColor", dark_item_bg => "darkItemBg", dark_sub_menu_item_bg => "darkSubMenuItemBg",
            dark_item_selected_color => "darkItemSelectedColor", dark_item_selected_bg => "darkItemSelectedBg",
            dark_item_hover_bg => "darkItemHoverBg", dark_group_title_color => "darkGroupTitleColor",
            dark_item_hover_color => "darkItemHoverColor", dark_item_disabled_color => "darkItemDisabledColor",
            dark_danger_item_selected_bg => "darkDangerItemSelectedBg", dark_danger_item_hover_color => "darkDangerItemHoverColor",
            dark_danger_item_selected_color => "darkDangerItemSelectedColor", dark_danger_item_active_bg => "darkDangerItemActiveBg",
        }
    }
}

#[test]
fn menu_visual_states_match_original_browser_computed_styles() {
    let reference = fixture();
    let mut compared_rows = 0;
    for case in reference["cases"].as_array().unwrap() {
        let name = case["theme"].as_str().unwrap();
        let themes = [config(name, false).resolve(), config(name, true).resolve()];
        assert_eq!(case["menus"].as_array().unwrap().len(), 160);
        for menu in case["menus"].as_array().unwrap() {
            let options = &menu["options"];
            let context = format!("{name} {options}");
            let custom = options["custom"].as_bool().unwrap();
            let t = themes[usize::from(custom)].component::<MenuTheme>();
            let dark = options["colorTheme"] == "dark";
            let style = t.colors(if dark {
                MenuColorTheme::Dark
            } else {
                MenuColorTheme::Light
            });
            let mode = options["mode"].as_str().unwrap();
            let horizontal = mode == "horizontal";
            color(
                &context,
                "root background",
                if mode == "popup" {
                    style.popup_bg
                } else {
                    style.bg
                },
                &menu["style"]["background"],
            );
            number(
                &context,
                "font size",
                t.globals().font_size,
                &menu["style"]["fontSize"],
            );
            number(
                &context,
                "root radius",
                if mode == "popup" {
                    t.popup_radius()
                } else {
                    0.
                },
                &menu["style"]["radius"],
            );
            if mode == "collapsed" {
                number(
                    &context,
                    "collapsed width",
                    t.collapsed_width,
                    &menu["bounds"]["width"],
                );
            }
            let state = options["state"].as_str().unwrap();
            for row in menu["rows"].as_array().unwrap() {
                let flags = &row["flags"];
                let submenu = row["kind"] == "submenu";
                let selected = flags["selected"].as_bool().unwrap_or(false);
                let disabled = flags["disabled"].as_bool().unwrap_or(false);
                let row_state = MenuVisualState {
                    horizontal,
                    submenu,
                    selected: selected && !submenu,
                    descendant_selected: selected && submenu,
                    danger: flags["danger"].as_bool().unwrap_or(false),
                    disabled,
                    hovered: state == "hover",
                    active: state == "active",
                    open: flags["open"].as_bool().unwrap_or(false),
                };
                let expected = style.row(row_state);
                let context = format!("{context} row={}", row["id"]);
                color(&context, "text", expected.text, &row["style"]["color"]);
                color(
                    &context,
                    "background",
                    expected.background,
                    &row["style"]["background"],
                );
                number(
                    &context,
                    "height",
                    if horizontal {
                        t.horizontal_line_height
                    } else {
                        t.item_height
                    },
                    &row["bounds"]["height"],
                );
                number(
                    &context,
                    "row font",
                    t.globals().font_size,
                    &row["style"]["fontSize"],
                );
                number(
                    &context,
                    "icon size",
                    if mode == "collapsed" {
                        t.collapsed_icon_size
                    } else {
                        t.icon_size
                    },
                    &row["icon"]["fontSize"],
                );
                let radius = if mode == "popup" {
                    t.sub_menu_item_border_radius
                } else if horizontal && !submenu {
                    t.horizontal_item_border_radius
                } else {
                    t.item_border_radius
                };
                number(&context, "row radius", radius, &row["style"]["radius"]);
                if !horizontal {
                    number(
                        &context,
                        "row margin",
                        t.item_margin_block,
                        &row["style"]["marginTop"],
                    );
                    number(
                        &context,
                        "row inline margin",
                        t.item_margin_inline,
                        &row["style"]["marginLeft"],
                    );
                }
                if horizontal && !submenu {
                    color(
                        &context,
                        "horizontal indicator",
                        expected.indicator,
                        &row["indicator"]["borderBottomColor"],
                    );
                    number(
                        &context,
                        "active bar height",
                        style.active_bar_height,
                        &row["indicator"]["borderBottomWidth"],
                    );
                }
                if state == "focus" && !disabled {
                    color(
                        &context,
                        "focus",
                        style.focus_color,
                        &row["style"]["outlineColor"],
                    );
                    number(
                        &context,
                        "focus width",
                        style.focus_width,
                        &row["style"]["outlineWidth"],
                    );
                    number(
                        &context,
                        "focus offset",
                        style.focus_offset,
                        &row["style"]["outlineOffset"],
                    );
                }
                compared_rows += 1;
            }
            if let Some(group) = menu["group"].as_object() {
                color(&context, "group text", style.group_text, &group["color"]);
                number(
                    &context,
                    "group font",
                    t.group_title_font_size,
                    &group["fontSize"],
                );
                number(
                    &context,
                    "group line height",
                    t.group_title_line_height * t.group_title_font_size,
                    &group["lineHeight"],
                );
                number(
                    &context,
                    "group vertical padding",
                    t.globals().padding_xs,
                    &group["paddingTop"],
                );
                number(
                    &context,
                    "group horizontal padding",
                    if mode == "collapsed" {
                        t.globals().padding_xs
                    } else {
                        t.globals().padding
                    },
                    &group["paddingLeft"],
                );
            }
            if !menu["nested"].is_null() {
                color(
                    &context,
                    "inline child surface",
                    style.submenu_bg,
                    &menu["nested"]["background"],
                );
            }
        }
    }
    assert_eq!(compared_rows, 3968);
}
