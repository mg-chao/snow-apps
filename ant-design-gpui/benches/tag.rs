// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Focused Release CPU measurements. Run scripts/run-tag-perf.ps1.

use std::{hint::black_box, time::Instant};

use ant_design_gpui::{
    CheckableTag, ConfigScope, Tag,
    icons::IconName,
    tag::{TagColor, TagStyle, TagTheme, TagVariant},
    theme::{PresetColor, ThemeConfig},
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
    Static,
    Closable,
    Checkable,
    Checked,
    Disabled,
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
                if matches!(self.mode, Mode::Checkable | Mode::Checked | Mode::Disabled) {
                    CheckableTag::new(index)
                        .scope(self.scope.clone())
                        .label("Category")
                        .checked(matches!(self.mode, Mode::Checked))
                        .disabled(matches!(self.mode, Mode::Disabled))
                        .into_any_element()
                } else {
                    Tag::new(index)
                        .scope(self.scope.clone())
                        .label("Category")
                        .color(TagColor::Preset(
                            PresetColor::ALL[index % PresetColor::ALL.len()],
                        ))
                        .variant(match index % 3 {
                            0 => TagVariant::Filled,
                            1 => TagVariant::Solid,
                            _ => TagVariant::Outlined,
                        })
                        .when(matches!(self.mode, Mode::Closable), |tag| {
                            tag.closable(true).icon(IconName::TagOutlined)
                        })
                        .into_any_element()
                }
            }))
    }
}

fn main() {
    if cfg!(debug_assertions) {
        eprintln!("Tag benchmarks require Release");
        std::process::exit(2);
    }
    let theme = ThemeConfig::default().resolve();
    let tokens = theme.component::<TagTheme>();
    measure("tag/style/cached-tokens", 200_000, || {
        black_box(TagStyle::from_tokens(
            black_box(tokens.as_ref()),
            TagColor::Preset(PresetColor::Blue),
            TagVariant::Outlined,
            None,
            false,
        ));
    });
    measure("tag/style/theme-lookup", 200_000, || {
        black_box(TagStyle::resolve(
            black_box(&theme),
            TagColor::Success,
            TagVariant::Filled,
            None,
            false,
        ));
    });
    measure("tag/builder", 200_000, || {
        black_box(
            Tag::new("bench")
                .label("Category")
                .color(TagColor::Success)
                .variant(TagVariant::Outlined)
                .icon(IconName::CheckCircleOutlined)
                .closable(true),
        );
    });

    let mut cx = TestAppContext::single();
    let window = cx.open_window(size(px(1200.), px(900.)), |_, cx| {
        let scope = ConfigScope::new_window(cx);
        scope
            .update_patch(cx, |patch| patch.motion = Some(false))
            .unwrap();
        Grid {
            scope,
            mode: Mode::Static,
        }
    });
    let draw = |cx: &mut TestAppContext| {
        cx.update_window(window.into(), |_, window, cx| {
            window.draw(cx).clear(cx);
            assert_eq!(
                window.simulate_next_frame(cx),
                0,
                "idle Tags must not schedule animation frames"
            );
        })
        .unwrap();
    };
    for (mode, label) in [
        (Mode::Static, "tag/grid100/static-warm-draw"),
        (Mode::Closable, "tag/grid100/closable-icon-warm-draw"),
        (Mode::Checkable, "tag/grid100/checkable-warm-draw"),
        (Mode::Checked, "tag/grid100/checked-warm-draw"),
        (Mode::Disabled, "tag/grid100/disabled-warm-draw"),
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
