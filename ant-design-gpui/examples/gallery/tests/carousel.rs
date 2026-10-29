// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;

fn open_carousel(f: &mut Fixture) {
    f.gallery.update(&mut f.cx, |gallery, cx| {
        gallery.selected = ComponentId::Carousel;
        cx.notify();
    });
    f.draw();
    f.next_frame();
}

#[test]
fn carousel_gallery_preserves_reference_order_and_supports_native_capture() {
    assert_eq!(
        crate::carousel::DEMOS
            .iter()
            .map(|demo| (demo.id, demo.title))
            .collect::<Vec<_>>(),
        [
            ("carousel-basic", "Basic"),
            ("carousel-placement", "Position"),
            ("carousel-autoplay", "Scroll automatically"),
            ("carousel-fade", "Fade in"),
            ("carousel-arrows", "Arrows for switching"),
            ("carousel-dot-duration", "Progress of dots"),
            ("carousel-component-token", "Component Token"),
        ],
    );
    let options = Options::parse([
        "--component".into(),
        "carousel".into(),
        "--dark".into(),
        "--compact".into(),
        "--rtl".into(),
        "--reduced-motion".into(),
        "--headless".into(),
        "--capture".into(),
        "C:/temp/carousel-gallery.png".into(),
    ])
    .unwrap();
    assert_eq!(options.component, ComponentId::Carousel);
    assert!(!options.interactive());
    assert!(options.dark && options.compact && options.rtl && options.reduced_motion);
    assert_eq!(
        COMPONENTS
            .iter()
            .find(|component| component.id == ComponentId::Carousel)
            .unwrap()
            .group,
        "Data Display"
    );
}

#[test]
fn carousel_gallery_navigation_and_responsive_cards_follow_the_viewport() {
    let mut f = Fixture::new();
    f.reveal_sidebar("gallery.navigation.carousel");
    f.click("gallery.navigation.carousel");
    assert_eq!(
        f.gallery.read_with(&f.cx, |gallery, _| gallery.selected),
        ComponentId::Carousel
    );
    for width in [1280., 1100., 640.] {
        f.resize(width);
        let first = f.bounds("gallery.card.carousel-basic");
        let second = f.bounds("gallery.card.carousel-placement");
        let content = f.bounds("gallery.content");
        assert!(first.left() >= content.left());
        assert!(second.right() <= content.right());
        if width == 1280. {
            assert_eq!(first.top(), second.top());
        } else {
            assert!(second.top() >= first.bottom());
        }
        assert_eq!(f.bounds("carousel.basic").size.height, px(160.));
    }
    f.resize(1280.);
    f.reveal_sidebar("gallery.jump.carousel-arrows");
    f.click("gallery.jump.carousel-arrows");
    f.next_frame();
    assert!(
        (f.bounds("gallery.card.carousel-arrows").top() - f.bounds("gallery.content").top()).abs()
            <= px(1.)
    );
    assert_eq!(f.bounds("carousel.arrows.horizontal").size.height, px(160.));
    assert_eq!(f.bounds("carousel.arrows.vertical").size.height, px(160.));
    f.click("gallery.control.top");
    f.next_frame();
    assert_eq!(
        f.gallery
            .read_with(&f.cx, |gallery, _| gallery.scroll.offset().y),
        px(0.)
    );
}

#[test]
fn carousel_gallery_dots_emit_after_change_and_placement_preserves_retained_slides() {
    let mut f = Fixture::new();
    open_carousel(&mut f);
    let page = f
        .gallery
        .read_with(&f.cx, |gallery, _| gallery.carousel.clone());
    let states = page.read_with(&f.cx, |page, _| page.states.clone());
    let basic = f.bounds("carousel.basic");
    f.cx.simulate_click(
        point(basic.center().x + px(40.), basic.bottom() - px(13.5)),
        Modifiers::none(),
    );
    f.draw();
    f.next_frame();
    assert_eq!(
        states[0].read_with(&f.cx, |state, _| state.current_index()),
        Some(3)
    );
    assert_eq!(page.read_with(&f.cx, |page, _| page.last_change), Some(3));

    f.reveal("gallery.preview.carousel-placement");
    let placement_focus = page.read_with(&f.cx, |page, _| page.placement_focus[0].clone());
    f.cx.update(|window, cx| window.focus(&placement_focus, cx));
    f.draw();
    f.key("right");
    f.key("right");
    assert_eq!(page.read_with(&f.cx, |page, _| page.placement), 2);
    assert_eq!(f.bounds("carousel.placement").size.height, px(160.));
    f.cx.update(|window, cx| {
        states[1].update(cx, |state, cx| state.go_to(2, true, window, cx));
    });
    f.draw();
    for control in [
        "gallery.control.theme",
        "gallery.control.density",
        "gallery.control.direction",
    ] {
        f.click(control);
        assert_eq!(page.read_with(&f.cx, |page, _| page.placement), 2);
        assert_eq!(
            states[0].read_with(&f.cx, |state, _| state.current_index()),
            Some(3)
        );
        assert_eq!(
            states[1].read_with(&f.cx, |state, _| state.current_index()),
            Some(2)
        );
    }
}

#[test]
fn carousel_gallery_component_tokens_are_local_and_follow_parent_presentation() {
    use ant_design_gpui::carousel::CarouselTheme;

    let mut f = Fixture::new();
    open_carousel(&mut f);
    let page = f
        .gallery
        .read_with(&f.cx, |gallery, _| gallery.carousel.clone());
    for dark in [false, true] {
        if dark {
            f.click("gallery.control.theme");
            f.click("gallery.control.direction");
        }
        page.read_with(&f.cx, |page, cx| {
            let parent = page.scope.snapshot(cx);
            let custom = page.token_scope.snapshot(cx);
            let ordinary = parent.theme.component::<CarouselTheme>();
            let overridden = custom.theme.component::<CarouselTheme>();
            assert_eq!(
                (
                    ordinary.dot_width,
                    ordinary.dot_height,
                    ordinary.dot_active_width
                ),
                (16., 3., 24.)
            );
            assert_eq!(
                (
                    overridden.dot_width,
                    overridden.dot_height,
                    overridden.dot_active_width
                ),
                (50., 50., 80.)
            );
            assert_eq!(custom.direction, parent.direction);
            assert_eq!(
                custom.theme.tokens.color_bg_container,
                parent.theme.tokens.color_bg_container
            );
        });
    }
}
