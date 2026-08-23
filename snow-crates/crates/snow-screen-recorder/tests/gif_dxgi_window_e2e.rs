#![cfg(target_os = "windows")]

use std::ffi::c_void;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::mpsc;
use std::thread::{self, JoinHandle};
use std::time::Duration;

use ffmpeg_next as ffmpeg;
use snow_screen_recorder::{
    CaptureBackendKind, EditingSession, ExportFormat, RecordingAudioConfig,
    RecordingAudioTrackConfig, RecordingConfig, RecordingRegion, RecordingSession, RecordingTarget,
    WindowSelector,
};
use tempfile::tempdir;
use windows::Win32::Foundation::{COLORREF, HINSTANCE, HWND, LPARAM, LRESULT, POINT, RECT, WPARAM};
use windows::Win32::Graphics::Gdi::{
    BeginPaint, CreateSolidBrush, DeleteObject, EndPaint, FillRect, PAINTSTRUCT,
};
use windows::Win32::System::LibraryLoader::GetModuleHandleW;
use windows::Win32::UI::HiDpi::{
    DPI_AWARENESS_CONTEXT, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2, SetThreadDpiAwarenessContext,
};
use windows::Win32::UI::WindowsAndMessaging::{
    CREATESTRUCTW, CreateWindowExW, DefWindowProcW, DestroyWindow, DispatchMessageW, GWLP_USERDATA,
    GetCursorPos, GetMessageW, KillTimer, MSG, PostMessageW, PostQuitMessage, RegisterClassW,
    SetCursorPos, SetTimer, SetWindowLongPtrW, ShowWindow, TranslateMessage, WM_CLOSE, WM_CREATE,
    WM_DESTROY, WM_NCCREATE, WM_PAINT, WM_TIMER, WNDCLASSW, WS_EX_TOOLWINDOW, WS_EX_TOPMOST,
    WS_POPUP, WS_VISIBLE,
};
use windows::core::PCWSTR;

const WINDOW_SIZE: i32 = 100;
const WINDOW_X: i32 = 64;
const WINDOW_Y: i32 = 64;
const TIMER_ID: usize = 1;
const TIMER_INTERVAL_MS: u32 = 80;
const COLORS: [COLORREF; 4] = [
    COLORREF(0x0000_00ff),
    COLORREF(0x0000_ff00),
    COLORREF(0x00ff_0000),
    COLORREF(0x0000_ffff),
];
const EXPECTED_RGB: [[u8; 3]; 4] = [
    [0xff, 0x00, 0x00],
    [0x00, 0xff, 0x00],
    [0x00, 0x00, 0xff],
    [0xff, 0xff, 0x00],
];
const SPARSE_BACKGROUND_RGB: [u8; 3] = [0xf0, 0xf0, 0xf0];
const SPARSE_BLOCK_RGB: [u8; 3] = [0xff, 0x00, 0x00];
const SPARSE_BLOCK_SIZE: i32 = 18;
const SPARSE_BLOCK_Y: i32 = 41;
const SPARSE_BLOCK_X: [i32; 3] = [8, 41, 74];
const LARGE_WINDOW_WIDTH: i32 = 1280;
const LARGE_WINDOW_HEIGHT: i32 = 720;
const LARGE_WINDOW_X: i32 = 128;
const LARGE_WINDOW_Y: i32 = 96;
const LARGE_BLOCK_SIZE: i32 = 96;
const LARGE_BLOCK_Y: i32 = 312;
const LARGE_BLOCK_X: [i32; 3] = [64, 592, 1120];
const COLOR_CHANNEL_TOLERANCE: u8 = 64;
const MIN_SOLID_COLOR_RATIO: f64 = 0.9;
static NEXT_WINDOW_CLASS_ID: AtomicU64 = AtomicU64::new(1);

struct ThreadDpiAwareness(DPI_AWARENESS_CONTEXT);

impl ThreadDpiAwareness {
    fn per_monitor_v2() -> Self {
        Self(unsafe { SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) })
    }
}

impl Drop for ThreadDpiAwareness {
    fn drop(&mut self) {
        unsafe {
            SetThreadDpiAwarenessContext(self.0);
        }
    }
}

