// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Deterministic native popup captures. Render a real hidden platform window:
//! cargo run --example popup_visual -- --output <directory> [--all]

use ant_design_gpui::{
    Button, ConfigScope, Direction, Popconfirm, Popover, Tooltip, UiRoot,
    popup::{PopupArrow, PopupColor, PopupHandle, PopupPlacement, PopupTriggers},
    theme::{AlgorithmChain, Color, PresetColor},
};
use gpui::{
    App, AppContext, Bounds, Context, IntoElement, Render, StyleRefinement, Window, WindowBounds,
    WindowOptions, div, prelude::*, px, size,
};
use serde_json::{Value, json};
use std::{path::PathBuf, time::Duration};

const PLACEMENTS: [&str; 12] = [
    "top",
    "topLeft",
    "topRight",
    "bottom",
    "bottomLeft",
    "bottomRight",
    "left",
    "leftTop",
    "leftBottom",
    "right",
    "rightTop",
    "rightBottom",
];

struct Capture {
    scope: ConfigScope,
    handle: PopupHandle,
    unique_first: PopupHandle,
    component: String,
    scenario: Value,
    placement: usize,
}
impl Render for Capture {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let config = self.scope.snapshot(cx);
        if self.component == "unique" {
            return div()
                .size_full()
                .bg(config.theme.tokens.color_bg_container)
                .children((0_usize..2).map(|i| {
                    div()
                        .absolute()
                        .left(px(if i == 0 { 120. } else { 540. }))
                        .top(px(234.))
                        .child(
                            Tooltip::new(("unique", i))
                                .scope(self.scope.clone())
                                .unique(true)
                                .handle(if i == 0 {
                                    &self.unique_first
                                } else {
                                    &self.handle
                                })
                                .open(self.scenario["active"].as_u64() == Some(i as u64))
                                .title(if i == 0 {
                                    "Short hint"
                                } else {
                                    "Longer tooltip content that wraps naturally onto two lines"
                                })
                                .child(
                                    Button::new(("trigger", i))
                                        .label(if i == 0 { "Short" } else { "Long" })
                                        .w(px(80.))
                                        .h(px(32.)),
                                ),
                        )
                }))
                .into_any_element();
        }
        let trigger = Button::new("trigger")
            .scope(self.scope.clone())
            .label("Open")
            .w(px(80.))
            .h(px(32.));
        let placement = PopupPlacement::ALL[self.placement];
        let scenario = &self.scenario;
        let arrow = PopupArrow {
            visible: scenario["arrow"] != "hidden",
            point_at_center: scenario["arrow"] == "center",
        };
        let long = scenario["long"].as_bool().unwrap_or(false);
        let semantic = scenario["semantic"].as_bool().unwrap_or(false);
        let long_text = "This longer explanation wraps naturally onto several lines without losing its padding or moving the arrow away from the trigger.";
        let mut surface = StyleRefinement::default();
        if let Some(width) = scenario["width"].as_f64() {
            surface = surface.w(px(width as f32));
        }
        if semantic {
            surface = surface
                .border_1()
                .border_color(config.theme.tokens.color_primary)
                .p(px(20.));
        }
        let content_style = if semantic {
            StyleRefinement::default().italic()
        } else {
            StyleRefinement::default()
        };
        let title_style = if semantic {
            StyleRefinement::default().text_color(config.theme.tokens.color_primary)
        } else {
            StyleRefinement::default()
        };
        let color: Option<PopupColor> = scenario["color"].as_str().map(|color| {
            if color == "magenta" {
                PresetColor::Magenta.into()
            } else {
                color.parse::<Color>().unwrap().into()
            }
        });
        macro_rules! configured {
            ($control:expr) => {
                $control
                    .scope(self.scope.clone())
                    .handle(&self.handle)
                    .child(trigger)
                    .placement(placement)
                    .arrow(arrow)
                    .surface_style(surface)
                    .default_open(true)
                    .triggers(PopupTriggers::NONE)
            };
        }
        let popup = match self.component.as_str() {
            "tooltip" => {
                let mut control = configured!(
                    Tooltip::new("popup")
                        .title(if long { long_text } else { "Hint" })
                        .content_style(content_style)
                );
                if let Some(color) = color {
                    control = control.color(color);
                }
                control.into_any_element()
            }
            "popover" => {
                let mut control = configured!(
                    Popover::new("popup")
                        .title("Title")
                        .text(if long { long_text } else { "Content" })
                        .title_style(title_style)
                        .content_style(content_style)
                );
                if let Some(color) = color {
                    control = control.color(color);
                }
                control.into_any_element()
            }
            _ => configured!(
                Popconfirm::new("popup")
                    .title("Delete the item?")
                    .description(if long {
                        long_text
                    } else {
                        "This operation cannot be undone."
                    })
                    .title_style(title_style)
                    .description_style(content_style)
            )
            .into_any_element(),
        };
        let x = scenario["anchor"][0].as_f64().unwrap_or(360.) as f32;
        let y = scenario["anchor"][1].as_f64().unwrap_or(234.) as f32;
        div()
            .size_full()
            .bg(config.theme.tokens.color_bg_container)
            .child(div().absolute().left(px(x)).top(px(y)).child(popup))
            .into_any_element()
    }
}

