//! `cargo run --release -p snow-capture --example memory_buffer_benchmark -- CASE 31`
//! CASE: capture-reuse, capture-resize, capture-cow, capture-import, capture-churn.
//! All APIs and workloads are compatible with the pre-optimization revision.

#[path = "../../../benchmark-support/memory.rs"]
mod memory;

use snow_capture::frame::Frame;
use std::hint::black_box;

fn main() {
    let (scenario, repetitions) = memory::arguments("capture-cow");
    let (width, height) = (3840, 2160);
    let length = width as usize * height as usize * 4;
    memory::phase(&scenario, "empty", width, height, 0);
    let mut source = Frame::empty();
    source.ensure_rgba_capacity(width, height).unwrap();
    source.as_mut_bytes().fill(0x5a);
    let mut output = Frame::empty();
    let mut batch = Vec::with_capacity(8);
    let logical_bytes = match scenario.as_str() {
        "capture-reuse" | "capture-resize" => length,
        "capture-cow" | "capture-import" => length * 2,
        "capture-churn" => length * 9,
        _ => panic!("unknown capture scenario: {scenario}"),
    };
    memory::phase(&scenario, "setup", width, height, length);
    memory::measure(
        &scenario,
        width,
        height,
        repetitions,
        logical_bytes,
        || match scenario.as_str() {
            "capture-reuse" => {
                source.ensure_rgba_capacity(width, height).unwrap();
                source.as_mut_bytes().fill(0x5a);
                memory::checksum(source.as_bytes())
            }
            "capture-resize" => {
                source.ensure_rgba_capacity(width, height - 64).unwrap();
                source.as_mut_bytes().fill(0x5a);
                source.ensure_rgba_capacity(width, height).unwrap();
                source.as_mut_bytes().fill(0x5a);
                memory::checksum(source.as_bytes())
            }
            "capture-cow" => {
                output = source.clone();
                output.as_mut_bytes()[0] = 0xa5;
                assert_eq!(source.as_bytes()[0], 0x5a);
                assert_eq!(output.as_bytes()[length - 1], 0x5a);
                memory::checksum(output.as_bytes())
            }
            "capture-import" => {
                output = Frame::from_rgba8(width, height, source.as_bytes().to_vec()).unwrap();
                memory::checksum(output.as_bytes())
            }
            "capture-churn" => {
                batch.clear();
                for _ in 0..8 {
                    let mut frame = Frame::empty();
                    frame.ensure_rgba_capacity(width, height).unwrap();
                    frame.as_mut_bytes().fill(0x5a);
                    batch.push(frame);
                }
                assert_eq!(batch.len(), 8);
                batch.iter().fold(0_u64, |hash, frame| {
                    hash.wrapping_add(memory::checksum(black_box(frame.as_bytes())))
                })
            }
            _ => unreachable!(),
        },
    );
    drop(batch);
    drop(output);
    drop(source);
    memory::phase(&scenario, "drop", width, height, 0);
}
