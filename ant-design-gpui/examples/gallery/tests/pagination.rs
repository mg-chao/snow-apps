// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::super::pagination;
use super::*;
use ant_design_gpui::PaginationState;

fn pagination_page(f: &Fixture) -> Entity<pagination::PaginationPage> {
    f.gallery
        .read_with(&f.cx, |gallery, _| gallery.pagination.clone())
}

fn pagination_state(f: &Fixture, id: &str) -> Entity<PaginationState> {
    pagination_page(f).read_with(&f.cx, |page, _| page.states[id].clone())
}

fn show_pagination(f: &mut Fixture) {
    f.gallery.update(&mut f.cx, |gallery, cx| {
        gallery.selected = ComponentId::Pagination;
        gallery.scroll.set_offset(Default::default());
        cx.notify();
    });
    f.draw();
}

#[test]
fn pagination_gallery_catalogue_matches_all_public_reference_demos() {
    assert_eq!(
        pagination::DEMOS
            .iter()
            .map(|demo| demo.id)
            .collect::<Vec<_>>(),
        [
            "basic",
            "align",
            "more",
            "changer",
            "jump",
            "mini",
            "simple",
            "controlled",
            "total",
            "all",
            "itemRender",
            "components",
            "style-class"
        ]
    );
    assert!(
        pagination::DEMOS
            .iter()
            .all(|demo| demo.coverage == super::super::input_examples::Coverage::Complete)
    );
    assert_eq!(
        Options::parse(["--component".into(), "pagination".into()])
            .unwrap()
            .component,
        ComponentId::Pagination
    );
    let component = COMPONENTS
        .iter()
        .find(|component| component.id == ComponentId::Pagination)
        .unwrap();
    assert_eq!(component.title, "Pagination");
    assert_eq!(component.group, "Navigation");
    assert!(
        Options::parse(["--component".into(), "pagination".into()])
            .unwrap()
            .interactive()
    );
    assert!(
        !Options::parse([
            "--component".into(),
            "pagination".into(),
            "--capture".into(),
            "pagination.png".into(),
        ])
        .unwrap()
        .interactive()
    );
}

#[test]
fn pagination_gallery_initial_values_and_independent_controllers_match_reference() {
    let mut f = Fixture::new();
    show_pagination(&mut f);
    assert_eq!(
        pagination_page(&f).read_with(&f.cx, |page, _| page.states.len()),
        28
    );
    for (id, current, total, size) in [
        ("basic", 1, 50, 10),
        ("align.start", 1, 50, 10),
        ("align.center", 1, 50, 10),
        ("align.end", 1, 50, 10),
        ("more", 6, 500, 10),
        ("changer", 3, 500, 10),
        ("changer.disabled", 3, 500, 10),
        ("jump", 2, 500, 10),
        ("jump.disabled", 2, 500, 10),
        ("mini.small", 1, 50, 10),
        ("mini.small.options", 1, 50, 10),
        ("mini.small.total", 1, 50, 10),
        ("mini.small.disabled", 1, 50, 10),
        ("mini.large", 1, 50, 10),
        ("mini.large.options", 1, 50, 10),
        ("mini.large.total", 1, 50, 10),
        ("mini.large.disabled", 1, 50, 10),
        ("simple", 2, 50, 10),
        ("simple.read-only", 2, 50, 10),
        ("simple.disabled", 2, 50, 10),
        ("controlled", 3, 50, 10),
        ("total", 1, 85, 20),
        ("total.range", 1, 85, 20),
        ("all", 1, 85, 10),
        ("itemRender", 1, 500, 10),
        ("components", 3, 500, 10),
        ("style-class", 1, 500, 10),
        ("style-class.small", 1, 500, 10),
    ] {
        let state = pagination_state(&f, id);
        assert_eq!(
            state.read_with(&f.cx, |s, _| (s.current(), s.total(), s.page_size())),
            (current, total, size),
            "{id}"
        );
    }
    let basic = pagination_state(&f, "basic");
    f.cx.update(|window, cx| basic.update(cx, |state, cx| state.focus_page(2, window, cx)));
    f.key("enter");
    assert_eq!(basic.read_with(&f.cx, |s, _| s.current()), 2);
    assert_eq!(
        pagination_state(&f, "more").read_with(&f.cx, |s, _| s.current()),
        6
    );
    assert_eq!(
        pagination_state(&f, "changer.disabled").read_with(&f.cx, |s, _| s.current()),
        3
    );

    f.reveal("gallery.pagination.card.controlled");
    let controlled = pagination_state(&f, "controlled");
    f.cx.update(|window, cx| controlled.update(cx, |state, cx| state.focus_page(4, window, cx)));
    f.key("enter");
    assert_eq!(
        pagination_page(&f).read_with(&f.cx, |page, _| page.controlled_current),
        4
    );
    assert_eq!(controlled.read_with(&f.cx, |s, _| s.current()), 4);
}

