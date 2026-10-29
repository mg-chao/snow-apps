// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Focused Release CPU measurements. Run scripts/run-carousel-perf.ps1.

use std::{
    cell::Cell,
    hint::black_box,
    rc::Rc,
    time::{Duration, Instant},
};

use ant_design_gpui::{
    ConfigScope, UiRoot,
    carousel::{
        Carousel, CarouselEffect, CarouselItems, CarouselSlide, CarouselState, CarouselTheme,
    },
    theme::ThemeConfig,
};
use gpui::{
    AppContext, Context, Entity, IntoElement, Render, TestAppContext, Window, WindowHandle, div,
    prelude::*, px, rgb, size,
};
use serde_json::{Value, json};

fn sample(name: &str, iterations: usize, mut operation: impl FnMut()) -> Value {
    let mut values = Vec::with_capacity(iterations);
    for _ in 0..iterations {
        let start = Instant::now();
        operation();
        values.push(start.elapsed().as_secs_f64() * 1e6);
    }
    values.sort_by(f64::total_cmp);
    let median = values[iterations / 2];
    let p95 = values[iterations * 95 / 100];
    println!("{name}: median={median:.2} us p95={p95:.2} us n={iterations}");
    json!({"name":name,"iterations":iterations,"median_microseconds":median,"p95_microseconds":p95})
}

fn items(count: usize, rendered: Rc<Cell<usize>>) -> CarouselItems {
    CarouselItems::new((0..count).map(|index| {
        let rendered = rendered.clone();
        CarouselSlide::new(format!("slide-{index}")).content(move |_, _| {
            rendered.set(rendered.get() + 1);
            div()
                .w_full()
                .h(px(160.))
                .flex()
                .items_center()
                .justify_center()
                .bg(rgb(0x364d79))
                .text_color(rgb(0xffffff))
                .child(format!("Slide {index}"))
                .into_any_element()
        })
    }))
    .expect("unique benchmark slide keys")
}

struct Bench {
    scope: ConfigScope,
    states: Vec<Entity<CarouselState>>,
    effect: CarouselEffect,
    dots: bool,
    autoplay: bool,
    progress: bool,
}

impl Render for Bench {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        let columns = self.states.len().clamp(1, 5);
        div()
            .w(px(columns as f32 * 320.))
            .grid()
            .grid_cols(columns as u16)
            .gap(px(8.))
            .children(self.states.iter().enumerate().map(|(index, state)| {
                Carousel::new(("carousel-bench", index), state)
                    .scope(self.scope.clone())
                    .effect(self.effect)
                    .dots(self.dots)
                    .autoplay(self.autoplay)
                    .autoplay_speed(Duration::from_secs(600))
                    .dot_duration(self.progress)
                    .w(px(312.))
                    .h(px(160.))
            }))
    }
}

struct Fixture {
    cx: TestAppContext,
    window: WindowHandle<UiRoot>,
    view: Entity<Bench>,
    states: Vec<Entity<CarouselState>>,
    rendered: Rc<Cell<usize>>,
    elapsed: Duration,
    frozen: bool,
}

impl Fixture {
    fn new(controllers: usize, slides: usize) -> Self {
        Self::with_clock(controllers, slides, true)
    }

    fn with_clock(controllers: usize, slides: usize, frozen: bool) -> Self {
        let mut cx = TestAppContext::single();
        let rendered = Rc::new(Cell::new(0));
        let mut states = Vec::new();
        let mut view = None;
        let window = cx.open_window(size(px(1700.), px(4000.)), |window, cx| {
            ant_design_gpui::init(cx);
            let scope = ConfigScope::new_window(cx);
            states = (0..controllers)
                .map(|_| {
                    cx.new(|cx| CarouselState::new(items(slides, rendered.clone()), window, cx))
                })
                .collect();
            if frozen {
                for state in &states {
                    state.update(cx, |state, cx| {
                        state.set_test_time(Some(Duration::ZERO), cx)
                    });
                }
            }
            let content = cx.new(|_| Bench {
                scope: scope.clone(),
                states: states.clone(),
                effect: CarouselEffect::Scroll,
                dots: false,
                autoplay: false,
                progress: false,
            });
            view = Some(content.clone());
            UiRoot::new(scope, content, window, cx)
        });
        let mut fixture = Self {
            cx,
            window,
            view: view.unwrap(),
            states,
            rendered,
            elapsed: Duration::ZERO,
            frozen,
        };
        fixture
            .cx
            .update_window(window.into(), |_, window, cx| {
                window.dispatch_event(
                    gpui::PlatformInput::MouseMove(gpui::MouseMoveEvent {
                        position: gpui::point(px(1690.), px(3990.)),
                        ..Default::default()
                    }),
                    cx,
                );
            })
            .unwrap();
        fixture.frame(1000);
        fixture
    }

