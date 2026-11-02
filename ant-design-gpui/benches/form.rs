// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Focused Release CPU/allocation measurements; no GPU or input-latency claims.

use ant_design_gpui::{
    ConfigScope, Input, InputState, UiRoot,
    form::{
        FieldOptions, Form, FormCodec, FormField, FormItem, FormList, FormPresence, FormRule,
        FormRuleResult, FormState, FormValue, NamePath, ValidateOptions, ValidationTrigger,
    },
};
use gpui::{
    App, AppContext, Context, Entity, IntoElement, Render, SharedString, Subscription,
    TestAppContext, Window, WindowHandle, prelude::*, px,
};
use std::{
    alloc::{GlobalAlloc, Layout, System},
    cell::Cell,
    hint::black_box,
    rc::Rc,
    sync::atomic::{AtomicBool, AtomicU64, Ordering::Relaxed},
    time::{Duration, Instant},
};

struct CountAlloc;
static COUNT: AtomicBool = AtomicBool::new(false);
static CALLS: AtomicU64 = AtomicU64::new(0);
static BYTES: AtomicU64 = AtomicU64::new(0);
#[global_allocator]
static ALLOCATOR: CountAlloc = CountAlloc;
fn record(bytes: usize) {
    if COUNT.load(Relaxed) {
        CALLS.fetch_add(1, Relaxed);
        BYTES.fetch_add(bytes as u64, Relaxed);
    }
}
unsafe impl GlobalAlloc for CountAlloc {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let ptr = unsafe { System.alloc(layout) };
        if !ptr.is_null() {
            record(layout.size());
        }
        ptr
    }
    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        let ptr = unsafe { System.alloc_zeroed(layout) };
        if !ptr.is_null() {
            record(layout.size());
        }
        ptr
    }
    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        unsafe {
            System.dealloc(ptr, layout);
        }
    }
    unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, size: usize) -> *mut u8 {
        let ptr = unsafe { System.realloc(ptr, layout, size) };
        if !ptr.is_null() {
            record(size);
        }
        ptr
    }
}

fn measure(name: &str, iterations: usize, mut work: impl FnMut()) {
    for _ in 0..5 {
        work();
    }
    let mut times = Vec::with_capacity(iterations);
    let mut allocations = 0;
    let mut bytes = 0;
    for _ in 0..iterations {
        CALLS.store(0, Relaxed);
        BYTES.store(0, Relaxed);
        COUNT.store(true, Relaxed);
        let started = Instant::now();
        work();
        let elapsed = started.elapsed();
        COUNT.store(false, Relaxed);
        times.push(elapsed.as_secs_f64() * 1_000_000.);
        allocations += CALLS.load(Relaxed);
        bytes += BYTES.load(Relaxed);
    }
    times.sort_by(f64::total_cmp);
    println!(
        "{}",
        serde_json::json!({"name":name,"iterations":iterations,"us":{"p50":times[iterations/2],"p95":times[iterations*95/100],"p99":times[iterations*99/100]},"allocations":allocations as f64/iterations as f64,"allocated_bytes":bytes as f64/iterations as f64})
    );
}

