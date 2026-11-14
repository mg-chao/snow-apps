// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;
use ant_design_gpui::Size;

fn open_descriptions(f: &mut Fixture) {
    f.gallery.update(&mut f.cx, |gallery, cx| {
        gallery.selected = ComponentId::Descriptions;
        cx.notify();
    });
    f.draw();
    f.next_frame();
}

#[test]
fn descriptions_gallery_keeps_public_reference_order_and_native_capture_options() {
    assert_eq!(
        crate::descriptions::DEMOS
            .iter()
            .map(|demo| demo.id)
            .collect::<Vec<_>>(),
        [
            "descriptions-basic",
            "descriptions-border",
            "descriptions-size",
            "descriptions-responsive",
            "descriptions-vertical",
            "descriptions-vertical-border",
            "descriptions-style-class",
            "descriptions-block",
            "descriptions-component-token"
        ],
    );
    let options = Options::parse([
        "--component".into(),
        "descriptions".into(),
        "--dark".into(),
        "--compact".into(),
        "--rtl".into(),
        "--reduced-motion".into(),
        "--headless".into(),
        "--capture".into(),
        "C:/temp/descriptions-gallery.png".into(),
    ])
    .unwrap();
    assert_eq!(options.component, ComponentId::Descriptions);
    assert!(!options.interactive());
    assert!(options.dark && options.compact && options.rtl && options.reduced_motion);
    assert_eq!(
        COMPONENTS
            .iter()
            .find(|component| component.id == ComponentId::Descriptions)
            .unwrap()
            .group,
        "Data Display"
    );
}

#[test]
fn descriptions_gallery_navigation_and_full_width_cards_follow_the_viewport() {
    let mut f = Fixture::new();
    f.reveal_sidebar("gallery.navigation.descriptions");
    f.click("gallery.navigation.descriptions");
    assert_eq!(
        f.gallery.read_with(&f.cx, |gallery, _| gallery.selected),
        ComponentId::Descriptions
    );
    for width in [1280., 1100., 640.] {
        f.resize(width);
        let first = f.bounds("gallery.card.descriptions-basic");
        let second = f.bounds("gallery.card.descriptions-border");
        let content = f.bounds("gallery.content");
        assert!(first.left() >= content.left());
        assert!(second.right() <= content.right());
        assert_eq!(first.left(), second.left());
        assert_eq!(first.size.width, second.size.width);
        assert!(second.top() >= first.bottom());
    }
    f.resize(1280.);
    f.reveal_sidebar("gallery.jump.descriptions-responsive");
    f.click("gallery.jump.descriptions-responsive");
    f.next_frame();
    assert!(
        (f.bounds("gallery.card.descriptions-responsive").top()
            - f.bounds("gallery.content").top())
        .abs()
            <= px(1.)
    );
    f.click("gallery.control.top");
    f.next_frame();
    assert_eq!(
        f.gallery
            .read_with(&f.cx, |gallery, _| gallery.scroll.offset().y),
        px(0.)
    );
}

#[test]
fn descriptions_gallery_size_and_edit_actions_preserve_state_across_display_changes() {
    let mut f = Fixture::new();
    open_descriptions(&mut f);
    let page = f
        .gallery
        .read_with(&f.cx, |gallery, _| gallery.descriptions.clone());
    f.reveal("descriptions.size.control");
    f.click_checkbox("descriptions.size.control");
    f.key("right");
    f.key("right");
    assert_eq!(page.read_with(&f.cx, |page, _| page.size), Size::Small);
    f.reveal("descriptions.size.edit");
    let focus = page.read_with(&f.cx, |page, _| page.edit_focus.clone());
    f.cx.update(|window, cx| window.focus(&focus, cx));
    f.key("enter");
    f.key("space");
    assert_eq!(page.read_with(&f.cx, |page, _| page.edit_count), 2);
    for control in [
        "gallery.control.theme",
        "gallery.control.density",
        "gallery.control.direction",
    ] {
        f.click(control);
        assert_eq!(
            page.read_with(&f.cx, |page, _| (page.size, page.edit_count)),
            (Size::Small, 2)
        );
    }
    f.reveal("descriptions.size.plain-edit");
    f.click("descriptions.size.plain-edit");
    assert_eq!(page.read_with(&f.cx, |page, _| page.edit_count), 3);
}
