//! End-to-end live benchmark for the GPU zero-copy recording pipeline.
//!
//! Drives a real `DirectRecordingSession` over one monitor and reports
//! effective capture/encode throughput plus per-stage GPU timings (when
//! built with the `recording-benchmark` feature). `--cpu` runs the same
//! harness on the CPU pipeline for direct comparison. Unless
//! `--no-animate` is passed, a topmost GDI window animates inside the
//! recorded area so Windows Graphics Capture keeps delivering frames —
//! a quiet desktop produces almost no capture events by design.
//!
//! Run via `scripts/run-recording-gpu-perf.ps1` (release build with
//! feature `recording-benchmark`), or manually:
//! `cargo run -p snow-recording-runtime --features recording-benchmark
//!  --release --example recording_gpu_benchmark -- --seconds 12`.

use std::path::PathBuf;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

use snow_recording_runtime::direct::{DirectRecordingConfig, DirectRecordingSession};

fn arg_value(name: &str) -> Option<String> {
    let mut iter = std::env::args().skip(1);
    while let Some(arg) = iter.next() {
        if let Some(stripped) = arg.strip_prefix("--") {
            if let Some((key, value)) = stripped.split_once('=') {
                if key == name {
                    return Some(value.to_string());
                }
            } else if stripped == name {
                return iter.next();
            }
        }
    }
    None
}

fn arg_flag(name: &str) -> bool {
    std::env::args().skip(1).any(|arg| {
        arg.strip_prefix("--")
            .is_some_and(|stripped| stripped.split('=').next() == Some(name))
    })
}

fn parse_size(value: &str) -> Option<(u32, u32)> {
    let (width, height) = value.split_once('x')?;
    Some((width.parse().ok()?, height.parse().ok()?))
}

/// The recorded monitor: `--size WxH` picks the matching monitor, the
/// default is the primary.
fn target_region() -> snow_recording_runtime::config::RecordingRegion {
    let layout = snow_capture::MonitorLayout::snapshot().expect("monitor layout");
    let wanted = arg_value("size").and_then(|value| parse_size(&value));
    let chosen = wanted
        .and_then(|(width, height)| {
            layout
                .monitors
                .iter()
                .find(|entry| entry.width == width && entry.height == height)
        })
        .or_else(|| {
            layout
                .monitors
                .iter()
                .find(|entry| entry.monitor.is_primary())
        })
        .or_else(|| layout.monitors.first())
        .expect("at least one monitor");
    eprintln!(
        "recording monitor: {}x{} at ({}, {})",
        chosen.width, chosen.height, chosen.x, chosen.y
    );
    snow_recording_runtime::config::RecordingRegion::new(
        chosen.x,
        chosen.y,
        chosen.width,
        chosen.height,
    )
}

