// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Focused Release CPU/layout measurements. These do not measure GPU presentation
//! or native input latency. Use scripts/run-menu-perf.ps1 with the performance preset.

use std::{cell::Cell, hint::black_box, rc::Rc, time::Instant};

use ant_design_gpui::{
    ConfigScope, UiRoot,
    menu::{Menu, MenuItem, MenuItems, MenuMode, MenuState, SubMenu},
};
use gpui::{
    AppContext, Context, Entity, IntoElement, KeyDownEvent, Keystroke, PlatformInput, Render,
    StyleRefinement, TestAppContext, Window, div, prelude::*, px, size,
};

fn measure(name: &str, iterations: usize, work: impl FnMut()) {
    measure_batch(name, iterations, 1, work);
}

fn measure_batch(name: &str, iterations: usize, batch: usize, mut work: impl FnMut()) {
    for _ in 0..5 {
        for _ in 0..batch {
            work();
        }
    }
    let mut samples = Vec::with_capacity(iterations);
    for _ in 0..iterations {
        let start = Instant::now();
        for _ in 0..batch {
            work();
        }
        samples.push(start.elapsed().as_nanos() as f64 / batch as f64);
    }
    samples.sort_unstable_by(f64::total_cmp);
    println!(
        "{name}: n={iterations}, batch={batch}, mean={:.1} ns, p50={:.1} ns, p95={:.1} ns, max={:.1} ns",
        samples.iter().sum::<f64>() / iterations as f64,
        samples[iterations / 2],
        samples[iterations * 95 / 100],
        samples[iterations - 1]
    );
}

fn items(count: usize, renders: &Rc<Cell<usize>>) -> MenuItems {
    MenuItems::new((0..count).map(|index| {
        let renders = renders.clone();
        let label = format!("Navigation item {index}");
        MenuItem::new(format!("item-{index}"), label.clone())
            .content(move |_, _, _| {
                renders.set(renders.get() + 1);
                div().child(label.clone())
            })
            .into()
    }))
    .unwrap()
}

fn branch(count: usize, renders: &Rc<Cell<usize>>) -> MenuItems {
    MenuItems::new([
        SubMenu::new("branch", "Navigation", items(count, renders)).into(),
        MenuItem::new("settings", "Settings").into(),
    ])
    .unwrap()
}

#[derive(Clone, Copy)]
enum Mode {
    Inline,
    LongRoot,
    Popup,
    Horizontal,
}

struct Scene {
    scope: ConfigScope,
    mode: Mode,
    inline: Entity<MenuState>,
    long_root: Entity<MenuState>,
    popup: Entity<MenuState>,
    horizontal: Entity<MenuState>,
    collapsed: bool,
    width: f32,
    renders: Rc<Cell<usize>>,
}

impl Render for Scene {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        self.renders.set(self.renders.get() + 1);
        let menu = match self.mode {
            Mode::Inline => Menu::new("inline", &self.inline)
                .scope(self.scope.clone())
                .mode(MenuMode::Inline)
                .inline_collapsed(self.collapsed)
                .default_selected_keys(["item-0"])
                .default_open_keys(["branch"])
                .w(px(if self.collapsed { 80. } else { 256. })),
            Mode::LongRoot => Menu::new("root1000", &self.long_root)
                .scope(self.scope.clone())
                .mode(MenuMode::Inline)
                .max_height(px(420.))
                .w(px(256.)),
            Mode::Popup => Menu::new("popup", &self.popup)
                .scope(self.scope.clone())
                .mode(MenuMode::Vertical)
                .w(px(256.)),
            Mode::Horizontal => Menu::new("horizontal", &self.horizontal)
                .scope(self.scope.clone())
                .mode(MenuMode::Horizontal)
                .w(px(self.width)),
        };
        div().p(px(40.)).child(menu)
    }
}

struct StaticContent(Rc<Cell<usize>>);
impl Render for StaticContent {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        self.0.set(self.0.get() + 1);
        div().child("Retained workspace")
    }
}

struct CachedScene {
    scene: Entity<Scene>,
    sibling: Entity<StaticContent>,
}
impl Render for CachedScene {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        div()
            .relative()
            .size_full()
            .child(
                self.scene
                    .clone()
                    .cached(StyleRefinement::default().size_full()),
            )
            .child(
                self.sibling.clone().cached(
                    StyleRefinement::default()
                        .absolute()
                        .top(px(8.))
                        .right(px(8.))
                        .w(px(160.))
                        .h(px(24.)),
                ),
            )
    }
}

