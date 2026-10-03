//! Benchmark the unchanged bincode wire format through actual StoredFrame APIs.
//! `cargo run --release -p snow-recording-model --example memory_decode_benchmark -- recording-decode 31`

#[path = "../../../benchmark-support/memory.rs"]
mod memory;

use snow_recording_model::StoredFrame;

fn main() {
    let (scenario, repetitions) = memory::arguments("recording-decode");
    assert_eq!(scenario, "recording-decode");
    let (width, height) = (1920, 1080);
    let length = width as usize * height as usize * 4;
    memory::phase(&scenario, "empty", width, height, 0);
    // Construct the stable wire bytes directly so fixtures allocate the same
    // storage on both revisions (the production field changes Vec -> RasterBuffer).
    let mut encoded = Vec::with_capacity(28 + length);
    encoded.extend_from_slice(&7_u64.to_le_bytes());
    encoded.extend_from_slice(&33_u32.to_le_bytes());
    encoded.extend_from_slice(&width.to_le_bytes());
    encoded.extend_from_slice(&height.to_le_bytes());
    encoded.extend_from_slice(&(length as u64).to_le_bytes());
    encoded.resize(28 + length, 0x5a);
    let mut decoded = Vec::with_capacity(8);
    memory::phase(&scenario, "setup", width, height, encoded.len());
    memory::measure(
        &scenario,
        width,
        height,
        repetitions,
        length * 9 + 28,
        || {
            decoded.clear();
            for _ in 0..8 {
                let frame: StoredFrame = bincode::deserialize(&encoded).unwrap();
                assert_eq!(frame.timestamp_ms, 7);
                assert_eq!(frame.duration_ms, 33);
                assert_eq!((frame.width, frame.height), (width, height));
                assert_eq!(frame.rgba.len(), length);
                decoded.push(frame);
            }
            decoded.iter().fold(0_u64, |hash, frame| {
                hash.wrapping_add(memory::checksum(&frame.rgba))
            })
        },
    );
    drop(decoded);
    drop(encoded);
    memory::phase(&scenario, "drop", width, height, 0);
}
