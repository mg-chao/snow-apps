// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
//! Hidden native GPU captures of the shared genuine-upstream Descriptions cases.
use ant_design_gpui::{
    ConfigPatch, ConfigScope, DescriptionItem, Descriptions, DescriptionsBreakpoints,
    DescriptionsColumns, DescriptionsLayout, DescriptionsSpan, Direction, Size, UiRoot,
    descriptions::{
        DescriptionItemStyles, DescriptionsStyles, DescriptionsTheme, DescriptionsTokenPatch,
    },
    theme::{AlgorithmChain, Color, LineType},
};
use gpui::{
    AnyElement, App, AppContext, Bounds, Context, Element, ElementId, GlobalElementId,
    InspectorElementId, IntoElement, LayoutId, Pixels, Refineable, Render, SharedString,
    StyleRefinement, Window, WindowBounds, WindowOptions, div, prelude::*, px, size,
};
use serde_json::{Value, json};
use std::{cell::RefCell, collections::HashMap, path::PathBuf, rc::Rc, time::Duration};

mod descriptions_support;
mod behavior {
    pub(crate) use crate::descriptions_support::{paragraph, text};
}

type RootBounds = Rc<RefCell<Vec<Option<Bounds<Pixels>>>>>;
// A paint-free delegate also observes zero-height roots that produce no GPU quad.
struct ObservedRoot {
    child: AnyElement,
    bounds: RootBounds,
    index: usize,
}
impl IntoElement for ObservedRoot {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}
impl Element for ObservedRoot {
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
        self.bounds.borrow_mut()[self.index] = Some(bounds);
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
    // The uniquely titled hidden window belongs to this process only.
    unsafe {
        let handle = FindWindowW(std::ptr::null(), title.as_ptr());
        assert_ne!(handle, 0, "hidden Descriptions capture HWND");
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

fn text(value: &Value) -> Option<SharedString> {
    match value {
        Value::String(value) => Some(SharedString::from(value.clone())),
        Value::Number(value) => Some(SharedString::from(value.to_string())),
        _ => None,
    }
}
fn color(value: &Value) -> Option<Color> {
    value
        .as_str()
        .and_then(|value| u32::from_str_radix(value.trim_start_matches('#'), 16).ok())
        .map(Color::rgb)
}
fn refinement(value: &Value) -> StyleRefinement {
    let mut style = div();
    if let Some(value) = color(&value["background"]) {
        style = style.bg(value.to_hsla());
    }
    if let Some(value) = color(&value["color"]) {
        style = style.text_color(value.to_hsla());
    }
    if let Some(value) = value["width"].as_f64() {
        style = style.w(px(value as f32));
    }
    if let Some(value) = value["padding"].as_f64() {
        style = style.p(px(value as f32));
    }
    style.style().clone()
}
fn breakpoint(value: &Value) -> DescriptionsBreakpoints<usize> {
    let get = |key: &str| value[key].as_u64().map(|v| v as usize);
    DescriptionsBreakpoints {
        xs: get("xs"),
        sm: get("sm"),
        md: get("md"),
        lg: get("lg"),
        xl: get("xl"),
        xxl: get("xxl"),
        xxxl: get("xxxl"),
    }
}
fn token_patch(value: &Value) -> DescriptionsTokenPatch {
    DescriptionsTokenPatch {
        label_bg: color(&value["labelBg"]),
        label_color: color(&value["labelColor"]),
        title_color: color(&value["titleColor"]),
        title_margin_bottom: value["titleMarginBottom"].as_f64(),
        item_padding_bottom: value["itemPaddingBottom"].as_f64(),
        item_padding_end: value["itemPaddingEnd"].as_f64(),
        colon_margin_right: value["colonMarginRight"].as_f64(),
        colon_margin_left: value["colonMarginLeft"].as_f64(),
        content_color: color(&value["contentColor"]),
        extra_color: color(&value["extraColor"]),
    }
}
fn marker(component: usize, slot: usize) -> u32 {
    0x315000 + component as u32 * 1024 + slot as u32
}
fn marked(style: &mut StyleRefinement, component: usize, slot: usize) {
    style.refine(&div().bg(gpui::rgb(marker(component, slot))).style().clone());
}
fn custom_node(value: &Value, scope: ConfigScope, cx: &mut App, base: usize) -> Option<AnyElement> {
    if let Some(value) = value.as_str().filter(|text| text.contains('\n')) {
        let direction = scope.snapshot(cx).direction;
        return Some(
            div()
                .flex()
                .flex_col()
                .children(value.split('\n').enumerate().map(|(index, line)| {
                    div()
                        .id(("line", index))
                        .child(behavior::text::anywhere_paragraph_text(
                            SharedString::from(line.to_owned()),
                            direction,
                        ))
                }))
                .into_any_element(),
        );
    }
    if let Some(props) = value.get("descriptions") {
        return Some(component(props, scope, false, base, None, cx).into_any_element());
    }
    if let Some(label) = text(&value["block"]) {
        return Some(
            div()
                .w(px(value["width"].as_f64().unwrap_or(80.) as f32))
                .h(px(value["height"].as_f64().unwrap_or(22.) as f32))
                .flex()
                .items_center()
                .child(label)
                .into_any_element(),
        );
    }
    None
}
fn component(
    sample: &Value,
    mut scope: ConfigScope,
    markers: bool,
    index: usize,
    observations: Option<RootBounds>,
    cx: &mut App,
) -> Descriptions {
    if sample["tokens"].is_object() || sample["globalTokens"].is_object() {
        let mut patch = ConfigPatch::default();
        let theme = patch.theme.get_or_insert_default();
        if sample["tokens"].is_object() {
            theme
                .components
                .set::<DescriptionsTheme>(token_patch(&sample["tokens"]));
        }
        theme.seed.line_width = sample["globalTokens"]["lineWidth"].as_f64();
        theme.seed.line_type =
            sample["globalTokens"]["lineType"]
                .as_str()
                .map(|value| match value {
                    "dashed" => LineType::Dashed,
                    "dotted" => LineType::Dotted,
                    _ => LineType::Solid,
                });
        scope = scope.child(patch, cx).unwrap();
    }
    let mut result = Descriptions::new(SharedString::from(
        sample["id"].as_str().unwrap().to_owned(),
    ))
    .scope(scope.clone());
    if let Some(value) = text(&sample["title"]) {
        if !value.is_empty()
            && value
                .bytes()
                .all(|byte| matches!(byte, b' ' | b'\t' | b'\r' | b'\n' | 0x0c))
            && let Some(bounds) = observations.clone()
        {
            let direction = scope.snapshot(cx).direction;
            result = result.title_content(ObservedRoot {
                child: behavior::text::normal_paragraph_text(value, direction),
                bounds,
                index: index * 3 + 1,
            });
        } else {
            result = result.title(value);
        }
    }
    if let Some(value) = text(&sample["extra"]) {
        if !value.is_empty()
            && value
                .bytes()
                .all(|byte| matches!(byte, b' ' | b'\t' | b'\r' | b'\n' | 0x0c))
            && let Some(bounds) = observations
        {
            let direction = scope.snapshot(cx).direction;
            result = result.extra_content(ObservedRoot {
                child: behavior::text::normal_paragraph_text(value, direction),
                bounds,
                index: index * 3 + 2,
            });
        } else {
            result = result.extra(value);
        }
    }
    result = result
        .bordered(sample["bordered"] == true)
        .colon(sample["colon"] != false)
        .layout(if sample["layout"] == "vertical" {
            DescriptionsLayout::Vertical
        } else {
            DescriptionsLayout::Horizontal
        });
    if let Some(value) = sample["size"].as_str() {
        result = result.size(match value {
            "small" => Size::Small,
            "medium" => Size::Medium,
            _ => Size::Large,
        });
    }
    if let Some(value) = sample["column"].as_u64() {
        result = result.columns(value as usize);
    } else if sample["column"].is_object() {
        result = result.columns(DescriptionsColumns::Responsive(breakpoint(
            &sample["column"],
        )));
    }
    let mut semantic = DescriptionsStyles {
        root: refinement(&sample["styles"]["root"]),
        header: refinement(&sample["styles"]["header"]),
        title: refinement(&sample["styles"]["title"]),
        extra: refinement(&sample["styles"]["extra"]),
        label: refinement(&sample["styles"]["label"]),
        content: refinement(&sample["styles"]["content"]),
    };
    if markers {
        marked(&mut semantic.root, index, 0);
        marked(&mut semantic.header, index, 1);
        marked(&mut semantic.title, index, 2);
        marked(&mut semantic.extra, index, 3);
    }
    result = result.styles(semantic);
    for (item_index, source) in sample["items"].as_array().unwrap().iter().enumerate() {
        let id = SharedString::from(format!(
            "{}-{}",
            sample["id"].as_str().unwrap(),
            source["key"].as_str().unwrap()
        ));
        let mut item = DescriptionItem::new(id);
        if let Some(value) = text(&source["label"]) {
            item = item.label(value);
        } else if let Some(value) = custom_node(&source["label"], scope.clone(), cx, index + 128) {
            item = item.label_content(value);
        }
        if let Some(value) = custom_node(&source["content"], scope.clone(), cx, index + 128) {
            item = item.content_element(value);
        } else if let Some(value) = text(&source["content"]) {
            item = item.content(value);
        }
        if source["span"] == "filled" {
            item = item.span(DescriptionsSpan::Filled);
        } else if let Some(value) = source["span"].as_u64() {
            item = item.span(value as usize);
        } else if source["span"].is_object() {
            item = item.span(DescriptionsSpan::Responsive(breakpoint(&source["span"])));
        }
        let mut item_styles = DescriptionItemStyles {
            label: refinement(&source["styles"]["label"]),
            content: refinement(&source["styles"]["content"]),
        };
        if markers {
            item = item.bg(gpui::rgb(marker(index, 16 + item_index * 3)));
            marked(&mut item_styles.label, index, 17 + item_index * 3);
            marked(&mut item_styles.content, index, 18 + item_index * 3);
        }
        result = result.item(item.styles(item_styles));
    }
    result
}

struct Capture {
    scope: ConfigScope,
    scenes: Value,
    scene: String,
    markers: bool,
    roots: RootBounds,
}
impl Render for Capture {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        self.roots.borrow_mut().fill(None);
        let config = self.scope.snapshot(cx);
        let sample = &self.scenes[&self.scene];
        let mut page = div()
            .w(px(sample["viewport"]["width"].as_f64().unwrap() as f32))
            .h(px(sample["viewport"]["height"].as_f64().unwrap() as f32))
            .p(px(20.))
            .flex()
            .flex_col()
            .items_start()
            .gap(px(24.))
            // The browser fixture's RTL direction and flex-end cross alignment
            // together leave its narrower wrappers at the physical left edge.
            .bg(config.theme.tokens.color_bg_container.to_hsla());
        for (index, sample) in sample["components"].as_array().unwrap().iter().enumerate() {
            let mut wrapper = div().flex_none().max_w_full();
            if sample["shrinkToFit"] != true {
                wrapper = wrapper.w(px(sample["width"].as_f64().unwrap() as f32));
            }
            let child = component(
                sample,
                self.scope.clone(),
                self.markers,
                index,
                Some(self.roots.clone()),
                cx,
            )
            .into_any_element();
            page = page.child(wrapper.child(ObservedRoot {
                child,
                bounds: self.roots.clone(),
                index: index * 3,
            }));
        }
        page
    }
}
fn find_marker(window: &Window, component: usize, slot: usize) -> Value {
    let scale = window.scale_factor();
    let expected = gpui::rgb(marker(component, slot));
    window.rendered_scene().quads.iter().find(|q|q.background.as_solid().is_some_and(|actual|{
        let actual=actual.to_rgb();(actual.r-expected.r).abs()<0.0001&&(actual.g-expected.g).abs()<0.0001&&(actual.b-expected.b).abs()<0.0001
    })).map_or(Value::Null,|q|json!({"x":q.bounds.origin.x.0/scale,"y":q.bounds.origin.y.0/scale,"width":q.bounds.size.width.0/scale,"height":q.bounds.size.height.0/scale}))
}
fn main() {
    let mut args = std::env::args().skip(1);
    let mut options = HashMap::new();
    while let Some(key) = args.next() {
        options.insert(key, args.next().expect("argument value"));
    }
    assert!(
        options
            .get("--benchmark")
            .is_none_or(|value| value != "true")
            || options.contains_key("--scene"),
        "--benchmark true requires a selected --scene"
    );
    let output = PathBuf::from(options.get("--output").expect("--output directory"));
    std::fs::create_dir_all(&output).unwrap();
    gpui_platform::application().run(move|cx:&mut App|{
        ant_design_gpui::init(cx);let scope=ConfigScope::new_window(cx);
        scope.update_patch(cx,|p|p.motion=Some(false)).unwrap();
        let scenes:Value=serde_json::from_str(include_str!("../fixtures/descriptions/visual-cases.json")).unwrap();
        let names:Vec<String>=scenes.as_object().unwrap().keys().filter(|name|options.get("--scene").is_none_or(|wanted|wanted==*name)).cloned().collect();
        assert!(!names.is_empty(),"unknown scene");
        cx.open_window(WindowOptions {window_bounds:Some(WindowBounds::Windowed(Bounds::centered(None,size(px(900.),px(1000.)),cx))),show:false,focus:false,..Default::default()},|window,cx|{
            let title=format!("Ant Design Descriptions hidden capture {}",std::process::id());window.set_window_title(&title);
            let root_count=scenes.as_object().unwrap().values().map(|scene|scene["components"].as_array().unwrap().len()).max().unwrap();
            let roots=Rc::new(RefCell::new(vec![None;root_count*3]));
            let capture=cx.new(|_|Capture {scope:scope.clone(),scenes,scene:names[0].clone(),markers:false,roots});
            let root=cx.new(|cx|UiRoot::new(scope.clone(),capture.clone(),window,cx));
            window.spawn(cx,async move|cx|{
                let mut metrics:Vec<Value>=if options.contains_key("--scene") || options.contains_key("--theme") || options.contains_key("--direction") || options.contains_key("--scale") {
                    std::fs::read(output.join("native-metrics.json")).ok().map(|bytes|serde_json::from_slice(&bytes).unwrap()).unwrap_or_default()
                }else{Vec::new()};
                cx.background_executor().timer(Duration::from_millis(150)).await;
                for scale in [1.,1.5,2.] {
                    if options.get("--scale").is_some_and(|v|v.parse::<f32>().unwrap()!=scale){continue;}
                    set_native_scale(&title,scale);
                    for scene in &names {
                        let (width,height)=cx.update(|window,cx|{
                            capture.update(cx,|c,cx|{c.scene=scene.clone();cx.notify();});
                            let scene=&capture.read(cx).scenes[scene];
                            let width=scene["viewport"]["width"].as_f64().unwrap()as f32;
                            let height=scene["viewport"]["height"].as_f64().unwrap()as f32;
                            window.resize(size(px(width),px(height)));window.refresh();window.draw(cx).clear(cx);(width,height)
                        }).unwrap();
                        cx.background_executor().timer(Duration::from_millis(50)).await;
                        cx.update(|window,cx|{window.bounds_changed(cx);
                            #[cfg(not(target_os="windows"))]
                            window.set_scale_factor(scale);
                            assert!((window.scale_factor()-scale).abs()<0.001);window.draw(cx).clear(cx);
                        }).unwrap();
                        for (theme,algorithms) in [("default",AlgorithmChain::default()),("dark",AlgorithmChain::dark()),("compact",AlgorithmChain::compact()),("dark-compact",AlgorithmChain::dark_compact())] {
                            if options.get("--theme").is_some_and(|v|v!=theme){continue;}
                            for (direction,value) in [("ltr",Direction::Ltr),("rtl",Direction::Rtl)] {
                                if options.get("--direction").is_some_and(|v|v!=direction){continue;}
                                cx.update(|_,cx|scope.update_patch(cx,|p|{p.direction=Some(value);p.theme.get_or_insert_default().algorithms=Some(algorithms.clone());}).unwrap()).unwrap();
                                cx.background_executor().timer(Duration::from_millis(20)).await;
                                cx.update(|window,cx|{
                                    window.draw(cx).clear(cx);let name=format!("{scene}-{theme}-{direction}-{}",(scale*100.)as u32);
                                    let image=window.render_to_image().expect("native GPU frame");
                                    assert_eq!(image.width(),(width*scale).round()as u32);assert_eq!(image.height(),(height*scale).round()as u32);
                                    image.save(output.join(format!("{name}.png"))).unwrap();
                                    let performance=if options.get("--benchmark").is_some_and(|value|value=="true") {
                                        let mut samples=Vec::with_capacity(30);
                                        for _ in 0..30 {
                                            let start=std::time::Instant::now();
                                            window.refresh();window.draw(cx).clear(cx);
                                            samples.push(start.elapsed().as_secs_f64()*1_000_000.);
                                        }
                                        samples.sort_by(f64::total_cmp);
                                        json!({"iterations":samples.len(),"unit":"microseconds",
                                            "median":(samples[14]+samples[15])/2.,"p95":samples[28],"samples":samples,
                                            "renderer":"production GPUI Window with native platform text system",
                                            "text_backend":if cfg!(target_os="windows"){ "DirectWrite" }else{ "platform native" },
                                            "profile":if cfg!(debug_assertions){ "debug" }else{ "release" },
                                            "timing_scope":"CPU window.refresh + window.draw + frame clear; excludes screenshots, marker passes, and GPU completion fences"})
                                    }else{Value::Null};
                                    capture.update(cx,|c,cx|{c.markers=true;cx.notify();});window.draw(cx).clear(cx);
                                    let components:Vec<Value>=capture.read(cx).scenes[scene]["components"].as_array().unwrap().iter().enumerate().map(|(index,sample)|{
                                        let items:Vec<Value>=sample["items"].as_array().unwrap().iter().enumerate().map(|(item,_)|json!({"item":item,"cell":find_marker(window,index,16+item*3),"label":find_marker(window,index,17+item*3),"content":find_marker(window,index,18+item*3)})).collect();
                                        let bounds=capture.read(cx).roots.borrow()[index*3].unwrap();
                                        let root=json!({"x":f32::from(bounds.origin.x),"y":f32::from(bounds.origin.y),"width":f32::from(bounds.size.width),"height":f32::from(bounds.size.height)});
                                        let observed=|offset:usize|capture.read(cx).roots.borrow()[index*3+offset].map_or(Value::Null,|bounds:Bounds<Pixels>|json!({"x":f32::from(bounds.origin.x),"y":f32::from(bounds.origin.y),"width":f32::from(bounds.size.width),"height":f32::from(bounds.size.height)}));
                                        let mut title=find_marker(window,index,2);let mut extra=find_marker(window,index,3);
                                        let title_observation=observed(1);let extra_observation=observed(2);
                                if title.is_null(){title=title_observation.clone();}
                                if extra.is_null(){extra=extra_observation.clone();}
                                        json!({"id":sample["id"],"root":root,"header":find_marker(window,index,1),"title":title,"extra":extra,"items":items,
                                            "content_observations":{"title":title_observation,"extra":extra_observation}})
                                    }).collect();
                                    let config=scope.snapshot(cx);
                                    let font_probes:Vec<Value>=["UserName","Telephone","Live","Remark","Address",":","Cloud Database","Automatic Renewal"].into_iter().map(|text|{
                                        let run=gpui::TextRun {len:text.len(),font:config.font.clone(),color:config.theme.tokens.color_text.to_hsla(),background_color:None,underline:None,strikethrough:None};
                                        let line=window.text_system().shape_line(text.into(),px(config.theme.tokens.font_size as f32),std::slice::from_ref(&run),None);
                                        let mut kern_font=config.font.clone();
                                        let mut features=kern_font.features.tag_value_list().to_vec();
                                        features.retain(|(tag,_)|tag!="kern");features.push(("kern".into(),1));
                                        kern_font.features=gpui::FontFeatures(std::sync::Arc::new(features));
                                        let kern_run=gpui::TextRun {font:kern_font,..run};
                                        let kern_line=window.text_system().shape_line(text.into(),px(config.theme.tokens.font_size as f32),&[kern_run],None);
                                        json!({"text":text,"width":f32::from(line.width()),"kern_width":f32::from(kern_line.width()),"font":format!("{:?}",config.font),"font_size":config.theme.tokens.font_size})
                                    }).collect();
                                    metrics.retain(|previous|previous["name"]!=name);
                                    metrics.push(json!({"name":name,"scene":scene,"theme":theme,"direction":direction,"scale":scale,"viewport":{"width":width,"height":height},
                                        "actual_viewport":{"width":f32::from(window.viewport_size().width),"height":f32::from(window.viewport_size().height)},
                                        "components":components,"font_probes":font_probes,"performance":performance}));
                                    capture.update(cx,|c,cx|{c.markers=false;cx.notify();});window.draw(cx).clear(cx);
                                }).unwrap();
                            }
                        }
                        std::fs::write(output.join("native-metrics.json"),serde_json::to_vec_pretty(&metrics).unwrap()).unwrap();
                        println!("Captured native Descriptions {scene} at {}%",(scale*100.)as u32);
                    }
                }
                cx.update(|_,cx|cx.quit()).unwrap();
            }).detach();root
        }).expect("hidden Descriptions capture window");
    });
}
