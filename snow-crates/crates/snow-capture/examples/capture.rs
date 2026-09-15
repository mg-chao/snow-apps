use std::path::Path;
use std::time::Instant;

use anyhow::{Context, Result, bail};
use snow_capture::{
    CaptureOptions, CaptureSystem, CaptureTarget, backend::CaptureBackendKind, frame::Frame,
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

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum HdrConversionMode {
    Cpu,
    Gpu,
}

impl HdrConversionMode {
    fn as_str(self) -> &'static str {
        match self {
            Self::Cpu => "cpu",
            Self::Gpu => "gpu",
        }
    }

    fn gpu_hdr_conversion_enabled(self) -> bool {
        matches!(self, Self::Gpu)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum BackendSelection {
    All,
    Dxgi,
    Wgc,
}

impl BackendSelection {
    fn from_str(raw: &str) -> Result<Self> {
        match raw {
            "all" => Ok(Self::All),
            "dxgi" => Ok(Self::Dxgi),
            "wgc" => Ok(Self::Wgc),
            _ => bail!("invalid --backend value: {raw}. Use `dxgi`, `wgc`, or `all`"),
        }
    }
}

#[derive(Clone, Copy, Debug)]
struct CliOptions {
    hdr_mode: HdrConversionMode,
    backend: BackendSelection,
    hdr_lut: bool,
}

fn parse_options() -> Result<CliOptions> {
    let args: Vec<String> = std::env::args().collect();
    let mut hdr_mode = HdrConversionMode::Gpu;
    let mut backend = BackendSelection::All;
    let mut hdr_lut = true;

    let mut i = 1usize;
    while i < args.len() {
        match args[i].as_str() {
            "--gpu" => {
                hdr_mode = HdrConversionMode::Gpu;
                i += 1;
            }
            "--cpu" => {
                hdr_mode = HdrConversionMode::Cpu;
                i += 1;
            }
            "--backend" => {
                let value = args.get(i + 1).map(String::as_str);
                let value = value.context("--backend requires `dxgi`, `wgc`, or `all`")?;
                backend = BackendSelection::from_str(value)?;
                i += 2;
            }
            "--hdr-lut" => {
                hdr_lut = true;
                i += 1;
            }
            "--no-hdr-lut" => {
                hdr_lut = false;
                i += 1;
            }
            "--help" | "-h" => {
                println!(
                    "Usage: cargo run --release --example capture -- [options]
  --gpu                  Enable GPU HDR conversion (default)
  --cpu                  Force CPU HDR conversion
  --backend <kind>       dxgi | wgc | all (default: all)
  --hdr-lut              Enable LUT HDR tonemap approximation (default)
  --no-hdr-lut           Disable LUT approximation (use precise BT.2390)

This example captures exactly one frame per selected backend.
Tip: HDR conversion is exercised only when Windows HDR/Advanced Color is enabled on the target monitor.

Examples:
  cargo run --release --example capture -- --gpu --backend dxgi
  cargo run --release --example capture -- --cpu --backend all --no-hdr-lut"
                );
                std::process::exit(0);
            }
            other => bail!("unknown argument: {other}. Use --help for usage."),
        }
    }

    Ok(CliOptions {
        hdr_mode,
        backend,
        hdr_lut,
    })
}

fn selected_backends(selection: BackendSelection) -> &'static [(CaptureBackendKind, &'static str)] {
    match selection {
        BackendSelection::All => &[
            (CaptureBackendKind::DxgiDuplication, "DXGI"),
            (CaptureBackendKind::WindowsGraphicsCapture, "WGC"),
        ],
        BackendSelection::Dxgi => &[(CaptureBackendKind::DxgiDuplication, "DXGI")],
        BackendSelection::Wgc => &[(CaptureBackendKind::WindowsGraphicsCapture, "WGC")],
    }
}

fn capture_primary_to_png(
    kind: CaptureBackendKind,
    label: &str,
    output_path: &str,
    options: CliOptions,
) -> Result<()> {
    let target = CaptureTarget::PrimaryMonitor;

    let begin = Instant::now();
    let system = CaptureSystem::builder()
        .with_backend_kind(kind)
        .build()
        .with_context(|| format!("failed to initialize {label} capture system"))?;
    let mut session = system
        .open_session(
            target,
            CaptureOptions {
                backend_tuning: snow_capture::tuning::BackendTuning::Windows(
                    snow_capture::tuning::windows::WindowsCaptureOptions {
                        gpu_hdr_conversion: options.hdr_mode.gpu_hdr_conversion_enabled(),
                        hdr_tonemap_lut: options.hdr_lut,
                        ..Default::default()
                    },
                ),
                ..Default::default()
            },
        )
        .with_context(|| format!("failed to initialize {label} capture session"))?;
    let init_ms = begin.elapsed().as_secs_f64() * 1000.0;

    println!(
        "Initialized {label} session in {init_ms:.3} ms (hdr-conversion={}, hdr-lut={})",
        options.hdr_mode.as_str(),
        options.hdr_lut
    );

    let begin = Instant::now();
    let frame = session
        .capture()
        .with_context(|| format!("failed to capture frame using {label}"))?;
    let elapsed_ms = begin.elapsed().as_secs_f64() * 1000.0;
    let pipeline_ms = frame
        .metadata()
        .capture_duration()
        .map(|d| d.as_secs_f64() * 1000.0)
        .unwrap_or(elapsed_ms);

    println!(
        "Captured {label} frame: {}x{}, wall={elapsed_ms:.3} ms, pipeline={pipeline_ms:.3} ms, duplicate={}, color={:?}",
        frame.width(),
        frame.height(),
        frame.metadata().is_duplicate(),
        frame.metadata().color_space()
    );

    save_frame_png(&frame, Path::new(output_path))?;
    println!("Saved {label} capture to {output_path}\n");
    Ok(())
}

fn main() -> Result<()> {
    let options = parse_options()?;
    let mode_tag = options.hdr_mode.as_str();

    println!(
        "Single-frame HDR conversion benchmark (hdr-conversion={}, backend={:?}).",
        mode_tag, options.backend
    );
    println!(
        "Note: if the monitor is not in Windows HDR/Advanced Color mode, HDR conversion path will not be exercised.\n"
    );

    for (kind, label) in selected_backends(options.backend) {
        let output = format!(
            "./capture-{}-hdr-{}.png",
            label.to_ascii_lowercase(),
            mode_tag
        );
        capture_primary_to_png(*kind, label, &output, options)?;
    }

    Ok(())
}