#[derive(Clone, Copy)]
enum WindowPattern {
    Solid,
    SparseMovingBlock(SparsePattern),
}

#[derive(Clone, Copy)]
struct SparsePattern {
    block_size: i32,
    block_y: i32,
    block_x: [i32; 3],
}

#[derive(Clone, Copy)]
struct TestWindowSpec {
    x: i32,
    y: i32,
    width: i32,
    height: i32,
    pattern: WindowPattern,
}

struct ColorWindowState {
    color_index: usize,
    block_position: usize,
    pattern: WindowPattern,
}

struct ColorWindow {
    raw_handle: isize,
    thread: Option<JoinHandle<()>>,
}

impl ColorWindow {
    fn spawn() -> Self {
        Self::spawn_with_spec(TestWindowSpec {
            x: WINDOW_X,
            y: WINDOW_Y,
            width: WINDOW_SIZE,
            height: WINDOW_SIZE,
            pattern: WindowPattern::Solid,
        })
    }

    fn spawn_sparse() -> Self {
        Self::spawn_with_spec(TestWindowSpec {
            x: WINDOW_X,
            y: WINDOW_Y,
            width: WINDOW_SIZE,
            height: WINDOW_SIZE,
            pattern: WindowPattern::SparseMovingBlock(SparsePattern {
                block_size: SPARSE_BLOCK_SIZE,
                block_y: SPARSE_BLOCK_Y,
                block_x: SPARSE_BLOCK_X,
            }),
        })
    }

    fn spawn_large_sparse() -> Self {
        Self::spawn_with_spec(TestWindowSpec {
            x: LARGE_WINDOW_X,
            y: LARGE_WINDOW_Y,
            width: LARGE_WINDOW_WIDTH,
            height: LARGE_WINDOW_HEIGHT,
            pattern: WindowPattern::SparseMovingBlock(SparsePattern {
                block_size: LARGE_BLOCK_SIZE,
                block_y: LARGE_BLOCK_Y,
                block_x: LARGE_BLOCK_X,
            }),
        })
    }

