// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Hidden native Notification accessibility fixture; no global input or settings changes.

use std::{path::PathBuf, time::Duration};

use ant_design_gpui::{
    Button, ConfigScope, UiRoot,
    notification::{
        Notification, NotificationHost, NotificationOptions, NotificationState, NotificationType,
    },
};
use gpui::{
    App, AppContext, Bounds, Context, Entity, FocusHandle, IntoElement, Render, Window,
    WindowBounds, WindowOptions, div, prelude::*, px, size,
};
use serde_json::{Value, json};

struct Fixture {
    scope: ConfigScope,
    notifications: Entity<NotificationState>,
    focus: FocusHandle,
    stage: usize,
    action_count: usize,
}

impl Fixture {
    fn set_stage(&mut self, stage: usize, window: &mut Window, cx: &mut Context<Self>) {
        let api = Notification::new(&self.notifications);
        if stage == 3 {
            api.destroy_all(window, cx);
        } else {
            let mut options = if stage == 4 {
                let scope = self.scope.clone();
                let fixture = cx.weak_entity();
                NotificationOptions::new("Document ready").actions(move |_, _, _| {
                    let fixture = fixture.clone();
                    div()
                        .flex()
                        .gap(px(8.))
                        .items_center()
                        .child("The document is ready")
                        .child(
                            Button::new("notification.action")
                                .scope(scope.clone())
                                .label("View document")
                                .automation_id("notification.action")
                                .on_click(move |_, _, cx| {
                                    let _ = fixture.update(cx, |fixture, cx| {
                                        fixture.action_count += 1;
                                        cx.notify();
                                    });
                                }),
                        )
                })
            } else {
                NotificationOptions::new(match stage {
                    1 => "The document was saved",
                    2 => "The document could not be saved",
                    _ => "Saving the document",
                })
            };
            if stage == 2 {
                options = options.accessibility_role(gpui::Role::Status);
            }
            api.open(
                options
                    .close_disabled(stage == 2)
                    .key("notification-main")
                    .kind(match stage {
                        1 | 4 => NotificationType::Success,
                        2 => NotificationType::Error,
                        _ => NotificationType::Info,
                    })
                    .show_progress(stage == 0)
                    .duration(if stage == 0 {
                        Duration::from_secs(86400)
                    } else {
                        Duration::ZERO
                    })
                    .automation_id("notification.main")
                    .accessibility_label(match stage {
                        1 => "Document saved",
                        2 => "Save failed",
                        4 => "Document ready",
                        _ => "Saving document",
                    })
                    .accessibility_description("Document operation feedback"),
                window,
                cx,
            );
        }
        self.stage = stage;
        cx.notify();
    }
}

impl Render for Fixture {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        NotificationHost::new(
            "notification-a11y-host",
            &self.notifications,
            div().size_full().bg(gpui::white()).p(px(24.)).child(
                Button::new("notification.outside")
                    .scope(self.scope.clone())
                    .label("Outside notification")
                    .focus_handle(&self.focus)
                    .automation_id("notification.outside"),
            ),
        )
    }
}

#[cfg(target_os = "windows")]
fn request_native_accessibility(title: &str) {
    #[link(name = "user32")]
    unsafe extern "system" {
        fn FindWindowW(class: *const u16, title: *const u16) -> isize;
        fn SendMessageW(window: isize, notification: u32, wparam: usize, lparam: isize) -> isize;
        fn IsWindowVisible(window: isize) -> i32;
    }
    let title: Vec<u16> = title.encode_utf16().chain(Some(0)).collect();
    // Ask only this fixture's hidden HWND to activate its ordinary AccessKit provider.
    unsafe {
        let handle = FindWindowW(std::ptr::null(), title.as_ptr());
        assert_ne!(handle, 0, "hidden Notification accessibility HWND");
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
            .expect("notification_accessibility <state.json> <command.json>"),
    );
    let command = PathBuf::from(args.next().expect("command.json"));
    assert!(args.next().is_none());
    if let Some(parent) = output.parent() {
        std::fs::create_dir_all(parent).unwrap();
    }
    gpui_platform::application().run(move |cx: &mut App| {
        ant_design_gpui::init(cx);
        cx.open_window(WindowOptions {
            window_bounds: Some(WindowBounds::Windowed(Bounds::centered(None, size(px(640.), px(480.)), cx))),
            show: false, focus: false, ..Default::default()
        }, move |window, cx| {
            let title = format!("Notification hidden accessibility fixture {}", std::process::id());
            window.set_window_title(&title);
            window.resize(size(px(640.), px(480.)));
            let scope = ConfigScope::new_window(cx);
            scope.update_patch(cx, |patch| patch.motion = Some(false)).unwrap();
            let state = cx.new(|cx| NotificationState::new(scope.clone(), window, cx));
            let fixture = cx.new(|cx| Fixture { scope: scope.clone(), notifications: state, focus: cx.focus_handle(), stage: usize::MAX, action_count: 0 });
            let observation = fixture.downgrade();
            let root = cx.new(|cx| UiRoot::new(scope, fixture.clone(), window, cx));
            window.spawn(cx, async move |cx| {
                cx.background_executor().timer(Duration::from_millis(150)).await;
                cx.update(|window, cx| {
                    window.draw(cx).clear(cx);
                    if let Some(fixture) = observation.upgrade() {
                        window.focus(&fixture.read(cx).focus.clone(), cx);
                    }
                }).unwrap();
                request_native_accessibility(&title);
                for _ in 0..1200 {
                    cx.background_executor().timer(Duration::from_millis(50)).await;
                    let input: Value = std::fs::read(&command).ok().and_then(|bytes| serde_json::from_slice(&bytes).ok()).unwrap_or_default();
                    if input["quit"] == true { cx.update(|_, cx| cx.quit()).unwrap(); return; }
                    if cx.update(|window, cx| {
                        let Some(fixture) = observation.upgrade() else { return; };
                        let stage = input["stage"].as_u64().unwrap_or(0) as usize;
                        if fixture.read(cx).stage != stage { fixture.update(cx, |view, cx| view.set_stage(stage, window, cx)); }
                        window.draw(cx).clear(cx);
                        let tree = window.debug_a11y_tree_json().and_then(|tree| serde_json::from_str::<Value>(&tree).ok());
                        let value = json!({"process_id":std::process::id(),"title":title,"stage":stage,"hidden":true,
                            "a11y_active":window.is_a11y_active(),"focus_preserved":fixture.read(cx).focus.is_focused(window),
                            "active_notifications":Notification::new(&fixture.read(cx).notifications).len(cx),"action_count":fixture.read(cx).action_count,"tree":tree});
                        let temporary = output.with_extension("tmp");
                        std::fs::write(&temporary, serde_json::to_vec(&value).unwrap()).and_then(|_| std::fs::rename(temporary, &output)).unwrap();
                    }).is_err() { return; }
                }
                cx.update(|_, cx| cx.quit()).unwrap();
            }).detach();
            root
        }).expect("hidden Notification accessibility window");
    });
}
