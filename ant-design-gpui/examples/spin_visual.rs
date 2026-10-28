// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Hidden native GPU captures corresponding to the independent upstream Spin scenes.
//! Run: cargo run --example spin_visual -- --output <directory>

use std::{
    cell::{Cell, RefCell},
    path::PathBuf,
    rc::Rc,
    time::Duration,
};

use ant_design_gpui::{
    ConfigPatch, ConfigScope, Direction, Size, Spin, UiRoot,
    spin::{SpinStyles, SpinTheme, SpinTokenPatch},
    theme::{AlgorithmChain, Color},
};
use gpui::{
    A11ySubtreeBuilder, App, AppContext, Bounds, Context, Element, ElementId, GlobalElementId,
    InspectorElementId, IntoElement, LayoutId, Pixels, Render, Role, SharedString, Window,
    WindowBounds, WindowOptions, accesskit, div, prelude::*, px, size,
};
use serde_json::{Value, json};

type Rectangle = Rc<Cell<Option<Bounds<Pixels>>>>;
type Rows = Rc<RefCell<Vec<(Rectangle, Rectangle)>>>;

/// Observe the actual component bounds without inserting another layout box.
struct Measured<E> {
    element: E,
    bounds: Rectangle,
}

fn measured<E: IntoElement>(element: E, bounds: Rectangle) -> Measured<E::Element> {
    Measured {
        element: element.into_element(),
        bounds,
    }
}

impl<E: Element> IntoElement for Measured<E> {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}

impl<E: Element> Element for Measured<E> {
    type RequestLayoutState = E::RequestLayoutState;
    type PrepaintState = E::PrepaintState;
    fn id(&self) -> Option<ElementId> {
        self.element.id()
    }
    fn source_location(&self) -> Option<&'static std::panic::Location<'static>> {
        self.element.source_location()
    }
    fn a11y_role(&self) -> Option<Role> {
        self.element.a11y_role()
    }
    fn write_a11y_info(&self, node: &mut accesskit::Node) {
        self.element.write_a11y_info(node);
    }
    fn a11y_synthetic_children(
        &mut self,
        state: &mut Self::PrepaintState,
        builder: &mut A11ySubtreeBuilder,
    ) {
        self.element.a11y_synthetic_children(state, builder);
    }
    fn request_layout(
        &mut self,
        id: Option<&GlobalElementId>,
        inspector: Option<&InspectorElementId>,
        window: &mut Window,
        cx: &mut App,
    ) -> (LayoutId, Self::RequestLayoutState) {
        self.element.request_layout(id, inspector, window, cx)
    }
    fn prepaint(
        &mut self,
        id: Option<&GlobalElementId>,
        inspector: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        state: &mut Self::RequestLayoutState,
        window: &mut Window,
        cx: &mut App,
    ) -> Self::PrepaintState {
        self.bounds.set(Some(bounds));
        self.element
            .prepaint(id, inspector, bounds, state, window, cx)
    }
    fn paint(
        &mut self,
        id: Option<&GlobalElementId>,
        inspector: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        state: &mut Self::RequestLayoutState,
        prepaint: &mut Self::PrepaintState,
        window: &mut Window,
        cx: &mut App,
    ) {
        self.element
            .paint(id, inspector, bounds, state, prepaint, window, cx);
    }
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
        fn SendMessageW(window: isize, message: u32, wparam: usize, lparam: isize) -> isize;
    }
    let title: Vec<u16> = title.encode_utf16().chain(Some(0)).collect();
    // This uniquely titled window belongs to this capture process only.
    unsafe {
        let handle = FindWindowW(std::ptr::null(), title.as_ptr());
        assert_ne!(handle, 0, "hidden Spin capture HWND");
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
    custom_scope: ConfigScope,
    scenes: Value,
    scene: String,
    rows: Rows,
}

