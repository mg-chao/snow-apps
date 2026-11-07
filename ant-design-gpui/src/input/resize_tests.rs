// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;
use crate::{ConfigScope, Direction, UiRoot};
use gpui::{
    App, AppContext, Bounds, Context, Entity, Focusable, IntoElement, MouseButton, MouseDownEvent,
    MouseMoveEvent, MouseUpEvent, Pixels, PlatformInput, Point, Render, TestAppContext, Window,
    WindowHandle, div, point, prelude::*, px, size,
};

struct Host {
    scope: ConfigScope,
    editor: Entity<InputState>,
    sibling: Entity<InputState>,
    resize: TextAreaResize,
    auto_size: bool,
    disabled: bool,
    read_only: bool,
    shown: bool,
    height: Option<f32>,
    max_height: Option<f32>,
}
impl Render for Host {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        let mut textarea = TextArea::new("resize-editor", &self.editor)
            .scope(self.scope.clone())
            .rows(3)
            .disabled(self.disabled)
            .read_only(self.read_only)
            .resize(self.resize);
        if self.auto_size {
            textarea = textarea.auto_size(3, 5);
        }
        if let Some(height) = self.height {
            textarea = textarea.h(px(height));
        }
        if let Some(height) = self.max_height {
            textarea = textarea.max_h(px(height));
        }
        div()
            .w(px(320.))
            .p(px(10.))
            .when(self.shown, |root| root.child(textarea))
            .child(Input::new("resize-sibling", &self.sibling).scope(self.scope.clone()))
    }
}
struct Fixture {
    cx: TestAppContext,
    window: WindowHandle<UiRoot>,
    host: Entity<Host>,
    editor: Entity<InputState>,
    sibling: Entity<InputState>,
    scope: ConfigScope,
}
impl Fixture {
    fn new(direction: Direction) -> Self {
        let mut cx = TestAppContext::single();
        let mut entities = None;
        let window = cx.add_window(|window, cx| {
            window.activate_window();
            let scope = ConfigScope::new_window(cx);
            scope
                .update_patch(cx, |patch| {
                    patch.motion = Some(false);
                    patch.direction = Some(direction);
                })
                .unwrap();
            let editor = cx.new(|cx| InputState::new(window, cx));
            let sibling = cx.new(|cx| InputState::new(window, cx));
            let host = cx.new(|_| Host {
                scope: scope.clone(),
                editor: editor.clone(),
                sibling: sibling.clone(),
                resize: TextAreaResize::Vertical,
                auto_size: false,
                disabled: false,
                read_only: false,
                shown: true,
                height: None,
                max_height: None,
            });
            entities = Some((host.clone(), editor, sibling, scope.clone()));
            UiRoot::new(scope, host, window, cx)
        });
        let (host, editor, sibling, scope) = entities.unwrap();
        let mut fixture = Self {
            cx,
            window,
            host,
            editor,
            sibling,
            scope,
        };
        fixture.draw();
        fixture
    }
    fn draw(&mut self) {
        self.cx.run_until_parked();
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                window.draw(cx).clear(cx)
            })
            .unwrap();
        self.cx.run_until_parked();
    }
    fn app<R>(&mut self, work: impl FnOnce(&mut Window, &mut App) -> R) -> R {
        let result = self
            .cx
            .update_window(self.window.into(), |_, window, cx| work(window, cx))
            .unwrap();
        self.draw();
        result
    }
    fn configure(&mut self, work: impl FnOnce(&mut Host)) {
        self.host.update(&mut self.cx, |host, cx| {
            work(host);
            cx.notify();
        });
        self.draw();
    }
    fn frame(&mut self) -> Bounds<Pixels> {
        self.app(|window, _| {
            let scale = window.scale_factor();
            let quad = window
                .rendered_scene()
                .quads
                .iter()
                .find(|quad| {
                    quad.border_widths.top.0 > 0.
                        && quad.bounds.size.width.0 / scale > 50.
                        && quad.bounds.origin.y.0 / scale < 20.
                })
                .expect("native textarea frame");
            Bounds::new(
                point(
                    px(quad.bounds.origin.x.0 / scale),
                    px(quad.bounds.origin.y.0 / scale),
                ),
                size(
                    px(quad.bounds.size.width.0 / scale),
                    px(quad.bounds.size.height.0 / scale),
                ),
            )
        })
    }
    fn down(&mut self, position: Point<Pixels>) {
        self.app(|window, cx| {
            window.dispatch_event(
                PlatformInput::MouseDown(MouseDownEvent {
                    button: MouseButton::Left,
                    position,
                    click_count: 1,
                    ..Default::default()
                }),
                cx,
            );
        });
    }
    fn movement(&mut self, position: Point<Pixels>) {
        self.app(|window, cx| {
            window.dispatch_event(
                PlatformInput::MouseMove(MouseMoveEvent {
                    position,
                    pressed_button: Some(MouseButton::Left),
                    ..Default::default()
                }),
                cx,
            );
        });
    }
    fn up(&mut self, position: Point<Pixels>) {
        self.app(|window, cx| {
            window.dispatch_event(
                PlatformInput::MouseUp(MouseUpEvent {
                    button: MouseButton::Left,
                    position,
                    click_count: 1,
                    ..Default::default()
                }),
                cx,
            );
        });
    }
    fn drag(&mut self, delta: Point<Pixels>, rtl: bool) {
        let frame = self.frame();
        let origin = point(
            if rtl {
                frame.left() + px(3.)
            } else {
                frame.right() - px(3.)
            },
            frame.bottom() - px(3.),
        );
        self.down(origin);
        self.movement(origin + delta);
        self.up(origin + delta);
    }
}