struct Probe {
    scope: ConfigScope,
    form: Entity<FormState>,
    controls: Vec<(FormField<SharedString>, Entity<InputState>)>,
    owner_renders: Rc<Cell<usize>>,
    item_renders: Rc<Vec<Cell<usize>>>,
    _subscriptions: Vec<Subscription>,
}
impl Render for Probe {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        self.owner_renders.set(self.owner_renders.get() + 1);
        Form::new("form-perf", &self.form)
            .scope(self.scope.clone())
            .w(px(640.))
            .items(
                self.controls
                    .iter()
                    .enumerate()
                    .map(|(index, (field, editor))| {
                        let editor = editor.clone();
                        let renders = self.item_renders.clone();
                        FormItem::new(("perf-field", index))
                            .label(format!("Field {index}"))
                            .control(field, move |context| {
                                renders[index].set(renders[index].get() + 1);
                                Input::new(context.id, &editor)
                            })
                    }),
            )
    }
}
struct Fixture {
    cx: TestAppContext,
    window: WindowHandle<UiRoot>,
    form: Entity<FormState>,
    field: FormField<SharedString>,
    list: FormList,
    dependency_calls: Rc<Cell<usize>>,
    selected_notifications: Rc<Cell<usize>>,
    unrelated_notifications: Rc<Cell<usize>>,
    owner_renders: Rc<Cell<usize>>,
    owner_notifications: Rc<Cell<usize>>,
    item_renders: Rc<Vec<Cell<usize>>>,
}
impl Fixture {
    fn new(count: usize) -> Self {
        Self::with_controls(count, count.min(12))
    }
    fn with_controls(count: usize, control_count: usize) -> Self {
        let mut cx = TestAppContext::single();
        let mut retained = None;
        let window = cx.add_window(|window, cx| {
            ant_design_gpui::init(cx);
            let scope = ConfigScope::new_window(cx);
            scope
                .update_patch(cx, |patch| patch.motion = Some(false))
                .unwrap();
            let form = cx.new(|cx| FormState::new(window, cx));
            let fields = form.update(cx, |form, cx| {
                form.set_scope(scope.clone(), cx);
                (0..count)
                    .map(|index| {
                        form.register(
                            format!("field-{index}"),
                            FormCodec::text(),
                            FieldOptions::default().initial_value("value"),
                            cx,
                        )
                        .unwrap()
                    })
                    .collect::<Vec<_>>()
            });
            let dependency_calls = Rc::new(Cell::new(0));
            let calls = dependency_calls.clone();
            form.update(cx, |form, cx| {
                form.register(
                    "dependent",
                    FormCodec::text(),
                    FieldOptions::default()
                        .initial_value("ready")
                        .dependencies(["field-0"])
                        .rule(FormRule::custom(move |_, _, _| {
                            calls.set(calls.get() + 1);
                            FormRuleResult::Ready(Ok(()))
                        })),
                    cx,
                )
                .unwrap();
            });
            let list = form.update(cx, |form, cx| {
                form.register_list(
                    "rows",
                    Some(
                        (0..64)
                            .map(|index| FormValue::object([("name", format!("Row {index}"))]))
                            .collect(),
                    ),
                    cx,
                )
                .unwrap()
            });
            let selected = form.update(cx, |form, cx| form.watch("field-0", FormCodec::text(), cx));
            let unrelated = form.update(cx, |form, cx| {
                form.watch("field-999", FormCodec::text(), cx)
            });
            let selected_notifications = Rc::new(Cell::new(0));
            let unrelated_notifications = Rc::new(Cell::new(0));
            let controls = fields
                .iter()
                .take(control_count)
                .map(|field| (field.clone(), cx.new(|cx| InputState::new(window, cx))))
                .collect();
            let owner_renders = Rc::new(Cell::new(0));
            let owner_notifications = Rc::new(Cell::new(0));
            let item_renders: Rc<Vec<Cell<usize>>> =
                Rc::new((0..control_count).map(|_| Cell::new(0)).collect());
            let content = cx.new(|cx| {
                let selected_count = selected_notifications.clone();
                let unrelated_count = unrelated_notifications.clone();
                let owner_count = owner_notifications.clone();
                Probe {
                    scope: scope.clone(),
                    form: form.clone(),
                    controls,
                    owner_renders: owner_renders.clone(),
                    item_renders: item_renders.clone(),
                    _subscriptions: vec![
                        cx.observe_self(move |_, _| owner_count.set(owner_count.get() + 1)),
                        cx.observe(&selected, move |_, _, _| {
                            selected_count.set(selected_count.get() + 1)
                        }),
                        cx.observe(&unrelated, move |_, _, _| {
                            unrelated_count.set(unrelated_count.get() + 1)
                        }),
                        // Retain watcher entities without introducing a form-wide observer.
                        cx.on_release(move |_, _| {
                            drop(selected);
                            drop(unrelated);
                        }),
                    ],
                }
            });
            retained = Some((
                form,
                fields[0].clone(),
                list,
                dependency_calls,
                selected_notifications,
                unrelated_notifications,
                owner_renders,
                owner_notifications,
                item_renders,
            ));
            UiRoot::new(scope, content, window, cx)
        });
        let (
            form,
            field,
            list,
            dependency_calls,
            selected_notifications,
            unrelated_notifications,
            owner_renders,
            owner_notifications,
            item_renders,
        ) = retained.unwrap();
        let mut fixture = Self {
            cx,
            window,
            form,
            field,
            list,
            dependency_calls,
            selected_notifications,
            unrelated_notifications,
            owner_renders,
            owner_notifications,
            item_renders,
        };
        fixture.draw();
        fixture
    }
    fn settle(&mut self) {
        for _ in 0..8 {
            let frames = self.app(|window, cx| window.simulate_next_frame(cx));
            if frames == 0 {
                return;
            }
            self.draw();
        }
        panic!("Form did not settle within its bounded measurement passes");
    }
    fn app<R>(&mut self, work: impl FnOnce(&mut Window, &mut App) -> R) -> R {
        let result = self
            .cx
            .update_window(self.window.into(), |_, window, cx| work(window, cx))
            .unwrap();
        self.cx.run_until_parked();
        result
    }
    fn draw(&mut self) {
        self.app(|window, cx| {
            window.draw(cx).clear(cx);
        });
    }
    fn idle(&mut self) -> usize {
        self.cx.executor().advance_clock(Duration::from_secs(2));
        self.cx.run_until_parked();
        self.app(|window, cx| {
            window.simulate_next_frame(cx);
            window.draw(cx).clear(cx);
        });
        self.app(|window, cx| window.simulate_next_frame(cx))
    }
}

