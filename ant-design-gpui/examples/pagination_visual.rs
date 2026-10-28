// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Hidden native captures driven by fixtures/pagination/visual-cases.json.
use ant_design_gpui::{
    ConfigScope, Direction, InputNumber, InputNumberState, Pagination, PaginationState, Size,
    UiRoot, Variant,
    locale::Locale,
    pagination::{
        PaginationAlign, PaginationItemKind, PaginationMode, PaginationStyles, PaginationTheme,
        PaginationTokenPatch, QuickJumper,
    },
    theme::{AlgorithmChain, Color},
};
use gpui::{
    App, AppContext, Bounds, Context, Entity, IntoElement, KeyDownEvent, KeyUpEvent, Keystroke,
    MouseButton, MouseDownEvent, MouseMoveEvent, MouseUpEvent, Pixels, PlatformInput, Render,
    Window, WindowBounds, WindowOptions, canvas, div, point, prelude::*, px, size,
};
use serde_json::{Value, json};
use std::{
    cell::RefCell,
    collections::{BTreeMap, HashMap},
    path::PathBuf,
    rc::Rc,
    time::Duration,
};

// Use the exact production compositor for supplementary shaping diagnostics;
// keep the library's implementation private to its normal public API.
#[path = "../src/pagination/bidi.rs"]
mod paragraph_probe;

