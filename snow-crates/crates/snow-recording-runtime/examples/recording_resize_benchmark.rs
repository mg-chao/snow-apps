//! Exact-output A/B benchmark for recording resize and buffer reuse.
#[path = "../src/rgba_resizer.rs"]
mod rgba_resizer;

use rgba_resizer::RgbaResizer;
use std::{fs, hint::black_box, path::PathBuf, time::Instant};

// Original implementation, retained only as the performance/correctness reference.
fn baseline(source: &[u8], source_size: (u32, u32), size: (u32, u32)) -> Vec<u8> {
    if source_size == size {
        return source.to_vec();
    }
    let mut output = vec![0; size.0 as usize * size.1 as usize * 4];
    for y in 0..size.1 {
        let source_y = (u64::from(y) * u64::from(source_size.1) / u64::from(size.1)) as u32;
        for x in 0..size.0 {
            let source_x = (u64::from(x) * u64::from(source_size.0) / u64::from(size.0)) as u32;
            let from = (source_y as usize * source_size.0 as usize + source_x as usize) * 4;
            let to = (y as usize * size.0 as usize + x as usize) * 4;
            if from + 4 <= source.len() {
                output[to..to + 4].copy_from_slice(&source[from..from + 4]);
            }
        }
    }
    output
}

fn main() {
    if cfg!(debug_assertions) {
        panic!("benchmark requires Release");
    }
    let directory = PathBuf::from(
        std::env::args()
            .nth(1)
            .unwrap_or_else(|| "target/perf/recording-resize".into()),
    );
    fs::create_dir_all(&directory).unwrap();
    let mut csv = String::from("source_width,source_height,width,height,mode,sample,frame_ms\n");
    for (source_size, size) in [
        ((1920, 1080), (1920, 1080)),
        ((3840, 2160), (3840, 2160)),
        ((3840, 2160), (1920, 1080)),
        ((3840, 2160), (1280, 720)),
        ((1920, 1080), (1280, 720)),
    ] {
        let source: Vec<u8> = (0..source_size.0 as usize * source_size.1 as usize * 4)
            .map(|index| (index ^ (index >> 11)) as u8)
            .collect();
        let mut resizer = RgbaResizer::default();
        let mut output = Vec::new();
        resizer.resize_into(&source, source_size, size, &mut output);
        assert_eq!(output, baseline(&source, source_size, size));
        for sample in 0usize..=7 {
            for offset in 0..2 {
                let optimized = (sample + offset).is_multiple_of(2);
                let start = Instant::now();
                for _ in 0..60 {
                    if optimized {
                        resizer.resize_into(
                            black_box(&source),
                            black_box(source_size),
                            black_box(size),
                            &mut output,
                        );
                        black_box(&output);
                    } else {
                        black_box(baseline(
                            black_box(&source),
                            black_box(source_size),
                            black_box(size),
                        ));
                    }
                }
                if sample > 0 {
                    let row = format!(
                        "{},{},{},{},{},{sample},{:.6}\n",
                        source_size.0,
                        source_size.1,
                        size.0,
                        size.1,
                        if optimized {
                            "cached-reuse"
                        } else {
                            "baseline"
                        },
                        start.elapsed().as_secs_f64() * 1000.0 / 60.0
                    );
                    print!("{row}");
                    csv.push_str(&row);
                }
            }
        }
    }
    fs::write(directory.join("recording-resize.csv"), csv).unwrap();
}
