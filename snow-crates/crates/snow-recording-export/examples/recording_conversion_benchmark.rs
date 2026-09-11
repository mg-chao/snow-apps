//! Pixel-equivalent libswscale conversion benchmark, without capture/encoding noise.
#[path = "../src/rgba_converter.rs"]
mod rgba_converter;
use ffmpeg_next as ffmpeg;
use rgba_converter::RgbaConverter;
use std::{fs, hint::black_box, path::PathBuf, time::Instant};

fn main() {
    if cfg!(debug_assertions) {
        panic!("benchmark requires Release");
    }
    ffmpeg::init().unwrap();
    let directory = PathBuf::from(
        std::env::args()
            .nth(1)
            .unwrap_or_else(|| "target/perf/recording-conversion".into()),
    );
    fs::create_dir_all(&directory).unwrap();
    let mut csv = String::from("width,height,format,mode,sample,frame_ms\n");
    for (width, height) in [(1920, 1080), (3840, 2160)] {
        for format in [ffmpeg::format::Pixel::NV12, ffmpeg::format::Pixel::YUV420P] {
            let mut source = ffmpeg::frame::Video::new(ffmpeg::format::Pixel::RGBA, width, height);
            for (index, byte) in source.data_mut(0).iter_mut().enumerate() {
                *byte = (index ^ (index >> 11)) as u8;
            }
            let mut legacy = ffmpeg::software::scaling::Context::get(
                ffmpeg::format::Pixel::RGBA,
                width,
                height,
                format,
                width,
                height,
                ffmpeg::software::scaling::Flags::BICUBIC,
            )
            .unwrap();
            let mut converter = RgbaConverter::new(width, height, format, 4).unwrap();
            let mut expected = ffmpeg::frame::Video::new(format, width, height);
            let mut actual = ffmpeg::frame::Video::new(format, width, height);
            legacy.run(&source, &mut expected).unwrap();
            converter.run(&source, &mut actual).unwrap();
            for plane in 0..actual.planes() {
                assert_eq!(actual.data(plane), expected.data(plane));
            }
            for sample in 0usize..=5 {
                for offset in 0..2 {
                    let optimized = (sample + offset).is_multiple_of(2);
                    let start = Instant::now();
                    for _ in 0..30 {
                        if optimized {
                            converter.run(black_box(&source), &mut actual).unwrap();
                        } else {
                            legacy.run(black_box(&source), &mut expected).unwrap();
                        }
                    }
                    if sample > 0 {
                        let row = format!(
                            "{width},{height},{format:?},{},{sample},{:.6}\n",
                            if optimized {
                                "frame-4-threads"
                            } else {
                                "legacy"
                            },
                            start.elapsed().as_secs_f64() * 1000.0 / 30.0
                        );
                        print!("{row}");
                        csv.push_str(&row);
                    }
                }
            }
        }
    }
    fs::write(directory.join("recording-conversion.csv"), csv).unwrap();
}
