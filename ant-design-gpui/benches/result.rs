// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Focused Release CPU measurements. Run scripts/run-result-perf.ps1.

use std::{hint::black_box, time::Instant};

use ant_design_gpui::{
    Button, ConfigScope, Direction, Result, ResultStatus, UiRoot,
    button::ButtonType,
    icons::{IconCacheStatistics, IconRenderer},
    result::{ResultStyle, ResultTheme},
    theme::ThemeConfig,
};
use gpui::{
    AppContext, Context, IntoElement, Render, TestAppContext, Window, WindowHandle, div,
    prelude::*, px, size,
};
use serde_json::{Value, json};

fn batch(name: &str, count: u32, mut operation: impl FnMut()) -> Value {
    let start = Instant::now();
    for _ in 0..count {
        operation();
    }
    let ns = start.elapsed().as_nanos() as f64 / f64::from(count);
    println!("{name}: {ns:.1} ns/iteration n={count}");
    json!({"name":name,"iterations":count,"nanoseconds_per_iteration":ns})
}

fn sample(name: &str, count: usize, mut operation: impl FnMut()) -> Value {
    let mut values = Vec::with_capacity(count);
    for _ in 0..count {
        let start = Instant::now();
        operation();
        values.push(start.elapsed().as_secs_f64() * 1e6);
    }
    values.sort_by(f64::total_cmp);
    if count == 1 {
        let duration = values[0];
        println!("{name}: {duration:.2} us n=1");
        return json!({"name":name,"iterations":1,"microseconds":duration});
    }
    let median = values[count / 2];
    let p95 = values[count * 95 / 100];
    println!("{name}: median={median:.2} us p95={p95:.2} us n={count}");
    json!({"name":name,"iterations":count,"median_microseconds":median,"p95_microseconds":p95})
}

#[derive(Clone, Copy)]
enum Mode {
    Status,
    Exception,
    Rich,
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
            .w(px(columns as f32 * 360.))
            .grid()
            .grid_cols(columns)
            .items_start()
            .children((0..self.count).map(|index| {
                let status = match self.mode {
                    Mode::Exception => [
                        ResultStatus::Forbidden,
                        ResultStatus::NotFound,
                        ResultStatus::InternalServerError,
                    ][index % 3],
                    _ => [
                        ResultStatus::Info,
                        ResultStatus::Success,
                        ResultStatus::Warning,
                        ResultStatus::Error,
                    ][index % 4],
                };
                let mut result = Result::new(("result-bench", index))
                    .scope(self.scope.clone())
                    .status(status)
                    .title("Operation complete")
                    .sub_title("Review the result and continue.");
                if matches!(self.mode, Mode::Rich) {
                    result = result
                        .extra(
                            Button::new(("result-action", index))
                                .scope(self.scope.clone())
                                .label("Continue")
                                .button_type(ButtonType::Primary)
                                .wave(false),
                        )
                        .child(div().child("Additional information about this operation."));
                }
                result
            }))
    }
}

fn draw(window: WindowHandle<UiRoot>, cx: &mut TestAppContext, refresh: bool) {
    cx.update_window(window.into(), |_, window, cx| {
        if refresh {
            window.refresh();
        }
        window.draw(cx).clear(cx);
        assert_eq!(window.simulate_next_frame(cx), 0, "Result must remain idle");
    })
    .unwrap();
}

fn stats(cx: &mut TestAppContext) -> IconCacheStatistics {
    cx.update(|cx| {
        cx.try_global::<IconRenderer>()
            .map(IconRenderer::statistics)
            .unwrap_or_default()
    })
}

