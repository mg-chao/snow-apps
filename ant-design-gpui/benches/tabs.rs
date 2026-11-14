// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
//! Focused Release measurements; run scripts/run-tabs-perf.ps1.
use ant_design_gpui::{ConfigScope, TabItem, Tabs, TabsItems, TabsState, UiRoot};
use gpui::{
    AppContext, Context, Entity, IntoElement, Render, TestAppContext, Window, div, prelude::*, px,
    size,
};
use std::{hint::black_box, time::Instant};

fn measure(name: &str, iterations: usize, mut operation: impl FnMut()) {
    let mut elapsed = Vec::with_capacity(iterations);
    for _ in 0..iterations {
        let start = Instant::now();
        operation();
        elapsed.push(start.elapsed().as_secs_f64() * 1e6);
    }
    elapsed.sort_by(f64::total_cmp);
    println!(
        "{name}: median={:.2} us p95={:.2} us n={iterations}",
        elapsed[iterations / 2],
        elapsed[iterations * 95 / 100]
    );
}
struct Bench {
    scope: ConfigScope,
    tabs: Entity<TabsState>,
}
impl Render for Bench {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        div().w(px(800.)).h(px(400.)).child(
            Tabs::new("tabs", &self.tabs)
                .scope(self.scope.clone())
                .w_full(),
        )
    }
}
fn main() {
    if cfg!(debug_assertions) {
        eprintln!("Tabs measurements require Release");
        std::process::exit(2);
    }
    for count in [3, 30, 100, 1_000] {
        let items = TabsItems::new(
            (0..count).map(|i| TabItem::new(i.to_string(), format!("Tab {i}")).text("Content")),
        )
        .unwrap();
        measure(&format!("tabs/{count}/snapshot-clone"), 10_000, || {
            black_box(items.clone());
        });
        let mut cx = TestAppContext::single();
        let mut controller = None;
        let window = cx.open_window(size(px(900.), px(500.)), |window, cx| {
            let scope = ConfigScope::new_window(cx);
            scope.update_patch(cx, |p| p.motion = Some(false)).unwrap();
            let tabs = cx.new(|cx| TabsState::new(items, window, cx));
            controller = Some(tabs.clone());
            let view = cx.new(|_| Bench {
                scope: scope.clone(),
                tabs,
            });
            UiRoot::new(scope, view, window, cx)
        });
        let state = controller.unwrap();
        let draw = |cx: &mut TestAppContext, select: Option<usize>| {
            cx.update_window(window.into(), |_, window, cx| {
                if let Some(select) = select {
                    state.update(cx, |s, cx| {
                        s.set_active_key(Some(select.to_string().into()), cx)
                    });
                }
                window.draw(cx).clear(cx);
                assert_eq!(
                    window.simulate_next_frame(cx),
                    0,
                    "idle Tabs must not schedule frames"
                );
            })
            .unwrap();
            cx.run_until_parked();
        };
        for _ in 0..4 {
            draw(&mut cx, None);
        }
        measure(&format!("tabs/{count}/warm-draw"), 200, || {
            draw(&mut cx, None)
        });
        let mut i = 0;
        measure(&format!("tabs/{count}/selection-and-layout"), 200, || {
            i = (i + 1) % count;
            draw(&mut cx, Some(i));
        });
    }
}
