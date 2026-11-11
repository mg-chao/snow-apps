//! Functional native acceptance: immutable desktop pixels through capture,
//! encoding and deferred replay. Never inject physical mouse/keyboard input.
#![cfg(all(windows, feature = "bench-synthetic-input"))]

use anyhow::{Context, Result, bail};
use ffmpeg_next as ffmpeg;
use snow_capture::{CaptureSystem, region::MonitorGeometry};
use snow_recording_export::{ExportFormat, VideoCodec};
use snow_recording_model::VideoEncodingSpeed;
use snow_recording_runtime::{
    CaptureBackendKind, DeferredRecordingOptions, DeferredRecordingSession, DirectRecordingConfig,
    DirectRecordingSession, RecordingRegion,
};
use std::{
    path::Path,
    sync::{Arc, Mutex, mpsc},
    thread::{self, JoinHandle},
    time::Duration,
};
use windows::{
    Win32::{
        Foundation::{COLORREF, HWND, LPARAM, LRESULT, RECT, WPARAM},
        Graphics::Gdi::{
            BeginPaint, CreateSolidBrush, DeleteObject, EndPaint, FillRect, InvalidateRect,
            PAINTSTRUCT, UpdateWindow,
        },
        System::LibraryLoader::GetModuleHandleW,
        UI::{
            HiDpi::{DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2, SetProcessDpiAwarenessContext},
            WindowsAndMessaging::*,
        },
    },
    core::w,
};

type Rgb = [u8; 3];
const COLORS: [[Rgb; 2]; 2] = [
    [[220, 40, 40], [40, 220, 40]],
    [[40, 40, 220], [220, 180, 40]],
];
static PANELS: Mutex<Vec<(isize, [Rgb; 2])>> = Mutex::new(Vec::new());

struct SyntheticCursor;
impl SyntheticCursor {
    fn at(point: (i32, i32)) -> Self {
        snow_capture::set_bench_follow_mouse_position(Some(point));
        Self
    }
}
impl Drop for SyntheticCursor {
    fn drop(&mut self) {
        snow_capture::set_bench_follow_mouse_position(None);
    }
}

