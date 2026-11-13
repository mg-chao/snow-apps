//! Serial, opt-in native qualification. Synthetic positions never move the
//! user's pointer. Run with --features bench-follow-mouse -- --ignored.
#![cfg(all(windows, feature = "bench-follow-mouse"))]

use std::time::{Duration, Instant};

use snow_capture::backend::CaptureBackendKind;
use snow_capture::gpu::{GpuCaptureEvent, GpuCaptureStream, GpuCapturedFrame};
use snow_capture::{
    CaptureEvent, CaptureOptions, CaptureRegion, CaptureStream, CaptureStreamConfig,
    CaptureWorkload, CapturedFrame, FrameMetadata, MonitorLayout, set_bench_follow_mouse_position,
};
use snow_media::follow_mouse::FollowMouseOptions;

struct Reset;
impl Drop for Reset {
    fn drop(&mut self) {
        set_bench_follow_mouse_position(None);
    }
}
fn config() -> CaptureStreamConfig {
    CaptureStreamConfig {
        target_fps: 30,
        min_fps: 30,
        buffer_depth: 2,
        max_consecutive_errors: 30,
        adaptive_fps: false,
        pause_on_resolution_change: false,
        include_cursor: true,
    }
}
fn at(metadata: &FrameMetadata, x: i32, y: i32) -> bool {
    let Some(transform) = metadata.capture_transform() else {
        return false;
    };
    let inside = f64::from(x) >= transform.source.x
        && f64::from(y) >= transform.source.y
        && f64::from(x) < transform.source.x + transform.source.width
        && f64::from(y) < transform.source.y + transform.source.height;
    inside
        && metadata.cursor().is_some_and(|cursor| {
            cursor.x == x - transform.source.x as i32 && cursor.y == y - transform.source.y as i32
        })
}
fn gpu_hidden_at(stream: &GpuCaptureStream, x: i32, y: i32) -> GpuCapturedFrame {
    let deadline = Instant::now() + Duration::from_secs(8);
    loop {
        assert!(
            Instant::now() < deadline,
            "hidden cursor following source did not reach target"
        );
        match stream.recv_timeout(Duration::from_millis(100)) {
            Ok(GpuCaptureEvent::Frame(frame)) => {
                assert!(frame.metadata().cursor().is_none());
                let source = frame.metadata().capture_transform().unwrap().source;
                if f64::from(x) >= source.x
                    && f64::from(y) >= source.y
                    && f64::from(x) < source.x + source.width
                    && f64::from(y) < source.y + source.height
                {
                    return frame;
                }
            }
            Ok(GpuCaptureEvent::Error(error)) => panic!("hidden cursor following capture: {error}"),
            Ok(GpuCaptureEvent::StreamEnded) => panic!("hidden cursor following capture ended"),
            _ => {}
        }
    }
}
fn gpu_at(stream: &GpuCaptureStream, x: i32, y: i32) -> GpuCapturedFrame {
    gpu_at_when(stream, x, y, |_| true)
}

fn gpu_at_when(
    stream: &GpuCaptureStream,
    x: i32,
    y: i32,
    ready: impl Fn(&FrameMetadata) -> bool,
) -> GpuCapturedFrame {
    let deadline = Instant::now() + Duration::from_secs(8);
    loop {
        assert!(
            Instant::now() < deadline,
            "following GPU source did not reach target"
        );
        match stream.recv_timeout(Duration::from_millis(100)) {
            Ok(GpuCaptureEvent::Frame(frame))
                if at(frame.metadata(), x, y) && ready(frame.metadata()) =>
            {
                return frame;
            }
            Ok(GpuCaptureEvent::Error(error)) => panic!("following GPU capture: {error}"),
            Ok(GpuCaptureEvent::StreamEnded) => panic!("following GPU capture ended"),
            _ => {}
        }
    }
}

