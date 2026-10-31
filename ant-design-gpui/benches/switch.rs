// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Focused Release CPU measurements. Run scripts/run-switch-perf.ps1.

use std::{hint::black_box, time::Instant};

use ant_design_gpui::{
    ConfigScope, Size, Switch,
    icons::{IconName, IconPalette, icon},
    switch::{SwitchStyle, SwitchTheme},
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
    Plain,
    Labeled,
    Icons,
}

struct Grid {
    scope: ConfigScope,
    mode: Mode,
}

impl Render for Grid {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let config = self.scope.snapshot(cx);
        let palette = IconPalette {
            text: config.theme.tokens.color_text_light_solid.into(),
            ..config.icon_palette
        };
        div()
            .w(px(1200.))
            .p(px(16.))
            .flex()
            .flex_wrap()
            .gap(px(8.))
            .children((0..100).map(|index| {
                let switch = Switch::new(index)
                    .scope(self.scope.clone())
                    .accessibility_label("Notifications")
                    .size(if index % 3 == 0 {
                        Size::Small
                    } else {
                        Size::Medium
                    })
                    .checked(index % 2 == 0)
                    .disabled(index % 5 == 0);
                match self.mode {
                    Mode::Plain => switch,
                    Mode::Labeled => switch.checked_label("On").unchecked_label("Off"),
                    Mode::Icons => switch
                        .checked_content(
                            icon(IconName::CheckOutlined).size(px(12.)).palette(palette),
                        )
                        .unchecked_content(
                            icon(IconName::CloseOutlined).size(px(12.)).palette(palette),
                        ),
                }
            }))
    }
}

fn main() {
    if cfg!(debug_assertions) {
        eprintln!("Switch benchmarks require Release");
        std::process::exit(2);
    }
    let theme = ThemeConfig::default().resolve();
    let tokens = theme.component::<SwitchTheme>();
    measure("switch/style/cached-tokens", 200_000, || {
        black_box(SwitchStyle::from_tokens(
            black_box(tokens.as_ref()),
            Size::Medium,
            black_box(true),
            false,
            false,
        ));
    });
    measure("switch/style/theme-lookup", 200_000, || {
        black_box(SwitchStyle::resolve(
            black_box(&theme),
            Size::Medium,
            true,
            false,
            false,
        ));
    });
    measure("switch/builder", 200_000, || {
        black_box(
            Switch::new("bench")
                .checked(true)
                .checked_label("On")
                .unchecked_label("Off")
                .accessibility_label("Notifications"),
        );
    });

    let mut cx = TestAppContext::single();
    let window = cx.open_window(size(px(1200.), px(900.)), |_, cx| Grid {
        scope: ConfigScope::new_window(cx),
        mode: Mode::Plain,
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
                "idle Switches must not schedule animation frames",
            );
        })
        .unwrap();
    };
    for (mode, label) in [
        (Mode::Plain, "switch/plain100"),
        (Mode::Labeled, "switch/labeled100"),
        (Mode::Icons, "switch/icons100"),
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
