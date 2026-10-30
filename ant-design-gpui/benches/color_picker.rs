// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
//! Release CPU/allocation distributions; not GPU or physical input-latency measurements.

use ant_design_gpui::{
    ColorPicker, ColorPickerMode, ColorPickerPreset, ColorPickerState, ColorPickerValue, ColorStop,
    ConfigScope, PickerColor, UiRoot,
};
use gpui::{
    AppContext, Context, Entity, EntityId, IntoElement, MouseButton, MouseDownEvent,
    MouseMoveEvent, MouseUpEvent, Pixels, PlatformInput, Point, Render, TestAppContext, Window,
    WindowHandle, div, point, prelude::*, px,
};
use std::{
    alloc::{GlobalAlloc, Layout, System},
    hint::black_box,
    sync::Arc,
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

fn measure(name: &str, count: usize, mut work: impl FnMut()) {
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
        samples.push(start.elapsed().as_secs_f64() * 1_000_000.);
        COUNT.store(false, Relaxed);
        allocations += CALLS.load(Relaxed);
        bytes += BYTES.load(Relaxed);
    }
    samples.sort_by(f64::total_cmp);
    let percentile = |p: f64| samples[((count - 1) as f64 * p).round() as usize];
    println!(
        "{}",
        serde_json::json!({"name":name,"iterations":count,
        "us":{"p50":percentile(0.5),"p95":percentile(0.95),"p99":percentile(0.99)},
        "allocations":allocations as f64/count as f64,"allocated_bytes":bytes as f64/count as f64})
    );
}

struct Probe {
    scope: ConfigScope,
    state: Entity<ColorPickerState>,
    open: bool,
    presets: Option<Arc<[ColorPickerPreset]>>,
}
impl Render for Probe {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        let mut picker = ColorPicker::new("picker", &self.state)
            .scope(self.scope.clone())
            .open(self.open)
            .modes([ColorPickerMode::Single, ColorPickerMode::Gradient]);
        if let Some(presets) = &self.presets {
            picker = picker.presets(presets.iter().cloned());
        }
        div().w(px(640.)).h(px(720.)).p(px(48.)).child(picker)
    }
}

struct Fixture {
    cx: TestAppContext,
    window: WindowHandle<UiRoot>,
    probe: Entity<Probe>,
    editors: [EntityId; 6],
    primitive_limit: usize,
}
impl Fixture {
    fn new() -> Self {
        let mut cx = TestAppContext::single();
        let mut probe = None;
        let window = cx.add_window(|window, cx| {
            ant_design_gpui::init(cx);
            let scope = ConfigScope::new_window(cx);
            scope
                .update_patch(cx, |patch| patch.motion = Some(false))
                .unwrap();
            let state = cx.new(|cx| {
                let mut state = ColorPickerState::new(window, cx);
                state
                    .set_value("#1677ff".parse().unwrap(), window, cx)
                    .unwrap();
                state
            });
            let entity = cx.new(|_| Probe {
                scope: scope.clone(),
                state,
                open: false,
                presets: None,
            });
            probe = Some(entity.clone());
            UiRoot::new(scope, entity, window, cx)
        });
        let probe = probe.unwrap();
        let editors = probe.read_with(&cx, |probe, cx| probe.state.read(cx).retained_editor_ids());
        let mut fixture = Self {
            cx,
            window,
            probe,
            editors,
            primitive_limit: 64,
        };
        fixture.draw(false);
        fixture
    }
    fn draw(&mut self, refresh: bool) {
        self.cx.run_until_parked();
        assert_eq!(
            self.editors,
            self.probe.read_with(&self.cx, |probe, cx| probe
                .state
                .read(cx)
                .retained_editor_ids()),
            "channel editors must retain entity identity through edits"
        );
        let primitive_limit = self.primitive_limit;
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                if refresh {
                    window.refresh();
                }
                window.draw(cx).clear(cx);
                let scene = window.rendered_scene();
                assert!(
                    scene.quads.len()
                        + scene.paths.len()
                        + scene.shadows.len()
                        + scene.monochrome_sprites.len()
                        + scene.subpixel_sprites.len()
                        + scene.polychrome_sprites.len()
                        <= primitive_limit,
                    "picker surfaces must remain bounded by controls and gradient stops"
                );
                assert!(
                    scene
                        .paths
                        .iter()
                        .map(|path| path.vertices.len())
                        .sum::<usize>()
                        <= primitive_limit * 64,
                    "gradient mesh detail must remain bounded"
                );
                assert_eq!(
                    window.simulate_next_frame(cx),
                    0,
                    "settled ColorPicker must not schedule frames"
                );
            })
            .unwrap();
        self.cx.run_until_parked();
    }
    fn print_primitives(&mut self, name: &str) {
        let counts=self.cx.update_window(self.window.into(), |_,window,_|{
            let scene=window.rendered_scene();
            serde_json::json!({"name":name,"kind":"scene-primitives","quads":scene.quads.len(),"paths":scene.paths.len(),"shadows":scene.shadows.len(),"glyph_sprites":scene.monochrome_sprites.len()+scene.subpixel_sprites.len()+scene.polychrome_sprites.len(),"path_vertices":scene.paths.iter().map(|p|p.vertices.len()).sum::<usize>(),"primitive_limit":self.primitive_limit})
        }).unwrap();
        println!("{counts}");
    }
    fn pointer(&mut self, event: PlatformInput) {
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                window.dispatch_event(event, cx);
            })
            .unwrap();
        self.draw(false);
    }
    fn set_value(&mut self, value: ColorPickerValue) {
        let state = self
            .probe
            .read_with(&self.cx, |probe, _| probe.state.clone());
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                state.update(cx, |state, cx| state.set_value(value, window, cx).unwrap())
            })
            .unwrap();
        self.draw(false);
    }
    fn drag(
        &mut self,
        name: &str,
        start: Point<Pixels>,
        first: Point<Pixels>,
        second: Point<Pixels>,
    ) {
        self.pointer(PlatformInput::MouseDown(MouseDownEvent {
            position: start,
            button: MouseButton::Left,
            click_count: 1,
            ..Default::default()
        }));
        let mut alternate = false;
        measure(name, 200, || {
            alternate = !alternate;
            self.pointer(PlatformInput::MouseMove(MouseMoveEvent {
                position: if alternate { first } else { second },
                pressed_button: Some(MouseButton::Left),
                ..Default::default()
            }));
        });
        self.pointer(PlatformInput::MouseUp(MouseUpEvent {
            position: second,
            button: MouseButton::Left,
            ..Default::default()
        }));
    }
}

