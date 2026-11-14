// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Focused Release CPU/layout measurements with GPUI's offscreen text system.
//! These measure retained scene work, not GPU presentation or input latency.
//! Run scripts/run-context-scroll-perf.ps1 with the performance preset configured.

use std::{cell::Cell, hint::black_box, rc::Rc, time::Instant};

use ant_design_gpui::{
    Button, ConfigScope, ContextMenu, ContextMenuState, MenuItem, MenuItems, ScrollArea,
    ScrollAreaState, UiRoot,
    scroll_area::{ScrollAxis, ScrollbarVisibility},
};
use gpui::{
    AppContext, Context, Entity, IntoElement, KeyDownEvent, Keystroke, PlatformInput, Render,
    ScrollHandle, StyleRefinement, TestAppContext, Window, div, point, prelude::*, px, size,
};

fn measure(name: &str, iterations: usize, mut work: impl FnMut()) {
    for _ in 0..5 {
        work();
    }
    let mut samples = Vec::with_capacity(iterations);
    for _ in 0..iterations {
        let start = Instant::now();
        work();
        samples.push(start.elapsed().as_nanos());
    }
    samples.sort_unstable();
    let mean = samples.iter().sum::<u128>() as f64 / iterations as f64;
    println!(
        "{name}: n={iterations}, mean={mean:.1} ns, p50={} ns, p95={} ns, max={} ns",
        samples[iterations / 2],
        samples[iterations * 95 / 100],
        samples[iterations - 1]
    );
}

fn menu_items(count: usize) -> MenuItems {
    MenuItems::new((0..count).map(|i| {
        MenuItem::new(("item", i), format!("Command {i}"))
            .disabled(i % 11 == 1)
            .shortcut("Ctrl+Shift+K")
            .into()
    }))
    .unwrap()
}

#[derive(Clone, Copy)]
enum Mode {
    Menu,
    LongMenu,
    NativeScroll,
    ScrollArea,
}

struct Content {
    renders: Rc<Cell<usize>>,
}
impl Render for Content {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        self.renders.set(self.renders.get() + 1);
        div()
            .w(px(1400.))
            .flex()
            .flex_col()
            .children((0_usize..1000).map(|i| {
                div()
                    .id(("row", i))
                    .h(px(36.))
                    .flex_none()
                    .px(px(12.))
                    .child(format!(
                        "Record {i}: content retained across native scroll updates"
                    ))
            }))
    }
}

struct Scene {
    scope: ConfigScope,
    mode: Mode,
    menu: Entity<ContextMenuState>,
    long_menu: Entity<ContextMenuState>,
    scroll: Entity<ScrollAreaState>,
    native_scroll: ScrollHandle,
    content: Entity<Content>,
    renders: Rc<Cell<usize>>,
}
impl Render for Scene {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        self.renders.set(self.renders.get() + 1);
        let content = self
            .content
            .clone()
            // Cached views take their layout size from this style, before their
            // render method runs. Their natural child size is not consulted.
            .cached(
                StyleRefinement::default()
                    .w(px(1400.))
                    .h(px(36_000.))
                    .flex_none(),
            );
        let child = match self.mode {
            Mode::Menu | Mode::LongMenu => {
                let menu = if matches!(self.mode, Mode::Menu) {
                    &self.menu
                } else {
                    &self.long_menu
                };
                let id = if matches!(self.mode, Mode::Menu) {
                    "menu30"
                } else {
                    "menu1000"
                };
                ContextMenu::new(id, menu)
                    .scope(self.scope.clone())
                    .child(Button::new("trigger").label("Commands"))
                    .into_any_element()
            }
            Mode::NativeScroll => div()
                .id("native-scroll")
                .w(px(720.))
                .h(px(480.))
                .overflow_scroll()
                .track_scroll(&self.native_scroll)
                .child(content)
                .into_any_element(),
            Mode::ScrollArea => ScrollArea::new("scroll-area", &self.scroll)
                .scope(self.scope.clone())
                .axis(ScrollAxis::Both)
                .scrollbar_visibility(ScrollbarVisibility::Always)
                .w(px(720.))
                .h(px(480.))
                .child(content)
                .into_any_element(),
        };
        div().p(px(40.)).child(child)
    }
}

