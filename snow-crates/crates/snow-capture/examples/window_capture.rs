use std::path::Path;
use std::time::Instant;

use anyhow::{Context, Result};
use snow_capture::{
    CaptureSystem, CaptureTarget, WindowId, backend::CaptureBackendKind, frame::Frame,
};

fn save_frame_png(frame: &Frame, path: &Path) -> anyhow::Result<()> {
    image::save_buffer(
        path,
        frame.as_rgba_bytes(),
        frame.width(),
        frame.height(),
        image::ColorType::Rgba8,
    )
    .map_err(|e| anyhow::anyhow!("failed to write PNG to {}: {e}", path.display()))
}

/// Returns the top-level window handle under the current mouse cursor.
#[cfg(windows)]
fn window_under_cursor() -> Result<isize> {
    use windows::Win32::Foundation::POINT;
    use windows::Win32::UI::WindowsAndMessaging::{
        GA_ROOT, GetAncestor, GetCursorPos, WindowFromPoint,
    };

    let mut pt = POINT::default();
    unsafe { GetCursorPos(&mut pt) }
        .ok()
        .context("GetCursorPos failed")?;

    let hwnd = unsafe { WindowFromPoint(pt) };
    if hwnd.0.is_null() {
        anyhow::bail!("no window found under cursor at ({}, {})", pt.x, pt.y);
    }

    let root = unsafe { GetAncestor(hwnd, GA_ROOT) };
    let handle = if root.0.is_null() { hwnd } else { root };

    println!(
        "Cursor at ({}, {}), window handle: {:?}",
        pt.x, pt.y, handle.0
    );
    Ok(handle.0 as isize)
}

fn capture_window_to_png(
    kind: CaptureBackendKind,
    label: &str,
    window: &WindowId,
    output_path: &str,
) -> Result<()> {
    let target = CaptureTarget::Window(*window);

    let begin = Instant::now();
    let system = CaptureSystem::builder()
        .with_backend_kind(kind)
        .build()
        .with_context(|| format!("failed to initialize {label} capture system"))?;
    let mut session = system
        .open_session(target, Default::default())
        .with_context(|| format!("failed to initialize {label} capture session"))?;
    let init_ms = begin.elapsed().as_secs_f64() * 1000.0;
    println!("Initialized {label} capture session in {init_ms:.3} ms");

    let begin = Instant::now();
    let frame = session
        .capture()
        .with_context(|| format!("failed to capture window using {label}"))?;
    let cap_ms = begin.elapsed().as_secs_f64() * 1000.0;

    println!(
        "Captured {label} window frame: {}x{} in {cap_ms:.3} ms",
        frame.width(),
        frame.height(),
    );

    save_frame_png(&frame, Path::new(output_path))?;
    println!("Saved {label} window capture to {output_path}\n");
    Ok(())
}

fn main() -> Result<()> {
    let raw_handle = window_under_cursor()?;
    let window = WindowId::from_windows_handle(raw_handle);

    capture_window_to_png(
        CaptureBackendKind::Gdi,
        "GDI",
        &window,
        "./window-capture-gdi.png",
    )?;
    capture_window_to_png(
        CaptureBackendKind::DxgiDuplication,
        "DXGI",
        &window,
        "./window-capture-dxgi.png",
    )?;
    capture_window_to_png(
        CaptureBackendKind::WindowsGraphicsCapture,
        "WGC",
        &window,
        "./window-capture-wgc.png",
    )?;

    Ok(())
}

#[cfg(not(windows))]
fn window_under_cursor() -> Result<isize> {
    anyhow::bail!(
        "automatic window picking is Windows-only; use the macOS harness for window enumeration"
    )
}
