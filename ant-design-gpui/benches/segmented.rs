// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Focused Release CPU measurements. Run scripts/run-segmented-perf.ps1.

use std::{hint::black_box, time::Instant};

use ant_design_gpui::{
    ConfigPatch, ConfigScope, Segmented, SegmentedOption,
    icons::IconName,
    segmented::{SegmentedShape, SegmentedStyle, SegmentedStyleOptions, SegmentedTheme},
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
    Text,
    Round,
    Icons,
    Vertical,
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
            .w(px(1600.))
            .children((0..100).map(|index| {
                Segmented::new(index)
                    .scope(self.scope.clone())
                    .block(true)
                    .vertical(matches!(self.mode, Mode::Vertical))
                    .shape(if matches!(self.mode, Mode::Round) {
                        SegmentedShape::Round
                    } else {
                        SegmentedShape::Default
                    })
                    .value((index % 3) as u8)
                    .options(["Daily", "Weekly", "Monthly"].into_iter().enumerate().map(
                        |(option_index, label)| {
                            let option =
                                SegmentedOption::new(option_index, option_index as u8, label);
                            if matches!(self.mode, Mode::Icons) {
                                option.icon(match option_index {
                                    0 => IconName::BarsOutlined,
                                    1 => IconName::AppstoreOutlined,
                                    _ => IconName::LineChartOutlined,
                                })
                            } else {
                                option
                            }
                        },
                    ))
            }))
    }
}

fn main() {
    if cfg!(debug_assertions) {
        eprintln!("Segmented benchmarks require Release");
        std::process::exit(2);
    }
    let theme = ThemeConfig::default().resolve();
    let tokens = theme.component::<SegmentedTheme>();
    let options = SegmentedStyleOptions::default();
    measure("segmented/style/cached-tokens", 200_000, || {
        black_box(SegmentedStyle::from_tokens(black_box(&tokens), options));
    });
    measure("segmented/style/theme-lookup", 200_000, || {
        black_box(SegmentedStyle::resolve(black_box(&theme), options));
    });
    measure("segmented/options3/builder", 200_000, || {
        black_box(Segmented::new("bench").value(1).options([
            SegmentedOption::new("daily", 0, "Daily"),
            SegmentedOption::new("weekly", 1, "Weekly"),
            SegmentedOption::new("monthly", 2, "Monthly"),
        ]));
    });
    measure("segmented/options100/builder", 10_000, || {
        black_box(
            Segmented::new("bench")
                .value(50)
                .options((0..100).map(|index| SegmentedOption::new(index, index, "Option"))),
        );
    });

    let mut cx = TestAppContext::single();
    let window = cx.open_window(size(px(1600.), px(1200.)), |_, cx| {
        let scope = ConfigScope::new_window(cx);
        scope
            .set_patch(
                ConfigPatch {
                    motion: Some(false),
                    ..Default::default()
                },
                cx,
            )
            .unwrap();
        Grid {
            scope,
            mode: Mode::Text,
        }
    });
    let draw = |cx: &mut TestAppContext, rerender: bool| {
        cx.update_window(window.into(), |view, window, cx| {
            if rerender {
                view.downcast::<Grid>()
                    .unwrap()
                    .update(cx, |_, cx| cx.notify());
            }
            window.draw(cx).clear(cx);
            assert_eq!(
                window.simulate_next_frame(cx),
                0,
                "idle Segmented groups must not schedule animation frames",
            );
        })
        .unwrap();
    };
    for (mode, label) in [
        (Mode::Text, "segmented/groups100/text"),
        (Mode::Round, "segmented/groups100/round"),
        (Mode::Icons, "segmented/groups100/icons"),
        (Mode::Vertical, "segmented/groups100/vertical"),
    ] {
        cx.update_window(window.into(), |view, _, cx| {
            view.downcast::<Grid>().unwrap().update(cx, |grid, cx| {
                grid.mode = mode;
                cx.notify();
            });
        })
        .unwrap();
        draw(&mut cx, false);
        measure(&format!("{label}/warm-draw"), 2_000, || {
            draw(&mut cx, false)
        });
        measure(&format!("{label}/rerender"), 2_000, || draw(&mut cx, true));
    }
}