struct CachedScene(Entity<Scene>);
impl Render for CachedScene {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        self.0
            .clone()
            .cached(StyleRefinement::default().size_full())
    }
}

fn main() {
    if cfg!(debug_assertions) {
        eprintln!("Context menu and scroll area benchmarks require Release");
        std::process::exit(2);
    }
    measure("context-menu/model/build-1000", 200, || {
        black_box(menu_items(1000));
    });
    let items = menu_items(1000);
    measure("context-menu/model/shared-snapshot-1000", 100_000, || {
        black_box(black_box(&items).clone());
    });

    let mut cx = TestAppContext::single();
    let mut scene = None;
    let scene_renders = Rc::new(Cell::new(0));
    let content_renders = Rc::new(Cell::new(0));
    let window = cx.open_window(size(px(1200.), px(900.)), |window, cx| {
        let scope = ConfigScope::new_window(cx);
        scope
            .update_patch(cx, |patch| patch.motion = Some(false))
            .unwrap();
        let menu = cx.new(|cx| ContextMenuState::new(menu_items(30), window, cx));
        let long_menu = cx.new(|cx| ContextMenuState::new(items, window, cx));
        let scroll = cx.new(|cx| ScrollAreaState::new(window, cx));
        let content = cx.new(|_| Content {
            renders: content_renders.clone(),
        });
        let view = cx.new(|_| Scene {
            scope: scope.clone(),
            mode: Mode::Menu,
            menu,
            long_menu,
            scroll,
            native_scroll: ScrollHandle::new(),
            content,
            renders: scene_renders.clone(),
        });
        scene = Some(view.clone());
        let cached = cx.new(|_| CachedScene(view));
        UiRoot::new(scope, cached, window, cx)
    });
    let scene = scene.unwrap();
    // Trigger binding discovers whether its child owns a focus handle at first
    // paint, then settles its fallback tab stop on the following frame.
    let settle_mount = |cx: &mut TestAppContext| {
        for _ in 0..4 {
            let scheduled = cx
                .update_window(window.into(), |_, window, cx| {
                    window.draw(cx).clear(cx);
                    window.simulate_next_frame(cx)
                })
                .unwrap();
            if scheduled == 0 {
                return;
            }
        }
        panic!("mount must settle within four frames");
    };
    let draw = |cx: &mut TestAppContext, rerender: bool| {
        cx.update_window(window.into(), |_, window, cx| {
            if rerender {
                scene.update(cx, |_, cx| cx.notify());
            }
            window.draw(cx).clear(cx);
            assert_eq!(
                window.simulate_next_frame(cx),
                0,
                "settled context menus and scrollbars must not schedule frames"
            );
        })
        .unwrap();
    };

    for (mode, label) in [
        (Mode::Menu, "menu30/closed"),
        (Mode::LongMenu, "menu1000/closed"),
        (Mode::NativeScroll, "native-scroll/content1000"),
        (Mode::ScrollArea, "scroll-area/content1000"),
    ] {
        scene.update(&mut cx, |scene, cx| {
            scene.mode = mode;
            cx.notify();
        });
        settle_mount(&mut cx);
        let initial_renders = scene_renders.get();
        measure(&format!("context-scroll/{label}/cached-scene"), 100, || {
            draw(&mut cx, false);
        });
        assert_eq!(
            scene_renders.get(),
            initial_renders,
            "cached scenes must retain the component subtree"
        );
        measure(&format!("context-scroll/{label}/rerender"), 100, || {
            draw(&mut cx, true);
        });
    }

    let native = scene.read_with(&cx, |scene, _| scene.native_scroll.clone());
    scene.update(&mut cx, |scene, cx| {
        scene.mode = Mode::NativeScroll;
        cx.notify();
    });
    settle_mount(&mut cx);
    let before = content_renders.get();
    let mut step = 0;
    measure("context-scroll/native-scroll/scroll-redraw", 200, || {
        step += 1;
        native.set_offset(point(px(0.), px(-(((step * 60) % 30_000) as f32))));
        scene.update(&mut cx, |_, cx| cx.notify());
        draw(&mut cx, false);
    });
    let native_content_renders = content_renders.get() - before;

    scene.update(&mut cx, |scene, cx| {
        scene.mode = Mode::ScrollArea;
        cx.notify();
    });
    settle_mount(&mut cx);
    let scroll = scene.read_with(&cx, |scene, _| scene.scroll.clone());
    let before = content_renders.get();
    measure("context-scroll/scroll-area/scroll-redraw", 200, || {
        step += 1;
        scroll.update(&mut cx, |state, cx| {
            state.scroll_to(point(px(0.), px(-(((step * 60) % 30_000) as f32))), cx);
        });
        draw(&mut cx, false);
    });
    let themed_content_renders = content_renders.get() - before;
    // GPUI's cache key includes absolute bounds and the content mask. Scrolling
    // a single large cached view therefore rerenders it on both paths. Report
    // that work explicitly; use GPUI's list elements for virtualized datasets.
    println!(
        "context-scroll/translated-content-renders: native={native_content_renders}, themed={themed_content_renders}"
    );
    assert!(scroll.read_with(&cx, |state, _| state.offset().y < px(0.)));

    for (mode, label) in [(Mode::Menu, "menu30"), (Mode::LongMenu, "menu1000")] {
        scene.update(&mut cx, |scene, cx| {
            scene.mode = mode;
            cx.notify();
        });
        settle_mount(&mut cx);
        let menu = scene.read_with(&cx, |scene, _| {
            if matches!(mode, Mode::Menu) {
                scene.menu.clone()
            } else {
                scene.long_menu.clone()
            }
        });
        cx.update_window(window.into(), |_, window, cx| {
            menu.update(cx, |state, cx| {
                state.open_at(point(px(80.), px(80.)), window, cx);
            });
        })
        .unwrap();
        draw(&mut cx, false);
        assert!(menu.read_with(&cx, |state, _| state.is_open()));
        measure(
            &format!("context-scroll/{label}/open-cached-scene"),
            100,
            || {
                draw(&mut cx, false);
            },
        );
        measure(
            &format!("context-scroll/{label}/open-rerender"),
            100,
            || {
                draw(&mut cx, true);
            },
        );
        let first_active = menu.read_with(&cx, |state, _| state.active_item().unwrap().id.clone());
        let mut observed_navigation = false;
        measure(
            &format!("context-scroll/{label}/navigate-redraw"),
            100,
            || {
                cx.update_window(window.into(), |_, window, cx| {
                    window.dispatch_event(
                        PlatformInput::KeyDown(KeyDownEvent {
                            keystroke: Keystroke::parse("down").unwrap(),
                            is_held: false,
                            prefer_character_input: false,
                        }),
                        cx,
                    );
                })
                .unwrap();
                draw(&mut cx, false);
                observed_navigation |= menu.read_with(&cx, |state, _| {
                    state
                        .active_item()
                        .is_some_and(|item| item.id != first_active)
                });
            },
        );
        assert!(
            observed_navigation,
            "navigation must move the active menu item"
        );
        measure(
            &format!("context-scroll/{label}/warm-close-reopen"),
            100,
            || {
                cx.update_window(window.into(), |_, window, cx| {
                    menu.update(cx, |state, cx| state.close(window, cx));
                })
                .unwrap();
                draw(&mut cx, false);
                cx.update_window(window.into(), |_, window, cx| {
                    menu.update(cx, |state, cx| {
                        state.open_at(point(px(80.), px(80.)), window, cx);
                    });
                })
                .unwrap();
                draw(&mut cx, false);
            },
        );
        cx.update_window(window.into(), |_, window, cx| {
            menu.update(cx, |state, cx| state.close(window, cx));
        })
        .unwrap();
        draw(&mut cx, false);
    }
    println!(
        "context-scroll/invariants: settled_frames=0, cached_content_renders={}",
        content_renders.get()
    );
}
