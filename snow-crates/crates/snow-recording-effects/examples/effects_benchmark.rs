//! Run through the windows-msvc-performance target, with native font rendering.
use snow_recording_effects::keyboard_overlay::{KeyEvent, KeyboardOverlayConfig};
use snow_recording_effects::mouse_effects::RenderClick;
use snow_recording_effects::mouse_hook::ObservedMouseButton;
use snow_recording_effects::preview::{EffectsPreview, PreviewConfig};
use std::alloc::{GlobalAlloc, Layout, System};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::time::Instant;

struct CountingAllocator;
static ALLOCATIONS: AtomicUsize = AtomicUsize::new(0);
static LIVE_BYTES: AtomicUsize = AtomicUsize::new(0);
static MAX_ALLOCATION: AtomicUsize = AtomicUsize::new(0);
#[global_allocator]
static ALLOCATOR: CountingAllocator = CountingAllocator;
unsafe impl GlobalAlloc for CountingAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let pointer = unsafe { System.alloc(layout) };
        if !pointer.is_null() {
            ALLOCATIONS.fetch_add(1, Ordering::Relaxed);
            LIVE_BYTES.fetch_add(layout.size(), Ordering::Relaxed);
            MAX_ALLOCATION.fetch_max(layout.size(), Ordering::Relaxed);
        }
        pointer
    }
    unsafe fn dealloc(&self, pointer: *mut u8, layout: Layout) {
        LIVE_BYTES.fetch_sub(layout.size(), Ordering::Relaxed);
        unsafe {
            System.dealloc(pointer, layout);
        }
    }
}

fn main() -> Result<(), String> {
    for size in [(1920, 1080), (3840, 2160)] {
        let style = KeyboardOverlayConfig {
            background_rgba: [31, 31, 31, 204],
            text_rgba: [255, 255, 255, 217],
            border_rgba: [66, 66, 66, 100],
            labels: Default::default(),
        };
        let config = PreviewConfig {
            region: (0, 0, size.0, size.1),
            output: size,
            trail: [255, 40, 60, 180],
            click: [40, 180, 255, 160],
            keyboard: Some(style.clone()),
            generation: 1,
        };
        let mut preview = EffectsPreview::new(
            config,
            Some(snow_recording_effects::keyboard_rasterizer::create(&style)?),
        );
        for (index, label) in ["S", "Space", "Enter", "Backspace"].iter().enumerate() {
            let model = &mut preview.keyboard.as_mut().unwrap().model;
            let event = KeyEvent {
                at_ms: index as u64 * 100,
                key: 65 + index as u16,
                down: true,
                label: (*label).into(),
                modifiers: vec![(0xa2, "Ctrl".into()), (0xa0, "Shift".into())],
            };
            model.event(event.clone());
            model.event(KeyEvent {
                at_ms: event.at_ms + 30,
                down: false,
                ..event
            });
        }
        for index in 0..35 {
            preview.observe(
                Some((100 + index * 16, 200 + (index % 7) * 20)),
                300 + index as u64 * 10,
            );
        }
        preview.click(RenderClick {
            timestamp_ms: 600,
            x: 700,
            y: 300,
            button: ObservedMouseButton::Left,
        });
        let mut samples = Vec::new();
        let mut presented = preview.render(700)?;
        let mut max_tiles = 0;
        let mut allocations = 0;
        for iteration in 0..550 {
            if iteration == 50 {
                allocations = ALLOCATIONS.load(Ordering::Relaxed);
                MAX_ALLOCATION.store(0, Ordering::Relaxed);
            }
            std::hint::black_box(&presented);
            let start = Instant::now();
            let next = preview.render(700)?;
            max_tiles = max_tiles.max(next.len());
            // Hold the displayed snapshot across production of its successor, as Qt does.
            presented = std::hint::black_box(next);
            if iteration >= 50 {
                samples.push(start.elapsed().as_secs_f64() * 1000.0);
            }
        }
        std::hint::black_box(&presented);
        samples.sort_by(f64::total_cmp);
        println!(
            "{}x{} combined preview p50={:.3}ms p95={:.3}ms tiles={} tile_bytes={} affected_pixels={} allocations_per_frame={:.1} largest_warmed_allocation={}",
            size.0,
            size.1,
            samples[250],
            samples[475],
            max_tiles,
            preview.allocated_bytes(),
            max_tiles * 128 * 128,
            (ALLOCATIONS.load(Ordering::Relaxed) - allocations) as f64 / 500.0,
            MAX_ALLOCATION.load(Ordering::Relaxed)
        );
        if MAX_ALLOCATION.load(Ordering::Relaxed) > 128 * 128 * 4 {
            return Err("warmed preview allocated more than a single tile".into());
        }
        if samples[475] > 4.0 {
            return Err("combined preview p95 exceeds 4 ms".into());
        }
        drop(presented);
        if !preview.render(3000)?.is_empty() || preview.next_frame_at(3000).is_some() {
            return Err("expired preview continues rendering or scheduling".into());
        }
        // A minute of simulated 60 Hz input, retaining a displayed lease during rendering.
        let mut halfway_bytes = 0;
        let mut displayed = Default::default();
        for index in 0..3600 {
            let now = 4000 + index * 17;
            preview.observe(
                Some((
                    (index * 13 % u64::from(size.0)) as i32,
                    (index * 7 % u64::from(size.1)) as i32,
                )),
                now,
            );
            if index % 15 == 0 {
                let event = KeyEvent {
                    at_ms: now,
                    key: 65,
                    down: true,
                    label: format!("Key {}", index % 16),
                    modifiers: vec![],
                };
                let model = &mut preview.keyboard.as_mut().unwrap().model;
                model.event(event.clone());
                model.event(KeyEvent {
                    down: false,
                    ..event
                });
                preview.click(RenderClick {
                    timestamp_ms: now,
                    x: (index * 13 % u64::from(size.0)) as i32,
                    y: (index * 7 % u64::from(size.1)) as i32,
                    button: ObservedMouseButton::Left,
                });
            }
            std::hint::black_box(&displayed);
            displayed = preview.render(now)?;
            if index == 1799 {
                halfway_bytes = LIVE_BYTES.load(Ordering::Relaxed);
            }
        }
        let retained = LIVE_BYTES.load(Ordering::Relaxed);
        println!(
            "{}x{} sustained Rust bytes halfway={} final={} tile_storage={}",
            size.0,
            size.1,
            halfway_bytes,
            retained,
            preview.allocated_bytes()
        );
        if retained > halfway_bytes + 8 * 1024 * 1024 || retained > 64 * 1024 * 1024 {
            return Err("sustained preview memory is not bounded".into());
        }
        std::hint::black_box(displayed);
    }
    Ok(())
}
