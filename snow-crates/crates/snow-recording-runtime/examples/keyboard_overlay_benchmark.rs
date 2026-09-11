//! Focused Release benchmark and encoded visual fixtures for the production keyboard overlay.
#[allow(dead_code)]
#[path = "../src/keyboard_overlay.rs"]
mod keyboard_overlay;
#[path = "../src/keyboard_rasterizer.rs"]
mod keyboard_rasterizer;

use keyboard_overlay::{KeyEvent, KeyboardOverlay, KeyboardOverlayConfig};
use snow_recording_export::{
    ExportExecutionMode, ExportFormat, SoftwareH264Priority, StreamingEncoder,
    StreamingEncoderConfig, StreamingPixelOrder, VideoCodec,
};
use snow_recording_model::{VideoEncodeConfig, VideoEncodingSpeed};
use std::path::{Path, PathBuf};
use std::time::Instant;

fn style(dark: bool) -> KeyboardOverlayConfig {
    KeyboardOverlayConfig {
        keycap_size: 64,
        background_rgba: if dark {
            [31, 31, 31, 204]
        } else {
            [255, 255, 255, 204]
        },
        text_rgba: if dark {
            [255, 255, 255, 217]
        } else {
            [0, 0, 0, 224]
        },
        border_rgba: if dark {
            [66, 66, 66, 100]
        } else {
            [217, 217, 217, 100]
        },
        labels: Default::default(),
    }
}

fn press(overlay: &mut KeyboardOverlay, at_ms: u64, key: u16, label: &str) {
    overlay.model.event(KeyEvent {
        at_ms,
        key,
        down: true,
        label: label.into(),
        modifiers: vec![(0xa2, "Ctrl".into()), (0xa0, "Shift".into())],
    });
    overlay.model.event(KeyEvent {
        at_ms: at_ms + 80,
        key,
        down: false,
        label: label.into(),
        modifiers: vec![],
    });
}

fn background(size: (u32, u32)) -> Vec<u8> {
    let mut rgba = vec![0; size.0 as usize * size.1 as usize * 4];
    for y in 0..size.1 {
        for x in 0..size.0 {
            let i = (y as usize * size.0 as usize + x as usize) * 4;
            let light = ((x / 100 + y / 100) % 2) as u8 * 25;
            rgba[i..i + 4].copy_from_slice(&[70 + light, 105 + light, 135 + light, 255]);
        }
    }
    rgba
}

fn ppm(path: &Path, size: (u32, u32), rgba: &[u8]) -> Result<(), String> {
    use std::io::Write;
    let mut file = std::io::BufWriter::new(std::fs::File::create(path).map_err(|e| e.to_string())?);
    write!(file, "P6\n{} {}\n255\n", size.0, size.1).map_err(|e| e.to_string())?;
    for pixel in rgba.chunks_exact(4) {
        file.write_all(&pixel[..3]).map_err(|e| e.to_string())?;
    }
    Ok(())
}

fn measure(size: (u32, u32), output: &Path) -> Result<(), String> {
    for dark in [false, true] {
        let config = style(dark);
        let mut overlay = KeyboardOverlay::new(size, keyboard_rasterizer::create(&config)?);
        for (index, label) in ["S", "C", "V", "Space"].iter().enumerate() {
            press(&mut overlay, index as u64 * 100, 65 + index as u16, label);
        }
        let base = background(size);
        let mut pixels = base.clone();
        overlay.draw(&mut pixels, 700)?;
        ppm(
            &output.join(format!(
                "{}-{}.ppm",
                size.1,
                if dark { "dark" } else { "light" }
            )),
            size,
            &pixels,
        )?;
        let mut samples = Vec::with_capacity(500);
        for _ in 0..500 {
            pixels.copy_from_slice(&base);
            let start = Instant::now();
            overlay.draw(&mut pixels, 700)?;
            samples.push(start.elapsed().as_secs_f64() * 1000.0);
        }
        samples.sort_by(f64::total_cmp);
        println!(
            "{}x{} theme={} warmed four-row composition p50={:.3}ms p95={:.3}ms",
            size.0,
            size.1,
            if dark { "dark" } else { "light" },
            samples[250],
            samples[475]
        );
        if samples[475] > 1000.0 / 30.0 * 0.05 {
            return Err("overlay p95 exceeds 5% of a 30fps frame interval".into());
        }
        overlay.draw(&mut pixels, 2100)?;
        if overlay.model.needs_frame(2100) {
            return Err("expired overlay continues scheduling frames".into());
        }
    }
    Ok(())
}

