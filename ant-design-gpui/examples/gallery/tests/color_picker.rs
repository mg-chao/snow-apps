// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::super::color_picker;
use super::*;

#[test]
fn color_picker_gallery_catalogue_preserves_reference_examples() {
    assert_eq!(
        color_picker::DEMOS
            .iter()
            .map(|demo| demo.id)
            .collect::<Vec<_>>(),
        [
            "base",
            "size",
            "controlled",
            "line-gradient",
            "text-render",
            "disabled",
            "disabled-alpha",
            "allowClear",
            "trigger",
            "trigger-event",
            "format",
            "presets",
            "panel-render",
            "style-class",
            "presets-line-gradient",
            "pure-panel",
        ]
    );
    let options = Options::parse([
        "--component".into(),
        "color-picker".into(),
        "--dark".into(),
        "--compact".into(),
        "--rtl".into(),
        "--reduced-motion".into(),
        "--headless".into(),
        "--capture".into(),
        "C:/temp/color-picker.png".into(),
    ])
    .unwrap();
    assert_eq!(options.component, ComponentId::ColorPicker);
    assert!(!options.interactive());
    assert!(options.dark && options.compact && options.rtl && options.reduced_motion);
    assert_eq!(
        COMPONENTS
            .iter()
            .find(|entry| entry.id == ComponentId::ColorPicker)
            .unwrap()
            .group,
        "Data Entry"
    );
}

#[test]
fn color_picker_gallery_triggers_keep_content_width_in_column_previews() {
    let mut f = Fixture::new();
    f.gallery.update(&mut f.cx, |gallery, cx| {
        gallery.selected = ComponentId::ColorPicker;
        cx.notify();
    });
    f.draw();
    let page = f
        .gallery
        .read_with(&f.cx, |gallery, _| gallery.color_picker.clone());
    let states = page.read_with(&f.cx, |page, _| page.states.clone());
    for width in [1280., 1100., 640.] {
        f.resize(width);
        for (key, height) in [
            ("base", 32.),
            ("size.small", 24.),
            ("size", 32.),
            ("size.large", 40.),
        ] {
            let bounds = states[key].read_with(&f.cx, |state, _| state.geometry().trigger.unwrap());
            assert_eq!(
                bounds.size,
                size(px(height), px(height)),
                "{key} at {width}"
            );
        }
        let text = states["size.medium.text"]
            .read_with(&f.cx, |state, _| state.geometry().trigger.unwrap());
        for (key, swatch_delta) in [("size.small.text", -8.), ("size.large.text", 8.)] {
            let bounds = states[key].read_with(&f.cx, |state, _| state.geometry().trigger.unwrap());
            assert!(bounds.size.width > px(32.), "{key} includes its label");
            if swatch_delta < 0. {
                assert_eq!(bounds.size.width, text.size.width + px(swatch_delta));
            } else {
                assert!(bounds.size.width > text.size.width + px(swatch_delta));
            }
        }
    }
}

#[test]
fn color_picker_gallery_resizes_and_keeps_retained_values_when_display_changes() {
    let mut f = Fixture::new();
    f.reveal_sidebar("gallery.navigation.color-picker");
    f.click("gallery.navigation.color-picker");
    assert_eq!(
        f.gallery.read_with(&f.cx, |gallery, _| gallery.selected),
        ComponentId::ColorPicker
    );
    let page = f
        .gallery
        .read_with(&f.cx, |gallery, _| gallery.color_picker.clone());
    let states = page.read_with(&f.cx, |page, _| page.states.clone());
    let value = ant_design_gpui::color_picker::PickerColor::parse("#722ed1")
        .unwrap()
        .into();
    f.cx.update(|window, cx| {
        states["base"].update(cx, |s, cx| s.set_value(value, window, cx).unwrap())
    });
    for width in [1280., 1100., 640.] {
        f.resize(width);
        let content = f.bounds("gallery.content");
        for demo in color_picker::DEMOS {
            let selector =
                Box::leak(format!("gallery.color-picker.card.{}", demo.id).into_boxed_str());
            let card = f.bounds(selector);
            assert!(
                card.left() >= content.left() && card.right() <= content.right() + px(1.),
                "{} at {width}: {card:?} / {content:?}",
                demo.id
            );
        }
    }
    f.resize(1280.);
    for selector in [
        "gallery.control.theme",
        "gallery.control.density",
        "gallery.control.direction",
    ] {
        f.click(selector);
        assert_eq!(
            states["base"].read_with(&f.cx, |s, _| s.value().active_color(0).to_hex_string()),
            "#722ed1"
        );
    }
    f.reveal_sidebar("gallery.jump.presets");
    f.click("gallery.jump.presets");
    f.next_frame();
    assert!(
        (f.bounds("gallery.color-picker.card.presets").top() - f.bounds("gallery.content").top())
            .abs()
            <= px(1.)
    );
}

#[test]
fn color_picker_gallery_opens_palette_and_escape_restores_the_trigger() {
    let mut f = Fixture::new();
    f.gallery.update(&mut f.cx, |gallery, cx| {
        gallery.selected = ComponentId::ColorPicker;
        cx.notify();
    });
    f.draw();
    let page = f
        .gallery
        .read_with(&f.cx, |gallery, _| gallery.color_picker.clone());
    let state = page.read_with(&f.cx, |page, _| page.states["base"].clone());
    let trigger = state.read_with(&f.cx, |state, _| state.geometry().trigger.unwrap());
    f.cx.simulate_click(trigger.center(), Modifiers::none());
    f.draw();
    f.next_frame();
    assert!(state.read_with(&f.cx, |state, _| state.is_open()));
    let bounds = state.read_with(&f.cx, |state, _| state.geometry().palette.unwrap());
    f.cx.simulate_click(bounds.center(), Modifiers::none());
    f.draw();
    f.key("escape");
    assert!(!state.read_with(&f.cx, |state, _| state.is_open()));
    assert_ne!(
        state.read_with(&f.cx, |state, _| state
            .value()
            .active_color(0)
            .to_hex_string()),
        "#1677ff"
    );
}
