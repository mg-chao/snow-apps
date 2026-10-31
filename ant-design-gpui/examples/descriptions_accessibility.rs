// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Hidden native Descriptions accessibility fixture. Keyboard commands are
//! dispatched only to this owned GPUI window; no global keyboard input is sent.

use std::{path::PathBuf, time::Duration};

use ant_design_gpui::{
    Button, ConfigScope, DescriptionItem, Descriptions, DescriptionsBreakpoints,
    DescriptionsLayout, Direction, Input, InputState, UiRoot,
    behavior::{focus_handle, paragraph_text},
};
use gpui::{
    App, AppContext, Bounds, Context, Entity, FocusHandle, Focusable, IntoElement, KeyDownEvent,
    KeyUpEvent, Keystroke, PlatformInput, Render, Window, WindowBounds, WindowOptions, div,
    prelude::*, px, size,
};
use serde_json::{Value, json};

struct Fixture {
    scope: ConfigScope,
    stage: usize,
    editor: Entity<InputState>,
    outside_focus: FocusHandle,
    action_focus: FocusHandle,
    action_count: usize,
}

impl Render for Fixture {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let mut details = Descriptions::new("descriptions-native-a11y")
            .scope(self.scope.clone())
            .title("Account details")
            .automation_id("descriptions.main")
            .accessibility_description("Review account details and edit the alias")
            .columns(DescriptionsBreakpoints {
                xs: Some(1),
                md: Some(2),
                ..Default::default()
            })
            .bordered(matches!(self.stage, 1 | 2))
            .layout(if matches!(self.stage, 2 | 3) {
                DescriptionsLayout::Vertical
            } else {
                DescriptionsLayout::Horizontal
            });
        let order = if matches!(self.stage, 4 | 5) {
            [2, 0, 3, 1]
        } else {
            [0, 1, 2, 3]
        };
        for index in order {
            details = details.item(match index {
                0 => DescriptionItem::new("name").label("Name").content("Lin"),
                1 => DescriptionItem::new("department")
                    .label("Department")
                    .content("Research"),
                2 => DescriptionItem::new("alias")
                    .label_content(paragraph_text("rich-alias-label", "Alias", Direction::Ltr))
                    .accessibility_label("Alias")
                    .content_element(
                        Input::new("alias-editor", &self.editor)
                            .scope(self.scope.clone())
                            .w(px(160.))
                            .accessibility_label("Account alias")
                            .automation_id("descriptions.alias"),
                    ),
                _ => DescriptionItem::new("actions")
                    .label("Actions")
                    .content_element(
                        Button::new("save-account")
                            .scope(self.scope.clone())
                            .label("Save account")
                            .wave(false)
                            .focus_handle(&self.action_focus)
                            .automation_id("descriptions.save")
                            .on_click(cx.listener(|this, _, _, cx| {
                                this.action_count += 1;
                                cx.notify();
                            })),
                    ),
            });
        }
        div()
            .size_full()
            .p(px(24.))
            .flex()
            .flex_col()
            .gap(px(20.))
            .child(
                Button::new("outside-descriptions")
                    .scope(self.scope.clone())
                    .label("Outside descriptions")
                    .wave(false)
                    .focus_handle(&self.outside_focus)
                    .automation_id("descriptions.outside"),
            )
            .when(self.stage != 6, |root| root.child(details))
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
    // Only this process's uniquely titled window is queried. WM_GETOBJECT
    // activates AccessKit's normal native UIA registration for its root.
    unsafe {
        let handle = FindWindowW(std::ptr::null(), title.as_ptr());
        assert_ne!(handle, 0, "hidden Descriptions accessibility HWND");
        assert_eq!(IsWindowVisible(handle), 0, "fixture remains hidden");
        SendMessageW(handle, 0x003D, 0, -25);
    }
}

#[cfg(not(target_os = "windows"))]
fn request_native_accessibility(_: &str) {}