type Centers = Rc<RefCell<HashMap<PaginationItemKind, gpui::Point<Pixels>>>>;
struct Capture {
    scope: ConfigScope,
    scenario: Value,
    state: Entity<PaginationState>,
    custom_size: Entity<InputNumberState>,
    centers: Centers,
}
impl Capture {
    fn configure(&mut self, scenario: Value, window: &mut Window, cx: &mut Context<Self>) {
        self.state = cx.new(|cx| PaginationState::new(window, cx));
        self.custom_size = cx.new(|cx| {
            let mut state = InputNumberState::new(window, cx);
            state
                .set_value(
                    Some((scenario["page_size"].as_u64().unwrap_or(10) as f64).into()),
                    window,
                    cx,
                )
                .unwrap();
            state
        });
        self.centers.borrow_mut().clear();
        self.scenario = scenario;
        cx.notify();
    }
}
impl Render for Capture {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        let s = &self.scenario;
        let kind = s["kind"].as_str().unwrap_or("basic");
        let total = s["total"].as_u64().unwrap_or(500);
        let current = s["current"].as_u64().unwrap_or(6);
        let page_size = s["page_size"].as_u64().unwrap_or(10);
        let mut pager = Pagination::new("pagination", &self.state)
            .scope(self.scope.clone())
            .total(total)
            .default_current(current)
            .default_page_size(page_size)
            .accessibility_label("Pagination")
            .disabled(s["disabled"] == true)
            .responsive(s["responsive"] == true)
            .show_less_items(s["show_less"] == true)
            .hide_on_single_page(s["hide_single"] == true)
            .show_title(s["show_title"] != false)
            .show_prev_next_jumpers(s["show_jumpers"] != false)
            .mode(match kind {
                "simple" => PaginationMode::Simple,
                "simple-readonly" => PaginationMode::SimpleReadOnly,
                _ => PaginationMode::Full,
            })
            .align(match s["align"].as_str() {
                Some("center") => PaginationAlign::Center,
                Some("end") => PaginationAlign::End,
                _ => PaginationAlign::Start,
            });
        if let Some(size) = s["size"].as_str() {
            pager = pager.size(parse_size(size));
        }
        if let Some(boundary) = s["size_boundary"].as_u64() {
            pager = pager.total_boundary_show_size_changer(boundary);
        }
        if let Some(options) = s["page_size_options"].as_array() {
            pager = pager.page_size_options(options.iter().map(|value| value.as_u64().unwrap()));
        }
        if s["show_size_changer"].as_str() != Some("auto") {
            pager = pager.show_size_changer(
                s["show_size_changer"]
                    .as_bool()
                    .unwrap_or(matches!(kind, "changer" | "all" | "custom-changer")),
            );
        }
        if matches!(kind, "quick" | "quick-go" | "all") || s["show_quick_jumper"] == true {
            pager = pager.quick_jumper(if kind == "quick-go" || s["go"] == true {
                QuickJumper::Confirm
            } else {
                QuickJumper::Blur
            });
        }
        if kind == "total" || kind == "all" {
            pager = pager.show_total_text(|total, range, _, _| {
                format!("{}-{} of {total} items", range[0], range[1])
            });
        }
        if kind == "custom-changer" {
            let custom = self.custom_size.clone();
            pager = pager.size_changer_render(move |info, _, cx| {
                InputNumber::new("custom-page-size", &custom, cx)
                    .scope(info.scope.clone())
                    .w(px(100.))
                    .disabled(info.disabled)
                    .accessibility_label("Page Size")
            });
        }
        let open = s["open"] == true;
        pager = pager.size_changer_select(move |select, _, _| select.open(open));
        if kind == "semantic" {
            let root = div()
                .border_2()
                .border_dashed()
                .border_color(Color::rgb(0xcccccc))
                .p(px(8.))
                .style()
                .clone();
            let mut item = div().rounded(px(999.));
            if s["size"] == "small" {
                item = item.bg(Color::rgb(0xc8c8c8).with_alpha(0.3)).mr(px(4.));
            }
            pager = pager.styles(PaginationStyles {
                root,
                item: item.style().clone(),
            });
        }
        let custom = kind == "custom";
        let centers = self.centers.clone();
        let item_scope = self.scope.clone();
        pager = pager.item_render(move |info, original, window, cx| {
            let content = if custom {
                match info.kind {
                    PaginationItemKind::Previous => {
                        intrinsic_label("Previous", &item_scope, window, cx).into_any_element()
                    }
                    PaginationItemKind::Next => {
                        intrinsic_label("Next", &item_scope, window, cx).into_any_element()
                    }
                    _ => original,
                }
            } else {
                original
            };
            let centers = centers.clone();
            let kind = info.kind;
            Some(
                div()
                    .relative()
                    .child(content)
                    .child(
                        canvas(
                            move |bounds, _, _| {
                                centers.borrow_mut().insert(kind, bounds.center());
                            },
                            |_, _, _, _| {},
                        )
                        .absolute()
                        .inset_0()
                        .size_full(),
                    )
                    .into_any_element(),
            )
        });
        let width = s["viewport_width"].as_f64().unwrap_or(1280.);
        let background = if s["theme"].as_str().unwrap_or("default").starts_with("dark") {
            Color::rgb(0x141414)
        } else {
            Color::WHITE
        };
        div().size_full().bg(background).child(
            div()
                .id(gpui::SharedString::from(
                    s["name"]
                        .as_str()
                        .unwrap_or("pagination-fixture")
                        .to_owned(),
                ))
                .absolute()
                .left(px(64.))
                .top(px(48.))
                .w(px(s["width"].as_f64().unwrap_or(width - 128.) as f32))
                .child(pager),
        )
    }
}
fn intrinsic_label(
    text: impl Into<gpui::SharedString>,
    scope: &ConfigScope,
    window: &mut Window,
    cx: &gpui::App,
) -> gpui::Div {
    let text = text.into();
    let config = scope.snapshot(cx);
    let globals = &config.theme.tokens;
    let run = gpui::TextRun {
        len: text.len(),
        font: config.font.clone(),
        color: globals.color_text.to_hsla(),
        background_color: None,
        underline: None,
        strikethrough: None,
    };
    let width = window
        .text_system()
        .shape_line(text.clone(), px(globals.font_size as f32), &[run], None)
        .width;
    div()
        .w(width)
        .min_w_0()
        .whitespace_nowrap()
        .text_color(globals.color_link)
        .child(text)
}
fn parse_size(size: &str) -> Size {
    match size {
        "small" => Size::Small,
        "large" => Size::Large,
        _ => Size::Medium,
    }
}

