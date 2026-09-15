#![cfg(windows)]

use snow_capture::backend::CaptureBackendKind;
use snow_capture::{CaptureOptions, CaptureSystem, CaptureTarget, WindowId};
use std::time::Duration;
use windows::Win32::Foundation::{COLORREF, HWND};
use windows::Win32::Graphics::Gdi::{
    CreateSolidBrush, DeleteObject, FillRect, GdiFlush, GetDC, ReleaseDC,
};
use windows::Win32::UI::Magnification::{
    MAGCOLOREFFECT, MagGetFullscreenColorEffect, MagInitialize, MagSetFullscreenColorEffect,
    MagUninitialize,
};
use windows::Win32::UI::WindowsAndMessaging::{
    CreateWindowExW, DestroyWindow, DispatchMessageW, MSG, PM_REMOVE, PeekMessageW, SW_SHOW,
    ShowWindow, TranslateMessage, WS_EX_TOPMOST, WS_POPUP,
};
use windows::core::w;

struct DesktopGuard(MAGCOLOREFFECT, HWND);

fn capture_settled<T>(
    hwnd: HWND,
    mut capture: impl FnMut() -> Result<T, snow_capture::error::CaptureError>,
) -> Result<T, snow_capture::error::CaptureError> {
    for attempt in 0..5 {
        paint_reference(hwnd);
        match capture() {
            Err(snow_capture::error::CaptureError::Timeout) if attempt < 4 => {
                std::thread::sleep(Duration::from_millis(100));
            }
            result => return result,
        }
    }
    unreachable!()
}

fn paint_reference(hwnd: HWND) {
    unsafe {
        // Service this thread's window so Windows does not replace it with a hung-window ghost.
        let mut message = MSG::default();
        while PeekMessageW(&mut message, None, 0, 0, PM_REMOVE).as_bool() {
            let _ = TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        let dc = GetDC(Some(hwnd));
        let brush = CreateSolidBrush(COLORREF(0x00604020));
        let rect = windows::Win32::Foundation::RECT {
            left: 8,
            top: 8,
            right: 256,
            bottom: 256,
        };
        FillRect(dc, &rect, brush);
        let bright = CreateSolidBrush(COLORREF(0x0060a0e0));
        FillRect(
            dc,
            &windows::Win32::Foundation::RECT {
                left: 180,
                top: 180,
                right: 256,
                bottom: 256,
            },
            bright,
        );
        let _ = DeleteObject(bright.into());
        let _ = GdiFlush();
        let _ = ReleaseDC(Some(hwnd), dc);
        let _ = DeleteObject(brush.into());
    }
}

struct RepaintGuard(
    std::sync::Arc<std::sync::atomic::AtomicBool>,
    Option<std::thread::JoinHandle<()>>,
);

impl RepaintGuard {
    fn start(hwnd: HWND) -> Self {
        let stopped = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
        let worker_stopped = stopped.clone();
        let handle = hwnd.0 as usize;
        let thread = std::thread::spawn(move || {
            let hwnd = HWND(handle as *mut _);
            let mut tick = 0u32;
            while !worker_stopped.load(std::sync::atomic::Ordering::Relaxed) {
                // Keep duplication/WGC producing frames without changing the sampled pixel.
                unsafe {
                    let dc = GetDC(Some(hwnd));
                    let brush = CreateSolidBrush(COLORREF(tick & 255));
                    let rect = windows::Win32::Foundation::RECT {
                        left: 0,
                        top: 0,
                        right: 8,
                        bottom: 8,
                    };
                    FillRect(dc, &rect, brush);
                    let _ = GdiFlush();
                    let _ = ReleaseDC(Some(hwnd), dc);
                    let _ = DeleteObject(brush.into());
                }
                tick = tick.wrapping_add(1);
                std::thread::sleep(Duration::from_millis(16));
            }
        });
        Self(stopped, Some(thread))
    }
}

impl Drop for RepaintGuard {
    fn drop(&mut self) {
        self.0.store(true, std::sync::atomic::Ordering::Relaxed);
        if let Some(thread) = self.1.take() {
            let _ = thread.join();
        }
    }
}

struct SdrGuard(Vec<windows::Win32::Devices::Display::DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE>);

impl SdrGuard {
    fn enter() -> anyhow::Result<Self> {
        use windows::Win32::Devices::Display::*;
        let mut guard = Self(Vec::new());
        if std::env::var_os("SNOW_CAPTURE_TEST_SDR").is_none() {
            return Ok(guard);
        }
        let mut path_count = 0;
        let mut mode_count = 0;
        unsafe {
            GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &mut path_count, &mut mode_count)
        }
        .ok()?;
        let mut paths = vec![DISPLAYCONFIG_PATH_INFO::default(); path_count as usize];
        let mut modes = vec![DISPLAYCONFIG_MODE_INFO::default(); mode_count as usize];
        unsafe {
            QueryDisplayConfig(
                QDC_ONLY_ACTIVE_PATHS,
                &mut path_count,
                paths.as_mut_ptr(),
                &mut mode_count,
                modes.as_mut_ptr(),
                None,
            )
        }
        .ok()?;
        for path in paths.iter().take(path_count as usize) {
            let mut info = DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO {
                header: DISPLAYCONFIG_DEVICE_INFO_HEADER {
                    r#type: DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO,
                    size: std::mem::size_of::<DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO>() as u32,
                    adapterId: path.targetInfo.adapterId,
                    id: path.targetInfo.id,
                },
                ..Default::default()
            };
            if unsafe { DisplayConfigGetDeviceInfo(&mut info.header) } == 0
                && unsafe { info.Anonymous.value } & 2 != 0
            {
                let mut state = DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE {
                    header: DISPLAYCONFIG_DEVICE_INFO_HEADER {
                        r#type: DISPLAYCONFIG_DEVICE_INFO_SET_ADVANCED_COLOR_STATE,
                        size: std::mem::size_of::<DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE>() as u32,
                        adapterId: path.targetInfo.adapterId,
                        id: path.targetInfo.id,
                    },
                    ..Default::default()
                };
                anyhow::ensure!(
                    unsafe { DisplayConfigSetDeviceInfo(&state.header) } == 0,
                    "could not temporarily select SDR"
                );
                state.Anonymous.value = 1;
                guard.0.push(state);
            }
        }
        std::thread::sleep(Duration::from_millis(1500));
        Ok(guard)
    }
}