fn main() {
    if cfg!(debug_assertions) {
        eprintln!("Use scripts/run-color-picker-perf.ps1 (Release)");
        std::process::exit(1);
    }
    let mut hue = 0.;
    measure("color-picker/hsba-to-rgba", 1_000, || {
        hue = (hue + 0.173) % 360.;
        black_box(PickerColor::new(hue, 0.731, 0.839, 0.57).unwrap().to_rgba());
    });
    measure("color-picker/parse-rgba", 1_000, || {
        black_box(PickerColor::parse(black_box("rgba(22, 119, 255, 0.57)")).unwrap());
    });
    let mut fixture = Fixture::new();
    measure("color-picker/closed-trigger-redraw", 200, || {
        fixture.draw(true)
    });
    fixture.print_primitives("color-picker/closed-trigger");
    fixture.primitive_limit = 256;
    fixture.probe.update(&mut fixture.cx, |probe, cx| {
        probe.open = true;
        cx.notify();
    });
    fixture.draw(false);
    measure("color-picker/open-panel-redraw", 200, || fixture.draw(true));
    fixture.print_primitives("color-picker/open-panel");
    let geometry = fixture
        .probe
        .read_with(&fixture.cx, |probe, cx| probe.state.read(cx).geometry());
    for (name, bounds) in [
        ("palette", geometry.palette),
        ("hue", geometry.hue),
        ("alpha", geometry.alpha),
    ] {
        let bounds = bounds.expect("visible picker surface");
        fixture.drag(
            &format!("color-picker/{name}-drag"),
            bounds.center(),
            point(
                bounds.left() + bounds.size.width * 0.2,
                bounds.top() + bounds.size.height * 0.3,
            ),
            point(
                bounds.left() + bounds.size.width * 0.8,
                bounds.top() + bounds.size.height * 0.7,
            ),
        );
    }
    for translucent in [false, true] {
        for count in [2, 8, 32] {
            let opacity = if translucent { "translucent" } else { "opaque" };
            fixture.primitive_limit = 256 + 4 * count;
            let value = ColorPickerValue::gradient((0..count).map(|index| {
                ColorStop::new(
                    PickerColor::new(
                        index as f64 * 360. / count as f64,
                        0.8,
                        0.9,
                        if translucent {
                            0.3 + 0.7 * index as f64 / count as f64
                        } else {
                            1.
                        },
                    )
                    .unwrap(),
                    index as f64 * 100. / (count - 1) as f64,
                )
                .unwrap()
            }))
            .unwrap();
            fixture.set_value(value);
            measure(
                &format!("color-picker/{opacity}-gradient-{count}-stops-redraw"),
                100,
                || fixture.draw(true),
            );
            fixture.print_primitives(&format!("color-picker/{opacity}-gradient-{count}-stops"));
        }
    }
    let geometry = fixture
        .probe
        .read_with(&fixture.cx, |probe, cx| probe.state.read(cx).geometry());
    let rail = geometry.gradient.unwrap();
    let stop = geometry
        .handles
        .iter()
        .find(|handle| handle.kind.as_ref() == "gradient" && handle.index == 0)
        .unwrap()
        .bounds
        .center();
    fixture.drag(
        "color-picker/translucent-gradient-32-stops-stop-drag",
        stop,
        point(rail.left() + rail.size.width * 0.2, rail.center().y),
        point(rail.left() + rail.size.width * 0.8, rail.center().y),
    );
    fixture.set_value("#1677ff".parse().unwrap());
    for count in [24, 240] {
        // Each visible cell uses a checker, fill, inset shadow and four
        // disjoint border strips; reserve one more for a selected checkmark.
        fixture.primitive_limit = 256 + 8 * count;
        let groups: Arc<[ColorPickerPreset]> = vec![ColorPickerPreset::new(
            "stress",
            "Presets",
            (0..count).map(|index| {
                ColorPickerValue::Single(
                    PickerColor::new(index as f64 * 360. / count as f64, 0.8, 0.9, 1.).unwrap(),
                )
            }),
        )]
        .into();
        fixture.probe.update(&mut fixture.cx, |probe, cx| {
            probe.presets = Some(groups);
            cx.notify();
        });
        fixture.draw(false);
        measure(
            &format!("color-picker/presets-{count}-warm-redraw"),
            100,
            || fixture.draw(true),
        );
        fixture.print_primitives(&format!("color-picker/presets-{count}"));
    }
}
