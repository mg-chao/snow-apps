// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Hidden native Progress accessibility fixture. No global keyboard input or settings changes.
//! Run through scripts/test-progress-accessibility.ps1 for native UIA assertions.

use ant_design_gpui::{Button, ConfigScope, Progress, UiRoot, progress::ProgressKind};
use gpui::{
    App, AppContext, Bounds, Context, IntoElement, Render, Window, WindowBounds, WindowOptions,
    div, prelude::*, px, size,
};
use serde_json::{Value, json};
use std::{path::PathBuf, time::Duration};

struct Fixture {
    scope: ConfigScope,
    stage: usize,
}

impl Render for Fixture {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        let values = [
            25.,
            75.,
            100.,
            60.,
            50.,
            42.,
            60.,
            -10.,
            120.,
            f64::NAN,
            70.,
        ];
        let mut progress = Progress::new("progress-a11y")
            .scope(self.scope.clone())
            .percent(values[self.stage.min(10)])
            .accessibility_label("Download progress")
            .accessibility_description("Retrieving application data")
            .automation_id("progress.main");
        progress = match self.stage {
            1 => progress.kind(ProgressKind::Circle),
            2 => progress.kind(ProgressKind::Dashboard),
            3 => progress.success(30.),
            4 => progress.steps(5),
            5 => progress.format_content(|_, _, _, _| div().child("42 files")),
            6 => progress.kind(ProgressKind::Circle).size(14),
            10 => progress.show_info(false),
            _ => progress,
        };
        div()
            .size_full()
            .bg(gpui::white())
            .p(px(24.))
            .flex()
            .flex_col()
            .gap(px(20.))
            .child(
                Button::new("outside")
                    .scope(self.scope.clone())
                    .label("Outside Progress")
                    .automation_id("progress.outside"),
            )
            .when(self.stage != 11, |d| d.child(progress))
    }
}

#[cfg(target_os = "windows")]
fn request_native_accessibility(title: &str) {
    #[link(name = "user32")]
    unsafe extern "system" {
        fn FindWindowW(class: *const u16, title: *const u16) -> isize;
        fn SendMessageW(window: isize, message: u32, wparam: usize, lparam: isize) -> isize;
        fn IsWindowVisible(window: isize) -> i32;
    }
    let title: Vec<u16> = title.encode_utf16().chain(Some(0)).collect();
    // Only the fixture's uniquely titled HWND is queried. WM_GETOBJECT requests
    // the UIA root and activates AccessKit's ordinary lazy tree registration.
    unsafe {
        let handle = FindWindowW(std::ptr::null(), title.as_ptr());
        assert_ne!(handle, 0, "hidden Progress accessibility HWND");
        assert_eq!(IsWindowVisible(handle), 0, "fixture stays hidden");
        SendMessageW(handle, 0x003D, 0, -25);
    }
}

#[cfg(not(target_os = "windows"))]
fn request_native_accessibility(_: &str) {}

fn main() {
    let mut args = std::env::args().skip(1);
    let output = PathBuf::from(
        args.next()
            .expect("progress_accessibility <state.json> <command.json>"),
    );
    let command = PathBuf::from(args.next().expect("command.json"));
    assert!(args.next().is_none(), "unexpected fixture argument");
    if let Some(parent) = output.parent() {
        std::fs::create_dir_all(parent).expect("create fixture directory");
    }
    gpui_platform::application().run(move |cx: &mut App| {
        ant_design_gpui::init(cx);
        cx.on_window_closed(|cx, _| {
            if cx.windows().is_empty() {
                cx.quit();
            }
        })
        .detach();
        cx.open_window(
            WindowOptions {
                window_bounds: Some(WindowBounds::Windowed(Bounds::centered(
                    None,
                    size(px(640.), px(480.)),
                    cx,
                ))),
                show: false,
                focus: false,
                ..Default::default()
            },
            move |window, cx| {
                let title = format!(
                    "Progress hidden accessibility fixture {}",
                    std::process::id()
                );
                window.set_window_title(&title);
                window.resize(size(px(640.), px(480.)));
                let scope = ConfigScope::new_window(cx);
                scope
                    .update_patch(cx, |patch| patch.motion = Some(false))
                    .expect("valid Progress fixture config");
                let fixture = cx.new(|_| Fixture {
                    scope: scope.clone(),
                    stage: 0,
                });
                let observation = fixture.downgrade();
                window
                    .spawn(cx, async move |cx| {
                        cx.background_executor()
                            .timer(Duration::from_millis(150))
                            .await;
                        request_native_accessibility(&title);
                        for _ in 0..1200 {
                            cx.background_executor()
                                .timer(Duration::from_millis(50))
                                .await;
                            let input: Value = std::fs::read(&command)
                                .ok()
                                .and_then(|bytes| serde_json::from_slice(&bytes).ok())
                                .unwrap_or_default();
                            if input["quit"] == true {
                                cx.update(|_, cx| cx.quit()).expect("close fixture");
                                return;
                            }
                            if cx
                                .update(|window, cx| {
                                    let Some(fixture) = observation.upgrade() else {
                                        return;
                                    };
                                    let stage = input["stage"].as_u64().unwrap_or(0) as usize;
                                    if fixture.read(cx).stage != stage {
                                        fixture.update(cx, |fixture, cx| {
                                            fixture.stage = stage;
                                            cx.notify();
                                        });
                                    }
                                    window.draw(cx).clear(cx);
                                    let tree = window
                                        .debug_a11y_tree_json()
                                        .and_then(|tree| serde_json::from_str::<Value>(&tree).ok());
                                    let value = json!({
                                        "process_id":std::process::id(),"title":title,
                                        "stage":stage,"hidden":true,
                                        "a11y_active":window.is_a11y_active(),"tree":tree,
                                    });
                                    let temporary = output.with_extension("tmp");
                                    std::fs::write(&temporary, serde_json::to_vec(&value).unwrap())
                                        .and_then(|_| std::fs::rename(temporary, &output))
                                        .expect("publish native accessibility state");
                                })
                                .is_err()
                            {
                                return;
                            }
                        }
                        cx.update(|_, cx| cx.quit()).expect("fixture timeout");
                    })
                    .detach();
                cx.new(|cx| UiRoot::new(scope, fixture, window, cx))
            },
        )
        .expect("open hidden Progress accessibility window");
    });
}
