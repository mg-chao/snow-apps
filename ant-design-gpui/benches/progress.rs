// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Focused Release CPU benchmark. Run scripts/run-progress-perf.ps1.
use ant_design_gpui::{ConfigScope, Progress, UiRoot, progress::*, theme::Color};
use gpui::{
    AppContext, Context, Render, TestAppContext, Window, WindowHandle, div, prelude::*, px, size,
};
use serde_json::{Value, json};
use std::alloc::{GlobalAlloc, Layout, System};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::{hint::black_box, time::Instant};
struct CountingAllocator;
static RECORD: AtomicBool = AtomicBool::new(false);
static ALLOCATIONS: AtomicUsize = AtomicUsize::new(0);
static BYTES: AtomicUsize = AtomicUsize::new(0);
fn allocated(bytes: usize) {
    if RECORD.load(Ordering::Relaxed) {
        ALLOCATIONS.fetch_add(1, Ordering::Relaxed);
        BYTES.fetch_add(bytes, Ordering::Relaxed);
    }
}
// The benchmark counts process allocations only during the measured operation.
unsafe impl GlobalAlloc for CountingAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        allocated(layout.size());
        unsafe { System.alloc(layout) }
    }
    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        unsafe { System.dealloc(ptr, layout) }
    }
    unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, size: usize) -> *mut u8 {
        allocated(size);
        unsafe { System.realloc(ptr, layout, size) }
    }
}
#[global_allocator]
static ALLOCATOR: CountingAllocator = CountingAllocator;

#[derive(Clone, Copy, Debug)]
enum Mode {
    Line,
    Circle,
    Dashboard,
    GradientLine,
    GradientCircle,
    LineSteps,
    CircleSteps,
    InnerContent,
}
struct Bench {
    scope: ConfigScope,
    count: usize,
    mode: Mode,
    percent: f64,
    generation: usize,
}
impl Render for Bench {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        let gradient = ProgressGradient::new([
            (0., Color::rgb(0x108ee9)),
            (0.5, Color::rgb(0xffe58f)),
            (1., Color::rgb(0x87d068)),
        ]);
        div()
            .w(px(1800.))
            .grid()
            .grid_cols(10)
            .children((0..self.count).map(|i| {
                let p = Progress::new(("bench", self.generation * 100 + i))
                    .scope(self.scope.clone())
                    .percent(self.percent);
                let p = match self.mode {
                    Mode::Line => p,
                    Mode::Circle => p.kind(ProgressKind::Circle),
                    Mode::Dashboard => p.kind(ProgressKind::Dashboard),
                    Mode::GradientLine => p.stroke_color(gradient.clone()),
                    Mode::GradientCircle => {
                        p.kind(ProgressKind::Circle).stroke_color(gradient.clone())
                    }
                    Mode::LineSteps => p.steps(1000).show_info(false),
                    Mode::CircleSteps => p.kind(ProgressKind::Circle).steps(ProgressSteps {
                        count: 100,
                        gap: 0.1,
                    }),
                    Mode::InnerContent => p
                        .size([160., 24.])
                        .percent_position(ProgressPercentPosition {
                            align: ProgressAlign::Center,
                            position: ProgressInfoPosition::Inner,
                        })
                        .format_content(|_, _, _, _| {
                            div()
                                .flex()
                                .gap(px(4.))
                                .child(div().size(px(8.)).bg(gpui::white()))
                                .child("Files")
                        }),
                };
                div().w(px(170.)).h(px(140.)).overflow_hidden().child(p)
            }))
    }
}
fn draw(window: WindowHandle<UiRoot>, cx: &mut TestAppContext) {
    cx.run_until_parked();
    cx.update_window(window.into(), |_, w, cx| {
        w.refresh();
        w.draw(cx).clear(cx)
    })
    .unwrap();
    cx.run_until_parked();
}
fn measure(name: &str, n: usize, mut f: impl FnMut()) -> Value {
    let mut samples = Vec::with_capacity(n);
    let mut allocations = Vec::with_capacity(n);
    let mut bytes = Vec::with_capacity(n);
    for _ in 0..n {
        ALLOCATIONS.store(0, Ordering::Relaxed);
        BYTES.store(0, Ordering::Relaxed);
        RECORD.store(true, Ordering::Relaxed);
        let start = Instant::now();
        f();
        RECORD.store(false, Ordering::Relaxed);
        samples.push(start.elapsed().as_secs_f64() * 1e6);
        allocations.push(ALLOCATIONS.load(Ordering::Relaxed));
        bytes.push(BYTES.load(Ordering::Relaxed));
    }
    samples.sort_by(f64::total_cmp);
    allocations.sort_unstable();
    bytes.sort_unstable();
    let median = samples[n / 2];
    let p95 = samples[n * 95 / 100];
    println!("{name}: median={median:.2} us p95={p95:.2} us n={n}");
    json!({"name":name,"samples":n,"median_microseconds":median,"p95_microseconds":p95,
            "median_allocations":allocations[n/2],"median_allocated_bytes":bytes[n/2]})
}
fn main() {
    assert!(
        !black_box(cfg!(debug_assertions)),
        "Progress measurements require Release"
    );
    let mut results = Vec::new();
    for count in [1, 20, 100] {
        let mut cx = TestAppContext::single();
        let mut content = None;
        let window = cx.open_window(size(px(1800.), px(1500.)), |window, cx| {
            ant_design_gpui::init(cx);
            let scope = ConfigScope::new_window(cx);
            scope.update_patch(cx, |p| p.motion = Some(false)).unwrap();
            let view = cx.new(|_| Bench {
                scope: scope.clone(),
                count,
                mode: Mode::Line,
                percent: 65.,
                generation: 0,
            });
            content = Some(view.clone());
            UiRoot::new(scope, view, window, cx)
        });
        let content = content.unwrap();
        for mode in [
            Mode::Line,
            Mode::Circle,
            Mode::Dashboard,
            Mode::GradientLine,
            Mode::GradientCircle,
            Mode::LineSteps,
            Mode::CircleSteps,
            Mode::InnerContent,
        ] {
            content.update(&mut cx, |v, cx| {
                v.mode = mode;
                cx.notify();
            });
            draw(window, &mut cx);
            results.push(measure(
                &format!("progress/{count}/{mode:?}/cold"),
                30,
                || {
                    content.update(&mut cx, |v, cx| {
                        v.generation += 1;
                        cx.notify();
                    });
                    draw(window, &mut cx);
                },
            ));
            results.push(measure(
                &format!("progress/{count}/{mode:?}/warm"),
                100,
                || draw(window, &mut cx),
            ));
            results.push(measure(
                &format!("progress/{count}/{mode:?}/update"),
                100,
                || {
                    content.update(&mut cx, |v, cx| {
                        v.percent = if v.percent == 65. { 45. } else { 65. };
                        cx.notify();
                    });
                    draw(window, &mut cx);
                },
            ));
            let callbacks = cx
                .update_window(window.into(), |_, w, cx| w.simulate_next_frame(cx))
                .unwrap();
            assert_eq!(callbacks, 0, "settled progress must not schedule frames");
        }
    }
    if let Ok(path) = std::env::var("PROGRESS_PERF_OUTPUT") {
        std::fs::write(path,serde_json::to_vec_pretty(&json!({"profile":"release","kind":"offscreen-cpu","measurements":results,"idle_frame_assertions":24,"builder_inline_bytes":std::mem::size_of::<Progress>()})).unwrap()).unwrap();
    }
}
