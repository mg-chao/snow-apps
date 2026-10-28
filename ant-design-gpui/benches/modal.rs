// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
//! Focused Release CPU/layout measurements, not GPU presentation or input latency.
use ant_design_gpui::{
    ConfigScope, Modal, ModalManager, ModalState, UiRoot,
    modal::{ModalContent, ModalOptions, ModalTheme},
};
use gpui::{
    AppContext, Context, Entity, IntoElement, Render, TestAppContext, Window, WindowHandle, div,
    prelude::*, px, size,
};
use std::{cell::Cell, hint::black_box, rc::Rc, time::Instant};

fn measure(name: &str, n: usize, mut work: impl FnMut()) {
    for _ in 0..10 {
        work();
    }
    let mut times = Vec::with_capacity(n);
    for _ in 0..n {
        let t = Instant::now();
        work();
        times.push(t.elapsed().as_secs_f64() * 1e6);
    }
    times.sort_by(f64::total_cmp);
    println!(
        "{name}: n={n}, mean={:.3} us, p50={:.3} us, p95={:.3} us",
        times.iter().sum::<f64>() / n as f64,
        times[n / 2],
        times[n * 95 / 100]
    );
}
struct Scene {
    states: Vec<Entity<ModalState>>,
    scope: ConfigScope,
}
impl Render for Scene {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        div().size_full().children(
            self.states
                .iter()
                .enumerate()
                .map(|(i, s)| Modal::new(("modal", i), s, cx).scope(self.scope.clone())),
        )
    }
}
struct Body;
impl Render for Body {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        div()
            .flex()
            .flex_col()
            .gap(px(8.))
            .children((0..20).map(|i| div().child(format!("Retained content row {i}"))))
    }
}
struct Fixture {
    cx: TestAppContext,
    window: WindowHandle<UiRoot>,
    states: Vec<Entity<ModalState>>,
    scope: ConfigScope,
    constructions: Rc<Cell<usize>>,
}
impl Fixture {
    fn new(count: usize) -> Self {
        let mut cx = TestAppContext::single();
        let mut data = None;
        let constructions = Rc::new(Cell::new(0));
        let counts = constructions.clone();
        let window = cx.add_window(|w, cx| {
            let scope = ConfigScope::new_window(cx);
            scope.update_patch(cx, |p| p.motion = Some(false)).unwrap();
            let states = (0..count)
                .map(|_| {
                    cx.new(|cx| {
                        let mut s = ModalState::new(w, cx);
                        s.set_scope(scope.clone(), cx);
                        let counts = counts.clone();
                        s.set_options(
                            ModalOptions {
                                title: Some(ModalContent::text("Modal benchmark")),
                                content: Some(ModalContent::view(move |_, cx| {
                                    counts.set(counts.get() + 1);
                                    cx.new(|_| Body).into()
                                })),
                                ..Default::default()
                            },
                            cx,
                        );
                        s
                    })
                })
                .collect::<Vec<_>>();
            data = Some((states.clone(), scope.clone()));
            let scene = cx.new(|_| Scene {
                states,
                scope: scope.clone(),
            });
            UiRoot::new(scope, scene, w, cx)
        });
        let (states, scope) = data.unwrap();
        let mut f = Self {
            cx,
            window,
            states,
            scope,
            constructions,
        };
        f.draw(false);
        f
    }
    fn draw(&mut self, refresh: bool) {
        self.cx.run_until_parked();
        self.cx
            .update_window(self.window.into(), |_, w, cx| {
                if refresh {
                    w.refresh();
                }
                w.draw(cx).clear(cx)
            })
            .unwrap();
        self.cx.run_until_parked();
    }
    fn open(&mut self, open: bool) {
        let state = self.states[0].clone();
        self.cx
            .update_window(self.window.into(), |_, w, cx| {
                state.update(cx, |s, cx| s.set_open(open, w, cx))
            })
            .unwrap();
        self.draw(false);
    }
    fn idle(&mut self) {
        let frames = self
            .cx
            .update_window(self.window.into(), |_, w, cx| w.simulate_next_frame(cx))
            .unwrap();
        assert_eq!(frames, 0, "settled Modal must not request animation frames");
    }
}
fn main() {
    let theme = ant_design_gpui::theme::ThemeConfig::default().resolve();
    measure("cached_token_resolution", 1000, || {
        for _ in 0..100 {
            black_box(theme.component::<ModalTheme>());
        }
    });
    let mut f = Fixture::new(100);
    assert_eq!(f.constructions.get(), 0);
    measure("100_closed_draw", 200, || f.draw(false));
    measure("100_closed_invalidated", 200, || f.draw(true));
    assert_eq!(f.constructions.get(), 0);
    f.idle();
    drop(f);
    let mut f = Fixture::new(1);
    f.open(true);
    assert_eq!(f.constructions.get(), 1);
    measure("one_open_20_rows_draw", 200, || f.draw(false));
    measure("one_open_20_rows_invalidated", 200, || f.draw(true));
    f.idle();
    measure("retained_close_reopen", 100, || {
        f.open(false);
        f.open(true);
    });
    assert_eq!(f.constructions.get(), 1);
    let mut narrow = false;
    measure("resize_dialog", 100, || {
        narrow = !narrow;
        f.cx.update_window(f.window.into(), |_, w, cx| {
            w.resize(size(px(if narrow { 360. } else { 1000. }), px(600.)));
            w.refresh();
            w.draw(cx).clear(cx);
        })
        .unwrap();
        f.cx.run_until_parked();
    });
    f.idle();
    let (manager, handles) =
        f.cx.update_window(f.window.into(), |_, w, cx| {
            let manager = ModalManager::new(f.scope.clone(), w);
            let handles = (0..5)
                .map(|_| {
                    manager.confirm(
                        ModalOptions {
                            title: Some(ModalContent::text("Nested confirmation")),
                            content: Some(ModalContent::text("A scoped dialog")),
                            ..Default::default()
                        },
                        w,
                        cx,
                    )
                })
                .collect::<Vec<_>>();
            (manager, handles)
        })
        .unwrap();
    f.draw(false);
    measure("six_stacked_dialogs", 100, || f.draw(true));
    f.idle();
    f.cx.update_window(f.window.into(), |_, w, cx| manager.destroy_all(w, cx))
        .unwrap();
    f.draw(false);
    f.draw(false);
    assert!(
        handles.iter().all(|h| h.state().is_none()),
        "closed helper controllers leaked"
    );
    f.open(false);
    f.idle();
    println!(
        "Verified: closed_lazy_constructions=0; retained_body_constructions=1; idle_frames=0; released_helpers=5/5"
    );
}
