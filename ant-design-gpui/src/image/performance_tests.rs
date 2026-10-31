// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;
use crate::{ConfigScope, UiRoot};
use gpui::{
    AppContext, Context, Entity, IntoElement, Render, TestAppContext, Window, WindowHandle,
    WindowVisibility, div, prelude::*, px,
};
use std::time::Duration;

struct Page {
    image: Entity<ImageState>,
    scope: ConfigScope,
    offset: f32,
}
impl Render for Page {
    fn render(&mut self, w: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        div()
            .flex()
            .flex_col()
            .w(w.viewport_size().width)
            .h(w.viewport_size().height)
            .overflow_hidden()
            .child(div().h(px(self.offset)).flex_none())
            .child(
                Image::new("performance-progress", &self.image, cx)
                    .scope(self.scope.clone())
                    .w(px(200.))
                    .h(px(200.))
                    .progress(ImageProgress::default()),
            )
    }
}
struct Fixture {
    cx: TestAppContext,
    window: WindowHandle<UiRoot>,
    page: Entity<Page>,
    scope: ConfigScope,
}
impl Fixture {
    fn new() -> Self {
        let mut cx = TestAppContext::single();
        let mut data = None;
        let window = cx.add_window(|w, cx| {
            let scope = ConfigScope::new_window(cx);
            let image = cx.new(|cx| ImageState::empty(w, cx));
            let page = cx.new(|_| Page {
                image,
                scope: scope.clone(),
                offset: 0.,
            });
            data = Some((page.clone(), scope.clone()));
            UiRoot::new(scope, page, w, cx)
        });
        let (page, scope) = data.unwrap();
        cx.update_window(window.into(), |_, w, _| w.activate_window())
            .unwrap();
        let mut f = Self {
            cx,
            window,
            page,
            scope,
        };
        f.draw();
        f
    }
    fn draw(&mut self) {
        self.cx.run_until_parked();
        self.cx
            .update_window(self.window.into(), |_, w, cx| w.draw(cx).clear(cx))
            .unwrap();
        self.cx.run_until_parked();
    }
    fn tick(&mut self) -> usize {
        self.cx.executor().advance_clock(Duration::from_millis(16));
        let count = self
            .cx
            .update_window(self.window.into(), |_, w, cx| w.simulate_next_frame(cx))
            .unwrap();
        self.draw();
        count
    }
    fn offset(&mut self, offset: f32) {
        self.page.update(&mut self.cx, |p, cx| {
            p.offset = offset;
            cx.notify();
        });
        self.draw();
    }
}
#[test]
fn loading_ink_animates_only_while_its_native_region_is_visible() {
    let mut f = Fixture::new();
    let state =
        f.cx.update_window(f.window.into(), |_, w, cx| {
            (
                w.is_visible(),
                w.is_window_active(),
                f.page.read(cx).image.read(cx).bounds(),
                w.viewport_size(),
            )
        })
        .unwrap();
    assert!(f.tick() > 0, "visible loading ink must animate: {state:?}");
    f.offset(2000.);
    f.tick();
    assert_eq!(
        f.tick(),
        0,
        "clipped loading ink must not retain a frame loop"
    );
    f.offset(0.);
    assert!(f.tick() > 0);
    f.cx.simulate_window_visibility_change(f.window.into(), WindowVisibility::Hidden);
    f.draw();
    f.tick();
    assert_eq!(f.tick(), 0, "hidden windows must stop loading motion");
    f.cx.simulate_window_visibility_change(f.window.into(), WindowVisibility::Visible);
    f.draw();
    assert!(f.tick() > 0, "presenting the window resumes visible ink");
    f.cx.update(|cx| {
        f.scope
            .update_patch(cx, |p| p.motion = Some(false))
            .unwrap()
    });
    f.draw();
    f.tick();
    assert_eq!(f.tick(), 0, "scoped reduced motion removes the frame loop");
}
#[test]
fn loading_ink_behind_a_modal_does_not_schedule_frames() {
    let mut f = Fixture::new();
    assert!(f.tick() > 0);
    let mut modal = None;
    f.cx.update_window(f.window.into(), |_, w, cx| {
        let handle = crate::ModalManager::new(f.scope.clone(), w).info(
            crate::modal::ModalOptions {
                content: Some(crate::modal::ModalContent::text("Covered loading image")),
                ..Default::default()
            },
            w,
            cx,
        );
        modal = Some(handle);
    })
    .unwrap();
    f.draw();
    f.cx.executor().advance_clock(Duration::from_millis(400));
    f.tick();
    f.tick();
    assert_eq!(
        f.tick(),
        0,
        "covered image must not redraw an otherwise settled dialog"
    );
    f.cx.update_window(f.window.into(), |_, w, cx| {
        modal.take().unwrap().destroy(w, cx)
    })
    .unwrap();
    f.draw();
    f.cx.executor().advance_clock(Duration::from_millis(400));
    f.tick();
    assert!(
        f.tick() > 0,
        "uncovering the native region resumes its animation"
    );
}

struct Empty;
impl Render for Empty {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        div()
    }
}
#[test]
fn loading_ink_suspends_for_another_active_native_window() {
    let mut f = Fixture::new();
    assert!(f.tick() > 0);
    let other = f.cx.add_window(|w, cx| {
        let scope = ConfigScope::new_window(cx);
        let content = cx.new(|_| Empty);
        UiRoot::new(scope, content, w, cx)
    });
    f.cx.update_window(other.into(), |_, w, _| w.activate_window())
        .unwrap();
    f.draw();
    f.tick();
    assert!(
        !f.cx
            .update_window(f.window.into(), |_, w, _| w.is_window_active())
            .unwrap()
    );
    assert_eq!(
        f.tick(),
        0,
        "background windows must not request continuous loading frames"
    );
    f.cx.update_window(f.window.into(), |_, w, _| w.activate_window())
        .unwrap();
    f.draw();
    assert!(
        f.tick() > 0,
        "returning to the window resumes visible loading motion"
    );
}