#[test]
fn textarea_real_resize_matches_original_vertical_drag_for_disabled_read_only_and_rtl() {
    let reference: serde_json::Value = serde_json::from_str(include_str!(
        "../../fixtures/form/textarea-resize-reference.json"
    ))
    .unwrap();
    for direction in [Direction::Ltr, Direction::Rtl] {
        for state in ["enabled", "disabled", "readonly"] {
            let mut fixture = Fixture::new(direction);
            fixture.configure(|host| {
                host.disabled = state == "disabled";
                host.read_only = state == "readonly";
            });
            let before = fixture.frame();
            // UiRoot intentionally gives BODY-equivalent traversal focus an
            // explicit handle. Preserve that exact native identity, as the
            // original preserves document.body (root.rs::UiRoot::new).
            let focused = fixture.app(|window, cx| window.focused(cx));
            fixture.drag(
                point(
                    px(if direction == Direction::Rtl {
                        -48.
                    } else {
                        48.
                    }),
                    px(36.),
                ),
                direction == Direction::Rtl,
            );
            let after = fixture.frame();
            let oracle = reference["cases"]
                .as_array()
                .unwrap()
                .iter()
                .find(|case| {
                    case["config"]["theme"] == "default"
                        && case["config"]["direction"]
                            == if direction == Direction::Ltr {
                                "ltr"
                            } else {
                                "rtl"
                            }
                        && case["config"]["state"] == state
                        && case["config"].get("focus").is_none()
                        && case["before"]["direction"]
                            == if direction == Direction::Ltr {
                                "ltr"
                            } else {
                                "rtl"
                            }
                })
                .expect("executed original drag");
            let change = oracle["after"]["height"].as_f64().unwrap()
                - oracle["before"]["height"].as_f64().unwrap();
            assert_eq!(after.size.width, before.size.width);
            assert_eq!(
                after.size.height - before.size.height,
                px(change as f32),
                "{direction:?}/{state}"
            );
            fixture.app(|window, cx| assert_eq!(window.focused(cx), focused));
        }
    }
}

#[test]
fn textarea_resize_preserves_native_focus_selection_and_text_history() {
    for sibling_focus in [false, true] {
        let mut fixture = Fixture::new(Direction::Ltr);
        let editor = fixture.editor.clone();
        let selected = if sibling_focus {
            fixture.sibling.clone()
        } else {
            editor.clone()
        };
        fixture.app(|window, cx| {
            editor.update(cx, |editor, cx| {
                editor.set_value("unchanged text", window, cx)
            });
            selected.update(cx, |selected, cx| {
                selected.set_value("selected text", window, cx);
                selected.focus(window, cx);
                selected.select(
                    TextSelection {
                        anchor: 2,
                        caret: 5,
                    },
                    window,
                    cx,
                );
            });
        });
        let before = fixture.frame();
        fixture.drag(point(px(20.), px(36.)), false);
        assert_eq!(fixture.frame().size.height, before.size.height + px(36.));
        fixture.app(|window, cx| {
            assert!(selected.read(cx).focus_handle(cx).is_focused(window));
            assert_eq!(
                selected.read(cx).selection(),
                TextSelection {
                    anchor: 2,
                    caret: 5
                }
            );
            assert!(!editor.read(cx).can_undo());
        });
    }
}