fn main() {
    let mut output = None;
    let mut all = false;
    let mut scale = None;
    let mut placements = false;
    let mut input_cases = None;
    let mut unique = false;
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--output" => output = args.next().map(PathBuf::from),
            "--all" => all = true,
            "--placements" => placements = true,
            "--unique" => unique = true,
            "--cases" => input_cases = Some(args.next().expect("case JSON path")),
            "--scale" => {
                scale = Some(
                    args.next()
                        .expect("scale value")
                        .parse::<f32>()
                        .expect("numeric scale"),
                )
            }
            _ => panic!("Usage: popup_visual --output <directory> [--all]"),
        }
    }
    let output = output.expect("--output <directory>");
    std::fs::create_dir_all(&output).expect("create capture directory");
    gpui_platform::application().run(move |cx: &mut App| {
        ant_design_gpui::init(cx);
        let scope = ConfigScope::new_window(cx);
        scope.update_patch(cx, |patch| patch.motion = Some(false)).unwrap();
        cx.open_window(WindowOptions {
            window_bounds: Some(WindowBounds::Windowed(Bounds::centered(None, size(px(800.), px(500.)), cx))),
            show: false,
            focus: false,
            ..Default::default()
        }, |window, cx| {
            window.resize(size(px(800.), px(500.)));
            let title = format!("Popup visual fixture {}", std::process::id());
            window.set_window_title(&title);
            let capture = cx.new(|_| Capture { scope: scope.clone(), handle: PopupHandle::new(), unique_first: PopupHandle::new(), component: "tooltip".into(), placement: 0, scenario: Value::Null });
            let root = cx.new(|cx| UiRoot::new(scope.clone(), capture.clone(), window, cx));
            window.spawn(cx, async move |cx| {
                cx.background_executor().timer(Duration::from_millis(150)).await;
                if let Some(scale) = scale {
                    let finished = change_native_dpi(title, scale);
                    while !finished.is_finished() {
                        cx.background_executor().timer(Duration::from_millis(20)).await;
                    }
                    finished.join().expect("native DPI delivery");
                    cx.update(|window, _| window.resize(size(px(800.), px(500.)))).unwrap();
                    cx.background_executor().timer(Duration::from_millis(100)).await;
                    cx.update(|window, _| {
                        assert_eq!(window.scale_factor(), scale, "native DPI notification applied");
                        assert_eq!(window.viewport_size(), size(px(800.), px(500.)), "native logical viewport");
                    }).unwrap();
                }
                let mut cases = Vec::new();
                if unique {
                    cx.update(|window, cx| {
                        scope.update_patch(cx, |patch| patch.motion = Some(true)).unwrap();
                        capture.update(cx, |view, cx| { view.component = "unique".into(); view.scenario = json!({"active":0}); cx.notify(); });
                        window.draw(cx).clear(cx);
                    }).unwrap();
                    cx.background_executor().timer(Duration::from_millis(400)).await;
                    cx.update(|window, cx| { window.refresh(); window.draw(cx).clear(cx); window.render_to_image().unwrap().save(output.join("unique-before.png")).unwrap(); }).unwrap();
                    for time in [0_u64, 75, 150, 225, 350] {
                        // Readback/PNG encoding takes longer than a frame. Start
                        // each sample from a settled source instead of allowing
                        // the previous capture to consume this sample's timeline.
                        if time > 0 {
                            cx.update(|window, cx| {
                                capture.update(cx, |view, cx| { view.scenario = json!({"active":0}); cx.notify(); });
                                window.draw(cx).clear(cx);
                            }).unwrap();
                            cx.background_executor().timer(Duration::from_millis(400)).await;
                            cx.update(|window, cx| { window.refresh(); window.draw(cx).clear(cx); }).unwrap();
                        }
                        let started = std::time::Instant::now();
                        cx.update(|window, cx| {
                            capture.update(cx, |view, cx| { view.scenario = json!({"active":1}); cx.notify(); });
                            window.draw(cx).clear(cx);
                        }).unwrap();
                        cx.background_executor().timer(Duration::from_millis(time).saturating_sub(started.elapsed())).await;
                        let metric = cx.update(|window, cx| {
                            window.refresh();
                            let sample_time = started.elapsed().as_secs_f64()*1000.;
                            window.draw(cx).clear(cx);
                            let image = window.render_to_image().unwrap();
                            image.save(output.join(format!("unique-{time:03}.png"))).unwrap();
                            let scale = window.scale_factor();
                            let bodies: Vec<_> = window.rendered_scene().quads.iter().filter(|q| q.background.as_solid().is_some_and(|color| color.l < 0.001 && color.a > 0.8)).map(|q| {
                                json!({"x":q.bounds.origin.x.0/scale,"y":q.bounds.origin.y.0/scale,"width":q.bounds.size.width.0/scale,"height":q.bounds.size.height.0/scale})
                            }).collect();
                            json!({"requested_ms":time,"sample_ms":sample_time,"backgrounds":bodies})
                        }).unwrap();
                        cases.push(metric);
                    }
                    std::fs::write(output.join("unique-metrics.json"), serde_json::to_string_pretty(&cases).unwrap()+"\n").unwrap();
                    println!("Captured native unique handoff frames");
                    cx.update(|_, cx| cx.quit()).ok();
                    return;
                }
                if let Some(input_cases) = input_cases {
                    let inputs: Vec<Value> = serde_json::from_str(&std::fs::read_to_string(input_cases).unwrap()).unwrap();
                    for input in inputs {
                        let mut result = input.clone();
                        let name = input["name"].as_str().unwrap();
                        let metric = cx.update(|window, cx| {
                            scope.update_patch(cx, |patch| {
                                let theme = patch.theme.get_or_insert_default();
                                theme.seed.border_radius = (input["theme"] == "radius-16").then_some(16.);
                                patch.direction = Some(if input["rtl"] == true { Direction::Rtl } else { Direction::Ltr });
                            }).unwrap();
                            capture.update(cx, |view, cx| {
                                view.component = input["component"].as_str().unwrap().into();
                                view.placement = PLACEMENTS.iter().position(|p| Some(*p) == input["placement"].as_str()).unwrap();
                                view.scenario = input.clone(); cx.notify();
                            });
                            window.draw(cx).clear(cx);
                            window.draw(cx).clear(cx);
                            let image = window.render_to_image().unwrap();
                            image.save(output.join(format!("{name}.png"))).unwrap();
                            let layout = capture.read(cx).handle.layout(cx).unwrap();
                            json!({"scale":window.scale_factor(),"image":[image.width(),image.height()],
                                "body":{"x":f32::from(layout.bounds.left()),"y":f32::from(layout.bounds.top()),
                                "width":f32::from(layout.bounds.size.width),"height":f32::from(layout.bounds.size.height)}})
                        }).unwrap();
                        result.as_object_mut().unwrap().extend(metric.as_object().unwrap().clone());
                        cases.push(result);
                    }
                    std::fs::write(output.join("native-metrics.json"), serde_json::to_string_pretty(&cases).unwrap() + "\n").unwrap();
                    println!("Captured {} extended native popup cases", cases.len());
                    cx.update(|_, cx| cx.quit()).ok();
                    return;
                }
                let themes = if all { vec!["default", "dark", "compact", "dark-compact"] } else { vec!["default"] };
                for theme in themes {
                    for wireframe in if all { vec![false, true] } else { vec![false] } {
                    for rtl in if all || placements { vec![false, true] } else { vec![false] } {
                        for component in ["tooltip", "popover", "popconfirm"] {
                            for (placement, placement_name) in PLACEMENTS.iter().enumerate().take(if all || placements { 12 } else { 1 }) {
                                let name = format!("{theme}{}-{}-{component}-{}", if wireframe { "-wireframe" } else { "" }, if rtl { "rtl" } else { "ltr" }, placement_name);
                                let path = output.join(format!("{name}.png"));
                                let result = cx.update(|window, cx| {
                                    scope.update_patch(cx, |patch| {
                                        patch.theme.get_or_insert_default().seed.wireframe = Some(wireframe);
                                        patch.direction = Some(if rtl { Direction::Rtl } else { Direction::Ltr });
                                        patch.theme.get_or_insert_default().algorithms = Some(match theme { "dark" => AlgorithmChain::dark(), "compact" => AlgorithmChain::compact(), "dark-compact" => AlgorithmChain::dark_compact(), _ => AlgorithmChain::default() });
                                    }).unwrap();
                                    capture.update(cx, |view, cx| { view.component = component.into(); view.placement = placement; cx.notify(); });
                                    window.draw(cx).clear(cx);
                                    // Text/glyph assets may request one additional draw.
                                    window.draw(cx).clear(cx);
                                    let image = window.render_to_image().expect("native render");
                                    image.save(path).expect("save native capture");
                                    let layout = capture.read(cx).handle.layout(cx).unwrap_or_else(|| panic!("visible popup layout: viewport={:?}, open={}", window.viewport_size(), capture.read(cx).handle.is_open(cx)));
                                    json!({ "name": name, "theme": theme, "wireframe": wireframe, "rtl": rtl, "component": component, "placement": placement_name, "scale": window.scale_factor(), "image": [image.width(), image.height()], "body": { "x": f32::from(layout.bounds.origin.x), "y": f32::from(layout.bounds.origin.y), "width": f32::from(layout.bounds.size.width), "height": f32::from(layout.bounds.size.height) } })
                                }).expect("capture window remains open");
                                cases.push(result);
                            }
                        }
                    }
                }
                }
                std::fs::write(output.join("native-metrics.json"), serde_json::to_string_pretty(&cases).unwrap() + "\n").expect("save metrics");
                println!("Captured {} native popup cases", cases.len());
                cx.update(|_, cx| cx.quit()).ok();
            }).detach();
            root
        }).expect("native capture window");
    });
}

