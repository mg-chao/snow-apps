// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Focused Release CPU measurements. Run scripts/run-alert-perf.ps1.

use std::{
    cell::Cell,
    hint::black_box,
    rc::Rc,
    time::{Duration, Instant},
};

use ant_design_gpui::{
    Alert, Button, ConfigScope, Direction, Size, UiRoot,
    alert::{AlertStyle, AlertTheme, AlertType, AlertVariant},
    behavior::focus_handle,
    button::ButtonType,
    theme::ThemeConfig,
};
use gpui::{
    AppContext, Context, FocusHandle, IntoElement, KeyDownEvent, KeyUpEvent, Keystroke,
    PlatformInput, Render, TestAppContext, Window, WindowHandle, div, prelude::*, px, size,
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
    Title,
    Description,
    Action,
}

struct Bench {
    scope: ConfigScope,
    count: usize,
    mode: Mode,
    epoch: usize,
    close_focus: FocusHandle,
    completions: Rc<Cell<usize>>,
}

impl Render for Bench {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        let columns = match self.count {
            1 => 1,
            20 => 5,
            _ => 10,
        };
        div()
            .w(px(columns as f32 * 240.))
            .grid()
            .grid_cols(columns)
            .items_start()
            .gap(px(8.))
            .children((0..self.count).map(|index| {
                let completions = self.completions.clone();
                let mut alert = Alert::new(("alert-bench", self.epoch * 1000 + index))
                    .scope(self.scope.clone())
                    .title("Status update")
                    .alert_type(
                        [
                            AlertType::Success,
                            AlertType::Info,
                            AlertType::Warning,
                            AlertType::Error,
                        ][index % 4],
                    )
                    .show_icon(true)
                    .closable(true)
                    .after_close(move |_, _| completions.set(completions.get() + 1));
                if index == 0 {
                    alert = alert.close_focus_handle(&self.close_focus);
                }
                if !matches!(self.mode, Mode::Title) {
                    alert = alert
                        .description("An additional description wraps within the available width.");
                }
                if matches!(self.mode, Mode::Action) {
                    alert = alert.action(
                        Button::new(("action", index))
                            .scope(self.scope.clone())
                            .label("Undo")
                            .size(Size::Small)
                            .button_type(ButtonType::Text)
                            .wave(false),
                    );
                }
                alert
            }))
    }
}

fn draw(window: WindowHandle<UiRoot>, cx: &mut TestAppContext) {
    cx.update_window(window.into(), |_, window, cx| window.draw(cx).clear(cx))
        .unwrap();
    cx.run_until_parked();
}

fn idle(window: WindowHandle<UiRoot>, cx: &mut TestAppContext) {
    draw(window, cx);
    cx.update_window(window.into(), |_, window, cx| {
        assert_eq!(
            window.simulate_next_frame(cx),
            0,
            "idle Alerts must not schedule frames"
        );
    })
    .unwrap();
}

fn main() {
    if cfg!(debug_assertions) {
        eprintln!("Alert measurements require Release");
        std::process::exit(2);
    }
    let theme = ThemeConfig::default().resolve();
    let tokens = theme.component::<AlertTheme>();
    measure_batch("alert/style/cached-tokens", 200_000, || {
        black_box(AlertStyle::from_tokens(
            black_box(&tokens),
            AlertType::Success,
            AlertVariant::Outlined,
            true,
            false,
        ));
    });
    measure_batch("alert/style/theme-lookup", 200_000, || {
        black_box(AlertStyle::resolve(
            black_box(&theme),
            AlertType::Info,
            AlertVariant::Filled,
            false,
            false,
        ));
    });
    measure_batch("alert/builder", 200_000, || {
        black_box(
            Alert::new("bench")
                .title("Status update")
                .description("Additional information")
                .show_icon(true)
                .closable(true),
        );
    });

    for count in [1, 20, 100] {
        let mut cx = TestAppContext::single();
        let mut content = None;
        let window = cx.open_window(size(px(2400.), px(1800.)), |window, cx| {
            ant_design_gpui::init(cx);
            let scope = ConfigScope::new_window(cx);
            let view = cx.new(|cx| Bench {
                scope: scope.clone(),
                count,
                mode: Mode::Title,
                epoch: 0,
                close_focus: focus_handle(cx),
                completions: Rc::default(),
            });
            content = Some(view.clone());
            UiRoot::new(scope, view, window, cx)
        });
        let content = content.unwrap();
        for (mode, name) in [
            (Mode::Title, "title"),
            (Mode::Description, "description"),
            (Mode::Action, "description-action"),
        ] {
            content.update(&mut cx, |view, cx| {
                view.mode = mode;
                cx.notify();
            });
            for _ in 0..3 {
                draw(window, &mut cx);
            }
            measure(&format!("alert/{count}/{name}/warm-draw"), 200, || {
                idle(window, &mut cx)
            });
        }
        if count > 1 {
            content.update(&mut cx, |view, cx| {
                view.scope
                    .update_patch(cx, |patch| patch.direction = Some(Direction::Rtl))
                    .unwrap();
                cx.notify();
            });
            for (mode, name) in [
                (Mode::Description, "description"),
                (Mode::Action, "description-action"),
            ] {
                content.update(&mut cx, |view, cx| {
                    view.mode = mode;
                    cx.notify();
                });
                for _ in 0..3 {
                    draw(window, &mut cx);
                }
                measure(&format!("alert/{count}/{name}/rtl-warm-draw"), 200, || {
                    idle(window, &mut cx)
                });
            }
            content.update(&mut cx, |view, cx| {
                view.scope
                    .update_patch(cx, |patch| patch.direction = Some(Direction::Ltr))
                    .unwrap();
                cx.notify();
            });
            draw(window, &mut cx);
        }
        measure(&format!("alert/{count}/close-and-layout"), 100, || {
            content.update(&mut cx, |view, cx| {
                view.epoch += 1;
                cx.notify();
            });
            draw(window, &mut cx);
            let (focus, completions) = content.read_with(&cx, |view, _| {
                (view.close_focus.clone(), view.completions.clone())
            });
            let before = completions.get();
            cx.update_window(window.into(), |_, window, cx| {
                window.focus(&focus, cx);
                let keystroke = Keystroke::parse("enter").unwrap();
                window.dispatch_event(
                    PlatformInput::KeyDown(KeyDownEvent {
                        keystroke: keystroke.clone(),
                        is_held: false,
                        prefer_character_input: false,
                    }),
                    cx,
                );
                window.dispatch_event(PlatformInput::KeyUp(KeyUpEvent { keystroke }), cx);
            })
            .unwrap();
            draw(window, &mut cx);
            // Advance virtual time: these measurements contain transition work,
            // without sleeping for the 300ms reference animation on every sample.
            for _ in 0..4 {
                cx.executor().advance_clock(Duration::from_millis(100));
                cx.update_window(window.into(), |_, window, cx| {
                    window.simulate_next_frame(cx);
                })
                .unwrap();
                draw(window, &mut cx);
            }
            idle(window, &mut cx);
            assert_eq!(
                completions.get(),
                before + 1,
                "close completion occurs exactly once"
            );
        });
    }
}