fn original_line_ranges(reference: &Value, scenario: &Value, scale: f32) -> Vec<(usize, usize)> {
    let Some(label) = reference["scales"]
        .as_array()
        .unwrap()
        .iter()
        .find(|entry| entry["scale"].as_f64() == Some(f64::from(scale)))
        .and_then(|entry| {
            entry["cases"]
                .as_array()
                .unwrap()
                .iter()
                .find(|case| case["scenario"]["name"] == scenario["name"])
        })
        .and_then(|case| {
            case["text"]
                .as_array()
                .unwrap()
                .iter()
                .find(|label| label["selector"] == ".ant-pagination-total-text")
        })
    else {
        return Vec::new();
    };
    if label["line_count"].as_u64().unwrap() < 2 {
        return Vec::new();
    }
    let mut lines = BTreeMap::<i64, (usize, usize)>::new();
    let mut offset = 0;
    for character in label["characters"].as_array().unwrap() {
        let text = character["character"].as_str().unwrap();
        let start = offset;
        offset += text.len();
        if text.trim().is_empty() {
            continue;
        }
        let y = (character["y"].as_f64().unwrap() * 64.).round() as i64;
        lines
            .entry(y)
            .and_modify(|range| range.1 = offset)
            .or_insert((start, offset));
    }
    lines.into_values().collect()
}
fn main() {
    let mut args = std::env::args().skip(1);
    let (mut output, mut input, mut scale) = (None, None, None);
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--output" => output = args.next().map(PathBuf::from),
            "--cases" => input = args.next(),
            "--scale" => scale = Some(args.next().unwrap().parse::<f32>().unwrap()),
            _ => panic!(
                "Usage: pagination_visual --output DIRECTORY [--cases JSON] [--scale 1|1.5|2]"
            ),
        }
    }
    let output = output.expect("--output DIRECTORY");
    std::fs::create_dir_all(&output).unwrap();
    let scenarios: Vec<Value> = input.map_or_else(
        || vec![json!({"name":"pagination-basic","kind":"basic"})],
        |path| serde_json::from_str(&std::fs::read_to_string(path).unwrap()).unwrap(),
    );
    let text_reference: Value = serde_json::from_str(
        &std::fs::read_to_string(concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/fixtures/pagination/text-reference.json"
        ))
        .unwrap(),
    )
    .unwrap();
    gpui_platform::application().run(move |cx: &mut App| {
        ant_design_gpui::init(cx);
        let scope = ConfigScope::new_window(cx);
        scope.update_patch(cx, |p| p.motion = Some(false)).unwrap();
        cx.open_window(WindowOptions { window_bounds: Some(WindowBounds::Windowed(Bounds::centered(None,
            size(px(1280.), px(420.)), cx))), show: false, focus: false, ..Default::default() }, |window, cx| {
            window.resize(size(px(1280.), px(420.)));
            let title = format!("Pagination visual fixture {}", std::process::id());
            window.set_window_title(&title);
            let state = cx.new(|cx| PaginationState::new(window, cx));
            let custom_size = cx.new(|cx| InputNumberState::new(window, cx));
            let capture = cx.new(|_| Capture { scope: scope.clone(), scenario: Value::Null, state,
                custom_size, centers: Rc::new(RefCell::new(HashMap::new())) });
            let root = cx.new(|cx| UiRoot::new(scope.clone(), capture.clone(), window, cx));
            window.spawn(cx, async move |cx| {
                cx.background_executor().timer(Duration::from_millis(150)).await;
                if let Some(scale) = scale {
                    let worker = change_native_dpi(title, scale);
                    while !worker.is_finished() { cx.background_executor().timer(Duration::from_millis(20)).await; }
                    worker.join().unwrap();
                    cx.update(|window, _| window.resize(size(px(1280.), px(420.)))).unwrap();
                    cx.background_executor().timer(Duration::from_millis(100)).await;
                    cx.update(|window, _| assert_eq!(window.scale_factor(), scale)).unwrap();
                }
                let mut metrics = Vec::new();
                for scenario in scenarios {
                    cx.update(|window, _| window.resize(size(px(scenario["viewport_width"].as_f64().unwrap_or(1280.) as f32), px(420.)))).unwrap();
                    cx.background_executor().timer(Duration::from_millis(20)).await;
                    cx.update(|window, cx| {
                        window.blur(cx);
                        for key in ["space", "enter"] { window.dispatch_event(PlatformInput::KeyUp(KeyUpEvent { keystroke: Keystroke::parse(key).unwrap() }), cx); }
                        window.dispatch_event(PlatformInput::MouseUp(MouseUpEvent { position: point(px(0.), px(0.)), button: MouseButton::Left, click_count: 1, ..Default::default() }), cx);
                        window.dispatch_event(PlatformInput::MouseMove(MouseMoveEvent { position: point(px(0.), px(0.)), ..Default::default() }), cx);
                        scope.update_patch(cx, |p| {
                            p.direction = Some(if scenario["direction"] == "rtl" { Direction::Rtl } else { Direction::Ltr });
                            p.size = scenario["provider_size"].as_str().map(parse_size);
                            p.variant = Some(match scenario["variant"].as_str() { Some("filled") => Variant::Filled,
                                Some("borderless") => Variant::Borderless, Some("underlined") => Variant::Underlined, _ => Variant::Outlined });
                            p.locale.locale = Some(match scenario["locale"].as_str() { Some("zh_CN") => Locale::zh_cn(), Some("zh_TW") => Locale::zh_tw(), _ => Locale::en_us() });
                            let theme = p.theme.get_or_insert_default();
                            theme.algorithms = Some(match scenario["theme"].as_str() { Some("dark") => AlgorithmChain::dark(),
                                Some("compact") => AlgorithmChain::compact(), Some("dark-compact") => AlgorithmChain::dark_compact(), _ => AlgorithmChain::default() });
                            theme.seed.wireframe = Some(scenario["wireframe"] == true);
                            theme.components.remove::<PaginationTheme>();
                            if scenario["tokens"] == true { theme.components.set::<PaginationTheme>(PaginationTokenPatch {
                                item_size: Some(36.), item_size_sm: Some(28.), item_size_lg: Some(44.),
                                item_bg: Some(Color::rgb(0xe6f4ff)), item_active_bg: Some(Color::rgb(0xd6e4ff)),
                                item_active_color: Some(Color::rgb(0x531dab)), item_active_color_hover: Some(Color::rgb(0x9254de)),
                                item_link_bg: Some(Color::rgb(0xfff7e6)), item_active_bg_disabled: Some(Color::rgb(0xfff1f0)),
                                item_active_color_disabled: Some(Color::rgb(0xa8071a)), item_input_bg: Some(Color::rgb(0xf9f0ff)),
                                mini_options_size_changer_top: Some(2.),
                            }); }
                        }).unwrap();
                        capture.update(cx, |capture, cx| capture.configure(scenario.clone(), window, cx));
                        window.draw(cx).clear(cx);
                        if let Some(focus) = scenario["focus"].as_str() {
                            window.dispatch_event(PlatformInput::KeyDown(KeyDownEvent { keystroke: Keystroke::parse("f24").unwrap(), is_held: false, prefer_character_input: false }), cx);
                            let state = capture.read(cx).state.clone();
                            match focus {
                                "quick" => {
                                    let input = state.read(cx).quick_input().clone();
                                    input.update(cx, |input, cx| input.focus(window, cx));
                                }
                                "simple" => {
                                    let input = state.read(cx).simple_input().clone();
                                    input.update(cx, |input, cx| input.focus(window, cx));
                                }
                                "changer" => state.read(cx).size_selector().clone().update(cx, |state, cx| state.focus(window, cx)),
                                "go" => {
                                    state.read(cx).quick_input().clone().update(cx, |input, cx| input.focus(window, cx));
                                    window.dispatch_event(PlatformInput::KeyDown(KeyDownEvent { keystroke: Keystroke::parse("tab").unwrap(), is_held:false, prefer_character_input:false }), cx);
                                    window.dispatch_event(PlatformInput::KeyUp(KeyUpEvent { keystroke: Keystroke::parse("tab").unwrap() }), cx);
                                }
                                _ => state.update(cx, |state, cx| state.focus_page(1, window, cx)),
                            }
                        }
                        if let Some(hover) = scenario["hover"].as_str() {
                            let current = scenario["current"].as_u64().unwrap_or(6);
                            let kind = match hover { "active" => PaginationItemKind::Page(current), "prev" => PaginationItemKind::Previous,
                                "jump" => PaginationItemKind::JumpNext, _ => PaginationItemKind::Page(1) };
                            let center = if hover == "go" {
                                // The plain form button is the shortest painted
                                // bordered control in these Go scenarios. Use
                                // native geometry even for translucent disabled
                                // paint, without borrowing browser coordinates.
                                let quad = window.rendered_scene().quads.iter().filter(|quad|
                                    quad.border_color.a > 0. && quad.border_widths.top.0 > 0.)
                                    .min_by(|left,right| left.bounds.size.height.0.total_cmp(&right.bounds.size.height.0))
                                    .expect("visible resting Go frame");
                                let scale = window.scale_factor();
                                point(px((quad.bounds.origin.x.0 + quad.bounds.size.width.0 / 2.) / scale),
                                    px((quad.bounds.origin.y.0 + quad.bounds.size.height.0 / 2.) / scale))
                            } else { capture.read(cx).centers.borrow().get(&kind).copied().expect("visible hover target") };
                            window.dispatch_event(PlatformInput::MouseMove(MouseMoveEvent { position: center, ..Default::default() }), cx);
                            if scenario["pressed"] == true { window.dispatch_event(PlatformInput::MouseDown(MouseDownEvent {
                                position: center, button: MouseButton::Left, click_count: 1, ..Default::default() }), cx); }
                        }
                    }).unwrap();
                    // Hover/focus notifications flush after the App update returns.
                    cx.background_executor().timer(Duration::from_millis(20)).await;
                    if let Some(key) = scenario["held_key"].as_str() {
                        let key = if key == "space" { "space" } else { "enter" };
                        cx.update(|window, cx| {
                            window.dispatch_event(PlatformInput::KeyDown(KeyDownEvent { keystroke: Keystroke::parse(key).unwrap(), is_held:false, prefer_character_input:false }), cx);
                        }).unwrap();
                        cx.background_executor().timer(Duration::from_millis(20)).await;
                    }
                    let result = cx.update(|window, cx| {
                        window.refresh(); window.draw(cx).clear(cx);
                        window.render_to_image().unwrap().save(output.join(format!("{}.png", scenario["name"].as_str().unwrap()))).unwrap();
                        let dpi = window.scale_factor();
                        let quads: Vec<_> = window.rendered_scene().quads.iter().map(|q| json!({
                            "x":q.bounds.origin.x.0/dpi,"y":q.bounds.origin.y.0/dpi,
                            "w":q.bounds.size.width.0/dpi,"h":q.bounds.size.height.0/dpi,
                            "background":format!("{:?}",q.background), "border_color":format!("Solid({:?})",q.border_color),
                            "border_widths":[q.border_widths.top.0/dpi,q.border_widths.right.0/dpi,q.border_widths.bottom.0/dpi,q.border_widths.left.0/dpi],
                            "radii":[q.corner_radii.top_left.0/dpi,q.corner_radii.top_right.0/dpi,q.corner_radii.bottom_right.0/dpi,q.corner_radii.bottom_left.0/dpi],
                        })).collect();
                        let centers: Vec<_> = capture.read(cx).centers.borrow().iter().map(|(kind, center)| json!({
                            "kind":format!("{kind:?}"), "x":f32::from(center.x), "y":f32::from(center.y),
                        })).collect();
                        let pointer = window.mouse_position();
                        let config = scope.snapshot(cx);
                        let font_probe: Vec<_> = ["Go to", "Page", "Previous", "/", "50", "12345678"].into_iter().map(|text| {
                            let run = gpui::TextRun { len:text.len(), font:config.font.clone(), color:config.theme.tokens.color_text.to_hsla(),
                                background_color:None, underline:None, strikethrough:None };
                            let line = window.text_system().shape_line(text.into(), px(config.theme.tokens.font_size as f32), std::slice::from_ref(&run), None);
                            let mut kern_font = config.font.clone();
                            let mut features = kern_font.features.tag_value_list().to_vec();
                            features.retain(|(tag, _)| tag != "kern");
                            features.push(("kern".into(), 1));
                            kern_font.features = gpui::FontFeatures(std::sync::Arc::new(features));
                            let kern_run = gpui::TextRun { font:kern_font, ..run };
                            let kern_line = window.text_system().shape_line(text.into(), px(config.theme.tokens.font_size as f32), &[kern_run], None);
                            json!({"text":text,"width":f32::from(line.width),"kern_width":f32::from(kern_line.width),"font":format!("{:?}",config.font),"font_size":config.theme.tokens.font_size})
                        }).collect();
                        // Supplement actual PNG review with native shaping evidence
                        // for the same logical labels and paragraph embedding.
                        let state = capture.read(cx).state.read(cx);
                        let range = state.range();
                        let labels = [
                            (".ant-select-content", format!("{} {}", state.page_size(), config.locale.pagination.items_per_page)),
                            (".ant-pagination-total-text", format!("{}-{} of {} items", range.0, range.1, state.total())),
                        ];
                        let bidi_probe: Vec<_> = labels.into_iter().map(|(selector, logical)| {
                            let text = logical.clone();
                            let mut font = config.font.clone();
                            let mut features = font.features.tag_value_list().to_vec();
                            features.retain(|(tag, _)| tag != "kern");
                            features.push(("kern".into(), 1));
                            font.features = gpui::FontFeatures(std::sync::Arc::new(features));
                            let line = paragraph_probe::shape_paragraph(text.clone().into(), config.direction, px(config.theme.tokens.font_size as f32), &gpui::TextRun {
                                len:text.len(), font, color:config.theme.tokens.color_text.to_hsla(), background_color:None, underline:None, strikethrough:None,
                            }, window);
                            let glyphs:Vec<_> = line.runs.iter().flat_map(|run| &run.glyphs).map(|glyph| json!({
                                "index":glyph.index, "x":f32::from(glyph.position.x), "y":f32::from(glyph.position.y),
                            })).collect();
                            json!({"selector":selector,"logical_text":logical,"shaped_text":text,"width":f32::from(line.width),"glyphs":glyphs})
                        }).collect();
                        // Original line ranges are verification inputs only;
                        // production layout discovers its own wrap boundaries.
                        let paragraph = format!("{}-{} of {} items", range.0, range.1, state.total());
                        let wrapped_bidi_probe: Vec<_> = original_line_ranges(&text_reference, &scenario, dpi).into_iter().map(|(start,end)| {
                            let mut font = config.font.clone();
                            let mut features = font.features.tag_value_list().to_vec();
                            features.retain(|(tag, _)| tag != "kern");
                            features.push(("kern".into(), 1));
                            font.features = gpui::FontFeatures(std::sync::Arc::new(features));
                            let line = paragraph_probe::shape_wrapped_line(paragraph.clone().into(), start..end, config.direction,
                                px(config.theme.tokens.font_size as f32), &gpui::TextRun { len:paragraph.len(),font,
                                color:config.theme.tokens.color_text.to_hsla(),background_color:None,underline:None,strikethrough:None },window);
                            let glyphs:Vec<_> = line.runs.iter().flat_map(|run| &run.glyphs).map(|glyph| json!({
                                "index":glyph.index, "x":f32::from(glyph.position.x), "y":f32::from(glyph.position.y),
                            })).collect();
                            json!({"selector":".ant-pagination-total-text","range":[start,end],"paragraph_logical_text":paragraph,
                                "logical_text":&paragraph[start..end],"shaped_text":&paragraph[start..end],"glyphs":glyphs})
                        }).collect();
                        json!({"scenario":scenario,"scale":dpi,"quads":quads,
                            "input_probe":{"pointer":{"x":f32::from(pointer.x),"y":f32::from(pointer.y)},"centers":centers},"font_probe":font_probe,"bidi_probe":bidi_probe,"wrapped_bidi_probe":wrapped_bidi_probe})
                    }).unwrap();
                    metrics.push(result);
                    if metrics.len() % 20 == 0 { println!("Captured {} native Pagination cases", metrics.len()); }
                }
                std::fs::write(output.join("native-metrics.json"), serde_json::to_string_pretty(&metrics).unwrap()+"\n").unwrap();
                println!("Captured {} native Pagination cases", metrics.len());
                cx.update(|_, cx| cx.quit()).ok();
            }).detach();
            root
        }).unwrap();
    });
}

// Send only this hidden fixture a native DPI notification; monitor settings stay unchanged.
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
        // SAFETY: the unique process title identifies this live fixture. The rectangle
        // remains allocated until synchronous SendMessage returns.
        unsafe {
            let hwnd = FindWindowW(std::ptr::null(), title.as_ptr());
            assert!(!hwnd.is_null());
            assert_ne!(GetWindowRect(hwnd, &mut rect), 0);
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
