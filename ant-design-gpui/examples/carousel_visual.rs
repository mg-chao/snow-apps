// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Hidden native GPU captures corresponding to the independent upstream Carousel scenes.
//! Run: cargo run --example carousel_visual -- --output <directory>

use std::{
    cell::{Cell, RefCell},
    path::PathBuf,
    rc::Rc,
    time::Duration,
};

use ant_design_gpui::{
    Carousel, ConfigPatch, ConfigScope, Direction, UiRoot,
    carousel::{
        CarouselEffect, CarouselItems, CarouselSlide, CarouselState, CarouselTheme,
        CarouselTokenPatch, DotPlacement,
    },
    theme::{AlgorithmChain, Color},
};
use gpui::{
    A11ySubtreeBuilder, App, AppContext, Bounds, Context, Element, ElementId, Entity, FontWeight,
    GlobalElementId, InspectorElementId, IntoElement, KeyDownEvent, Keystroke, LayoutId,
    MouseMoveEvent, Pixels, PlatformInput, Render, Role, SharedString, Window, WindowBounds,
    WindowOptions, accesskit, div, point, prelude::*, px, size,
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
        assert_ne!(handle, 0, "hidden Carousel capture HWND");
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
    states: Vec<Entity<CarouselState>>,
    rows: Rows,
}

fn slides(sample: &Value, scope: &ConfigScope) -> CarouselItems {
    CarouselItems::new((0..sample["count"].as_u64().unwrap_or(4)).map(|index| {
        let layered = sample["layered"] == true;
        let height = if sample["adaptive"] == true && index % 2 == 1 {
            100.
        } else {
            160.
        };
        let scope = scope.clone();
        CarouselSlide::new(index.to_string()).content(move |_, cx| {
            let config = scope.snapshot(cx);
            let text = SharedString::from((index + 1).to_string());
            if layered {
                div()
                    .relative()
                    .w_full()
                    .h(px(160.))
                    .bg(Color::rgb(if index % 2 == 1 { 0x7aa8cc } else { 0x426e99 }))
                    .child(
                        div()
                            .absolute()
                            .left(px(40.))
                            .top(px(40.))
                            .w(px(200.))
                            .h(px(80.))
                            .line_height(px(80.))
                            .text_center()
                            .text_color(Color::rgb(0xffffff))
                            .bg(Color::rgb(0xe58d8d))
                            .child(text),
                    )
                    .into_any_element()
            } else {
                div()
                    .w_full()
                    .h(px(height))
                    .line_height(px(height))
                    .text_center()
                    .font_weight(FontWeight::BOLD)
                    .text_size(px((config.theme.tokens.font_size * 1.17) as f32))
                    .text_color(Color::rgb(0xffffff))
                    .bg(Color::rgb(0x364d79))
                    .child(text)
                    .into_any_element()
            }
        })
    }))
    .unwrap()
}

impl Capture {
    fn reset(&mut self, scene: &str, window: &mut Window, cx: &mut Context<Self>) {
        self.scene = scene.to_owned();
        self.states = self.scenes[scene]
            .as_array()
            .unwrap()
            .iter()
            .map(|sample| {
                let items = slides(sample, &self.scope);
                cx.new(|cx| {
                    #[allow(unused_mut)]
                    let mut state = CarouselState::new(items, window, cx);
                    #[cfg(feature = "test-support")]
                    state.set_test_time(Some(Duration::ZERO), cx);
                    state
                })
            })
            .collect();
        cx.notify();
    }
}

impl Render for Capture {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let config = self.scope.snapshot(cx);
        let mut page = div()
            .w(px(520.))
            .h(px(1000.))
            .p(px(20.))
            .flex()
            .flex_col()
            .gap(px(12.))
            .items_start()
            .bg(config.theme.tokens.color_bg_container.to_hsla());
        for (index, sample) in self.scenes[&self.scene]
            .as_array()
            .unwrap()
            .iter()
            .enumerate()
        {
            let carousel = Carousel::new(
                SharedString::from(format!("visual-{}-{index}", self.scene)),
                &self.states[index],
            )
            .scope(if sample["tokenCase"] == true {
                self.custom_scope.clone()
            } else {
                self.scope.clone()
            })
            .arrows(sample["arrows"].as_bool().unwrap_or(false))
            .dots(sample["dots"].as_bool().unwrap_or(true))
            .infinite(sample["infinite"].as_bool().unwrap_or(true))
            .adaptive_height(sample["adaptive"] == true)
            .when_some(sample["vertical"].as_bool(), |carousel, vertical| {
                carousel.vertical(vertical)
            })
            .initial_slide(sample["initial"].as_u64().unwrap_or(0) as usize)
            .effect(if sample["effect"] == "fade" {
                CarouselEffect::Fade
            } else {
                CarouselEffect::Scroll
            })
            .dot_placement(match sample["placement"].as_str() {
                Some("top") => DotPlacement::Top,
                Some("start") => DotPlacement::Start,
                Some("end") => DotPlacement::End,
                _ => DotPlacement::Bottom,
            })
            .autoplay(sample["progress"] == true)
            .dot_duration(sample["progress"] == true)
            .w_full()
            .when(sample["adaptive"] != true, |carousel| carousel.h(px(160.)));
            let (row_bounds, component_bounds) = self.rows.borrow()[index].clone();
            page = page.child(measured(
                div()
                    .relative()
                    .flex_none()
                    .w(px(sample["width"].as_f64().unwrap() as f32))
                    .h(px(sample["height"].as_f64().unwrap() as f32))
                    .child(measured(carousel, component_bounds)),
                row_bounds,
            ));
        }
        page
    }
}