    fn spawn_with_spec(spec: TestWindowSpec) -> Self {
        let (handle_tx, handle_rx) = mpsc::sync_channel(1);
        let thread = thread::spawn(move || {
            let _dpi_awareness = ThreadDpiAwareness::per_monitor_v2();
            let instance = unsafe { GetModuleHandleW(None).expect("module handle should resolve") };
            let class_id = NEXT_WINDOW_CLASS_ID.fetch_add(1, Ordering::Relaxed);
            let class_name: Vec<u16> = format!(
                "SnowShotRecordingColorWindow{}_{}\0",
                std::process::id(),
                class_id
            )
            .encode_utf16()
            .collect();
            let class_name = PCWSTR(class_name.as_ptr());
            let class = WNDCLASSW {
                hInstance: HINSTANCE(instance.0),
                lpszClassName: class_name,
                lpfnWndProc: Some(color_window_proc),
                ..Default::default()
            };
            assert_ne!(unsafe { RegisterClassW(&class) }, 0);

            let state = Box::into_raw(Box::new(ColorWindowState {
                color_index: 0,
                block_position: 0,
                pattern: spec.pattern,
            }));
            let hwnd = unsafe {
                CreateWindowExW(
                    WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                    class_name,
                    class_name,
                    WS_POPUP | WS_VISIBLE,
                    spec.x,
                    spec.y,
                    spec.width,
                    spec.height,
                    None,
                    None,
                    Some(HINSTANCE(instance.0)),
                    Some(state.cast::<c_void>()),
                )
                .expect("color window should be created")
            };
            unsafe {
                let _ = ShowWindow(hwnd, windows::Win32::UI::WindowsAndMessaging::SW_SHOW);
            }
            handle_tx.send(hwnd.0 as isize).unwrap();

            let mut message = MSG::default();
            while unsafe { GetMessageW(&mut message, None, 0, 0) }.into() {
                unsafe {
                    let _ = TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            }
        });

        let raw_handle = handle_rx
            .recv_timeout(Duration::from_secs(5))
            .expect("color window should become ready");
        thread::sleep(Duration::from_millis(250));
        Self {
            raw_handle,
            thread: Some(thread),
        }
    }
}

impl Drop for ColorWindow {
    fn drop(&mut self) {
        let hwnd = HWND(self.raw_handle as *mut c_void);
        unsafe {
            let _ = PostMessageW(Some(hwnd), WM_CLOSE, WPARAM(0), LPARAM(0));
        }
        if let Some(thread) = self.thread.take() {
            thread.join().expect("color window thread should exit");
        }
    }
}

extern "system" fn color_window_proc(
    hwnd: HWND,
    message: u32,
    _wparam: WPARAM,
    lparam: LPARAM,
) -> LRESULT {
    match message {
        WM_NCCREATE => unsafe {
            let create = &*(lparam.0 as *const CREATESTRUCTW);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, create.lpCreateParams as isize);
            LRESULT(1)
        },
        WM_CREATE => unsafe {
            SetTimer(Some(hwnd), TIMER_ID, TIMER_INTERVAL_MS, None);
            LRESULT(0)
        },
        WM_TIMER => unsafe {
            if let Some(state) = color_window_state(hwnd) {
                match state.pattern {
                    WindowPattern::Solid => {
                        state.color_index = (state.color_index + 1) % COLORS.len();
                        let _ =
                            windows::Win32::Graphics::Gdi::InvalidateRect(Some(hwnd), None, false);
                    }
                    WindowPattern::SparseMovingBlock(pattern) => {
                        let old_rect = sparse_block_rect(pattern, state.block_position);
                        state.block_position = (state.block_position + 1) % pattern.block_x.len();
                        let new_rect = sparse_block_rect(pattern, state.block_position);
                        let _ = windows::Win32::Graphics::Gdi::InvalidateRect(
                            Some(hwnd),
                            Some(&old_rect),
                            false,
                        );
                        let _ = windows::Win32::Graphics::Gdi::InvalidateRect(
                            Some(hwnd),
                            Some(&new_rect),
                            false,
                        );
                    }
                }
            }
            LRESULT(0)
        },
        WM_PAINT => unsafe {
            let mut paint = PAINTSTRUCT::default();
            let dc = BeginPaint(hwnd, &mut paint);
            if let Some(state) = color_window_state(hwnd) {
                match state.pattern {
                    WindowPattern::Solid => {
                        let brush = CreateSolidBrush(COLORS[state.color_index]);
                        FillRect(dc, &paint.rcPaint, brush);
                        let _ = DeleteObject(brush.into());
                    }
                    WindowPattern::SparseMovingBlock(pattern) => {
                        let background = CreateSolidBrush(COLORREF(0x00f0_f0f0));
                        FillRect(dc, &paint.rcPaint, background);
                        let _ = DeleteObject(background.into());
                        let block = CreateSolidBrush(COLORS[0]);
                        FillRect(dc, &sparse_block_rect(pattern, state.block_position), block);
                        let _ = DeleteObject(block.into());
                    }
                }
            }
            let _ = EndPaint(hwnd, &paint);
            LRESULT(0)
        },
        WM_CLOSE => unsafe {
            DestroyWindow(hwnd).expect("color window should be destroyed");
            LRESULT(0)
        },
        WM_DESTROY => unsafe {
            let _ = KillTimer(Some(hwnd), TIMER_ID);
            let state = SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            if state != 0 {
                drop(Box::from_raw(state as *mut ColorWindowState));
            }
            PostQuitMessage(0);
            LRESULT(0)
        },
        _ => unsafe { DefWindowProcW(hwnd, message, WPARAM(0), lparam) },
    }
}

fn sparse_block_rect(pattern: SparsePattern, position: usize) -> RECT {
    let left = pattern.block_x[position % pattern.block_x.len()];
    RECT {
        left,
        top: pattern.block_y,
        right: left + pattern.block_size,
        bottom: pattern.block_y + pattern.block_size,
    }
}

unsafe fn color_window_state(hwnd: HWND) -> Option<&'static mut ColorWindowState> {
    use windows::Win32::UI::WindowsAndMessaging::GetWindowLongPtrW;
    let state = unsafe { GetWindowLongPtrW(hwnd, GWLP_USERDATA) } as *mut ColorWindowState;
    unsafe { state.as_mut() }
}