#[test]
#[ignore = "requires a live interactive desktop; uses synthetic pointer coordinates"]
fn follow_mouse_native_motion_reprojects_cursor_without_stationary_geometry_revisions() {
    let _reset = Reset;
    let layout = MonitorLayout::snapshot().unwrap();
    let monitor = layout
        .monitors
        .iter()
        .find(|monitor| monitor.width >= 640 && monitor.height >= 400)
        .expect("a display must fit the test viewport and its pan");
    let (x, y) = (
        monitor.x + (monitor.width / 2) as i32,
        monitor.y + (monitor.height / 2) as i32,
    );
    let region = CaptureRegion {
        x: x - 160,
        y: y - 90,
        width: 320,
        height: 180,
    };
    set_bench_follow_mouse_position(Some((x, y)));
    let stream = GpuCaptureStream::spawn_follow_mouse_with_motion(
        region,
        CaptureBackendKind::WindowsGraphicsCapture,
        config(),
        FollowMouseOptions {
            smooth_movement: true,
            reduce_jitter: true,
        },
    )
    .unwrap();
    let first = gpu_at(&stream, x, y);
    let left = f64::from(region.x);
    assert_eq!(first.metadata().capture_transform().unwrap().source.x, left);

    set_bench_follow_mouse_position(Some((x + 133, y + 74)));
    let inside = gpu_at(&stream, x + 133, y + 74);
    assert_eq!(
        inside.metadata().configuration_generation(),
        first.metadata().configuration_generation()
    );

    let goal = left + 134.0;
    set_bench_follow_mouse_position(Some((x + 134, y)));
    let panning = gpu_at_when(&stream, x + 134, y, |metadata| {
        let origin = metadata.capture_transform().unwrap().source.x;
        origin > left && origin < goal
    });
    assert!(
        panning.metadata().configuration_generation()
            > inside.metadata().configuration_generation()
    );
    let settled = gpu_at_when(&stream, x + 134, y, |metadata| {
        metadata.capture_transform().unwrap().source.x == goal
    });
    assert!(
        settled.metadata().configuration_generation()
            > panning.metadata().configuration_generation()
    );

    set_bench_follow_mouse_position(Some((x + 60, y)));
    let held = gpu_at(&stream, x + 60, y);
    assert_eq!(
        held.metadata().configuration_generation(),
        settled.metadata().configuration_generation()
    );
    stream.stop_and_drain();
}

fn cpu_at(stream: &CaptureStream, x: i32, y: i32) -> CapturedFrame {
    let deadline = Instant::now() + Duration::from_secs(8);
    loop {
        assert!(
            Instant::now() < deadline,
            "following CPU source did not reach target"
        );
        match stream.recv_timeout(Duration::from_millis(100)) {
            Ok(CaptureEvent::Frame(frame)) if at(frame.metadata(), x, y) => return frame,
            Ok(CaptureEvent::Error(error)) => panic!("following CPU capture: {error}"),
            Ok(CaptureEvent::StreamEnded) => panic!("following CPU capture ended"),
            _ => {}
        }
    }
}

