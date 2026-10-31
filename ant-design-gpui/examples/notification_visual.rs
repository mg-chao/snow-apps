// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
//! Native GPU captures of the same descriptions as the independent browser renderer.
use ant_design_gpui::{
    Button, ConfigPatch, ConfigScope, Direction, Size, UiRoot,
    button::ButtonType,
    icons::{IconColors, IconName},
    notification::{
        Notification, NotificationConfigPatch, NotificationHost, NotificationOptions,
        NotificationPlacement, NotificationStack, NotificationState, NotificationStyles,
        NotificationTheme, NotificationTokenPatch, NotificationType,
    },
    theme::{AlgorithmChain, Color},
};
use gpui::{
    App, AppContext, Bounds, Context, Entity, IntoElement, Render, Window, WindowBounds,
    WindowOptions, div, linear_color_stop, linear_gradient, point, prelude::*, px, size,
};
use serde_json::{Value, json};
use std::{path::PathBuf, time::Duration};
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
        assert_ne!(handle, 0, "hidden Notification capture HWND");
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
    notifications: Entity<NotificationState>,
}
impl Render for Capture {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        NotificationHost::new(
            "notification-visual-host",
            &self.notifications,
            div()
                .size_full()
                .bg(self.scope.snapshot(cx).theme.tokens.color_bg_container),
        )
    }
}
fn placement(value: &Value) -> NotificationPlacement {
    match value.as_str().unwrap_or("topRight") {
        "top" => NotificationPlacement::Top,
        "topLeft" => NotificationPlacement::TopLeft,
        "bottom" => NotificationPlacement::Bottom,
        "bottomLeft" => NotificationPlacement::BottomLeft,
        "bottomRight" => NotificationPlacement::BottomRight,
        _ => NotificationPlacement::TopRight,
    }
}
fn gradient(angle: f32, a: u32, b: u32) -> gpui::Background {
    linear_gradient(
        angle,
        linear_color_stop(Color::rgb(a).to_hsla(), 0.),
        linear_color_stop(Color::rgb(b).to_hsla(), 1.),
    )
}
fn configure(view: &Entity<Capture>, sample: &Value, window: &mut Window, cx: &mut App) {
    view.update(cx, |view, cx| {
        Notification::new(&view.notifications).destroy_all(window, cx);
        let scope = if sample["tokenCase"] == true {
            let mut patch = ConfigPatch::default();
            patch
                .theme
                .get_or_insert_default()
                .components
                .set::<NotificationTheme>(NotificationTokenPatch {
                    width: Some(px(420.).into()),
                    progress_bg: Some(gradient(135., 0x6253e1, 0x04befe)),
                    color_success_bg: Some(gradient(30., 0xd9f7be, 0xf6ffed)),
                    color_info_bg: Some(gradient(30., 0xbae0ff, 0xe6f4ff)),
                    color_warning_bg: Some(gradient(30., 0xffffb8, 0xfeffe6)),
                    color_error_bg: Some(gradient(30., 0xffccc7, 0xfff1f0)),
                    ..Default::default()
                });
            view.scope.child(patch, cx).unwrap()
        } else {
            view.scope.clone()
        };
        let state = cx.new(|cx| NotificationState::new(scope.clone(), window, cx));
        let api = Notification::new(&state);
        api.set_config(
            NotificationConfigPatch {
                styles: if sample["holderStyles"] == true {
                    NotificationStyles {
                        list: gpui::StyleRefinement::default()
                            .p(px(31.))
                            .bg(gpui::rgb(0xfafafa)),
                        list_content: gpui::StyleRefinement::default()
                            .gap(px(37.))
                            .bg(gpui::rgb(0xf0f5ff)),
                        ..Default::default()
                    }
                } else {
                    NotificationStyles::default()
                },
                top: sample["top"].as_f64().map(|v| v as f32),
                bottom: sample["bottom"].as_f64().map(|v| v as f32),
                placement: Some(placement(&sample["placement"])),
                max_count: Some(sample["maxCount"].as_u64().map(|v| v as usize)),
                stack: Some(if sample["stack"] == true {
                    NotificationStack::Enabled {
                        threshold: sample["threshold"].as_u64().unwrap_or(3) as usize,
                    }
                } else {
                    NotificationStack::Disabled
                }),
                ..Default::default()
            },
            window,
            cx,
        );
        for item in sample["items"].as_array().unwrap() {
            let mut options = NotificationOptions::default()
                .key(item["key"].as_str().unwrap().to_owned())
                .duration(if item["showProgress"] == true {
                    Duration::from_secs(86400)
                } else {
                    Duration::ZERO
                });
            if let Some(title) = item["title"].as_str() {
                options = options.title(title.to_owned());
            }
            if let Some(description) = item["description"].as_str() {
                options = options.description(description.to_owned());
            }
            if item["placement"].is_string() {
                options = options.placement(placement(&item["placement"]));
            }
            if let Some(kind) = item["type"].as_str() {
                options = options.kind(match kind {
                    "success" => NotificationType::Success,
                    "error" => NotificationType::Error,
                    "warning" => NotificationType::Warning,
                    _ => NotificationType::Info,
                });
            }
            if item["icon"] == "none" {
                options = options.show_icon(false);
            }
            if item["icon"] == "smile" {
                options = options.icon(
                    IconName::SmileOutlined
                        .icon()
                        .with_colors(IconColors::primary(Color::rgb(0x108ee9).to_rgba())),
                );
            }
            if let Some(value) = item["closable"].as_bool() {
                options = options.closable(value);
            }
            options = options
                .close_disabled(item["closeDisabled"] == true)
                .show_progress(item["showProgress"] == true);
            if item["closeIcon"] == "minus" {
                options = options.close_icon(IconName::MinusOutlined);
            }
            if item["actions"] == true {
                let scope = scope.clone();
                options = options.actions(move |handle, _, cx| {
                    let handle = handle.clone();
                    div()
                        .flex()
                        .gap(px(8.))
                        .when(scope.snapshot(cx).direction == Direction::Rtl, |row| {
                            row.flex_row_reverse()
                        })
                        .child(
                            Button::new("destroy")
                                .scope(scope.clone())
                                .size(Size::Small)
                                .button_type(ButtonType::Link)
                                .label("Destroy All"),
                        )
                        .child(
                            Button::new("confirm")
                                .scope(scope.clone())
                                .size(Size::Small)
                                .button_type(ButtonType::Primary)
                                .label("Confirm")
                                .on_click(move |_, window, cx| handle.close(window, cx)),
                        )
                });
            }
            if item["semantic"] == true {
                let mut root = div()
                    .bg(Color::rgb(0xf6ffed))
                    .border_2()
                    .border_color(Color::rgb(0x95de64))
                    .rounded(px(16.))
                    .shadow(vec![gpui::BoxShadow {
                        color: Color::rgb(0xd9f7be).to_hsla(),
                        offset: point(px(4.), px(4.)),
                        blur_radius: px(0.),
                        spread_radius: px(0.),
                        inset: false,
                    }]);
                let mut title = div()
                    .text_color(Color::rgb(0x237804))
                    .font_weight(gpui::FontWeight::SEMIBOLD);
                let mut description = div().text_color(Color::rgb(0x3f6600));
                let mut icon = div().text_color(Color::rgb(0x237804));
                options = options.styles(NotificationStyles {
                    root: root.style().clone(),
                    title: title.style().clone(),
                    description: description.style().clone(),
                    icon: icon.style().clone(),
                    ..Default::default()
                });
            }
            api.open(options, window, cx);
        }
        view.notifications = state;
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
            let title = format!("Ant Design Notification hidden capture {}", std::process::id());
            window.set_window_title(&title);
            let notifications = cx.new(|cx| NotificationState::new(scope.clone(), window, cx));
            let capture = cx.new(|_| Capture { scope: scope.clone(), notifications });
            let root = cx.new(|cx| UiRoot::new(scope.clone(), capture.clone(), window, cx));
            window.spawn(cx, async move |cx| {
                let scenes: Value = serde_json::from_str(include_str!("../fixtures/notification/visual-cases.json")).unwrap();
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
                                        let api = Notification::new(&capture.read(cx).notifications);
                                        let layouts = api.layouts(cx);
                                        let latest_key = sample["items"].as_array().unwrap().last().unwrap()["key"].as_str().unwrap();
                                        let latest = layouts.iter().find(|layout| layout.key.as_ref() == latest_key).expect("latest stack notification");
                                        window.simulate_mouse_move(latest.bounds.center(), cx);
                                    }
                                    window.draw(cx).clear(cx);
                                }).unwrap();
                                cx.background_executor().timer(Duration::from_millis(40)).await;
                                cx.update(|window, cx| {
                                    window.draw(cx).clear(cx);
                                    let name = format!("{scene}-{theme}-{direction}-{}", (scale * 100.) as u32);
                                    let image = window.render_to_image().expect("native Notification GPU frame");
                                    assert_eq!(image.width(), (width * scale) as u32);
                                    assert_eq!(image.height(), (height * scale) as u32);
                                    image.save(output.join(format!("{name}.png"))).unwrap();
                                    let api = Notification::new(&capture.read(cx).notifications);
                                    let layouts = api.layouts(cx);
                                    let items = sample["items"].as_array().unwrap();
                                    let retained_from = sample["maxCount"].as_u64().filter(|count| *count > 0).map_or(0, |count| items.len().saturating_sub(count as usize));
                                    let rows: Vec<_> = items.iter().skip(retained_from).map(|item| {
                                        let key = item["key"].as_str().unwrap();
                                        let layout = layouts.iter().find(|layout| layout.key.as_ref() == key);
                                        json!({"key":key,"root":layout.map(|layout| rectangle(layout.bounds)),"visible":layout.is_some_and(|layout| layout.visible && !layout.closing)})
                                    }).collect();
                                    metrics.push(json!({"name":name,"scale":window.scale_factor(),"imageWidth":image.width(),"imageHeight":image.height(),"rows":rows}));
                                }).unwrap();
                            }
                            println!("Captured native Notification {scene}/{theme}/{direction}");
                        }
                    }
                }
                assert!(!metrics.is_empty(), "capture filter selected no cases");
                std::fs::write(output.join("native-metrics.json"), serde_json::to_vec_pretty(&metrics).unwrap()).unwrap();
                cx.update(|_, cx| cx.quit()).unwrap();
            }).detach();
            root
        }).expect("hidden Notification capture window");
    });
}
