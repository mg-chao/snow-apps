// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use ant_design_gpui::{InputNumberState, InputState, input::TextSelection};
use gpui::{EntityInputHandler, Focusable};

use super::super::{
    input,
    input_examples::{self, Coverage},
    input_number,
};
use super::*;

impl Fixture {
    fn example_bounds(&mut self, selector: String) -> Option<Bounds<Pixels>> {
        // The GPUI test API requires static selectors; these live only for this test process.
        self.cx.debug_bounds(Box::leak(selector.into_boxed_str()))
    }

    fn input_state(&self, key: &str) -> Entity<InputState> {
        self.gallery.read_with(&self.cx, |gallery, cx| {
            gallery.input.read(cx).texts[key].clone()
        })
    }
    fn number_state(&self, key: &str) -> Entity<InputNumberState> {
        self.gallery.read_with(&self.cx, |gallery, cx| {
            gallery.input_number.read(cx).numbers[key].clone()
        })
    }
    fn edit_input(&mut self, key: &str, value: &str) {
        let state = self.input_state(key);
        self.cx.update(|window, cx| {
            state.update(cx, |state, cx| {
                state.focus(window, cx);
                state.select_all(window, cx);
                state.replace_text_in_range(None, value, window, cx);
            })
        });
        self.draw();
    }
    fn edit_number(&mut self, key: &str, value: &str) {
        let state = self.number_state(key);
        self.cx.update(|window, cx| {
            state.update(cx, |state, cx| {
                state.focus(window, cx);
                state.select_all(window, cx);
            })
        });
        self.draw();
        self.cx.simulate_input(value);
        self.draw();
    }
    fn number_value(&self, key: &str) -> String {
        self.number_state(key)
            .read_with(&self.cx, |state, _| state.value().unwrap().to_string())
    }
}

#[test]
fn input_gallery_reference_catalogue_and_coverage_are_explicit() {
    assert_eq!(
        input::DEMOS.iter().map(|d| d.id).collect::<Vec<_>>(),
        [
            "basic",
            "size",
            "variant",
            "compact-style",
            "search-input",
            "search-input-loading",
            "textarea",
            "autosize-textarea",
            "otp",
            "tooltip",
            "presuffix",
            "password-input",
            "allowClear",
            "show-count",
            "advance-count",
            "status",
            "focus",
            "style-class",
        ]
    );
    assert_eq!(
        input_number::DEMOS.iter().map(|d| d.id).collect::<Vec<_>>(),
        [
            "basic",
            "size",
            "disabled",
            "digit",
            "formatter",
            "keyboard",
            "change-on-wheel",
            "variant",
            "spinner",
            "out-of-range",
            "presuffix",
            "status",
            "focus",
            "style-class",
        ]
    );
    assert_eq!(
        input_examples::summary(input::DEMOS),
        "11 complete · 6 partial · 1 planned"
    );
    assert_eq!(
        input_examples::summary(input_number::DEMOS),
        "11 complete · 2 partial · 1 planned"
    );
    for demos in [input::DEMOS, input_number::DEMOS] {
        for demo in demos {
            if let Coverage::Partial(note) | Coverage::Planned(note) = demo.coverage {
                assert!(!note.is_empty());
            }
        }
    }
    let position = COMPONENTS
        .iter()
        .position(|c| c.id == ComponentId::Input)
        .unwrap();
    assert_eq!(COMPONENTS[position + 1].id, ComponentId::InputNumber);
    for (slug, component) in [
        ("input", ComponentId::Input),
        ("input-number", ComponentId::InputNumber),
    ] {
        assert_eq!(
            Options::parse(["--component".into(), slug.into()])
                .unwrap()
                .component,
            component
        );
    }
}