/// Keep the desktop inside `region` changing at `fps` so WGC emits frames.
/// A D3D11 flip-model swapchain presents a fresh color every frame, which
/// forces DWM composition (and therefore WGC delivery) at the present
/// rate; GDI animation composes at a fraction of that.
fn spawn_animator(
    region: snow_recording_runtime::config::RecordingRegion,
    fps: u32,
    stop: Arc<AtomicBool>,
) -> std::thread::JoinHandle<()> {
    std::thread::spawn(move || {
        // Safety: windowing, the D3D11 device, and the swapchain all live
        // on this thread and die with it.
        unsafe {
            use windows::Win32::Foundation::{HWND, LPARAM, LRESULT, WPARAM};
            use windows::Win32::Graphics::Direct3D::{
                D3D_DRIVER_TYPE_HARDWARE, D3D_FEATURE_LEVEL_11_0,
            };
            use windows::Win32::Graphics::Direct3D11::{
                D3D11_CREATE_DEVICE_BGRA_SUPPORT, D3D11_SDK_VERSION, D3D11_VIEWPORT,
                D3D11CreateDevice, ID3D11Device, ID3D11DeviceContext, ID3D11RenderTargetView,
            };
            use windows::Win32::Graphics::Dxgi::Common::{
                DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_SAMPLE_DESC,
            };
            use windows::Win32::Graphics::Dxgi::{
                CreateDXGIFactory1, DXGI_PRESENT, DXGI_SWAP_CHAIN_DESC1,
                DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, DXGI_USAGE_RENDER_TARGET_OUTPUT, IDXGIFactory2,
            };
            use windows::Win32::UI::WindowsAndMessaging::{
                CS_HREDRAW, CS_VREDRAW, CreateWindowExW, DefWindowProcW, DispatchMessageW, MSG,
                PM_REMOVE, PeekMessageW, RegisterClassW, SW_SHOW, ShowWindow, WINDOW_EX_STYLE,
                WNDCLASSW, WS_POPUP, WS_VISIBLE,
            };

            unsafe extern "system" fn wnd_proc(
                hwnd: HWND,
                message: u32,
                wparam: WPARAM,
                lparam: LPARAM,
            ) -> LRESULT {
                unsafe { DefWindowProcW(hwnd, message, wparam, lparam) }
            }

            let class_name: windows::core::HSTRING = "snow-recording-gpu-benchmark".into();
            let window_name: windows::core::HSTRING = "recording-gpu-benchmark".into();
            let class = WNDCLASSW {
                style: CS_HREDRAW | CS_VREDRAW,
                lpfnWndProc: Some(wnd_proc),
                lpszClassName: windows::core::PCWSTR(class_name.as_ptr()),
                ..Default::default()
            };
            if RegisterClassW(&class) == 0 {
                eprintln!("animator: RegisterClassW failed");
                return;
            }
            let Ok(hwnd) = CreateWindowExW(
                WINDOW_EX_STYLE(0x8), // WS_EX_TOPMOST
                windows::core::PCWSTR(class_name.as_ptr()),
                windows::core::PCWSTR(window_name.as_ptr()),
                WS_POPUP | WS_VISIBLE,
                region.x,
                region.y,
                region.width as i32,
                region.height as i32,
                None,
                None,
                None,
                None,
            ) else {
                eprintln!("animator: CreateWindowExW failed");
                return;
            };
            let _ = ShowWindow(hwnd, SW_SHOW);

            let mut device = None;
            let mut context = None;
            let levels = [D3D_FEATURE_LEVEL_11_0];
            let device_result = D3D11CreateDevice(
                None,
                D3D_DRIVER_TYPE_HARDWARE,
                windows::Win32::Foundation::HMODULE::default(),
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                Some(&levels),
                D3D11_SDK_VERSION,
                Some(&mut device),
                None,
                Some(&mut context),
            );
            if device_result.is_err() {
                eprintln!("animator: D3D11CreateDevice failed: {device_result:?}");
                return;
            }
            let device: ID3D11Device = device.unwrap();
            let context: ID3D11DeviceContext = context.unwrap();

            let factory: IDXGIFactory2 = CreateDXGIFactory1().expect("dxgi factory");
            let swap_desc = DXGI_SWAP_CHAIN_DESC1 {
                Width: region.width,
                Height: region.height,
                Format: DXGI_FORMAT_B8G8R8A8_UNORM,
                Stereo: false.into(),
                SampleDesc: DXGI_SAMPLE_DESC {
                    Count: 1,
                    Quality: 0,
                },
                BufferUsage: DXGI_USAGE_RENDER_TARGET_OUTPUT,
                BufferCount: 2,
                Scaling: Default::default(),
                SwapEffect: DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL,
                AlphaMode: Default::default(),
                Flags: 0,
            };
            let swapchain = match factory.CreateSwapChainForHwnd(
                &device,
                hwnd,
                &swap_desc,
                None,
                None::<&windows::Win32::Graphics::Dxgi::IDXGIOutput>,
            ) {
                Ok(swapchain) => swapchain,
                Err(error) => {
                    eprintln!("animator: swapchain failed: {error}");
                    return;
                }
            };

            let Ok(backbuffer) =
                swapchain.GetBuffer::<windows::Win32::Graphics::Direct3D11::ID3D11Texture2D>(0)
            else {
                eprintln!("animator: GetBuffer failed");
                return;
            };
            let mut view = None;
            if device
                .CreateRenderTargetView(&backbuffer, None, Some(&mut view))
                .is_err()
            {
                eprintln!("animator: render target view failed");
                return;
            }
            let view: ID3D11RenderTargetView = view.unwrap();
            context.RSSetViewports(Some(&[D3D11_VIEWPORT {
                TopLeftX: 0.0,
                TopLeftY: 0.0,
                Width: region.width as f32,
                Height: region.height as f32,
                MinDepth: 0.0,
                MaxDepth: 1.0,
            }]));

            let frame = Duration::from_secs_f64(1.0 / f64::from(fps.max(1)));
            let mut frame_index = 0u64;
            let mut message = MSG::default();
            while !stop.load(Ordering::Relaxed) {
                let started = Instant::now();
                while PeekMessageW(&mut message, None, 0, 0, PM_REMOVE).as_bool() {
                    let _ = DispatchMessageW(&message);
                }
                // Cycle the full-screen clear color; every pixel changes
                // every frame.
                let phase = frame_index as f32 * 0.13;
                let color = [
                    phase.sin().abs(),
                    (phase * 0.7).sin().abs(),
                    (phase * 1.3).sin().abs(),
                    1.0,
                ];
                context.ClearRenderTargetView(&view, &color);
                let _ = swapchain.Present(1, DXGI_PRESENT(0));
                frame_index += 1;
                let elapsed = started.elapsed();
                if elapsed < frame {
                    std::thread::sleep(frame - elapsed);
                }
            }
            eprintln!("animator: {frame_index} frames drawn");
        }
    })
}