fn decode_video_rgba(
    path: &std::path::Path,
    expected_width: u32,
    expected_height: u32,
) -> Vec<Vec<u8>> {
    ffmpeg::init().expect("FFmpeg should initialize");
    let mut input = ffmpeg::format::input(path).expect("recorded video should open");
    let stream = input
        .streams()
        .best(ffmpeg::media::Type::Video)
        .expect("recording should have a video stream");
    let stream_index = stream.index();
    let context = ffmpeg::codec::context::Context::from_parameters(stream.parameters())
        .expect("video decoder context should be created");
    let mut decoder = context
        .decoder()
        .video()
        .expect("video decoder should open");
    let width = decoder.width();
    let height = decoder.height();
    assert_eq!((width, height), (expected_width, expected_height));
    let mut scaler = ffmpeg::software::scaling::Context::get(
        decoder.format(),
        width,
        height,
        ffmpeg::format::Pixel::RGBA,
        width,
        height,
        ffmpeg::software::scaling::flag::Flags::POINT,
    )
    .expect("RGBA scaler should be created");
    let mut decoded = ffmpeg::frame::Video::empty();
    let mut rgba = ffmpeg::frame::Video::new(ffmpeg::format::Pixel::RGBA, width, height);
    let mut frames = Vec::new();
    let mut collect_frame = |decoded: &ffmpeg::frame::Video| {
        scaler
            .run(decoded, &mut rgba)
            .expect("video frame should convert to RGBA");
        let stride = rgba.stride(0);
        let row_bytes = width as usize * 4;
        let source = rgba.data(0);
        let mut pixels = Vec::with_capacity(row_bytes * height as usize);
        for row in 0..height as usize {
            let start = row * stride;
            pixels.extend_from_slice(&source[start..start + row_bytes]);
        }
        frames.push(pixels);
    };

    for (packet_stream, packet) in input.packets() {
        if packet_stream.index() != stream_index {
            continue;
        }
        decoder
            .send_packet(&packet)
            .expect("video packet should decode");
        while decoder.receive_frame(&mut decoded).is_ok() {
            collect_frame(&decoded);
        }
    }
    decoder.send_eof().expect("video decoder should flush");
    while decoder.receive_frame(&mut decoded).is_ok() {
        collect_frame(&decoded);
    }
    frames
}

fn changed_pixel_ratio(left: &[u8], right: &[u8]) -> f64 {
    assert_eq!(left.len(), right.len());
    let changed = left
        .chunks_exact(4)
        .zip(right.chunks_exact(4))
        .filter(|(left, right)| {
            left[..3]
                .iter()
                .zip(&right[..3])
                .any(|(left, right)| left.abs_diff(*right) >= 32)
        })
        .count();
    changed as f64 / (left.len() / 4) as f64
}

fn strongest_expected_color_ratio(frame: &[u8]) -> f64 {
    EXPECTED_RGB
        .iter()
        .map(|expected| {
            frame
                .chunks_exact(4)
                .filter(|pixel| {
                    pixel[..3].iter().zip(expected).all(|(actual, expected)| {
                        actual.abs_diff(*expected) <= COLOR_CHANNEL_TOLERANCE
                    })
                })
                .count() as f64
                / (frame.len() / 4) as f64
        })
        .fold(0.0, f64::max)
}

fn matching_color_ratio(frame: &[u8], expected: [u8; 3]) -> f64 {
    frame
        .chunks_exact(4)
        .filter(|pixel| {
            pixel[..3]
                .iter()
                .zip(expected)
                .all(|(actual, expected)| actual.abs_diff(expected) <= COLOR_CHANNEL_TOLERANCE)
        })
        .count() as f64
        / (frame.len() / 4) as f64
}

fn matching_color_ratio_in_rect(frame: &[u8], rect: RECT, expected: [u8; 3]) -> f64 {
    matching_color_ratio_in_rect_with_width(frame, WINDOW_SIZE as usize, rect, expected)
}

