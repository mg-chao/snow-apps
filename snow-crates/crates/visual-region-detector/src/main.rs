mod cli;

use std::{env, error::Error, fs, path::Path, process::ExitCode};
use visual_region_detector::{
    BgrImage, backend_name, detect_regions, draw_regions, regions_to_jsonable, write_crops,
};

fn parent(path: &Path) -> Result<(), Box<dyn Error>> {
    if let Some(p) = path.parent().filter(|p| !p.as_os_str().is_empty()) {
        fs::create_dir_all(p)?;
    }
    Ok(())
}
fn run() -> Result<(), Box<dyn Error>> {
    let options = match cli::parse(env::args().skip(1))? {
        cli::Command::Help => {
            println!(
                "Usage: visual-region-detector <source.png> [--out-json PATH] [--out-preview PATH] [--no-preview] [--out-crops DIRECTORY]"
            );
            return Ok(());
        }
        cli::Command::Version => {
            #[cfg(feature = "opencv")]
            println!(
                "visual-region-detector {} (backend {}; OpenCV {})",
                env!("CARGO_PKG_VERSION"),
                backend_name(),
                visual_region_detector::opencv_backend::version()?
            );
            #[cfg(not(feature = "opencv"))]
            println!(
                "visual-region-detector {} (backend {})",
                env!("CARGO_PKG_VERSION"),
                backend_name()
            );
            return Ok(());
        }
        cli::Command::Detect(options) => options,
    };
    let cli::Options {
        source,
        out_json,
        preview,
        crops,
        no_preview,
    } = options;
    let rgb = image::open(&source)
        .map_err(|error| format!("{}: {error}", source.display()))?
        .to_rgb8();
    let bgr = BgrImage::from_rgb8(&rgb);
    let regions = detect_regions(&bgr)?;
    let mut payload = regions_to_jsonable(&source.to_string_lossy(), &bgr, &regions);
    if let Some(dir) = crops {
        payload["crops"] = write_crops(&bgr, &regions, &dir)?.into();
    }
    let text = serde_json::to_string_pretty(&payload)?;
    if let Some(path) = out_json {
        parent(&path)?;
        fs::write(path, &text)?;
    }
    if !no_preview {
        let path = preview.unwrap_or_else(|| {
            source.with_file_name(format!(
                "{}_regions.png",
                source.file_stem().unwrap_or_default().to_string_lossy()
            ))
        });
        parent(&path)?;
        draw_regions(&bgr, &regions, 2)?.to_rgb8().save(&path)?;
        eprintln!("wrote {}", path.display());
    }
    println!("{text}");
    Ok(())
}
fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("{e}");
            ExitCode::FAILURE
        }
    }
}