fn main() {
    // Keep this a runtime guard: development check/clippy must compile the
    // target, while execution is reserved for the Release performance preset.
    assert!(
        !black_box(cfg!(debug_assertions)),
        "run form benchmarks with the Release performance preset"
    );
    for width in [1000, 10_000] {
        let mut values = FormValue::object(
            (0..width).map(|index| (format!("field-{index}"), FormValue::native(vec![index; 8]))),
        );
        let untouched = values
            .get(&NamePath::from(format!("field-{}", width - 1)))
            .unwrap()
            .clone();
        let mut previous = Vec::new();
        let mut revision = 0;
        measure(&format!("snapshot_update_{width}_siblings"), 256, || {
            previous.push(values.clone());
            revision += 1;
            values = values
                .with_path(
                    &NamePath::from("field-0"),
                    FormValue::text(revision.to_string()),
                )
                .unwrap();
            assert!(
                values
                    .get(&NamePath::from(format!("field-{}", width - 1)))
                    .unwrap()
                    .ptr_eq(&untouched)
            );
            black_box(&previous);
        });
    }
    for count in [10, 100, 1000] {
        let mut native = Fixture::with_controls(count, count);
        native.settle();
        assert!(native.item_renders.iter().all(|renders| renders.get() > 0));
        let before: Vec<_> = native.item_renders.iter().map(Cell::get).collect();
        let owner_before = native.owner_renders.get();
        let notifications_before = native.owner_notifications.get();
        let field = native.field.clone();
        let mut revision = 0;
        measure(
            &format!("native_field_update_with_automatic_draw_{count}_controls"),
            64,
            || {
                revision += 1;
                native.app(|window, cx| {
                    field
                        .set(
                            FormPresence::Value(format!("edit-{revision}").into()),
                            window,
                            cx,
                        )
                        .unwrap();
                });
                // GPUI's test-mode effect flush already draws the dirty window.
                // app() settles that pipeline; another draw would double-count.
            },
        );
        assert!(native.item_renders[0].get() > before[0]);
        assert_eq!(
            native.owner_notifications.get(),
            notifications_before,
            "a field edit notified its owning view"
        );
        for (renders, before) in native.item_renders.iter().zip(before).skip(1) {
            assert_eq!(
                renders.get(),
                before,
                "field edit rebuilt an unrelated native item"
            );
        }
        let owner_renders_including_warmup = native.owner_renders.get() - owner_before;
        let idle = native.idle();
        assert_eq!(idle, 0, "settled native Form requested idle callbacks");
        println!(
            "{}",
            serde_json::json!({
                "kind":"native_assertions", "native_controls":count,
                "owner_renders_including_warmup":owner_renders_including_warmup,
                "work_iterations_including_warmup":69,
                "owner_notifications":native.owner_notifications.get()-notifications_before,
                "unrelated_item_rebuilds":0,
                "idle_frame_callbacks":idle,
            })
        );
    }
    let mut fixture = Fixture::new(1000);
    let mut revision = 0;
    let field = fixture.field.clone();
    measure("silent_field_set_1000_fields", 128, || {
        revision += 1;
        fixture.app(|window, cx| {
            field
                .set(FormPresence::Value(revision.to_string().into()), window, cx)
                .unwrap()
        });
    });
    assert_eq!(
        fixture.unrelated_notifications.get(),
        0,
        "unrelated watcher was notified"
    );
    assert!(fixture.selected_notifications.get() >= 128);
    measure(
        "user_change_with_dependency_and_native_field_frame",
        128,
        || {
            revision += 1;
            fixture.app(|window, cx| {
                field
                    .change(
                        FormPresence::Value(revision.to_string().into()),
                        ValidationTrigger::Change,
                        window,
                        cx,
                    )
                    .unwrap()
            });
        },
    );
    assert!(fixture.dependency_calls.get() >= 128);
    let list = fixture.list.clone();
    measure("list_move_add_remove_64_rows", 64, || {
        fixture.app(|window, cx| {
            let before = list.items(cx);
            list.move_item(0, 63, window, cx).unwrap();
            assert_eq!(list.items(cx)[63].key, before[0].key);
            list.add(FormValue::object([("name", "Added")]), Some(1), window, cx)
                .unwrap();
            list.remove([1], window, cx).unwrap();
            assert_eq!(list.items(cx).len(), 64);
        });
    });
    let form = fixture.form.clone();
    measure("validate_and_reset_1000_fields", 32, || {
        let validation = fixture.app(|window, cx| {
            form.update(cx, |form, cx| {
                form.validate_fields(ValidateOptions::default(), window, cx)
            })
        });
        assert!(validation.is_finished());
        fixture.app(|window, cx| form.update(cx, |form, cx| form.reset(window, cx)));
    });
    fixture.draw();
    let idle = fixture.idle();
    assert_eq!(idle, 0, "settled form requested idle animation callbacks");
    println!(
        "{}",
        serde_json::json!({"kind":"assertions","persistent_snapshot_sharing":true,"unrelated_watcher_notifications":fixture.unrelated_notifications.get(),"stable_list_keys":true,"idle_frame_callbacks":idle,"measurement":"Release CPU and allocations; deterministic offscreen GPUI, no GPU presentation or physical input latency"})
    );
}
