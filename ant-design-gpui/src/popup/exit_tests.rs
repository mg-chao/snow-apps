// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;
use crate::{Button, ConfigScope, UiRoot};
use gpui::{
    AppContext, Context, Entity, IntoElement, Modifiers, Render, TestAppContext, VisualTestContext,
    Window, WindowHandle, div, prelude::*, px,
};
use std::{cell::Cell, rc::Rc, time::Duration};

struct Consumer {
    handle: PopupHandle,
    open: bool,
    calls: Rc<Cell<usize>>,
    scope: ConfigScope,
}
impl Render for Consumer {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        let calls = self.calls.clone();
        div().p(px(20.)).child(
            Popup::new("menu-exit-test")
                .scope(self.scope.clone())
                .handle(&self.handle)
                .open(self.open)
                .triggers(PopupTriggers::NONE)
                .arrow(false)
                .placement(PopupPlacement::BottomLeft)
                .motion_duration(Duration::from_millis(200))
                .exit_duration(Duration::from_millis(200))
                .child(Button::new("exit-trigger").label("Open"))
                .content(move |_, _, _| {
                    let calls = calls.clone();
                    div().debug_selector(|| "exit-action".into()).child(
                        Button::new("exit-action")
                            .label("Action")
                            .on_click(move |_, _, _| calls.set(calls.get() + 1)),
                    )
                }),
        )
    }
}
struct Fixture {
    cx: VisualTestContext,
    host: Entity<Consumer>,
    handle: PopupHandle,
    calls: Rc<Cell<usize>>,
    _window: WindowHandle<UiRoot>,
}
impl Fixture {
    fn new() -> Self {
        let mut app = TestAppContext::single();
        let mut host = None;
        let handle = PopupHandle::new();
        let calls = Rc::new(Cell::new(0));
        let window = app.add_window(|window, cx| {
            crate::init(cx);
            window.activate_window();
            let scope = ConfigScope::new_window(cx);
            let consumer = cx.new(|_| Consumer {
                handle: handle.clone(),
                open: false,
                calls: calls.clone(),
                scope: scope.clone(),
            });
            host = Some(consumer.clone());
            UiRoot::new(scope, consumer, window, cx)
        });
        let mut fixture = Self {
            cx: VisualTestContext::from_window(window.into(), &app),
            host: host.unwrap(),
            handle,
            calls,
            _window: window,
        };
        fixture.draw();
        fixture
    }
    fn draw(&mut self) {
        self.cx.run_until_parked();
        self.cx.update(|w, c| w.draw(c).clear(c));
        self.cx.run_until_parked();
    }
    fn open(&mut self, open: bool) {
        self.host.update(&mut self.cx, |h, c| {
            h.open = open;
            c.notify();
        });
        self.draw();
    }
    fn advance(&mut self, millis: u64) {
        self.cx
            .executor()
            .advance_clock(Duration::from_millis(millis));
        self.cx.update(|w, c| w.simulate_next_frame(c));
        self.draw();
    }
    fn exiting(&mut self) -> bool {
        let handle = self.handle.clone();
        self.cx
            .update(|_, cx| handle.state().unwrap().upgrade().unwrap().read(cx).exiting)
    }
    fn opacity(&mut self) -> f32 {
        let handle = self.handle.clone();
        self.cx.update(|window, cx| {
            let state = handle.state().unwrap().upgrade().unwrap();
            state.update(cx, |s, cx| s.sample_opacity(s.presented(), window, cx).0)
        })
    }
}
#[test]
fn popup_menu_exit_retains_pixels_without_live_actions_and_releases_when_settled() {
    let mut f = Fixture::new();
    f.open(true);
    f.advance(250);
    let target = f.cx.debug_bounds("exit-action").unwrap().center();
    f.open(false);
    assert!(f.exiting());
    f.cx.simulate_click(target, Modifiers::none());
    f.draw();
    assert_eq!(f.calls.get(), 0);
    f.advance(100);
    assert!(f.exiting());
    f.advance(150);
    assert!(!f.exiting());
    f.draw();
    assert!(f.cx.debug_bounds("exit-action").is_none());
}
#[test]
fn popup_menu_exit_reduced_motion_removes_the_surface_immediately() {
    let mut f = Fixture::new();
    f.open(true);
    f.advance(250);
    let scope = f.host.read_with(&f.cx, |h, _| h.scope.clone());
    f.cx.update(|_, cx| scope.update_patch(cx, |p| p.motion = Some(false)).unwrap());
    f.draw();
    f.open(false);
    assert!(!f.exiting());
    assert!(f.cx.debug_bounds("exit-action").is_none());
}

#[test]
fn popup_menu_exit_reopening_continues_from_the_painted_opacity() {
    let mut f = Fixture::new();
    f.open(true);
    f.advance(100);
    let entered = f.opacity();
    f.open(false);
    f.advance(50);
    let leaving = f.opacity();
    assert!(leaving > 0. && leaving < entered);
    f.open(true);
    let resumed = f.opacity();
    assert!((resumed - leaving).abs() < 0.001);
    f.advance(250);
    assert_eq!(f.opacity(), 1.);
    assert!(!f.exiting());
}
