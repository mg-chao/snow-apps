// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Independent values from actual upstream Message cards, styles and CSS motion.

use std::sync::OnceLock;

use gpui::{Edges, Hsla};
use serde_json::Value;

use super::{MessageStyle, MessageTheme, MessageTokenPatch, MessageType};
use crate::{
    behavior::evaluate_cubic_bezier,
    theme::{AlgorithmChain, Color, ComponentThemeConfig, ThemeConfig, TokenPatch},
};

fn reference() -> &'static Value {
    static REFERENCE: OnceLock<Value> = OnceLock::new();
    REFERENCE.get_or_init(|| {
        serde_json::from_str(include_str!("../../fixtures/message/reference.json"))
            .expect("actual upstream Message browser measurements")
    })
}

fn theme_config(name: &str, custom: bool) -> ThemeConfig {
    let mut config = ThemeConfig {
        algorithms: Some(match name {
            "default" => AlgorithmChain::default(),
            "dark" => AlgorithmChain::dark(),
            "compact" => AlgorithmChain::compact(),
            "dark-compact" => AlgorithmChain::dark_compact(),
            unknown => panic!("unknown reference theme {unknown}"),
        }),
        ..Default::default()
    };
    if custom {
        // Developer inputs shared by the two renderers, rather than native output.
        config.components.set_theme(
            ComponentThemeConfig::<MessageTheme>::new(MessageTokenPatch {
                content_bg: Some(Color::rgb(0xf6ffed)),
                content_padding: Some(Edges {
                    top: 14.,
                    right: 20.,
                    bottom: 14.,
                    left: 20.,
                }),
                ..Default::default()
            })
            .with_tokens(TokenPatch {
                border_radius_lg: Some(12.),
                font_size: Some(16.),
                line_height: Some(1.5),
                font_size_lg: Some(20.),
                ..Default::default()
            }),
        );
    }
    config
}

fn near(native: f64, browser: f64, tolerance: f64, context: &str) {
    assert!(
        (native - browser).abs() <= tolerance,
        "{context}: native={native}, browser={browser}"
    );
}

fn number(value: &Value) -> f64 {
    value.as_f64().expect("numeric upstream measurement")
}

fn color(native: Hsla, browser: &str, context: &str) {
    let channels: Vec<f64> = browser
        .trim_start_matches("rgba(")
        .trim_start_matches("rgb(")
        .trim_end_matches(')')
        .split(',')
        .map(|channel| channel.trim().parse().expect("CSS color channel"))
        .collect();
    assert!(matches!(channels.len(), 3 | 4), "{context}: {browser}");
    let native: gpui::Rgba = native.into();
    for (native, browser) in [native.r, native.g, native.b].into_iter().zip(&channels) {
        near(f64::from(native), browser / 255., 0.0001, context);
    }
    near(
        f64::from(native.a),
        channels.get(3).copied().unwrap_or(1.),
        0.0021,
        context,
    );
}

fn shadows(value: &str) -> Vec<(&str, [f64; 4])> {
    let mut parts = Vec::new();
    let mut depth = 0;
    let mut begin = 0;
    for (index, ch) in value.char_indices() {
        match ch {
            '(' => depth += 1,
            ')' => depth -= 1,
            ',' if depth == 0 => {
                parts.push(value[begin..index].trim());
                begin = index + 1;
            }
            _ => {}
        }
    }
    parts.push(value[begin..].trim());
    parts
        .into_iter()
        .map(|part| {
            let end = part.find(')').expect("CSS shadow color") + 1;
            let values: Vec<f64> = part[end..]
                .split_whitespace()
                .map(|value| {
                    value
                        .trim_end_matches("px")
                        .parse()
                        .expect("CSS shadow length")
                })
                .collect();
            assert_eq!(values.len(), 4);
            (&part[..end], [values[0], values[1], values[2], values[3]])
        })
        .collect()
}