fn main() {
    let mut args = std::env::args().skip(1);
    let output = PathBuf::from(
        args.next()
            .expect("descriptions_accessibility <state.json> <command.json>"),
    );
    let command = PathBuf::from(args.next().expect("command.json"));
    assert!(args.next().is_none(), "unexpected fixture argument");
    if let Some(parent) = output.parent() {
        std::fs::create_dir_all(parent).expect("create fixture directory");
    }
    gpui_platform::application().run(move |cx: &mut App| {
        ant_design_gpui::init(cx);
        cx.on_window_closed(|cx, _| { if cx.windows().is_empty() { cx.quit(); } }).detach();
        cx.open_window(WindowOptions {
            window_bounds: Some(WindowBounds::Windowed(Bounds::centered(None, size(px(860.), px(640.)), cx))),
            show: false, focus: false, ..Default::default()
        }, move |window, cx| {
            let title = format!("Descriptions hidden accessibility fixture {}", std::process::id());
            window.set_window_title(&title);
            let scope = ConfigScope::new_window(cx);
            scope.update_patch(cx, |patch| patch.motion = Some(false)).unwrap();
            let editor = cx.new(|cx| {
                let mut editor = InputState::new(window, cx);
                editor.set_value("Lin", window, cx);
                editor
            });
            let fixture = cx.new(|cx| Fixture {
                scope: scope.clone(), stage: 0, editor,
                outside_focus: focus_handle(cx), action_focus: focus_handle(cx), action_count: 0,
            });
            let observation = fixture.downgrade();
            window.spawn(cx, async move |cx| {
                cx.background_executor().timer(Duration::from_millis(150)).await;
                request_native_accessibility(&title);
                let mut revision = u64::MAX;
                for _ in 0..2400 {
                    cx.background_executor().timer(Duration::from_millis(50)).await;
                    let input: Value = std::fs::read(&command).ok()
                        .and_then(|bytes| serde_json::from_slice(&bytes).ok()).unwrap_or_default();
                    if input["quit"] == true { cx.update(|_, cx| cx.quit()).unwrap(); return; }
                    if cx.update(|window, cx| {
                        let Some(fixture) = observation.upgrade() else { return; };
                        let next_revision = input["revision"].as_u64().unwrap_or(revision);
                        let new_command = next_revision != revision;
                        if new_command {
                            revision = next_revision;
                            if let Some(stage) = input["stage"].as_u64() {
                                fixture.update(cx, |view, cx| {
                                    view.stage = stage as usize;
                                    cx.notify();
                                });
                                window.resize(size(px(if stage == 4 { 520. } else { 860. }), px(640.)));
                            }
                        }
                        window.draw(cx).clear(cx);
                        if new_command {
                            let view = fixture.read(cx);
                            let focus = match input["focus"].as_str() {
                                Some("outside") => Some(view.outside_focus.clone()),
                                Some("alias") => Some(view.editor.read(cx).focus_handle(cx)),
                                Some("save") => Some(view.action_focus.clone()),
                                _ => None,
                            };
                            if let Some(focus) = focus { window.focus(&focus, cx); }
                            let key = match input["key"].as_str() {
                                Some("tab") => Some("tab"),
                                Some("shift-tab") => Some("shift-tab"),
                                Some("space") => Some("space"),
                                Some("enter") => Some("enter"),
                                _ => None,
                            };
                            if let Some(key) = key {
                                window.dispatch_event(PlatformInput::KeyDown(KeyDownEvent {
                                    keystroke: Keystroke::parse(key).unwrap(), is_held: false, prefer_character_input: false,
                                }), cx);
                                window.dispatch_event(PlatformInput::KeyUp(KeyUpEvent { keystroke: Keystroke::parse(key).unwrap() }), cx);
                            }
                            window.draw(cx).clear(cx);
                        }
                        let view = fixture.read(cx);
                        let alias_focus = view.editor.read(cx).focus_handle(cx);
                        let focused = if alias_focus.is_focused(window) { "alias" }
                            else if view.action_focus.is_focused(window) { "save" }
                            else if view.outside_focus.is_focused(window) { "outside" }
                            else { "none" };
                        let tree = window.debug_a11y_tree_json().and_then(|tree| serde_json::from_str::<Value>(&tree).ok());
                        let value = json!({
                            "process_id": std::process::id(), "title": title, "stage":view.stage,
                            "revision":revision, "hidden":true, "a11y_active":window.is_a11y_active(),
                            "focused":focused, "action_count":view.action_count,
                            "editor_value":view.editor.read(cx).value(),
                            "viewport_width":f32::from(window.viewport_size().width), "tree":tree,
                        });
                        let temporary = output.with_extension("tmp");
                        std::fs::write(&temporary, serde_json::to_vec(&value).unwrap())
                            .and_then(|_| std::fs::rename(temporary, &output)).expect("publish native accessibility state");
                    }).is_err() { return; }
                }
                cx.update(|_, cx| cx.quit()).unwrap();
            }).detach();
            cx.new(|cx| UiRoot::new(scope, fixture, window, cx))
        }).expect("open hidden Descriptions accessibility window");
    });
}
