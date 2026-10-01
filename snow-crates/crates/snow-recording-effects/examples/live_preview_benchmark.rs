//! Unchanged live-adapter harness for controlled HEAD/candidate Release comparisons.
use std::collections::BTreeMap;
use std::time::Instant;

use snow_recording_effects::keyboard_overlay::{KeyEvent, KeyboardOverlayConfig};
use snow_recording_effects::mouse_effects::RenderClick;
use snow_recording_effects::mouse_hook::ObservedMouseButton;
use snow_recording_effects::preview::{EffectsPreview, PreviewConfig};

const WARMUP: u64 = 120;
const SAMPLES: u64 = 900;

fn percentile(samples: &mut [f64], p: f64) -> f64 {
    samples.sort_by(f64::total_cmp);
    samples[((samples.len() - 1) as f64 * p).round() as usize]
}

fn run(size: (u32, u32), moving: bool, enabled: bool) -> Result<(), String> {
    let style = KeyboardOverlayConfig {
        font: None,
        keycap_size: 64,
        background_rgba: [31, 31, 31, 204],
        text_rgba: [255, 255, 255, 217],
        border_rgba: [66, 66, 66, 100],
        labels: BTreeMap::new(),
    };
    let config = PreviewConfig {
        region: (0, 0, size.0, size.1),
        canvas: size,
        output: size,
        trail: if enabled { [255, 40, 60, 180] } else { [0; 4] },
        trail_duration_ms: 500,
        click: if enabled { [40, 180, 255, 160] } else { [0; 4] },
        highlight: [0; 4],
        record_mouse_clicks: false,
        show_keyboard: enabled,
        keyboard: enabled.then_some(style.clone()),
        generation: 1,
    };
    let rasterizer = if enabled {
        Some(snow_recording_effects::keyboard_rasterizer::create(&style)?)
    } else {
        None
    };
    let mut effects = EffectsPreview::new(config, rasterizer);
    let mut samples = Vec::with_capacity(SAMPLES as usize);
    let mut fingerprint = 0u64;
    let mut tile_count = 0usize;
    for index in 0..WARMUP + SAMPLES {
        let at = index * 1000 / 60;
        let position = if moving {
            (
                (100 + index * 11 % u64::from(size.0 - 200)) as i32,
                (100 + index * 7 % u64::from(size.1 - 200)) as i32,
            )
        } else {
            (500, 400)
        };
        let start = Instant::now();
        effects.observe(Some(position), at);
        if enabled && index.is_multiple_of(15) {
            effects.click(RenderClick {
                timestamp_ms: at,
                x: position.0,
                y: position.1,
                button: ObservedMouseButton::Left,
            });
            for down in [true, false] {
                effects.key_event(KeyEvent {
                    at_ms: at,
                    key: 65,
                    down,
                    label: "A".into(),
                    modifiers: vec![(0x11, "Ctrl".into())],
                });
            }
        }
        let layers = effects.render(at)?;
        let elapsed = start.elapsed().as_secs_f64() * 1000.0;
        if index >= WARMUP {
            samples.push(elapsed);
            tile_count += layers.len();
        }
        if index.is_multiple_of(30) {
            for tile in layers.iter() {
                fingerprint = fingerprint.wrapping_mul(1099511628211) ^ u64::from(tile.x);
                fingerprint = fingerprint.wrapping_mul(1099511628211) ^ u64::from(tile.y);
                for byte in tile.pixels.iter() {
                    fingerprint = fingerprint.wrapping_mul(1099511628211) ^ u64::from(*byte);
                }
            }
        }
        std::hint::black_box(&layers);
    }
    println!(
        "{}x{},{},{},{:.4},{:.4},{:.2},{fingerprint:016x}",
        size.0,
        size.1,
        if moving { "moving" } else { "static" },
        if enabled { "effects" } else { "none" },
        percentile(&mut samples, 0.5),
        percentile(&mut samples, 0.95),
        tile_count as f64 / SAMPLES as f64,
    );
    Ok(())
}

fn main() -> Result<(), String> {
    if cfg!(debug_assertions) {
        return Err("live-preview benchmarks require Release".into());
    }
    println!("resolution,source,case,compose_p50_ms,compose_p95_ms,tiles_per_frame,fingerprint");
    for size in [(1920, 1080), (3840, 2160)] {
        for moving in [false, true] {
            for enabled in [false, true] {
                run(size, moving, enabled)?;
            }
        }
    }
    Ok(())
}
