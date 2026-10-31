// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::PopupHandle;
use crate::{Button, ConfigScope, Tooltip, UiRoot};
use gpui::{
    AppContext, Bounds, Context, Entity, IntoElement, MouseButton, MouseDownEvent, MouseUpEvent,
    Pixels, PlatformInput, Render, TestAppContext, Window, WindowHandle, div, prelude::*, px,
};
use std::{cell::Cell, rc::Rc, time::Duration};

struct Consumer {
    scope: ConfigScope,
    handles: [PopupHandle; 3],
    mounted: [bool; 3],
    clicks: Rc<Cell<usize>>,
}
impl Render for Consumer {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        div()
            .flex()
            .gap(px(90.))
            .p(px(180.))
            .children((0..3).filter(|i| self.mounted[*i]).map(|i| {
                let clicks = self.clicks.clone();
                Tooltip::new(("unique", i))
                    .scope(self.scope.clone())
                    .unique(true)
                    .handle(&self.handles[i])
                    .child(Button::new(("trigger", i)).label("Hover"))
                    .title_content(move |_, _, _| {
                        let clicks = clicks.clone();
                        div()
                            .w(px(60. + i as f32 * 50.))
                            .h(px(32. + i as f32 * 20.))
                            .child(
                                Button::new(("content", i))
                                    .label(format!("Content {i}"))
                                    .on_click(move |_, _, _| clicks.set(clicks.get() + 1)),
                            )
                    })
            }))
    }
}
struct Fixture {
    cx: TestAppContext,
    window: WindowHandle<UiRoot>,
    consumer: Entity<Consumer>,
    handles: [PopupHandle; 3],
}
impl Fixture {
    fn new() -> Self {
        let mut cx = TestAppContext::single();
        let handles = std::array::from_fn(|_| PopupHandle::new());
        let mut consumer = None;
        let window = cx.add_window(|window, cx| {
            let scope = ConfigScope::new_window(cx);
            let view = cx.new(|_| Consumer {
                scope: scope.clone(),
                handles: handles.clone(),
                mounted: [true; 3],
                clicks: Rc::default(),
            });
            consumer = Some(view.clone());
            UiRoot::new(scope, view, window, cx)
        });
        let mut fixture = Self {
            cx,
            window,
            consumer: consumer.unwrap(),
            handles,
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
    fn open(&mut self, i: usize, open: bool) {
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                self.handles[i].set_open(open, window, cx)
            })
            .unwrap();
        self.draw();
    }
    fn advance(&mut self, millis: u64) {
        self.cx
            .executor()
            .advance_clock(Duration::from_millis(millis));
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                window.simulate_next_frame(cx)
            })
            .unwrap();
        self.draw();
    }
    fn geometry(&mut self, i: usize) -> (Bounds<Pixels>, Bounds<Pixels>) {
        self.cx.update(|cx| {
            let state = self.handles[i].state().unwrap().upgrade().unwrap();
            let state = state.read(cx);
            (
                state.layout.unwrap().bounds,
                state.visual_layout.unwrap().bounds,
            )
        })
    }
    fn idle_frames(&mut self) -> usize {
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                window.simulate_next_frame(cx)
            })
            .unwrap()
    }
}

#[test]
fn unique_background_moves_and_resizes_while_content_uses_its_final_layout() {
    let mut f = Fixture::new();
    f.open(0, true);
    f.advance(300);
    let first = f.geometry(0).0;
    f.open(1, true);
    let (target, start) = f.geometry(1);
    assert_ne!(target, first);
    assert_eq!(start, first);
    assert!(f.cx.update(|cx| f.handles[0].layout(cx).is_none()));
    f.advance(100);
    let (content, middle) = f.geometry(1);
    assert_eq!(
        content, target,
        "one stable input and accessibility subtree"
    );
    assert!(middle.left() > first.left() && middle.left() < target.left());
    assert!(middle.size.width > first.size.width && middle.size.width < target.size.width);
    assert!(middle.size.height > first.size.height && middle.size.height < target.size.height);
    f.cx.update_window(f.window.into(), |_, window, _| {
        let scale = window.scale_factor();
        assert!(
            window.rendered_scene().quads.iter().any(|quad| {
                (quad.bounds.origin.x.0 / scale - f32::from(middle.left())).abs() < 1.
                    && (quad.bounds.origin.y.0 / scale - f32::from(middle.top())).abs() < 1.
                    && (quad.bounds.size.width.0 / scale - f32::from(middle.size.width)).abs() < 1.
                    && quad
                        .background
                        .as_solid()
                        .is_some_and(|color| color.a > 0.8)
            }),
            "native background is painted at the interpolated bounds without fading out"
        );
    })
    .unwrap();
    f.advance(200);
    assert_eq!(f.geometry(1), (target, target));
    f.idle_frames();
    f.draw();
    assert_eq!(f.idle_frames(), 0, "no settled geometry polling");
}

#[test]
fn unique_replacement_starts_at_the_last_painted_background_and_removes_old_input() {
    let mut f = Fixture::new();
    f.open(0, true);
    f.advance(300);
    let old_content = f.geometry(0).0;
    f.open(1, true);
    f.advance(100);
    let middle = f.geometry(1).1;
    f.open(2, true);
    assert_eq!(
        f.geometry(2).1,
        middle,
        "rapid handoff must not jump to the old destination"
    );
    let position = old_content.origin + gpui::point(px(20.), px(20.));
    f.cx.update_window(f.window.into(), |_, window, cx| {
        window.dispatch_event(
            PlatformInput::MouseDown(MouseDownEvent {
                button: MouseButton::Left,
                position,
                ..Default::default()
            }),
            cx,
        );
        window.dispatch_event(
            PlatformInput::MouseUp(MouseUpEvent {
                button: MouseButton::Left,
                position,
                ..Default::default()
            }),
            cx,
        );
    })
    .unwrap();
    f.draw();
    assert_eq!(
        f.consumer.read_with(&f.cx, |state, _| state.clicks.get()),
        0
    );
}

#[test]
fn unique_close_is_immediate_and_only_recent_geometry_can_bridge_hover_timers() {
    let mut f = Fixture::new();
    f.open(0, true);
    f.advance(300);
    let first = f.geometry(0).1;
    f.open(0, false);
    assert!(f.cx.update(|cx| f.handles[0].layout(cx).is_none()));
    f.advance(50);
    f.open(1, true);
    assert_eq!(f.geometry(1).1, first);
    f.advance(300);
    f.open(1, false);
    f.advance(301);
    f.open(2, true);
    let (content, background) = f.geometry(2);
    assert_eq!(
        background, content,
        "expired geometry is not a new handoff origin"
    );
}

#[test]
fn unique_reduced_motion_finishes_handoffs_and_unmount_releases_geometry() {
    let mut f = Fixture::new();
    f.open(0, true);
    f.advance(300);
    f.open(1, true);
    f.advance(50);
    f.cx.update(|cx| cx.set_reduce_motion(true));
    f.draw();
    let (content, background) = f.geometry(1);
    assert_eq!(background, content);
    let host = f.cx.update(|cx| {
        f.handles[1]
            .state()
            .unwrap()
            .upgrade()
            .unwrap()
            .read(cx)
            .host
            .clone()
    });
    f.consumer.update(&mut f.cx, |state, cx| {
        state.mounted[1] = false;
        cx.notify();
    });
    f.draw();
    f.draw();
    assert!(
        host.borrow().unique_geometry.is_empty(),
        "geometry cannot retain an unmounted owner"
    );
    f.idle_frames();
    f.draw();
    assert_eq!(f.idle_frames(), 0);
}
