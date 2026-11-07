// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
//! Release CPU/allocation work; not GPU presentation or physical input latency.

use ant_design_gpui::{
    ConfigScope, UiRoot,
    date_picker::{
        DateRangeSelection, DateSelection, DateTimeSelection, NaiveDate, NaiveDateTime, Picker,
        PickerFormat, PickerFormatContext, PickerOptions, PickerPanelMode, PickerRange,
        PickerSelection, PickerState, TimeOptions, TimeSelection, TimeUnit, WeekRules, add_months,
        panel_cells,
    },
};
use chrono::Datelike;
use gpui::{
    AppContext, Context, Entity, EntityId, IntoElement, MouseButton, MouseDownEvent,
    MouseMoveEvent, MouseUpEvent, PlatformInput, Render, ScrollDelta, ScrollEvent, TestAppContext,
    Window, WindowHandle, div, point, prelude::*, px,
};
use std::{
    alloc::{GlobalAlloc, Layout, System},
    hint::black_box,
    sync::atomic::{AtomicBool, AtomicU64, Ordering::Relaxed},
    time::{Duration, Instant},
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
    unsafe fn alloc(&self, l: Layout) -> *mut u8 {
        let p = unsafe { System.alloc(l) };
        if !p.is_null() {
            allocated(l.size());
        }
        p
    }
    unsafe fn alloc_zeroed(&self, l: Layout) -> *mut u8 {
        let p = unsafe { System.alloc_zeroed(l) };
        if !p.is_null() {
            allocated(l.size());
        }
        p
    }
    unsafe fn dealloc(&self, p: *mut u8, l: Layout) {
        unsafe {
            System.dealloc(p, l);
        }
    }
    unsafe fn realloc(&self, p: *mut u8, l: Layout, size: usize) -> *mut u8 {
        let p = unsafe { System.realloc(p, l, size) };
        if !p.is_null() {
            allocated(size);
        }
        p
    }
}
fn measure(name: &str, count: usize, mut work: impl FnMut()) {
    if std::env::args().any(|arg| arg == "--validate-only") {
        for _ in 0..3 {
            work();
        }
        println!(
            "{}",
            serde_json::json!({"kind":"validation","name":name,"status":"passed"})
        );
        return;
    }
    for _ in 0..5 {
        work();
    }
    let mut samples = Vec::with_capacity(count);
    let (mut allocations, mut bytes) = (0, 0);
    for _ in 0..count {
        CALLS.store(0, Relaxed);
        BYTES.store(0, Relaxed);
        COUNT.store(true, Relaxed);
        let start = Instant::now();
        work();
        let elapsed = start.elapsed().as_secs_f64() * 1_000_000.;
        COUNT.store(false, Relaxed);
        samples.push(elapsed);
        allocations += CALLS.load(Relaxed);
        bytes += BYTES.load(Relaxed);
    }
    samples.sort_by(f64::total_cmp);
    let percentile = |p: f64| samples[((count - 1) as f64 * p).round() as usize];
    println!(
        "{}",
        serde_json::json!({"name":name,"iterations":count,"us":{"p50":percentile(0.5),"p95":percentile(0.95),"p99":percentile(0.99)},"allocations":allocations as f64/count as f64,"allocated_bytes":bytes as f64/count as f64})
    );
}
fn date(day: u32) -> NaiveDate {
    NaiveDate::from_ymd_opt(2024, 2, day).unwrap()
}
fn now() -> NaiveDateTime {
    date(15).and_hms_milli_opt(13, 24, 35, 120).unwrap()
}
struct Probe<S: PickerSelection> {
    scope: ConfigScope,
    state: Entity<PickerState<S>>,
    open: bool,
    milliseconds: bool,
    format: PickerFormat,
}
impl<S: PickerSelection> Render for Probe<S> {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        let mut picker = Picker::<S>::new("picker", &self.state)
            .scope(self.scope.clone())
            .open(self.open)
            .clock(now);
        if self.milliseconds {
            picker = picker
                .options(PickerOptions {
                    time: TimeOptions {
                        show_millisecond: true,
                        millisecond_step: 1,
                        ..Default::default()
                    },
                    ..Default::default()
                })
                .format(self.format.clone());
        }
        div().w(px(1000.)).h(px(760.)).p(px(32.)).child(picker)
    }
}
struct Fixture<S: PickerSelection> {
    cx: TestAppContext,
    window: WindowHandle<UiRoot>,
    probe: Entity<Probe<S>>,
    editors: [EntityId; 2],
}
impl<S: PickerSelection> Fixture<S> {
    fn new(value: S::Value, open: bool, milliseconds: bool) -> Self {
        let mut cx = TestAppContext::single();
        cx.update(|cx| cx.set_reduce_motion(false));
        let mut probe = None;
        let window = cx.add_window(|window, cx| {
            window.activate_window();
            ant_design_gpui::init(cx);
            let scope = ConfigScope::new_window(cx);
            scope
                .update_patch(cx, |patch| patch.motion = Some(false))
                .unwrap();
            let state = cx.new(|cx| {
                let mut state = PickerState::<S>::new(window, cx);
                state.set_value(value, window, cx);
                state
            });
            let entity = cx.new(|_| Probe {
                scope: scope.clone(),
                state,
                open,
                milliseconds,
                format: if milliseconds {
                    PickerFormat::new("%H:%M:%S%.3f").unwrap()
                } else {
                    PickerFormat::default()
                },
            });
            probe = Some(entity.clone());
            UiRoot::new(scope, entity, window, cx)
        });
        let probe = probe.unwrap();
        let editors = probe.read_with(&cx, |p, cx| p.state.read(cx).retained_editor_ids());
        let mut fixture = Self {
            cx,
            window,
            probe,
            editors,
        };
        for _ in 0..3 {
            fixture.draw(false);
        }
        fixture
    }
    fn draw(&mut self, refresh: bool) {
        self.cx.run_until_parked();
        self.probe.read_with(&self.cx, |probe, cx| {
            let config = probe.scope.snapshot(cx);
            assert_eq!(
                config.motion_policy(cx).allows_motion(),
                config.motion,
                "benchmark motion policy must be effective"
            );
        });
        assert_eq!(
            self.editors,
            self.probe
                .read_with(&self.cx, |p, cx| p.state.read(cx).retained_editor_ids()),
            "date/time editors must retain their entity identities"
        );
        self.cx.update_window(self.window.into(),|_,window,cx|{
            assert!(window.is_window_active()&&window.is_visible(),"benchmark TestWindow must be active and visible");
            if refresh{window.refresh();}window.draw(cx).clear(cx);
            let scene=window.rendered_scene();let primitives=scene.quads.len()+scene.paths.len()+scene.shadows.len()+scene.monochrome_sprites.len()+scene.subpixel_sprites.len()+scene.polychrome_sprites.len();
            assert!(primitives<=1200,"date/time rendering must be bounded by visible rows, not the millisecond domain");
            assert_eq!(window.simulate_next_frame(cx),0,"settled date/time picker must not request idle frames");
        }).unwrap();
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
    fn geometry(&self) -> ant_design_gpui::date_picker::PickerGeometry {
        self.probe
            .read_with(&self.cx, |p, cx| p.state.read(cx).geometry().clone())
    }
    fn click(&mut self, position: gpui::Point<gpui::Pixels>) {
        self.pointer(PlatformInput::MouseDown(MouseDownEvent {
            position,
            button: MouseButton::Left,
            click_count: 1,
            ..Default::default()
        }));
        self.pointer(PlatformInput::MouseUp(MouseUpEvent {
            position,
            button: MouseButton::Left,
            ..Default::default()
        }));
    }
    fn set_value(&mut self, value: S::Value) {
        let state = self.probe.read_with(&self.cx, |p, _| p.state.clone());
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                state.update(cx, |s, cx| s.set_value(value, window, cx))
            })
            .unwrap();
        self.draw(false);
    }
    fn set_motion(&mut self, enabled: bool) {
        let scope = self.probe.read_with(&self.cx, |p, _| p.scope.clone());
        self.cx
            .update_window(self.window.into(), |_, _, cx| {
                scope
                    .update_patch(cx, |patch| patch.motion = Some(enabled))
                    .unwrap();
            })
            .unwrap();
        for _ in 0..3 {
            self.draw(true);
        }
    }
    fn set_open(&mut self, open: bool) {
        self.probe.update(&mut self.cx, |probe, cx| {
            probe.open = open;
            cx.notify();
        });
        for _ in 0..2 {
            self.draw(false);
        }
        assert_eq!(
            self.probe
                .read_with(&self.cx, |probe, cx| probe.state.read(cx).is_open()),
            open
        );
        assert_eq!(
            self.geometry().popup.is_some(),
            open,
            "cycle must lay out and remove the actual popup"
        );
    }
    fn open_and_settle_motion(&mut self) {
        self.probe.update(&mut self.cx, |probe, cx| {
            probe.open = true;
            cx.notify();
        });
        let mut callbacks = 0;
        for frame in 0..61 {
            if frame > 0 {
                self.cx.executor().advance_clock(Duration::from_millis(16));
            }
            self.cx.run_until_parked();
            callbacks += self
                .cx
                .update_window(self.window.into(), |_, window, cx| {
                    assert!(window.is_window_active() && window.is_visible());
                    window.draw(cx).clear(cx);
                    window.simulate_next_frame(cx)
                })
                .unwrap();
        }
        assert!(
            callbacks > 0,
            "active motion-enabled opening must exercise actual animation callbacks"
        );
        self.draw(false);
        println!(
            "{}",
            serde_json::json!({"kind":"fixture-policy","name":"motion-enabled-popup","active":true,"visible":true,
            "reduced_motion":false,"opening_animation_callbacks":callbacks,"settled_idle_callbacks":0})
        );
    }
    fn first_visible_hour(&self) -> u32 {
        let geometry = self.geometry();
        let column = geometry.time_columns[0];
        geometry
            .cells
            .values()
            .filter(|cell| {
                cell.unit == Some(TimeUnit::Hour)
                    && cell.bounds.bottom() > column.top()
                    && cell.bounds.top() < column.bottom()
            })
            .min_by(|a, b| f32::from(a.bounds.top()).total_cmp(&f32::from(b.bounds.top())))
            .and_then(|cell| cell.value)
            .expect("visible virtualized hour cell")
    }
    fn wheel(&mut self, delta_y: f32) {
        let column = self.geometry().time_columns[0];
        self.pointer(PlatformInput::Scroll(ScrollEvent {
            position: column.center(),
            delta: ScrollDelta::Pixels(point(px(0.), px(delta_y))),
            ..Default::default()
        }));
    }
}

