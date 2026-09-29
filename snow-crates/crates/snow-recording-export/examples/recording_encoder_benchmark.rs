//! Deterministic encoder capacity and decoded-quality experiment.
//! Arguments: output.mp4 clip-seconds threads hardware(0/1) codec(h264/h265) quality(0..100) scene(ui/gradient).
//! Release only.
//! Encodes a fixed frame count so file size and quality are comparable across runs.
use ffmpeg_next as ffmpeg;
use snow_recording_export::{
    ExportExecutionMode, ExportFormat, SoftwareH264Priority, StreamingEncoder,
    StreamingEncoderConfig, VideoCodec,
};
use snow_recording_model::{VideoEncodeConfig, VideoEncodingSpeed};
use std::{path::Path, time::Instant};

#[derive(Clone, Copy, Debug)]
enum Scene {
    Ui,
    Gradient,
}

fn source(rgba: &mut [u8], index: u64, scene: Scene) {
    match scene {
        Scene::Ui => {
            for (y, row) in rgba.chunks_exact_mut(1920 * 4).enumerate() {
                for (x, pixel) in row.chunks_exact_mut(4).enumerate() {
                    let tile = ((x / 48 + y / 48) % 2) as u8 * 24;
                    let moving = ((x as u64 + index * 13) % 1920) < 180 && (200..700).contains(&y);
                    let color = if moving {
                        [220, 110, 40, 255]
                    } else {
                        [tile + 40, tile + 70, tile + 100, 255]
                    };
                    pixel.copy_from_slice(&color);
                }
            }
        }
        Scene::Gradient => {
            for (y, row) in rgba.chunks_exact_mut(1920 * 4).enumerate() {
                for (x, pixel) in row.chunks_exact_mut(4).enumerate() {
                    let color = [
                        ((x + y + index as usize * 3) % 256) as u8,
                        ((2 * x + y / 2 + index as usize * 5) % 256) as u8,
                        ((x / 2 + 3 * y + index as usize * 2) % 256) as u8,
                        255,
                    ];
                    pixel.copy_from_slice(&color);
                }
            }
        }
    }
}

