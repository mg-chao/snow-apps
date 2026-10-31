// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::super::{select, slider};
use super::*;
use ant_design_gpui::{SelectEvent, SelectState, input::TextSelection};

fn select_page(f: &Fixture) -> Entity<select::SelectPage> {
    f.gallery
        .read_with(&f.cx, |gallery, _| gallery.select.clone())
}
fn slider_page(f: &Fixture) -> Entity<slider::SliderPage> {
    f.gallery
        .read_with(&f.cx, |gallery, _| gallery.slider.clone())
}
fn select_state(f: &Fixture, key: &str) -> Entity<SelectState<String>> {
    select_page(f).read_with(&f.cx, |page, _| page.states[key].clone())
}

#[test]
fn select_slider_gallery_catalogue_matches_public_reference_demos() {
    assert_eq!(
        select::DEMOS.iter().map(|d| d.id).collect::<Vec<_>>(),
        [
            "basic",
            "search",
            "search-filter-option",
            "search-multi-field",
            "multiple",
            "size",
            "option-render",
            "search-sort",
            "tags",
            "optgroup",
            "coordinate",
            "label-in-value",
            "automatic-tokenization",
            "custom-tokenization",
            "select-users",
            "suffix",
            "custom-dropdown-menu",
            "hide-selected",
            "variant",
            "custom-tag-render",
            "custom-label-render",
            "responsive",
            "big-data",
            "status",
            "placement",
            "maxCount",
            "style-class",
        ]
    );
    assert_eq!(
        slider::DEMOS.iter().map(|d| d.id).collect::<Vec<_>>(),
        [
            "basic",
            "input-number",
            "icon-slider",
            "tip-formatter",
            "event",
            "mark",
            "vertical",
            "show-tooltip",
            "reverse",
            "draggableTrack",
            "multiple",
            "editable",
            "disabled-handle",
            "style-class",
        ]
    );
    for (name, id) in [
        ("select", ComponentId::Select),
        ("slider", ComponentId::Slider),
    ] {
        assert_eq!(
            Options::parse(["--component".into(), name.into()])
                .unwrap()
                .component,
            id
        );
        assert!(COMPONENTS.iter().any(|component| component.id == id));
    }
    assert_eq!(
        select::quoted_tokens("\"San Francisco, CA\", New York\nTokyo"),
        ["San Francisco, CA", "New York", "Tokyo"]
    );
}

#[test]
fn select_slider_gallery_cards_fit_and_navigation_anchors_work() {
    let mut f = Fixture::new();
    for (page, id, demos) in [
        ("select", ComponentId::Select, select::DEMOS),
        ("slider", ComponentId::Slider, slider::DEMOS),
    ] {
        f.gallery.update(&mut f.cx, |gallery, cx| {
            gallery.selected = id;
            gallery.scroll.set_offset(Default::default());
            cx.notify();
        });
        for rtl in [false, true] {
            let scope = f.scope();
            f.cx.update(|_, cx| {
                scope
                    .update_patch(cx, |patch| {
                        patch.direction = Some(if rtl { Direction::Rtl } else { Direction::Ltr })
                    })
                    .unwrap()
            });
            for width in [1280., 1100., 640.] {
                f.resize(width);
                let content = f.bounds("gallery.content");
                for demo in demos {
                    let selector =
                        Box::leak(format!("gallery.{page}.card.{}", demo.id).into_boxed_str());
                    let card = f.bounds(selector);
                    assert!(
                        card.left() >= content.left() && card.right() <= content.right() + px(1.),
                        "{page}/{} width {width} rtl {rtl}: {card:?} / {content:?}",
                        demo.id
                    );
                }
            }
        }
        f.resize(1280.);
        let jump = if page == "select" {
            "gallery.jump.coordinate"
        } else {
            "gallery.jump.mark"
        };
        f.reveal_sidebar(jump);
        f.click(jump);
        f.next_frame();
        let selector = Box::leak(
            format!(
                "gallery.{page}.card.{}",
                if page == "select" {
                    "coordinate"
                } else {
                    "mark"
                }
            )
            .into_boxed_str(),
        );
        assert!((f.bounds(selector).top() - f.bounds("gallery.content").top()).abs() <= px(1.));
    }
}