#[test]
fn pagination_gallery_cards_and_anchors_fit_responsive_direction_changes() {
    let mut f = Fixture::new();
    show_pagination(&mut f);
    for direction in [Direction::Ltr, Direction::Rtl] {
        let scope = f.scope();
        f.cx.update(|_, cx| {
            scope
                .update_patch(cx, |patch| patch.direction = Some(direction))
                .unwrap()
        });
        for width in [1280., 1100., 640.] {
            f.resize(width);
            let content = f.bounds("gallery.content");
            for demo in pagination::DEMOS {
                let selector =
                    Box::leak(format!("gallery.pagination.card.{}", demo.id).into_boxed_str());
                let card = f.bounds(selector);
                assert!(
                    card.left() >= content.left() && card.right() <= content.right() + px(1.),
                    "{} at {width}/{direction:?}: {card:?} within {content:?}",
                    demo.id
                );
                if width == 1280. && !matches!(demo.id, "basic" | "align" | "simple" | "controlled")
                {
                    let basic = f.bounds("gallery.pagination.card.basic");
                    assert!(
                        card.size.width > basic.size.width * 1.9,
                        "{} needs the full grid width for changers and jumpers: {card:?}",
                        demo.id
                    );
                }
            }
        }
    }
    f.resize(1280.);
    f.reveal_sidebar("gallery.jump.more");
    f.click("gallery.jump.more");
    f.next_frame();
    assert!(
        (f.bounds("gallery.pagination.card.more").top() - f.bounds("gallery.content").top()).abs()
            <= px(1.)
    );
    f.reveal_sidebar("gallery.jump.components");
    f.click("gallery.jump.components");
    f.next_frame();
    let card = f.bounds("gallery.pagination.card.components");
    let content = f.bounds("gallery.content");
    // The final rows clamp to the maximum scroll offset instead of leaving a blank viewport.
    assert!(card.top() >= content.top() && card.bottom() <= content.bottom() + px(1.));
}

#[test]
fn pagination_gallery_custom_number_requests_size_changes_and_retains_page_state() {
    let mut f = Fixture::new();
    show_pagination(&mut f);
    f.reveal("gallery.pagination.card.components");
    let page = pagination_page(&f);
    let number = page.read_with(&f.cx, |page, _| page.number.clone());
    f.cx.update(|window, cx| number.update(cx, |state, cx| state.focus(window, cx)));
    f.draw();
    f.key("up");
    let components = pagination_state(&f, "components");
    assert_eq!(
        components.read_with(&f.cx, |s, _| (s.current(), s.page_size())),
        (3, 11)
    );
    f.gallery.update(&mut f.cx, |gallery, cx| {
        gallery.selected = ComponentId::Button;
        cx.notify();
    });
    f.draw();
    show_pagination(&mut f);
    assert_eq!(
        components.read_with(&f.cx, |s, _| (s.current(), s.page_size())),
        (3, 11)
    );
}
