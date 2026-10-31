// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
//! Real hidden platform-window captures of the shared ColorPicker reference scenes.

use ant_design_gpui::{
    ColorPicker, ColorPickerFormat, ColorPickerMode, ColorPickerPanel, ColorPickerPreset,
    ColorPickerState, ColorPickerStyles, ColorPickerTheme, ColorPickerTokenPatch, ColorPickerValue,
    ColorStop, ConfigPatch, ConfigScope, Direction, PickerColor, Size, UiRoot, ValidationStatus,
    button::Button,
    icons::{IconName, icon},
    popup::PopupPlacement,
    theme::{AlgorithmChain, Color, ComponentThemeConfig, TokenPatch},
};
use gpui::{
    App, AppContext, Bounds, Context, Entity, IntoElement, MouseMoveEvent, Pixels, PlatformInput,
    Render, Window, WindowBounds, WindowOptions, div, point, prelude::*, px, relative, size,
};
use serde_json::{Value, json};
use std::{
    path::PathBuf,
    sync::atomic::{AtomicU64, Ordering},
    time::Duration,
};

static FOREGROUND_CHECKS: AtomicU64 = AtomicU64::new(0);

#[cfg(target_os = "windows")]
fn assert_background() -> u32 {
    #[link(name = "user32")]
    unsafe extern "system" {
        fn GetForegroundWindow() -> isize;
        fn GetWindowThreadProcessId(window: isize, process: *mut u32) -> u32;
    }
    let mut process = 0;
    unsafe {
        GetWindowThreadProcessId(GetForegroundWindow(), &mut process);
    }
    assert_ne!(
        process,
        std::process::id(),
        "hidden capture must never own foreground focus"
    );
    FOREGROUND_CHECKS.fetch_add(1, Ordering::Relaxed);
    process
}

#[cfg(not(target_os = "windows"))]
fn assert_background() -> u32 {
    0
}

fn color_value(value: &Value) -> ColorPickerValue {
    if value.is_null() {
        ColorPickerValue::Cleared
    } else if let Some(stops) = value.as_array() {
        ColorPickerValue::gradient(stops.iter().map(|stop| {
            ColorStop::new(
                PickerColor::parse(stop["color"].as_str().unwrap()).unwrap(),
                stop["percent"].as_f64().unwrap(),
            )
            .unwrap()
        }))
        .unwrap()
    } else {
        value.as_str().unwrap().parse().unwrap()
    }
}

fn presets() -> Vec<ColorPickerPreset> {
    vec![
        ColorPickerPreset::new(
            "recommended",
            "Recommended",
            [
                "#f5222d", "#fa8c16", "#fadb14", "#52c41a", "#13c2c2", "#1677ff", "#722ed1",
                "#eb2f96",
            ]
            .map(|text| text.parse().unwrap()),
        ),
        ColorPickerPreset::new(
            "recent",
            "Recent",
            [
                "#1677ff80".parse().unwrap(),
                "#ffffff".parse().unwrap(),
                "#000000".parse().unwrap(),
                ColorPickerValue::gradient([
                    ColorStop::new(PickerColor::parse("#1677ff").unwrap(), 0.).unwrap(),
                    ColorStop::new(PickerColor::parse("#722ed1").unwrap(), 100.).unwrap(),
                ])
                .unwrap(),
            ],
        )
        .default_open(false),
    ]
}

struct Capture {
    scope: ConfigScope,
    custom_scope: ConfigScope,
    scenes: Value,
    scene: String,
    states: Vec<Entity<ColorPickerState>>,
}

impl Capture {
    fn configure(&mut self, scene: String, window: &mut Window, cx: &mut Context<Self>) {
        self.states = self.scenes[&scene]
            .as_array()
            .unwrap()
            .iter()
            .map(|_| cx.new(|cx| ColorPickerState::new(window, cx)))
            .collect();
        self.scene = scene;
        cx.notify();
    }
}

