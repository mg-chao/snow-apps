// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Focused CPU, layout, shadow-raster and retained-scene measurements.
//! Run scripts/run-popup-perf.ps1 with the configured performance preset.
use std::{
    hint::black_box,
    time::{Duration, Instant},
};

use ant_design_gpui::{
    Button, ConfigScope, Popconfirm, Popover, Tooltip, UiRoot,
    popup::{PopupArrow, PopupGeometry, PopupHandle, PopupPlacement, popup_shadow_statistics},
};
use gpui::{
    AppContext, Bounds, Context, IntoElement, Render, StyleRefinement, TestAppContext, Window, div,
    point, prelude::*, px, size,
};

fn measure(name: &str, iterations: usize, mut work: impl FnMut()) {
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

#[derive(Clone, Copy)]
enum Mode {
    Buttons,
    ClosedTooltips,
    Confirmation,
    Nested,
    Unique,
}
struct Scene {
    scope: ConfigScope,
    mode: Mode,
    width: f32,
    handle: PopupHandle,
    nested_handle: PopupHandle,
    unique_handles: [PopupHandle; 2],
}
struct CachedScene(gpui::Entity<Scene>);
impl Render for CachedScene {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        self.0
            .clone()
            .cached(StyleRefinement::default().size_full())
    }
}
impl Render for Scene {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        match self.mode {
            Mode::Unique => div()
                .p(px(180.))
                .flex()
                .gap(px(140.))
                .children((0..2).map(|i| {
                    Tooltip::new(("unique", i))
                        .scope(self.scope.clone())
                        .unique(true)
                        .handle(&self.unique_handles[i])
                        .child(Button::new(("trigger", i)).label("Hover"))
                        .title_content(move |_, _, _| {
                            div()
                                .w(px(60. + i as f32 * 140.))
                                .h(px(22. + i as f32 * 42.))
                                .child("Content")
                        })
                }))
                .into_any_element(),
            Mode::Nested => {
                let scope = self.scope.clone();
                let nested = self.nested_handle.clone();
                div()
                    .p(px(200.))
                    .child(
                        Popover::new("parent")
                            .scope(scope.clone())
                            .handle(&self.handle)
                            .default_open(true)
                            .title("Parent")
                            .child(Button::new("trigger").label("Open"))
                            .content(move |_, _, _| {
                                Popover::new("nested")
                                    .scope(scope.clone())
                                    .handle(&nested)
                                    .default_open(true)
                                    .placement(PopupPlacement::Right)
                                    .title("Nested")
                                    .text("Nested content")
                                    .child(Button::new("nested-trigger").label("Nested"))
                            }),
                    )
                    .into_any_element()
            }
            Mode::Confirmation => div()
                .p(px(200.))
                .child(
                    Popconfirm::new("confirm")
                        .scope(self.scope.clone())
                        .handle(&self.handle)
                        .default_open(true)
                        .title("Delete the item?")
                        .description("This operation cannot be undone.")
                        .surface_style(StyleRefinement::default().w(px(self.width)))
                        .child(
                            Button::new("trigger")
                                .scope(self.scope.clone())
                                .label("Delete"),
                        ),
                )
                .into_any_element(),
            Mode::Buttons | Mode::ClosedTooltips => div()
                .w(px(1200.))
                .flex()
                .flex_wrap()
                .gap(px(4.))
                .children((0_usize..1000).map(|i| {
                    let button = Button::new(("trigger", i))
                        .scope(self.scope.clone())
                        .label("Action");
                    match self.mode {
                        Mode::Unique => div()
                            .p(px(180.))
                            .flex()
                            .gap(px(140.))
                            .children((0..2).map(|i| {
                                Tooltip::new(("unique", i))
                                    .scope(self.scope.clone())
                                    .unique(true)
                                    .handle(&self.unique_handles[i])
                                    .child(Button::new(("trigger", i)).label("Hover"))
                                    .title_content(move |_, _, _| {
                                        div()
                                            .w(px(60. + i as f32 * 140.))
                                            .h(px(22. + i as f32 * 42.))
                                            .child("Content")
                                    })
                            }))
                            .into_any_element(),
                        Mode::Nested => {
                            let scope = self.scope.clone();
                            let nested = self.nested_handle.clone();
                            div()
                                .p(px(200.))
                                .child(
                                    Popover::new("parent")
                                        .scope(scope.clone())
                                        .handle(&self.handle)
                                        .default_open(true)
                                        .title("Parent")
                                        .child(Button::new("trigger").label("Open"))
                                        .content(move |_, _, _| {
                                            Popover::new("nested")
                                                .scope(scope.clone())
                                                .handle(&nested)
                                                .default_open(true)
                                                .placement(PopupPlacement::Right)
                                                .title("Nested")
                                                .text("Nested content")
                                                .child(
                                                    Button::new("nested-trigger").label("Nested"),
                                                )
                                        }),
                                )
                                .into_any_element()
                        }
                        Mode::Buttons => button.into_any_element(),
                        _ => Tooltip::new(("tooltip", i))
                            .scope(self.scope.clone())
                            .title("More details")
                            .child(button)
                            .into_any_element(),
                    }
                }))
                .into_any_element(),
        }
    }
}

