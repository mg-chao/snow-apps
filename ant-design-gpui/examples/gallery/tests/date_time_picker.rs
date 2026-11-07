// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::super::date_time_picker::{DATE_DEMOS, TIME_DEMOS};
use super::*;
use ant_design_gpui::date_picker::{NaiveDate, NaiveTime};

#[test]
fn date_time_picker_gallery_catalogues_cover_the_native_reference_family() {
    assert_eq!(DATE_DEMOS.len(), 27);
    assert_eq!(TIME_DEMOS.len(), 18);
    for (argument, component, demos) in [
        ("date-picker", ComponentId::DatePicker, DATE_DEMOS),
        ("time-picker", ComponentId::TimePicker, TIME_DEMOS),
    ] {
        let options = Options::parse([
            "--component".into(),
            argument.into(),
            "--dark".into(),
            "--compact".into(),
            "--rtl".into(),
            "--reduced-motion".into(),
            "--headless".into(),
            "--capture".into(),
            format!("C:/temp/{argument}.png"),
        ])
        .unwrap();
        assert_eq!(options.component, component);
        assert!(!options.interactive());
        assert!(options.dark && options.compact && options.rtl && options.reduced_motion);
        assert_eq!(
            COMPONENTS
                .iter()
                .find(|entry| entry.id == component)
                .unwrap()
                .group,
            "Data Entry"
        );
        assert!(
            demos
                .iter()
                .all(|demo| demo.coverage == super::super::input_examples::Coverage::Complete)
        );
        let unique: std::collections::BTreeSet<_> = demos.iter().map(|demo| demo.id).collect();
        assert_eq!(unique.len(), demos.len());
    }
    assert_eq!(DATE_DEMOS[0].id, "date-basic");
    assert_eq!(TIME_DEMOS[0].id, "time-basic");
    for expected in [
        "date-range",
        "date-multiple",
        "date-time",
        "date-mask",
        "date-locale",
    ] {
        assert!(DATE_DEMOS.iter().any(|demo| demo.id == expected));
    }
    for expected in [
        "time-12hours",
        "time-scroll",
        "time-range",
        "time-millisecond",
    ] {
        assert!(TIME_DEMOS.iter().any(|demo| demo.id == expected));
    }
}

#[test]
fn date_time_picker_gallery_pages_follow_the_viewport_and_keep_typed_values() {
    let mut fixture = Fixture::new();
    fixture.gallery.update(&mut fixture.cx, |gallery, cx| {
        gallery.selected = ComponentId::DatePicker;
        cx.notify();
    });
    fixture.draw();
    let date_page = fixture
        .gallery
        .read_with(&fixture.cx, |gallery, _| gallery.date_picker.clone());
    let state = date_page.read_with(&fixture.cx, |page, _| page.dates["date-basic"].clone());
    let selected = NaiveDate::from_ymd_opt(2026, 10, 15).unwrap();
    fixture.cx.update(|window, cx| {
        state.update(cx, |state, cx| state.set_value(Some(selected), window, cx))
    });
    fixture.draw();
    for width in [1280., 1100., 640.] {
        fixture.resize(width);
        let first = fixture.bounds("gallery.date-picker.card.date-basic");
        let second = fixture.bounds("gallery.date-picker.card.date-range");
        let content = fixture.bounds("gallery.content");
        assert!(first.left() >= content.left());
        assert!(second.right() <= content.right());
        if width == 1280. {
            assert_eq!(first.top(), second.top());
        } else {
            assert!(second.top() >= first.bottom());
        }
        assert_eq!(
            state.read_with(&fixture.cx, |state, _| *state.value()),
            Some(selected)
        );
    }
    fixture.gallery.update(&mut fixture.cx, |gallery, cx| {
        gallery.selected = ComponentId::TimePicker;
        cx.notify();
    });
    fixture.resize(1280.);
    let time_page = fixture
        .gallery
        .read_with(&fixture.cx, |gallery, _| gallery.time_picker.clone());
    assert!(time_page.read_with(&fixture.cx, |page, _| page.time_page));
    let time_state = time_page.read_with(&fixture.cx, |page, _| page.times["time-basic"].clone());
    assert_eq!(
        time_state.read_with(&fixture.cx, |state, _| *state.value()),
        Some(NaiveTime::from_hms_opt(13, 30, 56).unwrap())
    );
    fixture.gallery.update(&mut fixture.cx, |gallery, cx| {
        gallery.selected = ComponentId::DatePicker;
        cx.notify();
    });
    fixture.draw();
    assert_eq!(
        state.read_with(&fixture.cx, |state, _| *state.value()),
        Some(selected)
    );
}

#[test]
fn date_time_picker_gallery_calendar_commits_native_selection_and_survives_theme_changes() {
    let mut fixture = Fixture::new();
    fixture.gallery.update(&mut fixture.cx, |gallery, cx| {
        gallery.selected = ComponentId::DatePicker;
        cx.notify();
    });
    fixture.draw();
    let page = fixture
        .gallery
        .read_with(&fixture.cx, |gallery, _| gallery.date_picker.clone());
    let state = page.read_with(&fixture.cx, |page, _| page.dates["date-basic"].clone());
    let initial = NaiveDate::from_ymd_opt(2026, 10, 15).unwrap();
    let chosen = NaiveDate::from_ymd_opt(2026, 10, 16).unwrap();
    fixture.cx.update(|window, cx| {
        state.update(cx, |state, cx| {
            state.set_value(Some(initial), window, cx);
            state.set_open(true, window, cx);
        })
    });
    fixture.draw();
    let cell = state.read_with(&fixture.cx, |state, _| {
        state
            .geometry()
            .cells
            .values()
            .find(|cell| cell.date == Some(chosen) && !cell.disabled)
            .expect("rendered chosen day")
            .bounds
    });
    fixture.cx.simulate_click(cell.center(), Modifiers::none());
    fixture.draw();
    assert_eq!(
        state.read_with(&fixture.cx, |state, _| *state.value()),
        Some(chosen)
    );
    let scope = fixture.scope();
    fixture.cx.update(|_, cx| {
        scope
            .update_patch(cx, |patch| {
                patch.direction = Some(Direction::Rtl);
                patch.theme.get_or_insert_default().algorithms =
                    Some(ant_design_gpui::theme::AlgorithmChain::dark_compact());
            })
            .unwrap()
    });
    fixture.draw();
    assert_eq!(
        state.read_with(&fixture.cx, |state, _| *state.value()),
        Some(chosen)
    );
}