#[test]
fn cards_match_actual_upstream_tokens_typography_colors_and_shadows() {
    let reference = reference();
    let cases = reference["cases"].as_array().unwrap();
    assert_eq!(cases.len(), 80);
    let mut compared = 0;
    for case in cases {
        let scene = case["scene"].as_str().unwrap();
        // The custom scene deliberately applies native semantic refinements, covered
        // by the paired GPU captures instead of comparing base token output here.
        if scene == "custom" {
            continue;
        }
        let name = case["theme"].as_str().unwrap();
        let theme = theme_config(name, scene == "tokens").resolve();
        let tokens = theme.component::<MessageTheme>();
        for row in case["rows"].as_array().unwrap() {
            let context = format!(
                "{}/{}",
                case["name"].as_str().unwrap(),
                row["key"].as_str().unwrap()
            );
            let kind = match row["options"]["type"].as_str() {
                Some("info") => Some(MessageType::Info),
                Some("success") => Some(MessageType::Success),
                Some("error") => Some(MessageType::Error),
                Some("warning") => Some(MessageType::Warning),
                Some("loading") => Some(MessageType::Loading),
                None => None,
                unknown => panic!("unknown reference Message type {unknown:?}"),
            };
            let style = MessageStyle::from_tokens(&tokens, kind);
            let root = &row["root"];
            near(style.font_size, number(&root["fontSize"]), 0.0001, &context);
            near(
                style.font_size * style.line_height,
                number(&root["lineHeight"]),
                0.0001,
                &context,
            );
            near(style.radius, number(&root["radius"]), 0.0001, &context);
            for (native, field) in [
                (style.padding.top, "paddingTop"),
                (style.padding.right, "paddingRight"),
                (style.padding.bottom, "paddingBottom"),
                (style.padding.left, "paddingLeft"),
            ] {
                near(native, number(&root[field]), 0.0001, &context);
            }
            color(
                style.background,
                root["background"].as_str().unwrap(),
                &context,
            );
            color(style.color, root["color"].as_str().unwrap(), &context);
            if row["title"].is_object() {
                color(
                    style.title_color,
                    row["title"]["color"].as_str().unwrap(),
                    &context,
                );
                near(
                    style.font_size,
                    number(&row["title"]["fontSize"]),
                    0.0001,
                    &context,
                );
                near(
                    style.font_size * style.line_height,
                    number(&row["title"]["lineHeight"]),
                    0.0001,
                    &context,
                );
            }
            if row["options"]["icon"] == "none" {
                assert_eq!(row["options"]["type"], "info");
                near(0., number(&row["icon"]["width"]), 0., &context);
                near(0., number(&row["icon"]["height"]), 0., &context);
            }
            if row["icon"].is_object() {
                color(
                    style.icon_color,
                    row["icon"]["color"].as_str().unwrap(),
                    &context,
                );
                near(
                    style.icon_size,
                    number(&row["icon"]["fontSize"]),
                    0.0001,
                    &context,
                );
                near(
                    style.icon_gap,
                    number(&row["wrapper"]["gap"]),
                    0.0001,
                    &context,
                );
            }
            if scene == "narrow" && row["key"] == "unbroken" {
                near(
                    320. - style.edge_margin * 2.,
                    number(&root["width"]),
                    0.0001,
                    &context,
                );
                near(
                    style.padding.top + style.font_size * style.line_height + style.padding.bottom,
                    number(&root["height"]),
                    0.0001,
                    &context,
                );
                assert!(number(&row["title"]["width"]) > number(&root["width"]));
            }
            let browser_shadows = shadows(root["shadow"].as_str().unwrap());
            assert_eq!(tokens.shadows().len(), browser_shadows.len(), "{context}");
            for (native, (browser_color, dimensions)) in
                tokens.shadows().iter().zip(browser_shadows)
            {
                color(native.color, browser_color, &context);
                // GPUI consumes Gaussian sigma; CSS box-shadow reports twice sigma.
                for (native, browser) in [
                    f32::from(native.offset.x),
                    f32::from(native.offset.y),
                    f32::from(native.blur_radius) * 2.,
                    f32::from(native.spread_radius),
                ]
                .into_iter()
                .zip(dimensions)
                {
                    near(f64::from(native), browser, 0.0001, &context);
                }
                assert!(!native.inset, "{context}");
            }
            compared += 1;
        }
    }
    assert_eq!(compared, 256);
    for (name, values) in reference["themes"].as_object().unwrap() {
        let theme = theme_config(name, false).resolve();
        let tokens = theme.component::<MessageTheme>();
        near(
            tokens.z_index_popup as f64,
            number(&values["defaults"]["zIndexPopup"]),
            0.,
            name,
        );
        color(
            tokens.content_bg.into(),
            values["defaults"]["contentBg"].as_str().unwrap(),
            name,
        );
    }
}