fn decoded_summary(path: &std::path::Path) -> (usize, u32, u32, f64) {
    use ffmpeg_next as ffmpeg;
    let mut input = ffmpeg::format::input(path).expect("open recording");
    let stream = input
        .streams()
        .best(ffmpeg::media::Type::Video)
        .expect("video stream");
    let stream_index = stream.index();
    let time_base = stream.time_base();
    let mut decoder = ffmpeg::codec::context::Context::from_parameters(stream.parameters())
        .unwrap()
        .decoder()
        .video()
        .unwrap();
    let dimensions = (decoder.width(), decoder.height());
    let mut decoded = ffmpeg::frame::Video::empty();
    let mut count = 0usize;
    let mut first_pts = None;
    let mut last_pts = None;
    for (stream, packet) in input.packets() {
        if stream.index() != stream_index {
            continue;
        }
        decoder.send_packet(&packet).unwrap();
        while decoder.receive_frame(&mut decoded).is_ok() {
            if let Some(pts) = decoded.pts() {
                first_pts.get_or_insert(pts);
                last_pts = Some(pts);
            }
            count += 1;
        }
    }
    decoder.send_eof().unwrap();
    while decoder.receive_frame(&mut decoded).is_ok() {
        if let Some(pts) = decoded.pts() {
            first_pts.get_or_insert(pts);
            last_pts = Some(pts);
        }
        count += 1;
    }
    let seconds = match (first_pts, last_pts) {
        (Some(first), Some(last)) => {
            (last - first) as f64 * f64::from(time_base.numerator())
                / f64::from(time_base.denominator())
        }
        _ => 0.0,
    };
    (count, dimensions.0, dimensions.1, seconds)
}

fn probe_capture(region: snow_recording_runtime::config::RecordingRegion, fps: u32) {
    use snow_capture::backend::CaptureBackendKind;
    use snow_capture::{
        CaptureOptions, CapturePixelFormat, CaptureStream, CaptureStreamConfig, CaptureWorkload,
        SurfaceDelivery,
    };
    let system = snow_capture::CaptureSystem::builder()
        .with_backend_kind(CaptureBackendKind::WindowsGraphicsCapture)
        .build()
        .expect("capture system");
    let session = system
        .open_session(
            snow_capture::CaptureTarget::Region(
                snow_capture::CaptureRegion::new(region.x, region.y, region.width, region.height)
                    .unwrap(),
            ),
            CaptureOptions {
                workload: CaptureWorkload::Continuous,
                output_pixel_format: CapturePixelFormat::Bgra8,
                output_surface: SurfaceDelivery::GpuTexture {
                    native_cursor: true,
                },
                ..CaptureOptions::default()
            },
        )
        .expect("surface session");
    let stream = CaptureStream::spawn(
        session,
        CaptureStreamConfig {
            target_fps: fps,
            min_fps: 1,
            buffer_depth: 8,
            max_consecutive_errors: 30,
            adaptive_fps: false,
            pause_on_resolution_change: false,
            include_cursor: false,
        },
    )
    .expect("surface stream");
    let deadline = Instant::now() + Duration::from_secs(8);
    let mut events = 0u64;
    let mut duplicates = 0u64;
    while Instant::now() < deadline {
        match stream.recv_timeout(Duration::from_millis(100)) {
            Ok(snow_capture::CaptureEvent::Surface(frame)) => {
                events += 1;
                duplicates += u64::from(frame.is_duplicate());
            }
            Ok(_) => {}
            Err(_) => {}
        }
    }
    let stats = stream.stats();
    let captured = stats.snapshot().frames_captured;
    println!(
        "probe: events={events} duplicates={duplicates} frames_captured={captured} rate={:.1}/s",
        events as f64 / 8.0
    );
}

