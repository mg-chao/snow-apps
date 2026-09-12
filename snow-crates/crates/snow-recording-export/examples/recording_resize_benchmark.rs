//! Focused pixel-equivalent resize benchmark; run in the performance Release build.
use snow_recording_export::resize::NearestResizePlan;
use std::hint::black_box;
use std::time::{Duration, Instant};

fn scalar(source: &[u8], source_size: (u32, u32), output_size: (u32, u32), out: &mut [u8]) {
    for y in 0..output_size.1 {
        let sy = u64::from(y) * u64::from(source_size.1) / u64::from(output_size.1);
        for x in 0..output_size.0 {
            let sx = u64::from(x) * u64::from(source_size.0) / u64::from(output_size.0);
            let src = (sy * u64::from(source_size.0) + sx) as usize * 4;
            let dst = (y * output_size.0 + x) as usize * 4;
            out[dst..dst + 4].copy_from_slice(&source[src..src + 4]);
        }
    }
}

fn main() {
    if cfg!(debug_assertions) {
        panic!("use the performance Release build");
    }
    let pool = std::env::args()
        .any(|argument| argument == "--parallel")
        .then(|| {
            rayon::ThreadPoolBuilder::new()
                .num_threads(2)
                .build()
                .expect("two resize workers")
        });
    println!("pair,variant,width,height,iterations,nanoseconds_per_resize");
    for (source_size, output_size) in [
        ((3840, 2160), (1920, 1080)),
        ((2160, 3840), (1080, 1920)),
        ((1919, 1079), (959, 539)),
    ] {
        let source: Vec<u8> = (0..source_size.0 * source_size.1 * 4)
            .map(|i| (i % 251) as u8)
            .collect();
        let plan =
            NearestResizePlan::new(source_size.0, source_size.1, output_size.0, output_size.1);
        let mut reference = vec![0; (output_size.0 * output_size.1 * 4) as usize];
        let mut output = reference.clone();
        scalar(&source, source_size, output_size, &mut reference);
        plan.resize_into(&source, &mut output);
        assert_eq!(reference, output);
        if let Some(pool) = pool.as_ref() {
            plan.resize_into_with_pool(&source, &mut output, pool);
            assert_eq!(reference, output);
        }
        for pair in 0..5 {
            for variant in if pair % 2 == 0 {
                [false, true]
            } else {
                [true, false]
            } {
                let started = Instant::now();
                let mut iterations = 0;
                while started.elapsed() < Duration::from_secs(2) {
                    if variant {
                        if let Some(pool) = pool.as_ref() {
                            plan.resize_into_with_pool(
                                black_box(&source),
                                black_box(&mut output),
                                pool,
                            );
                        } else {
                            plan.resize_into(black_box(&source), black_box(&mut output));
                        }
                    } else if pool.is_some() {
                        plan.resize_into(black_box(&source), black_box(&mut output));
                    } else {
                        scalar(
                            black_box(&source),
                            black_box(source_size),
                            black_box(output_size),
                            black_box(&mut output),
                        );
                    }
                    black_box(&output);
                    iterations += 1;
                }
                println!(
                    "{},{},{},{},{},{}",
                    pair + 1,
                    match (variant, pool.is_some()) {
                        (true, true) => "parallel2",
                        (false, false) => "scalar",
                        _ => "planned",
                    },
                    output_size.0,
                    output_size.1,
                    iterations,
                    started.elapsed().as_nanos() / iterations
                );
            }
        }
    }
}
