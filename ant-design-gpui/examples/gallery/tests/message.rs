// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;
use ant_design_gpui::message::Message;

fn open_message(f: &mut Fixture) {
    f.gallery.update(&mut f.cx, |gallery, cx| {
        gallery.selected = ComponentId::Message;
        cx.notify();
    });
    f.draw();
    f.next_frame();
}

#[test]
fn message_gallery_preserves_reference_order_and_supports_native_capture() {
    assert_eq!(
        crate::message::DEMOS
            .iter()
            .map(|demo| demo.id)
            .collect::<Vec<_>>(),
        [
            "message-hooks",
            "message-other",
            "message-duration",
            "message-stack",
            "message-loading",
            "message-thenable",
            "message-style-class",
            "message-update",
            "message-info",
            "message-component-token",
        ]
    );
    let options = Options::parse([
        "--component".into(),
        "message".into(),
        "--dark".into(),
        "--compact".into(),
        "--rtl".into(),
        "--reduced-motion".into(),
        "--headless".into(),
        "--capture".into(),
        "C:/temp/message-gallery.png".into(),
        "--capture-scroll-y".into(),
        "123.5".into(),
    ])
    .unwrap();
    assert_eq!(options.component, ComponentId::Message);
    assert!(!options.interactive());
    assert!(options.dark && options.compact && options.rtl && options.reduced_motion);
    assert_eq!(options.capture_scroll_y, Some(123.5));
    for invalid in ["-1", "NaN", "inf", "-inf", "invalid"] {
        assert!(
            Options::parse([
                "--capture".into(),
                "C:/temp/message-gallery.png".into(),
                "--capture-scroll-y".into(),
                invalid.into(),
            ])
            .is_err()
        );
    }
    assert!(Options::parse(["--capture-scroll-y".into()]).is_err());
    assert!(Options::parse(["--capture-scroll-y".into(), "0".into()]).is_err());
    assert_eq!(
        Options::parse([
            "--capture".into(),
            "C:/temp/message-gallery.png".into(),
            "--capture-scroll-y".into(),
            "0".into(),
        ])
        .unwrap()
        .capture_scroll_y,
        Some(0.)
    );
    assert_eq!(
        COMPONENTS
            .iter()
            .find(|component| component.id == ComponentId::Message)
            .unwrap()
            .group,
        "Feedback"
    );
}

#[test]
fn message_gallery_navigation_anchors_and_cards_follow_the_viewport() {
    let mut f = Fixture::new();
    f.reveal_sidebar("gallery.navigation.message");
    f.click("gallery.navigation.message");
    assert_eq!(
        f.gallery.read_with(&f.cx, |gallery, _| gallery.selected),
        ComponentId::Message
    );
    for width in [1280., 1100., 640.] {
        f.resize(width);
        let first = f.bounds("gallery.card.message-hooks");
        let second = f.bounds("gallery.card.message-other");
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
    f.reveal_sidebar("gallery.jump.message-stack");
    f.click("gallery.jump.message-stack");
    f.next_frame();
    assert!(
        (f.bounds("gallery.card.message-stack").top() - f.bounds("gallery.content").top()).abs()
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
fn message_gallery_triggers_stacks_destruction_and_presentation_changes() {
    let mut f = Fixture::new();
    open_message(&mut f);
    let page = f
        .gallery
        .read_with(&f.cx, |gallery, _| gallery.message.clone());
    f.reveal("message.normal");
    f.click("message.normal");
    let state = page.read_with(&f.cx, |page, _| page.messages.clone());
    assert_eq!(f.cx.update(|_, cx| Message::new(&state).len(cx)), 1);
    for control in [
        "gallery.control.theme",
        "gallery.control.density",
        "gallery.control.direction",
    ] {
        f.click(control);
        assert_eq!(f.cx.update(|_, cx| Message::new(&state).len(cx)), 1);
    }
    f.reveal("message.stack.open");
    for _ in 0..4 {
        f.click("message.stack.open");
    }
    let stack = page.read_with(&f.cx, |page, _| page.stack_messages.clone());
    assert_eq!(f.cx.update(|_, cx| Message::new(&stack).len(cx)), 4);
    f.click("message.stack.destroy");
    assert_eq!(f.cx.update(|_, cx| Message::new(&stack).len(cx)), 0);
    f.click("message.stack.enabled");
    assert!(!page.read_with(&f.cx, |page, _| page.stack_enabled));
}

#[test]
fn message_gallery_keyed_update_loading_and_completion_chain_use_virtual_time() {
    let mut f = Fixture::new();
    open_message(&mut f);
    let page = f
        .gallery
        .read_with(&f.cx, |gallery, _| gallery.message.clone());
    f.reveal("message.update");
    f.click("message.update");
    let state = page.read_with(&f.cx, |page, _| page.messages.clone());
    f.cx.executor().advance_clock(Duration::from_secs(1));
    f.draw();
    assert_eq!(page.read_with(&f.cx, |page, _| page.updates), 1);
    assert_eq!(f.cx.update(|_, cx| Message::new(&state).len(cx)), 1);
    f.reveal("message.loading");
    f.click("message.loading");
    assert!(page.read_with(&f.cx, |page, _| page.loading));
    f.cx.executor().advance_clock(Duration::from_millis(2500));
    f.draw();
    assert!(!page.read_with(&f.cx, |page, _| page.loading));
    f.reveal("message.sequence");
    f.click("message.sequence");
    assert_eq!(page.read_with(&f.cx, |page, _| page.sequence_step), 1);
    for expected in [2, 3, 0] {
        f.cx.executor().advance_clock(Duration::from_millis(2500));
        f.draw();
        assert_eq!(
            page.read_with(&f.cx, |page, _| page.sequence_step),
            expected
        );
    }
}

#[test]
fn message_gallery_status_customization_tokens_and_window_api_remain_interactive() {
    let mut f = Fixture::new();
    open_message(&mut f);
    let page = f
        .gallery
        .read_with(&f.cx, |gallery, _| gallery.message.clone());
    let state = page.read_with(&f.cx, |page, _| page.messages.clone());
    for trigger in [
        "message.success",
        "message.error",
        "message.warning",
        "message.semantic.object",
        "message.semantic.function",
    ] {
        f.reveal(trigger);
        f.click(trigger);
        assert_eq!(f.cx.update(|_, cx| Message::new(&state).len(cx)), 1);
        f.cx.update(|window, cx| Message::new(&state).destroy_all(window, cx));
        f.draw();
    }
    f.reveal("message.tokens");
    f.click("message.tokens");
    let tokens = page.read_with(&f.cx, |page, _| page.token_messages.clone());
    assert_eq!(f.cx.update(|_, cx| Message::new(&tokens).len(cx)), 1);
    f.reveal("message.window");
    f.click("message.window");
    assert_eq!(
        f.cx.update(|window, cx| Message::for_window(window, cx).unwrap().len(cx)),
        1
    );
}