fn matching_color_ratio_in_rect_with_width(
    frame: &[u8],
    frame_width: usize,
    rect: RECT,
    expected: [u8; 3],
) -> f64 {
    let mut matching = 0usize;
    let mut total = 0usize;
    for y in rect.top as usize..rect.bottom as usize {
        for x in rect.left as usize..rect.right as usize {
            let offset = (y * frame_width + x) * 4;
            if frame[offset..offset + 3]
                .iter()
                .zip(expected)
                .all(|(actual, expected)| actual.abs_diff(expected) <= COLOR_CHANNEL_TOLERANCE)
            {
                matching += 1;
            }
            total += 1;
        }
    }
    matching as f64 / total as f64
}

fn black_pixel_ratio(frame: &[u8]) -> f64 {
    frame
        .chunks_exact(4)
        .filter(|pixel| pixel[..3].iter().all(|channel| *channel <= 32))
        .count() as f64
        / (frame.len() / 4) as f64
}

fn assert_frames_are_complete_solid_presents(frames: &[Vec<u8>], label: &str) {
    let (worst_index, worst_ratio) = frames
        .iter()
        .enumerate()
        .map(|(index, frame)| (index, strongest_expected_color_ratio(frame)))
        .min_by(|left, right| left.1.total_cmp(&right.1))
        .expect("recording should contain at least one frame");
    assert!(
        worst_ratio >= MIN_SOLID_COLOR_RATIO,
        "{label} frame {worst_index} mixed pixels from different presents; only {worst_ratio:.3} of its pixels matched one expected solid color"
    );
}

#[test]
fn dxgi_window_recording_exports_visually_changing_gif_frames() {
    let temp = tempdir().expect("temporary output directory should be created");
    let window = ColorWindow::spawn();
    let mut audio_track = RecordingAudioTrackConfig::system_default("system");
    audio_track.enabled = false;
    let config = RecordingConfig {
        target: RecordingTarget::Window(WindowSelector::new(window.raw_handle)),
        capture_backend: CaptureBackendKind::DxgiDuplication,
        output_dir: temp.path().to_path_buf(),
        fps: 20,
        audio: RecordingAudioConfig {
            tracks: vec![audio_track],
            ..RecordingAudioConfig::default()
        },
        ..RecordingConfig::default()
    };

    let mut recording = RecordingSession::create(config).expect("recording should be created");
    recording
        .start()
        .expect("DXGI window recording should start");
    thread::sleep(Duration::from_secs(1));
    let artifact = recording.stop().expect("DXGI window recording should stop");

    let source_frames = decode_video_rgba(
        &artifact.bundle_path,
        WINDOW_SIZE as u32,
        WINDOW_SIZE as u32,
    );
    let source_max_changed_ratio = source_frames
        .windows(2)
        .map(|pair| changed_pixel_ratio(&pair[0], &pair[1]))
        .fold(0.0, f64::max);
    assert!(
        source_frames.len() >= 2,
        "intermediate recording should decode to multiple frames"
    );
    assert_frames_are_complete_solid_presents(&source_frames, "intermediate recording");
    assert!(
        source_max_changed_ratio > 0.5,
        "intermediate recording frames should visibly change; maximum changed-pixel ratio was {source_max_changed_ratio:.3} across {} decoded frames",
        source_frames.len()
    );

    let editing = EditingSession::open(artifact).expect("recording should open for editing");
    let output_path = temp.path().join("dxgi-color-window.gif");
    let mut request = editing.export_request();
    request.format = ExportFormat::Gif;
    request.output_path = output_path.clone();
    editing.export(request).expect("GIF export should succeed");

    let frames = decode_video_rgba(&output_path, WINDOW_SIZE as u32, WINDOW_SIZE as u32);
    assert!(frames.len() >= 2, "GIF should decode to multiple frames");
    assert_frames_are_complete_solid_presents(&frames, "GIF");
    let max_changed_ratio = frames
        .windows(2)
        .map(|pair| changed_pixel_ratio(&pair[0], &pair[1]))
        .fold(0.0, f64::max);
    assert!(
        max_changed_ratio > 0.5,
        "GIF frames should visibly change; maximum changed-pixel ratio was {max_changed_ratio:.3} across {} decoded frames",
        frames.len()
    );
}

#[test]
fn wgc_region_recording_produces_complete_visually_changing_frames() {
    assert_sparse_region_recording(CaptureBackendKind::WindowsGraphicsCapture, "WGC");
}

