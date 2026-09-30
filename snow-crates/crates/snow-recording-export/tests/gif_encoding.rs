use std::path::Path;
use std::time::{Duration, Instant};

use ffmpeg_next as ffmpeg;
use snow_recording_export::{
    ExportExecutionMode, ExportFormat, SoftwareH264Priority, StreamingEncoder,
    StreamingEncoderConfig, VideoCodec,
};
use snow_recording_model::VideoEncodeConfig;

fn config(path: &Path, width: u32, height: u32) -> StreamingEncoderConfig {
    StreamingEncoderConfig {
        output_path: path.into(),
        format: ExportFormat::Gif,
        width,
        height,
        fps: 20,
        loop_animated_images: true,
        codec: VideoCodec::H264,
        prefer_hardware_h264: false,
        execution_mode: ExportExecutionMode::SoftwareOnly,
        software_h264_priority: SoftwareH264Priority::X264First,
        video: VideoEncodeConfig::default(),
        encode_threads: 1,
        audio: Vec::new(),
    }
}

#[derive(Clone, Copy, Debug)]
enum Scene {
    Ui,
    Gradient,
    ScrollingGradient,
}

fn frame(width: u32, height: u32, index: usize, scene: Scene) -> Vec<u8> {
    let mut rgba = Vec::with_capacity(width as usize * height as usize * 4);
    for y in 0..height {
        for x in 0..width {
            let (gx, gy) = if matches!(scene, Scene::ScrollingGradient) {
                (
                    (x + index as u32 * 7) % width,
                    (y + index as u32 * 3) % height,
                )
            } else {
                (x, y)
            };
            let color = if x.abs_diff((index as u32 * 7) % width) < width / 16
                && y > height / 3
                && y < height * 2 / 3
            {
                [215, 139, 71]
            } else if !matches!(scene, Scene::Ui) {
                [
                    (gx * 220 / width + 17) as u8,
                    (gy * 190 / height + 23) as u8,
                    ((gx + gy) * 180 / (width + height) + 31) as u8,
                ]
            } else if y % 27 < 3 && x > width / 6 {
                [181, 197, 211]
            } else if x < width / 6 {
                [37, 49, 61]
            } else {
                [73, 85, 97]
            };
            rgba.extend_from_slice(&[color[0], color[1], color[2], 255]);
        }
    }
    rgba
}

fn encode(
    path: &Path,
    width: u32,
    height: u32,
    count: usize,
    scene: Scene,
    adaptive: bool,
) -> (Duration, Duration) {
    ffmpeg::init().unwrap();
    let start = Instant::now();
    if adaptive {
        let mut encoder = StreamingEncoder::create(config(path, width, height)).unwrap();
        for index in 0..count {
            encoder
                .push_rgba_frame_at_pts(index as u64, &frame(width, height, index, scene))
                .unwrap();
        }
        let finish = Instant::now();
        assert_eq!(
            encoder.finish_at_pts(count as u64).unwrap().encoded_frames,
            count as u64
        );
        (start.elapsed(), finish.elapsed())
    } else {
        // Previous production policy: libswscale RGBA -> fixed RGB8 -> GIF.
        let codec = ffmpeg::encoder::find(ffmpeg::codec::Id::GIF).unwrap();
        let mut encoder = ffmpeg::codec::context::Context::new_with_codec(codec)
            .encoder()
            .video()
            .unwrap();
        encoder.set_width(width);
        encoder.set_height(height);
        encoder.set_format(ffmpeg::format::Pixel::RGB8);
        encoder.set_time_base((1, 20));
        let mut encoder = encoder.open_as(codec).unwrap();
        let mut output = ffmpeg::format::output(path).unwrap();
        {
            let mut stream = output.add_stream(codec).unwrap();
            stream.set_parameters(&encoder);
            stream.set_time_base((1, 20));
        }
        output.write_header().unwrap();
        let time_base = output.stream(0).unwrap().time_base();
        let mut rgba = ffmpeg::frame::Video::new(ffmpeg::format::Pixel::RGBA, width, height);
        let mut indexed = ffmpeg::frame::Video::new(ffmpeg::format::Pixel::RGB8, width, height);
        let mut scaler = ffmpeg::software::scaling::Context::get(
            ffmpeg::format::Pixel::RGBA,
            width,
            height,
            ffmpeg::format::Pixel::RGB8,
            width,
            height,
            ffmpeg::software::scaling::Flags::BICUBIC,
        )
        .unwrap();
        let mut drain = |encoder: &mut ffmpeg::encoder::video::Encoder| {
            let mut packet = ffmpeg::Packet::empty();
            while encoder.receive_packet(&mut packet).is_ok() {
                packet.set_stream(0);
                packet.set_duration(1);
                packet.rescale_ts((1, 20), time_base);
                packet.write_interleaved(&mut output).unwrap();
            }
        };
        for index in 0..count {
            let pixels = frame(width, height, index, scene);
            let stride = rgba.stride(0);
            for y in 0..height as usize {
                rgba.data_mut(0)[y * stride..][..width as usize * 4]
                    .copy_from_slice(&pixels[y * width as usize * 4..][..width as usize * 4]);
            }
            // The codec may retain the previous frame for transparent differences.
            assert!(unsafe { ffmpeg::ffi::av_frame_make_writable(indexed.as_mut_ptr()) } >= 0);
            scaler.run(&rgba, &mut indexed).unwrap();
            indexed.set_pts(Some(index as i64));
            encoder.send_frame(&indexed).unwrap();
            drain(&mut encoder);
        }
        let finish = Instant::now();
        encoder.send_eof().unwrap();
        drain(&mut encoder);
        output.write_trailer().unwrap();
        (start.elapsed(), finish.elapsed())
    }
}

