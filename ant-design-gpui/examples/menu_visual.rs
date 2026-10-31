// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Native platform captures paired with scripts/menu_reference.tsx.

use ant_design_gpui::{
    ConfigScope, Direction, Menu, UiRoot,
    icons::IconName,
    menu::{
        MenuColorTheme, MenuDivider, MenuEntry, MenuGroup, MenuItem, MenuItems, MenuMode,
        MenuState, MenuTheme, MenuTokenPatch, SubMenu,
    },
    theme::{AlgorithmChain, Color},
};
use gpui::{
    App, AppContext, Bounds, Context, Entity, IntoElement, KeyDownEvent, Keystroke, PlatformInput,
    Render, Window, WindowBounds, WindowOptions, div, prelude::*, px, size,
};
use serde_json::{Value, json};
use std::{cell::Cell, path::PathBuf, rc::Rc, time::Duration};

struct Capture {
    scope: ConfigScope,
    scenario: Value,
    state: Entity<MenuState>,
    root_bounds: Rc<Cell<Option<Bounds<gpui::Pixels>>>>,
}

fn items(kind: &str) -> MenuItems {
    if kind == "empty" {
        return MenuItems::new(std::iter::empty()).unwrap();
    }
    if kind == "empty-group" {
        return MenuItems::new([MenuGroup::new(
            "group",
            "Group",
            MenuItems::new(std::iter::empty()).unwrap(),
        )
        .into()])
        .unwrap();
    }
    if kind == "only-submenu" {
        return MenuItems::new([SubMenu::new(
            "settings",
            "Settings",
            MenuItems::new([MenuItem::new("profile", "Profile").into()]).unwrap(),
        )
        .into()])
        .unwrap();
    }
    if kind == "only-divider" {
        return MenuItems::new([MenuDivider::new("divider").into()]).unwrap();
    }
    let no_icon = kind == "no-icon";
    let item = |key, label, icon_name| {
        let item = MenuItem::new(key, label);
        if no_icon { item } else { item.icon(icon_name) }
    };
    let advanced = SubMenu::new(
        "advanced",
        "Advanced",
        MenuItems::new([
            MenuItem::new("security", "Security").into(),
            MenuItem::new("privacy", "Privacy").into(),
        ])
        .unwrap(),
    );
    let mut settings = SubMenu::new(
        "settings",
        "Settings",
        MenuItems::new([
            MenuItem::new("general", "General").into(),
            MenuItem::new("profile", "Profile").into(),
            advanced.into(),
        ])
        .unwrap(),
    );
    if !no_icon {
        settings = settings.icon(IconName::SettingOutlined);
    }
    if kind == "submenu-theme" {
        settings = settings.theme(MenuColorTheme::Light);
    }
    let mut home = item("home", "Home", IconName::MailOutlined);
    if kind == "extra" {
        home = home.extra("Ctrl+H");
    }
    let mut entries: Vec<MenuEntry> = vec![
        home.into(),
        item("projects", "Projects", IconName::AppstoreOutlined).into(),
        item("disabled", "Disabled", IconName::DesktopOutlined)
            .disabled(true)
            .into(),
        settings.into(),
        item("delete", "Delete", IconName::DeleteOutlined)
            .danger(true)
            .into(),
    ];
    if kind == "groups" {
        entries.push(MenuDivider::new("divider").into());
        entries.push(
            MenuGroup::new(
                "group",
                "Management",
                MenuItems::new([
                    MenuItem::new("team", "Team").into(),
                    MenuItem::new("billing", "Billing").into(),
                ])
                .unwrap(),
            )
            .into(),
        );
    }
    MenuItems::new(entries).unwrap()
}

