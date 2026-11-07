// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Focused Release CPU measurements. Run scripts/run-radio-perf.ps1.

use std::{hint::black_box, time::Instant};

use ant_design_gpui::{
    ConfigPatch, ConfigScope, Radio, RadioGroup, RadioOption, Size,
    radio::{RadioButtonStyle, RadioButtonVisualStyle, RadioOptionType, RadioStyle, RadioTheme},
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
    Standard,
    Button,
    Group,
    ButtonGroup,
}

impl Mode {
    fn option_type(self) -> RadioOptionType {
        match self {
            Self::Button | Self::ButtonGroup => RadioOptionType::Button,
            _ => RadioOptionType::Default,
        }
    }

    fn is_group(self) -> bool {
        matches!(self, Self::Group | Self::ButtonGroup)
    }
}

struct Grid {
    scope: ConfigScope,
    mode: Mode,
}

impl Render for Grid {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        let content = div().w(px(1200.)).flex().flex_wrap().gap(px(8.));
        if self.mode.is_group() {
            content
                .child(
                    RadioGroup::new("choices")
                        .scope(self.scope.clone())
                        .accessibility_label("Choices")
                        .option_type(self.mode.option_type())
                        .options((0..100).map(|index| {
                            RadioOption::new(index, index, "Radio").disabled(index % 5 == 0)
                        }))
                        .value(51),
                )
                .into_any_element()
        } else {
            content
                .children((0..100).map(|index| {
                    Radio::new(index)
                        .label("Radio")
                        .scope(self.scope.clone())
                        .option_type(self.mode.option_type())
                        .checked(index % 2 == 0)
                        .disabled(index % 5 == 0)
                }))
                .into_any_element()
        }
    }
}

fn main() {
    if cfg!(debug_assertions) {
        eprintln!("Radio benchmarks require Release");
        std::process::exit(2);
    }
    let theme = ThemeConfig::default().resolve();
    let tokens = theme.component::<RadioTheme>();
    measure("radio/style/cached-tokens", 200_000, || {
        black_box(RadioStyle::from_tokens(&tokens, black_box(true), false));
    });
    measure("radio/style/theme-lookup", 200_000, || {
        black_box(RadioStyle::resolve(&theme, black_box(true), false));
    });
    measure("radio/button-style/cached-tokens", 200_000, || {
        black_box(RadioButtonVisualStyle::from_tokens(
            &tokens,
            Size::Medium,
            RadioButtonStyle::Solid,
            black_box(true),
            false,
        ));
    });
    measure("radio/builder", 200_000, || {
        black_box(Radio::new("bench").label("Radio").checked(true));
    });
    measure("radio/group100/builder", 10_000, || {
        black_box(
            RadioGroup::new("bench")
                .options((0..100).map(|index| RadioOption::new(index, index, "Radio")))
                .value(51),
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
            mode: Mode::Standard,
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
                "idle Radios must not schedule animation frames",
            );
        })
        .unwrap();
    };
    for (mode, label) in [
        (Mode::Standard, "radio/grid100"),
        (Mode::Button, "radio/buttons100"),
        (Mode::Group, "radio/group100"),
        (Mode::ButtonGroup, "radio/button-group100"),
    ] {
        cx.update_window(window.into(), |view, _, cx| {
            view.downcast::<Grid>().unwrap().update(cx, |grid, cx| {
                grid.mode = mode;
                cx.notify();
            });
        })
        .unwrap();
        draw(&mut cx, false);
        let retained_focus = mode.is_group().then(|| {
            cx.update_window(window.into(), |_, window, cx| {
                window.blur(cx);
                window.focus_next(cx);
                window.focused(cx).expect("group has one enabled tab stop")
            })
            .unwrap()
        });
        measure(&format!("{label}/warm-draw"), 2_000, || {
            draw(&mut cx, false)
        });
        measure(&format!("{label}/rerender"), 2_000, || draw(&mut cx, true));
        if let Some(focus) = retained_focus {
            cx.update_window(window.into(), |_, window, cx| {
                assert_eq!(
                    window.focused(cx),
                    Some(focus),
                    "group retains focus identity"
                );
                window.blur(cx);
            })
            .unwrap();
        }
    }
}
