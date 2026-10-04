//! Deterministic region-dispatch benchmark; no native GPU capture is performed.
//! Run in Release as the only test in a fresh process:
//! `SNOW_REGION_BENCHMARK_CASE=full SNOW_REGION_BENCHMARK_SAMPLES=31 cargo test
//! --release -p snow-capture --lib
//! capture_session::region_memory_benchmark::shared_stale_region_capture
//! -- --ignored --exact --nocapture --test-threads=1`
//! Use `uncovered` to measure capture of the left half plus clearing the output.

#[allow(
    dead_code,
    reason = "shared example argument parsing is unused by the test harness"
)]
#[path = "../../../benchmark-support/memory.rs"]
mod memory;

use super::*;
use crate::region::MonitorGeometry;
use snow_memory::RasterBuffer;

struct MockBackend {
    monitor: MonitorId,
    pixels: Arc<RasterBuffer>,
    width: u32,
    height: u32,
    covered_width: u32,
}

struct MockCapturer {
    pixels: Arc<RasterBuffer>,
    stride: usize,
}

impl CaptureBackend for MockBackend {
    fn enumerate_monitors(&self) -> CaptureResult<Vec<MonitorId>> {
        Ok(vec![self.monitor.clone()])
    }

    fn primary_monitor(&self) -> CaptureResult<MonitorId> {
        Ok(self.monitor.clone())
    }

    fn monitor_layout(&self) -> CaptureResult<MonitorLayout> {
        Ok(MonitorLayout {
            monitors: vec![MonitorGeometry {
                monitor: self.monitor.clone(),
                x: 0,
                y: 0,
                width: self.covered_width,
                height: self.height,
            }],
            virtual_left: 0,
            virtual_top: 0,
            virtual_width: self.covered_width,
            virtual_height: self.height,
        })
    }

    fn create_monitor_capturer(
        &self,
        _monitor: &MonitorId,
    ) -> CaptureResult<Box<dyn MonitorCapturer>> {
        Ok(Box::new(MockCapturer {
            pixels: Arc::clone(&self.pixels),
            stride: self.width as usize * 4,
        }))
    }
}

impl MonitorCapturer for MockCapturer {
    fn capture(&mut self, _reuse: Option<Frame>) -> CaptureResult<Frame> {
        unreachable!("the fixture implements direct region capture")
    }

    fn capture_region_into(
        &mut self,
        blit: CaptureBlitRegion,
        destination: &mut Frame,
        destination_has_history: bool,
    ) -> CaptureResult<Option<CaptureSampleMetadata>> {
        assert!(!destination_has_history);
        let destination_stride = destination.width() as usize * 4;
        let row_bytes = blit.width as usize * 4;
        let bytes = destination.as_mut_bytes();
        for row in 0..blit.height as usize {
            let source_start = (blit.src_y as usize + row) * self.stride + blit.src_x as usize * 4;
            let destination_start =
                (blit.dst_y as usize + row) * destination_stride + blit.dst_x as usize * 4;
            bytes[destination_start..destination_start + row_bytes]
                .copy_from_slice(&self.pixels[source_start..source_start + row_bytes]);
        }
        Ok(Some(CaptureSampleMetadata::default()))
    }
}

#[test]
#[ignore = "Release-only memory and latency benchmark; run serially in a fresh process"]
#[allow(
    clippy::assertions_on_constants,
    reason = "the test is linted in debug but may only be measured in Release"
)]
fn shared_stale_region_capture() -> CaptureResult<()> {
    assert!(
        !cfg!(debug_assertions),
        "memory benchmarks require --release"
    );
    let case = std::env::var("SNOW_REGION_BENCHMARK_CASE").unwrap_or_else(|_| "full".into());
    assert!(matches!(case.as_str(), "full" | "uncovered"));
    let repetitions =
        std::env::var("SNOW_REGION_BENCHMARK_SAMPLES").map_or(31, |value| value.parse().unwrap());
    assert!(repetitions > 0);
    let scenario = format!("mock-region-stale-{case}");
    let (width, height) = (3840, 2160);
    let covered_width = if case == "full" { width } else { width / 2 };
    let len = width as usize * height as usize * 4;
    memory::phase(&scenario, "empty", width, height, 0);
    let mut source = Frame::empty();
    source.ensure_rgba_capacity(width, height)?;
    for (index, byte) in source.as_mut_bytes().iter_mut().enumerate() {
        *byte = (index % 251) as u8;
    }
    let backend: Arc<dyn CaptureBackend> = Arc::new(MockBackend {
        monitor: MonitorId::from_parts(101, 103, 0, "memory-benchmark-monitor", true),
        pixels: source.shared_bytes(),
        width,
        height,
        covered_width,
    });
    let mut session = CaptureSession::builder()
        .target(CaptureTarget::Region(CaptureRegion::new(
            0, 0, width, height,
        )?))
        .with_backend(backend)
        .build()?;
    let mut output = Frame::empty();
    memory::phase(&scenario, "setup", width, height, len);
    memory::measure(&scenario, width, height, repetitions, len * 2, || {
        // Source sequence zero never belongs to the session's output history.
        // Sharing its storage forces the actual region dispatch to detach it.
        output = session.capture_reuse(source.clone()).unwrap();
        assert_ne!(source.as_bytes().as_ptr(), output.as_bytes().as_ptr());
        assert_eq!(source.as_bytes()[len - 1], ((len - 1) % 251) as u8);
        memory::checksum(output.as_bytes())
    });
    for (actual, expected) in output
        .as_bytes()
        .chunks_exact(width as usize * 4)
        .zip(source.as_bytes().chunks_exact(width as usize * 4))
    {
        let copied = covered_width as usize * 4;
        assert_eq!(&actual[..copied], &expected[..copied]);
        assert!(actual[copied..].iter().all(|&byte| byte == 0));
    }
    drop(output);
    drop(session);
    drop(source);
    memory::phase(&scenario, "drop", width, height, 0);
    Ok(())
}
