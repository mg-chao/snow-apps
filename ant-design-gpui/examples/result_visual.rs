// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
//! Hidden native GPU captures of the shared genuine-upstream Result cases.
use ant_design_gpui::{
    ConfigPatch, ConfigScope, Direction, Result, ResultStatus, UiRoot,
    icons::IconName,
    result::{ResultMargin, ResultStyles, ResultTheme, ResultTokenPatch},
    theme::AlgorithmChain,
};
use gpui::{
    AnyElement, App, AppContext, Bounds, Context, Element, ElementId, FontWeight, GlobalElementId,
    InspectorElementId, IntoElement, LayoutId, Pixels, Render, SharedString, Window, WindowBounds,
    WindowOptions, canvas, div, prelude::*, px, size,
};
use serde_json::Value;
use std::{cell::RefCell, path::PathBuf, rc::Rc, time::Duration};
type RowBounds = Rc<RefCell<Vec<Option<Bounds<Pixels>>>>>;
type ActionBounds = Rc<RefCell<Vec<Vec<Option<Bounds<Pixels>>>>>>;

/// Delegate the original child without adding a layout box or painting pixels.
struct MeasuredAction {
    child: AnyElement,
    bounds: ActionBounds,
    row: usize,
    action: usize,
}
impl IntoElement for MeasuredAction {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}
impl Element for MeasuredAction {
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
        self.bounds.borrow_mut()[self.row][self.action] = Some(bounds);
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
    // Only this process's uniquely named hidden window receives the DPI message.
    // Call outside App::update, so native resize callbacks can borrow the App.
    unsafe {
        let handle = FindWindowW(std::ptr::null(), title.as_ptr());
        assert_ne!(handle, 0, "hidden Result capture HWND");
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
    scene: String,
    scenes: Value,
    rows: RowBounds,
    markers: bool,
    action_bounds: ActionBounds,
}
impl Render for Capture {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let config = self.scope.snapshot(cx);
        let g = &config.theme.tokens;
        let mut content = div()
            .w(px(740.))
            .h(px(760.))
            .p(px(20.))
            .flex()
            .flex_col()
            .gap(px(12.))
            .when(config.direction == Direction::Rtl, |d| d.items_end())
            .bg(g.color_bg_container.to_hsla());
        for (index, sample) in self.scenes[&self.scene]
            .as_array()
            .unwrap()
            .iter()
            .enumerate()
        {
            let mut result =
                Result::new(SharedString::from(format!("result-{}-{index}", self.scene)))
                    .scope(if sample["tokenCase"] == true {
                        self.custom_scope.clone()
                    } else {
                        self.scope.clone()
                    })
                    .status(match sample["status"].as_str().unwrap_or("info") {
                        "success" => ResultStatus::Success,
                        "warning" => ResultStatus::Warning,
                        "error" => ResultStatus::Error,
                        "403" => ResultStatus::Forbidden,
                        "404" => ResultStatus::NotFound,
                        "500" => ResultStatus::InternalServerError,
                        _ => ResultStatus::Info,
                    });
            if let Some(title) = sample["title"].as_str() {
                result = result.title(SharedString::from(title.to_owned()));
            }
            if let Some(subtitle) = sample["subTitle"].as_str() {
                result = result.sub_title(SharedString::from(subtitle.to_owned()));
            }
            if sample["showIcon"] == false {
                result = result.show_icon(false);
            }
            if sample["customIcon"] == true {
                result = result.icon(IconName::SmileOutlined);
            }
            if sample["rich"] == true {
                result = result
                    .title_content(
                        div()
                            .flex()
                            .justify_center()
                            .child("Rich ")
                            .child(div().font_weight(FontWeight::BOLD).child("result title")),
                    )
                    .sub_title_content(
                        div()
                            .flex()
                            .justify_center()
                            .child("Description with ")
                            .child(div().font_weight(FontWeight::BOLD).child("details")),
                    );
            }
            for action in 0..sample["actions"].as_u64().unwrap_or(0) {
                result = result.extra(MeasuredAction {
                    child: div()
                        .w(px(if action == 0 { 72. } else { 68. }))
                        .h(px(32.))
                        .px(px(15.))
                        .flex()
                        .items_center()
                        .justify_center()
                        .border_1()
                        .border_color(g.color_border.to_hsla())
                        .rounded(px(6.))
                        .bg(g.color_bg_container.to_hsla())
                        .text_color(g.color_text.to_hsla())
                        .text_size(px(14.))
                        .line_height(px(22.))
                        .child(if action == 0 { "Action" } else { "Again" })
                        .into_any_element(),
                    bounds: self.action_bounds.clone(),
                    row: index,
                    action: action as usize,
                });
            }
            if sample["body"] == true {
                result = result.child(div().child("The content you submitted needs attention."));
            }
            if let Some(text) = sample["extraText"].as_str() {
                result = result.extra(SharedString::from(text.to_owned()));
            }
            if let Some(text) = sample["bodyText"].as_str() {
                result = result.child(SharedString::from(text.to_owned()));
            }
            if sample["semantic"] == true {
                let mut title = div().text_color(gpui::rgb(0x1890ff)).style().clone();
                title.text.font_style = Some(gpui::FontStyle::Italic);
                result = result.styles(ResultStyles {
                    root: div()
                        .border_2()
                        .border_dashed()
                        .border_color(gpui::rgb(0x1677ff))
                        .p(px(16.))
                        .style()
                        .clone(),
                    title,
                    sub_title: div().font_weight(FontWeight::BOLD).style().clone(),
                    icon: div().opacity(0.8).style().clone(),
                    extra: div().bg(gpui::rgb(0xf0f0f0)).p(px(8.)).style().clone(),
                    body: div().bg(gpui::rgb(0xfafafa)).p(px(12.)).style().clone(),
                });
            }
            if self.markers {
                result = result.styles_fn(move |_, _, _| ResultStyles {
                    root: div().bg(gpui::rgb(marker(index, 0))).style().clone(),
                    icon: div().bg(gpui::rgb(marker(index, 1))).style().clone(),
                    title: div().bg(gpui::rgb(marker(index, 2))).style().clone(),
                    sub_title: div().bg(gpui::rgb(marker(index, 3))).style().clone(),
                    extra: div().bg(gpui::rgb(marker(index, 4))).style().clone(),
                    body: div().bg(gpui::rgb(marker(index, 5))).style().clone(),
                });
            }
            let rows = self.rows.clone();
            content = content.child(
                div()
                    .relative()
                    .w(px(sample["width"].as_f64().unwrap_or(700.) as f32))
                    .flex_shrink_0()
                    .child(result)
                    .child(
                        canvas(
                            move |bounds, _, _| rows.borrow_mut()[index] = Some(bounds),
                            |_, _, _, _| {},
                        )
                        .absolute()
                        .inset_0()
                        .size_full(),
                    ),
            );
        }
        content
    }
}
fn main() {
    let mut args = std::env::args().skip(1);
    assert_eq!(args.next().as_deref(), Some("--output"));
    let output = PathBuf::from(args.next().expect("output directory"));
    let only_scene = match args.next() {
        Some(flag) => {
            assert_eq!(flag, "--scene");
            Some(args.next().expect("scene name"))
        }
        None => None,
    };
    assert!(args.next().is_none(), "unexpected argument");
    std::fs::create_dir_all(&output).unwrap();
    gpui_platform::application().run(move|cx:&mut App| {
        ant_design_gpui::init(cx);
        let scope=ConfigScope::new_window(cx);
        scope.update_patch(cx,|p|p.motion=Some(false)).unwrap();
        let mut custom=ConfigPatch::default();
        custom.theme.get_or_insert_default().components.set::<ResultTheme>(ResultTokenPatch {
            title_font_size:Some(18.),subtitle_font_size:Some(14.),icon_font_size:Some(48.),extra_margin:Some(ResultMargin::new(12.,0.,0.,0.)),
        });
        let custom_scope=scope.child(custom,cx).unwrap();
        cx.open_window(WindowOptions {
            window_bounds:Some(WindowBounds::Windowed(Bounds::centered(None,size(px(740.),px(760.)),cx))),show:false,focus:false,..Default::default()
        },|window,cx| {
            let native_title=format!("Ant Design Result hidden capture {}",std::process::id());window.set_window_title(&native_title);
            let scenes:Value=serde_json::from_str(include_str!("../fixtures/result/visual-cases.json")).unwrap();
            let scene_names:Vec<String>=scenes.as_object().unwrap().keys().filter(|scene|only_scene.as_ref().is_none_or(|wanted|wanted==*scene)).cloned().collect();
            assert!(!scene_names.is_empty(),"unknown capture scene");
            let rows=Rc::new(RefCell::new(vec![None;2]));
            let action_bounds=Rc::new(RefCell::new(vec![vec![None;2];2]));
            let capture=cx.new(|_|Capture {scope:scope.clone(),custom_scope,scene:scene_names[0].clone(),scenes,rows:rows.clone(),markers:false,action_bounds:action_bounds.clone()});
            let root=cx.new(|cx|UiRoot::new(scope.clone(),capture.clone(),window,cx));
            window.spawn(cx,async move|cx| {
                let mut metrics=Vec::new();
                let previous=if only_scene.is_some() {std::fs::read(output.join("native-metrics.json")).ok().map(|bytes|serde_json::from_slice::<Vec<Value>>(&bytes).unwrap()).unwrap_or_default()}else{Vec::new()};
                cx.background_executor().timer(Duration::from_millis(150)).await;
                for scale in [1.,1.5,2.] {
                                set_native_scale(&native_title,scale);
                                cx.update(|window,cx|{window.resize(size(px(740.),px(760.)));window.refresh();window.draw(cx).clear(cx);}).unwrap();
                                cx.background_executor().timer(Duration::from_millis(80)).await;
                                cx.update(|window,cx|{
                                    window.bounds_changed(cx);
                                    #[cfg(not(target_os="windows"))]
                                    window.set_scale_factor(scale);
                                    assert!((window.scale_factor()-scale).abs()<0.001,"platform DPI mismatch");
                                    window.draw(cx).clear(cx);
                                }).unwrap();
                    for scene in &scene_names {
                        cx.update(|_,cx|capture.update(cx,|c,cx|{c.scene=scene.clone();cx.notify();})).unwrap();
                        for (theme,algorithms) in [("default",AlgorithmChain::default()),("dark",AlgorithmChain::dark()),("compact",AlgorithmChain::compact()),("dark-compact",AlgorithmChain::dark_compact())] {
                            for (direction,value) in [("ltr",Direction::Ltr),("rtl",Direction::Rtl)] {
                                cx.update(|_,cx|scope.update_patch(cx,|p|{p.direction=Some(value);p.theme.get_or_insert_default().algorithms=Some(algorithms.clone());}).unwrap()).unwrap();
                                cx.background_executor().timer(Duration::from_millis(20)).await;
                                cx.update(|window,cx|{
                                    window.draw(cx).clear(cx);
                                    let name=format!("{scene}-{theme}-{direction}-{}",(scale*100.)as u32);
                                    let image=window.render_to_image().expect("native GPU frame");
                                    assert_eq!(image.width(),(740.*scale)as u32);assert_eq!(image.height(),(760.*scale)as u32);
                                    image.save(output.join(format!("{name}.png"))).unwrap();
                                    let count=capture.read(cx).scenes[scene].as_array().unwrap().len();
                                    capture.update(cx,|c,cx|{c.markers=true;cx.notify();});
                                    window.draw(cx).clear(cx);
                                    let measured:Vec<_>=rows.borrow().iter().take(count).enumerate().map(|(index,b)|{
                                        let b=b.unwrap();let mut row=serde_json::json!({"x":f32::from(b.origin.x),"y":f32::from(b.origin.y),"width":f32::from(b.size.width),"height":f32::from(b.size.height)});
                                        for (part,slot) in [("icon",1),("title",2),("subTitle",3),("extra",4),("body",5)] {
                                            row[part]=window.rendered_scene().quads.iter().find(|q|q.background.as_solid().is_some_and(|color|{
                                                let actual=color.to_rgb();let expected=gpui::rgb(marker(index,slot));(actual.r-expected.r).abs()<0.001&&(actual.g-expected.g).abs()<0.001&&(actual.b-expected.b).abs()<0.001
                                            })).map_or(Value::Null,|q|serde_json::json!({"x":q.bounds.origin.x.0/scale,"y":q.bounds.origin.y.0/scale,"width":q.bounds.size.width.0/scale,"height":q.bounds.size.height.0/scale}));
                                        }
                                        row["image"]=window.rendered_scene().polychrome_sprites.iter().find(|sprite|{
                                            let width=sprite.bounds.size.width.0/scale;
                                            let height=sprite.bounds.size.height.0/scale;
                                            let y=sprite.bounds.origin.y.0/scale;
                                            (250.0..=255.0).contains(&width)&&(height-294.).abs()<0.01&&y>=f32::from(b.top())&&y<f32::from(b.bottom())
                                        }).map_or(Value::Null,|sprite|serde_json::json!({"x":sprite.bounds.origin.x.0/scale,"y":sprite.bounds.origin.y.0/scale,"width":sprite.bounds.size.width.0/scale,"height":sprite.bounds.size.height.0/scale}));
                                        row["actions"]=Value::Array((0..capture.read(cx).scenes[scene][index]["actions"].as_u64().unwrap_or(0)).map(|action|{
                                            let bounds=action_bounds.borrow()[index][action as usize].expect("native action prepaint bounds");
                                            serde_json::json!({"x":f32::from(bounds.origin.x),"y":f32::from(bounds.origin.y),"width":f32::from(bounds.size.width),"height":f32::from(bounds.size.height)})
                                        }).collect());
                                        row
                                    }).collect();
                                    metrics.push(serde_json::json!({"name":name,"scale":window.scale_factor(),"imageWidth":image.width(),"imageHeight":image.height(),"rows":measured}));
                                    capture.update(cx,|c,cx|{c.markers=false;cx.notify();});
                                    window.draw(cx).clear(cx);
                                }).unwrap();
                            }
                        }
                        println!("Captured native Result {scene} at {}%",(scale*100.)as u32);
                        save_metrics(&output,&metrics,&previous);
                    }
                }
                save_metrics(&output,&metrics,&previous);
                cx.update(|_,cx|cx.quit()).unwrap();
            }).detach();
            root
        }).expect("hidden Result capture window");
    });
}
fn marker(row: usize, part: u32) -> u32 {
    0x315700 + row as u32 * 16 + part
}
fn save_metrics(output: &std::path::Path, metrics: &[Value], previous: &[Value]) {
    let merged: Vec<_> = previous
        .iter()
        .filter(|old| !metrics.iter().any(|new| old["name"] == new["name"]))
        .chain(metrics.iter())
        .collect();
    std::fs::write(
        output.join("native-metrics.json"),
        serde_json::to_vec_pretty(&merged).unwrap(),
    )
    .unwrap();
}