#[test]
fn input_gallery_pages_layout_pending_cards_and_jump_anchors_follow_window_width() {
    for (navigation, page, demos, jump, target) in [
        (
            "gallery.navigation.input",
            "input",
            input::DEMOS,
            "gallery.jump.search-input",
            "gallery.input.card.search-input",
        ),
        (
            "gallery.navigation.input-number",
            "input-number",
            input_number::DEMOS,
            "gallery.jump.change-on-wheel",
            "gallery.input-number.card.change-on-wheel",
        ),
    ] {
        let mut f = Fixture::new();
        f.click(navigation);
        for width in [1280., 1100., 640.] {
            f.resize(width);
            let content = f.bounds("gallery.content");
            let first = f
                .example_bounds(format!("gallery.{page}.card.basic"))
                .unwrap();
            let second = f
                .example_bounds(format!("gallery.{page}.card.size"))
                .unwrap();
            if width == 1280. {
                assert_eq!(first.top(), second.top());
            } else {
                assert!(second.top() >= first.bottom());
            }
            for demo in demos {
                let card = f
                    .example_bounds(format!("gallery.{page}.card.{}", demo.id))
                    .unwrap();
                assert!(
                    card.left() >= content.left() && card.right() <= content.right(),
                    "{page}/{} at {width}",
                    demo.id
                );
                let pending = f.example_bounds(format!("gallery.{page}.pending.{}", demo.id));
                assert_eq!(pending.is_some(), demo.coverage != Coverage::Complete);
            }
        }
        for setting in [
            "gallery.control.theme",
            "gallery.control.density",
            "gallery.control.direction",
        ] {
            f.click(setting);
        }
        for width in [1280., 1100., 640.] {
            f.resize(width);
            let content = f.bounds("gallery.content");
            for demo in demos {
                let card = f
                    .example_bounds(format!("gallery.{page}.card.{}", demo.id))
                    .unwrap();
                assert!(
                    card.left() >= content.left() && card.right() <= content.right(),
                    "dark compact RTL: {page}/{}",
                    demo.id
                );
            }
        }
        f.resize(1280.);
        let sidebar = f.bounds("gallery.sidebar");
        f.scroll(sidebar.center(), -600.);
        f.click(jump);
        f.next_frame();
        assert!((f.bounds(target).top() - f.bounds("gallery.content").top()).abs() <= px(1.));
    }
}

#[test]
fn input_gallery_retains_independent_values_and_events_across_display_changes() {
    let mut f = Fixture::new();
    f.click("gallery.navigation.input");
    f.edit_input("basic", "中文 👩🏽‍💻");
    f.key("enter");
    let page = f
        .gallery
        .read_with(&f.cx, |gallery, _| gallery.input.clone());
    assert_eq!(
        page.read_with(&f.cx, |page, _| (page.changes, page.commits)),
        (1, 1)
    );
    let basic = f.input_state("basic");
    let original = page.read_with(&f.cx, |page, _| page.texts.clone());
    f.edit_input("size.large", "large");
    assert_eq!(
        f.input_state("size.medium")
            .read_with(&f.cx, |s, _| s.value().to_owned()),
        ""
    );
    for control in [
        "gallery.control.theme",
        "gallery.control.density",
        "gallery.control.direction",
        "gallery.control.motion",
    ] {
        f.click(control);
    }
    f.click("gallery.navigation.input-number");
    f.click("gallery.navigation.input");
    assert_eq!(
        basic.read_with(&f.cx, |s, _| s.value().to_owned()),
        "中文 👩🏽‍💻"
    );
    assert_eq!(
        original,
        page.read_with(&f.cx, |page, _| page.texts.clone())
    );
    assert_eq!(
        f.input_state("size.large")
            .read_with(&f.cx, |s, _| s.value().to_owned()),
        "large"
    );
}

