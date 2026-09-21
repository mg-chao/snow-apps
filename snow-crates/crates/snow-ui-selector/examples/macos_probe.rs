//! Live diagnostic (not a benchmark): macos_probe X Y DISPLAY_ID [EXCLUDED_WINDOW_ID ...].
#[cfg(target_os = "macos")]
fn main() -> snow_ui_selector::SelectorResult<()> {
    use snow_ui_selector::{
        AccessibilityBackend, ElementRegionService, HitTestMode, Point, QueryControl,
    };
    let args: Vec<_> = std::env::args().skip(1).collect();
    if args.len() < 3 {
        return Err("usage: macos_probe X Y DISPLAY_ID [EXCLUDED_WINDOW_ID ...]".into());
    }
    let point = Point {
        x: args[0].parse()?,
        y: args[1].parse()?,
        display_id: args[2].parse()?,
    };
    let excluded = args[3..]
        .iter()
        .map(|s| s.parse())
        .collect::<Result<Vec<usize>, _>>()?;
    let mut service = ElementRegionService::with_backend_excluding_ids(
        AccessibilityBackend::Accessibility,
        &excluded,
    )?;
    let initial = service.query(
        point,
        HitTestMode::UiElement,
        &QueryControl::foreground(),
        &mut |_| {},
    )?;
    println!("initial={initial:?}");
    let snapshot = service.window_snapshot().unwrap();
    let mut refinement = ElementRegionService::from_snapshot(&snapshot)?;
    let refined = refinement.query(
        point,
        HitTestMode::UiElement,
        &QueryControl::refinement(&|| false),
        &mut |path| println!("progress={path:?}"),
    )?;
    println!("refined={refined:?}");
    service.release_cache();
    println!("session released");
    Ok(())
}
#[cfg(not(target_os = "macos"))]
fn main() {
    eprintln!("macos_probe requires macOS");
}