#[test]
fn textarea_auto_size_and_opt_out_remove_manual_resize_and_cancel_active_capture() {
    for auto in [false, true] {
        let mut fixture = Fixture::new(Direction::Ltr);
        fixture.configure(|host| {
            host.auto_size = auto;
            if !auto {
                host.resize = TextAreaResize::None;
            }
        });
        let before = fixture.frame();
        fixture.drag(point(px(40.), px(36.)), false);
        assert_eq!(fixture.frame().size, before.size);
    }
    let mut fixture = Fixture::new(Direction::Ltr);
    let before = fixture.frame();
    let origin = point(before.right() - px(3.), before.bottom() - px(3.));
    fixture.down(origin);
    fixture.app(|window, _| assert!(window.captured_hitbox().is_some()));
    fixture.configure(|host| host.auto_size = true);
    fixture.app(|window, _| assert!(window.captured_hitbox().is_none()));
    fixture.movement(origin + point(px(0.), px(100.)));
    assert_eq!(fixture.frame().size.height, before.size.height);
}

#[test]
fn textarea_both_axes_obey_layout_constraints_and_new_declared_height_replaces_user_size() {
    let mut fixture = Fixture::new(Direction::Rtl);
    fixture.configure(|host| {
        host.resize = TextAreaResize::Both;
        host.max_height = Some(110.);
    });
    fixture.drag(point(px(60.), px(100.)), true);
    assert_eq!(fixture.frame().size, size(px(240.), px(110.)));
    fixture.drag(point(px(-400.), px(-400.)), true);
    assert_eq!(fixture.frame().size, size(px(300.), px(32.)));
    fixture.configure(|host| host.height = Some(90.));
    assert_eq!(fixture.frame().size.height, px(90.));
}

#[test]
fn textarea_resize_unmount_releases_native_capture_and_does_not_retain_editor() {
    let mut fixture = Fixture::new(Direction::Ltr);
    let editor = fixture.editor.downgrade();
    let frame = fixture.frame();
    fixture.down(point(frame.right() - px(3.), frame.bottom() - px(3.)));
    fixture.configure(|host| host.shown = false);
    fixture.app(|window, _| assert!(window.captured_hitbox().is_none()));
    drop(fixture);
    assert!(editor.upgrade().is_none());
}

#[test]
fn textarea_native_grip_matches_original_diagonals_in_both_theme_modes_and_directions() {
    for dark in [false, true] {
        for direction in [Direction::Ltr, Direction::Rtl] {
            let mut fixture = Fixture::new(direction);
            if dark {
                let scope = fixture.scope.clone();
                fixture.app(|_, cx| {
                    scope
                        .update_patch(cx, |patch| {
                            patch.theme.get_or_insert_default().algorithms =
                                Some(crate::theme::AlgorithmChain::dark())
                        })
                        .unwrap()
                });
            }
            let frame = fixture.frame();
            let (mut primary, mut highlight) = fixture.app(|window, _| {
                let scale = window.scale_factor();
                let mut primary = Vec::new();
                let mut highlight = Vec::new();
                for quad in &window.rendered_scene().quads {
                    if (quad.bounds.size.width.0 / scale - 1.).abs() > 0.01
                        || (quad.bounds.size.height.0 / scale - 1.).abs() > 0.01
                    {
                        continue;
                    }
                    let position = point(
                        px(quad.bounds.origin.x.0 / scale),
                        px(quad.bounds.origin.y.0 / scale),
                    );
                    if !frame.contains(&position) {
                        continue;
                    }
                    let offset = (
                        f32::from(position.x - frame.left()) as i32,
                        f32::from(position.y - frame.top()) as i32,
                    );
                    if quad.background
                        == gpui::Background::from(gpui::rgb(if dark { 0x080808 } else { 0x666666 }))
                    {
                        primary.push(offset);
                    }
                    if dark && quad.background == gpui::Background::from(gpui::rgb(0xa1a1a1)) {
                        highlight.push(offset);
                    }
                }
                (primary, highlight)
            });
            let mut expected = Vec::new();
            let mut light = Vec::new();
            // Pixel offsets measured independently from the original oracle's
            // default/dark PNGs (hashes are in textarea-resize-reference.json).
            let original = [
                (4, 10),
                (5, 9),
                (6, 8),
                (7, 7),
                (8, 6),
                (9, 5),
                (10, 4),
                (4, 6),
                (5, 5),
                (6, 4),
            ];
            for (index, (right, bottom)) in original.into_iter().enumerate() {
                let x = if direction == Direction::Rtl {
                    right - 1
                } else {
                    f32::from(frame.size.width) as i32 - right
                };
                let y = f32::from(frame.size.height) as i32 - bottom;
                expected.push((x, y));
                if dark && index != 0 && index != 7 {
                    light.push((x + if direction == Direction::Rtl { -1 } else { 1 }, y));
                }
            }
            primary.sort();
            highlight.sort();
            expected.sort();
            light.sort();
            assert_eq!(primary, expected, "{direction:?}/dark={dark}");
            assert_eq!(highlight, light);
        }
    }
}
