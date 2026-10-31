// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Focused Release CPU measurements. Run scripts/run-divider-perf.ps1.

use std::{hint::black_box, time::Instant};

use ant_design_gpui::{
    ConfigScope, Divider,
    divider::{DividerStyle, DividerTheme, DividerTitlePlacement, DividerVariant},
    theme::ThemeConfig,
};
use gpui::{Context, IntoElement, Render, TestAppContext, Window, div, prelude::*, px, size};

fn measure(name: &str, iterations: u32, mut work: impl FnMut()) {
    let started = Instant::now();
    for _ in 0..iterations {
        work();
    }
    println!(
        "{name}: {iterations} iterations, {:.1} ns/iteration",
        started.elapsed().as_nanos() as f64 / f64::from(iterations)
    );
}

#[derive(Clone, Copy)]
enum Mode {
    Untitled,
    Titled,
    Dashed,
    Dotted,
}

struct Grid {
    scope: ConfigScope,
    mode: Mode,
}

impl Render for Grid {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        div()
            .grid()
            .grid_cols(10)
            .gap(px(8.))
            .w(px(1200.))
            .children((0..100).map(|index| {
                Divider::new(index)
                    .scope(self.scope.clone())
                    .title_placement(match index % 3 {
                        0 => DividerTitlePlacement::Start,
                        1 => DividerTitlePlacement::Center,
                        _ => DividerTitlePlacement::End,
                    })
                    .variant(match self.mode {
                        Mode::Dashed => DividerVariant::Dashed,
                        Mode::Dotted => DividerVariant::Dotted,
                        _ => DividerVariant::Solid,
                    })
                    .when(!matches!(self.mode, Mode::Untitled), |divider| {
                        divider.label("Text")
                    })
            }))
    }
}

fn main() {
    if cfg!(debug_assertions) {
        eprintln!("Divider benchmarks require Release");
        std::process::exit(2);
    }
    let theme = ThemeConfig::default().resolve();
    let tokens = theme.component::<DividerTheme>();
    measure("divider/style/cached-tokens", 200_000, || {
        black_box(DividerStyle::from_tokens(
            black_box(tokens.as_ref()),
            None,
            true,
            false,
        ));
    });
    measure("divider/style/theme-lookup", 200_000, || {
        black_box(DividerStyle::resolve(black_box(&theme), None, true, false));
    });
    measure("divider/builder", 200_000, || {
        black_box(
            Divider::new("bench")
                .label("Text")
                .title_placement(DividerTitlePlacement::Start)
                .variant(DividerVariant::Dashed),
        );
    });

    let mut cx = TestAppContext::single();
    let window = cx.open_window(size(px(1200.), px(900.)), |_, cx| Grid {
        scope: ConfigScope::new_window(cx),
        mode: Mode::Untitled,
    });
    let draw = |cx: &mut TestAppContext| {
        cx.update_window(window.into(), |_, window, cx| {
            window.draw(cx).clear(cx);
            assert_eq!(
                window.simulate_next_frame(cx),
                0,
                "idle Dividers must not schedule animation frames",
            );
        })
        .unwrap();
    };
    for (mode, label) in [
        (Mode::Untitled, "divider/grid100/solid-warm-draw"),
        (Mode::Titled, "divider/grid100/titled-warm-draw"),
        (Mode::Dashed, "divider/grid100/dashed-warm-draw"),
        (Mode::Dotted, "divider/grid100/dotted-warm-draw"),
    ] {
        cx.update_window(window.into(), |view, _, cx| {
            view.downcast::<Grid>().unwrap().update(cx, |grid, cx| {
                grid.mode = mode;
                cx.notify();
            });
        })
        .unwrap();
        draw(&mut cx);
        measure(label, 2_000, || draw(&mut cx));
    }
}