impl Render for Capture {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let config = self.scope.snapshot(cx);
        let mut page = div()
            .w(px(740.))
            .h(px(1000.))
            .p(px(20.))
            .flex()
            .flex_col()
            .gap(px(12.))
            .items_start()
            .when(config.direction == Direction::Rtl, |d| d.items_end())
            .bg(config.theme.tokens.color_bg_container.to_hsla());
        for (index, sample) in self.scenes[&self.scene]
            .as_array()
            .unwrap()
            .iter()
            .enumerate()
        {
            let mut spin = Spin::new(SharedString::from(format!("visual-{}-{index}", self.scene)))
                .scope(if sample["tokenCase"] == true {
                    self.custom_scope.clone()
                } else {
                    self.scope.clone()
                })
                .size(match sample["size"].as_str() {
                    Some("small") => Size::Small,
                    Some("large") => Size::Large,
                    _ => Size::Medium,
                })
                .spinning(sample["spinning"].as_bool().unwrap_or(true))
                .fullscreen(sample["fullscreen"].as_bool().unwrap_or(false));
            if let Some(description) = sample["description"].as_str() {
                spin = spin.description(SharedString::from(description.to_owned()));
            }
            if let Some(percent) = sample["percent"].as_f64() {
                spin = spin.percent(percent);
            }
            if sample["indicator"] == "square" {
                let holder = if sample["tokenCase"] == true {
                    match sample["size"].as_str() {
                        Some("small") => 18.,
                        Some("large") => 44.,
                        _ => 28.,
                    }
                } else {
                    match sample["size"].as_str() {
                        Some("small") => config.theme.tokens.control_height_lg * 0.35,
                        Some("large") => config.theme.tokens.control_height,
                        _ => config.theme.tokens.control_height_lg / 2.,
                    }
                };
                spin = spin.indicator_content(
                    div()
                        .size(px(holder as f32))
                        .border_2()
                        .rounded(px(2.))
                        .border_color(config.theme.tokens.color_primary.to_hsla()),
                );
            }
            if sample["nested"] == true {
                spin = spin.child(
                    div()
                        .w(px(340.))
                        .h(px(100.))
                        .border_1()
                        .rounded(px(6.))
                        .p(px(16.))
                        .border_color(config.theme.tokens.color_border.to_hsla())
                        .bg(config.theme.tokens.color_bg_container.to_hsla())
                        .text_size(px(14.))
                        .line_height(px(22.))
                        .child("Loading content"),
                );
            }
            if sample["semantic"] == true {
                spin = spin.styles(SpinStyles {
                    section: div().gap(px(6.)).style().clone(),
                    indicator: div()
                        .text_color(config.theme.tokens.color_error.to_hsla())
                        .style()
                        .clone(),
                    description: div()
                        .text_color(config.theme.tokens.color_success.to_hsla())
                        .text_size(px(12.))
                        .line_height(px(12.))
                        .style()
                        .clone(),
                    ..Default::default()
                });
            }
            let (row_bounds, spin_bounds) = self.rows.borrow()[index].clone();
            let mut row = div()
                .relative()
                .w(px(sample["width"].as_f64().unwrap() as f32))
                .h(px(sample["height"].as_f64().unwrap() as f32))
                .flex_none()
                .flex()
                .items_center()
                .when(config.direction == Direction::Rtl, |d| d.flex_row_reverse());
            if sample["backdropCase"] == "uniform" {
                row = row.bg(Color::rgb(0x426e99));
            } else if sample["backdropCase"] == "stripes" {
                // Match the browser's repeating gradient with exact 40px solid
                // segments. Draw behind Spin without altering its layout.
                let colors = [0xe58d8d, 0x7aa8cc, 0xd6c077];
                row = row.child(
                    div()
                        .absolute()
                        .inset_0()
                        .flex()
                        .overflow_hidden()
                        .children((0..9).map(|index| {
                            div()
                                .flex_none()
                                .w(px(40.))
                                .h_full()
                                .bg(Color::rgb(colors[index % colors.len()]))
                        })),
                );
            }
            page = page.child(measured(row.child(measured(spin, spin_bounds)), row_bounds));
        }
        page
    }
}