fn encode_fixture(output: &Path, format: ExportFormat) -> Result<(), String> {
    let size = (640, 360);
    let config = style(true);
    let mut overlay = KeyboardOverlay::new(size, keyboard_rasterizer::create(&config)?);
    let mut encoder = StreamingEncoder::create(StreamingEncoderConfig {
        output_path: output.join(format!("keyboard.{}", format.file_extension())),
        format,
        width: size.0,
        height: size.1,
        fps: 30,
        codec: VideoCodec::H264,
        prefer_hardware_h264: false,
        execution_mode: ExportExecutionMode::SoftwareOnly,
        software_h264_priority: SoftwareH264Priority::X264First,
        video: VideoEncodeConfig {
            quality: 80,
            speed: VideoEncodingSpeed::VeryFast,
        },
        encode_threads: 2,
        pixel_order: StreamingPixelOrder::Rgba,
        audio: None,
    })
    .map_err(|e| e.to_string())?;
    let base = background(size);
    for index in 0..90u64 {
        let at = index * 1000 / 30;
        if index == 3 {
            press(&mut overlay, at, 83, "S");
        }
        if index == 12 {
            press(&mut overlay, at, 67, "C");
        }
        if index == 21 {
            press(&mut overlay, at, 86, "V");
        }
        let mut pixels = base.clone();
        overlay.draw(&mut pixels, at)?;
        encoder
            .push_rgba_frame(at, &pixels)
            .map_err(|e| e.to_string())?;
    }
    let report = encoder.finish().map_err(|e| e.to_string())?;
    println!("{} fixture encoded: {report:?}", format.file_extension());
    if format == ExportFormat::Mp4 {
        verify_fixture(&output.join("keyboard.mp4"), &base, size)?;
    }
    Ok(())
}

fn verify_fixture(path: &Path, base: &[u8], size: (u32, u32)) -> Result<(), String> {
    use ffmpeg_next::{codec, format, media, software, util};
    let verify = || -> Result<(), ffmpeg_next::Error> {
        let mut input = format::input(path)?;
        let stream = input
            .streams()
            .best(media::Type::Video)
            .ok_or(ffmpeg_next::Error::StreamNotFound)?;
        let index = stream.index();
        let mut decoder = codec::context::Context::from_parameters(stream.parameters())?
            .decoder()
            .video()?;
        let mut scaler = software::scaling::Context::get(
            decoder.format(),
            decoder.width(),
            decoder.height(),
            format::Pixel::RGBA,
            size.0,
            size.1,
            software::scaling::flag::Flags::BILINEAR,
        )?;
        let mut decoded = util::frame::video::Video::empty();
        let mut first_error = None;
        let mut largest_error = 0.0f64;
        let mut last_error = 0.0;
        let mut count = 0;
        let mut drain = |decoder: &mut codec::decoder::Video| -> Result<(), ffmpeg_next::Error> {
            while decoder.receive_frame(&mut decoded).is_ok() {
                let mut rgba = util::frame::video::Video::empty();
                scaler.run(&decoded, &mut rgba)?;
                let mut error = 0u64;
                // Only the bottom-right overlay area; codec noise in the rest is irrelevant.
                for y in size.1 - 150..size.1 {
                    for x in size.0 - 250..size.0 {
                        for channel in 0..3 {
                            let actual = rgba.data(0)
                                [y as usize * rgba.stride(0) + x as usize * 4 + channel];
                            let expected =
                                base[(y as usize * size.0 as usize + x as usize) * 4 + channel];
                            error += u64::from(actual.abs_diff(expected));
                        }
                    }
                }
                let mean = error as f64 / (150.0 * 250.0 * 3.0);
                first_error.get_or_insert(mean);
                largest_error = largest_error.max(mean);
                last_error = mean;
                count += 1;
            }
            Ok(())
        };
        for (stream, packet) in input.packets() {
            if stream.index() == index {
                decoder.send_packet(&packet)?;
                drain(&mut decoder)?;
            }
        }
        decoder.send_eof()?;
        drain(&mut decoder)?;
        assert!(count > 10, "fixture should contain animation frames");
        assert!(
            largest_error > first_error.unwrap_or_default() + 2.0,
            "encoded keys must be visible"
        );
        assert!(
            last_error < first_error.unwrap_or_default() + 1.5,
            "final encoded frame must clear the keys"
        );
        println!(
            "decoded {}: frames={count}, initial error={:.2}, key frame error={largest_error:.2}, final error={last_error:.2}",
            path.display(),
            first_error.unwrap_or_default()
        );
        Ok(())
    };
    verify().map_err(|e| e.to_string())
}

fn main() -> Result<(), String> {
    if cfg!(debug_assertions) {
        return Err("use windows-msvc-performance (Release)".into());
    }
    let output = std::env::args()
        .nth(1)
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("keyboard-overlay-qa"));
    std::fs::create_dir_all(&output).map_err(|e| e.to_string())?;
    measure((1920, 1080), &output)?;
    measure((3840, 2160), &output)?;
    for format in [
        ExportFormat::Mp4,
        ExportFormat::Gif,
        ExportFormat::Apng,
        ExportFormat::Webp,
    ] {
        encode_fixture(&output, format)?;
    }
    Ok(())
}
