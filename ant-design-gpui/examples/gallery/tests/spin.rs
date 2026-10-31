// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;

fn open_spin(f: &mut Fixture) {
    f.gallery.update(&mut f.cx, |gallery, cx| {
        gallery.selected = ComponentId::Spin;
        cx.notify();
    });
    f.draw();
    f.next_frame();
}

#[test]
fn spin_gallery_preserves_reference_order_and_supports_native_capture() {
    assert_eq!(
        crate::spin::DEMOS
            .iter()
            .map(|demo| (demo.id, demo.title))
            .collect::<Vec<_>>(),
        [
            ("spin-basic", "Basic Usage"),
            ("spin-size", "Size"),
            ("spin-nested", "Embedded mode"),
            ("spin-tip", "Customized description"),
            ("spin-delayAndDebounce", "Delay"),
            ("spin-custom-indicator", "Custom spinning indicator"),
            ("spin-percent", "Progress"),
            ("spin-style-class", "Custom semantic dom styling"),
            ("spin-fullscreen", "Fullscreen"),
        ],
    );
    let options = Options::parse([
        "--component".into(),
        "spin".into(),
        "--dark".into(),
        "--compact".into(),
        "--rtl".into(),
        "--reduced-motion".into(),
        "--headless".into(),
        "--capture".into(),
        "C:/temp/spin-gallery.png".into(),
    ])
    .unwrap();
    assert_eq!(options.component, ComponentId::Spin);
    assert!(!options.interactive());
    assert!(options.dark && options.compact && options.rtl && options.reduced_motion);
    assert_eq!(
        COMPONENTS
            .iter()
            .find(|component| component.id == ComponentId::Spin)
            .unwrap()
            .group,
        "Feedback"
    );
}

#[test]
fn spin_gallery_navigation_anchors_and_responsive_cards_follow_the_viewport() {
    let mut f = Fixture::new();
    f.reveal_sidebar("gallery.navigation.spin");
    f.click("gallery.navigation.spin");
    assert_eq!(
        f.gallery.read_with(&f.cx, |gallery, _| gallery.selected),
        ComponentId::Spin
    );
    for width in [1280., 1100., 640.] {
        f.resize(width);
        let first = f.bounds("gallery.card.spin-basic");
        let second = f.bounds("gallery.card.spin-size");
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
    f.reveal_sidebar("gallery.jump.spin-nested");
    f.click("gallery.jump.spin-nested");
    f.next_frame();
    let content = f.bounds("gallery.content");
    let card = f.bounds("gallery.card.spin-nested");
    assert!((card.top() - content.top()).abs() <= px(1.));
    f.click("gallery.control.top");
    f.next_frame();
    assert_eq!(
        f.gallery
            .read_with(&f.cx, |gallery, _| gallery.scroll.offset().y),
        px(0.)
    );
}

#[test]
fn spin_gallery_loading_controls_keep_content_geometry_and_survive_theme_changes() {
    let mut f = Fixture::new();
    open_spin(&mut f);
    let page = f
        .gallery
        .read_with(&f.cx, |gallery, _| gallery.spin.clone());
    f.reveal("spin.nested.toggle");
    let before = f.bounds("spin.nested.content").size;
    f.click("spin.nested.toggle");
    assert!(page.read_with(&f.cx, |page, _| page.nested_loading));
    assert_eq!(f.bounds("spin.nested.content").size, before);
    f.reveal("spin.delay.toggle");
    f.click("spin.delay.toggle");
    assert!(page.read_with(&f.cx, |page, _| page.delayed_loading));
    f.cx.executor().advance_clock(Duration::from_millis(250));
    f.draw();
    f.click("spin.delay.toggle");
    f.cx.executor().advance_clock(Duration::from_millis(501));
    f.draw();
    assert!(!page.read_with(&f.cx, |page, _| page.delayed_loading));
    for control in [
        "gallery.control.theme",
        "gallery.control.density",
        "gallery.control.direction",
    ] {
        f.click(control);
        assert!(page.read_with(&f.cx, |page, _| page.nested_loading));
        assert!(!page.read_with(&f.cx, |page, _| page.delayed_loading));
    }
}

#[test]
fn spin_gallery_progress_is_interactive_bounded_and_restartable() {
    let mut f = Fixture::new();
    open_spin(&mut f);
    let page = f
        .gallery
        .read_with(&f.cx, |gallery, _| gallery.spin.clone());
    f.reveal("spin.progress.auto");
    f.click("spin.progress.auto");
    assert!(page.read_with(&f.cx, |page, _| page.automatic));
    f.reveal("spin.progress.run");
    f.click("spin.progress.run");
    assert_eq!(
        page.read_with(&f.cx, |page, _| (
            page.percent,
            page.progress_running,
            page.automatic
        )),
        (0., true, false)
    );
    for _ in 0..3 {
        f.cx.executor().advance_clock(Duration::from_millis(100));
        f.draw();
    }
    assert_eq!(page.read_with(&f.cx, |page, _| page.percent), 15.);
    f.click("spin.progress.run");
    assert_eq!(page.read_with(&f.cx, |page, _| page.percent), 0.);
    for _ in 0..20 {
        f.cx.executor().advance_clock(Duration::from_millis(100));
        f.draw();
    }
    assert_eq!(
        page.read_with(&f.cx, |page, _| (page.percent, page.progress_running)),
        (100., false)
    );
    f.cx.executor().advance_clock(Duration::from_secs(10));
    f.draw();
    assert_eq!(page.read_with(&f.cx, |page, _| page.percent), 100.);
}

#[test]
fn spin_gallery_fullscreen_operation_completes_and_can_be_restarted() {
    let mut f = Fixture::new();
    open_spin(&mut f);
    let page = f
        .gallery
        .read_with(&f.cx, |gallery, _| gallery.spin.clone());
    f.reveal("spin.fullscreen.show");
    f.click("spin.fullscreen.show");
    assert!(page.read_with(&f.cx, |page, _| page.fullscreen));
    for _ in 0..28 {
        f.cx.executor().advance_clock(Duration::from_millis(100));
        f.draw();
    }
    assert_eq!(
        page.read_with(&f.cx, |page, _| (page.fullscreen, page.fullscreen_percent)),
        (false, 0.)
    );
    f.click("spin.fullscreen.show");
    assert!(page.read_with(&f.cx, |page, _| page.fullscreen));
}
