// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Native Modal verification surface. --capture <png> renders a hidden window and exits.
use ant_design_gpui::{
    Button, ConfigScope, Direction, Modal, ModalState, UiRoot,
    modal::{
        ModalAction, ModalContent, ModalFooter, ModalKind, ModalOptions, ModalTheme,
        ModalTokenPatch,
    },
    theme::{AlgorithmChain, Color},
};
use gpui::{
    App, AppContext, Bounds, Context, Entity, IntoElement, Render, Window, WindowBounds,
    WindowOptions, div, prelude::*, px, size,
};
use std::{path::PathBuf, rc::Rc, time::Duration};

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
        assert_ne!(handle, 0, "hidden Modal capture HWND");
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

struct Surface {
    state: Entity<ModalState>,
    scope: ConfigScope,
    capture: bool,
}
impl Render for Surface {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let state = self.state.clone();
        div()
            .size_full()
            .bg(self.scope.snapshot(cx).theme.tokens.color_bg_container)
            .when(!self.capture, |d| {
                d.child(
                    div().p(px(24.)).flex().child(
                        Button::new("open")
                            .scope(self.scope.clone())
                            .label("Open Modal")
                            .primary()
                            .on_click(move |_, w, cx| {
                                state.update(cx, |s, cx| s.set_open(true, w, cx))
                            }),
                    ),
                )
            })
            .child(Modal::new("modal", &self.state, cx).scope(self.scope.clone()))
    }
}
fn main() {
    let mut args = std::env::args().skip(1);
    let mut capture = None;
    let mut dark = false;
    let mut compact = false;
    let mut rtl = false;
    let mut wireframe = false;
    let mut centered = false;
    let mut loading = false;
    let mut kind = ModalKind::Standard;
    let mut scale = None;
    let mut variant = String::from("basic");
    let mut width = 800.;
    let mut height = 600.;
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--scale" => scale = Some(args.next().expect("scale").parse::<f32>().unwrap()),
            "--variant" => variant = args.next().expect("variant"),
            "--capture" => capture = Some(PathBuf::from(args.next().expect("capture path"))),
            "--dark" => dark = true,
            "--compact" => compact = true,
            "--rtl" => rtl = true,
            "--wireframe" => wireframe = true,
            "--centered" => centered = true,
            "--loading" => loading = true,
            "--window-size" => {
                let value = args.next().expect("WIDTHxHEIGHT");
                let (w, h) = value.split_once('x').expect("WIDTHxHEIGHT");
                width = w.parse().unwrap();
                height = h.parse().unwrap();
            }
            "--kind" => {
                kind = match args.next().as_deref() {
                    Some("confirm") => ModalKind::Confirm,
                    Some("info") => ModalKind::Info,
                    Some("success") => ModalKind::Success,
                    Some("warning") => ModalKind::Warning,
                    Some("error") => ModalKind::Error,
                    Some("standard") => ModalKind::Standard,
                    _ => panic!("unknown kind"),
                }
            }
            _ => panic!("unknown argument {arg}"),
        }
    }
    gpui_platform::application().run(move |cx: &mut App| {
        ant_design_gpui::init(cx);
        let scope = ConfigScope::new_window(cx);
        scope
            .update_patch(cx, |p| {
                p.motion = Some(capture.is_none());
                p.direction = Some(if rtl { Direction::Rtl } else { Direction::Ltr });
                let theme = p.theme.get_or_insert_default();
                theme.algorithms = Some(match (dark, compact) {
                    (false, false) => AlgorithmChain::default(),
                    (true, false) => AlgorithmChain::dark(),
                    (false, true) => AlgorithmChain::compact(),
                    (true, true) => AlgorithmChain::dark_compact(),
                });
                theme.tokens.wireframe = Some(wireframe);
                if variant == "custom-token" {
                    theme.components.set::<ModalTheme>(ModalTokenPatch {
                        content_bg: Some(Color::rgb(0xfffbe6)),
                        title_color: Some(Color::rgb(0xad6800)),
                        title_font_size: Some(20.),
                        header_margin_bottom: Some(16.),
                        footer_margin_top: Some(20.),
                        ..Default::default()
                    });
                }
            })
            .unwrap();
        cx.open_window(
            WindowOptions {
                window_bounds: Some(WindowBounds::Windowed(Bounds::centered(
                    None,
                    size(px(width), px(height)),
                    cx,
                ))),
                show: capture.is_none(),
                focus: capture.is_none(),
                ..Default::default()
            },
            |w, cx| {
                let native_title =
                    format!("Ant Design GPUI Modal verification {}", std::process::id());
                w.set_window_title(&native_title);
                let state = cx.new(|cx| {
                    let mut s = ModalState::new(w, cx);
                    s.set_scope(scope.clone(), cx);
                    let body_scope = scope.clone();
                    let row_count: usize = if variant == "tall" { 30 } else { 3 };
                    let paragraph_margin = scope.snapshot(cx).theme.tokens.font_size as f32;
                    let footer_margin = if variant == "no-footer" {
                        0.
                    } else {
                        scope
                            .snapshot(cx)
                            .theme
                            .component::<ModalTheme>()
                            .footer_margin_top as f32
                    };
                    s.set_options(
                        ModalOptions {
                            kind,
                            centered: Some(centered),
                            footer: if variant == "no-footer" {
                                ModalFooter::None
                            } else {
                                ModalFooter::Default
                            },
                            width: (variant == "wide").then(|| 1000_f32.into()),
                            ok_text: (variant == "custom-token").then(|| "Continue".into()),
                            cancel_text: (variant == "custom-token").then(|| "Back".into()),
                            loading,
                            title: (variant != "no-title").then(|| {
                                ModalContent::text(if kind == ModalKind::Standard {
                                    "Basic Modal"
                                } else {
                                    "Do you want to delete these items?"
                                })
                            }),
                            content: Some(if kind == ModalKind::Standard {
                                // HTML's last paragraph margin collapses with the footer margin.
                                ModalContent::render(move |_, _, cx| {
                                    let direction = body_scope.snapshot(cx).direction;
                                    div().flex().flex_col().children((0..row_count).map(|i| {
                                        div()
                                            .id(("paragraph", i))
                                            .mb(px(if i == row_count - 1 {
                                                (paragraph_margin - footer_margin).max(0.)
                                            } else {
                                                paragraph_margin
                                            }))
                                            .child(ant_design_gpui::behavior::paragraph_text(
                                                "text",
                                                "Some contents...",
                                                direction,
                                            ))
                                    }))
                                })
                            } else {
                                ModalContent::text("Some descriptions")
                            }),
                            on_ok: Some(Rc::new(|_, _, _| ModalAction::Close)),
                            on_cancel: Some(Rc::new(|_, _, _| ModalAction::Close)),
                            ..Default::default()
                        },
                        cx,
                    );
                    s.set_open(true, w, cx);
                    s
                });
                let surface = cx.new(|_| Surface {
                    state: state.clone(),
                    scope: scope.clone(),
                    capture: capture.is_some(),
                });
                let root = cx.new(|cx| UiRoot::new(scope, surface, w, cx));
                if let Some(path) = capture {
                    w.resize(size(px(width), px(height)));
                    w.spawn(cx, async move |cx| {
                        if let Some(scale) = scale {
                            cx.background_executor()
                                .timer(Duration::from_millis(80))
                                .await;
                            set_native_scale(&native_title, scale);
                            cx.update(|w, cx| {
                                w.resize(size(px(width), px(height)));
                                w.bounds_changed(cx);
                                w.refresh();
                            })
                            .unwrap();
                        }
                        for _ in 0..3 {
                            cx.background_executor()
                                .timer(Duration::from_millis(80))
                                .await;
                            cx.update(|w, cx| w.draw(cx).clear(cx)).unwrap();
                        }
                        cx.update(|w, cx| {
                            if let Some(parent) = path.parent() {
                                std::fs::create_dir_all(parent).unwrap();
                            }
                            if let Some(scale) = scale {
                                assert!((w.scale_factor() - scale).abs() < 0.001);
                            }
                            w.render_to_image().unwrap().save(&path).unwrap();
                            let b = state.read(cx).bounds();
                            let metrics = serde_json::json!({
                                "scale": w.scale_factor(),
                                "x": f32::from(b.origin.x), "y": f32::from(b.origin.y),
                                "w": f32::from(b.size.width), "h": f32::from(b.size.height),
                            });
                            std::fs::write(
                                path.with_extension("json"),
                                serde_json::to_vec_pretty(&metrics).unwrap(),
                            )
                            .unwrap();
                            println!(
                                "Modal bounds: {:?}; scale: {}; capture: {}",
                                state.read(cx).bounds(),
                                w.scale_factor(),
                                path.display()
                            );
                            cx.quit();
                        })
                        .unwrap();
                    })
                    .detach();
                } else {
                    w.activate_window();
                }
                root
            },
        )
        .unwrap();
    });
}
