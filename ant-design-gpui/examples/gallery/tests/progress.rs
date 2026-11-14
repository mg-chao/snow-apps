// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
use super::*;

fn open_progress(f: &mut Fixture) {
    f.gallery.update(&mut f.cx, |g, cx| {
        g.selected = ComponentId::Progress;
        cx.notify();
    });
    f.draw();
    f.next_frame();
}
#[test]
fn progress_gallery_reference_order_and_native_capture_options() {
    assert_eq!(
        crate::progress::DEMOS
            .iter()
            .map(|d| d.id)
            .collect::<Vec<_>>(),
        [
            "progress-line",
            "progress-circle",
            "progress-line-mini",
            "progress-circle-micro",
            "progress-circle-mini",
            "progress-dynamic",
            "progress-format",
            "progress-dashboard",
            "progress-segment",
            "progress-linecap",
            "progress-gradient-line",
            "progress-steps",
            "progress-circle-steps",
            "progress-size",
            "progress-info-position",
            "progress-style-class",
            "progress-tokens",
        ]
    );
    let options = Options::parse([
        "--component".into(),
        "progress".into(),
        "--dark".into(),
        "--compact".into(),
        "--rtl".into(),
        "--reduced-motion".into(),
        "--headless".into(),
        "--capture".into(),
        "C:/temp/progress.png".into(),
    ])
    .unwrap();
    assert_eq!(options.component, ComponentId::Progress);
    assert!(
        options.dark
            && options.compact
            && options.rtl
            && options.reduced_motion
            && !options.interactive()
    );
}
#[test]
fn progress_gallery_navigation_and_cards_are_responsive() {
    let mut f = Fixture::new();
    f.reveal_sidebar("gallery.navigation.progress");
    f.click("gallery.navigation.progress");
    assert_eq!(
        f.gallery.read_with(&f.cx, |g, _| g.selected),
        ComponentId::Progress
    );
    for width in [1280., 1100., 640.] {
        f.resize(width);
        let first = f.bounds("gallery.card.progress-line");
        let second = f.bounds("gallery.card.progress-circle");
        let content = f.bounds("gallery.content");
        assert!(first.left() >= content.left() && second.right() <= content.right());
        if width == 1280. {
            assert_eq!(first.top(), second.top());
        } else {
            assert!(second.top() >= first.bottom());
        }
    }
    f.resize(1280.);
    f.reveal_sidebar("gallery.jump.progress-dynamic");
    f.click("gallery.jump.progress-dynamic");
    f.next_frame();
    assert!(
        (f.bounds("gallery.card.progress-dynamic").top() - f.bounds("gallery.content").top()).abs()
            <= px(1.)
    );
}
#[test]
fn progress_gallery_dynamic_value_is_bounded_and_survives_config_changes() {
    let mut f = Fixture::new();
    open_progress(&mut f);
    let page = f.gallery.read_with(&f.cx, |g, _| g.progress.clone());
    f.reveal("progress.dynamic.increase");
    for _ in 0..12 {
        f.click("progress.dynamic.increase");
    }
    assert_eq!(page.read_with(&f.cx, |p, _| p.percent), 100.);
    for _ in 0..13 {
        f.click("progress.dynamic.decrease");
    }
    assert_eq!(page.read_with(&f.cx, |p, _| p.percent), 0.);
    f.click("progress.dynamic.increase");
    for control in [
        "gallery.control.theme",
        "gallery.control.density",
        "gallery.control.direction",
    ] {
        f.click(control);
        assert_eq!(page.read_with(&f.cx, |p, _| p.percent), 10.);
    }
}