#[test]
fn input_gallery_search_loading_clear_password_and_compact_actions_work() {
    let mut f = Fixture::new();
    f.click("gallery.navigation.input");
    let page = f
        .gallery
        .read_with(&f.cx, |gallery, _| gallery.input.clone());
    f.reveal("gallery.input.preview.search-input");
    f.edit_input("search.clear", "query");
    f.key("enter");
    assert_eq!(page.read_with(&f.cx, |p, _| p.searches), 1);
    f.key("tab");
    f.key("enter");
    assert_eq!(page.read_with(&f.cx, |p, _| p.searches), 2);
    assert_eq!(
        page.read_with(&f.cx, |p, _| p.last_search.clone()),
        "Action: query"
    );
    f.reveal("gallery.input.preview.search-input-loading");
    f.edit_input("loading.label", "ignored");
    f.key("enter");
    assert_eq!(page.read_with(&f.cx, |p, _| p.searches), 2);
    f.reveal("gallery.input.preview.allowClear");
    f.edit_input("clear.input", "remove me");
    let bounds = f.bounds("input.clear.input");
    f.cx.simulate_click(
        point(bounds.right() - px(16.), bounds.center().y),
        Modifiers::none(),
    );
    f.draw();
    assert_eq!(
        f.input_state("clear.input")
            .read_with(&f.cx, |s, _| s.value().to_owned()),
        ""
    );
    assert!(page.read_with(&f.cx, |p, _| p.feedback["clear"].contains("0 characters")));
    f.reveal("input.password.toggle");
    f.edit_input("password.controlled", "secret");
    f.click("input.password.toggle");
    assert!(
        f.input_state("password.controlled")
            .read_with(&f.cx, |s, _| s.password_visible())
    );
    assert!(
        !f.input_state("password.default")
            .read_with(&f.cx, |s, _| s.password_visible())
    );
    f.click("input.password.toggle");
    assert!(
        !f.input_state("password.controlled")
            .read_with(&f.cx, |s, _| s.password_visible())
    );
    f.reveal("gallery.input.preview.compact-style");
    f.edit_input("compact.submit", "New value");
    f.key("tab");
    f.key("enter");
    assert_eq!(
        page.read_with(&f.cx, |p, _| p.submitted.clone()).as_deref(),
        Some("Submitted: New value")
    );
}

#[test]
fn input_gallery_counts_otp_options_and_autosize_match_the_reference() {
    let mut f = Fixture::new();
    f.click("gallery.navigation.input");
    assert_eq!(
        f.input_state("advance.exceed")
            .read_with(&f.cx, |s, _| (s.character_count(), s.max_length())),
        (12, None)
    );
    assert_eq!(
        f.input_state("advance.emoji")
            .read_with(&f.cx, |s, _| s.character_count()),
        3
    );
    f.edit_input("count.input", &"🔥".repeat(11));
    assert_eq!(
        f.input_state("count.input").read_with(&f.cx, |s, _| (
            s.character_count(),
            s.value().chars().count()
        )),
        (20, 10)
    );
    f.edit_input("advance.limit", &"🔥".repeat(7));
    assert_eq!(
        f.input_state("advance.limit")
            .read_with(&f.cx, |s, _| s.character_count()),
        6
    );
    f.edit_input("textarea.limit", "1234567");
    assert_eq!(
        f.input_state("textarea.limit")
            .read_with(&f.cx, |s, _| s.value().to_owned()),
        "123456"
    );
    let page = f
        .gallery
        .read_with(&f.cx, |gallery, _| gallery.input.clone());
    let codes = page.read_with(&f.cx, |p, _| p.codes.clone());
    assert_eq!(
        codes["length"].read_with(&f.cx, |s, _| s.options().length),
        8
    );
    assert_eq!(
        codes["mask"]
            .read_with(&f.cx, |s, _| s.options().mask.clone())
            .as_deref(),
        Some("🔒")
    );
    f.reveal("gallery.input.preview.otp");
    f.cx.update(|window, cx| {
        codes["uppercase"].update(cx, |s, cx| s.paste("ab12cd", window, cx));
        codes["disabled"].update(cx, |s, cx| s.paste("123456", window, cx));
    });
    f.draw();
    assert_eq!(
        codes["uppercase"].read_with(&f.cx, |s, _| s.value()),
        "AB12CD"
    );
    assert_eq!(codes["disabled"].read_with(&f.cx, |s, _| s.value()), "");
    assert!(page.read_with(&f.cx, |p, _| p.otp_feedback.ends_with("code complete")));
    for index in 0..5 {
        assert!(
            f.example_bounds(format!("input.otp.separator.{index}"))
                .is_some()
        );
        assert!(
            f.example_bounds(format!("input.otp.colors.{index}"))
                .is_some()
        );
    }
    assert!(f.cx.debug_bounds("input.otp.separator.5").is_none());
    f.reveal("gallery.input.preview.autosize-textarea");
    let initial = f.bounds("input.autosize.free").size.height;
    let lines = "line\n".repeat(12);
    f.edit_input("autosize.free", &lines);
    f.edit_input("autosize.bounded", &lines);
    f.edit_input("autosize.controlled", &lines);
    assert!(f.bounds("input.autosize.free").size.height > initial * 6.);
    assert!(
        f.bounds("input.autosize.bounded").size.height
            < f.bounds("input.autosize.free").size.height
    );
    assert!(
        f.bounds("input.autosize.controlled").size.height
            < f.bounds("input.autosize.bounded").size.height
    );
}