impl Render for Capture {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let config = self.scope.snapshot(cx);
        let mut page = div()
            .w(px(640.))
            .h(px(720.))
            .p(px(48.))
            .pl(px(self.scenes[&self.scene][0]["anchorX"]
                .as_f64()
                .unwrap_or(48.) as f32))
            .flex()
            .items_start()
            .gap(px(12.))
            .bg(config.theme.tokens.color_bg_container.to_hsla());
        for (index, sample) in self.scenes[&self.scene]
            .as_array()
            .unwrap()
            .iter()
            .enumerate()
        {
            let picker_id = gpui::ElementId::from(format!("{}-{index}", self.scene));
            let scope = if sample["tokenCase"] == true {
                self.custom_scope.clone()
            } else {
                self.scope.clone()
            };
            if sample["standalone"] == true {
                page = page.child(
                    div().flex_none().child(
                        ColorPickerPanel::new(picker_id.clone(), &self.states[index])
                            .scope(scope)
                            .value(color_value(&sample["value"])),
                    ),
                );
                continue;
            }
            let mut picker = ColorPicker::new(picker_id, &self.states[index])
                .scope(scope)
                .value(color_value(&sample["value"]))
                .open(sample["open"] == true)
                .show_text(sample["showText"] == true)
                .allow_clear(sample["allowClear"] == true)
                .disabled(sample["disabled"] == true)
                .disabled_alpha(sample["disabledAlpha"] == true)
                .disabled_format(sample["disabledFormat"] == true)
                .status(match sample["status"].as_str() {
                    Some("error") => ValidationStatus::Error,
                    Some("warning") => ValidationStatus::Warning,
                    _ => ValidationStatus::None,
                })
                .auto_adjust_overflow(false);
            if let Some(arrow) = sample["arrow"].as_bool() {
                picker = picker.arrow(arrow);
            }
            if sample["placement"] == "bottomRight" {
                picker = picker.placement(PopupPlacement::BottomRight);
            }
            if sample["customTrigger"] == true {
                picker = picker.child(
                    Button::new("custom-trigger")
                        .scope(self.scope.clone())
                        .label("Choose color"),
                );
            }
            if sample["semantic"] == true {
                picker = picker.styles(ColorPickerStyles {
                    body: div()
                        .size(px(40.))
                        .rounded(px(12.))
                        .bg(gpui::rgb(0x00ff00))
                        .border(px(3.))
                        .border_color(gpui::rgb(0x800080))
                        .style()
                        .clone(),
                    content: div()
                        .size(px(20.))
                        .rounded(px(5.))
                        .bg(gpui::rgb(0xff0000))
                        .border(px(2.))
                        .border_color(gpui::rgb(0x0000ff))
                        .style()
                        .clone(),
                    ..Default::default()
                });
            }
            if sample["textRender"] == "custom" {
                picker = picker.text_render(|value, _, _| {
                    format!("Custom Text ({})", value.active_color(0).to_hex_string())
                });
            }
            if sample["textRender"] == "icon" {
                let font_size = config.theme.tokens.font_size as f32;
                let open = sample["open"] == true;
                picker = picker.text_render(move |_, _, _| {
                    icon(IconName::DownOutlined)
                        .size(px(font_size))
                        .inherit_color()
                        .text_color(Color::BLACK.with_alpha(0.25))
                        .rotation(gpui::radians(if open { std::f32::consts::PI } else { 0. }))
                });
            }
            picker = picker.size(match sample["size"].as_str() {
                Some("small") => Size::Small,
                Some("large") => Size::Large,
                _ => Size::Medium,
            });
            if let Some(format) = sample["format"].as_str() {
                picker = picker.format(match format {
                    "rgb" => ColorPickerFormat::Rgb,
                    "hsb" => ColorPickerFormat::Hsb,
                    _ => ColorPickerFormat::Hex,
                });
            }
            if let Some(modes) = sample["mode"].as_array() {
                picker = picker.modes(modes.iter().map(|mode| {
                    if mode == "gradient" {
                        ColorPickerMode::Gradient
                    } else {
                        ColorPickerMode::Single
                    }
                }));
            }
            if sample["presets"] == true {
                picker = picker.presets(presets());
            }
            if sample["customPanel"] == true {
                picker = picker.panel_render(|parts, window, cx| {
                    div()
                        .flex()
                        .flex_col()
                        .child(
                            div()
                                .text_size(px(12.))
                                .line_height(relative(20. / 12.))
                                .mb(px(8.))
                                .child("Color Picker"),
                        )
                        .child(parts.panel(window, cx))
                });
            }
            if sample["splitPanel"] == true {
                let split = config.theme.tokens.color_split;
                picker = picker.panel_render(move |parts, window, cx| {
                    // The source uses direction:rtl plus row-reverse in RTL;
                    // those cancel to this same physical order. GPUI flex
                    // order is physical and independent of text direction.
                    div()
                        .flex()
                        .gap(px(12.))
                        .w(px(493.))
                        .child(
                            div()
                                .w(px(234.))
                                .flex_none()
                                .child(parts.presets(window, cx)),
                        )
                        .child(div().w(px(1.)).flex_none().bg(split.to_hsla()))
                        .child(
                            div()
                                .w(px(234.))
                                .flex_none()
                                .child(parts.picker(window, cx)),
                        )
                });
            }
            page = page.child(div().flex_none().child(picker));
        }
        page
    }
}

