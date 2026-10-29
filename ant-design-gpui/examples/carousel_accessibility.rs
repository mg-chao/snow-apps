// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Hidden native Carousel fixture. The runner uses UI Automation without global input.

use std::{cell::Cell, path::PathBuf, rc::Rc, time::Duration};

use ant_design_gpui::{
    Button, ConfigScope, Direction, UiRoot,
    behavior::Accessibility,
    carousel::{Carousel, CarouselEvent, CarouselItems, CarouselSlide, CarouselState},
    locale::Locale,
};
use gpui::{
    App, AppContext, Bounds, Context, Entity, FocusHandle, IntoElement, Render, Role, Window,
    WindowBounds, WindowOptions, div, prelude::*, px, rgb, size,
};
use serde_json::{Value, json};

struct Fixture {
    scope: ConfigScope,
    carousel: Entity<CarouselState>,
    items: CarouselItems,
    outside_focus: FocusHandle,
    slide_focus: Vec<FocusHandle>,
    action_count: Rc<Cell<usize>>,
    events: Vec<Value>,
    stage: usize,
}

impl Fixture {
    fn set_stage(&mut self, stage: usize, window: &mut Window, cx: &mut Context<Self>) {
        if stage == 1 {
            if let Some(index) = self.carousel.read(cx).current_index() {
                window.focus(&self.slide_focus[index], cx);
            }
            self.scope
                .update_patch(cx, |patch| {
                    patch.direction = Some(Direction::Rtl);
                    patch.locale.locale = Some(Locale::zh_cn());
                })
                .unwrap();
        } else if stage >= 2 {
            let count = match stage {
                2 => 2,
                3 => 1,
                4 => 0,
                _ => 3,
            };
            let items = CarouselItems::new(self.items.entries()[..count].iter().cloned()).unwrap();
            self.carousel
                .update(cx, |state, cx| state.set_items(items, window, cx));
            if stage == 5 {
                self.scope
                    .update_patch(cx, |patch| patch.locale.locale = Some(Locale::zh_tw()))
                    .unwrap();
            }
        }
        self.stage = stage;
        cx.notify();
    }
}