fn rect_json(bounds: Bounds<Pixels>) -> Value {
    json!({"x":f32::from(bounds.origin.x),"y":f32::from(bounds.origin.y),
        "width":f32::from(bounds.size.width),"height":f32::from(bounds.size.height)})
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
                let value = value.parse::<f32>().expect("numeric device scale");
                assert!(
                    [1., 1.5, 2.].contains(&value),
                    "supported scales: 1, 1.5, 2"
                );
                selected_scale = Some(value);
            }
            _ => panic!("unknown capture option: {option}"),
        }
    }
    if let Some(theme) = selected_theme.as_deref() {
        assert!(
            ["default", "dark", "compact", "dark-compact"].contains(&theme),
            "unknown theme"
        );
    }
    if let Some(direction) = selected_direction.as_deref() {
        assert!(["ltr", "rtl"].contains(&direction), "unknown direction");
    }
    std::fs::create_dir_all(&output).expect("create output directory");
    gpui_platform::application().run(move |cx: &mut App| {
        ant_design_gpui::init(cx);
        let scope = ConfigScope::new_window(cx);
        scope.update_patch(cx, |patch| patch.motion = Some(false)).unwrap();
        let mut custom = ConfigPatch::default();
        custom.theme.get_or_insert_default().components.set::<SpinTheme>(SpinTokenPatch {
            dot_size: Some(28.), dot_size_sm: Some(18.), dot_size_lg: Some(44.),
            content_height: Some(120.),
        });
        let custom_scope = scope.child(custom, cx).unwrap();
        cx.open_window(WindowOptions {
            window_bounds: Some(WindowBounds::Windowed(Bounds::centered(None, size(px(740.), px(1000.)), cx))),
            show: false, focus: false, ..Default::default()
        }, |window, cx| {
            let native_title = format!("Ant Design Spin hidden capture {}", std::process::id());
            window.set_window_title(&native_title);
            let scenes: Value = serde_json::from_str(include_str!("../fixtures/spin/visual-cases.json")).unwrap();
            let scene_names: Vec<_> = scenes.as_object().unwrap().keys().filter(|scene| selected_scene.as_ref().is_none_or(|selected| selected == *scene)).cloned().collect();
            assert!(!scene_names.is_empty(), "unknown scene");
            let count = scenes.as_object().unwrap().values().map(|v| v.as_array().unwrap().len()).max().unwrap();
            let rows: Rows = Rc::new(RefCell::new((0..count).map(|_| (Rc::default(), Rc::default())).collect()));
            let capture = cx.new(|_| Capture { scope: scope.clone(), custom_scope, scenes, scene: scene_names[0].clone(), rows: rows.clone() });
            let root = cx.new(|cx| UiRoot::new(scope.clone(), capture.clone(), window, cx));
            window.spawn(cx, async move |cx| {
                let mut metrics = Vec::new();
                cx.background_executor().timer(Duration::from_millis(150)).await;
                for scene in scene_names {
                    cx.update(|_, cx| capture.update(cx, |view, cx| { view.scene = scene.clone(); cx.notify(); })).unwrap();
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
                                set_native_scale(&native_title, scale);
                                cx.update(|window, cx| { window.resize(size(px(740.), px(1000.))); window.refresh(); window.draw(cx).clear(cx); }).unwrap();
                                cx.background_executor().timer(Duration::from_millis(80)).await;
                                cx.update(|window, cx| {
                                    window.bounds_changed(cx);
                                    #[cfg(not(target_os = "windows"))]
                                    window.set_scale_factor(scale);
                                    assert!((window.scale_factor() - scale).abs() < 0.001, "platform DPI mismatch");
                                    window.draw(cx).clear(cx);
                                    let name = format!("{scene}-{theme}-{direction}-{}", (scale * 100.) as u32);
                                    let image = window.render_to_image().expect("native GPU frame");
                                    assert_eq!(image.width(), (740. * scale) as u32, "physical capture width");
                                    assert_eq!(image.height(), (1000. * scale) as u32, "physical capture height");
                                    image.save(output.join(format!("{name}.png"))).unwrap();
                                    let samples = capture.read(cx).scenes[&scene].as_array().unwrap();
                                    let rectangles = rows.borrow();
                                    let rows: Vec<_> = samples.iter().enumerate().map(|(index, sample)| {
                                        let (row, spin) = &rectangles[index];
                                        let mut record = rect_json(row.get().expect("row painted"));
                                        record["spin"] = if sample["fullscreen"] == true {
                                            rect_json(Bounds::new(gpui::point(px(0.), px(0.)), size(px(740.), px(1000.))))
                                        } else { rect_json(spin.get().expect("Spin painted")) };
                                        record
                                    }).collect();
                                    metrics.push(json!({"name":name,"scale":window.scale_factor(),"imageWidth":image.width(),"imageHeight":image.height(),"rows":rows}));
                                }).unwrap();
                            }
                            println!("Captured native Spin {scene}/{theme}/{direction}");
                        }
                    }
                }
                std::fs::write(output.join("native-metrics.json"), serde_json::to_vec_pretty(&metrics).unwrap()).unwrap();
                cx.update(|_, cx| cx.quit()).unwrap();
            }).detach();
            root
        }).expect("hidden Spin capture window");
    });
}