fn main() {
    if cfg!(debug_assertions) {
        eprintln!("Popup benchmarks require Release");
        std::process::exit(2);
    }
    let geometry = PopupGeometry {
        anchor: Bounds::new(point(px(4.), px(4.)), size(px(80.), px(32.))),
        popup_size: size(px(250.), px(100.)),
        viewport: Bounds::new(point(px(0.), px(0.)), size(px(1200.), px(900.))),
        placement: PopupPlacement::Top,
        arrow: PopupArrow::default(),
        arrow_size: px(16.),
        gap: px(4.),
        radius: px(8.),
        offset: point(px(0.), px(0.)),
        auto_adjust_overflow: true,
    };
    measure("popup/placement/flip-shift", 100_000, || {
        black_box(black_box(geometry).resolve());
    });
    let mut cx = TestAppContext::single();
    let mut view = None;
    let window = cx.open_window(size(px(1200.), px(900.)), |window, cx| {
        let scope = ConfigScope::new_window(cx);
        scope
            .update_patch(cx, |patch| patch.motion = Some(false))
            .unwrap();
        let scene = cx.new(|_| Scene {
            scope: scope.clone(),
            mode: Mode::Buttons,
            width: 280.,
            handle: PopupHandle::new(),
            nested_handle: PopupHandle::new(),
            unique_handles: std::array::from_fn(|_| PopupHandle::new()),
        });
        view = Some(scene.clone());
        let cached = cx.new(|_| CachedScene(scene));
        UiRoot::new(scope, cached, window, cx)
    });
    let view = view.unwrap();
    let draw = |cx: &mut TestAppContext, rerender| {
        cx.update_window(window.into(), |_, window, cx| {
            if rerender {
                view.update(cx, |_, cx| cx.notify());
            }
            window.draw(cx).clear(cx);
            assert_eq!(
                window.simulate_next_frame(cx),
                0,
                "settled popups must not schedule frames"
            );
        })
        .unwrap();
    };
    for (mode, label) in [
        (Mode::Buttons, "buttons1000"),
        (Mode::ClosedTooltips, "closed-tooltips1000"),
        (Mode::Nested, "nested-popovers"),
        (Mode::Confirmation, "open-confirmation"),
    ] {
        view.update(&mut cx, |scene, cx| {
            scene.mode = mode;
            cx.notify();
        });
        draw(&mut cx, true);
        measure(&format!("popup/{label}/cached-scene"), 200, || {
            draw(&mut cx, false)
        });
        measure(&format!("popup/{label}/rerender"), 200, || {
            draw(&mut cx, true)
        });
    }
    let handle = view.read_with(&cx, |scene, _| scene.handle.clone());
    let before = cx.update(|cx| popup_shadow_statistics(cx));
    measure("popup/force-align/warm-shadow", 500, || {
        cx.update(|cx| handle.force_align(cx));
        draw(&mut cx, false);
    });
    measure("popup/reopen/warm-shadow", 200, || {
        for open in [false, true] {
            cx.update_window(window.into(), |_, window, cx| {
                handle.set_open(open, window, cx)
            })
            .unwrap();
            draw(&mut cx, false);
        }
    });
    let after = cx.update(|cx| popup_shadow_statistics(cx));
    assert_eq!(
        before.misses, after.misses,
        "alignment and reopening must reuse the shadow raster"
    );
    let mut width = 300.;
    measure("popup/shadow/cold-size-change", 30, || {
        width += 1.;
        view.update(&mut cx, |scene, cx| {
            scene.width = width;
            cx.notify();
        });
        draw(&mut cx, false);
    });
    let stats = cx.update(|cx| popup_shadow_statistics(cx));
    assert!(stats.entries <= 64 && stats.bytes <= 16 * 1024 * 1024);
    println!("popup/shadows: {stats:?}");
    view.update(&mut cx, |scene, cx| {
        scene.mode = Mode::Unique;
        scene
            .scope
            .update_patch(cx, |patch| patch.motion = Some(true))
            .unwrap();
        cx.notify();
    });
    draw(&mut cx, true);
    let handles = view.read_with(&cx, |scene, _| scene.unique_handles.clone());
    cx.update_window(window.into(), |_, window, cx| {
        handles[0].set_open(true, window, cx)
    })
    .unwrap();
    cx.update_window(window.into(), |_, window, cx| window.draw(cx).clear(cx))
        .unwrap();
    cx.executor().advance_clock(Duration::from_millis(300));
    cx.update_window(window.into(), |_, window, cx| {
        window.simulate_next_frame(cx);
        window.draw(cx).clear(cx);
    })
    .unwrap();
    let before = cx.update(|cx| popup_shadow_statistics(cx));
    let mut frame = 0;
    measure("popup/unique-handoff/frame", 200, || {
        if frame % 20 == 0 {
            let owner = (frame / 20 + 1) % 2;
            cx.update_window(window.into(), |_, window, cx| {
                handles[owner].set_open(true, window, cx)
            })
            .unwrap();
        }
        cx.executor().advance_clock(Duration::from_millis(16));
        cx.update_window(window.into(), |_, window, cx| {
            window.simulate_next_frame(cx);
            window.draw(cx).clear(cx);
        })
        .unwrap();
        frame += 1;
    });
    let after = cx.update(|cx| popup_shadow_statistics(cx));
    println!(
        "popup/unique-handoff/shadows: hits={}, misses={}, bytes={}",
        after.hits - before.hits,
        after.misses - before.misses,
        after.bytes
    );
    draw(&mut cx, false);
}