impl Render for Fixture {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        div()
            .size_full()
            .p(px(24.))
            .bg(gpui::white())
            .flex()
            .flex_col()
            .gap(px(24.))
            .child(
                Button::new("carousel.outside")
                    .scope(self.scope.clone())
                    .label("Outside carousel")
                    .automation_id("carousel.outside")
                    .focus_handle(&self.outside_focus),
            )
            .child(
                Carousel::new("carousel.main", &self.carousel)
                    .scope(self.scope.clone())
                    .arrows(true)
                    .infinite(false)
                    .w_full()
                    .h(px(160.))
                    .accessibility(
                        Accessibility::new(Role::Region)
                            .automation_id("carousel.main")
                            .description("Featured documents"),
                    )
                    .on_event(cx.listener(|fixture, event, _, cx| {
                        let (kind, change) = match event {
                            CarouselEvent::BeforeChange(change) => ("before", change),
                            CarouselEvent::AfterChange(change) => ("after", change),
                        };
                        fixture.events.push(json!({"kind":kind,"from":change.from,
                            "to":change.to,"source":format!("{:?}",change.source)}));
                        cx.notify();
                    })),
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
    unsafe {
        let handle = FindWindowW(std::ptr::null(), title.as_ptr());
        assert_ne!(handle, 0, "hidden Carousel accessibility HWND");
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
            .expect("carousel_accessibility <state.json> <command.json>"),
    );
    let command = PathBuf::from(args.next().expect("command.json"));
    assert!(args.next().is_none());
    if let Some(parent) = output.parent() {
        std::fs::create_dir_all(parent).unwrap();
    }
    gpui_platform::application().run(move |cx: &mut App| {
        ant_design_gpui::init(cx);
        cx.open_window(WindowOptions {
            window_bounds: Some(WindowBounds::Windowed(Bounds::centered(None, size(px(640.), px(360.)), cx))),
            show: false, focus: false, ..Default::default()
        }, move |window, cx| {
            let title = format!("Carousel hidden accessibility fixture {}", std::process::id());
            window.set_window_title(&title);
            window.resize(size(px(640.), px(360.)));
            let scope = ConfigScope::new_window(cx);
            scope.update_patch(cx, |patch| patch.motion = Some(false)).unwrap();
            let slide_focus: Vec<_> = (0..3).map(|_| cx.focus_handle()).collect();
            let action_count = Rc::new(Cell::new(0));
            let items = CarouselItems::new(slide_focus.iter().enumerate().map(|(index, focus)| {
                let focus = focus.clone();
                let scope = scope.clone();
                let actions = action_count.clone();
                CarouselSlide::new(format!("document-{index}")).content(move |_, _| {
                    let actions = actions.clone();
                    div().size_full().flex().items_center().justify_center().bg(rgb(0x364d79))
                        .child(Button::new(format!("carousel.content.{index}"))
                            .scope(scope.clone()).label(format!("Open document {}", index+1))
                            .automation_id(format!("carousel.content.{index}"))
                            .focus_handle(&focus)
                            .on_click(move |_, _, _| actions.set(actions.get()+1)))
                        .into_any_element()
                })
            })).unwrap();
            let carousel = cx.new(|cx| CarouselState::new(items.clone(), window, cx));
            let fixture = cx.new(|cx| Fixture { scope: scope.clone(), carousel, items, outside_focus: cx.focus_handle(), slide_focus, action_count, events:Vec::new(), stage:usize::MAX });
            let observation = fixture.downgrade();
            let root = cx.new(|cx| UiRoot::new(scope, fixture, window, cx));
            window.spawn(cx, async move |cx| {
                cx.background_executor().timer(Duration::from_millis(150)).await;
                cx.update(|window, cx| {
                    window.draw(cx).clear(cx);
                    if let Some(fixture) = observation.upgrade() { window.focus(&fixture.read(cx).outside_focus.clone(), cx); }
                }).unwrap();
                request_native_accessibility(&title);
                for frame in 0..1200 {
                    cx.background_executor().timer(Duration::from_millis(50)).await;
                    let input:Value = std::fs::read(&command).ok().and_then(|bytes|serde_json::from_slice(&bytes).ok()).unwrap_or_default();
                    if input["quit"] == true { cx.update(|_,cx|cx.quit()).unwrap(); return; }
                    if cx.update(|window,cx| {
                        let Some(fixture) = observation.upgrade() else { return; };
                        let stage = input["stage"].as_u64().unwrap_or(0) as usize;
                        if fixture.read(cx).stage != stage { fixture.update(cx,|view,cx|view.set_stage(stage,window,cx)); }
                        // Ordinary unchanged draws keep the native cache eligible.
                        // Forcing window.refresh() would conceal semantic-cache bugs.
                        window.draw(cx).clear(cx);
                        let view=fixture.read(cx);
                        let state=view.carousel.read(cx);
                        let tree=window.debug_a11y_tree_json().and_then(|tree|serde_json::from_str::<Value>(&tree).ok());
                        let value=json!({"process_id":std::process::id(),"title":title,"stage":stage,"frame":frame,"hidden":true,
                            "a11y_active":window.is_a11y_active(),"outside_focus":view.outside_focus.is_focused(window),
                            "content_focus":view.slide_focus.iter().map(|focus|focus.is_focused(window)).collect::<Vec<_>>(),
                            "current":state.current_index(),"count":state.items().len(),"action_count":view.action_count.get(),
                            "events":view.events,"tree":tree});
                        let temporary=output.with_extension("tmp");
                        std::fs::write(&temporary,serde_json::to_vec(&value).unwrap()).and_then(|_|std::fs::rename(temporary,&output)).unwrap();
                    }).is_err(){return;}
                }
                cx.update(|_,cx|cx.quit()).unwrap();
            }).detach();
            root
        }).expect("hidden Carousel accessibility window");
    });
}
