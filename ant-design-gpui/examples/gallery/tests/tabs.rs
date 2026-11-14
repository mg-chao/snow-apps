// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
use super::*;
use gpui::Focusable;

fn open_tabs(f: &mut Fixture) {
    f.gallery.update(&mut f.cx, |g, cx| {
        g.selected = ComponentId::Tabs;
        cx.notify();
    });
    f.draw();
    f.next_frame();
}

#[test]
fn tabs_gallery_custom_search_accepts_keyboard_selection() {
    let mut f = Fixture::new();
    open_tabs(&mut f);
    f.reveal("gallery.tabs.card.popupRender-Search");
    f.next_frame();
    let page = f.gallery.read_with(&f.cx, |g, _| g.tabs.clone());
    let (state, editor) = page.read_with(&f.cx, |p, _| {
        (p.states["popupRender-Search"].clone(), p.search.clone())
    });
    f.cx.update(|w, cx| state.update(cx, |s, cx| s.focus(w, cx)));
    f.draw();
    f.key("tab");
    f.key("space");
    f.next_frame();
    assert!(f.cx.update(|w, cx| editor.read(cx).focus_handle(cx).is_focused(w)));
    f.cx.update(|w, cx| editor.update(cx, |s, cx| s.set_value("Tab-28", w, cx)));
    f.draw();
    let before = state.read_with(&f.cx, |s, _| s.active_key().cloned());
    f.click("gallery.tabs.search-result.28");
    assert_eq!(
        editor.read_with(&f.cx, |s, _| s.value().to_string()),
        "Tab-28"
    );
    assert_eq!(
        state.read_with(&f.cx, |s, _| s.active_key().cloned()),
        before
    );
    f.cx.update(|w, cx| editor.update(cx, |s, cx| s.set_value("Tab-29", w, cx)));
    f.draw();
    f.key("down");
    f.key("enter");
    f.next_frame();
    assert_eq!(
        state
            .read_with(&f.cx, |s, _| s.active_key().cloned())
            .as_deref(),
        Some("29")
    );
    assert!(editor.read_with(&f.cx, |s, _| s.value().is_empty()));
}

#[test]
fn tabs_gallery_native_drag_preserves_keyed_selection() {
    let mut f = Fixture::new();
    open_tabs(&mut f);
    f.reveal("gallery.tabs.card.custom-tab-bar-node");
    f.next_frame();
    let state = f.gallery.read_with(&f.cx, |g, cx| {
        g.tabs.read(cx).states["custom-tab-bar-node"].clone()
    });
    let (source, target) = state.read_with(&f.cx, |s, _| {
        (
            s.tab_bounds("1").unwrap().center(),
            s.tab_bounds("3").unwrap().center(),
        )
    });
    f.cx.simulate_mouse_down(source, gpui::MouseButton::Left, Modifiers::none());
    f.cx.simulate_mouse_move(target, gpui::MouseButton::Left, Modifiers::none());
    f.draw();
    f.cx.simulate_mouse_up(target, gpui::MouseButton::Left, Modifiers::none());
    f.draw();
    assert_eq!(
        state.read_with(&f.cx, |s, _| s
            .items()
            .entries()
            .iter()
            .map(|i| i.key.to_string())
            .collect::<Vec<_>>()),
        ["2", "3", "1"]
    );
    assert_eq!(
        state
            .read_with(&f.cx, |s, _| s.active_key().cloned())
            .as_deref(),
        Some("1")
    );
}

