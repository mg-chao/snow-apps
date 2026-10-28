// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Hidden native GPU captures of the same cases as the independent upstream renderer.

use std::{path::PathBuf, time::Duration};

use ant_design_gpui::{
    ConfigPatch, ConfigScope, Direction, UiRoot,
    icons::IconName,
    message::{
        Message, MessageConfigPatch, MessageHost, MessageOptions, MessageStack, MessageState,
        MessageStyles, MessageTheme, MessageTokenPatch, MessageType,
    },
    theme::{AlgorithmChain, Color, ComponentThemeConfig, TokenPatch},
};
use gpui::{
    App, AppContext, Bounds, Context, Entity, HighlightStyle, IntoElement, Render, StyleRefinement,
    StyledText, Window, WindowBounds, WindowOptions, div, point, prelude::*, px, size,
};
use serde_json::{Value, json};

#[cfg(target_os = "windows")]
fn set_native_scale(title: &str, scale: f32) {
    #[repr(C)]
    #[derive(Default)]
    struct Rect {
        left: i32,
        top: i32,
        right: i32,
        bottom: i32,
    }
    #[link(name = "user32")]
    unsafe extern "system" {
        fn FindWindowW(class: *const u16, title: *const u16) -> isize;
        fn GetWindowRect(window: isize, rect: *mut Rect) -> i32;
        fn SendMessageW(window: isize, message: u32, wparam: usize, lparam: isize) -> isize;
    }
    let title: Vec<u16> = title.encode_utf16().chain(Some(0)).collect();
    // Only the uniquely titled hidden window belonging to this process is addressed.
    unsafe {
        let handle = FindWindowW(std::ptr::null(), title.as_ptr());
        assert_ne!(handle, 0, "hidden Message capture HWND");
        let mut rect = Rect::default();
        assert_ne!(GetWindowRect(handle, &mut rect), 0);
        let dpi = (96. * scale).round() as usize;
        SendMessageW(
            handle,
            0x02E0,
            dpi | (dpi << 16),
            (&rect as *const Rect) as isize,
        );
    }
}

#[cfg(not(target_os = "windows"))]
fn set_native_scale(_: &str, _: f32) {}

struct Capture {
    scope: ConfigScope,
    messages: Entity<MessageState>,
}

impl Render for Capture {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        MessageHost::new(
            "message-visual-host",
            &self.messages,
            div()
                .size_full()
                .bg(self.scope.snapshot(cx).theme.tokens.color_bg_container),
        )
    }
}

fn kind(value: &Value) -> MessageType {
    match value.as_str().unwrap_or("info") {
        "success" => MessageType::Success,
        "error" => MessageType::Error,
        "warning" => MessageType::Warning,
        "loading" => MessageType::Loading,
        _ => MessageType::Info,
    }
}

