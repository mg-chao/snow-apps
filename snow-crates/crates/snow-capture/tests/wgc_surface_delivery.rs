//! Native WGC GPU surface delivery tests.
//!
//! These require an interactive Windows desktop with Windows Graphics
//! Capture support. Run them explicitly with:
//! `cargo test -p snow-capture --test wgc_surface_delivery -- --ignored`

use std::time::{Duration, Instant};

use snow_capture::backend::CaptureBackendKind;
use snow_capture::{
    CaptureEvent, CaptureOptions, CapturePixelFormat, CaptureStream, CaptureStreamConfig,
    CaptureSystem, CaptureTarget, CaptureWorkload, SurfaceDelivery,
};

/// Surface delivery is a WGC-only contract; the auto backend wrapper
/// rejects it so callers deliberately pin the WGC backend.
fn surface_capture_system() -> snow_capture::CaptureSystem {
    CaptureSystem::builder()
        .with_backend_kind(CaptureBackendKind::WindowsGraphicsCapture)
        .build()
        .expect("WGC capture system")
}

fn wait_for_surface(
    stream: &CaptureStream,
    timeout: Duration,
) -> Option<snow_capture::GpuSurfaceFrame> {
    let deadline = Instant::now() + timeout;
    while Instant::now() < deadline {
        if let Ok(CaptureEvent::Surface(frame)) = stream.recv_timeout(Duration::from_millis(250)) {
            return Some(frame);
        }
    }
    None
}

fn surface_stream_config() -> CaptureStreamConfig {
    CaptureStreamConfig {
        target_fps: 30,
        min_fps: 5,
        buffer_depth: 4,
        max_consecutive_errors: 5,
        adaptive_fps: false,
        pause_on_resolution_change: false,
        include_cursor: true,
    }
}

#[test]
#[ignore = "requires an interactive Windows desktop and WGC"]
fn surface_stream_delivers_wgc_textures_without_cpu_frames() {
    let system = surface_capture_system();
    let session = system
        .open_session(
            CaptureTarget::PrimaryMonitor,
            CaptureOptions {
                workload: CaptureWorkload::Continuous,
                output_pixel_format: CapturePixelFormat::Bgra8,
                output_surface: SurfaceDelivery::GpuTexture {
                    native_cursor: true,
                },
                ..CaptureOptions::default()
            },
        )
        .expect("WGC surface session should open on an interactive desktop");

    let stream = CaptureStream::spawn(session, surface_stream_config())
        .expect("surface stream should spawn");

    let first =
        wait_for_surface(&stream, Duration::from_secs(15)).expect("at least one surface frame");
    assert!(
        first.d3d11_texture().is_some(),
        "surface frames must carry a D3D11 texture"
    );
    assert!(
        first.d3d11_device().is_some(),
        "surface frames must carry the D3D11 device"
    );
    assert!(first.width() > 0 && first.height() > 0);

    // Hold several frames to exercise ring leases, then drop them to
    // exercise slot release.
    let mut frames = vec![first];
    for _ in 0..7 {
        if let Some(frame) = wait_for_surface(&stream, Duration::from_secs(5)) {
            frames.push(frame);
        }
    }
    assert!(
        frames.len() >= 4,
        "expected several surface frames, got {}",
        frames.len()
    );
    let duplicates = frames.iter().filter(|frame| frame.is_duplicate()).count();
    let sequences: Vec<u64> = frames
        .iter()
        .map(|frame| frame.metadata().sequence())
        .collect();
    assert!(
        sequences.windows(2).all(|pair| pair[0] < pair[1]),
        "sequences must be strictly increasing: {sequences:?}"
    );
    eprintln!("surface frames: {}, duplicates: {duplicates}", frames.len());

    drop(frames);
    stream.pause();
    stream.resume();
    assert!(
        wait_for_surface(&stream, Duration::from_secs(5)).is_some(),
        "stream must keep delivering after handles drop and pause/resume"
    );

    for event in stream.stop_and_drain() {
        assert!(
            !matches!(event, CaptureEvent::Error(_)),
            "surface stream must stop cleanly, got {event:?}"
        );
    }
}

#[test]
#[ignore = "requires an interactive Windows desktop and WGC"]
fn surface_region_delivery_crops_to_region_geometry() {
    let layout = snow_capture::MonitorLayout::snapshot().expect("monitor layout");
    let monitor = layout
        .monitors
        .first()
        .expect("at least one monitor")
        .clone();
    let width = (monitor.width / 2).max(64) & !1;
    let height = (monitor.height / 2).max(64) & !1;
    let region = snow_capture::CaptureRegion::new(monitor.x + 16, monitor.y + 12, width, height)
        .expect("region inside the monitor");

    let system = surface_capture_system();
    let session = system
        .open_session(
            CaptureTarget::Region(region),
            CaptureOptions {
                workload: CaptureWorkload::Continuous,
                output_pixel_format: CapturePixelFormat::Bgra8,
                output_surface: SurfaceDelivery::GpuTexture {
                    native_cursor: true,
                },
                ..CaptureOptions::default()
            },
        )
        .expect("region surface session");
    let stream = CaptureStream::spawn(session, surface_stream_config())
        .expect("region surface stream should spawn");

    let frame = wait_for_surface(&stream, Duration::from_secs(15)).expect("region surface frame");
    assert_eq!(frame.dimensions(), (width, height));
    let crop = frame.crop().expect("region frames must carry a crop rect");
    assert_eq!(
        (crop.x, crop.y, crop.width, crop.height),
        (16, 12, width, height)
    );
    assert!(
        frame.texture_dimensions().0 >= width && frame.texture_dimensions().1 >= height,
        "texture must be at least the crop size"
    );
    drop(stream);
}