#[test]
fn tabs_gallery_sticky_header_pins_and_stops_at_the_content_edge() {
    let mut f = Fixture::new();
    open_tabs(&mut f);
    f.reveal("gallery.tabs.card.custom-tab-bar");
    f.next_frame();
    let (state, scroll) = f.gallery.read_with(&f.cx, |g, cx| {
        let p = g.tabs.read(cx);
        (p.states["custom-tab-bar"].clone(), p.sticky_scroll.clone())
    });
    let original = state.read_with(&f.cx, |s, _| s.tab_bounds("1").unwrap().top());
    scroll.update(&mut f.cx, |s, cx| s.scroll_to(point(px(0.), px(-80.)), cx));
    f.draw();
    f.next_frame();
    f.next_frame();
    let pinned = state.read_with(&f.cx, |s, _| s.tab_bounds("1").unwrap().top());
    assert!(
        (pinned - original).abs() < px(1.),
        "{original:?} -> {pinned:?}"
    );
    scroll.update(&mut f.cx, |s, cx| s.scroll_to(point(px(0.), px(-250.)), cx));
    f.draw();
    f.next_frame();
    f.next_frame();
    assert!(state.read_with(&f.cx, |s, _| s.tab_bounds("1").unwrap().top()) < original - px(40.));
}

#[test]
fn tabs_gallery_owner_edits_recover_selection_and_preserve_other_groups() {
    let mut f = Fixture::new();
    open_tabs(&mut f);
    let page = f.gallery.read_with(&f.cx, |g, _| g.tabs.clone());
    f.cx.update(|w, cx| {
        page.update(cx, |p, cx| {
            p.edit(
                "editable-card",
                &ant_design_gpui::tabs::TabsEdit::Add,
                w,
                cx,
            )
        })
    });
    f.draw();
    let state = page.read_with(&f.cx, |p, _| p.states["editable-card"].clone());
    let added = state.read_with(&f.cx, |s, _| s.active_key().unwrap().clone());
    assert_eq!(state.read_with(&f.cx, |s, _| s.items().len()), 4);
    f.cx.update(|w, cx| {
        page.update(cx, |p, cx| {
            p.edit(
                "editable-card",
                &ant_design_gpui::tabs::TabsEdit::Remove(added),
                w,
                cx,
            )
        })
    });
    f.draw();
    assert_eq!(
        state
            .read_with(&f.cx, |s, _| s.active_key().cloned())
            .as_deref(),
        Some("3")
    );
    assert_eq!(
        page.read_with(&f.cx, |p, cx| p.states["basic"]
            .read(cx)
            .active_key()
            .cloned())
            .as_deref(),
        Some("1")
    );
}

#[test]
fn tabs_gallery_reference_demos_fit_and_navigation_preserves_state() {
    let mut f = Fixture::new();
    assert_eq!(
        Options::parse(["--component".into(), "tabs".into()])
            .unwrap()
            .component,
        ComponentId::Tabs
    );
    f.gallery.update(&mut f.cx, |g, cx| {
        g.selected = ComponentId::Tabs;
        cx.notify();
    });
    f.draw();
    f.next_frame();
    for width in [1280., 1100., 640.] {
        f.resize(width);
        f.next_frame();
        let first = f.bounds("gallery.tabs.card.basic");
        let second = f.bounds("gallery.tabs.card.disabled");
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
    f.next_frame();
    let basic = f
        .gallery
        .read_with(&f.cx, |g, cx| g.tabs.read(cx).states["basic"].clone());
    assert!(basic.read_with(&f.cx, |s, _| s.hidden_keys().is_empty()));
    f.reveal_sidebar("gallery.jump.custom-tab-bar");
    f.click("gallery.jump.custom-tab-bar");
    f.next_frame();
    let card = f.bounds("gallery.tabs.card.custom-tab-bar");
    assert!(card.top() >= f.bounds("gallery.content").top() - px(1.));
    let page = f.gallery.read_with(&f.cx, |g, _| g.tabs.clone());
    let state = page.read_with(&f.cx, |p, _| p.states["basic"].clone());
    state.update(&mut f.cx, |s, cx| s.set_active_key(Some("3".into()), cx));
    f.gallery.update(&mut f.cx, |g, cx| {
        g.selected = ComponentId::Button;
        cx.notify();
    });
    f.draw();
    f.gallery.update(&mut f.cx, |g, cx| {
        g.selected = ComponentId::Tabs;
        cx.notify();
    });
    f.draw();
    assert_eq!(
        state
            .read_with(&f.cx, |s, _| s.active_key().cloned())
            .as_deref(),
        Some("3")
    );
    assert_eq!(crate::tabs::DEMOS.len(), 20);
}