#[test]
fn enter_and_exit_match_original_css_transition_samples() {
    let reference = reference();
    let theme = theme_config("default", false).resolve();
    let style = MessageStyle::resolve(&theme, Some(MessageType::Info));
    for phase in ["enter", "leave"] {
        let motion = &reference["motion"][phase];
        let properties = motion["properties"].as_array().unwrap();
        assert_eq!(properties.len(), 2);
        for property in properties {
            near(
                style.motion_duration.as_secs_f64() * 1000.,
                number(&property["duration"]),
                0.,
                phase,
            );
            assert_eq!(property["easing"], "cubic-bezier(0.645, 0.045, 0.355, 1)");
        }
        for sample in motion["samples"].as_array().unwrap() {
            let progress = number(&sample["time"]) / (style.motion_duration.as_secs_f64() * 1000.);
            let eased = evaluate_cubic_bezier(style.motion_curve, progress);
            let opacity = if phase == "enter" { eased } else { 1. - eased };
            near(opacity, number(&sample["opacity"]), 0.00001, phase);
            near(
                8. - (1. - opacity) * style.motion_offset,
                number(&sample["y"]),
                0.0001,
                phase,
            );
        }
    }
}

#[test]
fn upstream_semantic_holder_slots_and_lifecycle_quirks_are_recorded() {
    let reference = reference();
    let slots = reference["behavior"]["semanticSlots"].as_array().unwrap();
    assert_eq!(slots.len(), 2);
    assert_eq!(slots[0]["listPadding"], 24);
    assert_eq!(slots[0]["listContentGap"], 16);
    assert_eq!(slots[1]["listPadding"], 31);
    assert_eq!(slots[1]["listContentGap"], 37);
    assert_eq!(slots[0]["rootBackground"], "rgb(255, 251, 230)");
    assert_eq!(
        reference["behavior"]["manualCloseResolvesCompletion"],
        false
    );
    let rows = reference["behavior"]["rows"].as_array().unwrap();
    let at = |label: &str| rows.iter().find(|row| row["label"] == label).unwrap();
    assert_eq!(
        at("hovered-four-seconds")["active"]
            .as_array()
            .unwrap()
            .len(),
        1
    );
    assert!(
        at("remaining-at-deadline")["active"]
            .as_array()
            .unwrap()
            .is_empty()
    );
    assert_eq!(at("same-duration-update")["active"][0], "Updated");
    assert!(
        at("same-duration-original-deadline")["active"]
            .as_array()
            .unwrap()
            .is_empty()
    );
}

fn matrix_translation_y(value: &Value) -> f64 {
    value
        .as_str()
        .unwrap()
        .trim_start_matches("matrix(")
        .trim_end_matches(')')
        .split(',')
        .nth(5)
        .unwrap()
        .trim()
        .parse()
        .unwrap()
}