// Exercise the native Windows DPI-change path on this fixture's own hidden
// window. This changes neither monitor settings nor the user's other windows.
// SendMessage runs on a worker so the UI thread can process the DPI callback.
#[cfg(target_os = "windows")]
fn change_native_dpi(title: String, scale: f32) -> std::thread::JoinHandle<()> {
    assert!([1., 1.5, 2.].contains(&scale));
    #[repr(C)]
    struct Rect {
        left: i32,
        top: i32,
        right: i32,
        bottom: i32,
    }
    #[link(name = "user32")]
    unsafe extern "system" {
        fn FindWindowW(class: *const u16, title: *const u16) -> *mut std::ffi::c_void;
        fn GetWindowRect(window: *mut std::ffi::c_void, rect: *mut Rect) -> i32;
        fn SendMessageW(
            window: *mut std::ffi::c_void,
            message: u32,
            wparam: usize,
            lparam: isize,
        ) -> isize;
    }
    std::thread::spawn(move || {
        let title: Vec<u16> = title.encode_utf16().chain(Some(0)).collect();
        let mut rect = Rect {
            left: 0,
            top: 0,
            right: 0,
            bottom: 0,
        };
        // SAFETY: this unique title identifies our live fixture window. The
        // rectangle stays allocated until synchronous message delivery finishes.
        unsafe {
            let hwnd = FindWindowW(std::ptr::null(), title.as_ptr());
            assert!(!hwnd.is_null());
            assert_ne!(GetWindowRect(hwnd, &mut rect), 0);
            // Ensure WM_SIZE runs even when the suggested size was unchanged.
            rect.right += 1;
            let dpi = (scale * 96.) as usize;
            SendMessageW(
                hwnd,
                0x02e0,
                dpi | (dpi << 16),
                &rect as *const Rect as isize,
            );
        }
    })
}

#[cfg(not(target_os = "windows"))]
fn change_native_dpi(_: String, _: f32) -> std::thread::JoinHandle<()> {
    panic!("--scale currently exercises the Windows native DPI notification path")
}
