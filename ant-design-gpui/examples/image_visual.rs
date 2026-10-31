// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
//! Hidden native GPU frames sharing inputs with genuine upstream Image renders.
use ant_design_gpui::{
    ConfigPatch, ConfigScope, Direction, Image, ImageState, PreviewGroup, UiRoot,
    image::{
        ImageCoverPlacement, ImageProgress, ImageTheme, ImageTokenPatch, PictureStatus,
        PreviewAction, PreviewItem, PreviewOptions, PreviewState, PreviewTransform,
    },
    theme::{AlgorithmChain, Color},
};
use gpui::{
    AnyElement, App, AppContext, Bounds, Context, Element, ElementId, Entity, GlobalElementId,
    InspectorElementId, IntoElement, LayoutId, MouseButton, MouseDownEvent, MouseMoveEvent,
    MouseUpEvent, Pixels, PlatformInput, Render, Window, WindowBounds, WindowOptions, div, point,
    prelude::*, px, size,
};
use serde_json::{Value, json};
use std::{cell::RefCell, collections::BTreeMap, path::PathBuf, rc::Rc, time::Duration};

type Measurements = Rc<RefCell<BTreeMap<String, Bounds<Pixels>>>>;

/// Delegate the original element without adding a layout box or changing paint.
struct Measured {
    child: AnyElement,
    key: String,
    measurements: Measurements,
}
impl IntoElement for Measured {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}
impl Element for Measured {
    type RequestLayoutState = ();
    type PrepaintState = ();
    fn id(&self) -> Option<ElementId> {
        None
    }
    fn source_location(&self) -> Option<&'static std::panic::Location<'static>> {
        None
    }
    fn request_layout(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        window: &mut Window,
        cx: &mut App,
    ) -> (LayoutId, ()) {
        (self.child.request_layout(window, cx), ())
    }
    fn prepaint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        _: &mut (),
        window: &mut Window,
        cx: &mut App,
    ) {
        self.measurements
            .borrow_mut()
            .insert(self.key.clone(), bounds);
        self.child.prepaint(window, cx);
    }
    fn paint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        _: Bounds<Pixels>,
        _: &mut (),
        _: &mut (),
        window: &mut Window,
        cx: &mut App,
    ) {
        self.child.paint(window, cx);
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
    // Only this process's uniquely named hidden window receives WM_DPICHANGED.
    // Call outside App::update so native callbacks can borrow the application.
    unsafe {
        let handle = FindWindowW(std::ptr::null(), title.as_ptr());
        assert_ne!(handle, 0, "hidden Image capture HWND");
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

fn source(name: &str) -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("resources/image")
        .join(format!("{name}.png"))
}
fn action(name: &str) -> PreviewAction {
    match name {
        "flipX" => PreviewAction::FlipX,
        "flipY" => PreviewAction::FlipY,
        "rotateLeft" => PreviewAction::RotateLeft,
        "rotateRight" => PreviewAction::RotateRight,
        "zoomOut" => PreviewAction::ZoomOut,
        "zoomIn" => PreviewAction::ZoomIn,
        _ => panic!("unknown fixture action {name}"),
    }
}

struct Capture {
    scope: ConfigScope,
    active_scope: ConfigScope,
    scene: Value,
    state: Entity<PreviewState>,
    images: Vec<Entity<ImageState>>,
    generation: usize,
    measurements: Measurements,
}
impl Capture {
    fn configure(&mut self, sample: &Value, window: &mut Window, cx: &mut Context<Self>) {
        self.state
            .update(cx, |state, cx| state.set_open(false, window, cx));
        self.generation += 1;
        self.scene = sample.clone();
        self.measurements.borrow_mut().clear();
        let mut patch = ConfigPatch::default();
        if let Some(tokens) = sample["tokens"].as_object() {
            patch
                .theme
                .get_or_insert_default()
                .components
                .set::<ImageTheme>(ImageTokenPatch {
                    preview_operation_size: tokens
                        .get("previewOperationSize")
                        .and_then(Value::as_f64)
                        .map(|value| value as f32),
                    preview_operation_color: tokens
                        .get("previewOperationColor")
                        .and_then(Value::as_str)
                        .map(|text| text.parse::<Color>().unwrap()),
                    preview_operation_hover_color: tokens
                        .get("previewOperationHoverColor")
                        .and_then(Value::as_str)
                        .map(|text| text.parse::<Color>().unwrap()),
                    preview_operation_color_disabled: tokens
                        .get("previewOperationColorDisabled")
                        .and_then(Value::as_str)
                        .map(|text| text.parse::<Color>().unwrap()),
                    ..Default::default()
                });
        }
        self.active_scope = self.scope.child(patch, cx).unwrap();
        self.images = sample["images"]
            .as_array()
            .map(|items| {
                items
                    .iter()
                    .map(|item| {
                        let state = cx.new(|cx| {
                            if let Some(name) = item["src"].as_str() {
                                ImageState::new(source(name), window, cx)
                            } else {
                                ImageState::empty(window, cx)
                            }
                        });
                        state.update(cx, |state, cx| {
                            state.set_scope(self.active_scope.clone(), cx);
                            if let Some(name) = item["fallback"].as_str() {
                                state.set_fallback(Some(source(name).into()), cx);
                            }
                        });
                        state
                    })
                    .collect()
            })
            .unwrap_or_default();
        let items = sample
            .get("preview")
            .map(|preview| {
                let names: Vec<_> = preview["items"]
                    .as_array()
                    .map(|items| items.iter().map(|name| name.as_str().unwrap()).collect())
                    .unwrap_or_else(|| vec![preview["src"].as_str().unwrap()]);
                names
                    .into_iter()
                    .enumerate()
                    .map(|(index, name)| {
                        PreviewItem::new(format!("fixture-{index}"), source(name))
                            .alt("Preview fixture")
                    })
                    .collect()
            })
            .unwrap_or_default();
        self.state = cx.new(|cx| PreviewState::new(items, window, cx));
        let image_measurements = self.measurements.clone();
        let action_measurements = self.measurements.clone();
        self.state.update(cx, |state, cx| {
            state.set_scope(self.active_scope.clone(), cx);
            state.set_options(
                PreviewOptions {
                    mask: sample["preview"]["mask"].as_bool().unwrap_or(true),
                    focus_trap: false,
                    show_count: (!sample["preview"]["items"].is_array()).then_some(false),
                    image_render: Some(Rc::new(move |original, _, _, _| {
                        Measured {
                            child: original,
                            key: "preview.image".into(),
                            measurements: image_measurements.clone(),
                        }
                        .into_any_element()
                    })),
                    actions_render: Some(Rc::new(move |original, _, _, _| {
                        Measured {
                            child: original,
                            key: "preview.actions".into(),
                            measurements: action_measurements.clone(),
                        }
                        .into_any_element()
                    })),
                    ..Default::default()
                },
                cx,
            );
            state.set_current(
                sample["preview"]["current"].as_u64().unwrap_or(0) as usize,
                cx,
            );
            if sample.get("preview").is_some() {
                state.set_open(true, window, cx);
            }
        });
        cx.notify();
    }

    fn ready(&self, window: &Window, cx: &App) -> bool {
        if self.scene.get("preview").is_some() {
            self.state.read(cx).picture().read(cx).status() == PictureStatus::Ready
                && self.measurements.borrow().contains_key("preview.image")
        } else {
            let loaded = self
                .images
                .iter()
                .zip(self.scene["images"].as_array().unwrap())
                .all(|(state, item)| {
                    !item["src"].is_string() || state.read(cx).status(cx) == PictureStatus::Ready
                });
            let progress = self.scene["images"]
                .as_array()
                .unwrap()
                .iter()
                .filter(|item| item.get("progress").is_some())
                .count();
            loaded
                && (progress == 0
                    || window.rendered_scene().polychrome_sprites.len() >= progress * 5)
        }
    }
}
impl Render for Capture {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let config = self.active_scope.snapshot(cx);
        let mut page = div()
            .w(px(self.scene["width"].as_f64().unwrap_or(800.) as f32))
            .h(px(self.scene["height"].as_f64().unwrap_or(600.) as f32))
            .p(px(24.))
            .flex()
            .items_start()
            .gap(px(16.))
            .bg(config.theme.tokens.color_bg_container.to_hsla())
            .when(config.direction == Direction::Rtl, |element| {
                element.flex_row_reverse()
            });
        for (index, item) in self.scene["images"]
            .as_array()
            .into_iter()
            .flatten()
            .enumerate()
        {
            let mut image = Image::new(
                ("visual-thumbnail", self.generation * 10 + index),
                &self.images[index],
                cx,
            )
            .scope(self.active_scope.clone())
            .alt(format!("Fixture {}", index + 1));
            if let Some(width) = item["width"].as_f64() {
                image = image.w(px(width as f32));
            }
            if let Some(height) = item["height"].as_f64() {
                image = image.h(px(height as f32));
            }
            if let Some(cover) = item["cover"].as_str() {
                let text = cover.to_owned();
                image = image
                    .cover(Rc::new(move |_, _| {
                        div().child(text.clone()).into_any_element()
                    }))
                    .cover_placement(match item["placement"].as_str() {
                        Some("top") => ImageCoverPlacement::Top,
                        Some("bottom") => ImageCoverPlacement::Bottom,
                        _ => ImageCoverPlacement::Center,
                    });
            }
            if let Some(progress) = item.get("progress") {
                let mut placeholder = ImageProgress {
                    percent: progress.as_f64().map(|value| value as f32),
                    render: (item["customProgress"] == true).then(|| {
                        Rc::new(
                            |bar: AnyElement, percent: u8, _: &mut Window, _: &mut App| {
                                div()
                                    .w_full()
                                    .flex()
                                    .flex_col()
                                    .items_center()
                                    .child(bar)
                                    .child(div().mt(px(8.)).child(format!("Generating {percent}%")))
                                    .into_any_element()
                            },
                        ) as ant_design_gpui::image::ImageProgressRenderer
                    }),
                };
                if let Some(label) = item["loadingText"].as_str() {
                    let label = label.to_owned();
                    placeholder.render = Some(Rc::new(move |_, _, _, _| {
                        div().child(label.clone()).into_any_element()
                    }));
                }
                image = image.progress(placeholder);
            }
            page = page.child(Measured {
                child: image.into_any_element(),
                key: format!("thumbnail.{index}"),
                measurements: self.measurements.clone(),
            });
        }
        PreviewGroup::new("visual-preview", &self.state).child(page)
    }
}