#[test]
fn input_gallery_focus_options_switch_editor_and_preserve_scroll() {
    let mut f = Fixture::new();
    f.click("gallery.navigation.input");
    f.reveal("gallery.input.preview.focus");
    let state = f.input_state("focus");
    for (button, expected) in [
        ("input.focus.start", TextSelection::collapsed(0)),
        ("input.focus.end", TextSelection::collapsed(20)),
        (
            "input.focus.all",
            TextSelection {
                anchor: 0,
                caret: 20,
            },
        ),
    ] {
        f.click(button);
        assert_eq!(state.read_with(&f.cx, |s, _| s.selection()), expected);
        f.cx.update(|window, cx| assert!(state.read(cx).focus_handle(cx).is_focused(window)));
    }
    let scroll = f.gallery.read_with(&f.cx, |g, _| g.scroll.clone());
    let offset = scroll.offset();
    f.click("input.focus.prevent-scroll");
    assert_eq!(scroll.offset(), offset);
    f.click("input.focus.toggle");
    assert!(state.read_with(&f.cx, |s, _| s.is_multiline()));
    assert_eq!(
        state.read_with(&f.cx, |s, _| s.value().to_owned()),
        "Ant Design love you!"
    );
    f.click("input.focus.toggle");
    assert!(!state.read_with(&f.cx, |s, _| s.is_multiline()));
}

#[test]
fn input_number_gallery_exact_values_bounds_keyboard_and_disabled_toggles_work() {
    let mut f = Fixture::new();
    f.click("gallery.navigation.input-number");
    f.edit_number("basic", "10");
    f.key("up");
    assert_eq!(f.number_value("basic"), "10");
    f.key("down");
    assert_eq!(f.number_value("basic"), "9");
    f.reveal("gallery.input-number.preview.digit");
    assert_eq!(
        f.number_state("digit")
            .read_with(&f.cx, |state, cx| state.text(cx).to_owned()),
        "1"
    );
    f.edit_number("digit", "1");
    f.key("up");
    assert_eq!(f.number_value("digit"), "1.00000000000001");
    f.reveal("gallery.input-number.preview.disabled");
    assert!(
        f.number_state("disabled")
            .read_with(&f.cx, |s, cx| s.is_disabled(cx))
    );
    f.click("input-number.disabled.toggle");
    f.edit_number("disabled", "5");
    assert_eq!(f.number_value("disabled"), "5");
    f.click("input-number.disabled.toggle");
    assert!(
        f.number_state("disabled")
            .read_with(&f.cx, |s, cx| s.is_disabled(cx))
    );
    f.reveal("gallery.input-number.preview.keyboard");
    f.click_checkbox("input-number.keyboard.toggle");
    f.edit_number("keyboard", "3");
    f.key("up");
    assert_eq!(f.number_value("keyboard"), "3");
    f.click_checkbox("input-number.keyboard.toggle");
    f.edit_number("keyboard", "3");
    f.key("up");
    assert_eq!(f.number_value("keyboard"), "4");
    assert_eq!(f.number_value("size.large"), "3");
}