fn rect(value: Option<Bounds<Pixels>>) -> Value {
    value.map_or(Value::Null, |bounds| {
        json!({"x":f32::from(bounds.origin.x),"y":f32::from(bounds.origin.y),
            "width":f32::from(bounds.size.width),"height":f32::from(bounds.size.height)})
    })
}

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
        fn GetClientRect(window: isize, rect: *mut Rect) -> i32;
        fn SendMessageW(window: isize, message: u32, wparam: usize, lparam: isize) -> isize;
        fn SetWindowPos(
            window: isize,
            after: isize,
            x: i32,
            y: i32,
            width: i32,
            height: i32,
            flags: u32,
        ) -> i32;
    }
    let title: Vec<u16> = title.encode_utf16().chain(Some(0)).collect();
    // The unique title resolves only this capture process's own hidden window.
    unsafe {
        assert_background();
        let handle = FindWindowW(std::ptr::null(), title.as_ptr());
        assert_ne!(handle, 0, "hidden ColorPicker capture HWND");
        let mut bounds = Rect::default();
        assert_ne!(GetWindowRect(handle, &mut bounds), 0);
        let dpi = (96. * scale).round() as usize;
        SendMessageW(
            handle,
            0x02E0,
            dpi | (dpi << 16),
            (&bounds as *const Rect) as isize,
        );
        // Window::resize in the pinned Windows backend omits SWP_NOACTIVATE.
        // Compute actual non-client extents and resize this owned HWND directly.
        let mut client = Rect::default();
        assert_ne!(GetWindowRect(handle, &mut bounds), 0);
        assert_ne!(GetClientRect(handle, &mut client), 0);
        let client_width = (640. * scale).round() as i32;
        let client_height = (720. * scale).round() as i32;
        let width = client_width + (bounds.right - bounds.left) - (client.right - client.left);
        let height = client_height + (bounds.bottom - bounds.top) - (client.bottom - client.top);
        assert_ne!(
            SetWindowPos(handle, 0, 0, 0, width, height, 0x2 | 0x4 | 0x10),
            0
        );
        assert_ne!(GetClientRect(handle, &mut client), 0);
        assert_eq!(
            (client.right - client.left, client.bottom - client.top),
            (client_width, client_height)
        );
        assert_background();
    }
}

#[cfg(not(target_os = "windows"))]
fn set_native_scale(_: &str, _: f32) {}

