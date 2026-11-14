// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Focused Release CPU measurements. Run scripts/run-notification-perf.ps1.

use std::{
    hint::black_box,
    time::{Duration, Instant},
};

use ant_design_gpui::{
    ConfigScope, UiRoot,
    icons::IconRenderer,
    notification::{
        Notification, NotificationConfigPatch, NotificationHost, NotificationOptions,
        NotificationStack, NotificationState, NotificationTheme, NotificationType,
    },
    theme::ThemeConfig,
};
use gpui::{
    AppContext, Context, Entity, IntoElement, Render, TestAppContext, Window, WindowHandle, div,
    prelude::*, px, size,
};
use serde_json::{Value, json};

fn sample(name: &str, count: usize, mut operation: impl FnMut()) -> Value {
    let mut values = Vec::with_capacity(count);
    for _ in 0..count {
        let start = Instant::now();
        operation();
        values.push(start.elapsed().as_secs_f64() * 1e6);
    }
    summarize(name, values)
}

fn batch(name: &str, iterations: u32, mut operation: impl FnMut()) -> Value {
    let start = Instant::now();
    for _ in 0..iterations {
        operation();
    }
    let nanoseconds = start.elapsed().as_secs_f64() * 1e9 / f64::from(iterations);
    println!("{name}: {nanoseconds:.2} ns/iteration n={iterations}");
    json!({"name":name,"iterations":iterations,"nanoseconds_per_iteration":nanoseconds})
}

fn summarize(name: &str, mut values: Vec<f64>) -> Value {
    values.sort_by(f64::total_cmp);
    let count = values.len();
    let median = values[count / 2];
    let p95 = values[count * 95 / 100];
    println!("{name}: median={median:.2} us p95={p95:.2} us n={count}");
    json!({"name":name,"iterations":count,"median_microseconds":median,"p95_microseconds":p95})
}

struct Bench {
    notifications: Entity<NotificationState>,
}
impl Render for Bench {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        NotificationHost::new(
            "notification-benchmark",
            &self.notifications,
            div().size_full(),
        )
    }
}

fn frame(window: WindowHandle<UiRoot>, cx: &mut TestAppContext, millis: u64) -> usize {
    cx.executor().advance_clock(Duration::from_millis(millis));
    let count = cx
        .update_window(window.into(), |_, window, cx| {
            let count = window.simulate_next_frame(cx);
            window.refresh();
            window.draw(cx).clear(cx);
            count
        })
        .unwrap();
    cx.run_until_parked();
    count
}

