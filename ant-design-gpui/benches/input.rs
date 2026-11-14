// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Release CPU and allocation diagnostics, using GPUI's deterministic offscreen
//! text system. These measure editing, layout/cache and scene construction, not
//! native font shaping, GPU presentation or physical keyboard latency.

use std::{
    alloc::{GlobalAlloc, Layout, System},
    hint::black_box,
    sync::atomic::{AtomicBool, AtomicU64, Ordering::Relaxed},
    time::{Duration, Instant},
};

use ant_design_gpui::{
    ConfigPatch, ConfigScope, Input, InputNumber, InputNumberState, InputState, OtpInput, OtpState,
    PasswordInput, SearchInput, TextArea,
    input::{
        EditOrigin, TextSelection,
        editing::{EditPolicy, EditingBuffer},
    },
};
use gpui::{
    AppContext, Context, Entity, IntoElement, Render, TestAppContext, Window, WindowHandle, div,
    prelude::*, px,
};

struct CountAlloc;
static COUNT: AtomicBool = AtomicBool::new(false);
static CALLS: AtomicU64 = AtomicU64::new(0);
static BYTES: AtomicU64 = AtomicU64::new(0);
#[global_allocator]
static ALLOCATOR: CountAlloc = CountAlloc;

fn allocated(bytes: usize) {
    if COUNT.load(Relaxed) {
        CALLS.fetch_add(1, Relaxed);
        BYTES.fetch_add(bytes as u64, Relaxed);
    }
}
unsafe impl GlobalAlloc for CountAlloc {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let pointer = unsafe { System.alloc(layout) };
        if !pointer.is_null() {
            allocated(layout.size());
        }
        pointer
    }
    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        let pointer = unsafe { System.alloc_zeroed(layout) };
        if !pointer.is_null() {
            allocated(layout.size());
        }
        pointer
    }
    unsafe fn dealloc(&self, pointer: *mut u8, layout: Layout) {
        unsafe { System.dealloc(pointer, layout) };
    }
    unsafe fn realloc(&self, pointer: *mut u8, layout: Layout, bytes: usize) -> *mut u8 {
        let pointer = unsafe { System.realloc(pointer, layout, bytes) };
        if !pointer.is_null() {
            allocated(bytes);
        }
        pointer
    }
}

fn measure(name: &str, iterations: usize, mut work: impl FnMut()) {
    for _ in 0..5 {
        work();
    }
    let mut samples = Vec::with_capacity(iterations);
    let mut allocations = 0;
    let mut bytes = 0;
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
        "{name}: n={iterations} us p50={:.2} p95={:.2} p99={:.2}; allocations={:.1} allocated_bytes={:.1}",
        percentile(0.50),
        percentile(0.95),
        percentile(0.99),
        allocations as f64 / iterations as f64,
        bytes as f64 / iterations as f64
    );
}

#[derive(Clone, Copy)]
enum Mode {
    Form,
    Notes,
}
struct Probe {
    scope: ConfigScope,
    mode: Mode,
    texts: Vec<Entity<InputState>>,
    numbers: Vec<Entity<InputNumberState>>,
    codes: Vec<Entity<OtpState>>,
}
impl Render for Probe {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        if matches!(self.mode, Mode::Notes) {
            return div()
                .w(px(640.))
                .child(
                    TextArea::new("notes", &self.texts[0])
                        .scope(self.scope.clone())
                        .rows(12),
                )
                .into_any_element();
        }
        div()
            .w(px(1200.))
            .flex()
            .flex_wrap()
            .gap(px(8.))
            .children(self.texts.iter().enumerate().map(|(index, state)| {
                let input = Input::new(index, state)
                    .scope(self.scope.clone())
                    .w(px(220.));
                match index % 4 {
                    1 => PasswordInput::new(index, state)
                        .scope(self.scope.clone())
                        .configure(|_| input)
                        .into_any_element(),
                    2 => SearchInput::new(index, state)
                        .scope(self.scope.clone())
                        .configure(|_| input)
                        .into_any_element(),
                    _ => input.allow_clear(true).show_count(true).into_any_element(),
                }
            }))
            .children(self.numbers.iter().enumerate().map(|(index, state)| {
                InputNumber::new(("number", index), state, cx).scope(self.scope.clone())
            }))
            .children(self.codes.iter().enumerate().map(|(index, state)| {
                OtpInput::new(("otp", index), state).scope(self.scope.clone())
            }))
            .into_any_element()
    }
}

fn draw(cx: &mut TestAppContext, window: WindowHandle<Probe>, rerender: bool) {
    cx.run_until_parked();
    cx.update_window(window.into(), |view, window, cx| {
        if rerender {
            view.downcast::<Probe>()
                .unwrap()
                .update(cx, |_, cx| cx.notify());
        }
        window.draw(cx).clear(cx);
        assert_eq!(
            window.simulate_next_frame(cx),
            0,
            "settled inputs must not schedule frames"
        );
    })
    .unwrap();
    cx.run_until_parked();
}