fn main() {
    if cfg!(debug_assertions) {
        eprintln!("Menu performance measurements require Release");
        std::process::exit(2);
    }
    let model_rows = Rc::new(Cell::new(0));
    measure("menu/model/build-1000", 100, || {
        black_box(items(1000, &model_rows));
    });
    let shared = items(1000, &model_rows);
    measure_batch("menu/model/shared-snapshot-1000", 1000, 1000, || {
        black_box(black_box(&shared).clone());
    });
    measure_batch("menu/model/key-lookup-1000", 1000, 1000, || {
        black_box(shared.item(black_box("item-997")));
    });

    let mut cx = TestAppContext::single();
    let scene_renders = Rc::new(Cell::new(0));
    let sibling_renders = Rc::new(Cell::new(0));
    let popup_rows = Rc::new(Cell::new(0));
    let inline_rows = Rc::new(Cell::new(0));
    let root_rows = Rc::new(Cell::new(0));
    let horizontal_rows = Rc::new(Cell::new(0));
    let mut scene = None;
    let window = cx.open_window(size(px(1200.), px(900.)), |window, cx| {
        let scope = ConfigScope::new_window(cx);
        scope
            .update_patch(cx, |patch| patch.motion = Some(false))
            .unwrap();
        let inline = cx.new(|cx| MenuState::new(branch(30, &inline_rows), window, cx));
        let long_root = cx.new(|cx| MenuState::new(items(1000, &root_rows), window, cx));
        let popup = cx.new(|cx| MenuState::new(branch(1000, &popup_rows), window, cx));
        let horizontal = cx.new(|cx| MenuState::new(items(30, &horizontal_rows), window, cx));
        let view = cx.new(|_| Scene {
            scope: scope.clone(),
            mode: Mode::Inline,
            inline,
            long_root,
            popup,
            horizontal,
            collapsed: false,
            width: 720.,
            renders: scene_renders.clone(),
        });
        scene = Some(view.clone());
        let sibling = cx.new(|_| StaticContent(sibling_renders.clone()));
        let cached = cx.new(|_| CachedScene {
            scene: view,
            sibling,
        });
        UiRoot::new(scope, cached, window, cx)
    });
    let scene = scene.unwrap();
    let settle = |cx: &mut TestAppContext| {
        for _ in 0..8 {
            cx.run_until_parked();
            let frames = cx
                .update_window(window.into(), |_, window, cx| {
                    window.draw(cx).clear(cx);
                    window.simulate_next_frame(cx)
                })
                .unwrap();
            if frames == 0 {
                return;
            }
        }
        panic!("Menu mount must settle within eight frames");
    };
    let draw = |cx: &mut TestAppContext, rerender: bool| {
        cx.run_until_parked();
        cx.update_window(window.into(), |_, window, cx| {
            if rerender {
                scene.update(cx, |_, cx| cx.notify());
            }
            window.draw(cx).clear(cx);
            assert_eq!(
                window.simulate_next_frame(cx),
                0,
                "settled Menu must schedule no animation frames"
            );
        })
        .unwrap();
    };

    let mut initial_root_rows = 0;
    for (mode, label) in [
        (Mode::Inline, "inline30"),
        (Mode::LongRoot, "inline1000/virtualized"),
        (Mode::Popup, "popup1000/closed"),
        (Mode::Horizontal, "horizontal30/overflow"),
    ] {
        scene.update(&mut cx, |scene, cx| {
            scene.mode = mode;
            cx.notify();
        });
        settle(&mut cx);
        if matches!(mode, Mode::LongRoot) {
            initial_root_rows = root_rows.get();
            assert!(
                initial_root_rows > 0 && initial_root_rows < 128,
                "bounded 1000-item root must render only the viewport, got {initial_root_rows}"
            );
        }
        let before = scene_renders.get();
        measure(&format!("menu/{label}/cached-scene"), 100, || {
            draw(&mut cx, false)
        });
        assert_eq!(
            scene_renders.get(),
            before,
            "cached host must retain Menu subtree"
        );
        measure(&format!("menu/{label}/host-rerender"), 100, || {
            draw(&mut cx, true)
        });
    }

    scene.update(&mut cx, |scene, cx| {
        scene.mode = Mode::LongRoot;
        cx.notify();
    });
    settle(&mut cx);
    let long_root = scene.read_with(&cx, |scene, _| scene.long_root.clone());
    root_rows.set(0);
    cx.update_window(window.into(), |_, window, cx| {
        long_root.update(cx, |state, cx| state.focus(window, cx));
        window.dispatch_event(
            PlatformInput::KeyDown(KeyDownEvent {
                keystroke: Keystroke::parse("end").unwrap(),
                is_held: false,
                prefer_character_input: false,
            }),
            cx,
        );
    })
    .unwrap();
    draw(&mut cx, false);
    assert_eq!(
        long_root.read_with(&cx, |state, _| state.active_key().map(ToString::to_string)),
        Some("item-999".into())
    );
    let distant_root_rows = root_rows.get();
    assert!(
        distant_root_rows > 0 && distant_root_rows < 128,
        "End navigation must reveal a bounded distant viewport, got {distant_root_rows}"
    );
    println!("menu/root1000/initial-content-renders={initial_root_rows}");
    println!("menu/root1000/distant-content-renders={distant_root_rows}");

    scene.update(&mut cx, |scene, cx| {
        scene.mode = Mode::Popup;
        cx.notify();
    });
    settle(&mut cx);
    let popup = scene.read_with(&cx, |scene, _| scene.popup.clone());
    popup_rows.set(0);
    popup.update(&mut cx, |state, cx| state.set_open_keys(["branch"], cx));
    settle(&mut cx);
    let initial_rows = popup_rows.get();
    assert!(
        initial_rows > 0 && initial_rows < 128,
        "opening a 1000-item popup must render a bounded viewport, got {initial_rows}"
    );
    println!("menu/popup1000/initial-content-renders={initial_rows}");
    measure("menu/popup1000/open-cached-scene", 100, || {
        draw(&mut cx, false)
    });
    let mut max_rows = 0;
    measure("menu/popup1000/open-host-rerender", 100, || {
        popup_rows.set(0);
        draw(&mut cx, true);
        max_rows = max_rows.max(popup_rows.get());
        assert!(
            popup_rows.get() < 128,
            "popup row construction must remain bounded"
        );
    });
    println!("menu/popup1000/max-content-renders-per-redraw={max_rows}");
    let before_selection = scene_renders.get();
    let sibling_before_selection = sibling_renders.get();
    let mut selected = false;
    measure("menu/popup1000/retained-selection", 100, || {
        selected = !selected;
        popup.update(&mut cx, |state, cx| {
            state.set_selected_keys([if selected { "item-4" } else { "item-5" }], cx);
        });
        draw(&mut cx, false);
    });
    assert_eq!(
        sibling_renders.get(),
        sibling_before_selection,
        "state-only Menu changes must preserve unrelated cached sibling content"
    );
    let selection_ancestor_renders = scene_renders.get() - before_selection;
    println!(
        "menu/popup1000/selection-ancestor-renders={selection_ancestor_renders}, unrelated-sibling-renders=0"
    );
    cx.update_window(window.into(), |_, window, cx| {
        popup.update(cx, |state, cx| state.focus(window, cx));
        window.dispatch_event(
            PlatformInput::KeyDown(KeyDownEvent {
                keystroke: Keystroke::parse("right").unwrap(),
                is_held: false,
                prefer_character_input: false,
            }),
            cx,
        );
    })
    .unwrap();
    draw(&mut cx, false);
    let before_navigation = popup.read_with(&cx, |state, _| state.active_key().cloned());
    let cached_host_renders = scene_renders.get();
    measure("menu/popup1000/keyboard-navigation", 100, || {
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
    });
    assert_ne!(
        popup.read_with(&cx, |state, _| state.active_key().cloned()),
        before_navigation
    );
    // The pinned GPUI Window::focus calls refresh(), which deliberately bypasses
    // every view cache. Native row focus is retained; report that framework cost.
    let focus_refreshes = scene_renders.get() - cached_host_renders;
    println!("menu/popup1000/keyboard-window-refresh-host-renders={focus_refreshes}");

    measure("menu/popup1000/warm-close-reopen", 50, || {
        popup.update(&mut cx, |state, cx| {
            state.set_open_keys(Vec::<String>::new(), cx)
        });
        draw(&mut cx, false);
        popup.update(&mut cx, |state, cx| state.set_open_keys(["branch"], cx));
        draw(&mut cx, false);
    });
    popup.update(&mut cx, |state, cx| {
        state.set_open_keys(Vec::<String>::new(), cx)
    });
    draw(&mut cx, false);

    scene.update(&mut cx, |scene, cx| {
        scene.mode = Mode::Inline;
        cx.notify();
    });
    settle(&mut cx);
    measure("menu/inline30/collapse-expand", 100, || {
        scene.update(&mut cx, |scene, cx| {
            scene.collapsed = !scene.collapsed;
            cx.notify();
        });
        draw(&mut cx, false);
    });
    scene.update(&mut cx, |scene, cx| {
        scene.mode = Mode::Horizontal;
        cx.notify();
    });
    settle(&mut cx);
    measure("menu/horizontal30/resize-overflow", 100, || {
        scene.update(&mut cx, |scene, cx| {
            scene.width = if scene.width < 600. { 1080. } else { 320. };
            cx.notify();
        });
        draw(&mut cx, false);
    });
    println!(
        "menu/invariants: settled_frames=0, popup1000_initial_rows={initial_rows}, popup1000_max_rows={max_rows}, host_renders={}",
        scene_renders.get()
    );
}
