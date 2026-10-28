// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Hidden GPU captures driven by the browser/native shared Alert visual cases.
//! Run: cargo run --example alert_visual -- --output <directory>

use std::{cell::RefCell, path::PathBuf, rc::Rc, time::Duration};

use ant_design_gpui::{
    Alert, AlertType, AlertVariant, ConfigPatch, ConfigScope, Direction, UiRoot,
    alert::{AlertPadding, AlertStyles, AlertTheme, AlertTokenPatch},
    icons::IconName,
    theme::AlgorithmChain,
};
use gpui::{
    App, AppContext, Bounds, Context, FocusHandle, FontWeight, IntoElement, KeyDownEvent,
    Keystroke, MouseMoveEvent, Pixels, PlatformInput, Render, SharedString, Window, WindowBounds,
    WindowOptions, canvas, div, point, prelude::*, px, size,
};
use serde_json::Value;

type RowBounds = Rc<RefCell<Vec<Option<Bounds<Pixels>>>>>;

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
        assert_ne!(handle, 0, "hidden Alert capture HWND");
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
    close_focus: Vec<FocusHandle>,
}

impl Render for Capture {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let config = self.scope.snapshot(cx);
        let mut content = div()
            .w(px(740.))
            .h(px(1000.))
            .p(px(20.))
            .flex()
            .flex_col()
            .gap(px(12.))
            .when(config.direction == Direction::Rtl, |d| d.items_end())
            .bg(config.theme.tokens.color_bg_container.to_hsla());
        for (index, sample) in self.scenes[&self.scene]
            .as_array()
            .unwrap()
            .iter()
            .enumerate()
        {
            let mode = sample["mode"].as_str().unwrap_or("title");
            let mut alert =
                Alert::new(SharedString::from(format!("visual-{}-{index}", self.scene)))
                    .scope(if sample["tokenCase"] == true {
                        self.custom_scope.clone()
                    } else {
                        self.scope.clone()
                    })
                    .title("Alert title")
                    .close_focus_handle(&self.close_focus[index])
                    .close_accessibility_label("Close alert");
            if let Some(kind) = sample["type"].as_str() {
                alert = alert.alert_type(match kind {
                    "success" => AlertType::Success,
                    "warning" => AlertType::Warning,
                    "error" => AlertType::Error,
                    _ => AlertType::Info,
                });
            }
            if matches!(mode, "description" | "description-only") {
                alert =
                    alert.description("Additional description and information about this alert.");
            }
            if mode == "description-only" {
                alert = alert.title("");
            }
            if sample["banner"] == true {
                alert = alert.banner(true);
            }
            if let Some(show) = sample["showIcon"].as_bool() {
                alert = alert.show_icon(show);
            }
            if let Some(closable) = sample["closable"].as_bool() {
                alert = alert.closable(closable);
            }
            if sample["closeDisabled"] == true {
                alert = alert.close_disabled(true);
            }
            if sample["customIcon"] == true {
                alert = alert.icon(IconName::SmileOutlined);
            }
            if sample["closeText"] == true {
                alert = alert.close_content(div().text_color(gpui::rgb(0)).child("Dismiss"));
            }
            if sample["variant"] == "filled" {
                alert = alert.variant(AlertVariant::Filled);
            }
            if sample["action"] == true {
                let g = &config.theme.tokens;
                alert = alert.action(
                    div()
                        .h(px(24.))
                        .px(px(7.))
                        .border_1()
                        .border_color(g.color_border.to_hsla())
                        .rounded(px(4.))
                        .bg(g.color_bg_container.to_hsla())
                        .text_color(g.color_text.to_hsla())
                        .text_size(px(12.))
                        .line_height(px(22.))
                        .child("Action"),
                );
            }
            if mode == "wrap" {
                alert = alert.title("Long alert title wraps to multiple lines when the alert container is narrow enough.");
            }
            if mode == "long-word" {
                alert = alert.title("LongUnbrokenAlertContentThatMustWrapWithinItsAvailableContentWidthWithoutPushingTheCloseButtonAway");
            }
            if mode == "rich" {
                alert = alert
                    .title_content(
                        div()
                            .flex()
                            .child("Rich ")
                            .child(div().font_weight(FontWeight::BOLD).child("alert title")),
                    )
                    .description_content(
                        div()
                            .flex()
                            .child("Description with ")
                            .child(div().font_weight(FontWeight::BOLD).child("details")),
                    );
            }
            if sample["firstLine"] == true {
                alert = alert.styles(AlertStyles {
                    root: div().items_start().style().clone(),
                    icon: div().mt(px(4.)).style().clone(),
                    close: div().mt(px(5.)).style().clone(),
                    ..Default::default()
                });
            }
            let rows = self.rows.clone();
            content = content.child(
                div()
                    .relative()
                    .w(px(if matches!(mode, "wrap" | "long-word") {
                        360.
                    } else {
                        700.
                    }))
                    .flex_shrink_0()
                    .child(alert)
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
    assert!(args.next().is_none(), "unexpected argument");
    std::fs::create_dir_all(&output).expect("create output directory");
    gpui_platform::application().run(move |cx: &mut App| {
        ant_design_gpui::init(cx);
        let scope = ConfigScope::new_window(cx);
        scope.update_patch(cx, |patch| patch.motion = Some(false)).unwrap();
        let mut custom = ConfigPatch::default();
        custom.theme.get_or_insert_default().components.set::<AlertTheme>(AlertTokenPatch {
            radius: Some(5.), default_padding: Some(AlertPadding::symmetric(10.,18.)),
            with_description_padding: Some(AlertPadding::symmetric(14.,18.)), with_description_icon_size: Some(32.),
        });
        let custom_scope = scope.child(custom,cx).unwrap();
        cx.open_window(WindowOptions {
            window_bounds: Some(WindowBounds::Windowed(Bounds::centered(None,size(px(740.),px(1000.)),cx))),
            show:false, focus:false, ..Default::default()
        }, |window,cx| {
            let native_title=format!("Ant Design Alert hidden capture {}",std::process::id());
            window.set_window_title(&native_title);
            let scenes:Value=serde_json::from_str(include_str!("../fixtures/alert/visual-cases.json")).unwrap();
            let rows=Rc::new(RefCell::new(vec![None;13]));
            let close_focus=(0..13).map(|_|cx.focus_handle()).collect();
            let capture=cx.new(|_|Capture {scope:scope.clone(),custom_scope,scene:"basic".into(),scenes,rows:rows.clone(),close_focus});
            let root=cx.new(|cx|UiRoot::new(scope.clone(),capture.clone(),window,cx));
            window.spawn(cx,async move|cx| {
                let mut metrics=Vec::new();
                cx.background_executor().timer(Duration::from_millis(150)).await;
                for scene in ["basic","advanced"] {
                    cx.update(|_,cx|capture.update(cx,|c,cx|{c.scene=scene.into();cx.notify();})).unwrap();
                    for (theme,algorithms) in [("default",AlgorithmChain::default()),("dark",AlgorithmChain::dark()),("compact",AlgorithmChain::compact()),("dark-compact",AlgorithmChain::dark_compact())] {
                        for (direction,direction_value) in [("ltr",Direction::Ltr),("rtl",Direction::Rtl)] {
                            cx.update(|_,cx|scope.update_patch(cx,|p|{p.direction=Some(direction_value);p.theme.get_or_insert_default().algorithms=Some(algorithms.clone());}).unwrap()).unwrap();
                            for scale in [1.,1.5,2.] {
                                set_native_scale(&native_title,scale);
                                cx.update(|window,cx|{
                                    window.resize(size(px(740.),px(1000.)));
                                    window.refresh();window.draw(cx).clear(cx);
                                }).unwrap();
                                cx.background_executor().timer(Duration::from_millis(80)).await;
                                cx.update(|window,cx|{
                                    window.bounds_changed(cx);
                                    #[cfg(not(target_os="windows"))]
                                    window.set_scale_factor(scale);
                                    assert!((window.scale_factor()-scale).abs()<0.001,"platform DPI mismatch");
                                }).unwrap();
                                let states:&[&str]=if scene=="basic"&&direction=="ltr"&&scale==1. {&["rest","hover","focus"]}else{&["rest"]};
                                for state in states {
                                    cx.update(|window,cx|{
                                        window.blur(cx);
                                        let row=rows.borrow()[0].expect("first row painted");
                                        let target=if *state=="hover" {point(row.right()-px(18.),row.center().y)}else{point(px(1.),px(1.))};
                                        window.dispatch_event(PlatformInput::MouseMove(MouseMoveEvent {position:target,..Default::default()}),cx);
                                        if *state=="focus" {
                                            window.dispatch_event(PlatformInput::KeyDown(KeyDownEvent {keystroke:Keystroke::parse("f6").unwrap(),is_held:false,prefer_character_input:false}),cx);
                                            let handle=capture.read(cx).close_focus[0].clone();window.focus(&handle,cx);
                                        }
                                        window.draw(cx).clear(cx);
                                    }).unwrap();
                                    cx.background_executor().timer(Duration::from_millis(20)).await;
                                    cx.update(|window,cx|{
                                        window.draw(cx).clear(cx);
                                        let suffix=if *state=="rest" {String::new()}else{format!("-{state}")};
                                        let name=format!("{scene}-{theme}-{direction}-{}{suffix}",(scale*100.)as u32);
                                        let image=window.render_to_image().expect("native GPU frame");
                                        assert_eq!(image.width(),(740.*scale)as u32,"physical capture width");
                                        assert_eq!(image.height(),(1000.*scale)as u32,"physical capture height");
                                        image.save(output.join(format!("{name}.png"))).unwrap();
                                        let count=capture.read(cx).scenes[scene].as_array().unwrap().len();
                                        metrics.push(serde_json::json!({"name":name,"scale":window.scale_factor(),"imageWidth":image.width(),"imageHeight":image.height(),"rows":rows.borrow().iter().take(count).map(|b|{let b=b.unwrap();serde_json::json!({"x":f32::from(b.origin.x),"y":f32::from(b.origin.y),"width":f32::from(b.size.width),"height":f32::from(b.size.height)})}).collect::<Vec<_>>() }));
                                    }).unwrap();
                                }
                            }
                            println!("Captured native Alert {scene}/{theme}/{direction}");
                        }
                    }
                }
                std::fs::write(output.join("native-metrics.json"),serde_json::to_vec_pretty(&metrics).unwrap()).unwrap();
                cx.update(|_,cx|cx.quit()).unwrap();
            }).detach();
            root
        }).expect("hidden Alert capture window");
    });
}
