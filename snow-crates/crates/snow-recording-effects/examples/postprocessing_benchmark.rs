//! Deterministic composition microbenchmark; build with windows-msvc-performance, Release only.
//! Decode, encode, native input hooks and media I/O are deliberately outside its timing stages.
use std::alloc::{GlobalAlloc, Layout, System};
use std::collections::BTreeMap;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::time::Instant;

use snow_recording_effects::keyboard_overlay::{KeyEvent, KeyboardOverlayConfig};
use snow_recording_effects::mouse_effects::RenderClick;
use snow_recording_effects::mouse_hook::ObservedMouseButton;
use snow_recording_effects::preview::{EffectsPreview, PreviewConfig};
use snow_recording_effects::{RecordedEffects, surface::TILE_SIZE};
use snow_recording_model::{
    EffectsConfig, FinalizedTimeline, InputMouseButton, PlaybackOverlay, RecordedInput,
    RecordedInputEvent, RecordedMouseClick, RenderConfig,
};

struct Allocator;
static LIVE_BYTES: AtomicUsize = AtomicUsize::new(0);
static PEAK_BYTES: AtomicUsize = AtomicUsize::new(0);
static ALLOCATIONS: AtomicUsize = AtomicUsize::new(0);
#[global_allocator]
static ALLOCATOR: Allocator = Allocator;
unsafe impl GlobalAlloc for Allocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let pointer = unsafe { System.alloc(layout) };
        if !pointer.is_null() {
            ALLOCATIONS.fetch_add(1, Ordering::Relaxed);
            let live = LIVE_BYTES.fetch_add(layout.size(), Ordering::Relaxed) + layout.size();
            PEAK_BYTES.fetch_max(live, Ordering::Relaxed);
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

const WARMUP: u64 = 60;
const SAMPLES: u64 = 360;

fn style() -> KeyboardOverlayConfig {
    KeyboardOverlayConfig {
        font: None,
        keycap_size: 64,
        background_rgba: [31, 31, 31, 204],
        text_rgba: [255, 255, 255, 217],
        border_rgba: [66, 66, 66, 100],
        labels: BTreeMap::new(),
    }
}

fn policy(size: (u32, u32), combined: bool, overlay: PlaybackOverlay) -> RenderConfig {
    RenderConfig {
        output_width: size.0,
        output_height: size.1,
        output_fps: 60,
        effects: EffectsConfig {
            show_cursor: false,
            show_keyboard: combined,
            keyboard: Some(style()),
            mouse_trail_rgba: if combined { [255, 40, 60, 180] } else { [0; 4] },
            mouse_click_rgba: if combined {
                [40, 180, 255, 160]
            } else {
                [0; 4]
            },
            ..EffectsConfig::default()
        },
        playback_overlay: overlay,
    }
}

fn key(at: u64, down: bool) -> KeyEvent {
    KeyEvent {
        at_ms: at,
        key: 65,
        down,
        label: "A".into(),
        modifiers: vec![(0x11, "Ctrl".into())],
    }
}

fn observe(
    effects: &mut RecordedEffects,
    sequence: &mut u64,
    at: u64,
    event: RecordedInput,
) -> Result<(), String> {
    effects.observe(&RecordedInputEvent {
        timestamp_ms: at,
        sequence: *sequence,
        event,
    })?;
    *sequence += 1;
    Ok(())
}

fn input(
    effects: &mut RecordedEffects,
    sequence: &mut u64,
    index: u64,
    at: u64,
    position: (i32, i32),
    combined: bool,
) -> Result<(), String> {
    observe(
        effects,
        sequence,
        at,
        RecordedInput::Pointer {
            position: Some(position),
            continuity: 0,
            at_ms: at,
        },
    )?;
    if combined && index.is_multiple_of(15) {
        observe(
            effects,
            sequence,
            at,
            RecordedInput::Click(RecordedMouseClick {
                x: position.0,
                y: position.1,
                button: InputMouseButton::Left,
                down: true,
                key: KeyEvent {
                    key: 0x200,
                    label: "Left click".into(),
                    ..key(at, true)
                },
            }),
        )?;
        observe(effects, sequence, at, RecordedInput::Key(key(at, true)))?;
        observe(effects, sequence, at, RecordedInput::Key(key(at, false)))?;
    }
    Ok(())
}

fn point(index: u64, size: (u32, u32), moving: bool) -> (i32, i32) {
    if moving {
        (
            (100 + index * 9 % u64::from(size.0 - 200)) as i32,
            (100 + index * 7 % u64::from(size.1 - 200)) as i32,
        )
    } else {
        (500, 400)
    }
}

fn percentile(samples: &mut [f64], p: f64) -> f64 {
    samples.sort_by(f64::total_cmp);
    samples[((samples.len() - 1) as f64 * p).round() as usize]
}

fn run(
    size: (u32, u32),
    moving: bool,
    name: &str,
    combined: bool,
    overlay: PlaybackOverlay,
) -> Result<(), String> {
    let before = LIVE_BYTES.load(Ordering::Relaxed);
    let timeline = FinalizedTimeline::new((WARMUP + SAMPLES) * 1000 / 60, 60)?;
    let mut effects =
        RecordedEffects::new(policy(size, combined, overlay), size, None)?.with_timeline(timeline);
    let mut source = [40, 70, 100, 255].repeat(size.0 as usize * size.1 as usize);
    let mut output = vec![0; source.len()];
    let mut prepare = Vec::with_capacity(SAMPLES as usize);
    let mut compose = Vec::with_capacity(SAMPLES as usize);
    let mut sequence = 0;
    let mut allocation_start = 0;
    for index in 0..WARMUP + SAMPLES {
        let frame = timeline.frame(index).ok_or("benchmark frame missing")?;
        if index == WARMUP {
            allocation_start = ALLOCATIONS.load(Ordering::Relaxed);
            PEAK_BYTES.store(LIVE_BYTES.load(Ordering::Relaxed), Ordering::Relaxed);
        }
        if moving {
            let offset = ((index * 7919) as usize % (size.0 as usize * size.1 as usize)) * 4;
            source[offset] = index as u8;
        }
        let start = Instant::now();
        output.copy_from_slice(&source);
        let prepared = start.elapsed().as_secs_f64() * 1000.0;
        let start = Instant::now();
        input(
            &mut effects,
            &mut sequence,
            index,
            frame.timestamp_ms,
            point(index, size, moving),
            combined,
        )?;
        effects.apply_rgba(&mut output, frame)?;
        let composed = start.elapsed().as_secs_f64() * 1000.0;
        std::hint::black_box(&output);
        if index >= WARMUP {
            prepare.push(prepared);
            compose.push(composed);
        }
    }
    let peak = PEAK_BYTES.load(Ordering::Relaxed).saturating_sub(before);
    let live = LIVE_BYTES.load(Ordering::Relaxed).saturating_sub(before);
    let allocations = ALLOCATIONS.load(Ordering::Relaxed) - allocation_start;
    println!(
        "{}x{},{},{},{:.4},{:.4},{:.4},{},{},{:.2}",
        size.0,
        size.1,
        if moving { "moving" } else { "static" },
        name,
        percentile(&mut prepare, 0.5),
        percentile(&mut compose, 0.5),
        percentile(&mut compose, 0.95),
        live,
        peak,
        allocations as f64 / SAMPLES as f64
    );
    Ok(())
}

fn compare_sparse(size: (u32, u32)) -> Result<(), String> {
    let style = style();
    let preview_config = PreviewConfig {
        region: (0, 0, size.0, size.1),
        canvas: size,
        output: size,
        trail: [255, 40, 60, 180],
        trail_duration_ms: 500,
        click: [40, 180, 255, 160],
        highlight: [0; 4],
        record_mouse_clicks: false,
        show_keyboard: true,
        keyboard: Some(style.clone()),
        generation: 1,
    };
    let mut preview = EffectsPreview::new(
        preview_config,
        Some(snow_recording_effects::keyboard_rasterizer::create(&style)?),
    );
    let mut recorded = RecordedEffects::new(policy(size, true, PlaybackOverlay::None), size, None)?;
    let timeline = FinalizedTimeline::new((WARMUP + SAMPLES) * 1000 / 60, 60)?;
    let mut live_samples = Vec::with_capacity(SAMPLES as usize);
    let mut replay_samples = Vec::with_capacity(SAMPLES as usize);
    let mut sequence = 0;
    let mut mismatches = 0usize;
    for index in 0..WARMUP + SAMPLES {
        let frame = timeline
            .frame(index)
            .ok_or("benchmark sparse frame missing")?;
        let position = point(index, size, true);
        preview.observe(Some(position), frame.timestamp_ms);
        if index.is_multiple_of(15) {
            preview.click(RenderClick {
                timestamp_ms: frame.timestamp_ms,
                x: position.0,
                y: position.1,
                button: ObservedMouseButton::Left,
            });
            preview.key_event(key(frame.timestamp_ms, true));
            preview.key_event(key(frame.timestamp_ms, false));
        }
        input(
            &mut recorded,
            &mut sequence,
            index,
            frame.timestamp_ms,
            position,
            true,
        )?;
        let start = Instant::now();
        let live = preview.render(frame.timestamp_ms)?;
        let live_time = start.elapsed().as_secs_f64() * 1000.0;
        let start = Instant::now();
        let replay = recorded.render_tiles(frame)?;
        let replay_time = start.elapsed().as_secs_f64() * 1000.0;
        if index >= WARMUP {
            live_samples.push(live_time);
            replay_samples.push(replay_time);
        }
        // Mouse and keyboard retain different preview surfaces. This fixture keeps
        // their pixel rectangles apart, making a tile-pixel equality comparison exact.
        for tile in &replay {
            let mut expected = vec![0u8; (TILE_SIZE * TILE_SIZE * 4) as usize];
            for prior in live
                .iter()
                .filter(|prior| prior.x == tile.x && prior.y == tile.y)
            {
                for (destination, source) in expected
                    .chunks_exact_mut(4)
                    .zip(prior.pixels.chunks_exact(4))
                {
                    let inverse = 255 - u32::from(source[3]);
                    for channel in 0..4 {
                        destination[channel] = (u32::from(source[channel])
                            + (u32::from(destination[channel]) * inverse + 127) / 255)
                            .min(255) as u8;
                    }
                }
            }
            mismatches += expected
                .iter()
                .zip(tile.pixels.iter())
                .filter(|(a, b)| a.abs_diff(**b) > 1)
                .count();
        }
        std::hint::black_box((&live, &replay));
    }
    println!(
        "sparse,{}x{},live_p50_ms={:.4},live_p95_ms={:.4},replay_p50_ms={:.4},replay_p95_ms={:.4},mismatched_channels={}",
        size.0,
        size.1,
        percentile(&mut live_samples, 0.5),
        percentile(&mut live_samples, 0.95),
        percentile(&mut replay_samples, 0.5),
        percentile(&mut replay_samples, 0.95),
        mismatches
    );
    if mismatches != 0 {
        return Err("recorded effects differ from identical live sparse observations".into());
    }
    Ok(())
}

fn main() -> Result<(), String> {
    if cfg!(debug_assertions) {
        return Err("post-processing benchmarks require Release".into());
    }
    println!(
        "resolution,source,case,prepare_p50_ms,compose_p50_ms,compose_p95_ms,rust_retained_bytes,rust_peak_bytes,allocations_per_frame"
    );
    for size in [(1920, 1080), (3840, 2160)] {
        for moving in [false, true] {
            run(size, moving, "none", false, PlaybackOverlay::None)?;
            run(
                size,
                moving,
                "bar",
                false,
                PlaybackOverlay::ProgressBar {
                    rgba: [22, 119, 255, 255],
                },
            )?;
            run(
                size,
                moving,
                "time",
                false,
                PlaybackOverlay::PlaybackTime { rgba: [255; 4] },
            )?;
            run(
                size,
                moving,
                "combined-bar",
                true,
                PlaybackOverlay::ProgressBar {
                    rgba: [22, 119, 255, 255],
                },
            )?;
            run(
                size,
                moving,
                "combined-time",
                true,
                PlaybackOverlay::PlaybackTime { rgba: [255; 4] },
            )?;
        }
        compare_sparse(size)?;
    }
    Ok(())
}