fn decode(path: &Path) -> Vec<(i64, Vec<u8>)> {
    // Independent decoder: production FFmpeg omits the GIF packet parser.
    let mut options = gif::DecodeOptions::new();
    options.set_color_output(gif::ColorOutput::RGBA);
    let mut decoder = options
        .read_info(std::fs::File::open(path).unwrap())
        .unwrap();
    let width = usize::from(decoder.width());
    let mut canvas = vec![0; width * usize::from(decoder.height()) * 4];
    let mut pts = 0;
    let mut frames = Vec::new();
    while let Some(frame) = decoder.read_next_frame().unwrap() {
        assert!(matches!(
            frame.dispose,
            gif::DisposalMethod::Keep | gif::DisposalMethod::Any
        ));
        for y in 0..usize::from(frame.height) {
            for x in 0..usize::from(frame.width) {
                let source = (y * usize::from(frame.width) + x) * 4;
                let destination =
                    ((y + usize::from(frame.top)) * width + x + usize::from(frame.left)) * 4;
                if frame.buffer[source + 3] != 0 {
                    canvas[destination..destination + 4]
                        .copy_from_slice(&frame.buffer[source..source + 4]);
                }
            }
        }
        frames.push((pts, canvas.clone()));
        pts += i64::from(frame.delay);
    }
    frames
}

fn mse(path: &Path, width: u32, height: u32, count: usize, scene: Scene) -> f64 {
    let decoded = decode(path);
    assert_eq!(decoded.len(), count);
    let mut error = 0u64;
    for (index, (_, pixels)) in decoded.into_iter().enumerate() {
        for (actual, expected) in pixels
            .chunks_exact(4)
            .zip(frame(width, height, index, scene).chunks_exact(4))
        {
            assert_eq!(actual[3], 255, "opaque recording must stay opaque");
            for c in 0..3 {
                error += (i64::from(actual[c]) - i64::from(expected[c])).pow(2) as u64;
            }
        }
    }
    error as f64 / (u64::from(width) * u64::from(height) * count as u64 * 3) as f64
}

#[test]
fn gif_adaptive_palette_improves_screen_colors_and_compression() {
    let directory = tempfile::tempdir().unwrap();
    for scene in [Scene::Ui, Scene::Gradient, Scene::ScrollingGradient] {
        let old = directory.path().join(format!("old-{scene:?}.gif"));
        let new = directory.path().join(format!("new-{scene:?}.gif"));
        encode(&old, 192, 108, 12, scene, false);
        encode(&new, 192, 108, 12, scene, true);
        let old_error = mse(&old, 192, 108, 12, scene);
        let new_error = mse(&new, 192, 108, 12, scene);
        let old_size = std::fs::metadata(old).unwrap().len();
        let new_size = std::fs::metadata(new).unwrap().len();
        eprintln!(
            "gradient={scene:?}, bytes={old_size}->{new_size}, MSE={old_error:.3}->{new_error:.3}"
        );
        assert!(new_error < old_error / 2.0);
        assert!(new_size < old_size);
    }
}