fn main() {
    let mut args = std::env::args().skip(1);
    assert_eq!(args.next().as_deref(), Some("--output"));
    let output = PathBuf::from(args.next().expect("output directory"));
    let mut filters = std::collections::HashMap::new();
    while let Some(option) = args.next() {
        assert!(
            ["--scene", "--theme", "--direction", "--scale"].contains(&option.as_str()),
            "unknown capture filter"
        );
        filters.insert(option, args.next().expect("capture filter value"));
    }
    if let Some(scale) = filters.get("--scale") {
        assert!(
            scale
                .split(',')
                .all(|scale| ["1", "1.5", "2"].contains(&scale)),
            "unsupported DPI scale"
        );
    }
    std::fs::create_dir_all(&output).unwrap();
    gpui_platform::application().run(move |cx: &mut App| {
        ant_design_gpui::init(cx);
        let scope = ConfigScope::new_window(cx);
        scope.update_patch(cx, |patch| patch.motion = Some(false)).unwrap();
        let mut patch = ConfigPatch::default();
        patch.theme.get_or_insert_default().components.set_theme::<ColorPickerTheme>(
            ComponentThemeConfig::new(ColorPickerTokenPatch::default()).with_tokens(TokenPatch {
                color_primary: Some(Color::rgb(0x722ed1)),
                color_primary_hover: Some(Color::rgb(0x9254de)),
                color_border: Some(Color::rgb(0xb37feb)),
                control_outline: Some(Color::rgb(0x722ed1).with_alpha(0.15)),
                border_radius: Some(10.),
                ..Default::default()
            }),
        );
        let custom_scope = scope.child(patch, cx).unwrap();
        cx.open_window(WindowOptions {
            window_bounds: Some(WindowBounds::Windowed(Bounds::centered(None, size(px(640.), px(720.)), cx))),
            show: false, focus: false, ..Default::default()
        }, |window, cx| {
            let native_title = format!("ColorPicker hidden capture {}", std::process::id());
            window.set_window_title(&native_title);
            assert_background();
            let font=scope.snapshot(cx).font.clone();
            let mut kern_font=font.clone();
            let mut features=kern_font.features.tag_value_list().to_vec();
            features.retain(|(tag,_)|tag!="kern");features.push(("kern".to_owned(),1));
            kern_font.features=gpui::FontFeatures(std::sync::Arc::new(features));
            let mut font_metrics=Vec::new();
            for font_size in [12.,14.] {
                for text in ["Transparent","Custom Text (#1677ff)","#1677FF"] {
                    let width=|font:gpui::Font|window.text_system().shape_line(text.to_owned().into(),px(font_size),&[gpui::TextRun{len:text.len(),font,color:gpui::black(),background_color:None,underline:None,strikethrough:None}],None).width;
                    font_metrics.push(json!({"text":text,"font_size":font_size,"default_width":f32::from(width(font.clone())),"kern_width":f32::from(width(kern_font.clone()))}));
                }
            }
            std::fs::write(output.join("font-metrics.json"),serde_json::to_vec_pretty(&font_metrics).unwrap()).unwrap();
            let scenes: Value = serde_json::from_str(include_str!("../fixtures/color_picker/visual-cases.json")).unwrap();
            let names: Vec<_> = scenes.as_object().unwrap().keys()
                .filter(|scene| filters.get("--scene").is_none_or(|filter| filter.split(',').any(|value| value == *scene))).cloned().collect();
            assert!(!names.is_empty(), "unknown scene");
            let capture = cx.new(|_| Capture { scope: scope.clone(), custom_scope, scenes, scene: names[0].clone(), states: vec![] });
            capture.update(cx, |view, cx| view.configure(names[0].clone(), window, cx));
            let root = cx.new(|cx| UiRoot::new(scope.clone(), capture.clone(), window, cx));
            window.spawn(cx, async move |cx| {
                let mut metrics = Vec::new();
                cx.background_executor().timer(Duration::from_millis(150)).await;
                for scene in names {
                    cx.update(|window, cx| capture.update(cx, |view, cx| view.configure(scene.clone(), window, cx))).unwrap();
                    for (theme, algorithms) in [("default", AlgorithmChain::default()), ("dark", AlgorithmChain::dark()), ("compact", AlgorithmChain::compact()), ("dark-compact", AlgorithmChain::dark_compact())] {
                        if filters.get("--theme").is_some_and(|filter| !filter.split(',').any(|value|value==theme)) { continue; }
                        for (direction, direction_value) in [("ltr", Direction::Ltr), ("rtl", Direction::Rtl)] {
                            if filters.get("--direction").is_some_and(|filter| !filter.split(',').any(|value|value==direction)) { continue; }
                            cx.update(|_, cx| scope.update_patch(cx, |patch| {
                                patch.direction = Some(direction_value);
                                patch.theme.get_or_insert_default().algorithms = Some(algorithms.clone());
                            }).unwrap()).unwrap();
                            for scale in [1., 1.5, 2.] {
                                if filters.get("--scale").is_some_and(|filter| !filter.split(',').any(|value|value.parse::<f32>().unwrap()==scale)) { continue; }
                                set_native_scale(&native_title, scale);
                                cx.update(|window, cx| {
                                    #[cfg(not(target_os="windows"))]
                                    window.resize(size(px(640.),px(720.)));
                                    assert_background();window.bounds_changed(cx);window.refresh();
                                    // bounds_changed polls the real platform cursor, even
                                    // for hidden windows. Restore local synthetic input
                                    // after every poll so the user's cursor is irrelevant.
                                    window.dispatch_event(PlatformInput::MouseMove(MouseMoveEvent{position:point(px(0.),px(0.)),..Default::default()}),cx);
                                    window.draw(cx).clear(cx);assert_background();
                                }).unwrap();
                                cx.background_executor().timer(Duration::from_millis(80)).await;
                                cx.update(|window, cx| {
                                    window.bounds_changed(cx);
                                    assert_background();
                                    #[cfg(not(target_os = "windows"))]
                                    window.set_scale_factor(scale);
                                    assert!((window.scale_factor() - scale).abs() < 0.001, "platform DPI mismatch");
                                    for _ in 0..2 {
                                        window.dispatch_event(PlatformInput::MouseMove(MouseMoveEvent{position:point(px(0.),px(0.)),..Default::default()}),cx);
                                        window.draw(cx).clear(cx);
                                    }
                                    let sample=capture.read(cx).scenes[&scene][0].clone();
                                    if sample["focus"] == "hex" {
                                        let state = capture.read(cx).states[0].clone();
                                        state.update(cx, |state, cx| state.focus_input(0, window, cx));
                                        window.draw(cx).clear(cx);
                                    }
                                    if sample["focus"] == "hue" {
                                        let state = capture.read(cx).states[0].clone();
                                        state.update(cx, |state, cx| state.focus_hue(window, cx));
                                        window.draw(cx).clear(cx);
                                    }
                                    if sample["hover"] == true {
                                        let bounds=capture.read(cx).states[0].read(cx).geometry().trigger.unwrap();
                                        for _ in 0..2 {
                                            window.dispatch_event(PlatformInput::MouseMove(MouseMoveEvent{position:bounds.center(),..Default::default()}),cx);
                                            window.draw(cx).clear(cx);
                                        }
                                    } else {
                                        window.dispatch_event(PlatformInput::MouseMove(MouseMoveEvent{position:point(px(0.),px(0.)),..Default::default()}),cx);
                                        window.draw(cx).clear(cx);
                                    }
                                }).unwrap();
                                // Let queued hover notifications invalidate retained
                                // elements before the final frame is read back.
                                cx.background_executor().timer(Duration::from_millis(20)).await;
                                cx.update(|window,cx| {
                                    assert_background();
                                    let sample=capture.read(cx).scenes[&scene][0].clone();
                                    let expects_hover=sample["hover"] == true;
                                    let pointer=if expects_hover {
                                        capture.read(cx).states[0].read(cx).geometry().trigger.unwrap().center()
                                    } else { point(px(0.),px(0.)) };
                                    for _ in 0..2 {
                                        window.dispatch_event(PlatformInput::MouseMove(MouseMoveEvent{position:pointer,..Default::default()}),cx);
                                        window.draw(cx).clear(cx);
                                    }
                                    let capture_pointer=window.mouse_position();
                                    if !expects_hover { assert_eq!(capture_pointer,point(px(0.),px(0.)),"neutral local pointer changed before capture"); }
                                    for (index,state) in capture.read(cx).states.iter().enumerate() {
                                        assert_eq!(state.read(cx).is_trigger_hovered(),expects_hover && index==0,"unexpected retained trigger hover in {scene}/{theme}/{direction}/{scale} row {index}");
                                    }
                                    let name = format!("{scene}-{theme}-{direction}-{}", (scale * 100.) as u32);
                                    let image = window.render_to_image().expect("native GPU frame");
                                    assert_eq!(image.width(), (640. * scale) as u32);
                                    assert_eq!(image.height(), (720. * scale) as u32);
                                    image.save(output.join(format!("{name}.png"))).unwrap();
                                    let foreground_pid=assert_background();
                                    let view = capture.read(cx);
                                    let bounds = view.states[0].read(cx).geometry();
                                    let rows: Vec<_> = view.states.iter().map(|state| {
                                        let bounds = state.read(cx).geometry();
                                        json!({"trigger":rect(bounds.trigger),"swatch":rect(bounds.trigger_swatch),"hovered":state.read(cx).is_trigger_hovered()})
                                    }).collect();
                                    let scene_counts=window.rendered_scene();
                                    let primitives=json!({"quads":scene_counts.quads.len(),"paths":scene_counts.paths.len(),"shadows":scene_counts.shadows.len(),"path_vertices":scene_counts.paths.iter().map(|p|p.vertices.len()).sum::<usize>()});
                                    let dpi=window.scale_factor();
                                    let quads:Vec<_>=scene_counts.quads.iter().map(|quad|json!({"x":quad.bounds.origin.x.0/dpi,"y":quad.bounds.origin.y.0/dpi,"width":quad.bounds.size.width.0/dpi,"height":quad.bounds.size.height.0/dpi,
                                        "background":format!("{:?}",quad.background),"border_color":format!("{:?}",quad.border_color),"border_widths":[quad.border_widths.top.0/dpi,quad.border_widths.right.0/dpi,quad.border_widths.bottom.0/dpi,quad.border_widths.left.0/dpi],
                                        "content_mask":{"x":quad.content_mask.bounds.origin.x.0/dpi,"y":quad.content_mask.bounds.origin.y.0/dpi,"width":quad.content_mask.bounds.size.width.0/dpi,"height":quad.content_mask.bounds.size.height.0/dpi},
                                        "radii":[quad.corner_radii.top_left.0/dpi,quad.corner_radii.top_right.0/dpi,quad.corner_radii.bottom_right.0/dpi,quad.corner_radii.bottom_left.0/dpi]})).collect();
                                    metrics.push(json!({"name":name,"scene":scene,"theme":theme,"direction":direction,"scale":window.scale_factor(),"canvas":{"width":640,"height":720},"rows":rows,"primitives":primitives,"quads":quads,"foreground_pid":foreground_pid,"capture_pid":std::process::id(),"capture_pointer":{"x":f32::from(capture_pointer.x),"y":f32::from(capture_pointer.y)},"expected_hover":expects_hover,
                                        "geometry":{"panel":rect(bounds.panel),"palette":rect(bounds.palette),"hue":rect(bounds.hue),"alpha":rect(bounds.alpha),"preview":rect(bounds.preview),"gradient":rect(bounds.gradient),"clear":rect(bounds.clear),
                                            "editors":bounds.editors.into_iter().map(|b|rect(Some(b))).collect::<Vec<_>>(),"presets":bounds.presets.into_iter().map(|b|rect(Some(b))).collect::<Vec<_>>(),
                                            "handles":bounds.handles.into_iter().map(|h|{let mut value=rect(Some(h.bounds));value["kind"]=json!(h.kind.as_ref());value["index"]=json!(h.index);value}).collect::<Vec<_>>()}}));
                                }).unwrap();
                            }
                            println!("Captured native ColorPicker {scene}/{theme}/{direction}");
                        }
                    }
                }
                assert!(!metrics.is_empty(), "no scenarios selected");
                std::fs::write(output.join("native-metrics.json"), serde_json::to_vec_pretty(&metrics).unwrap()).unwrap();
                std::fs::write(output.join("capture-context.json"),serde_json::to_vec_pretty(&json!({"process_id":std::process::id(),"foreground_checks":FOREGROUND_CHECKS.load(Ordering::Relaxed),"foreground_violations":0,"resize":"owned HWND SetWindowPos SWP_NOACTIVATE|SWP_NOZORDER|SWP_NOMOVE","pointer_policy":"neutral synthetic local pointer after every bounds poll; deliberate hover only in hover scene; retained hover asserted per row before capture","cases":metrics.len()})).unwrap()).unwrap();
                cx.update(|_, cx| cx.quit()).unwrap();
            }).detach();
            root
        }).expect("hidden ColorPicker capture window");
    });
}
