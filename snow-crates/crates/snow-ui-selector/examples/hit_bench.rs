use std::collections::BTreeMap;
use std::thread;
use std::time::{Duration, Instant};

use snow_ui_selector::{AccessibilityBackend, ElementRegionService, HitTestMode, QueryControl};
use windows::Win32::Foundation::POINT;
use windows::Win32::UI::WindowsAndMessaging::GetCursorPos;
use windows::core::Result;

#[path = "support/native_fixture.rs"]
mod native_fixture;

#[derive(Debug)]
struct Options {
    backend: AccessibilityBackend,
    mode: HitTestMode,
    samples: usize,
    interval_ms: u64,
    csv: bool,
    include_refresh: bool,
    point: Option<POINT>,
    points: Vec<POINT>,
    rounds: usize,
    native_fixture: bool,
}

impl Default for Options {
    fn default() -> Self {
        Self {
            backend: AccessibilityBackend::Msaa,
            mode: HitTestMode::UiElement,
            samples: 1,
            interval_ms: 250,
            csv: false,
            include_refresh: false,
            point: None,
            points: Vec::new(),
            rounds: 1,
            native_fixture: false,
        }
    }
}

fn main() -> Result<()> {
    let options = parse_options();
    snow_ui_selector::enable_high_dpi_support();
    let _fixture = options
        .native_fixture
        .then(native_fixture::NativeFixture::start);
    let mut selector = ElementRegionService::with_backend(options.backend)?;
    if !options.points.is_empty() {
        return run_sequence(&options, &mut selector);
    }

    if !options.include_refresh {
        selector.refresh()?;
    }

    if options.csv {
        println!(
            "sample,backend,mode,include_refresh,ok,elapsed_ms,mouse_x,mouse_y,rect_x,rect_y,rect_w,rect_h,rect_count,detail"
        );
    }

    for sample in 1..=options.samples {
        let point = match options.point {
            Some(point) => point,
            None => cursor_pos()?,
        };

        let started = Instant::now();
        let hit = if options.include_refresh {
            selector.refresh()?;
            selector
                .query(
                    point,
                    options.mode,
                    &QueryControl::foreground(),
                    &mut |_| {},
                )?
                .path
        } else {
            selector
                .query(
                    point,
                    options.mode,
                    &QueryControl::foreground(),
                    &mut |_| {},
                )?
                .path
        };
        let elapsed_ms = started.elapsed().as_secs_f64() * 1000.0;

        let rect_count = hit.as_ref().map_or(0, Vec::len);
        let first = hit.as_ref().and_then(|rects| rects.first());

        if options.csv {
            if let Some(rect) = first {
                println!(
                    "{sample},{},{},{},{},{elapsed_ms:.3},{},{},{},{},{},{},{},hit",
                    backend_name(options.backend),
                    mode_name(options.mode),
                    options.include_refresh as u8,
                    1,
                    point.x,
                    point.y,
                    rect.left(),
                    rect.top(),
                    rect.width(),
                    rect.height(),
                    rect_count
                );
            } else {
                println!(
                    "{sample},{},{},{},{},{elapsed_ms:.3},{},{},{},{},{},{},{},no-hit",
                    backend_name(options.backend),
                    mode_name(options.mode),
                    options.include_refresh as u8,
                    0,
                    point.x,
                    point.y,
                    0,
                    0,
                    0,
                    0,
                    rect_count
                );
            }
        } else if let Some(rect) = first {
            println!(
                "sample={sample} backend={} mode={} include_refresh={} ok=yes time={elapsed_ms:.3} ms",
                backend_name(options.backend),
                mode_name(options.mode),
                options.include_refresh
            );
            println!(
                "  mouse=({}, {}) rect=({}, {}, {}, {}) rect_count={rect_count}",
                point.x,
                point.y,
                rect.left(),
                rect.top(),
                rect.width(),
                rect.height()
            );
        } else {
            println!(
                "sample={sample} backend={} mode={} include_refresh={} ok=no time={elapsed_ms:.3} ms",
                backend_name(options.backend),
                mode_name(options.mode),
                options.include_refresh
            );
            println!("  mouse=({}, {}) no hit", point.x, point.y);
        }

        if sample != options.samples && options.interval_ms > 0 {
            thread::sleep(Duration::from_millis(options.interval_ms));
        }
    }

    Ok(())
}