#[test]
fn gif_preserves_screen_contrast_with_colorful_content() {
    let directory = tempfile::tempdir().unwrap();
    let path = directory.path().join("screen-contrast.gif");
    let (width, height) = (960usize, 540usize);
    let mut rgba = vec![255; width * height * 4];
    // A white desktop, a thin black text stroke, and an image with many colors.
    // A population-weighted palette used to average the stroke into image
    // colors, decoding pure black as a brighter, tinted color.
    for x in 100..200 {
        rgba[(100 * width + x) * 4..][..3].fill(0);
    }
    for y in 0..180 {
        for x in 0..width {
            let pixel = &mut rgba[((y + 360) * width + x) * 4..][..3];
            pixel.copy_from_slice(&[
                ((x * 73 + y * 19) % 256) as u8,
                ((x * 13 + y * 107) % 256) as u8,
                ((x * 37 + y * 43) % 256) as u8,
            ]);
        }
    }
    let mut encoder = StreamingEncoder::create(config(&path, width as u32, height as u32)).unwrap();
    for pts in 0..2 {
        encoder.push_rgba_frame_at_pts(pts, &rgba).unwrap();
    }
    encoder.finish_at_pts(2).unwrap();
    let frames = decode(&path);
    assert_eq!(frames.len(), 2);
    for (_, pixels) in frames {
        assert!(pixels[..width * 4].chunks_exact(4).all(|p| p == [255; 4]));
        for x in 100..200 {
            assert_eq!(&pixels[(100 * width + x) * 4..][..4], &[0, 0, 0, 255]);
        }
    }
}

#[test]
fn gif_preserves_variable_delays_last_hold_and_single_frame() {
    ffmpeg::init().unwrap();
    let directory = tempfile::tempdir().unwrap();
    for points in [&[0u64][..], &[0, 1, 9][..], &[0, 1, 9, 13][..]] {
        let path = directory.path().join(format!("{}.gif", points.len()));
        let mut encoder = StreamingEncoder::create(config(&path, 17, 9)).unwrap();
        let colors = [
            [23, 37, 51, 255],
            [220, 139, 71, 255],
            [12, 239, 221, 255],
            [23, 37, 51, 255],
        ];
        for (index, &pts) in points.iter().enumerate() {
            let color = colors[index];
            encoder
                .push_rgba_frame_at_pts(pts, &color.repeat(17 * 9))
                .unwrap();
        }
        encoder.finish_at_pts(15).unwrap();
        let decoded = decode(&path);
        assert_eq!(
            decoded.iter().map(|f| f.0).collect::<Vec<_>>(),
            points.iter().map(|p| (*p * 5) as i64).collect::<Vec<_>>()
        );
        for (index, (_, pixels)) in decoded.iter().enumerate() {
            for pixel in pixels.chunks_exact(4) {
                assert_eq!(pixel, colors[index]);
            }
        }
        let mut input = gif::DecodeOptions::new()
            .read_info(std::fs::File::open(&path).unwrap())
            .unwrap();
        let mut end = 0;
        while let Some(frame) = input.read_next_frame().unwrap() {
            end += u64::from(frame.delay);
        }
        assert_eq!(end, 75);
    }
}

#[test]
#[ignore = "Release GIF comparison; requires SNOW_GIF_BENCH_OUTPUT"]
fn gif_encoding_release_comparison() {
    if cfg!(debug_assertions) {
        panic!("use windows-msvc-performance Release");
    }
    let base = std::path::PathBuf::from(std::env::var_os("SNOW_GIF_BENCH_OUTPUT").unwrap());
    std::fs::create_dir_all(&base).unwrap();
    let directory = tempfile::Builder::new()
        .prefix("run-")
        .tempdir_in(base)
        .unwrap()
        .keep();
    eprintln!("output_dir={}", directory.display());
    for scene in [Scene::Ui, Scene::Gradient, Scene::ScrollingGradient] {
        for adaptive in [false, true] {
            let path = directory.join(format!("gradient-{scene:?}-adaptive-{adaptive}.gif"));
            let (elapsed, finish) = encode(&path, 960, 540, 60, scene, adaptive);
            let error = mse(&path, 960, 540, 60, scene);
            eprintln!(
                "gradient={scene:?}, adaptive={adaptive}, bytes={}, MSE={error:.3}, total_ms={:.2}, finish_ms={:.2}",
                std::fs::metadata(path).unwrap().len(),
                elapsed.as_secs_f64() * 1000.0,
                finish.as_secs_f64() * 1000.0
            );
        }
    }
}