    fn draw(&mut self) {
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                window.refresh();
                window.draw(cx).clear(cx);
            })
            .unwrap();
        self.cx.run_until_parked();
    }

    fn notify_draw(&mut self) {
        self.view.update(&mut self.cx, |_, cx| cx.notify());
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                window.draw(cx).clear(cx);
            })
            .unwrap();
        self.cx.run_until_parked();
    }

    fn frame(&mut self, millis: u64) -> usize {
        self.elapsed += Duration::from_millis(millis);
        self.cx
            .executor()
            .advance_clock(Duration::from_millis(millis));
        let elapsed = self.elapsed;
        let states = &self.states;
        let frozen = self.frozen;
        let callbacks = self
            .cx
            .update_window(self.window.into(), |_, window, cx| {
                if frozen {
                    for state in states {
                        state.update(cx, |state, cx| state.set_test_time(Some(elapsed), cx));
                    }
                }
                let callbacks = window.simulate_next_frame(cx);
                window.refresh();
                window.draw(cx).clear(cx);
                callbacks
            })
            .unwrap();
        self.cx.run_until_parked();
        callbacks
    }

    fn next(&mut self) {
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                for state in &self.states {
                    state.update(cx, |state, cx| state.next(window, cx));
                }
            })
            .unwrap();
        self.draw();
    }
}

