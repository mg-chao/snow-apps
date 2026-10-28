// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;
use ant_design_gpui::menu::{MenuColorTheme, MenuMode};
use gpui::SharedString;

#[test]
fn menu_gallery_navigation_and_nested_rows_fit_windows_main_thread_stack() {
    // Windows executables reserve 1 MiB for the main thread; the Rust test
    // harness normally provides 2 MiB and can hide recursive render overflows.
    std::thread::Builder::new()
        .name("menu-gallery-native-stack".into())
        .stack_size(1024 * 1024)
        .spawn(|| {
            let mut f = Fixture::new();
            f.click("gallery.navigation.menu");
            let page = f
                .gallery
                .read_with(&f.cx, |gallery, _| gallery.menu.clone());
            let state = page.read_with(&f.cx, |page, _| page.states["sider-current"].clone());
            assert_eq!(
                state.read_with(&f.cx, |state, _| state.open_keys().to_vec()),
                [SharedString::from("2"), SharedString::from("23")]
            );
            assert!(f.bounds("gallery.menu.card.sider-current").size.height > px(0.));
            f.resize(640.);
            f.resize(1280.);
            f.click("gallery.navigation.button");
            f.click("gallery.navigation.menu");
            assert_eq!(
                state.read_with(&f.cx, |state, _| state.selected_keys().to_vec()),
                [SharedString::from("231")]
            );
        })
        .expect("spawn gallery with the Windows main thread stack size")
        .join()
        .expect("render and navigate the Menu gallery on a 1 MiB stack");
}

#[test]
fn menu_gallery_preserves_public_reference_order_and_native_navigation() {
    use crate::menu::DEMOS as MENU_DEMOS;
    assert_eq!(
        MENU_DEMOS.iter().map(|demo| demo.id).collect::<Vec<_>>(),
        [
            "horizontal",
            "inline",
            "inline-collapsed",
            "tooltip",
            "sider-current",
            "vertical",
            "theme",
            "submenu-theme",
            "switch-mode",
            "style-class",
            "custom-popup-render",
        ]
    );
    assert!(
        MENU_DEMOS
            .iter()
            .all(|demo| demo.coverage == crate::input_examples::Coverage::Complete)
    );
    assert_eq!(
        Options::parse(["--component".into(), "menu".into()])
            .unwrap()
            .component,
        ComponentId::Menu
    );
    let mut f = Fixture::new();
    f.click("gallery.navigation.menu");
    assert_eq!(
        f.gallery.read_with(&f.cx, |gallery, _| gallery.selected),
        ComponentId::Menu
    );
    for width in [1280., 1100., 640.] {
        f.resize(width);
        let first = f.bounds("gallery.menu.card.horizontal");
        let second = f.bounds("gallery.menu.card.inline");
        let content = f.bounds("gallery.content");
        assert!(first.left() >= content.left());
        assert!(second.right() <= content.right());
        if width == 1280. {
            assert_eq!(first.top(), second.top());
        } else {
            assert!(second.top() >= first.bottom());
        }
    }
    f.resize(1280.);
    f.reveal_sidebar("gallery.jump.vertical");
    f.click("gallery.jump.vertical");
    f.next_frame();
    let top = f.bounds("gallery.menu.card.vertical").top();
    assert!((top - f.bounds("gallery.content").top()).abs() <= px(1.));
}

#[test]
fn menu_gallery_retains_selection_across_display_changes_and_page_switches() {
    let mut f = Fixture::new();
    f.click("gallery.navigation.menu");
    let page = f
        .gallery
        .read_with(&f.cx, |gallery, _| gallery.menu.clone());
    let state = page.read_with(&f.cx, |page, _| page.states["horizontal"].clone());
    state.update(&mut f.cx, |state, cx| {
        state.set_selected_keys(["setting:2"], cx)
    });
    f.draw();
    for control in [
        "gallery.control.theme",
        "gallery.control.density",
        "gallery.control.direction",
        "gallery.control.motion",
    ] {
        f.click(control);
        f.next_frame();
        assert_eq!(
            state.read_with(&f.cx, |state, _| state.selected_keys().to_vec()),
            [SharedString::from("setting:2")]
        );
    }
    f.click("gallery.navigation.button");
    f.click("gallery.navigation.menu");
    assert_eq!(
        state.read_with(&f.cx, |state, _| state.selected_keys().to_vec()),
        [SharedString::from("setting:2")]
    );
}

#[test]
fn menu_gallery_collapse_tooltip_and_mode_controls_update_retained_menus() {
    let mut f = Fixture::new();
    f.click("gallery.navigation.menu");
    let page = f
        .gallery
        .read_with(&f.cx, |gallery, _| gallery.menu.clone());
    f.reveal("menu.inline-collapsed.collapse");
    f.click("menu.inline-collapsed.collapse");
    let collapsed = page.read_with(&f.cx, |page, _| page.states["inline-collapsed"].clone());
    assert!(collapsed.read_with(&f.cx, |state, _| state.options().inline_collapsed));
    f.click("menu.inline-collapsed.collapse");
    assert!(!collapsed.read_with(&f.cx, |state, _| state.options().inline_collapsed));
    f.reveal("menu.tooltip.enabled");
    f.click("menu.tooltip.enabled");
    let tooltip = page.read_with(&f.cx, |page, _| page.states["tooltip"].clone());
    assert!(!tooltip.read_with(&f.cx, |state, _| state.options().tooltip));
    f.reveal("menu.switch-mode.mode");
    f.click("menu.switch-mode.mode");
    f.click("menu.switch-mode.theme");
    let mode = page.read_with(&f.cx, |page, _| page.states["switch-mode"].clone());
    assert_eq!(
        mode.read_with(&f.cx, |state, _| state.options().mode),
        MenuMode::Vertical
    );
    assert_eq!(
        mode.read_with(&f.cx, |state, _| state.options().color_theme),
        MenuColorTheme::Dark
    );
}

#[test]
fn menu_gallery_controlled_accordion_closes_other_levels_and_selects_with_keyboard() {
    let mut f = Fixture::new();
    f.click("gallery.navigation.menu");
    f.reveal("gallery.menu.card.sider-current");
    let page = f
        .gallery
        .read_with(&f.cx, |gallery, _| gallery.menu.clone());
    let state = page.read_with(&f.cx, |page, _| page.states["sider-current"].clone());
    f.cx.update(|window, cx| state.update(cx, |state, cx| state.focus(window, cx)));
    f.key("home");
    f.key("space");
    assert_eq!(
        page.read_with(&f.cx, |page, _| page.accordion_open.clone()),
        [SharedString::from("1")]
    );
    assert_eq!(
        state.read_with(&f.cx, |state, _| state.open_keys().to_vec()),
        [SharedString::from("1")]
    );
    f.key("down");
    f.key("down");
    f.key("enter");
    assert_eq!(
        state.read_with(&f.cx, |state, _| state.selected_keys().to_vec()),
        [SharedString::from("12")]
    );
    f.key("end");
    f.key("space");
    assert_eq!(
        page.read_with(&f.cx, |page, _| page.accordion_open.clone()),
        [SharedString::from("3")]
    );
}