fn main() {
    assert!(
        !black_box(cfg!(debug_assertions)),
        "Notification measurements require Release"
    );
    let theme = ThemeConfig::default().resolve();
    let mut measurements = vec![
        batch("notification/theme/cached-tokens", 200_000, || {
            black_box(theme.component::<NotificationTheme>());
        }),
        batch("notification/options", 200_000, || {
            black_box(
                NotificationOptions::new("Saved")
                    .kind(NotificationType::Success)
                    .duration(Duration::ZERO),
            );
        }),
    ];
    let mut cx = TestAppContext::single();
    let mut notifications = None;
    let mut scope = None;
    let window = cx.open_window(size(px(900.), px(6400.)), |window, cx| {
        ant_design_gpui::init(cx);
        let config = ConfigScope::new_window(cx);
        let state = cx.new(|cx| NotificationState::new(config.clone(), window, cx));
        notifications = Some(state.clone());
        scope = Some(config.clone());
        let content = cx.new(|_| Bench {
            notifications: state,
        });
        UiRoot::new(config, content, window, cx)
    });
    let api = Notification::new(&notifications.unwrap());
    let scope = scope.unwrap();
    frame(window, &mut cx, 0);
    assert_eq!(
        frame(window, &mut cx, 500),
        0,
        "empty host must remain idle"
    );
    for count in [1, 3, 20, 100] {
        for (name, stacked, progress, reduced) in [
            ("static", false, false, false),
            ("stacked", true, false, false),
            ("progress", false, true, false),
            ("reduced", false, true, true),
        ] {
            cx.update_window(window.into(), |_, window, cx| {
                api.destroy_all(window, cx);
                scope
                    .update_patch(cx, |patch| patch.motion = Some(!reduced))
                    .unwrap();
                api.set_config(
                    NotificationConfigPatch {
                        stack: Some(if stacked {
                            NotificationStack::default()
                        } else {
                            NotificationStack::Disabled
                        }),
                        ..Default::default()
                    },
                    window,
                    cx,
                );
                for index in 0..count {
                    api.open(
                        NotificationOptions::new(format!(
                            "Notification {index}: Operation complete"
                        ))
                        .key(format!("bench-{index}"))
                        .kind(if progress {
                            NotificationType::Info
                        } else {
                            NotificationType::Success
                        })
                        .show_progress(progress)
                        .description(
                            "Native content with measured wrapping and cached status icons.",
                        )
                        .duration(if progress {
                            Duration::from_secs(600)
                        } else {
                            Duration::ZERO
                        }),
                        window,
                        cx,
                    );
                }
            })
            .unwrap();
            for _ in 0..4 {
                frame(window, &mut cx, 500);
            }
            // Repeated invalidations before the next presentation share one wakeup.
            for _ in 0..8 {
                cx.update_window(window.into(), |_, window, cx| {
                    window.refresh();
                    window.draw(cx).clear(cx);
                })
                .unwrap();
            }
            assert_eq!(
                frame(window, &mut cx, 16),
                usize::from(progress),
                "rerenders coalesce into one controller frame callback"
            );
            let before = cx.update(|cx| cx.global::<IconRenderer>().statistics());
            measurements.push(sample(
                &format!("notification/{count}/{name}/warm-redraw"),
                100,
                || {
                    let callbacks = frame(window, &mut cx, 16);
                    if !progress {
                        assert_eq!(
                            callbacks, 0,
                            "settled static notifications schedule no frames"
                        );
                    } else {
                        assert_eq!(
                            callbacks, 1,
                            "one controller owns one guarded animation callback"
                        );
                    }
                },
            ));
            let after = cx.update(|cx| cx.global::<IconRenderer>().statistics());
            assert_eq!(
                before.parse_misses, after.parse_misses,
                "warm SVG parsing reuse"
            );
            assert_eq!(
                before.raster_misses, after.raster_misses,
                "warm SVG raster reuse"
            );
        }
    }
    let mut updates = Vec::with_capacity(1000);
    for _ in 0..1000 {
        cx.update_window(window.into(), |_, window, cx| {
            let options = NotificationOptions::new("Updated")
                .key("bench-0")
                .duration(Duration::ZERO);
            let start = Instant::now();
            api.open(options, window, cx);
            updates.push(start.elapsed().as_secs_f64() * 1e6);
            assert_eq!(api.len(cx), 100, "keyed updates preserve notice count");
        })
        .unwrap();
    }
    measurements.push(summarize("notification/keyed-update", updates));
    cx.update_window(window.into(), |_, window, cx| api.destroy_all(window, cx))
        .unwrap();
    for _ in 0..4 {
        frame(window, &mut cx, 500);
    }
    assert_eq!(
        frame(window, &mut cx, 1000),
        0,
        "destroying all notifications drains continuous work"
    );
    let mut opens = Vec::with_capacity(1000);
    let mut destroys = Vec::with_capacity(1000);
    for _ in 0..1000 {
        cx.update_window(window.into(), |_, window, cx| {
            let options = NotificationOptions::new("Transient notification")
                .key("transient")
                .kind(NotificationType::Success)
                .duration(Duration::ZERO);
            let start = Instant::now();
            api.open(options, window, cx);
            opens.push(start.elapsed().as_secs_f64() * 1e6);
            let start = Instant::now();
            api.destroy("transient", window, cx);
            destroys.push(start.elapsed().as_secs_f64() * 1e6);
        })
        .unwrap();
        cx.run_until_parked();
    }
    measurements.push(summarize("notification/open", opens));
    measurements.push(summarize("notification/destroy", destroys));
    assert!(cx.update(|cx| api.is_empty(cx)));
    assert_eq!(
        frame(window, &mut cx, 500),
        0,
        "lifecycle benchmark leaves no animation work"
    );
    let output = std::env::current_exe()
        .unwrap()
        .parent()
        .unwrap()
        .join("notification-performance.json");
    std::fs::write(output, serde_json::to_vec_pretty(&json!({"schema_version":1,"profile":"release","measurement":"CPU layout and scene preparation; diagnostic timings","measurements":measurements})).unwrap()).unwrap();
}
