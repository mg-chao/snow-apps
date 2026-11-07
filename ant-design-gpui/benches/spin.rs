// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Focused Release CPU measurements. Run scripts/run-spin-perf.ps1.

use std::{
    hint::black_box,
    time::{Duration, Instant},
};

use ant_design_gpui::{
    ConfigScope, Size, Spin, UiRoot,
    spin::{SpinPercent, SpinStyle, SpinTheme},
    theme::ThemeConfig,
};
use gpui::{
    AppContext, Context, IntoElement, Render, TestAppContext, Window, WindowHandle, div,
    prelude::*, px, size,
};

fn measure_batch(name: &str, iterations: u32, mut operation: impl FnMut()) {
    let start = Instant::now();
    for _ in 0..iterations {
        operation();
    }
    println!(
        "{name}: {:.1} ns/iteration n={iterations}",
        start.elapsed().as_nanos() as f64 / f64::from(iterations)
    );
}

fn measure(name: &str, iterations: usize, mut operation: impl FnMut()) {
    let mut samples = Vec::with_capacity(iterations);
    for _ in 0..iterations {
        let start = Instant::now();
        operation();
        samples.push(start.elapsed().as_secs_f64() * 1e6);
    }
    samples.sort_by(f64::total_cmp);
    println!(
        "{name}: median={:.2} us p95={:.2} us n={iterations}",
        samples[iterations / 2],
        samples[iterations * 95 / 100]
    );
}

#[derive(Clone, Copy)]
enum Mode {
    Hidden,
    Dots,
    Progress,
    Auto,
    Nested,
    Reduced,
    Custom,
}

struct Bench {
    scope: ConfigScope,
    count: usize,
    mode: Mode,
}

impl Render for Bench {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        let columns = match self.count {
            1 => 1,
            20 => 5,
            _ => 10,
        };
        div()
            .w(px(columns as f32 * 200.))
            .grid()
            .grid_cols(columns)
            .items_start()
            .gap(px(8.))
            .children((0..self.count).map(|index| {
                let spin = Spin::new(("spin-bench", index))
                    .scope(self.scope.clone())
                    .size(Size::Medium)
                    .spinning(!matches!(self.mode, Mode::Hidden));
                let spin = match self.mode {
                    Mode::Progress => spin.percent(SpinPercent::Value(65.)),
                    Mode::Auto => spin.percent(SpinPercent::Auto),
                    Mode::Nested => spin.description("Loading...").child(
                        div()
                            .w(px(180.))
                            .h(px(90.))
                            .p(px(12.))
                            .child("Content being loaded"),
                    ),
                    Mode::Custom => spin.indicator_content(
                        div().size(px(20.)).rounded_full().bg(gpui::rgb(0x1677ff)),
                    ),
                    _ => spin,
                };
                div().w(px(190.)).h(px(100.)).child(spin)
            }))
    }
}

fn draw(window: WindowHandle<UiRoot>, cx: &mut TestAppContext) {
    cx.update_window(window.into(), |_, window, cx| window.draw(cx).clear(cx))
        .unwrap();
    cx.run_until_parked();
}

fn frame(window: WindowHandle<UiRoot>, cx: &mut TestAppContext, millis: u64) -> usize {
    cx.executor().advance_clock(Duration::from_millis(millis));
    let callbacks = cx
        .update_window(window.into(), |_, window, cx| {
            window.simulate_next_frame(cx)
        })
        .unwrap();
    draw(window, cx);
    callbacks
}

fn main() {
    if cfg!(debug_assertions) {
        eprintln!("Spin measurements require Release");
        std::process::exit(2);
    }
    let theme = ThemeConfig::default().resolve();
    let tokens = theme.component::<SpinTheme>();
    measure_batch("spin/style/cached-tokens", 200_000, || {
        black_box(SpinStyle::from_tokens(
            black_box(&tokens),
            Size::Medium,
            false,
        ));
    });
    measure_batch("spin/style/theme-lookup", 200_000, || {
        black_box(SpinStyle::resolve(black_box(&theme), Size::Large, true));
    });
    measure_batch("spin/builder", 200_000, || {
        black_box(
            Spin::new("bench")
                .size(Size::Large)
                .description("Loading...")
                .spinning(true),
        );
    });
    for count in [1, 20, 100] {
        let mut cx = TestAppContext::single();
        let mut content = None;
        let window = cx.open_window(size(px(2400.), px(1800.)), |window, cx| {
            ant_design_gpui::init(cx);
            let scope = ConfigScope::new_window(cx);
            let view = cx.new(|_| Bench {
                scope: scope.clone(),
                count,
                mode: Mode::Hidden,
            });
            content = Some(view.clone());
            UiRoot::new(scope, view, window, cx)
        });
        let content = content.unwrap();
        for (mode, name, animated) in [
            (Mode::Hidden, "hidden", false),
            (Mode::Dots, "dots", true),
            (Mode::Progress, "progress", false),
            (Mode::Auto, "auto-progress", true),
            (Mode::Nested, "nested", true),
            (Mode::Reduced, "reduced-motion", false),
            (Mode::Custom, "static-custom", false),
        ] {
            content.update(&mut cx, |view, cx| {
                view.mode = mode;
                view.scope
                    .update_patch(cx, |patch| {
                        patch.motion = Some(!matches!(mode, Mode::Reduced))
                    })
                    .unwrap();
                cx.notify();
            });
            draw(window, &mut cx);
            // Retargeted percent and content-opacity transitions settle in virtual time.
            for _ in 0..3 {
                frame(window, &mut cx, 500);
            }
            measure(&format!("spin/{count}/{name}/warm-draw"), 200, || {
                let callbacks = frame(window, &mut cx, 16);
                if animated {
                    assert!(
                        callbacks > 0,
                        "active animated indicators must request frames"
                    );
                    assert!(
                        callbacks <= count,
                        "rerenders must not accumulate frame callbacks"
                    );
                } else {
                    assert_eq!(
                        callbacks, 0,
                        "settled static Spinners must not request frames"
                    );
                }
            });
        }
        content.update(&mut cx, |view, cx| {
            view.mode = Mode::Hidden;
            cx.notify();
        });
        draw(window, &mut cx);
        frame(window, &mut cx, 1000);
        assert_eq!(
            frame(window, &mut cx, 1000),
            0,
            "stopping Spin cancels continuous work"
        );
    }
}
