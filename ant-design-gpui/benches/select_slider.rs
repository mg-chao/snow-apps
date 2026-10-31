// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
//! Release CPU/allocation distributions with GPUI's deterministic offscreen text
//! system. These are not GPU presentation or physical input-latency measurements.

use ant_design_gpui::{
    ConfigScope, Select, SelectMode, SelectOption, SelectOptions, SelectSettings, SelectState,
    Slider, SliderOptions, SliderState, UiRoot,
    select::SelectSearch,
    slider::{SliderRange, SliderScale, SliderTooltip},
};
use gpui::{
    AppContext, Context, Entity, IntoElement, MouseButton, MouseDownEvent, MouseMoveEvent,
    MouseUpEvent, PlatformInput, Render, TestAppContext, Window, WindowHandle, div, point,
    prelude::*, px,
};
use std::{
    alloc::{GlobalAlloc, Layout, System},
    cell::Cell,
    hint::black_box,
    rc::Rc,
    sync::atomic::{AtomicBool, AtomicU64, Ordering::Relaxed},
    time::Instant,
};

struct CountAlloc;
static COUNT: AtomicBool = AtomicBool::new(false);
static CALLS: AtomicU64 = AtomicU64::new(0);
static BYTES: AtomicU64 = AtomicU64::new(0);
#[global_allocator]
static ALLOCATOR: CountAlloc = CountAlloc;
fn allocated(size: usize) {
    if COUNT.load(Relaxed) {
        CALLS.fetch_add(1, Relaxed);
        BYTES.fetch_add(size as u64, Relaxed);
    }
}
unsafe impl GlobalAlloc for CountAlloc {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let p = unsafe { System.alloc(layout) };
        if !p.is_null() {
            allocated(layout.size());
        }
        p
    }
    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        let p = unsafe { System.alloc_zeroed(layout) };
        if !p.is_null() {
            allocated(layout.size());
        }
        p
    }
    unsafe fn dealloc(&self, pointer: *mut u8, layout: Layout) {
        unsafe {
            System.dealloc(pointer, layout);
        }
    }
    unsafe fn realloc(&self, pointer: *mut u8, layout: Layout, size: usize) -> *mut u8 {
        let p = unsafe { System.realloc(pointer, layout, size) };
        if !p.is_null() {
            allocated(size);
        }
        p
    }
}
fn measure(name: &str, iterations: usize, mut work: impl FnMut()) {
    for _ in 0..5 {
        work();
    }
    let mut samples = Vec::with_capacity(iterations);
    let (mut allocations, mut bytes) = (0, 0);
    for _ in 0..iterations {
        CALLS.store(0, Relaxed);
        BYTES.store(0, Relaxed);
        COUNT.store(true, Relaxed);
        let start = Instant::now();
        work();
        let elapsed = start.elapsed().as_secs_f64() * 1_000_000.;
        COUNT.store(false, Relaxed);
        allocations += CALLS.load(Relaxed);
        bytes += BYTES.load(Relaxed);
        samples.push(elapsed);
    }
    samples.sort_by(f64::total_cmp);
    let percentile = |p: f64| samples[((iterations - 1) as f64 * p).round() as usize];
    println!(
        "{}",
        serde_json::json!({"name": name, "iterations": iterations, "us": {"p50": percentile(0.5), "p95": percentile(0.95), "p99": percentile(0.99)}, "allocations": allocations as f64 / iterations as f64, "allocated_bytes": bytes as f64 / iterations as f64})
    );
}
struct Probe {
    scope: ConfigScope,
    select: Entity<SelectState<String>>,
    slider: Entity<SliderState>,
    slider_visible: bool,
    responsive: bool,
    tooltips: bool,
    row_builds: Rc<Cell<usize>>,
    tag_builds: Rc<Cell<usize>>,
}
impl Render for Probe {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        let rows = self.row_builds.clone();
        let tags = self.tag_builds.clone();
        div()
            .w(px(800.))
            .h(px(600.))
            .p(px(40.))
            .child(if self.slider_visible {
                Slider::new("slider", &self.slider)
                    .scope(self.scope.clone())
                    .w(px(280.))
                    .tooltip(SliderTooltip {
                        open: Some(self.tooltips),
                        ..Default::default()
                    })
                    .into_any_element()
            } else {
                Select::new("select", &self.select)
                    .scope(self.scope.clone())
                    .w(px(280.))
                    .responsive_tags(self.responsive)
                    .option_render(move |option, _, _| {
                        rows.set(rows.get() + 1);
                        div().child(option.option.label.clone())
                    })
                    .tag_render(move |tag, _, _| {
                        tags.set(tags.get() + 1);
                        div().w(px(70.)).h(px(24.)).child(tag.value.label.clone())
                    })
                    .into_any_element()
            })
    }
}
struct Fixture {
    cx: TestAppContext,
    window: WindowHandle<UiRoot>,
    probe: Entity<Probe>,
}
impl Fixture {
    fn new(options: SelectOptions<String>) -> Self {
        let mut cx = TestAppContext::single();
        let mut probe = None;
        let window = cx.add_window(|window, cx| {
            let scope = ConfigScope::new_window(cx);
            scope.update_patch(cx, |p| p.motion = Some(false)).unwrap();
            let select = cx.new(|cx| {
                let mut state = SelectState::new(options, window, cx);
                state.set_search(
                    SelectSearch {
                        enabled: Some(true),
                        ..SelectSearch::label()
                    },
                    cx,
                );
                state
            });
            let slider = cx.new(|cx| SliderState::new(window, cx));
            let entity = cx.new(|_| Probe {
                scope: scope.clone(),
                select,
                slider,
                slider_visible: false,
                responsive: false,
                tooltips: false,
                row_builds: Rc::new(Cell::new(0)),
                tag_builds: Rc::new(Cell::new(0)),
            });
            probe = Some(entity.clone());
            UiRoot::new(scope, entity, window, cx)
        });
        let mut fixture = Self {
            cx,
            window,
            probe: probe.unwrap(),
        };
        fixture.draw(false);
        fixture
    }
    fn draw(&mut self, refresh: bool) {
        self.cx.run_until_parked();
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                if refresh {
                    window.refresh();
                }
                window.draw(cx).clear(cx);
                assert_eq!(
                    window.simulate_next_frame(cx),
                    0,
                    "settled controls must not schedule frames"
                );
            })
            .unwrap();
        self.cx.run_until_parked();
    }
    fn pointer(&mut self, event: PlatformInput) {
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                window.dispatch_event(event, cx);
            })
            .unwrap();
        self.draw(false);
    }
}
fn main() {
    if cfg!(debug_assertions) {
        panic!("Use scripts/run-select-slider-perf.ps1 (Release)");
    }
    for count in [1_000, 10_000, 100_000] {
        let options = SelectOptions::new(
            (0..count).map(|i| SelectOption::new(i as u64, i.to_string(), format!("Option {i}"))),
        )
        .unwrap();
        let policy = SelectSearch::label();
        measure(&format!("select/{count}/filter"), 100, || {
            black_box(options.search(black_box("19"), &policy));
        });
        let mut f = Fixture::new(options.clone());
        let (state, rows, tags) = f.probe.read_with(&f.cx, |p, _| {
            (p.select.clone(), p.row_builds.clone(), p.tag_builds.clone())
        });
        state.update(&mut f.cx, |s, cx| {
            s.set_values([(count - 1).to_string()], cx);
            s.set_open(true, cx);
        });
        f.draw(false);
        measure(&format!("select/{count}/warm-open-redraw"), 100, || {
            rows.set(0);
            f.draw(true);
            assert!(rows.get() <= 24, "visible row bound: {}", rows.get());
        });
        measure(&format!("select/{count}/reopen"), 100, || {
            for open in [false, true] {
                state.update(&mut f.cx, |s, cx| s.set_open(open, cx));
                f.draw(false);
            }
        });
        let mut top = 0;
        measure(&format!("select/{count}/scroll"), 100, || {
            top = if top == 0 { count - 10 } else { 0 };
            rows.set(0);
            state.update(&mut f.cx, |s, cx| {
                assert!(s.scroll_to(&top.to_string(), cx))
            });
            f.draw(false);
            assert!(rows.get() <= 24);
        });
        // Enable the bounded layout before publishing a large selection. TestAppContext
        // can draw between separate entity updates; a transient wrapping render would
        // allocate all tags and leave a large Taffy arena in the harness.
        f.probe.update(&mut f.cx, |p, cx| {
            p.responsive = true;
            cx.notify();
        });
        state.update(&mut f.cx, |s, cx| {
            s.set_open(false, cx);
            s.set_settings(
                SelectSettings {
                    mode: SelectMode::Multiple,
                    ..Default::default()
                },
                cx,
            );
            s.set_values((0..count).map(|i| i.to_string()), cx);
        });
        f.draw(false);
        measure(&format!("select/{count}/responsive-selected"), 100, || {
            tags.set(0);
            f.draw(true);
            assert!(tags.get() <= 5);
        });
        assert!(state.read_with(&f.cx, |s, _| s.options().same_collection(&options)));
    }
    let scale = SliderScale::new(&SliderOptions {
        step: Some(0.000001),
        marks: (0..10_000).map(|i| i as f64 / 100.).collect(),
        ..Default::default()
    })
    .unwrap();
    let mut value = 0.;
    measure("slider/snap-100m-steps-10k-marks", 1000, || {
        value = (value + 0.79173) % 100.;
        black_box(scale.snap(black_box(value)));
    });
    let mut f = Fixture::new(SelectOptions::default());
    let slider = f.probe.read_with(&f.cx, |p, _| p.slider.clone());
    f.probe.update(&mut f.cx, |p, cx| {
        p.slider_visible = true;
        cx.notify();
    });
    for handles in [1, 2, 10, 100] {
        f.cx.update_window(f.window.into(), |_, window, cx| {
            slider.update(cx, |s, cx| {
                s.set_options(
                    SliderOptions {
                        range: (handles > 1).then(SliderRange::default),
                        ..Default::default()
                    },
                    window,
                    cx,
                )
                .unwrap();
                let value = if handles == 1 {
                    30.0.into()
                } else {
                    ant_design_gpui::SliderValue::Range(
                        (0..handles)
                            .map(|i| 100. * i as f64 / (handles - 1) as f64)
                            .collect(),
                    )
                };
                s.set_value(value, window, cx).unwrap();
            })
        })
        .unwrap();
        f.draw(false);
        measure(&format!("slider/{handles}-handles/redraw"), 100, || {
            f.draw(true)
        });
    }
    f.cx.update_window(f.window.into(), |_, window, cx| {
        slider.update(cx, |s, cx| {
            s.set_options(SliderOptions::default(), window, cx).unwrap();
            s.set_value(30., window, cx).unwrap();
        })
    })
    .unwrap();
    f.draw(false);
    for tooltip in [false, true] {
        f.probe.update(&mut f.cx, |p, cx| {
            p.tooltips = tooltip;
            cx.notify();
        });
        f.draw(false);
        f.pointer(PlatformInput::MouseDown(MouseDownEvent {
            position: point(px(130.), px(52.)),
            button: MouseButton::Left,
            click_count: 1,
            ..Default::default()
        }));
        let mut x = 100.;
        measure(&format!("slider/drag/tooltip-{tooltip}"), 100, || {
            x = if x == 100. { 240. } else { 100. };
            f.pointer(PlatformInput::MouseMove(MouseMoveEvent {
                position: point(px(x), px(52.)),
                pressed_button: Some(MouseButton::Left),
                ..Default::default()
            }));
        });
        f.pointer(PlatformInput::MouseUp(MouseUpEvent {
            position: point(px(x), px(52.)),
            button: MouseButton::Left,
            ..Default::default()
        }));
    }
    f.probe.update(&mut f.cx, |p, cx| {
        p.tooltips = false;
        cx.notify();
    });
    f.cx.update_window(f.window.into(), |_, window, cx| {
        slider.update(cx, |s, cx| {
            s.set_options(
                SliderOptions {
                    dots: true,
                    step: Some(0.000001),
                    ..Default::default()
                },
                window,
                cx,
            )
            .unwrap();
        })
    })
    .unwrap();
    f.draw(false);
    measure("slider/dense-dots/redraw", 100, || f.draw(true));
    f.cx.update_window(f.window.into(), |_, window, cx| {
        slider.update(cx, |state, cx| {
            state
                .set_options(
                    SliderOptions {
                        marks: (0..=100_000).map(|i| f64::from(i) / 1000.).collect(),
                        step: None,
                        included: false,
                        ..Default::default()
                    },
                    window,
                    cx,
                )
                .unwrap();
        });
    })
    .unwrap();
    f.draw(false);
    measure("slider/100000-marks/redraw", 100, || f.draw(true));
}