fn configure(view: &Entity<Capture>, sample: &Value, window: &mut Window, cx: &mut App) {
    view.update(cx, |view, cx| {
        Message::new(&view.messages).destroy_all(window, cx);
        let scope = if sample["tokenCase"] == true {
            let mut patch = ConfigPatch::default();
            patch.theme.get_or_insert_default().components.set_theme(
                ComponentThemeConfig::<MessageTheme>::new(MessageTokenPatch {
                    content_bg: Some(Color::rgb(0xf6ffed)),
                    content_padding: Some(gpui::Edges {
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
            view.scope
                .child(patch, cx)
                .expect("valid Message reference tokens")
        } else {
            view.scope.clone()
        };
        let state = cx.new(|cx| MessageState::new(scope.clone(), window, cx));
        let api = Message::new(&state);
        api.set_config(
            MessageConfigPatch {
                top: Some(sample["top"].as_f64().unwrap_or(8.) as f32),
                stack: Some(if sample["stack"] == true {
                    MessageStack::Enabled {
                        threshold: sample["threshold"].as_u64().unwrap_or(3) as usize,
                    }
                } else {
                    MessageStack::Disabled
                }),
                max_count: Some(sample["maxCount"].as_u64().map(|value| value as usize)),
                styles: if sample["holderStyles"] == true {
                    MessageStyles {
                        list: StyleRefinement::default().p(px(31.)),
                        list_content: StyleRefinement::default().gap(px(37.)),
                        ..Default::default()
                    }
                } else {
                    MessageStyles::default()
                },
                ..Default::default()
            },
            window,
            cx,
        );
        for item in sample["items"].as_array().unwrap() {
            let text = item["content"].as_str().unwrap().to_owned();
            let mut options = if item["rich"] == true {
                let scope = scope.clone();
                MessageOptions::content(move |_, _, cx| {
                    let config = scope.snapshot(cx);
                    let content = format!("{text} View details");
                    let link = text.len() + 1..content.len();
                    StyledText::new(content).with_highlights([(
                        link,
                        HighlightStyle {
                            color: Some(config.theme.tokens.color_link.to_hsla()),
                            ..Default::default()
                        },
                    )])
                })
            } else {
                MessageOptions::new(text)
            };
            options = options
                .key(item["key"].as_str().unwrap().to_owned())
                .duration(Duration::ZERO);
            if item["type"].is_string() {
                options = options.kind(kind(&item["type"]));
            }
            if item["icon"] == "none" {
                options = options.show_icon(false);
            }
            if item["icon"] == "smile" {
                options = options.icon(IconName::SmileOutlined);
            }
            let mut styles = MessageStyles::default();
            if let Some(style) = item["style"].as_object() {
                if let Some(background) = style.get("background").and_then(Value::as_str) {
                    let color =
                        u32::from_str_radix(background.trim_start_matches('#'), 16).unwrap();
                    styles.root = styles.root.bg(Color::rgb(color));
                }
                if let Some(value) = style.get("paddingVertical").and_then(Value::as_f64) {
                    styles.root = styles.root.py(px(value as f32));
                }
                if let Some(value) = style.get("paddingHorizontal").and_then(Value::as_f64) {
                    styles.root = styles.root.px(px(value as f32));
                }
                if let Some(value) = style.get("radius").and_then(Value::as_f64) {
                    styles.root = styles.root.rounded(px(value as f32));
                }
            }
            if item["semantic"] == true {
                styles.root = StyleRefinement::default().bg(Color::rgb(0xfffbe6));
                styles.wrapper = StyleRefinement::default().gap(px(12.));
                styles.icon = StyleRefinement::default().text_color(Color::rgb(0x722ed1));
                styles.title = StyleRefinement::default()
                    .text_color(Color::rgb(0x0958d9))
                    .text_size(px(12.))
                    .line_height(px(20.));
            }
            options = options.styles(styles);
            api.open(options, window, cx);
        }
        view.messages = state;
        cx.notify();
    });
}

fn rectangle(bounds: Bounds<gpui::Pixels>) -> Value {
    json!({"x":f32::from(bounds.origin.x),"y":f32::from(bounds.origin.y),"width":f32::from(bounds.size.width),"height":f32::from(bounds.size.height)})
}

fn main() {
    let mut args = std::env::args().skip(1);
    assert_eq!(args.next().as_deref(), Some("--output"));
    let output = PathBuf::from(args.next().expect("output directory"));
    let mut selected_scene = None;
    let mut selected_theme = None;
    let mut selected_direction = None;
    let mut selected_scale = None;
    while let Some(option) = args.next() {
        let value = args.next().expect("capture filter value");
        match option.as_str() {
            "--scene" => selected_scene = Some(value),
            "--theme" => selected_theme = Some(value),
            "--direction" => selected_direction = Some(value),
            "--scale" => {
                let scale = value.parse::<f32>().expect("numeric scale");
                assert!([1., 1.5, 2.].contains(&scale));
                selected_scale = Some(scale);
            }
            _ => panic!("unknown capture option {option}"),
        }
    }
    std::fs::create_dir_all(&output).unwrap();
    gpui_platform::application().run(move |cx: &mut App| {
        ant_design_gpui::init(cx);
        let scope = ConfigScope::new_window(cx);
        scope.update_patch(cx, |patch| patch.motion = Some(false)).unwrap();
        cx.open_window(WindowOptions {
            window_bounds: Some(WindowBounds::Windowed(Bounds::centered(None, size(px(740.), px(1000.)), cx))),
            show: false, focus: false, ..Default::default()
        }, |window, cx| {
            let title = format!("Ant Design Message hidden capture {}", std::process::id());
            window.set_window_title(&title);
            let messages = cx.new(|cx| MessageState::new(scope.clone(), window, cx));
            let capture = cx.new(|_| Capture { scope: scope.clone(), messages });
            let root = cx.new(|cx| UiRoot::new(scope.clone(), capture.clone(), window, cx));
            window.spawn(cx, async move |cx| {
                let scenes: Value = serde_json::from_str(include_str!("../fixtures/message/visual-cases.json")).unwrap();
                let mut metrics = Vec::new();
                cx.background_executor().timer(Duration::from_millis(150)).await;
                for (scene, sample) in scenes.as_object().unwrap() {
                    if selected_scene.as_ref().is_some_and(|selected| selected != scene) { continue; }
                    let width = sample["width"].as_f64().unwrap_or(740.) as f32;
                    let height = sample["height"].as_f64().unwrap_or(1000.) as f32;
                    for (theme, algorithms) in [("default", AlgorithmChain::default()), ("dark", AlgorithmChain::dark()), ("compact", AlgorithmChain::compact()), ("dark-compact", AlgorithmChain::dark_compact())] {
                        if selected_theme.as_deref().is_some_and(|selected| selected != theme) { continue; }
                        for (direction, direction_value) in [("ltr", Direction::Ltr), ("rtl", Direction::Rtl)] {
                            if selected_direction.as_deref().is_some_and(|selected| selected != direction) { continue; }
                            cx.update(|_, cx| scope.update_patch(cx, |patch| {
                                patch.direction = Some(direction_value);
                                patch.theme.get_or_insert_default().algorithms = Some(algorithms.clone());
                            }).unwrap()).unwrap();
                            for scale in [1., 1.5, 2.] {
                                if selected_scale.is_some_and(|selected| selected != scale) { continue; }
                                set_native_scale(&title, scale);
                                cx.update(|window, cx| {
                                    window.resize(size(px(width), px(height)));
                                    window.simulate_mouse_move(point(px(width - 1.), px(height - 1.)), cx);
                                    configure(&capture, sample, window, cx);
                                    window.refresh();
                                    window.draw(cx).clear(cx);
                                }).unwrap();
                                cx.background_executor().timer(Duration::from_millis(80)).await;
                                cx.update(|window, cx| {
                                    window.bounds_changed(cx);
                                    #[cfg(not(target_os = "windows"))]
                                    window.set_scale_factor(scale);
                                    assert!((window.scale_factor() - scale).abs() < 0.001);
                                    window.draw(cx).clear(cx);
                                    if sample["hover"] == true {
                                        let api = Message::new(&capture.read(cx).messages);
                                        let layouts = api.layouts(cx);
                                        let latest_key = sample["items"].as_array().unwrap().last().unwrap()["key"].as_str().unwrap();
                                        let latest = layouts.iter().find(|layout| layout.key.as_ref() == latest_key).expect("latest stack message");
                                        window.simulate_mouse_move(latest.bounds.center(), cx);
                                    }
                                    window.draw(cx).clear(cx);
                                }).unwrap();
                                cx.background_executor().timer(Duration::from_millis(40)).await;
                                cx.update(|window, cx| {
                                    window.draw(cx).clear(cx);
                                    let name = format!("{scene}-{theme}-{direction}-{}", (scale * 100.) as u32);
                                    let image = window.render_to_image().expect("native Message GPU frame");
                                    assert_eq!(image.width(), (width * scale) as u32);
                                    assert_eq!(image.height(), (height * scale) as u32);
                                    image.save(output.join(format!("{name}.png"))).unwrap();
                                    let api = Message::new(&capture.read(cx).messages);
                                    let layouts = api.layouts(cx);
                                    let items = sample["items"].as_array().unwrap();
                                    let retained_from = sample["maxCount"].as_u64().filter(|count| *count > 0).map_or(0, |count| items.len().saturating_sub(count as usize));
                                    let rows: Vec<_> = items.iter().skip(retained_from).map(|item| {
                                        let key = item["key"].as_str().unwrap();
                                        let layout = layouts.iter().find(|layout| layout.key.as_ref() == key);
                                        json!({"key":key,"root":layout.map(|layout| rectangle(layout.bounds)),"visible":layout.is_some_and(|layout| !layout.closing)})
                                    }).collect();
                                    metrics.push(json!({"name":name,"scale":window.scale_factor(),"imageWidth":image.width(),"imageHeight":image.height(),"rows":rows}));
                                }).unwrap();
                            }
                            println!("Captured native Message {scene}/{theme}/{direction}");
                        }
                    }
                }
                assert!(!metrics.is_empty(), "capture filter selected no cases");
                std::fs::write(output.join("native-metrics.json"), serde_json::to_vec_pretty(&metrics).unwrap()).unwrap();
                cx.update(|_, cx| cx.quit()).unwrap();
            }).detach();
            root
        }).expect("hidden Message capture window");
    });
}