fn main() {
    if cfg!(debug_assertions) {
        eprintln!("Input benchmarks require Release");
        std::process::exit(2);
    }
    let policy = EditPolicy {
        multiline: true,
        ..Default::default()
    };
    let note = "A note with Unicode 中文 and emoji 👩🏽‍💻; editing stays responsive.\n".repeat(1250);
    println!(
        "input benchmark: note_bytes={}, process-wide allocation counts include reallocations, mock text shaping",
        note.len()
    );
    for (label, text) in [
        ("form", "Name 中文 👩🏽‍💻".to_owned()),
        ("notes100kb", note.clone()),
    ] {
        let mut buffer = EditingBuffer::new(text);
        measure(&format!("input/{label}/middle-edit-undo"), 300, || {
            let middle = buffer.snap(buffer.text().len() / 2, false);
            buffer.replace(
                Some(middle..middle),
                "字",
                EditOrigin::Typing,
                Duration::ZERO,
                &policy,
            );
            assert!(buffer.undo(Duration::ZERO, &policy));
            black_box(buffer.text());
        });
        measure(&format!("input/{label}/navigation-utf16"), 500, || {
            buffer.move_horizontal(false, false, false);
            black_box(buffer.utf16_range(buffer.selection().range()));
            buffer.move_horizontal(true, false, false);
        });
        println!(
            "input/{label}/retained: source_bytes={} grapheme_index_bytes={} history_bytes={}",
            buffer.text().len(),
            std::mem::size_of_val(buffer.grapheme_boundaries()),
            buffer.history_bytes()
        );
    }
    measure("input/large-paste-undo-redo", 100, || {
        let mut buffer = EditingBuffer::new(String::new());
        buffer.replace(None, &note, EditOrigin::Paste, Duration::ZERO, &policy);
        assert!(buffer.undo(Duration::ZERO, &policy));
        assert!(buffer.redo());
        assert_eq!(buffer.text(), note);
    });
    let mut cx = TestAppContext::single();
    let window = cx.add_window(|window, cx| {
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
        Probe {
            scope,
            mode: Mode::Form,
            texts: (0..40)
                .map(|_| {
                    cx.new(|cx| {
                        let mut state = InputState::new(window, cx);
                        state.set_value("Name 中文 👩🏽‍💻", window, cx);
                        state
                    })
                })
                .collect(),
            numbers: (0..8)
                .map(|_| cx.new(|cx| InputNumberState::new(window, cx)))
                .collect(),
            codes: (0..4)
                .map(|_| cx.new(|cx| OtpState::new(window, cx)))
                .collect(),
        }
    });
    draw(&mut cx, window, true);
    measure("input/form52/warm-draw", 200, || {
        draw(&mut cx, window, false)
    });
    measure("input/form52/rerender", 200, || draw(&mut cx, window, true));
    let editor = window
        .update(&mut cx, |view, window, cx| {
            view.mode = Mode::Notes;
            view.texts[0].update(cx, |state, cx| {
                state.set_multiline(true, window, cx);
                state.set_value(&note, window, cx);
                state.select(TextSelection::collapsed(0), window, cx);
            });
            cx.notify();
            view.texts[0].clone()
        })
        .unwrap();
    draw(&mut cx, window, true);
    measure("input/notes100kb/warm-draw", 200, || {
        draw(&mut cx, window, false)
    });
    measure("input/notes100kb/rerender", 200, || {
        draw(&mut cx, window, true)
    });
    let mut disabled = false;
    measure("input/notes100kb/color-change-draw", 200, || {
        disabled = !disabled;
        cx.update_window(window.into(), |_, window, cx| {
            editor.update(cx, |state, cx| state.set_disabled(disabled, window, cx));
        })
        .unwrap();
        draw(&mut cx, window, false);
    });
    cx.update_window(window.into(), |_, window, cx| {
        editor.update(cx, |state, cx| state.set_disabled(false, window, cx));
    })
    .unwrap();
    draw(&mut cx, window, false);
    measure("input/notes100kb/edit-draw-undo-draw", 100, || {
        cx.update_window(window.into(), |_, window, cx| {
            editor.update(cx, |state, cx| {
                state.replace_range(Some(0..0), "字", EditOrigin::Typing, window, cx);
                assert_eq!(state.value().len(), note.len() + "字".len());
            })
        })
        .unwrap();
        draw(&mut cx, window, false);
        cx.update_window(window.into(), |_, window, cx| {
            editor.update(cx, |state, cx| state.undo(window, cx))
        })
        .unwrap();
        draw(&mut cx, window, false);
    });
    editor.read_with(&cx, |state, _| {
        let geometry = state.geometry().unwrap();
        println!(
            "input/notes100kb/geometry: carets={} clusters={} payload_bytes={}",
            geometry.carets.len(),
            geometry.clusters.len(),
            std::mem::size_of_val(geometry.carets.as_slice())
                + std::mem::size_of_val(geometry.clusters.as_slice())
        );
    });
    let unbroken = note.replace('\n', " ");
    cx.update_window(window.into(), |_, window, cx| {
        editor.update(cx, |state, cx| {
            state.set_value(&unbroken, window, cx);
            state.select(TextSelection::collapsed(0), window, cx);
        })
    })
    .unwrap();
    draw(&mut cx, window, true);
    measure("input/one-paragraph100kb/warm-draw", 100, || {
        draw(&mut cx, window, false)
    });
    measure("input/one-paragraph100kb/edit-draw-undo-draw", 50, || {
        cx.update_window(window.into(), |_, window, cx| {
            editor.update(cx, |state, cx| {
                state.replace_range(Some(0..0), "字", EditOrigin::Typing, window, cx);
                assert_eq!(state.value().len(), unbroken.len() + "字".len());
            })
        })
        .unwrap();
        draw(&mut cx, window, false);
        cx.update_window(window.into(), |_, window, cx| {
            editor.update(cx, |state, cx| state.undo(window, cx))
        })
        .unwrap();
        draw(&mut cx, window, false);
    });
}