struct ManyProbe {
    scope: ConfigScope,
    states: Vec<Entity<PickerState<DateSelection>>>,
}
impl Render for ManyProbe {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        div()
            .w(px(1000.))
            .h(px(760.))
            .p(px(32.))
            .flex()
            .flex_wrap()
            .content_start()
            .gap(px(4.))
            .children(self.states.iter().enumerate().map(|(index, state)| {
                Picker::<DateSelection>::new(("many-date-picker", index), state)
                    .scope(self.scope.clone())
                    .clock(now)
                    .default_value(Some(date(12)))
            }))
    }
}
struct ManyFixture {
    cx: TestAppContext,
    window: WindowHandle<UiRoot>,
    probe: Entity<ManyProbe>,
    ids: Vec<[EntityId; 2]>,
}
impl ManyFixture {
    fn new() -> Self {
        let mut cx = TestAppContext::single();
        cx.update(|cx| cx.set_reduce_motion(false));
        let mut probe = None;
        let window = cx.add_window(|window, cx| {
            window.activate_window();
            ant_design_gpui::init(cx);
            let scope = ConfigScope::new_window(cx);
            scope
                .update_patch(cx, |patch| patch.motion = Some(true))
                .unwrap();
            let states = (0..100)
                .map(|_| cx.new(|cx| PickerState::new(window, cx)))
                .collect();
            let entity = cx.new(|_| ManyProbe {
                scope: scope.clone(),
                states,
            });
            probe = Some(entity.clone());
            UiRoot::new(scope, entity, window, cx)
        });
        let probe = probe.unwrap();
        let ids = probe.read_with(&cx, |p, cx| {
            p.states
                .iter()
                .map(|s| s.read(cx).retained_editor_ids())
                .collect()
        });
        let mut fixture = Self {
            cx,
            window,
            probe,
            ids,
        };
        for _ in 0..3 {
            fixture.draw();
        }
        fixture
    }
    fn draw(&mut self) {
        self.cx.run_until_parked();
        assert!(
            self.probe.read_with(&self.cx, |probe, cx| probe
                .scope
                .snapshot(cx)
                .motion_policy(cx)
                .allows_motion()),
            "100-picker motion policy must be enabled"
        );
        let ids: Vec<_> = self.probe.read_with(&self.cx, |p, cx| {
            p.states
                .iter()
                .map(|s| s.read(cx).retained_editor_ids())
                .collect()
        });
        assert_eq!(
            ids, self.ids,
            "all 100 mounted pickers must retain editor identities"
        );
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                assert!(
                    window.is_window_active() && window.is_visible(),
                    "100-picker TestWindow must be active and visible"
                );
                window.refresh();
                window.draw(cx).clear(cx);
                let scene = window.rendered_scene();
                let primitives = scene.quads.len()
                    + scene.paths.len()
                    + scene.shadows.len()
                    + scene.monochrome_sprites.len()
                    + scene.subpixel_sprites.len()
                    + scene.polychrome_sprites.len();
                assert!(
                    primitives <= 6400,
                    "100 closed pickers must have bounded rendered work"
                );
                assert_eq!(
                    window.simulate_next_frame(cx),
                    0,
                    "100 settled motion-enabled pickers must schedule no idle frames"
                );
            })
            .unwrap();
        self.cx.run_until_parked();
    }
}
fn main() {
    if cfg!(debug_assertions) {
        eprintln!("Use scripts/run-date-time-picker-perf.ps1 (Release)");
        std::process::exit(1);
    }
    let format = PickerFormat::new("%Y-%m-%d %H:%M:%S%.3f").unwrap();
    let context =
        PickerFormatContext::new(ant_design_gpui::date_picker::PickerMode::Date, true, true);
    measure("date-time-picker/compiled-format", 1000, || {
        black_box(format.format(black_box(now()), context));
    });
    measure("date-time-picker/compiled-parse", 1000, || {
        black_box(format.parse(black_box("2024-02-15 13:24:35.120"), context)).unwrap();
    });
    let month_end = NaiveDate::from_ymd_opt(2024, 1, 31).unwrap();
    let leap_day = NaiveDate::from_ymd_opt(2024, 2, 29).unwrap();
    assert_eq!(add_months(month_end, 1), Some(leap_day));
    assert_eq!(
        add_months(leap_day, 12),
        NaiveDate::from_ymd_opt(2025, 2, 28)
    );
    measure("date-time-picker/checked-month-navigation", 1000, || {
        black_box(add_months(black_box(month_end), black_box(1))).unwrap();
        black_box(add_months(black_box(leap_day), black_box(12))).unwrap();
        assert!(black_box(add_months(black_box(NaiveDate::MAX), black_box(1))).is_none());
    });
    let panels = [
        PickerPanelMode::Date,
        PickerPanelMode::Month,
        PickerPanelMode::Quarter,
        PickerPanelMode::Year,
        PickerPanelMode::Decade,
    ];
    for mode in panels {
        assert!(panel_cells(leap_day, mode, WeekRules::ISO).len() <= 42);
    }
    measure(
        "date-time-picker/bounded-calendar-grid-construction",
        1000,
        || {
            for mode in panels {
                let cells = panel_cells(
                    black_box(leap_day),
                    black_box(mode),
                    black_box(WeekRules::ISO),
                );
                assert!(cells.len() <= 42);
                black_box(cells);
            }
        },
    );
    let mut closed = Fixture::<DateSelection>::new(Some(date(12)), false, false);
    measure("date-time-picker/closed-date-redraw", 200, || {
        closed.draw(true)
    });
    let mut single = Fixture::<DateSelection>::new(Some(date(12)), true, false);
    measure("date-time-picker/open-date-redraw", 200, || {
        single.draw(true)
    });
    let mut opening = Fixture::<DateSelection>::new(Some(date(12)), false, false);
    measure("date-time-picker/popup-open-close-cycle", 200, || {
        opening.set_open(true);
        opening.set_open(false);
    });
    let mut motion_enabled = Fixture::<DateSelection>::new(Some(date(12)), false, false);
    motion_enabled.set_motion(true);
    motion_enabled.open_and_settle_motion();
    measure(
        "date-time-picker/open-date-motion-enabled-idle-redraw",
        200,
        || motion_enabled.draw(true),
    );
    let header = single.geometry().headers[0];
    let next = point(header.right() - px(38.), header.center().y);
    let before = single
        .probe
        .read_with(&single.cx, |probe, cx| probe.state.read(cx).panel_date());
    single.click(next);
    let after = single
        .probe
        .read_with(&single.cx, |probe, cx| probe.state.read(cx).panel_date());
    assert_eq!(
        after.year() * 12 + after.month0() as i32,
        before.year() * 12 + before.month0() as i32 + 1,
        "the measured pointer target must navigate exactly one month"
    );
    measure("date-time-picker/next-month-pointer", 100, || {
        single.click(next)
    });
    let mut range = Fixture::<DateRangeSelection>::new(
        PickerRange::new(Some(date(8)), Some(date(21))),
        true,
        false,
    );
    measure("date-time-picker/open-range-redraw", 200, || {
        range.draw(true)
    });
    let geometry = range.geometry();
    let positions = [12, 20].map(|day| {
        geometry
            .cells
            .values()
            .find(|cell| cell.in_view && cell.date == Some(date(day)))
            .expect("deterministic visible range-hover target")
            .bounds
            .center()
    });
    let mut index = 0;
    measure("date-time-picker/range-hover-pointer", 200, || {
        index ^= 1;
        range.pointer(PlatformInput::MouseMove(MouseMoveEvent {
            position: positions[index],
            ..Default::default()
        }));
    });
    let mut datetime = Fixture::<DateTimeSelection>::new(Some(now()), true, false);
    measure("date-time-picker/open-datetime-redraw", 200, || {
        datetime.draw(true)
    });
    let mut time = Fixture::<TimeSelection>::new(Some(now().time()), true, false);
    measure("date-time-picker/open-time-redraw", 200, || time.draw(true));
    let initial_hour = time.first_visible_hour();
    time.wheel(-28.);
    assert_ne!(
        time.first_visible_hour(),
        initial_hour,
        "wheel input must move actual virtualized rows"
    );
    time.wheel(28.);
    assert_eq!(
        time.first_visible_hour(),
        initial_hour,
        "opposite wheel input must restore actual rows"
    );
    let mut down = false;
    measure(
        "date-time-picker/time-column-native-wheel-scroll",
        200,
        || {
            down = !down;
            time.wheel(if down { -28. } else { 28. });
        },
    );
    let mut milliseconds = Fixture::<TimeSelection>::new(Some(now().time()), true, true);
    measure("date-time-picker/millisecond-step-1-redraw", 200, || {
        milliseconds.draw(true)
    });
    let mut alternate = false;
    measure("date-time-picker/millisecond-selection-sync", 200, || {
        alternate = !alternate;
        milliseconds.set_value(Some(
            date(15)
                .and_hms_milli_opt(13, 24, 35, if alternate { 120 } else { 720 })
                .unwrap()
                .time(),
        ));
    });
    let mut many = ManyFixture::new();
    measure(
        "date-time-picker/100-mounted-closed-motion-enabled-redraw",
        100,
        || many.draw(),
    );
}