fn rectangle(bounds: Bounds<Pixels>) -> Value {
    json!({"x":f32::from(bounds.origin.x),"y":f32::from(bounds.origin.y),"width":f32::from(bounds.size.width),"height":f32::from(bounds.size.height)})
}
fn transformed(bounds: Bounds<Pixels>, transform: PreviewTransform) -> Value {
    let (sine, cosine) = transform.rotate.to_radians().sin_cos();
    let width = (f32::from(bounds.size.width) * cosine.abs()
        + f32::from(bounds.size.height) * sine.abs())
        * transform.scale;
    let height = (f32::from(bounds.size.width) * sine.abs()
        + f32::from(bounds.size.height) * cosine.abs())
        * transform.scale;
    let center = bounds.center();
    rectangle(Bounds::new(
        point(
            center.x + px(transform.x - width / 2.),
            center.y + px(transform.y - height / 2.),
        ),
        size(px(width), px(height)),
    ))
}

fn main() {
    let mut arguments = std::env::args().skip(1);
    assert_eq!(arguments.next().as_deref(), Some("--output"));
    let output = PathBuf::from(arguments.next().expect("capture output directory"));
    let mut selected_scene = None;
    let mut selected_theme = None;
    let mut selected_direction = None;
    let mut selected_scale = None;
    while let Some(flag) = arguments.next() {
        let value = arguments.next().expect("filter value");
        match flag.as_str() {
            "--scene" => selected_scene = Some(value),
            "--theme" => selected_theme = Some(value),
            "--direction" => selected_direction = Some(value),
            "--scale" => selected_scale = Some(value.parse::<f32>().unwrap()),
            _ => panic!("unknown argument {flag}"),
        }
    }
    std::fs::create_dir_all(&output).unwrap();
    gpui_platform::application().run(move |cx: &mut App| {
        ant_design_gpui::init(cx);
        let scope = ConfigScope::new_window(cx);
        scope.update_patch(cx, |patch| patch.motion = Some(false)).unwrap();
        cx.open_window(WindowOptions { window_bounds: Some(WindowBounds::Windowed(Bounds::centered(None, size(px(800.), px(600.)), cx))), show: false, focus: false, ..Default::default() }, |window, cx| {
            let title = format!("Ant Design Image hidden capture {}", std::process::id());
            window.set_window_title(&title);
            let state = cx.new(|cx| PreviewState::new(Vec::new(), window, cx));
            let capture = cx.new(|_| Capture { scope: scope.clone(), active_scope: scope.clone(), scene: Value::Null, state, images: Vec::new(), generation: 0, measurements: Rc::new(RefCell::new(BTreeMap::new())) });
            let root = cx.new(|cx| UiRoot::new(scope.clone(), capture.clone(), window, cx));
            window.spawn(cx, async move |cx| {
                let scenes: Value = serde_json::from_str(include_str!("../fixtures/image/visual-cases.json")).unwrap();
                let mut metrics = Vec::new();
                cx.background_executor().timer(Duration::from_millis(100)).await;
                for (scene, sample) in scenes.as_object().unwrap() {
                    if selected_scene.as_ref().is_some_and(|selected| selected != scene) { continue; }
                    for (theme, algorithms) in [("default", AlgorithmChain::default()), ("dark", AlgorithmChain::dark()), ("compact", AlgorithmChain::compact()), ("dark-compact", AlgorithmChain::dark_compact())] {
                        if selected_theme.as_deref().is_some_and(|selected| selected != theme) { continue; }
                        for (direction, direction_value) in [("ltr", Direction::Ltr), ("rtl", Direction::Rtl)] {
                            if selected_direction.as_deref().is_some_and(|selected| selected != direction) { continue; }
                            cx.update(|_, cx| scope.update_patch(cx, |patch| { patch.direction = Some(direction_value); patch.theme.get_or_insert_default().algorithms = Some(algorithms.clone()); }).unwrap()).unwrap();
                            for scale in [1., 1.5, 2.] {
                                if selected_scale.is_some_and(|selected| (selected - scale).abs() > 0.001) { continue; }
                                set_native_scale(&title, scale);
                                let width = sample["width"].as_f64().unwrap() as f32;
                                let height = sample["height"].as_f64().unwrap() as f32;
                                cx.update(|window, cx| { window.resize(size(px(width), px(height))); window.bounds_changed(cx); capture.update(cx, |view, cx| view.configure(sample, window, cx)); window.refresh(); window.draw(cx).clear(cx); }).unwrap();
                                let mut ready = false;
                                for _ in 0..300 {
                                    cx.background_executor().timer(Duration::from_millis(10)).await;
                                    ready = cx.update(|window, cx| { window.draw(cx).clear(cx); capture.read(cx).ready(window, cx) }).unwrap();
                                    if ready { break; }
                                }
                                assert!(ready, "native Image did not load {scene}");
                                cx.update(|window, cx| {
                                    #[cfg(not(target_os = "windows"))] window.set_scale_factor(scale);
                                    assert!((window.scale_factor() - scale).abs() < 0.001);
                                    let state = capture.read(cx).state.clone();
                                    for name in sample["preview"]["actions"].as_array().into_iter().flatten() { state.update(cx, |state, cx| state.apply_action(action(name.as_str().unwrap()), window, cx)); }
                                    window.draw(cx).clear(cx);
                                    if let Some(pan) = sample["preview"]["pan"].as_array() {
                                        let center = point(px(width / 2.), px(height / 2.));
                                        let target = center + point(px(pan[0].as_f64().unwrap() as f32), px(pan[1].as_f64().unwrap() as f32));
                                        window.dispatch_event(PlatformInput::MouseDown(MouseDownEvent { position: center, button: MouseButton::Left, click_count: 1, ..Default::default() }), cx);
                                        window.dispatch_event(PlatformInput::MouseMove(MouseMoveEvent { position: target, pressed_button: Some(MouseButton::Left), ..Default::default() }), cx);
                                        window.dispatch_event(PlatformInput::MouseUp(MouseUpEvent { position: target, button: MouseButton::Left, click_count: 1, ..Default::default() }), cx);
                                    }
                                    let hover = if sample["hover"] == true {
                                        let bounds = capture.read(cx).measurements.borrow()["thumbnail.0"]; bounds.center()
                                    } else { point(px(width - 2.), px(height - 2.)) };
                                    window.simulate_mouse_move(hover, cx);
                                    window.draw(cx).clear(cx);
                                }).unwrap();
                                cx.background_executor().timer(Duration::from_millis(120)).await;
                                cx.update(|window, cx| {
                                    window.refresh(); window.draw(cx).clear(cx);
                                    let name = format!("{scene}-{theme}-{direction}-{}", (scale * 100.) as u32);
                                    let image = window.render_to_image().expect("native Image GPU frame");
                                    assert_eq!((image.width(), image.height()), ((width * scale) as u32, (height * scale) as u32));
                                    image.save(output.join(format!("{name}.png"))).unwrap();
                                    let view = capture.read(cx);
                                    let measurements = view.measurements.borrow();
                                    let state = view.state.read(cx);
                                    let transform = state.transform();
                                    let rows: Vec<_> = view.images.iter().enumerate().map(|(index, _)| json!({"key":format!("image-{index}"),"root":rectangle(measurements[&format!("thumbnail.{index}")])})).collect();
                                    let preview = sample.get("preview").map(|_| json!({"image":transformed(measurements["preview.image"], transform),"actions":rectangle(measurements["preview.actions"]),"transform":{"x":transform.x,"y":transform.y,"rotate":transform.rotate,"scale":transform.scale,"flipX":transform.flip_x,"flipY":transform.flip_y}}));
                                    metrics.push(json!({"name":name,"scale":window.scale_factor(),"scene":scene,"theme":theme,"direction":direction,"viewport":[width,height],"rows":rows,"preview":preview}));
                                }).unwrap();
                            }
                            println!("Captured native Image {scene}/{theme}/{direction}");
                        }
                    }
                }
                assert!(!metrics.is_empty());
                std::fs::write(output.join("native-metrics.json"), serde_json::to_vec_pretty(&metrics).unwrap()).unwrap();
                cx.update(|_, cx| cx.quit()).unwrap();
            }).detach();
            root
        }).expect("hidden Image capture window");
    });
}