impl Capture {
    fn configure(&mut self, scenario: Value, window: &mut Window, cx: &mut Context<Self>) {
        self.root_bounds.set(None);
        self.state = cx.new(|cx| {
            MenuState::new(
                items(scenario["kind"].as_str().unwrap_or("basic")),
                window,
                cx,
            )
        });
        self.scenario = scenario;
        cx.notify();
    }
}
impl Render for Capture {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let s = &self.scenario;
        let root_bounds = self.root_bounds.clone();
        let mode = s["mode"].as_str().unwrap_or("vertical");
        let selected =
            s["selected"]
                .as_str()
                .unwrap_or(if s["open"] == true { "profile" } else { "home" });
        let open = if s["open"] != true {
            vec![]
        } else if s["kind"] == "deep" {
            vec!["settings", "advanced"]
        } else {
            vec!["settings"]
        };
        let mut menu = Menu::new("menu", &self.state)
            .scope(self.scope.clone())
            .mode(match mode {
                "inline" | "collapsed" => MenuMode::Inline,
                "horizontal" => MenuMode::Horizontal,
                _ => MenuMode::Vertical,
            })
            .color_theme(if s["color_theme"] == "dark" {
                MenuColorTheme::Dark
            } else {
                MenuColorTheme::Light
            })
            .inline_collapsed(mode == "collapsed")
            .selected_keys([selected])
            .open_keys(open)
            .accessibility_label("Navigation");
        if s["kind"] == "custom-expand" {
            menu = menu.expand_icon(|context, _, _| {
                div().child(if context.expanded { "−" } else { "+" })
            });
        }
        if s["kind"] == "custom-overflow" {
            menu = menu.overflowed_indicator(|_, _, _| div().child("More"));
        }
        div()
            .size_full()
            .bg(self.scope.snapshot(cx).theme.tokens.color_bg_container)
            .child(
                div()
                    .absolute()
                    .left(px(64.))
                    .top(px(48.))
                    .w(px(s["width"].as_f64().unwrap_or(if mode == "horizontal" {
                        640.
                    } else {
                        256.
                    }) as f32))
                    .child(menu)
                    .on_children_prepainted(move |children, _, _| {
                        root_bounds.set(children.first().copied());
                    }),
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
            _ => panic!("Usage: menu_visual --output DIRECTORY [--cases JSON] [--scale 1|1.5|2]"),
        }
    }
    let output = output.expect("--output directory");
    std::fs::create_dir_all(&output).unwrap();
    let scenarios: Vec<Value> = input.map_or_else(
        || {
            vec![
                json!({"name":"inline-open","mode":"inline","open":true}),
                json!({"name":"vertical-open","mode":"vertical","open":true}),
                json!({"name":"collapsed","mode":"collapsed"}),
            ]
        },
        |file| serde_json::from_str(&std::fs::read_to_string(file).unwrap()).unwrap(),
    );
    gpui_platform::application().run(move |cx: &mut App| {
        ant_design_gpui::init(cx);
        let scope = ConfigScope::new_window(cx);
        scope.update_patch(cx, |patch| patch.motion = Some(false)).unwrap();
        cx.open_window(WindowOptions {
            window_bounds: Some(WindowBounds::Windowed(Bounds::centered(None, size(px(800.), px(500.)), cx))),
            show: false, focus: false, ..Default::default()
        }, |window, cx| {
            window.resize(size(px(800.), px(500.)));
            let title = format!("Menu visual fixture {}", std::process::id());
            window.set_window_title(&title);
            let state = cx.new(|cx| MenuState::new(items("basic"), window, cx));
            let capture = cx.new(|_| Capture { scope: scope.clone(), scenario: Value::Null, state,
                root_bounds:Rc::new(Cell::new(None)) });
            let root = cx.new(|cx| UiRoot::new(scope.clone(), capture.clone(), window, cx));
            window.spawn(cx, async move |cx| {
                cx.background_executor().timer(Duration::from_millis(150)).await;
                if let Some(scale) = scale {
                    let worker = change_native_dpi(title, scale);
                    while !worker.is_finished() { cx.background_executor().timer(Duration::from_millis(20)).await; }
                    worker.join().unwrap();
                    cx.update(|window,_| window.resize(size(px(800.),px(500.)))).unwrap();
                    cx.background_executor().timer(Duration::from_millis(100)).await;
                    cx.update(|window,_| assert_eq!(window.scale_factor(),scale)).unwrap();
                }
                let mut metrics = Vec::new();
                for scenario in scenarios {
                    cx.update(|window,cx| {
                        // Reset the preceding scenario's pointer before mounting
                        // a fresh controller; initial hover must not choose focus.
                        window.dispatch_event(PlatformInput::MouseMove(gpui::MouseMoveEvent {
                            position: gpui::point(px(790.),px(490.)), ..Default::default()
                        }),cx);
                        window.dispatch_event(PlatformInput::MouseUp(gpui::MouseUpEvent {
                            button: gpui::MouseButton::Left, position:gpui::point(px(790.),px(490.)),click_count:1,
                            ..Default::default()
                        }),cx);
                        scope.update_patch(cx, |patch| {
                            patch.direction = Some(if scenario["direction"]=="rtl" { Direction::Rtl } else { Direction::Ltr });
                            patch.theme.get_or_insert_default().algorithms = Some(match scenario["theme"].as_str() {
                                Some("dark") => AlgorithmChain::dark(), Some("compact") => AlgorithmChain::compact(),
                                Some("dark-compact") => AlgorithmChain::dark_compact(), _ => AlgorithmChain::default(),
                            });
                            let components = &mut patch.theme.get_or_insert_default().components;
                            components.remove::<MenuTheme>();
                            if scenario["tokens"]==true { components.set::<MenuTheme>(MenuTokenPatch {
                                item_height:Some(44.),item_border_radius:Some(10.),item_margin_block:Some(3.),item_margin_inline:Some(6.),
                                item_selected_bg:Some(Color::rgb(0xfff7e6)),item_selected_color:Some(Color::rgb(0xd46b08)),
                                dark_item_selected_bg:Some(Color::rgb(0x531dab)),active_bar_height:Some(4.),..Default::default()
                            }); }
                        }).unwrap();
                        capture.update(cx, |view,cx| view.configure(scenario.clone(),window,cx));
                        window.draw(cx).clear(cx);
                        window.dispatch_event(PlatformInput::MouseMove(gpui::MouseMoveEvent {
                            position: if scenario["hover"]==true { gpui::point(px(128.),px(204.)) }
                                else { gpui::point(px(790.),px(490.)) },
                            ..Default::default()
                        }),cx);
                        if scenario["pressed"].is_string() {
                            let tokens = scope.snapshot(cx).theme.component::<MenuTheme>();
                            let row = if scenario["pressed"] == "danger" { 4. } else { 0. };
                            let position = gpui::point(px(192.),px((48. + tokens.item_margin_block
                                + row*(tokens.item_height+tokens.item_margin_block) + tokens.item_height/2.)as f32));
                            window.dispatch_event(PlatformInput::MouseMove(gpui::MouseMoveEvent {position,..Default::default()}),cx);
                            window.dispatch_event(PlatformInput::MouseDown(gpui::MouseDownEvent {
                                button:gpui::MouseButton::Left,position,click_count:1,..Default::default()
                            }),cx);
                        }
                        window.refresh();
                    }).unwrap();
                    // Permit deferred popup placement and responsive measurement
                    // to settle before rendering the final deterministic scene.
                    cx.background_executor().timer(Duration::from_millis(80)).await;
                    let result = cx.update(|window,cx| {
                        if scenario["focus"]==true {
                            window.dispatch_event(PlatformInput::KeyDown(KeyDownEvent {
                                keystroke:Keystroke::parse("f24").unwrap(),is_held:false,prefer_character_input:false
                            }),cx);
                            capture.update(cx, |view,cx| view.state.update(cx, |state,cx| state.focus(window,cx)));
                        }
                        window.refresh();window.draw(cx).clear(cx);
                        let image=window.render_to_image().unwrap();
                        image.save(output.join(format!("{}.png",scenario["name"].as_str().unwrap()))).unwrap();
                        let dpi=window.scale_factor();
                        let quads:Vec<_>=window.rendered_scene().quads.iter().map(|quad|json!({
                            "x":quad.bounds.origin.x.0/dpi,"y":quad.bounds.origin.y.0/dpi,
                            "w":quad.bounds.size.width.0/dpi,"h":quad.bounds.size.height.0/dpi,
                            "background":format!("{:?}",quad.background),"border_color":format!("Solid({:?})",quad.border_color),
                            "border_widths":[quad.border_widths.top.0/dpi,quad.border_widths.right.0/dpi,quad.border_widths.bottom.0/dpi,quad.border_widths.left.0/dpi],
                            "radii":[quad.corner_radii.top_left.0/dpi,quad.corner_radii.top_right.0/dpi,quad.corner_radii.bottom_right.0/dpi,quad.corner_radii.bottom_left.0/dpi],
                        })).collect();
                        let foregrounds:Vec<_>=window.rendered_scene().monochrome_sprites.iter()
                            .map(|sprite|(sprite.bounds,sprite.color))
                            .chain(window.rendered_scene().subpixel_sprites.iter().map(|sprite|(sprite.bounds,sprite.color)))
                            .map(|(bounds,color)|json!({"x":bounds.origin.x.0/dpi,"y":bounds.origin.y.0/dpi,
                                "w":bounds.size.width.0/dpi,"h":bounds.size.height.0/dpi,
                                "color":format!("Solid({color:?})")})).collect();
                        let bounds=capture.read(cx).root_bounds.get().expect("Menu root bounds were recorded");
                        let root_bounds=json!({
                            "x":f32::from(bounds.origin.x),"y":f32::from(bounds.origin.y),
                            "w":f32::from(bounds.size.width),"h":f32::from(bounds.size.height),
                        });
                        json!({"scenario":scenario,"scale":dpi,"quads":quads,"foregrounds":foregrounds,"root_bounds":root_bounds})
                    }).unwrap();
                    metrics.push(result);
                }
                std::fs::write(output.join("native-metrics.json"),serde_json::to_string_pretty(&metrics).unwrap()+"\n").unwrap();
                println!("Captured {} native Menu cases",metrics.len());
                cx.update(|_,cx|cx.quit()).ok();
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