#[test]
#[ignore = "requires a live interactive desktop; uses synthetic pointer coordinates"]
fn follow_mouse_native_wgc_dxgi_cpu_and_monitor_handoffs() {
    let _reset = Reset;
    let layout = MonitorLayout::snapshot().unwrap();
    assert!(!layout.monitors.is_empty());
    let initial = &layout.monitors[0];
    eprintln!(
        "native monitor layout: {:?}",
        layout
            .monitors
            .iter()
            .map(|monitor| (
                monitor.monitor.stable_id(),
                monitor.x,
                monitor.y,
                monitor.width,
                monitor.height
            ))
            .collect::<Vec<_>>()
    );
    let nominal = CaptureRegion::new(100000, 100000, 321, 181).unwrap();
    for backend in [
        CaptureBackendKind::WindowsGraphicsCapture,
        CaptureBackendKind::DxgiDuplication,
    ] {
        let (x, y) = (initial.x + 20, initial.y + 20);
        set_bench_follow_mouse_position(Some((x, y)));
        let stream = GpuCaptureStream::spawn_follow_mouse(nominal, backend, config()).unwrap();
        let first = gpu_at(&stream, x, y);
        eprintln!(
            "original recording adapter: {:?}",
            stream.device().identity()
        );
        assert!(
            first.layers[0]
                .texture
                .device()
                .same_device(stream.device())
        );
        let first_geometry = first.metadata().configuration_generation();
        let first_content = first.metadata().content_generation();
        let (x, y) = (
            initial.x + initial.width as i32 - 20,
            initial.y + initial.height as i32 - 20,
        );
        set_bench_follow_mouse_position(Some((x, y)));
        let moved = gpu_at(&stream, x, y);
        assert!(moved.metadata().configuration_generation() > first_geometry);
        assert!(moved.metadata().content_generation() > first_content);
        assert!(
            moved.layers[0]
                .texture
                .device()
                .same_device(stream.device())
        );
        if let Some(destination) = layout.monitors.get(1) {
            stream.pause();
            std::thread::sleep(Duration::from_millis(30));
            let paused = stream.stats().snapshot();
            while stream.try_recv().is_ok() {}
            let (x, y) = (destination.x + 20, destination.y + 20);
            set_bench_follow_mouse_position(Some((x, y)));
            std::thread::sleep(Duration::from_millis(50));
            let still_paused = stream.stats().snapshot();
            assert_eq!(
                still_paused.follow_prepare_requests,
                paused.follow_prepare_requests
            );
            assert_eq!(
                still_paused.follow_monitor_switches,
                paused.follow_monitor_switches
            );
            assert_eq!(still_paused.frames_captured, paused.frames_captured);
            assert!(stream.recv_timeout(Duration::from_millis(20)).is_err());
            stream.resume();
            let resumed = gpu_at(&stream, x, y);
            assert!(
                resumed.layers[0]
                    .texture
                    .device()
                    .same_device(stream.device())
            );
        }
        for destination in layout.monitors.iter().skip(1).take(2) {
            let (x, y) = (
                destination.x + destination.width as i32 / 2,
                destination.y + destination.height as i32 / 2,
            );
            set_bench_follow_mouse_position(Some((x, y)));
            let switched = gpu_at(&stream, x, y);
            assert!(
                switched.layers[0]
                    .texture
                    .device()
                    .same_device(stream.device())
            );
            assert!(stream.stats().snapshot().follow_peak_sources <= 2);
        }
        eprintln!(
            "requested={backend:?}, actual={:?}, stats={:?}",
            moved.metadata().backend_kind(),
            stream.stats().snapshot()
        );
        stream.stop_and_drain();
        set_bench_follow_mouse_position(Some((initial.x + 20, initial.y + 20)));
        let cpu = CaptureStream::spawn_follow_mouse(
            nominal,
            backend,
            CaptureOptions {
                workload: CaptureWorkload::Continuous,
                ..Default::default()
            },
            config(),
        )
        .unwrap();
        let deadline = Instant::now() + Duration::from_secs(5);
        loop {
            assert!(
                Instant::now() < deadline,
                "CPU following source produced no first frame"
            );
            match cpu.recv_timeout(Duration::from_millis(100)) {
                Ok(CaptureEvent::Frame(frame))
                    if at(frame.metadata(), initial.x + 20, initial.y + 20) =>
                {
                    assert_eq!(frame.dimensions(), (321, 181));
                    assert_eq!(
                        frame.pixel_format(),
                        snow_capture::CapturePixelFormat::Rgba8
                    );
                    break;
                }
                Ok(CaptureEvent::Error(error)) => panic!("following CPU capture: {error}"),
                _ => {}
            }
        }
        cpu.stop_and_drain();
    }

    // Following remains independent of cursor rendering. No attached shape is
    // needed to pan or prepare another monitor when cursor observations are off.
    for backend in [
        CaptureBackendKind::WindowsGraphicsCapture,
        CaptureBackendKind::DxgiDuplication,
    ] {
        let (x, y) = (initial.x + 20, initial.y + 20);
        set_bench_follow_mouse_position(Some((x, y)));
        let mut hidden_config = config();
        hidden_config.include_cursor = false;
        let hidden = GpuCaptureStream::spawn_follow_mouse(nominal, backend, hidden_config).unwrap();
        let first = gpu_hidden_at(&hidden, x, y);
        let (x, y) = (
            initial.x + initial.width as i32 - 20,
            initial.y + initial.height as i32 - 20,
        );
        set_bench_follow_mouse_position(Some((x, y)));
        let moved = gpu_hidden_at(&hidden, x, y);
        assert!(
            moved.metadata().configuration_generation()
                > first.metadata().configuration_generation()
        );
        if let Some(destination) = layout.monitors.get(1) {
            let (x, y) = (destination.x + 20, destination.y + 20);
            set_bench_follow_mouse_position(Some((x, y)));
            let switched = gpu_hidden_at(&hidden, x, y);
            assert!(
                switched.layers[0]
                    .texture
                    .device()
                    .same_device(hidden.device())
            );
            assert!(hidden.stats().snapshot().follow_monitor_switches >= 1);
        }
        hidden.stop_and_drain();
    }

    // GDI preference must stay on the CPU API and report GDI after panning and
    // handoff. Paused cursor movement cannot prepare or admit another source.
    let (x, y) = (initial.x + 20, initial.y + 20);
    set_bench_follow_mouse_position(Some((x, y)));
    let gdi = CaptureStream::spawn_follow_mouse(
        nominal,
        CaptureBackendKind::Gdi,
        CaptureOptions {
            workload: CaptureWorkload::Continuous,
            ..Default::default()
        },
        config(),
    )
    .unwrap();
    let first = cpu_at(&gdi, x, y);
    assert_eq!(first.metadata().backend_kind(), CaptureBackendKind::Gdi);
    let (x, y) = (
        initial.x + initial.width as i32 - 20,
        initial.y + initial.height as i32 - 20,
    );
    set_bench_follow_mouse_position(Some((x, y)));
    let moved = cpu_at(&gdi, x, y);
    assert_eq!(moved.metadata().backend_kind(), CaptureBackendKind::Gdi);
    assert!(
        moved.metadata().configuration_generation() > first.metadata().configuration_generation()
    );
    gdi.pause();
    std::thread::sleep(Duration::from_millis(30));
    let paused = gdi.stats().snapshot();
    while gdi.try_recv().is_ok() {}
    let destination = layout.monitors.get(1).unwrap_or(initial);
    let (x, y) = (destination.x + 20, destination.y + 20);
    set_bench_follow_mouse_position(Some((x, y)));
    std::thread::sleep(Duration::from_millis(50));
    let still_paused = gdi.stats().snapshot();
    assert_eq!(
        still_paused.follow_prepare_requests,
        paused.follow_prepare_requests
    );
    assert_eq!(
        still_paused.follow_monitor_switches,
        paused.follow_monitor_switches
    );
    assert_eq!(still_paused.frames_captured, paused.frames_captured);
    assert!(gdi.recv_timeout(Duration::from_millis(20)).is_err());
    gdi.resume();
    let resumed = cpu_at(&gdi, x, y);
    assert_eq!(resumed.metadata().backend_kind(), CaptureBackendKind::Gdi);
    if layout.monitors.len() > 1 {
        assert!(gdi.stats().snapshot().follow_monitor_switches >= 1);
    }
    assert!(gdi.stats().snapshot().follow_peak_sources <= 2);
    eprintln!(
        "requested=Gdi, actual={:?}, stats={:?}",
        resumed.metadata().backend_kind(),
        gdi.stats().snapshot()
    );
    gdi.stop_and_drain();
}
