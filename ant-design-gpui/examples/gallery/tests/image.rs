// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
use super::*;
use ant_design_gpui::image::{PictureStatus, PreviewTransform};
use gpui::Focusable;

fn open_image(f: &mut Fixture) {
    f.reveal_sidebar("gallery.navigation.image");
    f.click("gallery.navigation.image");
}
#[test]
fn image_gallery_keeps_public_reference_order_and_native_capture_selection() {
    let demos = super::super::image::DEMOS;
    assert_eq!(
        demos.iter().map(|d| d.id).collect::<Vec<_>>(),
        [
            "image-basic",
            "image-placeholder",
            "image-fallback",
            "image-preview-group",
            "image-preview-group-visible",
            "image-previewSrc",
            "image-controlled-preview",
            "image-toolbarRender",
            "image-imageRender",
            "image-mask",
            "image-style-class",
            "image-nested",
        ]
    );
    assert_eq!(
        Options::parse(["--component".into(), "image".into()])
            .unwrap()
            .component,
        ComponentId::Image
    );
    assert_eq!(
        COMPONENTS
            .iter()
            .find(|c| c.id == ComponentId::Image)
            .unwrap()
            .group,
        "Data Display"
    );
}
#[test]
fn image_gallery_navigation_anchors_and_responsive_cards() {
    let mut f = Fixture::new();
    open_image(&mut f);
    assert_eq!(
        f.gallery.read_with(&f.cx, |g, _| g.selected),
        ComponentId::Image
    );
    for width in [1280., 1100., 640.] {
        f.resize(width);
        let first = f.bounds("gallery.card.image-basic");
        let second = f.bounds("gallery.card.image-placeholder");
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
    f.reveal_sidebar("gallery.jump.image-controlled-preview");
    f.click("gallery.jump.image-controlled-preview");
    f.next_frame();
    assert!(
        (f.bounds("gallery.card.image-controlled-preview").top()
            - f.bounds("gallery.content").top())
        .abs()
            <= px(1.)
    );
}
#[test]
fn image_gallery_controlled_preview_and_display_changes_preserve_state() {
    let mut f = Fixture::new();
    open_image(&mut f);
    f.reveal_sidebar("gallery.jump.image-controlled-preview");
    f.click("gallery.jump.image-controlled-preview");
    f.next_frame();
    let page = f.gallery.read_with(&f.cx, |g, _| g.image.clone());
    let group = page.read_with(&f.cx, |p, _| p.groups[3].clone());
    let zoom = page.read_with(&f.cx, |page, _| page.zoom_step.clone());
    f.cx.update(|window, cx| zoom.update(cx, |state, cx| state.focus(window, cx)));
    f.key("up");
    assert!((page.read_with(&f.cx, |page, _| page.scale_step) - 0.6).abs() < 0.001);
    assert!((group.read_with(&f.cx, |state, _| state.options().scale_step) - 0.6).abs() < 0.001);
    f.click("image.controlled.open");
    assert!(group.read_with(&f.cx, |s, _| s.is_open()));
    f.key("right");
    assert_eq!(group.read_with(&f.cx, |s, _| s.current()), 1);
    f.key("escape");
    assert!(!group.read_with(&f.cx, |s, _| s.is_open()));
    for id in [
        "gallery.control.theme",
        "gallery.control.density",
        "gallery.control.direction",
    ] {
        f.click(id);
    }
    assert_eq!(group.read_with(&f.cx, |s, _| s.current()), 1);
    f.reveal("gallery.card.image-style-class");
    let styled = page.read_with(&f.cx, |page, _| page.images[23].clone());
    let styled_preview = styled.read_with(&f.cx, |state, _| state.preview());
    let focus = styled.read_with(&f.cx, |state, cx| state.focus_handle(cx));
    f.cx.update(|window, cx| focus.focus(window, cx));
    f.key("enter");
    assert_eq!(
        styled_preview.read_with(&f.cx, |state, _| state.options().controlled_open),
        Some(false)
    );
    assert!(!styled_preview.read_with(&f.cx, |state, _| state.is_open()));
}

#[test]
fn image_gallery_generation_finishes_as_image_and_reload_obeys_placeholder_delay() {
    let mut f = Fixture::new();
    open_image(&mut f);
    f.reveal("gallery.card.image-placeholder");
    let page = f
        .gallery
        .read_with(&f.cx, |gallery, _| gallery.image.clone());
    let generated = page.read_with(&f.cx, |page, _| page.images[7].clone());
    f.reveal("image.generate");
    f.click("image.generate");
    assert!(page.read_with(&f.cx, |page, _| page.generating));
    for step in 1..=20 {
        f.cx.executor().advance_clock(Duration::from_millis(200));
        f.draw();
        assert_eq!(
            page.read_with(&f.cx, |page, _| page.percent),
            (step * 5) as f32
        );
    }
    f.draw();
    assert!(!page.read_with(&f.cx, |page, _| page.generating));
    assert_eq!(
        generated.read_with(&f.cx, |state, cx| state.status(cx)),
        PictureStatus::Ready
    );
    assert!(generated.read_with(&f.cx, |state, _| state.source().is_some()));
    let reload = page.read_with(&f.cx, |page, _| page.images[6].clone());
    f.reveal("image.reload");
    f.click("image.reload");
    assert!(f.cx.debug_bounds("image.reload.placeholder").is_some());
    assert!(reload.read_with(&f.cx, |state, _| state.source().is_none()));
    f.cx.executor().advance_clock(Duration::from_millis(599));
    f.draw();
    assert!(reload.read_with(&f.cx, |state, _| state.source().is_none()));
    f.cx.executor().advance_clock(Duration::from_millis(1));
    f.draw();
    f.draw();
    assert_eq!(
        reload.read_with(&f.cx, |state, cx| state.status(cx)),
        PictureStatus::Ready
    );
    assert!(f.cx.debug_bounds("image.reload.placeholder").is_none());
}

#[test]
fn image_gallery_custom_toolbar_preserves_reference_order_and_native_reset_actions() {
    let mut f = Fixture::new();
    open_image(&mut f);
    f.reveal("image.toolbar.thumbnail");
    let page = f
        .gallery
        .read_with(&f.cx, |gallery, _| gallery.image.clone());
    let group = page.read_with(&f.cx, |page, _| page.groups[2].clone());
    f.click("image.toolbar.thumbnail");
    assert!(group.read_with(&f.cx, |state, _| state.is_open()));
    let order = [
        "image.custom.previous",
        "image.custom.next",
        "image.custom.download",
        "image.custom.flip-y",
        "image.custom.flip-x",
        "image.custom.rotate-left",
        "image.custom.rotate-right",
        "image.custom.zoom-out",
        "image.custom.zoom-in",
        "image.custom.reset",
    ];
    let bounds = order.map(|id| f.bounds(id));
    assert!(
        bounds
            .windows(2)
            .all(|pair| pair[0].right() < pair[1].left())
    );
    f.click("image.custom.previous");
    assert_eq!(group.read_with(&f.cx, |state, _| state.current()), 0);
    f.click("image.custom.zoom-in");
    f.click("image.custom.rotate-right");
    f.click("image.custom.flip-y");
    let transformed = group.read_with(&f.cx, |state, _| state.transform());
    assert_eq!(transformed.scale, 1.5);
    assert_eq!(transformed.rotate, 90.);
    assert!(transformed.flip_y);
    f.click("image.custom.reset");
    assert_eq!(
        group.read_with(&f.cx, |state, _| state.transform()),
        PreviewTransform::default()
    );
    f.click("image.custom.next");
    assert_eq!(group.read_with(&f.cx, |state, _| state.current()), 1);
    f.key("escape");
    assert!(!group.read_with(&f.cx, |state, _| state.is_open()));
}

#[test]
fn image_gallery_nested_dialog_preview_closes_in_order_and_restores_trigger_focus() {
    let mut f = Fixture::new();
    open_image(&mut f);
    f.reveal("image.nested.open");
    let page = f
        .gallery
        .read_with(&f.cx, |gallery, _| gallery.image.clone());
    let modals = page.read_with(&f.cx, |page, _| page.modals.clone());
    let image = page.read_with(&f.cx, |page, _| page.images[25].clone());
    let preview = image.read_with(&f.cx, |image, _| image.preview());
    f.click("image.nested.open");
    f.click("image.nested.next.0");
    let modal_two_focus = page.read_with(&f.cx, |page, _| page.nested_focus[2].clone());
    f.click("image.nested.next.1");
    assert!(
        modals
            .iter()
            .all(|modal| modal.read_with(&f.cx, |state, _| state.is_open()))
    );
    f.click("image.nested.thumbnail");
    assert!(preview.read_with(&f.cx, |state, _| state.is_open()));
    f.key("escape");
    assert!(!preview.read_with(&f.cx, |state, _| state.is_open()));
    assert!(
        modals
            .iter()
            .all(|modal| modal.read_with(&f.cx, |state, _| state.is_open()))
    );
    let focus = image.read_with(&f.cx, |state, cx| state.focus_handle(cx));
    assert!(f.cx.update(|window, _| focus.is_focused(window)));
    f.key("escape");
    assert!(!modals[2].read_with(&f.cx, |state, _| state.is_open()));
    assert!(f.cx.update(|window, _| modal_two_focus.is_focused(window)));
    for expected in [1, 0] {
        f.key("escape");
        assert!(!modals[expected].read_with(&f.cx, |state, _| state.is_open()));
    }
}

#[test]
fn image_gallery_nested_previews_fit_windows_main_thread_stack() {
    // Native Windows executables reserve 1 MiB. The test harness's larger
    // default stack concealed the modal close button's child-scope creation
    // overflowing during the first ordinary parent dialog's prepaint.
    std::thread::Builder::new()
        .name("image-gallery-native-stack".into())
        .stack_size(1024 * 1024)
        .spawn(image_gallery_nested_dialog_preview_closes_in_order_and_restores_trigger_focus)
        .expect("spawn Image gallery with the Windows main thread stack size")
        .join()
        .expect("open nested Image dialogs and previews on a 1 MiB stack");
}
