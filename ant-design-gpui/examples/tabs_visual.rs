// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Hidden native GPU captures for the independently measured reference matrix.
use ant_design_gpui::{
    ConfigScope, Direction, Size, TabItem, TabPlacement, Tabs, TabsItems, TabsState, TabsType,
    UiRoot, theme::AlgorithmChain,
};
use gpui::{
    App, AppContext, Bounds, Context, Entity, IntoElement, KeyDownEvent, Keystroke, MouseButton,
    MouseDownEvent, MouseMoveEvent, MouseUpEvent, PlatformInput, Render, Window, WindowBounds,
    WindowOptions, div, point, prelude::*, px, size,
};
use std::{path::PathBuf, time::Duration};

struct Capture {
    scope: ConfigScope,
    state: Entity<TabsState>,
    size: Size,
    kind: TabsType,
    placement: TabPlacement,
    centered: bool,
}
impl Render for Capture {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        div()
            .size_full()
            .bg(self
                .scope
                .snapshot(cx)
                .theme
                .tokens
                .color_bg_container
                .to_hsla())
            .p(px(24.))
            .child(
                Tabs::new("reference", &self.state)
                    .scope(self.scope.clone())
                    .size(self.size)
                    .kind(self.kind)
                    .placement(self.placement)
                    .centered(self.centered)
                    .w(px(480.))
                    .when(self.placement.vertical(), |tabs| tabs.h(px(180.))),
            )
    }
}
fn main() {
    let mut args = std::env::args().skip(1);
    assert_eq!(
        args.next().as_deref(),
        Some("--output"),
        "Usage: tabs_visual --output <directory>"
    );
    let output = PathBuf::from(args.next().expect("output directory"));
    assert!(args.next().is_none(), "unexpected argument");
    std::fs::create_dir_all(&output).expect("create output directory");
    gpui_platform::application().run(move |cx: &mut App| {
        ant_design_gpui::init(cx);
        let scope = ConfigScope::new_window(cx);
        scope.update_patch(cx, |patch| patch.motion = Some(false)).unwrap();
        cx.open_window(WindowOptions {
            window_bounds: Some(WindowBounds::Windowed(Bounds::centered(None, size(px(528.), px(240.)), cx))),
            show: false, focus: false, ..Default::default()
        }, |window, cx| {
            window.resize(size(px(528.), px(240.)));
            let items = TabsItems::new((1..=3).map(|index| TabItem::new(index.to_string(), format!("Tab {index}"))
                .disabled(index == 2).text(format!("Content of Tab Pane {index}")))).unwrap();
            let state = cx.new(|cx| TabsState::new(items, window, cx));
            let capture = cx.new(|_| Capture {scope: scope.clone(), state, size: Size::Medium, kind: TabsType::Line, placement: TabPlacement::Top, centered: false});
            let root = cx.new(|cx| UiRoot::new(scope.clone(), capture.clone(), window, cx));
            window.spawn(cx, async move |cx| {
                let mut metrics = Vec::new();
                cx.background_executor().timer(Duration::from_millis(150)).await;
                for (theme, algorithms) in [("default", AlgorithmChain::default()), ("dark", AlgorithmChain::dark()), ("compact", AlgorithmChain::compact()), ("dark-compact", AlgorithmChain::dark_compact())] {
                    for (direction, direction_value) in [("ltr", Direction::Ltr), ("rtl", Direction::Rtl)] {
                        cx.update(|_, cx| scope.update_patch(cx, |patch| {
                            patch.direction = Some(direction_value);
                            patch.theme.get_or_insert_default().algorithms = Some(algorithms.clone());
                        }).unwrap()).unwrap();
                        for (size_name, tab_size) in [("small", Size::Small), ("medium", Size::Medium), ("large", Size::Large)] {
                            for (kind_name, kind) in [("line", TabsType::Line), ("card", TabsType::Card), ("editable-card", TabsType::EditableCard)] {
                                for (placement_name, placement) in [("top", TabPlacement::Top), ("bottom", TabPlacement::Bottom), ("start", TabPlacement::Start), ("end", TabPlacement::End)] {
                                    cx.update(|window, cx| {
                                        capture.update(cx, |c, cx| { c.size = tab_size; c.kind = kind; c.placement = placement; c.centered = false; cx.notify(); });
                                        window.refresh(); window.draw(cx).clear(cx);
                                    }).unwrap();
                                    cx.background_executor().timer(Duration::from_millis(20)).await;
                                    let filename = format!("{theme}-{direction}-{size_name}-{kind_name}-{placement_name}.png");
                                    cx.update(|window, cx| {
                                        window.draw(cx).clear(cx);
                                        let image = window.render_to_image().expect("native GPU frame");
                                        image.save(output.join(&filename)).expect("save frame");
                                        let state = &capture.read(cx).state;
                                        metrics.push(serde_json::json!({"name":filename.trim_end_matches(".png"), "scale":window.scale_factor(),
                                            "items": (1..=3).map(|i| { let b=state.read(cx).tab_bounds(&i.to_string()).unwrap();
                                                serde_json::json!({"x":f32::from(b.origin.x),"y":f32::from(b.origin.y),"width":f32::from(b.size.width),"height":f32::from(b.size.height)}) }).collect::<Vec<_>>() }));
                                    }).unwrap();
                                    if tab_size==Size::Medium && placement==TabPlacement::Top && kind!=TabsType::EditableCard {
                                        for mode in ["hover","active","focus"] {
                                            cx.update(|window,cx| {
                                                let state=capture.read(cx).state.clone();
                                                let target=state.read(cx).tab_bounds("3").unwrap().center();
                                                let outside=point(px(4.),px(4.));
                                                window.dispatch_event(PlatformInput::MouseMove(MouseMoveEvent {position:target,..Default::default()}),cx);
                                                if mode=="active" {window.dispatch_event(PlatformInput::MouseDown(MouseDownEvent {position:target,button:MouseButton::Left,click_count:1,..Default::default()}),cx);}
                                                if mode=="focus" {
                                                    window.dispatch_event(PlatformInput::MouseMove(MouseMoveEvent {position:outside,..Default::default()}),cx);
                                                    window.dispatch_event(PlatformInput::KeyDown(KeyDownEvent {keystroke:Keystroke::parse("f6").unwrap(),is_held:false,prefer_character_input:false}),cx);
                                                    state.update(cx,|s,cx|s.focus_tab("1",window,cx));
                                                }
                                                window.draw(cx).clear(cx);
                                            }).unwrap();
                                            cx.background_executor().timer(Duration::from_millis(20)).await;
                                            let variant=filename.trim_end_matches(".png").to_owned()+"-"+mode;
                                            cx.update(|window,cx| {
                                                window.draw(cx).clear(cx);window.render_to_image().unwrap().save(output.join(format!("{variant}.png"))).unwrap();
                                                let mut record=metrics.last().unwrap().clone();record["name"]=variant.into();metrics.push(record);
                                                let outside=point(px(4.),px(4.));
                                                window.dispatch_event(PlatformInput::MouseDown(MouseDownEvent {position:outside,button:MouseButton::Left,click_count:1,..Default::default()}),cx);
                                                window.dispatch_event(PlatformInput::MouseUp(MouseUpEvent {position:outside,button:MouseButton::Left,click_count:1,..Default::default()}),cx);
                                                window.dispatch_event(PlatformInput::MouseMove(MouseMoveEvent {position:outside,..Default::default()}),cx);
                                                window.draw(cx).clear(cx);
                                            }).unwrap();
                                        }
                                    }
                                    if tab_size == Size::Medium && placement == TabPlacement::Top && kind == TabsType::Line {
                                        cx.update(|window, cx| {
                                            capture.update(cx, |c, cx| {c.centered = true; cx.notify();});
                                            window.draw(cx).clear(cx);
                                        }).unwrap();
                                        cx.background_executor().timer(Duration::from_millis(20)).await;
                                        cx.update(|window, cx| {
                                            window.draw(cx).clear(cx);
                                            let name = format!("{theme}-{direction}-centered");
                                            window.render_to_image().unwrap().save(output.join(format!("{name}.png"))).unwrap();
                                            let state = &capture.read(cx).state;
                                            metrics.push(serde_json::json!({"name": name, "scale": window.scale_factor(),
                                                "items": (1..=3).map(|i| { let b=state.read(cx).tab_bounds(&i.to_string()).unwrap();
                                                    serde_json::json!({"x":f32::from(b.origin.x),"y":f32::from(b.origin.y),"width":f32::from(b.size.width),"height":f32::from(b.size.height)}) }).collect::<Vec<_>>() }));
                                        }).unwrap();
                                    }
                                }
                            }
                        }
                        println!("Captured {theme}/{direction}");
                    }
                }
                std::fs::write(output.join("native-metrics.json"), serde_json::to_vec_pretty(&metrics).unwrap()).unwrap();
                cx.update(|_, cx| cx.quit()).unwrap();
            }).detach();
            root
        }).expect("capture window");
    });
}