fn main() {
    assert!(
        !black_box(cfg!(debug_assertions)),
        "Carousel measurements require Release"
    );
    let theme = ThemeConfig::default().resolve();
    let mut measurements = vec![sample("carousel/theme/cached-tokens", 1000, || {
        for _ in 0..200 {
            black_box(theme.component::<CarouselTheme>());
        }
    })];

    for slides in [1, 4, 20, 100, 1000] {
        let mut fixture = Fixture::new(1, slides);
        fixture.frame(1000);
        fixture.notify_draw();
        let before = fixture.rendered.get();
        measurements.push(sample(
            &format!("carousel/{slides}-slides/owner-notify-cached-redraw"),
            100,
            || fixture.notify_draw(),
        ));
        let cached_content_renders = fixture.rendered.get() - before;
        assert_eq!(
            cached_content_renders, 0,
            "unchanged retained slide trees are reused"
        );
        measurements.push(json!({
            "name":format!("carousel/{slides}-slides/cached-content-renders"),
            "redraws":100,"content_renders":cached_content_renders,
        }));
        let before = fixture.rendered.get();
        measurements.push(sample(
            &format!("carousel/{slides}-slides/forced-refresh-redraw"),
            100,
            || fixture.draw(),
        ));
        let content_renders = fixture.rendered.get() - before;
        assert!(
            content_renders <= 200,
            "warm fixed-height rendering must be independent of item count"
        );
        measurements.push(json!({
            "name":format!("carousel/{slides}-slides/content-renders"),
            "redraws":100,"content_renders":content_renders,
        }));
        fixture.view.update(&mut fixture.cx, |view, cx| {
            view.dots = true;
            cx.notify();
        });
        fixture.draw();
        fixture.frame(1000);
        measurements.push(sample(
            &format!("carousel/{slides}-slides/dots-enabled-forced-refresh-redraw"),
            100,
            || fixture.draw(),
        ));
    }

    for controllers in [1, 20, 100] {
        let mut fixture = Fixture::new(controllers, 4);
        measurements.push(sample(
            &format!("carousel/{controllers}-controllers/forced-refresh-redraw"),
            100,
            || {
                fixture.frame(16);
            },
        ));
        for effect in [CarouselEffect::Scroll, CarouselEffect::Fade] {
            fixture.view.update(&mut fixture.cx, |view, cx| {
                view.effect = effect;
                cx.notify();
            });
            fixture.draw();
            measurements.push(sample(
                &format!("carousel/{controllers}-controllers/{effect:?}/navigation"),
                25,
                || {
                    fixture.next();
                    for _ in 0..4 {
                        fixture.frame(16);
                    }
                },
            ));
            for _ in 0..4 {
                fixture.frame(1000);
            }
        }
        fixture.view.update(&mut fixture.cx, |view, cx| {
            view.effect = CarouselEffect::Scroll;
            view.dots = true;
            view.autoplay = true;
            view.progress = true;
            cx.notify();
        });
        fixture.draw();
        for state in &fixture.states {
            assert!(state.read_with(&fixture.cx, |state, _| state.autoplay_running()));
        }
        measurements.push(sample(
            &format!("carousel/{controllers}-controllers/dot-progress/deterministic-redraw"),
            100,
            || {
                fixture.frame(16);
            },
        ));
        fixture.view.update(&mut fixture.cx, |view, cx| {
            view.scope
                .update_patch(cx, |patch| patch.motion = Some(false))
                .unwrap();
            cx.notify();
        });
        fixture.draw();
        for state in &fixture.states {
            assert!(state.read_with(&fixture.cx, |state, _| state.autoplay_running()));
        }
        measurements.push(sample(
            &format!("carousel/{controllers}-controllers/reduced-motion/forced-refresh-redraw"),
            100,
            || {
                fixture.frame(16);
            },
        ));
    }

    // Scheduling assertions use the ordinary live clock. The deterministic samples
    // above intentionally freeze clocks and therefore cannot prove scheduler idleness.
    let mut live = Fixture::with_clock(1, 4, false);
    assert_eq!(live.frame(16), 0, "ordinary settled Carousel is idle");
    live.view.update(&mut live.cx, |view, cx| {
        view.dots = true;
        view.autoplay = true;
        view.progress = true;
        cx.notify();
    });
    live.draw();
    for _ in 0..8 {
        live.draw();
    }
    assert_eq!(
        live.frame(16),
        1,
        "live redraws share one progress callback"
    );
    live.view.update(&mut live.cx, |view, cx| {
        view.scope
            .update_patch(cx, |patch| patch.motion = Some(false))
            .unwrap();
        cx.notify();
    });
    live.draw();
    // A callback queued before the policy change may drain once.
    live.frame(16);
    assert_eq!(
        live.frame(16),
        0,
        "reduced motion has no continuous progress frames"
    );
    assert!(live.states[0].read_with(&live.cx, |state, _| state.autoplay_running()));
    live.view.update(&mut live.cx, |view, cx| {
        view.scope
            .update_patch(cx, |patch| patch.motion = Some(true))
            .unwrap();
        cx.notify();
    });
    live.draw();
    live.frame(16);
    live.view.update(&mut live.cx, |view, cx| {
        view.states.clear();
        cx.notify();
    });
    live.draw();
    live.frame(16);
    assert_eq!(
        live.frame(1000),
        0,
        "unmounted Carousel drains queued callbacks"
    );
    assert!(!live.states[0].read_with(&live.cx, |state, _| state.autoplay_running()));
    let old_index = live.states[0].read_with(&live.cx, |state, _| state.current_index());
    live.frame(601_000);
    assert_eq!(
        live.states[0].read_with(&live.cx, |state, _| state.current_index()),
        old_index,
        "unmount cancels autoplay timer even if controller remains retained"
    );
    let output = std::env::current_exe()
        .unwrap()
        .parent()
        .unwrap()
        .join("carousel-performance.json");
    std::fs::write(
        output,
        serde_json::to_vec_pretty(&json!({
            "schema_version":1,
            "profile":"release",
            "measurement":"CPU layout and scene preparation; diagnostic timings",
            "assertions":["fixed-height slide rendering stays bounded for 1/4/20/100/1000 slides",
                "dot rendering costs measured separately from the viewport",
                "cached owner redraws do not rebuild retained slide content",
                "ordinary settled Carousel requests zero frames with a live clock",
                "eight live forced redraws share one progress callback",
                "reduced motion preserves autoplay without continuous progress frames",
                "unmount drains frames and cancels autoplay while retaining the controller"],
            "measurements":measurements,
        }))
        .unwrap(),
    )
    .unwrap();
}