fn rect_json(bounds: Bounds<Pixels>) -> Value {
    json!({"x":f32::from(bounds.origin.x),"y":f32::from(bounds.origin.y),
        "width":f32::from(bounds.size.width),"height":f32::from(bounds.size.height)})
}

fn frozen(states: &[Entity<CarouselState>], elapsed: u64, cx: &mut App) {
    #[cfg(feature = "test-support")]
    for state in states {
        state.update(cx, |state, cx| {
            state.set_test_time(Some(Duration::from_millis(elapsed)), cx)
        });
    }
    #[cfg(not(feature = "test-support"))]
    let _ = (states, elapsed, cx);
}

fn key(value: &'static str, window: &mut Window, cx: &mut App) {
    window.dispatch_event(
        PlatformInput::KeyDown(KeyDownEvent {
            keystroke: Keystroke::parse(value).unwrap(),
            is_held: false,
            prefer_character_input: false,
        }),
        cx,
    );
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
                let scale = value.parse::<f32>().expect("numeric device scale");
                assert!(
                    [1., 1.5, 2.].contains(&scale),
                    "supported scales: 1, 1.5, 2"
                );
                selected_scale = Some(scale);
            }
            _ => panic!("unknown capture option: {option}"),
        }
    }
    std::fs::create_dir_all(&output).expect("create output directory");
    gpui_platform::application().run(move |cx: &mut App| {
        ant_design_gpui::init(cx);
        let scope = ConfigScope::new_window(cx);
        let mut custom = ConfigPatch::default();
        custom.theme.get_or_insert_default().components.set::<CarouselTheme>(CarouselTokenPatch {
            dot_width: Some(50.), dot_height: Some(50.), dot_active_width: Some(80.), ..Default::default()
        });
        let custom_scope = scope.child(custom, cx).unwrap();
        cx.open_window(WindowOptions {
            window_bounds: Some(WindowBounds::Windowed(Bounds::centered(None, size(px(520.), px(1000.)), cx))),
            show: false, focus: false, ..Default::default()
        }, |window, cx| {
            let native_title = format!("Ant Design Carousel hidden capture {}", std::process::id());
            window.set_window_title(&native_title);
            let scenes: Value = serde_json::from_str(include_str!("../fixtures/carousel/visual-cases.json")).unwrap();
            let names: Vec<_> = scenes.as_object().unwrap().keys()
                .filter(|scene| selected_scene.as_ref().is_none_or(|selected| selected == *scene))
                .filter(|scene| cfg!(feature = "test-support") || !(scene.starts_with("motion") || scene.as_str() == "progress"))
                .cloned().collect();
            assert!(!names.is_empty(), "unknown scene or missing --features test-support");
            let count = scenes.as_object().unwrap().values().map(|scene| scene.as_array().unwrap().len()).max().unwrap();
            let rows: Rows = Rc::new(RefCell::new((0..count).map(|_| (Rc::default(), Rc::default())).collect()));
            let capture = cx.new(|cx| {
                let mut view = Capture { scope: scope.clone(), custom_scope, scenes, scene: names[0].clone(), states: Vec::new(), rows: rows.clone() };
                view.reset(&names[0], window, cx); view
            });
            let root = cx.new(|cx| UiRoot::new(scope.clone(), capture.clone(), window, cx));
            window.spawn(cx, async move |cx| {
                let mut metrics = Vec::new();
                cx.background_executor().timer(Duration::from_millis(150)).await;
                for scene in names {
                    for (theme, algorithms) in [("default", AlgorithmChain::default()), ("dark", AlgorithmChain::dark()), ("compact", AlgorithmChain::compact()), ("dark-compact", AlgorithmChain::dark_compact())] {
                        if selected_theme.as_deref().is_some_and(|selected| selected != theme) { continue; }
                        for (direction, value) in [("ltr", Direction::Ltr), ("rtl", Direction::Rtl)] {
                            if selected_direction.as_deref().is_some_and(|selected| selected != direction) { continue; }
                            cx.update(|_, cx| scope.update_patch(cx, |patch| {
                                patch.direction = Some(value);
                                patch.theme.get_or_insert_default().algorithms = Some(algorithms.clone());
                            }).unwrap()).unwrap();
                            let dynamic = scene.starts_with("motion") || scene == "progress";
                            for scale in &[1., 1.5, 2.] {
                                let scale = *scale;
                                if selected_scale.is_some_and(|selected| selected != scale) { continue; }
                                set_native_scale(&native_title, scale);
                                cx.update(|window, cx| {
                                    window.resize(size(px(520.), px(1000.))); window.refresh();
                                    window.dispatch_event(PlatformInput::MouseMove(MouseMoveEvent { position: point(px(4.), px(4.)), ..Default::default() }), cx);
                                    capture.update(cx, |view, cx| view.reset(&scene, window, cx));
                                    window.draw(cx).clear(cx);
                                }).unwrap();
                                cx.background_executor().timer(Duration::from_millis(40)).await;
                                cx.update(|window, cx| {
                                    window.bounds_changed(cx);
                                    #[cfg(not(target_os = "windows"))]
                                    window.set_scale_factor(scale);
                                    assert!((window.scale_factor() - scale).abs() < 0.001, "platform DPI mismatch");
                                    window.draw(cx).clear(cx);
                                    let states = capture.read(cx).states.clone();
                                    if scene.starts_with("motion") || scene == "adaptive" { for state in &states { state.update(cx, |state, cx| state.next(window, cx)); } }
                                    if scene == "adaptive" {
                                        // Establish the new control transition at t=0 before
                                        // sampling the settled adaptive-height layout.
                                        window.draw(cx).clear(cx);
                                        frozen(&states, 500, cx);
                                    }
                                    if scene == "hover-prev" || scene == "hover-dot" {
                                        let state = states[0].read(cx);
                                        let target = if scene == "hover-prev" { state.previous_arrow_bounds() } else { state.dot_bounds(1) }.unwrap().center();
                                        window.dispatch_event(PlatformInput::MouseMove(MouseMoveEvent { position: target, ..Default::default() }), cx);
                                        window.draw(cx).clear(cx);
                                        frozen(&states, 300, cx);
                                    } else if scene == "focus-next" {
                                        key("f6", window, cx);
                                        states[0].update(cx, |state, cx| state.focus(window, cx));
                                        key("tab", window, cx); key("tab", window, cx);
                                        window.draw(cx).clear(cx);
                                        frozen(&states, 300, cx);
                                    }
                                    window.draw(cx).clear(cx);
                                }).unwrap();
                                let phases: &[u64] = if scene.starts_with("motion") { &[0, 125, 250, 375, 500] } else if scene == "progress" { &[0, 750, 1500, 2250, 3000] } else { &[0] };
                                for phase in phases {
                                    cx.update(|window, cx| {
                                        let states = capture.read(cx).states.clone();
                                        if dynamic { frozen(&states, *phase, cx); }
                                        window.draw(cx).clear(cx);
                                        let name = format!("{scene}-{theme}-{direction}-{}{}", (scale * 100.) as u32, if dynamic { format!("-{phase}") } else { String::new() });
                                        let image = window.render_to_image().expect("native GPU frame");
                                        assert_eq!(image.width(), (520. * scale) as u32, "physical capture width");
                                        assert_eq!(image.height(), (1000. * scale) as u32, "physical capture height");
                                        image.save(output.join(format!("{name}.png"))).unwrap();
                                        let rectangles = rows.borrow();
                                        let samples: Vec<_> = states.iter().enumerate().map(|(index, state)| {
                                            let mut record = rect_json(rectangles[index].0.get().expect("row painted"));
                                            record["carousel"] = rect_json(rectangles[index].1.get().expect("Carousel painted"));
                                            let state = state.read(cx);
                                            record["currentIndex"] = state.current_index().map_or(Value::Null, |index| json!(index));
                                            record["viewport"] = state.viewport_bounds().map(rect_json).unwrap_or(Value::Null);
                                            record["arrows"] = [state.previous_arrow_bounds(), state.next_arrow_bounds()].into_iter().flatten().map(rect_json).collect();
                                            record["dots"] = (0..state.items().len()).filter_map(|index| state.dot_bounds(index).map(|bounds| {
                                                let mut dot = rect_json(bounds);
                                                dot["index"] = json!(index);
                                                dot["active"] = json!(state.current_index() == Some(index));
                                                dot
                                            })).collect();
                                            record
                                        }).collect();
                                        metrics.push(json!({"name":name,"scale":window.scale_factor(),"scene":scene,"phase":phase,"theme":theme,"direction":direction,"imageWidth":image.width(),"imageHeight":image.height(),"rows":samples}));
                                    }).unwrap();
                                }
                            }
                            println!("Captured native Carousel {scene}/{theme}/{direction}");
                        }
                    }
                }
                std::fs::write(output.join("native-metrics.json"), serde_json::to_vec_pretty(&metrics).unwrap()).unwrap();
                cx.update(|_, cx| cx.quit()).unwrap();
            }).detach();
            root
        }).expect("hidden Carousel capture window");
    });
}