impl Drop for SdrGuard {
    fn drop(&mut self) {
        for state in &self.0 {
            unsafe {
                let _ = windows::Win32::Devices::Display::DisplayConfigSetDeviceInfo(&state.header);
            }
        }
    }
}

impl Drop for DesktopGuard {
    fn drop(&mut self) {
        unsafe {
            let _ = MagSetFullscreenColorEffect(&self.0);
            let _ = DestroyWindow(self.1);
            let _ = MagUninitialize();
        }
    }
}

/// Interactive characterization only: temporarily changes the global Magnifier effect.
#[test]
#[ignore = "requires an interactive Windows desktop and temporarily changes its color effect"]
fn characterize_magnifier_capture() -> anyhow::Result<()> {
    let _sdr = SdrGuard::enter()?;
    unsafe { MagInitialize() }.ok()?;
    let mut previous = MAGCOLOREFFECT::default();
    unsafe { MagGetFullscreenColorEffect(&mut previous) }.ok()?;
    let mut guard = DesktopGuard(previous, HWND::default());
    let hwnd = unsafe {
        CreateWindowExW(
            WS_EX_TOPMOST,
            w!("STATIC"),
            w!("Snow capture color test"),
            WS_POPUP,
            100,
            100,
            256,
            256,
            None,
            None,
            None,
            None,
        )
    }?;
    guard.1 = hwnd;
    unsafe {
        let _ = ShowWindow(hwnd, SW_SHOW);
    }
    let _repaint = RepaintGuard::start(hwnd);
    let identity = MAGCOLOREFFECT {
        transform: [
            1., 0., 0., 0., 0., 0., 1., 0., 0., 0., 0., 0., 1., 0., 0., 0., 0., 0., 1., 0., 0., 0.,
            0., 0., 1.,
        ],
    };
    let inverse = MAGCOLOREFFECT {
        transform: [
            -1., 0., 0., 0., 0., 0., -1., 0., 0., 0., 0., 0., -1., 0., 0., 0., 0., 0., 1., 0., 1.,
            1., 1., 0., 1.,
        ],
    };
    let mut cycle = identity;
    cycle.transform[0] = 0.;
    cycle.transform[6] = 0.;
    cycle.transform[12] = 0.;
    cycle.transform[2] = 1.;
    cycle.transform[5] = 1.;
    cycle.transform[11] = 1.;
    let mut grayscale = identity;
    grayscale.transform[1] = 1.;
    grayscale.transform[2] = 1.;
    grayscale.transform[6] = 0.;
    grayscale.transform[12] = 0.;
    let mut baselines = std::collections::HashMap::new();
    for (name, effect) in [
        ("identity", identity),
        ("inverted", inverse),
        ("cycle", cycle),
        ("grayscale", grayscale),
    ] {
        unsafe { MagSetFullscreenColorEffect(&effect) }.ok()?;
        let dc = unsafe { GetDC(Some(hwnd)) };
        let brush = unsafe { CreateSolidBrush(COLORREF(0x00604020)) };
        let rect = windows::Win32::Foundation::RECT {
            left: 0,
            top: 0,
            right: 256,
            bottom: 256,
        };
        unsafe {
            FillRect(dc, &rect, brush);
            let _ = ReleaseDC(Some(hwnd), dc);
            let _ = DeleteObject(brush.into());
        }
        std::thread::sleep(Duration::from_millis(300));
        for backend in [
            CaptureBackendKind::Gdi,
            CaptureBackendKind::DxgiDuplication,
            CaptureBackendKind::WindowsGraphicsCapture,
        ] {
            for window in [false, true] {
                for correct in [false, true] {
                    let target = if window {
                        CaptureTarget::Window(WindowId::from_windows_handle(hwnd.0 as isize))
                    } else {
                        CaptureTarget::PrimaryMonitor
                    };
                    let system = CaptureSystem::builder()
                        .with_backend_kind(backend)
                        .build()?;
                    let options = CaptureOptions {
                        backend_tuning: snow_capture::tuning::BackendTuning::Windows(
                            snow_capture::tuning::windows::WindowsCaptureOptions {
                                gpu_hdr_conversion: std::env::var_os("SNOW_CAPTURE_TEST_CPU_HDR")
                                    .is_none(),
                                color_correction: if correct {
                                    snow_capture::color_effect::ColorCorrection::CurrentMagnifier
                                } else {
                                    snow_capture::color_effect::ColorCorrection::Disabled
                                },
                                ..Default::default()
                            },
                        ),
                        capture_retry_count: 3,
                        ..Default::default()
                    };
                    let mut session = system.open_session(target, options)?;
                    paint_reference(hwnd);
                    let result = capture_settled(hwnd, || session.capture());
                    match result {
                        Ok(frame) => {
                            let coordinate = if window { 128 } else { 228 };
                            let offset = (coordinate * frame.width() as usize + coordinate) * 4;
                            let bright_coordinate = if window { 200 } else { 300 };
                            let bright_offset = (bright_coordinate * frame.width() as usize
                                + bright_coordinate)
                                * 4;
                            let key = (backend, window);
                            if name == "identity" && !correct {
                                baselines.insert(
                                    key,
                                    [
                                        <[u8; 4]>::try_from(
                                            &frame.as_rgba_bytes()[offset..offset + 4],
                                        )
                                        .unwrap(),
                                        <[u8; 4]>::try_from(
                                            &frame.as_rgba_bytes()
                                                [bright_offset..bright_offset + 4],
                                        )
                                        .unwrap(),
                                    ],
                                );
                            }
                            let cpu_hdr = backend != CaptureBackendKind::Gdi
                                && std::env::var_os("SNOW_CAPTURE_TEST_CPU_HDR").is_some()
                                && std::env::var_os("SNOW_CAPTURE_TEST_SDR").is_none();
                            println!(
                                "{name} {backend:?} window={window} correct={correct}: {:?}",
                                &frame.as_rgba_bytes()[offset..offset + 4]
                            );
                            if backend != CaptureBackendKind::DxgiDuplication
                                || std::env::var_os("SNOW_CAPTURE_TEST_SDR").is_some()
                                || correct
                            {
                                let filtered = (backend == CaptureBackendKind::Gdi && !window)
                                    || backend == CaptureBackendKind::DxgiDuplication;
                                let mut expected = match (name, filtered, correct) {
                                    ("inverted", true, false) => [223, 191, 159, 255],
                                    ("cycle", true, false) => [64, 96, 32, 255],
                                    ("grayscale", true, _) => [32, 32, 32, 255],
                                    _ => [32, 64, 96, 255],
                                };
                                if cpu_hdr && (correct || !filtered) {
                                    expected = baselines[&key][0];
                                    if name == "grayscale" && filtered {
                                        expected = [expected[0], expected[0], expected[0], 255];
                                    }
                                }
                                for (actual, expected) in frame.as_rgba_bytes()[offset..offset + 4]
                                    .iter()
                                    .zip(expected)
                                {
                                    assert!(
                                        actual.abs_diff(expected) <= 1,
                                        "{name} {backend:?} window={window} correct={correct}"
                                    );
                                }
                                let coordinate = if window { 200 } else { 300 };
                                let offset = (coordinate * frame.width() as usize + coordinate) * 4;
                                let mut expected = match (name, filtered, correct) {
                                    ("inverted", true, false) => [31, 95, 159, 255],
                                    ("cycle", true, false) => [160, 96, 224, 255],
                                    ("grayscale", true, _) => [224, 224, 224, 255],
                                    _ => [224, 160, 96, 255],
                                };
                                if cpu_hdr && (correct || !filtered) {
                                    expected = baselines[&key][1];
                                    if name == "grayscale" && filtered {
                                        expected = [expected[0], expected[0], expected[0], 255];
                                    }
                                }
                                for (actual, expected) in frame.as_rgba_bytes()[offset..offset + 4]
                                    .iter()
                                    .zip(expected)
                                {
                                    assert!(
                                        actual.abs_diff(expected) <= 1,
                                        "bright SDR {name} {backend:?} window={window} correct={correct}: {:?}",
                                        &frame.as_rgba_bytes()[offset..offset + 4]
                                    );
                                }
                            }
                        }
                        Err(error) => anyhow::bail!("{name} {backend:?} window={window}: {error}"),
                    }
                    if !window && std::env::var_os("SNOW_CAPTURE_TEST_PERF").is_some() {
                        let mut timings = Vec::new();
                        for i in 0..35 {
                            paint_reference(hwnd);
                            let begin = std::time::Instant::now();
                            let frame = session.capture_once()?;
                            std::hint::black_box(frame.as_rgba_bytes());
                            if i >= 5 {
                                timings.push(begin.elapsed().as_secs_f64() * 1000.);
                            }
                        }
                        timings.sort_by(f64::total_cmp);
                        println!(
                            "PERF {name} {backend:?} correct={correct} p50={:.3}ms p95={:.3}ms",
                            timings[15], timings[28]
                        );
                    }
                }
            }
        }
    }
    // Reuse the same region session across effect changes and duplicate frames.
    for backend in [
        CaptureBackendKind::Gdi,
        CaptureBackendKind::DxgiDuplication,
        CaptureBackendKind::WindowsGraphicsCapture,
    ] {
        let system = CaptureSystem::builder()
            .with_backend_kind(backend)
            .build()?;
        let mut session = system.open_session(
            CaptureTarget::Region(snow_capture::CaptureRegion::new(110, 110, 128, 128)?),
            CaptureOptions {
                backend_tuning: snow_capture::tuning::BackendTuning::Windows(
                    snow_capture::tuning::windows::WindowsCaptureOptions {
                        gpu_hdr_conversion: std::env::var_os("SNOW_CAPTURE_TEST_CPU_HDR").is_none(),
                        color_correction:
                            snow_capture::color_effect::ColorCorrection::CurrentMagnifier,
                        ..Default::default()
                    },
                ),
                capture_retry_count: 3,
                workload: snow_capture::CaptureWorkload::Continuous,
                ..Default::default()
            },
        )?;
        let mut frame = snow_capture::frame::Frame::empty();
        let mut baseline = None;
        for effect in [identity, inverse, cycle, identity] {
            unsafe { MagSetFullscreenColorEffect(&effect) }.ok()?;
            std::thread::sleep(Duration::from_millis(250));
            for _ in 0..3 {
                paint_reference(hwnd);
                capture_settled(hwnd, || session.capture_into(&mut frame))
                    .map_err(|error| anyhow::anyhow!("region capture {backend:?}: {error}"))?;
                let offset = (64 * 128 + 64) * 4;
                let pixel =
                    <[u8; 4]>::try_from(&frame.as_rgba_bytes()[offset..offset + 4]).unwrap();
                let expected = *baseline.get_or_insert(pixel);
                assert_eq!(
                    pixel, expected,
                    "region capture {backend:?} must correct each source exactly once"
                );
            }
        }
    }
    Ok(())
}
