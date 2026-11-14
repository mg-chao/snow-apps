// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
//! Focused Release CPU/layout measurements. Run scripts/run-image-perf.ps1.
use ant_design_gpui::{
    ConfigScope, Image, ImageState, PreviewGroup, UiRoot,
    image::{ImageProgress, ImageTheme, PreviewAction, PreviewItem, PreviewState},
    theme::ThemeConfig,
};
use gpui::{
    AppContext, Context, Entity, IntoElement, Render, TestAppContext, Window, WindowHandle, div,
    prelude::*, px, size,
};
use std::{
    hint::black_box,
    sync::Arc,
    time::{Duration, Instant},
};

fn sampled(name: &str, mut operation: impl FnMut()) {
    let mut samples = Vec::with_capacity(100);
    for _ in 0..100 {
        let started = Instant::now();
        operation();
        samples.push(started.elapsed().as_secs_f64() * 1e6);
    }
    samples.sort_by(f64::total_cmp);
    println!(
        "{name}: median={:.2} us p95={:.2} us n=100",
        samples[50], samples[95]
    );
}

#[derive(Clone, Copy)]
enum Mode {
    Display,
    PreviewEnabled,
    Indeterminate,
    Determinate,
}

struct Bench {
    scope: ConfigScope,
    images: Vec<Entity<ImageState>>,
    group: Entity<PreviewState>,
    mode: Mode,
}
impl Render for Bench {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        PreviewGroup::new("bench-group", &self.group).child(
            div()
                .grid()
                .grid_cols(10)
                .gap(px(8.))
                .children(self.images.iter().enumerate().map(|(index, state)| {
                    let image = Image::new(("bench-image", index), state, cx)
                        .scope(self.scope.clone())
                        .w(px(120.))
                        .h(px(75.));
                    match self.mode {
                        Mode::Display => image.preview(false),
                        Mode::PreviewEnabled => image.preview_group(&self.group),
                        Mode::Indeterminate => image.progress(ImageProgress::default()),
                        Mode::Determinate => image.progress(ImageProgress {
                            percent: Some(65.),
                            ..Default::default()
                        }),
                    }
                })),
        )
    }
}
fn draw(window: WindowHandle<UiRoot>, cx: &mut TestAppContext) {
    cx.update_window(window.into(), |_, window, cx| window.draw(cx).clear(cx))
        .unwrap();
    cx.run_until_parked();
}

fn require_release() {
    #[cfg(debug_assertions)]
    panic!("Image measurements require Release");
}
fn main() {
    require_release();
    let source = Arc::new(gpui::Image::from_bytes(
        gpui::ImageFormat::Png,
        include_bytes!("../resources/image/landscape.png").to_vec(),
    ));
    let theme = ThemeConfig::default().resolve();
    let started = Instant::now();
    for _ in 0..200_000 {
        black_box(theme.component::<ImageTheme>());
    }
    println!(
        "image/theme/cached-lookup: {:.1} ns/iteration n=200000",
        started.elapsed().as_nanos() as f64 / 200_000.
    );
    for count in [1, 20, 100] {
        let mut cx = TestAppContext::single();
        let mut retained = None;
        let window = cx.open_window(size(px(1400.), px(1000.)), |window, cx| {
            ant_design_gpui::init(cx);
            let scope = ConfigScope::new_window(cx);
            scope
                .update_patch(cx, |patch| patch.motion = Some(false))
                .unwrap();
            let images = (0..count)
                .map(|_| cx.new(|cx| ImageState::new(source.clone(), window, cx)))
                .collect();
            let group = cx.new(|cx| {
                PreviewState::new(
                    (0..count)
                        .map(|index| PreviewItem::new(format!("item-{index}"), source.clone()))
                        .collect(),
                    window,
                    cx,
                )
            });
            group.update(cx, |state, cx| state.set_scope(scope.clone(), cx));
            let content = cx.new(|_| Bench {
                scope: scope.clone(),
                images,
                group,
                mode: Mode::Display,
            });
            retained = Some(content.clone());
            UiRoot::new(scope, content, window, cx)
        });
        let view = retained.unwrap();
        for (mode, name) in [
            (Mode::Display, "display"),
            (Mode::PreviewEnabled, "preview-enabled"),
            (Mode::Indeterminate, "indeterminate-reduced-motion"),
            (Mode::Determinate, "determinate-reduced-motion"),
        ] {
            view.update(&mut cx, |view, cx| {
                view.mode = mode;
                cx.notify();
            });
            for _ in 0..4 {
                draw(window, &mut cx);
                cx.executor().advance_clock(Duration::from_millis(100));
            }
            sampled(&format!("image/{count}/{name}/warm-draw"), || {
                cx.executor().advance_clock(Duration::from_millis(16));
                let callbacks = cx
                    .update_window(window.into(), |_, window, cx| {
                        window.simulate_next_frame(cx)
                    })
                    .unwrap();
                assert_eq!(
                    callbacks, 0,
                    "settled Image with motion disabled must not keep scheduling frames"
                );
                draw(window, &mut cx);
            });
        }
        let group = view.read_with(&cx, |view, _| view.group.clone());
        view.update(&mut cx, |view, cx| {
            view.mode = Mode::PreviewEnabled;
            cx.notify();
        });
        sampled(&format!("image/{count}/open-close"), || {
            cx.update_window(window.into(), |_, window, cx| {
                group.update(cx, |state, cx| state.set_open(true, window, cx))
            })
            .unwrap();
            draw(window, &mut cx);
            cx.update_window(window.into(), |_, window, cx| {
                group.update(cx, |state, cx| state.set_open(false, window, cx))
            })
            .unwrap();
            draw(window, &mut cx);
        });
        cx.update_window(window.into(), |_, window, cx| {
            group.update(cx, |state, cx| state.set_open(true, window, cx))
        })
        .unwrap();
        draw(window, &mut cx);
        let mut index = 0;
        sampled(&format!("image/{count}/navigate-transform"), || {
            cx.update_window(window.into(), |_, window, cx| {
                group.update(cx, |state, cx| {
                    state.set_current(index % count, cx);
                    index += 1;
                    state.apply_action(PreviewAction::RotateRight, window, cx);
                })
            })
            .unwrap();
            draw(window, &mut cx);
        });
    }
    println!("Verified: all warm reduced-motion Image scenes have zero idle animation callbacks");
}