#[test]
fn runtime_reflow_and_placeholder_transitions_match_native_token_curves() {
    let reference = reference();
    let runtime = &reference["runtimeMotion"];
    let theme = theme_config("default", false).resolve();
    let style = MessageStyle::resolve(&theme, Some(MessageType::Info));
    let fast = theme.tokens.motion_duration_fast.as_secs_f64() * 1000.;
    let mid = style.motion_duration.as_secs_f64() * 1000.;
    let slow = theme.tokens.motion_duration_slow.as_secs_f64() * 1000.;
    let eased = |time: f64, duration: f64| {
        evaluate_cubic_bezier(style.motion_curve, (time / duration).clamp(0., 1.))
    };
    let reflow = &runtime["reflow"];
    let properties = reflow["properties"].as_array().unwrap();
    let top = properties
        .iter()
        .find(|property| property["property"] == "top")
        .unwrap();
    near(mid, number(&top["duration"]), 0., "reflow duration");
    let height = properties
        .iter()
        .find(|property| property["property"] == "height")
        .unwrap();
    near(
        slow * 2.,
        number(&height["duration"]),
        0.,
        "height shrink duration",
    );
    near(mid, number(&height["delay"]), 0., "height shrink delay");
    let reflow_samples = reflow["samples"].as_array().unwrap();
    let first = &reflow_samples[0];
    let last = reflow_samples.last().unwrap();
    for sample in reflow_samples {
        let time = number(&sample["time"]);
        let initial = number(&first["card"]["y"]);
        let final_value = number(&last["card"]["y"]);
        near(
            initial + (final_value - initial) * eased(time, mid),
            number(&sample["card"]["y"]),
            0.02,
            "actual runtime reflow",
        );
        let initial = number(&first["content"]["height"]);
        let final_value = number(&last["content"]["height"]);
        near(
            initial + (final_value - initial) * eased((time - mid).max(0.), slow * 2.),
            number(&sample["content"]["height"]),
            0.02,
            "actual runtime height shrink",
        );
    }
    for phase in ["collapse", "expand"] {
        for property in runtime[phase]["properties"].as_array().unwrap() {
            if property["pseudo"].is_string() {
                near(fast, number(&property["duration"]), 0., phase);
            }
        }
        for sample in runtime[phase]["samples"].as_array().unwrap() {
            near(
                if phase == "collapse" { 56. } else { 208. },
                number(&sample["content"]["height"]),
                0.,
                "actual stack list height",
            );
            let progress = eased(number(&sample["time"]), fast);
            let opacity = if phase == "collapse" {
                progress
            } else {
                1. - progress
            };
            for pseudo in ["before", "after"] {
                near(opacity, number(&sample[pseudo]["opacity"]), 0.00001, phase);
                near(
                    style.icon_gap * 2. * (1. - opacity),
                    matrix_translation_y(&sample[pseudo]["transform"]),
                    0.0001,
                    phase,
                );
            }
        }
    }
    for property in runtime["widthChange"]["properties"].as_array().unwrap() {
        if property["pseudo"].is_string() {
            assert_eq!(property["property"], "width");
            near(
                slow,
                number(&property["duration"]),
                0.,
                "placeholder width duration",
            );
        }
    }
    let widths = runtime["widthChange"]["samples"].as_array().unwrap();
    for sample in widths {
        let progress = eased(number(&sample["time"]), slow);
        for pseudo in ["before", "after"] {
            let initial = number(&widths[0][pseudo]["width"]);
            let final_value = number(&widths.last().unwrap()[pseudo]["width"]);
            near(
                initial + (final_value - initial) * progress,
                number(&sample[pseudo]["width"]),
                0.02,
                "actual runtime placeholder width",
            );
        }
    }
    // Native intentionally tracks same-key size changes rather than retaining the
    // upstream ref-only measurement. Keep the observed upstream quirk explicit.
    let keyed = &runtime["sameKeyWidthChange"];
    assert_eq!(keyed["widthTransitions"], 0);
    assert_eq!(
        keyed["before"]["before"]["width"],
        keyed["after"]["before"]["width"]
    );
    assert!(
        number(&keyed["after"]["latest"]["width"]) > number(&keyed["before"]["latest"]["width"])
    );
    let interrupted = &runtime["interrupted"];
    assert_eq!(interrupted["rapidClose"]["peakCommittedNotices"], 0);
    assert_eq!(interrupted["rapidClose"]["remainingNotices"], 0);
    let middle = &interrupted["middleClose"];
    let at_close = number(&middle["beforeClose"]["opacity"]);
    near(
        eased(number(&middle["entryTime"]), mid),
        at_close,
        0.00001,
        "entry pose at interruption",
    );
    let duration = number(&middle["exitDuration"]);
    near(mid * at_close, duration, 0.0001, "CSS reversal shortening");
    let mut previous = at_close;
    for sample in middle["samples"].as_array().unwrap() {
        let opacity = at_close * (1. - eased(number(&sample["time"]), duration));
        near(
            opacity,
            number(&sample["card"]["opacity"]),
            0.00001,
            "interrupted exit opacity",
        );
        near(
            8. - style.motion_offset * (1. - opacity),
            number(&sample["card"]["y"]),
            0.0001,
            "interrupted exit translation",
        );
        assert!(number(&sample["card"]["opacity"]) <= previous + 0.000001);
        previous = number(&sample["card"]["opacity"]);
    }
}

