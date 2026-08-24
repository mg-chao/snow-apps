//! End-to-end validation of the opt-in per-stage capture timing
//! instrumentation (`CaptureOptions::record_stage_timings`) for each
//! Windows capture backend.
//!
//! Only built when snow-capture is compiled with the `stage-timing`
//! feature (`required-features` in Cargo.toml); run via
//! `cargo test -p snow-capture --features stage-timing`.
//!
//! Each backend performs one real snapshot capture; the test asserts that
//! stage timings are attached, carry the backend's stage-name prefix, and
//! that the additive primary stages fit inside the session-reported
//! `capture_duration`. Backends unavailable on the machine are skipped
//! with a note instead of failing.

#![cfg(target_os = "windows")]

use std::time::Duration;

use snow_capture::backend::CaptureBackendKind;
use snow_capture::timing::StageTiming;
use snow_capture::{CaptureOptions, CaptureSystem, CaptureTarget};

const SMOKE_TIMEOUT: Duration = Duration::from_secs(20);

fn primary_stages(kind: CaptureBackendKind) -> &'static [&'static str] {
    match kind {
        CaptureBackendKind::DxgiDuplication => &[
            "dxgi.acquire_loop",
            "dxgi.frame_metadata",
            "dxgi.hdr_prepare",
            "dxgi.dirty_eval",
            "dxgi.gpu_copy",
            "readback.map",
            "readback.convert",
        ],
        CaptureBackendKind::WindowsGraphicsCapture => &[
            "wgc.transport_wait",
            "wgc.submit",
            "wgc.readback_wait",
            "readback.map",
            "readback.convert",
        ],
        CaptureBackendKind::Gdi => &[
            "gdi.surface_prepare",
            "gdi.sys.bitblt",
            "readback.frame_alloc",
            "gdi.dirty_scan",
            "gdi.convert",
        ],
        CaptureBackendKind::Auto => &[],
    }
}

fn stage_prefix(kind: CaptureBackendKind) -> &'static str {
    match kind {
        CaptureBackendKind::DxgiDuplication => "dxgi.",
        CaptureBackendKind::WindowsGraphicsCapture => "wgc.",
        CaptureBackendKind::Gdi => "gdi.",
        CaptureBackendKind::Auto => "",
    }
}

/// Some backend stages (shared `readback.*`) intentionally carry no backend
/// prefix; everything else must.
fn has_expected_prefix(kind: CaptureBackendKind, name: &str) -> bool {
    name.starts_with(stage_prefix(kind)) || name.starts_with("readback.")
}

fn primary_sum(kind: CaptureBackendKind, timings: &[StageTiming]) -> Duration {
    let primary = primary_stages(kind);
    timings
        .iter()
        .filter(|timing| primary.contains(&timing.name))
        .map(|timing| timing.duration)
        .sum()
}

fn capture_with_backend(kind: CaptureBackendKind) {
    let context = format!("stage-timing smoke for {}", kind.as_str());

    let system = match CaptureSystem::builder().with_backend_kind(kind).build() {
        Ok(system) => system,
        Err(error) => {
            println!("{context}: backend unavailable ({error}), skipping");
            return;
        }
    };
    let mut session = match system.open_session(
        CaptureTarget::PrimaryMonitor,
        CaptureOptions {
            record_stage_timings: true,
            ..CaptureOptions::default()
        },
    ) {
        Ok(session) => session,
        Err(error) => {
            println!("{context}: session unavailable ({error}), skipping");
            return;
        }
    };

    // First capture may transiently fail while the display settles; retry
    // briefly before deciding the backend is unavailable.
    let mut last_error = None;
    let mut frame = None;
    for _ in 0..3 {
        match session.capture_once() {
            Ok(captured) => {
                frame = Some(captured);
                break;
            }
            Err(error) => {
                last_error = Some(error);
                std::thread::sleep(Duration::from_millis(200));
            }
        }
    }
    let Some(frame) = frame else {
        println!(
            "{context}: capture failed ({:?}), skipping",
            last_error.expect("capture failure recorded")
        );
        return;
    };

    let metadata = frame.metadata();
    assert!(
        frame.width() > 0 && frame.height() > 0,
        "{context}: captured frame has empty dimensions"
    );
    assert_eq!(
        metadata.backend_kind(),
        kind,
        "{context}: frame produced by unexpected backend"
    );

    let timings = metadata.stage_timings();
    assert!(
        !timings.is_empty(),
        "{context}: stage timings missing on captured frame"
    );
    for timing in timings {
        assert!(
            has_expected_prefix(kind, timing.name),
            "{context}: unexpected stage name {}",
            timing.name
        );
        assert!(
            timing.duration <= SMOKE_TIMEOUT,
            "{context}: stage {} reported implausible duration {:?}",
            timing.name,
            timing.duration
        );
    }

    let capture_duration = metadata
        .capture_duration()
        .unwrap_or_else(|| panic!("{context}: capture_duration missing"));
    let primary = primary_sum(kind, timings);
    assert!(
        primary <= capture_duration + Duration::from_millis(1),
        "{context}: primary stages ({primary:?}) exceed capture_duration ({capture_duration:?})"
    );

    println!(
        "{context}: ok ({} stages, capture_duration {capture_duration:?})",
        timings.len()
    );
}

#[test]
fn dxgi_stage_timings_attached_to_snapshot_frames() {
    capture_with_backend(CaptureBackendKind::DxgiDuplication);
}

#[test]
fn wgc_stage_timings_attached_to_snapshot_frames() {
    capture_with_backend(CaptureBackendKind::WindowsGraphicsCapture);
}

#[test]
fn gdi_stage_timings_attached_to_snapshot_frames() {
    capture_with_backend(CaptureBackendKind::Gdi);
}

#[test]
fn stage_timings_absent_when_recording_disabled() {
    let Ok(system) = CaptureSystem::builder()
        .with_backend_kind(CaptureBackendKind::Auto)
        .build()
    else {
        println!("stage-timing disabled smoke: no backend available, skipping");
        return;
    };
    let Ok(mut session) = system.open_session(
        CaptureTarget::PrimaryMonitor,
        CaptureOptions {
            record_stage_timings: false,
            ..CaptureOptions::default()
        },
    ) else {
        println!("stage-timing disabled smoke: session unavailable, skipping");
        return;
    };
    let Ok(frame) = session.capture_once() else {
        println!("stage-timing disabled smoke: capture unavailable, skipping");
        return;
    };
    assert!(
        frame.metadata().stage_timings().is_empty(),
        "stage timings must stay empty when recording is disabled"
    );
}
