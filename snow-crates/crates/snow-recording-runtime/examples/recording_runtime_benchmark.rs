use anyhow::{Context, Result, bail};

fn main() -> Result<()> {
    if cfg!(debug_assertions) {
        bail!("recording benchmarks require Release");
    }
    let args: Vec<_> = std::env::args().skip(1).collect();
    let output = args.first().context("output directory required")?;
    let scenario = args.get(1).map(String::as_str).unwrap_or("effects");
    let width = args.get(2).map(|v| v.parse()).transpose()?.unwrap_or(1920);
    let height = args.get(3).map(|v| v.parse()).transpose()?.unwrap_or(1080);
    let fps = args.get(4).map(|v| v.parse()).transpose()?.unwrap_or(60);
    let frames = args.get(5).map(|v| v.parse()).transpose()?.unwrap_or(120);
    snow_recording_runtime::direct::benchmark::run(
        std::path::Path::new(output),
        scenario,
        width,
        height,
        fps,
        frames,
        args.iter().any(|v| v == "--hardware"),
    )
    .map_err(|error| anyhow::anyhow!("{error}"))
}
