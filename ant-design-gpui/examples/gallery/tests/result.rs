// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;

fn open_result(f: &mut Fixture) {
    f.gallery.update(&mut f.cx, |gallery, cx| {
        gallery.selected = ComponentId::Result;
        cx.notify();
    });
    f.draw();
    f.next_frame();
}

#[test]
fn result_gallery_preserves_reference_order_and_supports_native_capture() {
    assert_eq!(
        crate::result::DEMOS
            .iter()
            .map(|demo| (demo.id, demo.title))
            .collect::<Vec<_>>(),
        [
            ("result-success", "Success"),
            ("result-info", "Info"),
            ("result-warning", "Warning"),
            ("result-403", "403"),
            ("result-404", "404"),
            ("result-500", "500"),
            ("result-error", "Error"),
            ("result-customIcon", "Custom icon"),
            ("result-style-class", "Custom semantic dom styling"),
            ("result-component-token", "Component Token"),
        ],
    );
    let options = Options::parse([
        "--component".into(),
        "result".into(),
        "--dark".into(),
        "--compact".into(),
        "--rtl".into(),
        "--reduced-motion".into(),
        "--headless".into(),
        "--capture".into(),
        "C:/temp/result-gallery.png".into(),
    ])
    .unwrap();
    assert_eq!(options.component, ComponentId::Result);
    assert!(!options.interactive());
    assert!(options.dark && options.compact && options.rtl && options.reduced_motion);
    assert_eq!(
        COMPONENTS
            .iter()
            .find(|component| component.id == ComponentId::Result)
            .unwrap()
            .group,
        "Feedback"
    );
}

#[test]
fn result_gallery_navigation_anchors_and_responsive_cards_follow_the_viewport() {
    let mut f = Fixture::new();
    f.reveal_sidebar("gallery.navigation.result");
    f.click("gallery.navigation.result");
    assert_eq!(
        f.gallery.read_with(&f.cx, |gallery, _| gallery.selected),
        ComponentId::Result
    );
    for width in [1280., 1100., 640.] {
        f.resize(width);
        let first = f.bounds("gallery.card.result-success");
        let second = f.bounds("gallery.card.result-info");
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
    f.reveal_sidebar("gallery.jump.result-403");
    f.click("gallery.jump.result-403");
    f.next_frame();
    let content = f.bounds("gallery.content");
    let card = f.bounds("gallery.card.result-403");
    assert!((card.top() - content.top()).abs() <= px(1.));
    f.click("gallery.control.top");
    f.next_frame();
    let scroll = f
        .gallery
        .read_with(&f.cx, |gallery, _| gallery.scroll.clone());
    assert_eq!(scroll.offset().y, px(0.));
}

#[test]
fn result_gallery_actions_use_native_keyboard_and_preserve_state_across_presentation_changes() {
    let mut f = Fixture::new();
    open_result(&mut f);
    let page = f
        .gallery
        .read_with(&f.cx, |gallery, _| gallery.result.clone());
    f.reveal("result.success.console");
    let focus = page.read_with(&f.cx, |page, _| page.console_focus.clone());
    f.cx.update(|window, cx| window.focus(&focus, cx));
    f.key("enter");
    assert_eq!(
        page.read_with(&f.cx, |page, _| (page.last_action, page.action_count)),
        (Some("Go Console"), 1)
    );
    f.key("space");
    assert_eq!(page.read_with(&f.cx, |page, _| page.action_count), 2);
    for control in [
        "gallery.control.theme",
        "gallery.control.density",
        "gallery.control.direction",
    ] {
        f.click(control);
        assert_eq!(page.read_with(&f.cx, |page, _| page.action_count), 2);
    }
    f.reveal("result.success.buy");
    f.click("result.success.buy");
    assert_eq!(
        page.read_with(&f.cx, |page, _| (page.last_action, page.action_count)),
        (Some("Buy Again"), 3)
    );
    let scope = f.scope();
    assert_eq!(
        f.cx.update(|_, cx| scope.snapshot(cx).direction),
        Direction::Rtl
    );
}

#[test]
fn result_gallery_error_recovery_and_exception_actions_remain_interactive() {
    let mut f = Fixture::new();
    open_result(&mut f);
    let page = f
        .gallery
        .read_with(&f.cx, |gallery, _| gallery.result.clone());
    for (selector, expected) in [
        ("result.403.home", "Back Home"),
        ("result.404.home", "Back Home"),
        ("result.500.home", "Back Home"),
        ("result.error.thaw", "Thaw immediately >"),
        ("result.error.unlock", "Apply Unlock >"),
        ("result.custom.next", "Next"),
        ("result.semantic.object.action", "Action"),
        ("result.semantic.function.action", "Action"),
        ("result.token.console", "Go Console"),
    ] {
        f.reveal(selector);
        f.click(selector);
        assert_eq!(
            page.read_with(&f.cx, |page, _| page.last_action),
            Some(expected)
        );
    }
    assert_eq!(page.read_with(&f.cx, |page, _| page.action_count), 9);
}