#[test]
fn dxgi_region_recording_produces_complete_visually_changing_frames() {
    assert_sparse_region_recording(CaptureBackendKind::DxgiDuplication, "DXGI");
}

fn assert_sparse_region_recording(backend: CaptureBackendKind, backend_label: &str) {
    let _dpi_awareness = ThreadDpiAwareness::per_monitor_v2();
    let temp = tempdir().expect("temporary output directory should be created");
    let _window = ColorWindow::spawn_sparse();
    let mut audio_track = RecordingAudioTrackConfig::system_default("system");
    audio_track.enabled = false;
    let config = RecordingConfig {
        target: RecordingTarget::Region(RecordingRegion::new(
            WINDOW_X,
            WINDOW_Y,
            WINDOW_SIZE as u32,
            WINDOW_SIZE as u32,
        )),
        capture_backend: backend,
        output_dir: temp.path().to_path_buf(),
        fps: 20,
        audio: RecordingAudioConfig {
            tracks: vec![audio_track],
            ..RecordingAudioConfig::default()
        },
        ..RecordingConfig::default()
    };

    let mut recording = RecordingSession::create(config).expect("recording should be created");
    recording
        .start()
        .unwrap_or_else(|error| panic!("{backend_label} region recording should start: {error}"));
    thread::sleep(Duration::from_secs(1));
    let artifact = recording
        .stop()
        .unwrap_or_else(|error| panic!("{backend_label} region recording should stop: {error}"));

    let frames = decode_video_rgba(
        &artifact.bundle_path,
        WINDOW_SIZE as u32,
        WINDOW_SIZE as u32,
    );
    assert!(
        frames.len() >= 2,
        "{backend_label} region recording should decode to multiple frames"
    );
    assert_sparse_frames_have_no_ghosts_or_black_blocks(&frames, backend_label);
    let max_changed_ratio = frames
        .windows(2)
        .map(|pair| changed_pixel_ratio(&pair[0], &pair[1]))
        .fold(0.0, f64::max);
    assert!(
        max_changed_ratio > 0.03,
        "{backend_label} region frames should visibly change; maximum changed-pixel ratio was {max_changed_ratio:.3} across {} decoded frames",
        frames.len()
    );
}

fn assert_sparse_frames_have_no_ghosts_or_black_blocks(frames: &[Vec<u8>], backend_label: &str) {
    for (frame_index, frame) in frames.iter().enumerate() {
        let background_ratio = matching_color_ratio(frame, SPARSE_BACKGROUND_RGB);
        assert!(
            background_ratio >= 0.9,
            "{backend_label} region frame {frame_index} has incomplete background; only {background_ratio:.3} matched the static background"
        );

        let black_ratio = black_pixel_ratio(frame);
        assert!(
            black_ratio <= 0.01,
            "{backend_label} region frame {frame_index} contains black blocks covering {black_ratio:.3} of the image"
        );

        let mut block_ratios = SPARSE_BLOCK_X
            .iter()
            .enumerate()
            .map(|(position, _)| {
                matching_color_ratio_in_rect(
                    frame,
                    sparse_block_rect(
                        SparsePattern {
                            block_size: SPARSE_BLOCK_SIZE,
                            block_y: SPARSE_BLOCK_Y,
                            block_x: SPARSE_BLOCK_X,
                        },
                        position,
                    ),
                    SPARSE_BLOCK_RGB,
                )
            })
            .collect::<Vec<_>>();
        block_ratios.sort_by(|left, right| right.total_cmp(left));
        assert!(
            block_ratios[0] >= 0.6,
            "{backend_label} region frame {frame_index} is missing the moving block; strongest position matched {:.3}",
            block_ratios[0]
        );
        assert!(
            block_ratios[1] <= 0.2,
            "{backend_label} region frame {frame_index} contains a ghost block; second-strongest old position matched {:.3}",
            block_ratios[1]
        );
    }
}

struct CursorPositionGuard {
    original: POINT,
}

impl CursorPositionGuard {
    fn move_to(x: i32, y: i32) -> Self {
        let mut original = POINT::default();
        unsafe { GetCursorPos(&mut original) }.expect("cursor position should be readable");
        unsafe { SetCursorPos(x, y) }.expect("cursor should move into the recording region");
        Self { original }
    }
}

