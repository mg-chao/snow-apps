// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Focused Release CPU measurements. Run scripts/run-checkbox-perf.ps1.

use std::{hint::black_box, time::Instant};

use ant_design_gpui::{
    Checkbox, CheckboxGroup, CheckboxOption, ConfigPatch, ConfigScope,
    checkbox::{CheckboxStyle, CheckboxTheme},
    theme::ThemeConfig,
};
use gpui::{Context, IntoElement, Render, TestAppContext, Window, div, prelude::*, px};

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
    Checked,
    Mixed,
    Group,
}

struct Grid {
    scope: ConfigScope,
    mode: Mode,
}

impl Render for Grid {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        let content = div().w(px(1200.)).flex().flex_wrap().gap(px(8.));
        match self.mode {
            Mode::Checked | Mode::Mixed => content
                .children((0..100).map(|index| {
                    Checkbox::new(index)
                        .label("Checkbox")
                        .scope(self.scope.clone())
                        .checked(index % 2 == 0)
                        .indeterminate(matches!(self.mode, Mode::Mixed) && index % 3 == 0)
                        .disabled(index % 5 == 0)
                }))
                .into_any_element(),
            Mode::Group => content
                .child(
                    CheckboxGroup::new("choices")
                        .scope(self.scope.clone())
                        .accessibility_label("Choices")
                        .options((0..100).map(|index| {
                            CheckboxOption::new(index, index, "Checkbox").disabled(index % 5 == 0)
                        }))
                        .value((0..100).filter(|index| index % 2 == 0)),
                )
                .into_any_element(),
        }
    }
}

fn main() {
    if cfg!(debug_assertions) {
        eprintln!("Checkbox benchmarks require Release");
        std::process::exit(2);
    }
    let theme = ThemeConfig::default().resolve();
    let tokens = theme.component::<CheckboxTheme>();
    measure("checkbox/style/cached-tokens", 200_000, || {
        black_box(CheckboxStyle::from_tokens(
            &tokens,
            black_box(true),
            false,
            false,
        ));
    });
    measure("checkbox/style/theme-lookup", 200_000, || {
        black_box(CheckboxStyle::resolve(
            &theme,
            black_box(true),
            false,
            false,
        ));
    });
    measure("checkbox/builder", 200_000, || {
        black_box(Checkbox::new("bench").label("Checkbox").checked(true));
    });
    measure("checkbox/group100/builder", 10_000, || {
        black_box(
            CheckboxGroup::new("bench")
                .options((0..100).map(|index| CheckboxOption::new(index, index, "Checkbox")))
                .value((0..100).filter(|index| index % 2 == 0)),
        );
    });

    let mut cx = TestAppContext::single();
    let window = cx.add_window(|_, cx| {
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
            mode: Mode::Checked,
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
                "idle Checkboxes must not schedule animation frames",
            );
        })
        .unwrap();
    };
    for (mode, label) in [
        (Mode::Checked, "checkbox/grid100"),
        (Mode::Mixed, "checkbox/mixed100"),
        (Mode::Group, "checkbox/group100"),
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