struct Panel {
    hwnd: isize,
    worker: Option<JoinHandle<()>>,
}
impl Panel {
    fn open(monitor: &MonitorGeometry, colors: [Rgb; 2]) -> Result<Self> {
        let bounds = (monitor.x, monitor.y, monitor.width, monitor.height);
        let (sender, ready) = mpsc::sync_channel(1);
        let worker = thread::spawn(move || {
            let run = || -> Result<()> {
                unsafe {
                    let instance = GetModuleHandleW(None)?;
                    let name = w!("SnowFollowRecordingNativeTest");
                    let class = WNDCLASSW {
                        hInstance: instance.into(),
                        lpfnWndProc: Some(panel_proc),
                        lpszClassName: name,
                        ..Default::default()
                    };
                    RegisterClassW(&class);
                    let hwnd = CreateWindowExW(
                        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                        name,
                        w!("Snow Shot follow recording test"),
                        WS_POPUP | WS_VISIBLE,
                        bounds.0,
                        bounds.1,
                        bounds.2 as i32,
                        bounds.3 as i32,
                        None,
                        None,
                        Some(instance.into()),
                        None,
                    )?;
                    PANELS.lock().unwrap().push((hwnd.0 as isize, colors));
                    let _ = InvalidateRect(Some(hwnd), None, false);
                    let _ = UpdateWindow(hwnd);
                    if sender.send(Ok(hwnd.0 as isize)).is_err() {
                        DestroyWindow(hwnd)?;
                        return Ok(());
                    }
                    let mut message = MSG::default();
                    while GetMessageW(&mut message, None, 0, 0).as_bool() {
                        let _ = TranslateMessage(&message);
                        DispatchMessageW(&message);
                    }
                }
                Ok(())
            };
            if let Err(error) = run() {
                let _ = sender.send(Err(error.to_string()));
            }
        });
        let hwnd = ready
            .recv_timeout(Duration::from_secs(5))?
            .map_err(anyhow::Error::msg)?;
        Ok(Self {
            hwnd,
            worker: Some(worker),
        })
    }
}
impl Drop for Panel {
    fn drop(&mut self) {
        let _ = unsafe {
            PostMessageW(
                Some(HWND(self.hwnd as *mut _)),
                WM_CLOSE,
                WPARAM(0),
                LPARAM(0),
            )
        };
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}

unsafe extern "system" fn panel_proc(hwnd: HWND, message: u32, wp: WPARAM, lp: LPARAM) -> LRESULT {
    unsafe {
        match message {
            WM_PAINT => {
                let colors = PANELS
                    .lock()
                    .unwrap()
                    .iter()
                    .find(|(handle, _)| *handle == hwnd.0 as isize)
                    .map(|(_, colors)| *colors);
                let mut paint = PAINTSTRUCT::default();
                let dc = BeginPaint(hwnd, &mut paint);
                let mut bounds = RECT::default();
                let _ = GetClientRect(hwnd, &mut bounds);
                if let Some(colors) = colors {
                    let background = CreateSolidBrush(COLORREF(0));
                    FillRect(dc, &bounds, background);
                    let _ = DeleteObject(background.into());
                    for (index, color) in colors.into_iter().enumerate() {
                        // A small marker at the exact synthetic pointer location
                        // verifies centering, including mixed-DPI desktop origins.
                        let x = (2 * index as i32 + 1) * bounds.right / 4;
                        let y = bounds.bottom / 2;
                        let rect = RECT {
                            left: x - 8,
                            right: x + 8,
                            top: y - 8,
                            bottom: y + 8,
                        };
                        let brush = CreateSolidBrush(COLORREF(
                            u32::from(color[0])
                                | (u32::from(color[1]) << 8)
                                | (u32::from(color[2]) << 16),
                        ));
                        FillRect(dc, &rect, brush);
                        let _ = DeleteObject(brush.into());
                    }
                }
                let _ = EndPaint(hwnd, &paint);
                LRESULT(0)
            }
            WM_DESTROY => {
                PostQuitMessage(0);
                LRESULT(0)
            }
            WM_NCDESTROY => {
                PANELS
                    .lock()
                    .unwrap()
                    .retain(|(handle, _)| *handle != hwnd.0 as isize);
                DefWindowProcW(hwnd, message, wp, lp)
            }
            _ => DefWindowProcW(hwnd, message, wp, lp),
        }
    }
}

fn config(path: &Path, initial: (i32, i32)) -> DirectRecordingConfig {
    DirectRecordingConfig {
        follow_mouse: true,
        follow_mouse_options: snow_media::follow_mouse::FollowMouseOptions {
            smooth_movement: true,
            reduce_jitter: true,
        },
        region: RecordingRegion::new(initial.0 - 160, initial.1 - 90, 321, 181),
        output_path: path.into(),
        capture_backend: CaptureBackendKind::Gdi,
        format: ExportFormat::Mp4,
        capture_fps: 15,
        output_fps: 15,
        maximum_width: None,
        maximum_height: None,
        codec: VideoCodec::H264,
        preset: VideoEncodingSpeed::VeryFast,
        quality: 95,
        prefer_hardware_encoder: false,
        enable_microphone: false,
        enable_system_audio: false,
        audio_mode: Default::default(),
        system_audio_gain_db: 0,
        microphone_gain_db: 0,
        show_cursor: false,
        show_keyboard: false,
        record_mouse_clicks: false,
        record_mouse_scrolling: false,
        keyboard: None,
        keyboard_filter: Default::default(),
        mouse_trail_rgba: [0; 4],
        mouse_trail_duration_ms: 500,
        mouse_click_rgba: [0; 4],
        mouse_highlight_radius: 32,
        mouse_highlight_rgba: [0; 4],
        webcam: None,
        loop_animated_images: false,
        excluded_windows: Arc::from([]),
        excluded_processes: Arc::from([]),
    }
}

fn drive(points: &[(i32, i32)]) {
    thread::sleep(Duration::from_millis(500));
    for point in &points[1..] {
        snow_capture::set_bench_follow_mouse_position(Some(*point));
        thread::sleep(Duration::from_millis(1000));
    }
}

struct Decoded {
    colors: Vec<Rgb>,
    pts_ms: Vec<i64>,
    decode_spans_ms: Vec<(i64, i64)>,
    end_ms: i64,
}
fn decode(path: &Path, size: (u32, u32)) -> Result<Decoded> {
    let mut input = ffmpeg::format::input(path)?;
    let stream = input
        .streams()
        .best(ffmpeg::media::Type::Video)
        .context("missing video")?;
    let index = stream.index();
    let time_base = stream.time_base();
    use ffmpeg::Rescale;
    let end_ms = stream.duration().rescale(time_base, (1, 1000));
    let mut decoder = ffmpeg::codec::context::Context::from_parameters(stream.parameters())?
        .decoder()
        .video()?;
    assert_eq!(
        (decoder.width(), decoder.height()),
        size,
        "fixed encoded size"
    );
    let mut scaler = ffmpeg::software::scaling::Context::get(
        decoder.format(),
        size.0,
        size.1,
        ffmpeg::format::Pixel::RGB24,
        size.0,
        size.1,
        ffmpeg::software::scaling::flag::Flags::BILINEAR,
    )?;
    let mut decoded = Decoded {
        colors: Vec::new(),
        pts_ms: Vec::new(),
        decode_spans_ms: Vec::new(),
        end_ms,
    };
    let mut receive = |decoder: &mut ffmpeg::decoder::Video| -> Result<()> {
        let mut frame = ffmpeg::util::frame::Video::empty();
        while decoder.receive_frame(&mut frame).is_ok() {
            unsafe {
                let matrix = ffmpeg::ffi::sws_getCoefficients(frame.color_space() as i32);
                let status = ffmpeg::ffi::sws_setColorspaceDetails(
                    scaler.as_mut_ptr(),
                    matrix,
                    i32::from(frame.color_range() == ffmpeg::color::Range::JPEG),
                    matrix,
                    1,
                    0,
                    1 << 16,
                    1 << 16,
                );
                if status < 0 {
                    bail!("decoded RGB colorspace configuration failed: {status}");
                }
            }
            let mut rgb = ffmpeg::util::frame::Video::empty();
            scaler.run(&frame, &mut rgb)?;
            let offset = (size.1 as usize / 2) * rgb.stride(0) + size.0 as usize / 2 * 3;
            decoded
                .colors
                .push(rgb.data(0)[offset..offset + 3].try_into().unwrap());
            let pts = frame.timestamp().context("missing decoded timestamp")?;
            decoded.pts_ms.push(pts.rescale(time_base, (1, 1000)));
        }
        Ok(())
    };
    for (stream, packet) in input.packets() {
        if stream.index() == index {
            // MP4 stts durations describe decode order. With B frames,
            // PTS + packet.duration is not a presentation hold boundary.
            let dts = packet.dts().context("missing encoded DTS")?;
            let start = dts.rescale(time_base, (1, 1000));
            let end = (dts + packet.duration()).rescale(time_base, (1, 1000));
            decoded.decode_spans_ms.push((start, end));
            decoder.send_packet(&packet)?;
            receive(&mut decoder)?;
        }
    }
    decoder.send_eof()?;
    receive(&mut decoder)?;
    Ok(decoded)
}

fn assert_timeline_and_views(decoded: &Decoded, expected: &[Rgb], label: &str) {
    assert!(!decoded.pts_ms.is_empty(), "{label}: no decoded frames");
    assert!(
        decoded.pts_ms.windows(2).all(|pair| pair[1] > pair[0]),
        "{label}: PTS regression"
    );
    assert!(decoded.pts_ms[0] <= 100, "{label}: initial timeline gap");
    let nominal_ms = 500 + (expected.len() as i64 - 1) * 1000;
    assert!(
        (nominal_ms - 250..=nominal_ms + 500).contains(&decoded.end_ms),
        "{label}: capture clock lost during handoff: {}ms",
        decoded.end_ms
    );
    let mut spans = decoded.decode_spans_ms.clone();
    spans.sort_unstable();
    for pair in spans.windows(2) {
        assert!(
            pair[1].0 <= pair[0].1 + 2,
            "{label}: decode timeline gap across a held view/handoff: {pair:?}"
        );
    }
    let mut next = 0;
    for color in &decoded.colors {
        if expected.get(next).is_some_and(|expected| {
            color
                .iter()
                .zip(expected)
                .all(|(actual, expected)| actual.abs_diff(*expected) <= 18)
        }) {
            next += 1;
        }
    }
    assert_eq!(
        next,
        expected.len(),
        "{label}: missing/reordered static views; decoded {:?}",
        decoded.colors
    );
}

#[test]
#[ignore = "requires an unlocked interactive Windows desktop; opens nonactivating test panels"]
fn native_follow_direct_and_deferred_decode_static_pans_and_monitor_handoff() -> Result<()> {
    unsafe {
        let _ = SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }
    ffmpeg::init()?;
    let system = CaptureSystem::builder().build()?;
    let mut monitors = system.monitor_layout()?.monitors;
    monitors.retain(|monitor| monitor.width >= 800 && monitor.height >= 400);
    monitors.sort_by_key(|monitor| (monitor.x, monitor.y));
    monitors.truncate(2);
    if monitors.is_empty() {
        eprintln!("SKIP: no native monitor large enough for odd-source acceptance");
        return Ok(());
    }
    let _panels = monitors
        .iter()
        .enumerate()
        .map(|(i, monitor)| Panel::open(monitor, COLORS[i]))
        .collect::<Result<Vec<_>>>()?;
    let left = &monitors[0];
    let initial = (
        left.x + left.width as i32 / 4,
        left.y + left.height as i32 / 2,
    );
    let mut points = vec![initial, (left.x + left.width as i32 * 3 / 4, initial.1)];
    let mut expected = vec![COLORS[0][0], COLORS[0][1]];
    if let Some(right) = monitors.get(1) {
        points.push((
            right.x + right.width as i32 / 4,
            right.y + right.height as i32 / 2,
        ));
        expected.push(COLORS[1][0]);
    } else {
        eprintln!("SKIP monitor-switch subcase: one native monitor; static panning still verified");
    }
    let _cursor = SyntheticCursor::at(initial);
    thread::sleep(Duration::from_millis(150));
    let temporary = tempfile::tempdir()?;
    let directory = temporary.path().to_path_buf();
    let _cleanup = if std::env::var_os("SNOW_TEST_KEEP_FOLLOW").is_some() {
        let _ = temporary.keep();
        eprintln!("Retaining follow acceptance media: {}", directory.display());
        None
    } else {
        Some(temporary)
    };
    let direct_path = directory.join("follow-direct.mp4");
    let direct_config = config(&direct_path, initial);
    let size =
        snow_recording_export::scaled_output_dimensions(321, 181, None, None, ExportFormat::Mp4);
    let mut direct = DirectRecordingSession::create(direct_config)?;
    direct.start().context("native direct startup")?;
    drive(&points);
    let report = direct.stop()?;
    if points.len() > 2 {
        assert!(
            report.follow_monitor_switches >= 1,
            "no ready monitor switch"
        );
    }
    let direct = decode(&direct_path, size)?;
    assert_timeline_and_views(&direct, &expected, "direct");

    snow_capture::set_bench_follow_mouse_position(Some(initial));
    let deferred_path = directory.join("follow-deferred.mp4");
    let mut deferred = DeferredRecordingSession::create(
        config(&deferred_path, initial),
        DeferredRecordingOptions {
            working_directory: Some(directory.join("sources")),
            ..Default::default()
        },
    )?;
    deferred.start().context("native deferred startup")?;
    drive(&points);
    let source = deferred.stop()?;
    if std::env::var_os("SNOW_TEST_KEEP_FOLLOW").is_some() {
        let retained = directory.join("source-before-render.snowrec");
        std::fs::copy(source.path(), &retained)?;
        eprintln!("Retained clean source: {}", retained.display());
    }
    source
        .render_async()?
        .wait()
        .context("deferred rendering")?;
    let rendered = decode(&deferred_path, size)?;
    assert_timeline_and_views(&rendered, &expected, "deferred");
    // Both independently encoded paths must reproduce the same desktop colors.
    for expected in &expected {
        let best = |frames: &[Rgb]| {
            *frames
                .iter()
                .min_by_key(|color| {
                    color
                        .iter()
                        .zip(expected)
                        .map(|(actual, expected)| u32::from(actual.abs_diff(*expected)))
                        .sum::<u32>()
                })
                .unwrap()
        };
        if best(&direct.colors)
            .iter()
            .zip(best(&rendered.colors))
            .any(|(a, b)| a.abs_diff(b) > 15)
        {
            bail!("direct/deferred color parity differs for {expected:?}");
        }
    }
    source.discard()?;
    eprintln!(
        "Verified direct+deferred {size:?}, {} static views, {} monitors, PTS ends {}/{}ms",
        expected.len(),
        monitors.len(),
        direct.end_ms,
        rendered.end_ms
    );
    Ok(())
}