#[test]
fn virtual_clock_native_close_reflow_matches_actual_upstream_frames() {
    use super::{Message, MessageOptions};
    use crate::{ConfigScope, UiRoot};
    use gpui::{
        AppContext, Context, IntoElement, Render, Styled, TestAppContext, Window, WindowHandle,
        div, px, size,
    };
    use std::time::Duration;

    struct Page;
    impl Render for Page {
        fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
            div().size_full()
        }
    }
    fn draw(cx: &mut TestAppContext, window: WindowHandle<UiRoot>) {
        cx.run_until_parked();
        cx.update_window(window.into(), |_, window, cx| window.draw(cx).clear(cx))
            .unwrap();
        cx.run_until_parked();
    }
    fn advance(cx: &mut TestAppContext, window: WindowHandle<UiRoot>, duration: u64) {
        cx.executor().advance_clock(Duration::from_millis(duration));
        cx.run_until_parked();
        cx.update_window(window.into(), |_, window, cx| {
            window.simulate_next_frame(cx);
        })
        .unwrap();
        draw(cx, window);
    }
    let mut cx = TestAppContext::single();
    let mut messages = None;
    let window = cx.open_window(size(px(740.), px(1000.)), |window, cx| {
        let scope = ConfigScope::new_window(cx);
        let page = cx.new(|_| Page);
        let root = UiRoot::new(scope, page, window, cx);
        messages = Some(root.messages());
        root
    });
    let messages: Message = messages.unwrap();
    draw(&mut cx, window);
    cx.update_window(window.into(), |_, window, cx| {
        messages.open(
            MessageOptions::new("Older message")
                .key("reflow-a")
                .duration(Duration::ZERO),
            window,
            cx,
        );
    })
    .unwrap();
    draw(&mut cx, window);
    advance(&mut cx, window, 250);
    let latest = cx
        .update_window(window.into(), |_, window, cx| {
            messages.open(
                MessageOptions::new("Newest message")
                    .key("reflow-b")
                    .duration(Duration::ZERO),
                window,
                cx,
            )
        })
        .unwrap();
    draw(&mut cx, window);
    advance(&mut cx, window, 250);
    cx.update_window(window.into(), |_, window, cx| latest.close(window, cx))
        .unwrap();
    draw(&mut cx, window);
    let mut previous = 0;
    for sample in reference()["runtimeMotion"]["reflow"]["samples"]
        .as_array()
        .unwrap()
    {
        let time = sample["time"].as_u64().unwrap();
        if time > 200 {
            break;
        }
        advance(&mut cx, window, time - previous);
        previous = time;
        let layout = cx
            .update_window(window.into(), |_, _, cx| {
                messages
                    .layouts(cx)
                    .into_iter()
                    .find(|layout| layout.key.as_ref() == "reflow-a")
                    .unwrap()
            })
            .unwrap();
        near(
            f64::from(f32::from(layout.bounds.origin.y)),
            number(&sample["card"]["y"]),
            0.03,
            "native virtual-clock reflow",
        );
    }
}