fn main() {
    let seconds: u64 = arg_value("seconds")
        .and_then(|value| value.parse().ok())
        .unwrap_or(10);
    let fps: u32 = arg_value("fps")
        .and_then(|value| value.parse().ok())
        .unwrap_or(60);
    let force_cpu = arg_flag("cpu");
    let animate = !arg_flag("no-animate");
    let enable_audio = arg_flag("audio");
    let output_directory = PathBuf::from(arg_value("output").unwrap_or_else(|| ".".to_string()));
    std::fs::create_dir_all(&output_directory).expect("output directory");

    let region = target_region();
    if arg_flag("probe-capture") {
        let stop = Arc::new(AtomicBool::new(false));
        let animator = spawn_animator(region, 60, Arc::clone(&stop));
        std::thread::sleep(Duration::from_millis(800));
        probe_capture(region, 60);
        stop.store(true, Ordering::Relaxed);
        let _ = animator.join();
        return;
    }
    let width = region.width;
    let height = region.height;
    let label = if force_cpu { "cpu" } else { "gpu" };
    let output_path = output_directory.join(format!("recording-{label}-{width}x{height}.mp4"));

    let stop = Arc::new(AtomicBool::new(false));
    let animator = animate.then(|| spawn_animator(region, fps, Arc::clone(&stop)));

    let config = DirectRecordingConfig {
        region,
        capture_backend: snow_recording_runtime::config::CaptureBackendKind::Auto,
        output_path: output_path.clone(),
        format: snow_recording_export::ExportFormat::Mp4,
        capture_fps: fps,
        output_fps: fps,
        maximum_width: None,
        maximum_height: None,
        codec: snow_recording_model::VideoCodec::H264,
        preset: snow_recording_model::VideoEncodingSpeed::VeryFast,
        prefer_hardware_encoder: true,
        enable_microphone: false,
        enable_system_audio: enable_audio,
        show_cursor: true,
        keyboard: None,
        mouse_trail_rgba: [0, 0, 0, 0],
        mouse_trail_duration_ms: 500,
        mouse_click_rgba: [0, 0, 0, 0],
    };

    let mut session = DirectRecordingSession::create(config).expect("session");
    if force_cpu {
        session.force_cpu_pipeline();
    }
    // Give the animator a moment to start composing before timing.
    if animate {
        std::thread::sleep(Duration::from_millis(500));
    }
    let started = Instant::now();
    session.start().expect("start");
    std::thread::sleep(Duration::from_secs(seconds));
    let report = session.stop().expect("stop");
    let wall_seconds = started.elapsed().as_secs_f64();
    stop.store(true, Ordering::Relaxed);
    if let Some(animator) = animator {
        let _ = animator.join();
    }
    let output_bytes = std::fs::metadata(&output_path)
        .map(|metadata| metadata.len())
        .unwrap_or_default();

    let (decoded_frames, decoded_width, decoded_height, decoded_seconds) =
        decoded_summary(&output_path);

    println!(
        "mode,width,height,target_fps,wall_seconds,encoded_frames,effective_fps,decoded_frames,decoded_seconds,coalesced,dropped_capture,output_bytes,video_encoder,used_gpu_pipeline,gpu_fallback_reason"
    );
    println!(
        "{label},{width},{height},{fps},{wall_seconds:.3},{},{:.2},{decoded_frames},{decoded_seconds:.3},{},{},{output_bytes},{},{},{}",
        report.encoded_frames,
        report.encoded_frames as f64 / wall_seconds.max(f64::EPSILON),
        report.coalesced_frames,
        report.dropped_capture_frames,
        report.video_encoder,
        report.used_gpu_pipeline,
        report.gpu_fallback_reason.as_deref().unwrap_or(""),
    );
    eprintln!(
        "recording: {}x{} decoded {} frames over {decoded_seconds:.2}s -> {}",
        decoded_width,
        decoded_height,
        decoded_frames,
        output_path.display()
    );

    #[cfg(feature = "recording-benchmark")]
    {
        let timings = &report.gpu_stage_timings;
        let blt_ms = timings.blt.as_secs_f64() * 1_000.0;
        let input_ms = timings.mft_input.as_secs_f64() * 1_000.0;
        let output_ms = timings.mft_output.as_secs_f64() * 1_000.0;
        eprintln!(
            "gpu stages: blt={blt_ms:.1}ms mft_input={input_ms:.1}ms mft_output={output_ms:.1}ms"
        );
        let stages_path = output_directory.join(format!("recording-{label}-stages.csv"));
        std::fs::write(
            &stages_path,
            format!("stage,total_ms\nblt,{blt_ms:.3}\nmft_input,{input_ms:.3}\nmft_output,{output_ms:.3}\n"),
        )
        .expect("stage csv");
        eprintln!("stage data: {}", stages_path.display());
    }
}