fn main() {
    if cfg!(debug_assertions) {
        eprintln!("Result measurements require Release");
        std::process::exit(2);
    }
    let theme = ThemeConfig::default().resolve();
    let tokens = theme.component::<ResultTheme>();
    let mut measurements = vec![
        batch("result/style/cached-tokens", 200_000, || {
            black_box(ResultStyle::from_tokens(
                black_box(&tokens),
                ResultStatus::Success,
            ));
        }),
        batch("result/style/theme-lookup", 200_000, || {
            black_box(ResultStyle::resolve(black_box(&theme), ResultStatus::Info));
        }),
        batch("result/builder", 200_000, || {
            black_box(
                Result::new("bench")
                    .title("Operation complete")
                    .sub_title("Review the result and continue."),
            );
        }),
    ];
    let mut caches = Vec::new();
    for (count, scale) in [1, 20, 100]
        .into_iter()
        .flat_map(|count| [1., 1.5, 2.].map(move |scale| (count, scale)))
    {
        let mut cx = TestAppContext::single();
        let mut content = None;
        let window = cx.open_window(size(px(3600.), px(6500.)), |window, cx| {
            ant_design_gpui::init(cx);
            let scope = ConfigScope::new_window(cx);
            let view = cx.new(|_| Bench {
                scope: scope.clone(),
                count,
                mode: Mode::Status,
            });
            content = Some(view.clone());
            UiRoot::new(scope, view, window, cx)
        });
        let content = content.unwrap();
        cx.simulate_window_scale_factor_change(window.into(), scale);
        for direction in [Direction::Ltr, Direction::Rtl] {
            content.update(&mut cx, |view, cx| {
                view.scope
                    .update_patch(cx, |patch| patch.direction = Some(direction))
                    .unwrap();
            });
            for (mode, name) in [
                (Mode::Status, "status"),
                (Mode::Exception, "exception"),
                (Mode::Rich, "rich"),
            ] {
                content.update(&mut cx, |view, cx| {
                    view.mode = mode;
                    cx.notify();
                });
                measurements.push(sample(
                    &format!("result/{count}/{scale}x/{direction:?}/{name}/first-draw"),
                    1,
                    || draw(window, &mut cx, true),
                ));
                for _ in 0..3 {
                    draw(window, &mut cx, true);
                }
                let before = stats(&mut cx);
                measurements.push(sample(
                    &format!("result/{count}/{scale}x/{direction:?}/{name}/warm-redraw"),
                    100,
                    || draw(window, &mut cx, true),
                ));
                let after = stats(&mut cx);
                assert_eq!(
                    after.parse_misses, before.parse_misses,
                    "warm SVG parse reuse"
                );
                assert_eq!(
                    after.raster_misses, before.raster_misses,
                    "warm raster reuse"
                );
                assert!(
                    after.raster_entries <= 3,
                    "only three exception rasters per scale"
                );
                assert!(
                    after.resident_rasters <= 3,
                    "mounted instances share artwork"
                );
                caches.push(
                    json!({"count":count,"scale":scale,"direction":format!("{direction:?}"),"mode":name,
                    "warm_parse_misses":after.parse_misses-before.parse_misses,
                    "warm_raster_misses":after.raster_misses-before.raster_misses,
                    "raster_entries":after.raster_entries,"raster_bytes":after.raster_bytes,
                    "resident_bytes":after.resident_bytes}),
                );
            }
        }
    }
    let report = json!({
        "kind":"focused Release CPU and offscreen GPUI drawing with TestAppContext/NoopTextSystem",
        "idle_animation_callbacks":0,
        "warm_cache_gates_passed":true,
        "measurements":measurements,
        "cache_cases":caches,
        "limitations":[
            "Offscreen CPU measurements do not measure native GPU frame latency.",
            "The deterministic GPUI test text system does not measure production operating-system font shaping or glyph rasterization latency.",
            "First-draw is one observation after a mode/direction change; RTL reuses prior LTR caches. It is not a repeated cold-cache distribution."
        ]
    });
    if let Some(path) = std::env::var_os("ADGPUI_RESULT_PERF_OUTPUT") {
        let path = std::path::PathBuf::from(path);
        if let Some(parent) = path.parent() {
            std::fs::create_dir_all(parent).unwrap();
        }
        std::fs::write(path, serde_json::to_string_pretty(&report).unwrap() + "\n").unwrap();
    }
}
