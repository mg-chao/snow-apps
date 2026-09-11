//! Compare compositor-style buffer submission with identical input and codec settings.
use std::{fs, path::PathBuf, time::Instant};

use anyhow::{Result, bail};
use ffmpeg_next as ffmpeg;
use snow_recording_export::{
    ExportExecutionMode, ExportFormat, SoftwareH264Priority, StreamingEncoder,
    StreamingEncoderConfig, VideoCodec,
};
use snow_recording_model::{VideoEncodeConfig, VideoEncodingSpeed};

fn verify_video(path: &std::path::Path, expected: u64, dimensions: (u32, u32)) -> Result<()> {
    let mut input = ffmpeg::format::input(path)?;
    let stream = input
        .streams()
        .best(ffmpeg::media::Type::Video)
        .ok_or_else(|| anyhow::anyhow!("missing video stream"))?;
    let stream_index = stream.index();
    let mut decoder = ffmpeg::codec::context::Context::from_parameters(stream.parameters())?
        .decoder()
        .video()?;
    if (decoder.width(), decoder.height()) != dimensions {
        bail!("decoded dimensions differ");
    }
    let mut frame = ffmpeg::frame::Video::empty();
    let mut count = 0;
    let mut drain = |decoder: &mut ffmpeg::decoder::Video| -> Result<()> {
        loop {
            match decoder.receive_frame(&mut frame) {
                Ok(()) => count += 1,
                Err(ffmpeg::Error::Eof) => return Ok(()),
                Err(ffmpeg::Error::Other { errno }) if errno == ffmpeg::error::EAGAIN => {
                    return Ok(());
                }
                Err(error) => return Err(error.into()),
            }
        }
    };
    for (stream, packet) in input.packets() {
        if stream.index() == stream_index {
            decoder.send_packet(&packet)?;
            drain(&mut decoder)?;
        }
    }
    decoder.send_eof()?;
    drain(&mut decoder)?;
    if count != expected {
        bail!("decoded {count} frames, expected {expected}");
    }
    Ok(())
}

fn main() -> Result<()> {
    if cfg!(debug_assertions) {
        bail!("benchmark requires Release");
    }
    let args: Vec<_> = std::env::args().skip(1).collect();
    let directory = PathBuf::from(
        args.first()
            .map(String::as_str)
            .unwrap_or("target/perf/streaming-copy"),
    );
    let samples: usize = args
        .get(1)
        .map(|value| value.parse())
        .transpose()?
        .unwrap_or(5);
    let frames: u64 = args
        .get(2)
        .map(|value| value.parse())
        .transpose()?
        .unwrap_or(60);
    let hardware = args.get(3).is_some_and(|value| value == "--hardware");
    if samples == 0 || frames < 2 {
        bail!("samples must be positive and frames >= 2");
    }
    fs::create_dir_all(&directory)?;
    let mut csv = String::from(
        "width,height,mode,sample,frames,submit_ms,finish_ms,total_ms,encoder,output_bytes\n",
    );
    for (width, height) in [(1920u32, 1080u32), (3840, 2160)] {
        // Several changing screen-like patterns, prepared outside the timed section.
        let sources: Vec<Vec<u8>> = (0..4)
            .map(|phase| {
                let mut rgba = vec![0; width as usize * height as usize * 4];
                for (index, pixel) in rgba.chunks_exact_mut(4).enumerate() {
                    let x = index as u32 % width;
                    let y = index as u32 / width;
                    let value = if (x / 64 + y / 32 + phase).is_multiple_of(7) {
                        180
                    } else {
                        40
                    };
                    pixel.copy_from_slice(&[value, value, value, 255]);
                }
                rgba
            })
            .collect();
        for sample in 0..=samples {
            // Alternate execution order and discard one complete warmup pair.
            for offset in 0..2 {
                let owned = (sample + offset).is_multiple_of(2);
                let mode = if owned {
                    "owned-recycle"
                } else {
                    "borrowed-allocate"
                };
                let path = directory.join(format!("{width}-{mode}-{sample}.mp4"));
                let mut encoder = StreamingEncoder::create(StreamingEncoderConfig {
                    output_path: path.clone(),
                    format: ExportFormat::Mp4,
                    width,
                    height,
                    fps: 60,
                    codec: VideoCodec::H264,
                    prefer_hardware_h264: hardware,
                    execution_mode: if hardware {
                        ExportExecutionMode::HardwarePreferred
                    } else {
                        ExportExecutionMode::SoftwareOnly
                    },
                    software_h264_priority: SoftwareH264Priority::X264First,
                    video: VideoEncodeConfig {
                        quality: 80,
                        speed: VideoEncodingSpeed::VeryFast,
                    },
                    encode_threads: 0,
                    audio: None,
                })?;
                let mut spare = Vec::new();
                let start = Instant::now();
                for index in 0..frames {
                    let source = &sources[index as usize % sources.len()];
                    if owned {
                        spare.resize(source.len(), 0);
                        spare.copy_from_slice(source);
                        spare = encoder.push_owned_rgba_frame(index * 1000 / 60, spare)?;
                    } else {
                        let composed = source.to_vec();
                        encoder.push_rgba_frame(index * 1000 / 60, &composed)?;
                    }
                }
                let submit_ms = start.elapsed().as_secs_f64() * 1000.0;
                let finish = Instant::now();
                let report = encoder.finish()?;
                let finish_ms = finish.elapsed().as_secs_f64() * 1000.0;
                if report.encoded_frames != frames || report.coalesced_frames != 0 {
                    bail!("frame loss in {mode}: {report:?}");
                }
                verify_video(&path, frames, (width, height))?;
                if sample > 0 {
                    let row = format!(
                        "{width},{height},{mode},{sample},{frames},{submit_ms:.3},{finish_ms:.3},{:.3},{},{}\n",
                        submit_ms + finish_ms,
                        report.video_encoder,
                        fs::metadata(&path)?.len()
                    );
                    print!("{row}");
                    csv.push_str(&row);
                }
                fs::remove_file(path)?;
            }
        }
    }
    fs::write(directory.join("streaming-copy.csv"), csv)?;
    Ok(())
}
