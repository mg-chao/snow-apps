// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
//! Focused Release CPU/allocation distributions, not physical GPU/input latency.
use ant_design_gpui::{
    ConfigScope, Pagination, PaginationState, UiRoot,
    input::EditOrigin,
    pagination::{PaginationItemKind, QuickJumper},
};
use gpui::{
    AppContext, Context, Entity, IntoElement, Render, TestAppContext, Window, WindowHandle, div,
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
        let value = unsafe { System.alloc(layout) };
        if !value.is_null() {
            allocated(layout.size());
        }
        value
    }
    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        let value = unsafe { System.alloc_zeroed(layout) };
        if !value.is_null() {
            allocated(layout.size());
        }
        value
    }
    unsafe fn dealloc(&self, pointer: *mut u8, layout: Layout) {
        unsafe {
            System.dealloc(pointer, layout);
        }
    }
    unsafe fn realloc(&self, pointer: *mut u8, layout: Layout, size: usize) -> *mut u8 {
        let value = unsafe { System.realloc(pointer, layout, size) };
        if !value.is_null() {
            allocated(size);
        }
        value
    }
}
#[derive(Default)]
struct RenderCounters {
    items: Cell<usize>,
    sequences: Cell<usize>,
    max_items_in_sequence: Cell<usize>,
    explicit_draws: Cell<usize>,
}
fn measure(
    name: &str,
    iterations: usize,
    counters: Option<&RenderCounters>,
    mut work: impl FnMut(),
) {
    for _ in 0..5 {
        work();
    }
    let mut samples = Vec::with_capacity(iterations);
    let (mut calls, mut bytes) = (0, 0);
    let (mut sequences, mut items, mut maximum_sequences, mut explicit_draws) = (0, 0, 0, 0);
    if let Some(counters) = counters {
        counters.max_items_in_sequence.set(0);
    }
    for _ in 0..iterations {
        let before = counters.map(|counters| {
            (
                counters.sequences.get(),
                counters.items.get(),
                counters.explicit_draws.get(),
            )
        });
        CALLS.store(0, Relaxed);
        BYTES.store(0, Relaxed);
        COUNT.store(true, Relaxed);
        let started = Instant::now();
        work();
        let elapsed = started.elapsed().as_secs_f64() * 1_000_000.;
        COUNT.store(false, Relaxed);
        calls += CALLS.load(Relaxed);
        bytes += BYTES.load(Relaxed);
        samples.push(elapsed);
        if let (Some(counters), Some((before_sequences, before_items, before_explicit))) =
            (counters, before)
        {
            let count = counters.sequences.get() - before_sequences;
            sequences += count;
            items += counters.items.get() - before_items;
            explicit_draws += counters.explicit_draws.get() - before_explicit;
            maximum_sequences = maximum_sequences.max(count);
        }
    }
    samples.sort_by(f64::total_cmp);
    let percentile = |p: f64| samples[((iterations - 1) as f64 * p).round() as usize];
    println!(
        "{}",
        serde_json::json!({"name":name,"iterations":iterations,"us":{"p50":percentile(0.5),"p95":percentile(0.95),"p99":percentile(0.99)},
        "allocations":calls as f64/iterations as f64,"allocated_bytes":bytes as f64/iterations as f64,
        "pagination_render_sequences_per_operation":counters.map(|_| sequences as f64/iterations as f64),
        "max_render_sequences_per_operation":counters.map(|_| maximum_sequences),
        "item_builds_per_operation":counters.map(|_| items as f64/iterations as f64),
        "explicit_window_draws_per_operation":counters.map(|_| explicit_draws as f64/iterations as f64),
        "max_items_per_render_sequence":counters.map(|counters| counters.max_items_in_sequence.get())})
    );
}
struct Probe {
    scope: ConfigScope,
    state: Entity<PaginationState>,
    total: u64,
    composite: bool,
    counters: Rc<RenderCounters>,
}
impl Render for Probe {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        let counters = self.counters.clone();
        let items_in_sequence = Cell::new(0);
        div().w(px(1200.)).h(px(400.)).p(px(40.)).child(
            Pagination::new("pagination", &self.state)
                .scope(self.scope.clone())
                .total(self.total)
                .default_current(50)
                .show_size_changer(self.composite)
                .quick_jumper(if self.composite {
                    QuickJumper::Blur
                } else {
                    QuickJumper::Hidden
                })
                .item_render(move |info, original, _, _| {
                    // The Previous item begins every bounded full-pager render.
                    // A single state update may legitimately cause more than one
                    // render; count those rather than confusing them with pages.
                    if info.kind == PaginationItemKind::Previous {
                        counters.sequences.set(counters.sequences.get() + 1);
                        items_in_sequence.set(0);
                    }
                    let count = items_in_sequence.get() + 1;
                    items_in_sequence.set(count);
                    counters.items.set(counters.items.get() + 1);
                    counters
                        .max_items_in_sequence
                        .set(counters.max_items_in_sequence.get().max(count));
                    assert!(
                        count <= 11,
                        "a Pagination render materialized {count} items"
                    );
                    Some(original)
                }),
        )
    }
}
struct Fixture {
    cx: TestAppContext,
    window: WindowHandle<UiRoot>,
    probe: Entity<Probe>,
}
impl Fixture {
    fn new(total: u64, composite: bool) -> Self {
        let mut cx = TestAppContext::single();
        let mut probe = None;
        let window = cx.add_window(|window, cx| {
            let scope = ConfigScope::new_window(cx);
            scope
                .update_patch(cx, |patch| patch.motion = Some(false))
                .unwrap();
            let state = cx.new(|cx| PaginationState::new(window, cx));
            let entity = cx.new(|_| Probe {
                scope: scope.clone(),
                state,
                total,
                composite,
                counters: Rc::new(RenderCounters::default()),
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
        let counters = self
            .probe
            .read_with(&self.cx, |probe, _| probe.counters.clone());
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                if refresh {
                    window.refresh();
                }
                counters
                    .explicit_draws
                    .set(counters.explicit_draws.get() + 1);
                window.draw(cx).clear(cx);
                assert_eq!(
                    window.simulate_next_frame(cx),
                    0,
                    "idle Pagination must schedule no animation frames"
                );
            })
            .unwrap();
        self.cx.run_until_parked();
    }
    fn settle_change(&mut self, before: usize, counters: &RenderCounters) {
        self.cx.run_until_parked();
        if counters.sequences.get() == before {
            self.draw(false);
        } else {
            // TestAppContext may already have serviced the frame requested by
            // the state change. Do not charge a second forced redraw to it.
            self.cx
                .update_window(self.window.into(), |_, window, cx| {
                    assert_eq!(
                        window.simulate_next_frame(cx),
                        0,
                        "settled Pagination must not animate"
                    );
                })
                .unwrap();
        }
    }
}
fn main() {
    if cfg!(debug_assertions) {
        eprintln!("Pagination benchmarks require Release; use scripts/run-pagination-perf.ps1");
        std::process::exit(2);
    }
    for total in [1_000, 1_000_000_000_000, u64::MAX] {
        let mut fixture = Fixture::new(total, false);
        let (state, counters) = fixture.probe.read_with(&fixture.cx, |probe, _| {
            (probe.state.clone(), probe.counters.clone())
        });
        measure(&format!("pagination/{total}/builder"), 2_000, None, || {
            black_box(Pagination::new("pagination", &state).total(black_box(total)));
        });
        measure(
            &format!("pagination/{total}/warm-draw"),
            100,
            Some(&counters),
            || fixture.draw(false),
        );
        measure(
            &format!("pagination/{total}/rerender"),
            100,
            Some(&counters),
            || {
                fixture.draw(true);
            },
        );
        let mut current = 49;
        measure(
            &format!("pagination/{total}/page-change"),
            100,
            Some(&counters),
            || {
                let before = counters.sequences.get();
                current = if current == 49 { 50 } else { 49 };
                fixture
                    .cx
                    .update_window(fixture.window.into(), |_, window, cx| {
                        state.update(cx, |state, cx| state.set_current(current, window, cx))
                    })
                    .unwrap();
                fixture.settle_change(before, &counters);
            },
        );
    }
    let mut fixture = Fixture::new(1_000_000_000_000, true);
    let state = fixture
        .probe
        .read_with(&fixture.cx, |probe, _| probe.state.clone());
    let counters = fixture
        .probe
        .read_with(&fixture.cx, |probe, _| probe.counters.clone());
    let selector = state.read_with(&fixture.cx, |state, _| state.size_selector().clone());
    selector.update(&mut fixture.cx, |selector, cx| selector.set_open(true, cx));
    fixture.draw(false);
    measure(
        "pagination/composite/open-size-changer/rerender",
        100,
        Some(&counters),
        || fixture.draw(true),
    );
    selector.update(&mut fixture.cx, |selector, cx| selector.set_open(false, cx));
    fixture.draw(false);
    let quick = state.read_with(&fixture.cx, |state, _| state.quick_input().clone());
    let mut long = false;
    measure(
        "pagination/composite/quick-edit",
        100,
        Some(&counters),
        || {
            let before = counters.sequences.get();
            long = !long;
            fixture
                .cx
                .update_window(fixture.window.into(), |_, window, cx| {
                    quick.update(cx, |quick, cx| {
                        let length = quick.value().len();
                        quick.replace_range(
                            Some(0..length),
                            if long { "123456" } else { "12" },
                            EditOrigin::Typing,
                            window,
                            cx,
                        );
                    })
                })
                .unwrap();
            fixture.settle_change(before, &counters);
        },
    );
}
