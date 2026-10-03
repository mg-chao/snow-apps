//! Compare complete capture replacement through preserving and overwrite preparation.
//! `cargo run --release -p snow-capture --example memory_overwrite_benchmark -- MODE 20`
//! MODE: preserve, overwrite. Both produce the same pixels and scenario identity.
//! The compatibility trait uses preserving preparation on revisions without the new API;
//! the metadata record reports whether that fallback actually ran.

#[path = "../../../benchmark-support/memory.rs"]
mod memory;

use snow_capture::error::CaptureResult;
use snow_capture::frame::{CapturePixelFormat, Frame};
use std::sync::atomic::{AtomicBool, Ordering};

static USED_FALLBACK: AtomicBool = AtomicBool::new(false);

// An inherent method takes precedence over this trait on the optimized revision.
// Retain the fallback so identical source can build against the earlier public API.
#[allow(
    dead_code,
    reason = "compatibility fallback for the earlier capture API"
)]
trait OverwritePreparation {
    fn prepare_for_overwrite(
        &mut self,
        width: u32,
        height: u32,
        format: CapturePixelFormat,
    ) -> CaptureResult<()>;
}

impl OverwritePreparation for Frame {
    fn prepare_for_overwrite(
        &mut self,
        width: u32,
        height: u32,
        format: CapturePixelFormat,
    ) -> CaptureResult<()> {
        USED_FALLBACK.store(true, Ordering::Relaxed);
        self.ensure_capacity(width, height, format)
    }
}

fn main() {
    let (operation, samples) = memory::arguments("overwrite");
    assert!(matches!(operation.as_str(), "preserve" | "overwrite"));
    let scenario = "capture-full-overwrite";
    let (width, height) = (3840, 2160);
    let length = width as usize * height as usize * 4;
    memory::phase(scenario, "empty", width, height, 0);
    let mut source = Frame::empty();
    source.ensure_rgba_capacity(width, height).unwrap();
    source.as_mut_bytes().fill(0x5a);
    let mut output = Frame::empty();
    memory::phase(scenario, "setup", width, height, length);
    memory::measure(scenario, width, height, samples, length * 2, || {
        output = source.clone();
        if operation == "preserve" {
            output.ensure_rgba_capacity(width, height).unwrap();
        } else {
            output
                .prepare_for_overwrite(width, height, CapturePixelFormat::Rgba8)
                .unwrap();
        }
        output.as_mut_bytes().copy_from_slice(source.as_bytes());
        assert_ne!(output.as_bytes().as_ptr(), source.as_bytes().as_ptr());
        assert_eq!(source.as_bytes()[0], 0x5a);
        assert_eq!(output.as_bytes()[length - 1], 0x5a);
        memory::checksum(output.as_bytes())
    });
    let implementation = if operation == "preserve" || USED_FALLBACK.load(Ordering::Relaxed) {
        "preserving"
    } else {
        "full-overwrite"
    };
    println!(
        "{{\"record\":\"metadata\",\"scenario\":\"{scenario}\",\"operation\":\"{operation}\",\"implementation\":\"{implementation}\",\"compatibility_fallback\":{}}}",
        USED_FALLBACK.load(Ordering::Relaxed)
    );
    drop(output);
    drop(source);
    memory::phase(scenario, "drop", width, height, 0);
}