fn run_sequence(options: &Options, selector: &mut ElementRegionService) -> Result<()> {
    let mut timings: BTreeMap<&str, Vec<f64>> = BTreeMap::new();
    println!("stage,round,point_index,sample,elapsed_ms,x,y,rect_count,path");
    for round in 0..options.rounds {
        let started = Instant::now();
        selector.refresh()?;
        let elapsed = started.elapsed().as_secs_f64() * 1000.0;
        timings.entry("refresh").or_default().push(elapsed);
        println!("refresh,{round},0,0,{elapsed:.6},0,0,0,\"\"");
        for (index, &point) in options.points.iter().enumerate() {
            for sample in 0..options.samples {
                let stage = if options.include_refresh {
                    "refresh_and_hit"
                } else if sample > 0 {
                    "warm"
                } else if index == 0 {
                    "cold"
                } else {
                    "new_branch"
                };
                let started = Instant::now();
                if options.include_refresh {
                    selector.refresh()?;
                }
                let path = selector
                    .query(
                        point,
                        options.mode,
                        &QueryControl::foreground(),
                        &mut |_| {},
                    )?
                    .path
                    .unwrap_or_default();
                let elapsed = started.elapsed().as_secs_f64() * 1000.0;
                timings.entry(stage).or_default().push(elapsed);
                let rectangles = path
                    .iter()
                    .map(|rect| {
                        format!(
                            "{}:{}:{}:{}",
                            rect.left(),
                            rect.top(),
                            rect.right(),
                            rect.bottom()
                        )
                    })
                    .collect::<Vec<_>>()
                    .join(";");
                println!(
                    "{stage},{round},{index},{sample},{elapsed:.6},{},{},{},\"{rectangles}\"",
                    point.x,
                    point.y,
                    path.len()
                );
                if options.interval_ms > 0 {
                    thread::sleep(Duration::from_millis(options.interval_ms));
                }
            }
        }
    }
    for (stage, mut values) in timings {
        values.sort_by(f64::total_cmp);
        let percentile =
            |percent: usize| values[(values.len() * percent).div_ceil(100).saturating_sub(1)];
        eprintln!(
            "{stage}: n={} p50={:.6} ms p95={:.6} ms p99={:.6} ms",
            values.len(),
            percentile(50),
            percentile(95),
            percentile(99)
        );
    }
    Ok(())
}

fn cursor_pos() -> Result<POINT> {
    let mut point = POINT::default();
    unsafe {
        GetCursorPos(&mut point)?;
    }
    Ok(point)
}

fn backend_name(backend: AccessibilityBackend) -> &'static str {
    match backend {
        AccessibilityBackend::Uia => "uia",
        AccessibilityBackend::Msaa => "msaa",
    }
}

fn mode_name(mode: HitTestMode) -> &'static str {
    match mode {
        HitTestMode::UiElement => "element",
        HitTestMode::Window => "window",
    }
}

fn parse_options() -> Options {
    let mut options = Options::default();
    let mut args = std::env::args().skip(1);

    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--native-fixture" => {
                options.native_fixture = true;
                options.points = vec![POINT { x: 100, y: 120 }, POINT { x: 350, y: 120 }];
            }
            "--backend" | "-b" => {
                let value = args
                    .next()
                    .unwrap_or_else(|| usage_exit("missing --backend value"));
                if value.eq_ignore_ascii_case("msaa") {
                    options.backend = AccessibilityBackend::Msaa;
                } else if value.eq_ignore_ascii_case("uia") {
                    options.backend = AccessibilityBackend::Uia;
                } else {
                    usage_exit("invalid --backend value");
                }
            }
            "--mode" | "-m" => {
                let value = args
                    .next()
                    .unwrap_or_else(|| usage_exit("missing --mode value"));
                if value.eq_ignore_ascii_case("element") {
                    options.mode = HitTestMode::UiElement;
                } else if value.eq_ignore_ascii_case("window") {
                    options.mode = HitTestMode::Window;
                } else {
                    usage_exit("invalid --mode value");
                }
            }
            "--samples" | "-n" => {
                let value = args
                    .next()
                    .unwrap_or_else(|| usage_exit("missing --samples value"));
                options.samples = parse_positive_usize(&value, "--samples");
            }
            "--rounds" => {
                let value = args
                    .next()
                    .unwrap_or_else(|| usage_exit("missing --rounds value"));
                options.rounds = parse_positive_usize(&value, "--rounds");
            }
            "--interval-ms" => {
                let value = args
                    .next()
                    .unwrap_or_else(|| usage_exit("missing --interval-ms value"));
                options.interval_ms = value
                    .parse::<u64>()
                    .unwrap_or_else(|_| usage_exit("invalid --interval-ms value"));
            }
            "--point" => {
                let x = args
                    .next()
                    .unwrap_or_else(|| usage_exit("missing --point x value"));
                let y = args
                    .next()
                    .unwrap_or_else(|| usage_exit("missing --point y value"));
                options.point = Some(POINT {
                    x: x.parse::<i32>()
                        .unwrap_or_else(|_| usage_exit("invalid --point x value")),
                    y: y.parse::<i32>()
                        .unwrap_or_else(|_| usage_exit("invalid --point y value")),
                });
                options
                    .points
                    .push(options.point.expect("point was parsed"));
            }
            "--include-refresh" => options.include_refresh = true,
            "--csv" => options.csv = true,
            "--help" | "-h" => {
                print_usage();
                std::process::exit(0);
            }
            _ => usage_exit("unknown argument"),
        }
    }

    options
}

fn parse_positive_usize(value: &str, name: &str) -> usize {
    let parsed = value
        .parse::<usize>()
        .unwrap_or_else(|_| usage_exit(&format!("invalid {name} value")));
    if parsed == 0 {
        usage_exit(&format!("{name} must be greater than zero"));
    }
    parsed
}

fn usage_exit(message: &str) -> ! {
    eprintln!("{message}");
    print_usage();
    std::process::exit(2);
}

fn print_usage() {
    eprintln!(
        "Usage: hit_bench [--backend msaa|uia] [--mode element|window] [--samples N] [--rounds N] [--interval-ms N] [--point X Y ...] [--native-fixture] [--include-refresh] [--csv]"
    );
}