fn quality(path: &Path, scene: Scene) -> Result<(usize, f64), Box<dyn std::error::Error>> {
    let mut input = ffmpeg::format::input(path)?;
    let stream = input
        .streams()
        .best(ffmpeg::media::Type::Video)
        .ok_or("no video")?;
    let stream_index = stream.index();
    let mut decoder = ffmpeg::codec::context::Context::from_parameters(stream.parameters())?
        .decoder()
        .video()?;
    let mut scaler = ffmpeg::software::scaling::Context::get(
        decoder.format(),
        1920,
        1080,
        ffmpeg::format::Pixel::RGBA,
        1920,
        1080,
        ffmpeg::software::scaling::flag::Flags::BILINEAR,
    )?;
    let mut count = 0usize;
    let mut compared = 0u64;
    let mut error = 0u64;
    let mut expected = vec![0; 1920 * 1080 * 4];
    let mut frame = ffmpeg::frame::Video::empty();
    let mut rgba = ffmpeg::frame::Video::empty();
    let mut receive = |decoder: &mut ffmpeg::decoder::Video| -> Result<(), ffmpeg::Error> {
        while decoder.receive_frame(&mut frame).is_ok() {
            if count.is_multiple_of(15) {
                scaler.run(&frame, &mut rgba)?;
                source(&mut expected, count as u64, scene);
                for y in 0..1080 {
                    let row = &rgba.data(0)[y * rgba.stride(0)..y * rgba.stride(0) + 1920 * 4];
                    for x in 0..1920 {
                        for channel in 0..3 {
                            let delta = i64::from(row[x * 4 + channel])
                                - i64::from(expected[(y * 1920 + x) * 4 + channel]);
                            error += (delta * delta) as u64;
                            compared += 1;
                        }
                    }
                }
            }
            count += 1;
        }
        Ok(())
    };
    for (stream, packet) in input.packets() {
        if stream.index() == stream_index {
            decoder.send_packet(&packet)?;
            receive(&mut decoder)?;
        }
    }
    decoder.send_eof()?;
    receive(&mut decoder)?;
    Ok((
        count,
        10.0 * (255.0 * 255.0 / (error as f64 / compared as f64)).log10(),
    ))
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    if cfg!(debug_assertions) {
        return Err("use the performance Release build".into());
    }
    let args: Vec<_> = std::env::args().collect();
    let path = std::path::PathBuf::from(args.get(1).ok_or("output path required")?);
    let seconds: u64 = args.get(2).map(String::as_str).unwrap_or("30").parse()?;
    let threads: u8 = args.get(3).map(String::as_str).unwrap_or("0").parse()?;
    let hardware = args.get(4).is_some_and(|v| v == "1");
    let codec = match args.get(5).map(String::as_str).unwrap_or("h264") {
        "h264" => VideoCodec::H264,
        "h265" => VideoCodec::H265,
        _ => return Err("codec must be h264 or h265".into()),
    };
    let requested_quality: u8 = args.get(6).map(String::as_str).unwrap_or("80").parse()?;
    if requested_quality > 100 {
        return Err("quality must be in 0..=100".into());
    }
    let scene = match args.get(7).map(String::as_str).unwrap_or("ui") {
        "ui" => Scene::Ui,
        "gradient" => Scene::Gradient,
        _ => return Err("scene must be ui or gradient".into()),
    };
    let setup = Instant::now();
    let mut encoder = StreamingEncoder::create(StreamingEncoderConfig {
        loop_animated_images: true,
        output_path: path.clone(),
        format: ExportFormat::Mp4,
        width: 1920,
        height: 1080,
        fps: 30,
        codec,
        prefer_hardware_h264: hardware,
        execution_mode: if hardware {
            ExportExecutionMode::HardwarePreferred
        } else {
            ExportExecutionMode::SoftwareOnly
        },
        software_h264_priority: SoftwareH264Priority::X264First,
        video: VideoEncodeConfig {
            quality: requested_quality,
            speed: VideoEncodingSpeed::VeryFast,
        },
        encode_threads: threads,
        audio: Vec::new(),
    })?;
    let setup_ms = setup.elapsed().as_secs_f64() * 1000.0;
    let mut pixels = vec![0; 1920 * 1080 * 4];
    let mut samples = Vec::new();
    let started = Instant::now();
    let mut index = 0;
    while index < seconds.max(1) * 30 {
        pixels.resize(1920 * 1080 * 4, 0);
        source(&mut pixels, index, scene);
        let at = Instant::now();
        pixels = encoder.push_owned_rgba_frame_at_pts(index, pixels)?;
        samples.push(at.elapsed().as_secs_f64() * 1000.0);
        index += 1;
    }
    let measured = started.elapsed().as_secs_f64();
    let stop = Instant::now();
    let report = encoder.finish_at_pts(index)?;
    let finish_ms = stop.elapsed().as_secs_f64() * 1000.0;
    std::fs::write(
        path.with_extension("samples.csv"),
        samples
            .iter()
            .enumerate()
            .map(|(i, v)| format!("{i},{v}\n"))
            .collect::<String>(),
    )?;
    samples.sort_by(f64::total_cmp);
    let decoded_quality = ffmpeg::decoder::find(match codec {
        VideoCodec::H264 => ffmpeg::codec::Id::H264,
        VideoCodec::H265 => ffmpeg::codec::Id::HEVC,
    })
    .map(|_| quality(&path, scene))
    .transpose()?;
    let packet_count = ffmpeg::format::input(&path)?.packets().count();
    assert_eq!(packet_count as u64, index);
    let (decoded, psnr) = decoded_quality
        .map(|(count, psnr)| (count.to_string(), psnr.to_string()))
        .unwrap_or_else(|| ("unavailable".into(), "unavailable".into()));
    let bytes = std::fs::metadata(&path)?.len();
    let csv = format!(
        "scene,codec,quality,threads,hardware_requested,hardware_used,encoder,frames,decoded,seconds,fps,setup_ms,finish_ms,p50_ms,p95_ms,psnr_db,bytes\n{scene:?},{codec:?},{requested_quality},{threads},{hardware},{},{},{index},{decoded},{measured},{},{setup_ms},{finish_ms},{},{},{psnr},{bytes}\n",
        report.used_hardware_video_encoder,
        report.video_encoder,
        index as f64 / measured,
        samples[samples.len() / 2],
        samples[samples.len() * 95 / 100]
    );
    std::fs::write(path.with_extension("csv"), &csv)?;
    println!("{csv}");
    if let Some((decoded_count, _)) = decoded_quality {
        assert_eq!(decoded_count as u64, index);
    }
    Ok(())
}