#[test]
fn select_slider_gallery_coordinates_values_and_keeps_large_options_retained() {
    let mut f = Fixture::new();
    f.gallery.update(&mut f.cx, |gallery, cx| {
        gallery.selected = ComponentId::Select;
        cx.notify();
    });
    f.draw();
    let province = select_state(&f, "coordinate");
    let city = select_state(&f, "coordinate.city");
    f.cx.update(|window, cx| province.update(cx, |state, cx| state.focus(window, cx)));
    f.draw();
    f.key("down");
    f.key("down");
    f.key("enter");
    assert_eq!(
        city.read_with(&f.cx, |s, _| s.values()[0].value.clone()),
        "Nanjing"
    );
    let large = select_state(&f, "big-data");
    let original = large.read_with(&f.cx, |s, _| s.options().clone());
    assert_eq!(original.option_count(), 100_000);
    let scope = f.scope();
    f.cx.update(|_, cx| {
        scope
            .update_patch(cx, |patch| patch.direction = Some(Direction::Rtl))
            .unwrap()
    });
    f.draw();
    assert!(large.read_with(&f.cx, |s, _| s.options().same_collection(&original)));
}

#[test]
fn select_slider_gallery_numeric_editors_synchronize_both_directions() {
    let mut f = Fixture::new();
    f.gallery.update(&mut f.cx, |gallery, cx| {
        gallery.selected = ComponentId::Slider;
        cx.notify();
    });
    f.draw();
    let page = slider_page(&f);
    for (key, initial, next) in [
        ("input-number", 1., 2.),
        ("input-number.fraction", 0., 0.01),
    ] {
        let (slider, number) = page.read_with(&f.cx, |page, _| {
            (page.states[key].clone(), page.numbers[key].clone())
        });
        assert_eq!(
            slider.read_with(&f.cx, |s, _| s.value().as_slice()[0]),
            initial
        );
        f.cx.update(|window, cx| slider.update(cx, |state, cx| state.focus(0, window, cx)));
        f.draw();
        f.key("right");
        assert_eq!(
            number.read_with(&f.cx, |s, _| s.value().unwrap().to_number().unwrap()),
            next
        );
        f.cx.update(|window, cx| {
            number.update(cx, |state, cx| {
                state.focus(window, cx);
                state.select(TextSelection::collapsed(0), window, cx);
                state.select_all(window, cx);
            })
        });
        f.draw();
        f.cx.simulate_input(if key == "input-number" { "13" } else { "0.75" });
        f.key("enter");
        assert_eq!(
            slider.read_with(&f.cx, |s, _| s.value().as_slice()[0]),
            if key == "input-number" { 13. } else { 0.75 }
        );
    }
}

#[test]
fn select_slider_gallery_remote_search_cancels_stale_delivery() {
    let mut f = Fixture::new();
    let state = select_state(&f, "select-users");
    // Emit the same boundary event as native typing. The demo owns asynchronous I/O,
    // while the component's IME and native text event path has separate tests.
    state.update(&mut f.cx, |_, cx| {
        cx.emit(SelectEvent::Search("Jack".into()))
    });
    f.cx.run_until_parked();
    state.update(&mut f.cx, |_, cx| {
        cx.emit(SelectEvent::Search("Lucy".into()))
    });
    f.cx.run_until_parked();
    f.cx.executor().advance_clock(Duration::from_millis(301));
    f.cx.run_until_parked();
    state.read_with(&f.cx, |state, _| {
        assert_eq!(state.options().option_count(), 1);
        assert!(state.options().option(&"Lucy Green".to_owned()).is_some());
    });
}
