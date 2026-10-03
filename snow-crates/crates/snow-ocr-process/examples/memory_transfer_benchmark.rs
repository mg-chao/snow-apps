//! `cargo run --release -p snow-ocr-process --no-default-features
//! --features dynamic-onnx-runtime --example memory_transfer_benchmark -- ocr-transfer 31`
//! Exercises the production shared-memory validation and RGBA -> BGR loop.

#[path = "../src/buffer.rs"]
mod buffer;
#[path = "../../../benchmark-support/memory.rs"]
mod memory;

use memmap2::MmapMut;
use std::sync::Arc;

fn main() {
    let (scenario, repetitions) = memory::arguments("ocr-transfer");
    assert_eq!(scenario, "ocr-transfer");
    let (width, height) = (3840_usize, 2160_usize);
    let stride = width * 4;
    let input_length = stride * height;
    let output_length = width * height * 3;
    memory::phase(&scenario, "empty", width as u32, height as u32, 0);
    let mut map = MmapMut::map_anon(buffer::SLOT_HEADER + input_length).unwrap();
    map[..8].copy_from_slice(&7_u64.to_le_bytes());
    for (offset, value) in [
        (8, 1),
        (12, width as u32),
        (16, height as u32),
        (20, stride as u32),
        (24, input_length as u32),
        (28, 0x544f_4c53),
    ] {
        map[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
    }
    for (index, pixel) in map[buffer::SLOT_HEADER..].chunks_exact_mut(4).enumerate() {
        pixel.copy_from_slice(&[(index % 251) as u8, 43, 91, 255]);
    }
    let shared = buffer::SharedImage {
        mmap: Arc::new(map.make_read_only().unwrap()),
        slot_bytes: buffer::SLOT_HEADER + input_length,
    };
    let mut output = None;
    memory::phase(
        &scenario,
        "setup",
        width as u32,
        height as u32,
        input_length,
    );
    memory::measure(
        &scenario,
        width as u32,
        height as u32,
        repetitions,
        input_length + output_length,
        || {
            let pixels = shared.read_bgr(0, width, height, stride, 7).unwrap();
            assert_eq!(pixels.len(), output_length);
            assert_eq!(&pixels[..3], &[91, 43, 0]);
            let checksum = memory::checksum(&pixels);
            output = Some(pixels);
            checksum
        },
    );
    drop(output);
    drop(shared);
    memory::phase(&scenario, "drop", width as u32, height as u32, 0);
}
