// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
use super::*;
use ant_design_gpui::notification::Notification;
fn open_notification(f: &mut Fixture) {
    f.gallery.update(&mut f.cx, |g, cx| {
        g.selected = ComponentId::Notification;
        cx.notify();
    });
    f.draw();
    f.next_frame();
}
#[test]
fn notification_gallery_preserves_all_reference_examples() {
    assert_eq!(
        crate::notification::DEMOS
            .iter()
            .map(|d| d.id)
            .collect::<Vec<_>>(),
        [
            "notification-hooks",
            "notification-duration",
            "notification-with-icon",
            "notification-with-btn",
            "notification-custom-icon",
            "notification-placement",
            "notification-update",
            "notification-stack",
            "notification-progress",
            "notification-window",
            "notification-progress-color",
            "notification-component-token",
            "notification-style",
        ]
    );
    let options = Options::parse([
        "--component".into(),
        "notification".into(),
        "--dark".into(),
        "--compact".into(),
        "--rtl".into(),
    ])
    .unwrap();
    assert_eq!(options.component, ComponentId::Notification);
    assert!(options.dark && options.compact && options.rtl);
}
#[test]
fn notification_gallery_navigation_anchors_and_cards_follow_the_viewport() {
    let mut f = Fixture::new();
    f.reveal_sidebar("gallery.navigation.notification");
    f.click("gallery.navigation.notification");
    assert_eq!(
        f.gallery.read_with(&f.cx, |gallery, _| gallery.selected),
        ComponentId::Notification
    );
    for width in [1280., 1100., 640.] {
        f.resize(width);
        let first = f.bounds("gallery.card.notification-hooks");
        let second = f.bounds("gallery.card.notification-duration");
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
    f.reveal_sidebar("gallery.jump.notification-stack");
    f.click("gallery.jump.notification-stack");
    f.next_frame();
    assert!(
        (f.bounds("gallery.card.notification-stack").top() - f.bounds("gallery.content").top())
            .abs()
            <= px(1.)
    );
    f.click("gallery.control.top");
    assert_eq!(
        f.gallery
            .read_with(&f.cx, |gallery, _| gallery.scroll.offset().y),
        px(0.)
    );
}

#[test]
fn notification_gallery_triggers_stacks_destruction_and_presentation_changes() {
    let mut f = Fixture::new();
    open_notification(&mut f);
    let page = f
        .gallery
        .read_with(&f.cx, |gallery, _| gallery.notification.clone());
    f.reveal("notification.open");
    f.click("notification.open");
    let state = page.read_with(&f.cx, |page, _| page.notifications.clone());
    assert_eq!(f.cx.update(|_, cx| Notification::new(&state).len(cx)), 1);
    for control in [
        "gallery.control.theme",
        "gallery.control.density",
        "gallery.control.direction",
    ] {
        f.click(control);
        assert_eq!(f.cx.update(|_, cx| Notification::new(&state).len(cx)), 1);
    }
    f.reveal("notification.stack.open");
    for _ in 0..4 {
        f.click("notification.stack.open");
    }
    let stack = page.read_with(&f.cx, |page, _| page.stack.clone());
    assert_eq!(f.cx.update(|_, cx| Notification::new(&stack).len(cx)), 4);
    f.click("notification.stack.destroy");
    assert_eq!(f.cx.update(|_, cx| Notification::new(&stack).len(cx)), 0);
    f.click("notification.stack.enabled");
    assert!(!page.read_with(&f.cx, |page, _| page.stack_enabled));
}

#[test]
fn notification_gallery_keyed_update_preserves_identity_and_uses_virtual_time() {
    let mut f = Fixture::new();
    open_notification(&mut f);
    let page = f.gallery.read_with(&f.cx, |g, _| g.notification.clone());
    let state = page.read_with(&f.cx, |p, _| p.notifications.clone());
    f.reveal("notification.update");
    f.click("notification.update");
    let before = f.cx.update(|_, cx| Notification::new(&state).layouts(cx));
    assert_eq!(before.len(), 1);
    f.cx.executor().advance_clock(Duration::from_secs(2));
    f.draw();
    let after = f.cx.update(|_, cx| Notification::new(&state).layouts(cx));
    assert_eq!(after[0].id, before[0].id);
    f.cx.executor().advance_clock(Duration::from_millis(4500));
    f.draw();
    assert_eq!(f.cx.update(|_, cx| Notification::new(&state).len(cx)), 0);
}
#[test]
fn notification_gallery_status_tokens_actions_and_window_api_are_wired() {
    let mut f = Fixture::new();
    open_notification(&mut f);
    let page = f.gallery.read_with(&f.cx, |g, _| g.notification.clone());
    let state = page.read_with(&f.cx, |p, _| p.notifications.clone());
    for trigger in [
        "notification.success",
        "notification.info",
        "notification.warning",
        "notification.error",
        "notification.custom",
        "notification.actions",
    ] {
        f.reveal(trigger);
        f.click(trigger);
        assert_eq!(f.cx.update(|_, cx| Notification::new(&state).len(cx)), 1);
        f.cx.update(|w, cx| Notification::new(&state).destroy_all(w, cx));
        f.cx.executor().advance_clock(Duration::from_secs(1));
        f.draw();
    }
    f.reveal("notification.tokens.success");
    f.click("notification.tokens.success");
    let tokens = page.read_with(&f.cx, |p, _| p.tokens.clone());
    assert_eq!(f.cx.update(|_, cx| Notification::new(&tokens).len(cx)), 1);
    f.reveal("notification.window");
    f.click("notification.window");
    assert_eq!(
        f.cx.update(|w, cx| Notification::for_window(w, cx).unwrap().len(cx)),
        1
    );
}
