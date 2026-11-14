// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Focused Release CPU measurements. Run scripts/run-button-perf.ps1.

use std::{hint::black_box, time::Instant};

use ant_design_gpui::{
    Button, ConfigPatch, ConfigScope,
    button::{
        ButtonColor, ButtonStyle, ButtonStyleOptions, ButtonTheme, ButtonType, ButtonVariant,
    },
    icons::IconName,
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

struct Grid {
    scope: ConfigScope,
    icons: bool,
    dashed: bool,
}

impl Render for Grid {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        div()
            .flex()
            .flex_wrap()
            .gap(px(8.))
            .w(px(1200.))
            .children((0..100).map(|index| {
                Button::new(index)
                    .label("Button")
                    .scope(self.scope.clone())
                    .when(index % 2 == 0, Button::primary)
                    .when(self.icons, |button| button.icon(IconName::SearchOutlined))
                    .when(self.dashed, |button| button.button_type(ButtonType::Dashed))
            }))
    }
}

fn main() {
    if cfg!(debug_assertions) {
        eprintln!("Button benchmarks require Release");
        std::process::exit(2);
    }
    let theme = ThemeConfig::default().resolve();
    let tokens = theme.component::<ButtonTheme>();
    let options = ButtonStyleOptions {
        color: ButtonColor::Primary,
        variant: ButtonVariant::Solid,
        ..Default::default()
    };
    measure("button/style/cached-tokens", 200_000, || {
        black_box(ButtonStyle::from_tokens(&tokens, options));
    });
    measure("button/style/theme-lookup", 200_000, || {
        black_box(ButtonStyle::resolve(&theme, options));
    });
    measure("button/builder", 200_000, || {
        black_box(
            Button::new("bench")
                .label("Button")
                .primary()
                .icon(IconName::SearchOutlined),
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
            icons: false,
            dashed: false,
        }
    });
    let draw = |cx: &mut TestAppContext| {
        cx.update_window(window.into(), |_, window, cx| {
            window.draw(cx).clear(cx);
            assert_eq!(
                window.simulate_next_frame(cx),
                0,
                "idle Buttons must not schedule frames"
            );
        })
        .unwrap();
    };
    draw(&mut cx);
    measure("button/grid100/warm-draw", 2_000, || draw(&mut cx));
    cx.update_window(window.into(), |view, _, cx| {
        view.downcast::<Grid>().unwrap().update(cx, |grid, cx| {
            grid.icons = true;
            cx.notify();
        });
    })
    .unwrap();
    draw(&mut cx);
    measure("button/grid100/icons-warm-draw", 2_000, || draw(&mut cx));
    cx.update_window(window.into(), |view, _, cx| {
        view.downcast::<Grid>().unwrap().update(cx, |grid, cx| {
            grid.icons = false;
            grid.dashed = true;
            cx.notify();
        });
    })
    .unwrap();
    draw(&mut cx);
    measure("button/grid100/dashed-warm-draw", 2_000, || draw(&mut cx));
}
