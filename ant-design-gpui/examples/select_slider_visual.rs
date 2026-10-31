// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Real platform-window captures corresponding to scripts/select_slider_reference.tsx.
use ant_design_gpui::{
    ConfigScope, Direction, Select, Size, Slider, UiRoot, ValidationStatus, Variant,
    popup::PopupPlacement,
    select::{
        SelectMode, SelectOption, SelectOptions, SelectSearch, SelectSettings, SelectState,
        SelectTheme, SelectTokenPatch,
    },
    slider::{
        SliderDisabled, SliderMark, SliderOptions, SliderOrientation, SliderRange, SliderState,
        SliderTheme, SliderTokenPatch, SliderTooltip, SliderValue,
    },
    theme::{AlgorithmChain, Color},
};
use gpui::{
    App, AppContext, Bounds, Context, Entity, IntoElement, KeyDownEvent, Keystroke, PlatformInput,
    Render, Window, WindowBounds, WindowOptions, div, prelude::*, px, size,
};
use serde_json::{Value, json};
use std::{path::PathBuf, time::Duration};

struct Capture {
    scope: ConfigScope,
    scenario: Value,
    select: Entity<SelectState<String>>,
    slider: Entity<SliderState>,
}
impl Capture {
    fn configure(&mut self, scenario: Value, window: &mut Window, cx: &mut Context<Self>) {
        let kind = scenario["kind"].as_str().unwrap_or("basic");
        let options = SelectOptions::new(
            [
                ("jack", "Jack"),
                ("lucy", "Lucy"),
                ("disabled", "Disabled"),
                ("yiminghe", "Yiminghe"),
            ]
            .into_iter()
            .filter(|_| kind != "empty")
            .map(|(value, label)| {
                SelectOption::new(value, value.to_owned(), label).disabled(value == "disabled")
            }),
        )
        .unwrap();
        let multiple = matches!(kind, "multiple" | "tags" | "responsive");
        self.select = cx.new(|cx| {
            let mut state = SelectState::new(options, window, cx);
            state.set_settings(
                SelectSettings {
                    mode: if kind == "tags" {
                        SelectMode::Tags
                    } else if multiple {
                        SelectMode::Multiple
                    } else {
                        SelectMode::Single
                    },
                    token_separators: vec![',', ';'],
                    ..Default::default()
                },
                cx,
            );
            state.set_search(
                SelectSearch {
                    enabled: Some(kind == "search" || multiple),
                    ..Default::default()
                },
                cx,
            );
            if !matches!(kind, "placeholder" | "empty") {
                state.set_values(
                    if multiple {
                        vec!["lucy".to_owned(), "jack".to_owned()]
                    } else {
                        vec!["lucy".to_owned()]
                    },
                    cx,
                );
            }
            state
        });
        let range = matches!(kind, "range" | "editable" | "draggable" | "disabled-handle");
        self.slider = cx.new(|cx| {
            let mut state = SliderState::with_options(
                SliderOptions {
                    range: range.then_some(SliderRange {
                        editable: kind == "editable",
                        draggable_track: kind == "draggable",
                        min_count: 1,
                        max_count: Some(5),
                    }),
                    orientation: if kind == "vertical" {
                        SliderOrientation::Vertical
                    } else {
                        SliderOrientation::Horizontal
                    },
                    reverse: scenario["reverse"] == true,
                    disabled: if kind == "disabled-handle" {
                        [false, true, false].into()
                    } else {
                        SliderDisabled::All(kind == "disabled")
                    },
                    marks: if kind == "marks" {
                        vec![0., 26., 37., 100.]
                    } else {
                        vec![]
                    },
                    step: (kind != "marks").then_some(1.),
                    dots: scenario["dots"] == true,
                    ..Default::default()
                },
                window,
                cx,
            )
            .unwrap();
            state
                .set_value(
                    if range {
                        SliderValue::Range(vec![20., 50., 80.])
                    } else {
                        30.0.into()
                    },
                    window,
                    cx,
                )
                .unwrap();
            state
        });
        self.scenario = scenario;
        cx.notify();
    }
}
impl Render for Capture {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let s = &self.scenario;
        let kind = s["kind"].as_str().unwrap_or("basic");
        let child = if s["component"] == "slider" {
            let mut slider = Slider::new("slider", &self.slider)
                .scope(self.scope.clone())
                .accessibility_label("Value")
                .tooltip(SliderTooltip {
                    open: (s["open"] == true).then_some(true),
                    ..Default::default()
                });
            if kind == "vertical" {
                slider = slider.h(px(280.));
            } else {
                slider = slider.w(px(280.));
            }
            if kind == "marks" {
                slider = slider.marks(
                    [(0., "0°C"), (26., "26°C"), (37., "37°C"), (100., "100°C")]
                        .map(|(v, l)| SliderMark::new(v, l)),
                );
            }
            slider.into_any_element()
        } else {
            let mut select = Select::new("select", &self.select)
                .scope(self.scope.clone())
                .w(px(280.))
                .placeholder("Please select")
                .allow_clear(true)
                .accessibility_label("Choose a value")
                .disabled(kind == "disabled")
                .loading(kind == "loading")
                .responsive_tags(kind == "responsive")
                .default_open(s["open"] == true)
                .size(match s["size"].as_str() {
                    Some("small") => Size::Small,
                    Some("large") => Size::Large,
                    _ => Size::Medium,
                })
                .variant(match s["variant"].as_str() {
                    Some("filled") => Variant::Filled,
                    Some("borderless") => Variant::Borderless,
                    Some("underlined") => Variant::Underlined,
                    _ => Variant::Outlined,
                })
                .status(match s["status"].as_str() {
                    Some("error") => ValidationStatus::Error,
                    Some("warning") => ValidationStatus::Warning,
                    _ => ValidationStatus::None,
                });
            if let Some(width) = s["popup_width"].as_f64() {
                select = select.popup_min_width(px(width as f32));
            }
            if let Some(placement) = s["placement"].as_str() {
                select = select.placement(match placement {
                    "bottomRight" => PopupPlacement::BottomRight,
                    "topLeft" => PopupPlacement::TopLeft,
                    "topRight" => PopupPlacement::TopRight,
                    _ => PopupPlacement::BottomLeft,
                });
            }
            if kind == "custom" {
                select = select
                    .prefix("To:")
                    .suffix("+")
                    .option_render(|option, _, _| {
                        div().child(format!("{} / team", option.option.label))
                    })
                    .label_render(|value, _, _| div().child(format!("{} / team", value.label)));
            }
            select.into_any_element()
        };
        div()
            .size_full()
            .bg(self.scope.snapshot(cx).theme.tokens.color_bg_container)
            .child(
                div()
                    .absolute()
                    .left(px(s["anchor_x"].as_f64().unwrap_or(64.) as f32))
                    .top(px(s["anchor_y"].as_f64().unwrap_or(48.) as f32))
                    .w(px(280.))
                    .child(child),
            )
    }
}
fn main() {
    let mut args = std::env::args().skip(1);
    let mut output = None;
    let mut input = None;
    let mut scale = None;
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--output" => output = args.next().map(PathBuf::from),
            "--cases" => input = args.next(),
            "--scale" => scale = Some(args.next().unwrap().parse::<f32>().unwrap()),
            _ => panic!(
                "Usage: select_slider_visual --output <directory> [--cases JSON] [--scale 1|1.5|2]"
            ),
        }
    }
    let output = output.expect("--output directory");
    std::fs::create_dir_all(&output).unwrap();
    let scenarios: Vec<Value> = input.map_or_else(
        || {
            vec![
                json!({"name":"select-open", "component":"select", "open":true}),
                json!({"name":"select-multiple", "component":"select", "kind":"multiple"}),
                json!({"name":"slider-basic", "component":"slider"}),
                json!({"name":"slider-open", "component":"slider", "open":true}),
            ]
        },
        |p| serde_json::from_str(&std::fs::read_to_string(p).unwrap()).unwrap(),
    );
    gpui_platform::application().run(move |cx: &mut App| {
        ant_design_gpui::init(cx);
        let scope = ConfigScope::new_window(cx);
        scope.update_patch(cx, |p| p.motion = Some(false)).unwrap();
        cx.open_window(WindowOptions {
            window_bounds: Some(WindowBounds::Windowed(Bounds::centered(None, size(px(800.), px(500.)), cx))),
            show: false, focus: false, ..Default::default()
        }, |window, cx| {
            window.resize(size(px(800.), px(500.)));
            let title = format!("Select Slider visual fixture {}", std::process::id());
            window.set_window_title(&title);
            let select = cx.new(|cx| SelectState::new(SelectOptions::new(Vec::<SelectOption<String>>::new()).unwrap(), window, cx));
            let slider = cx.new(|cx| SliderState::new(window, cx));
            let capture = cx.new(|_| Capture { scope: scope.clone(), scenario: Value::Null, select, slider });
            let root = cx.new(|cx| UiRoot::new(scope.clone(), capture.clone(), window, cx));
            window.spawn(cx, async move |cx| {
                cx.background_executor().timer(Duration::from_millis(150)).await;
                if let Some(scale) = scale {
                    let worker = change_native_dpi(title, scale);
                    while !worker.is_finished() { cx.background_executor().timer(Duration::from_millis(20)).await; }
                    worker.join().unwrap();
                    cx.update(|w,_| w.resize(size(px(800.), px(500.)))).unwrap();
                    cx.background_executor().timer(Duration::from_millis(100)).await;
                    cx.update(|w,_| assert_eq!(w.scale_factor(), scale)).unwrap();
                }
                let mut metrics = Vec::new();
                for scenario in scenarios {
                    let result = cx.update(|window, cx| {
                        scope.update_patch(cx, |p| {
                            p.direction = Some(if scenario["direction"] == "rtl" { Direction::Rtl } else { Direction::Ltr });
                            p.theme.get_or_insert_default().algorithms = Some(match scenario["theme"].as_str() {
                                Some("dark") => AlgorithmChain::dark(), Some("compact") => AlgorithmChain::compact(), Some("dark-compact") => AlgorithmChain::dark_compact(), _ => AlgorithmChain::default(),
                            });
                            let components = &mut p.theme.get_or_insert_default().components;
                            components.remove::<SelectTheme>();
                            components.remove::<SliderTheme>();
                            if scenario["tokens"] == true {
                                components.set::<SelectTheme>(SelectTokenPatch {
                                    selector_bg: Some(Color::rgb(0xf4fff8)), option_selected_bg: Some(Color::rgb(0xc3f5d2)),
                                    option_height: Some(40.), option_padding_block: Some(9.), option_padding_inline: Some(16.), ..Default::default()
                                });
                                components.set::<SliderTheme>(SliderTokenPatch {
                                    rail_size: Some(6.), handle_size: Some(14.), dot_size: Some(10.), rail_bg: Some(Color::rgb(0xeadfff)),
                                    track_bg: Some(Color::rgb(0x8855cc)), handle_color: Some(Color::rgb(0x8855cc)), ..Default::default()
                                });
                            }
                        }).unwrap();
                        capture.update(cx, |v,cx| v.configure(scenario.clone(), window, cx));
                        window.draw(cx).clear(cx);
                        if scenario["focus"] == true {
                            window.dispatch_event(PlatformInput::KeyDown(KeyDownEvent { keystroke: Keystroke::parse("f24").unwrap(), is_held: false, prefer_character_input: false }), cx);
                            capture.update(cx, |view, cx| {
                                if scenario["component"] == "slider" {
                                    view.slider.update(cx, |state, cx| state.focus(0, window, cx));
                                } else {
                                    view.select.update(cx, |state, cx| state.focus(window, cx));
                                }
                            });
                        }
                        window.refresh();
                        window.draw(cx).clear(cx);
                        let image = window.render_to_image().unwrap();
                        image.save(output.join(format!("{}.png", scenario["name"].as_str().unwrap()))).unwrap();
                        let dpi = window.scale_factor();
                        let quads: Vec<_> = window.rendered_scene().quads.iter().map(|q| json!({
                            "x":q.bounds.origin.x.0/dpi,"y":q.bounds.origin.y.0/dpi,
                            "w":q.bounds.size.width.0/dpi,"h":q.bounds.size.height.0/dpi,
                            "background":format!("{:?}",q.background),
                            "border_color":format!("Solid({:?})",q.border_color),
                            "border_widths":[q.border_widths.top.0/dpi,q.border_widths.right.0/dpi,q.border_widths.bottom.0/dpi,q.border_widths.left.0/dpi],
                            "radii":[q.corner_radii.top_left.0/dpi,q.corner_radii.top_right.0/dpi,q.corner_radii.bottom_right.0/dpi,q.corner_radii.bottom_left.0/dpi],
                        })).collect();
                        json!({"scenario":scenario,"scale":dpi,"quads":quads})
                    }).unwrap();
                    metrics.push(result);
                }
                std::fs::write(output.join("native-metrics.json"), serde_json::to_string_pretty(&metrics).unwrap()+"\n").unwrap();
                println!("Captured {} native Select/Slider cases", metrics.len());
                cx.update(|_,cx| cx.quit()).ok();
            }).detach();
            root
        }).unwrap();
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