#[test]
fn input_number_gallery_formatters_wheel_and_external_reset_work() {
    let mut f = Fixture::new();
    f.click("gallery.navigation.input-number");
    assert_eq!(input_number::currency("-1234567.89"), "$ -1,234,567.89");
    f.reveal("gallery.input-number.preview.formatter");
    for (key, text, value, formatted) in [
        (
            "formatter.currency",
            "$ 12,345.67",
            "12345.67",
            "$ 12,345.67",
        ),
        ("formatter.percent", "75%", "75", "75%"),
    ] {
        f.edit_number(key, text);
        f.key("enter");
        assert_eq!(f.number_value(key), value);
        assert_eq!(
            f.number_state(key)
                .read_with(&f.cx, |s, cx| s.text(cx).to_owned()),
            formatted
        );
    }
    f.reveal("gallery.input-number.preview.change-on-wheel");
    let bounds = f.bounds("input-number.wheel");
    f.scroll(point(bounds.left() + px(20.), bounds.center().y), 16.);
    assert_eq!(f.number_value("wheel"), "3");
    f.edit_number("wheel", "3");
    f.reveal("gallery.input-number.preview.change-on-wheel");
    let bounds = f.bounds("input-number.wheel");
    f.scroll(point(bounds.left() + px(20.), bounds.center().y), 16.);
    assert_eq!(f.number_value("wheel"), "4");
    let page = f.gallery.read_with(&f.cx, |g, _| g.input_number.clone());
    assert!(page.read_with(&f.cx, |p, _| p.feedback["wheel"].contains("Wheel")));
    f.reveal("gallery.input-number.preview.out-of-range");
    assert_eq!(f.number_value("out-of-range"), "99");
    f.edit_number("out-of-range", "5");
    f.key("enter");
    let changes = page.read_with(&f.cx, |p, _| p.changes);
    f.click("input-number.range.reset");
    assert_eq!(f.number_value("out-of-range"), "99");
    assert_eq!(page.read_with(&f.cx, |p, _| p.changes), changes);
}

#[test]
fn input_number_gallery_focus_and_state_survive_display_changes() {
    let mut f = Fixture::new();
    f.click("gallery.navigation.input-number");
    f.reveal("gallery.input-number.preview.focus");
    let state = f.number_state("focus");
    for (button, expected) in [
        ("input-number.focus.start", TextSelection::collapsed(0)),
        ("input-number.focus.end", TextSelection::collapsed(3)),
        (
            "input-number.focus.all",
            TextSelection {
                anchor: 0,
                caret: 3,
            },
        ),
    ] {
        f.click(button);
        assert_eq!(state.read_with(&f.cx, |s, cx| s.selection(cx)), expected);
        f.cx.update(|window, cx| assert!(state.read(cx).focus_handle(cx).is_focused(window)));
    }
    let scroll = f.gallery.read_with(&f.cx, |g, _| g.scroll.clone());
    let offset = scroll.offset();
    f.click("input-number.focus.prevent-scroll");
    assert_eq!(scroll.offset(), offset);
    f.edit_number("focus", "1234");
    for control in [
        "gallery.control.theme",
        "gallery.control.density",
        "gallery.control.direction",
        "gallery.control.motion",
    ] {
        f.click(control);
    }
    f.click("gallery.navigation.input");
    f.click("gallery.navigation.input-number");
    assert_eq!(f.number_value("focus"), "1234");
    assert_eq!(state, f.number_state("focus"));
    f.resize(640.);
    let content = f.bounds("gallery.content");
    for demo in input_number::DEMOS {
        let bounds = f
            .example_bounds(format!("gallery.input-number.card.{}", demo.id))
            .unwrap();
        assert!(bounds.left() >= content.left() && bounds.right() <= content.right());
    }
}