impl Drop for CursorPositionGuard {
    fn drop(&mut self) {
        let _ = unsafe { SetCursorPos(self.original.x, self.original.y) };
    }
}

#[test]
fn dxgi_large_region_remains_complete_through_snow_shot_mp4_export() {
    let temp = tempdir().expect("temporary output directory should be created");
    let _window = ColorWindow::spawn_large_sparse();
    let _cursor = CursorPositionGuard::move_to(
        LARGE_WINDOW_X + LARGE_WINDOW_WIDTH / 2,
        LARGE_WINDOW_Y + LARGE_WINDOW_HEIGHT / 2,
    );
    let mut audio_track = RecordingAudioTrackConfig::system_default("system");
    audio_track.enabled = false;
    let config = RecordingConfig {
        target: RecordingTarget::Region(RecordingRegion::new(
            LARGE_WINDOW_X,
            LARGE_WINDOW_Y,
            LARGE_WINDOW_WIDTH as u32,
            LARGE_WINDOW_HEIGHT as u32,
        )),
        capture_backend: CaptureBackendKind::DxgiDuplication,
        output_dir: temp.path().to_path_buf(),
        fps: 30,
        audio: RecordingAudioConfig {
            tracks: vec![audio_track],
            ..RecordingAudioConfig::default()
        },
        ..RecordingConfig::default()
    };

    let mut recording = RecordingSession::create(config).expect("recording should be created");
    recording
        .start()
        .expect("large DXGI region recording should start");
    thread::sleep(Duration::from_secs(2));
    let artifact = recording
        .stop()
        .expect("large DXGI region recording should stop");

    let source_frames = decode_video_rgba(
        &artifact.bundle_path,
        LARGE_WINDOW_WIDTH as u32,
        LARGE_WINDOW_HEIGHT as u32,
    );
    assert_large_sparse_frames_are_complete(&source_frames, "DXGI intermediate recording");

    let editing = EditingSession::open(artifact).expect("recording should open for editing");
    let output_path = temp.path().join("dxgi-large-region.mp4");
    let mut request = editing.export_request();
    request.format = ExportFormat::Mp4;
    request.output_path = output_path.clone();
    request.mouse.visible = true;
    editing.export(request).expect("MP4 export should succeed");

    let exported_frames = decode_video_rgba(
        &output_path,
        LARGE_WINDOW_WIDTH as u32,
        LARGE_WINDOW_HEIGHT as u32,
    );
    assert_large_sparse_frames_are_complete(&exported_frames, "Snow Shot MP4 export");
}

fn assert_large_sparse_frames_are_complete(frames: &[Vec<u8>], label: &str) {
    assert!(frames.len() >= 2, "{label} should contain multiple frames");
    let pattern = SparsePattern {
        block_size: LARGE_BLOCK_SIZE,
        block_y: LARGE_BLOCK_Y,
        block_x: LARGE_BLOCK_X,
    };
    for (frame_index, frame) in frames.iter().enumerate() {
        let background_ratio = matching_color_ratio(frame, SPARSE_BACKGROUND_RGB);
        assert!(
            background_ratio >= 0.97,
            "{label} frame {frame_index} is incomplete; only {background_ratio:.3} matched the background"
        );
        let black_ratio = black_pixel_ratio(frame);
        assert!(
            black_ratio <= 0.005,
            "{label} frame {frame_index} contains black blocks covering {black_ratio:.3} of the image"
        );

        let mut block_ratios = pattern
            .block_x
            .iter()
            .enumerate()
            .map(|(position, _)| {
                matching_color_ratio_in_rect_with_width(
                    frame,
                    LARGE_WINDOW_WIDTH as usize,
                    sparse_block_rect(pattern, position),
                    SPARSE_BLOCK_RGB,
                )
            })
            .collect::<Vec<_>>();
        block_ratios.sort_by(|left, right| right.total_cmp(left));
        assert!(
            block_ratios[0] >= 0.55,
            "{label} frame {frame_index} is missing the moving block; strongest position matched {:.3}",
            block_ratios[0]
        );
        assert!(
            block_ratios[1] <= 0.25,
            "{label} frame {frame_index} contains a ghost block; second position matched {:.3}",
            block_ratios[1]
        );
    }
}
