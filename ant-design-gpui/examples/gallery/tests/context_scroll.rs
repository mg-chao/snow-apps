// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;

#[test]
fn context_scroll_gallery_pages_fit_and_preserve_example_navigation() {
    let mut f = Fixture::new();
    for (name, id, first_id, second_id, anchor_id, card_id) in [
        (
            "context-menu",
            ComponentId::ContextMenu,
            "gallery.context-menu.card.basic",
            "gallery.context-menu.card.nested",
            "gallery.jump.selection",
            "gallery.context-menu.card.selection",
        ),
        (
            "scroll-area",
            ComponentId::ScrollArea,
            "gallery.scroll-area.card.vertical",
            "gallery.scroll-area.card.horizontal",
            "gallery.jump.both",
            "gallery.scroll-area.card.both",
        ),
    ] {
        assert_eq!(
            Options::parse(["--component".into(), name.into()])
                .unwrap()
                .component,
            id
        );
        f.gallery.update(&mut f.cx, |gallery, cx| {
            gallery.selected = id;
            gallery.scroll.set_offset(Default::default());
            cx.notify();
        });
        for width in [1280., 1100., 640.] {
            f.resize(width);
            let first = f.bounds(first_id);
            let second = f.bounds(second_id);
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
        f.reveal_sidebar(anchor_id);
        let scroll = f
            .gallery
            .read_with(&f.cx, |gallery, _| gallery.scroll.clone());
        let expected_offset = (scroll.offset().y + f.bounds("gallery.content").top()
            - f.bounds(card_id).top())
        .clamp(-scroll.max_offset().y, px(0.));
        f.click(anchor_id);
        f.next_frame();
        assert!((scroll.offset().y - expected_offset).abs() <= px(1.));
        assert!(f.bounds(card_id).top() >= f.bounds("gallery.content").top() - px(1.));
    }
}

#[test]
fn context_scroll_gallery_scrolls_without_moving_outer_page_and_retains_position() {
    let mut f = Fixture::new();
    f.click("gallery.navigation.scroll-area");
    let page = f
        .gallery
        .read_with(&f.cx, |gallery, _| gallery.scroll_area.clone());
    let state = page.read_with(&f.cx, |page, _| page.states["vertical"].clone());
    let region = f.bounds("gallery.scroll-area.preview.vertical");
    f.scroll(region.center(), -180.);
    let offset = state.read_with(&f.cx, |state, _| state.offset());
    assert!(offset.y < px(0.));
    assert_eq!(
        f.gallery
            .read_with(&f.cx, |gallery, _| gallery.scroll.offset().y),
        px(0.)
    );
    for control in [
        "gallery.control.theme",
        "gallery.control.density",
        "gallery.control.direction",
        "gallery.control.motion",
    ] {
        f.click(control);
        f.next_frame();
        assert_eq!(state.read_with(&f.cx, |state, _| state.offset()), offset);
    }
    let latest = page.read_with(&f.cx, |page, _| page.states["programmatic"].clone());
    f.reveal("gallery.scroll-area.card.programmatic");
    latest.update(&mut f.cx, |state, cx| state.scroll_to_bottom(cx));
    f.draw();
    assert!(latest.read_with(&f.cx, |state, _| state.offset().y) < px(-100.));
    latest.update(&mut f.cx, |state, cx| state.scroll_to_top(cx));
    f.draw();
    assert_eq!(latest.read_with(&f.cx, |state, _| state.offset().y), px(0.));
}

#[test]
fn context_scroll_gallery_menu_activates_once_and_dismisses_from_keyboard() {
    let mut f = Fixture::new();
    f.click("gallery.navigation.context-menu");
    let page = f
        .gallery
        .read_with(&f.cx, |gallery, _| gallery.context_menu.clone());
    let state = page.read_with(&f.cx, |page, _| page.states["basic"].clone());
    let position = f.bounds("context-menu.basic.target").center();
    f.cx.simulate_mouse_down(position, gpui::MouseButton::Right, Modifiers::none());
    f.cx.simulate_mouse_up(position, gpui::MouseButton::Right, Modifiers::none());
    f.draw();
    assert!(state.read_with(&f.cx, |state, _| state.is_open()));
    f.key("enter");
    assert!(!state.read_with(&f.cx, |state, _| state.is_open()));
    assert_eq!(
        page.read_with(&f.cx, |page, _| page.last_action.clone()),
        "Selected: Open"
    );
    f.key("shift-f10");
    assert!(state.read_with(&f.cx, |state, _| state.is_open()));
    f.key("escape");
    assert!(!state.read_with(&f.cx, |state, _| state.is_open()));
}
