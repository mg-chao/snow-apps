use std::{
    fs,
    path::{Path, PathBuf},
    sync::mpsc,
    thread,
    time::Instant,
};

use clap::Parser;
use rapid_ocr_rs::{
    EngineConfig, OcrCallOptions, OcrInput, OcrResult, OcrService, RapidOcr, StageTimings,
};
use serde_json::json;

fn main() {
    if let Err(err) = run_main() {
        eprintln!("error: {err}");
        std::process::exit(1);
    }
}

fn run_main() -> Result<(), Box<dyn std::error::Error>> {
    let cli = Cli::parse();
    let image_paths = collect_images(&cli.images_dir)?;
    if image_paths.is_empty() {
        return Err(format!("no images found under {}", cli.images_dir.display()).into());
    }

    // Decode once so measured runs represent warm OCR execution rather than
    // repeatedly mixing codec latency into model and preprocessing timings.
    let decode_start = Instant::now();
    let images = load_fixtures(&image_paths)?;
    let decode_ms = ms(decode_start);

    let cfg = if let Some(config_path) = &cli.config_path {
        EngineConfig::from_yaml_file(config_path)?
    } else {
        EngineConfig::default()
    };

    let init_start = Instant::now();
    let cold_engine = RapidOcr::new(cfg.clone())?;
    let init_ms = ms(init_start);
    drop(cold_engine);

    let service_start = Instant::now();
    let service = OcrService::new_with_workers(cfg.clone(), cli.concurrency)?;
    let service_start_ms = ms(service_start);

    for _ in 0..cli.warmup {
        let mut warmup_pending = Vec::with_capacity(images.len());
        for image in &images {
            warmup_pending.push(service.submit(image.input(), OcrCallOptions::default())?.1);
        }
        for receiver in warmup_pending {
            receiver.recv()??;
        }
    }

    let mut wall_ms = Vec::new();
    let mut det_ms = Vec::new();
    let mut cls_ms = Vec::new();
    let mut rec_ms = Vec::new();
    let mut e2e_ms = Vec::new();
    let mut e2e_aligned = Vec::new();
    let mut round_wall_ms = Vec::new();
    let mut det_pre_ms = Vec::new();
    let mut det_infer_ms = Vec::new();
    let mut det_post_ms = Vec::new();

    for _round in 0..cli.rounds {
        let round_start = Instant::now();
        let mut pending = Vec::with_capacity(images.len());
        for image in &images {
            let submitted = Instant::now();
            let (_, receiver) = service.submit(image.input(), OcrCallOptions::default())?;
            pending.push((submitted, receiver));
        }
        let pending_len = pending.len();
        let (completion_tx, completion_rx) = mpsc::channel();
        thread::scope(|scope| -> Result<(), Box<dyn std::error::Error>> {
            for (submitted, receiver) in pending {
                let completion_tx = completion_tx.clone();
                scope.spawn(move || {
                    let out = receiver.recv();
                    let _ = completion_tx.send((ms(submitted), out));
                });
            }
            drop(completion_tx);

            for _ in 0..pending_len {
                let (completed_wall_ms, out) = completion_rx.recv()?;
                let out = out??;
                wall_ms.push(completed_wall_ms);

                let timing = timings_from_result(&out);
                e2e_aligned.push(timing.and_then(|t| t.e2e_ms.map(f64::from)));
                if let Some(t) = timing {
                    if let Some(v) = t.det_ms {
                        det_ms.push(v as f64);
                    }
                    if let Some(v) = t.cls_ms {
                        cls_ms.push(v as f64);
                    }
                    if let Some(v) = t.rec_ms {
                        rec_ms.push(v as f64);
                    }
                    if let Some(v) = t.e2e_ms {
                        e2e_ms.push(v as f64);
                    }
                    if let Some(v) = t.det_pre_ms {
                        det_pre_ms.push(v as f64);
                    }
                    if let Some(v) = t.det_infer_ms {
                        det_infer_ms.push(v as f64);
                    }
                    if let Some(v) = t.det_post_ms {
                        det_post_ms.push(v as f64);
                    }
                }
            }
            Ok(())
        })?;
        round_wall_ms.push(ms(round_start));
    }

    let det_breakdown = if cli.profile_det_breakdown {
        Some(json!({
            "pre_ms": stats(&det_pre_ms),
            "infer_ms": stats(&det_infer_ms),
            "post_ms": stats(&det_post_ms),
        }))
    } else {
        None
    };

    let total_wall_ms = round_wall_ms.iter().sum::<f64>();
    let measured_images = wall_ms.len();
    let providers = service.worker_provider_resolutions();
    let service_stats = service.stats();

    let report = json!({
        "meta": {
            "images_dir": cli.images_dir,
            "image_count": images.len(),
            "rounds": cli.rounds,
            "concurrency": cli.concurrency,
            "warmup": cli.warmup,
            "input_mode": "preloaded_bgr",
            "decode_ms_total": decode_ms,
            "decode_ms_per_image": decode_ms / images.len() as f64,
            "init_ms": init_ms,
            "service_start_ms": service_start_ms,
            "profile_det_breakdown": cli.profile_det_breakdown,
            "providers": {
                "workers": providers
                    .iter()
                    .map(|worker| json!({
                        "det": format!("{:?}", worker.det),
                        "cls": format!("{:?}", worker.cls),
                        "rec": format!("{:?}", worker.rec),
                    }))
                    .collect::<Vec<_>>(),
            },
            "threads": {
                "det_intra": cfg.det.runtime.intra_threads,
                "det_inter": cfg.det.runtime.inter_threads,
                "det_rayon": cfg.det.runtime.rayon_threads,
                "det_budget": cfg.det.runtime.thread_budget,
                "rec_intra": cfg.rec.runtime.intra_threads,
                "rec_inter": cfg.rec.runtime.inter_threads,
                "rec_rayon": cfg.rec.runtime.rayon_threads,
                "rec_budget": cfg.rec.runtime.thread_budget,
                "effective_worker_budget": service.worker_thread_budget(),
            },
            "batch": {
                "cls": cfg.cls.cls_batch_num,
                "rec": cfg.rec.rec_batch_num,
            },
        },
        "stats": {
            "wall_ms": stats(&wall_ms),
            "det_ms": stats(&det_ms),
            "cls_ms": stats(&cls_ms),
            "rec_ms": stats(&rec_ms),
            "e2e_ms": stats(&e2e_ms),
            "det_breakdown_ms": det_breakdown,
            "measured_images": measured_images,
            "total_wall_ms": total_wall_ms,
            "round_wall_ms": stats(&round_wall_ms),
            "service": {
                "worker_limit": service_stats.worker_limit,
                "workers_started": service_stats.workers_started,
                "peak_active_workers": service_stats.peak_active_workers,
                "queued_at_report": service_stats.queued,
                "submitted": service_stats.submitted,
                "completed": service_stats.completed,
                "cancelled": service_stats.cancelled,
            },
            "images_per_second": if total_wall_ms > 0.0 {
                measured_images as f64 * 1000.0 / total_wall_ms
            } else {
                0.0
            },
            "wall_minus_e2e_ms": wall_minus_e2e(&wall_ms, &e2e_aligned),
        }
    });

    let text = serde_json::to_string_pretty(&report)?;
    if let Some(path) = cli.output_path {
        fs::write(&path, &text)?;
        println!("saved report to {}", path.display());
    }
    println!("{text}");
    Ok(())
}

#[derive(Debug, Clone, Parser)]
#[command(
    name = "bench_warm_e2e",
    about = "Warm benchmark for end-to-end OCR runs"
)]
struct Cli {
    #[arg(long = "config")]
    config_path: Option<PathBuf>,
    #[arg(long, default_value = "test/test_files")]
    images_dir: PathBuf,
    #[arg(long, default_value_t = 3, value_parser = parse_rounds)]
    rounds: usize,
    #[arg(long, default_value_t = 1, value_parser = parse_rounds)]
    warmup: usize,
    #[arg(long, default_value_t = 1, value_parser = parse_concurrency)]
    concurrency: usize,
    #[arg(long = "output")]
    output_path: Option<PathBuf>,
    #[arg(long)]
    profile_det_breakdown: bool,
}

struct ImageFixture {
    width: usize,
    height: usize,
    bgr: Vec<u8>,
}

impl ImageFixture {
    fn input(&self) -> OcrInput {
        OcrInput::BgrU8 {
            width: self.width,
            height: self.height,
            data: self.bgr.clone(),
        }
    }
}

fn load_fixtures(paths: &[PathBuf]) -> Result<Vec<ImageFixture>, Box<dyn std::error::Error>> {
    paths
        .iter()
        .map(|path| {
            let image = rapid_ocr_rs::RecImage::from_path(path)?;
            Ok(ImageFixture {
                width: image.width(),
                height: image.height(),
                bgr: image.into_bytes(),
            })
        })
        .collect()
}

fn collect_images(dir: &Path) -> Result<Vec<PathBuf>, Box<dyn std::error::Error>> {
    let mut out = Vec::new();
    for entry in fs::read_dir(dir)? {
        let entry = entry?;
        let path = entry.path();
        if !path.is_file() {
            continue;
        }
        let Some(ext) = path.extension().and_then(|v| v.to_str()) else {
            continue;
        };
        if is_image_ext(ext) {
            out.push(path);
        }
    }
    out.sort();
    Ok(out)
}

fn is_image_ext(ext: &str) -> bool {
    matches!(
        ext.to_ascii_lowercase().as_str(),
        "jpg" | "jpeg" | "png" | "bmp" | "webp" | "tif" | "tiff"
    )
}

fn timings_from_result(result: &OcrResult) -> Option<&StageTimings> {
    match result {
        OcrResult::Det(v) => Some(&v.timings),
        OcrResult::Cls(v) => Some(&v.timings),
        OcrResult::Rec(v) => Some(&v.timings),
        OcrResult::Full(v) => Some(&v.timings),
        OcrResult::Empty => None,
    }
}

fn ms(start: Instant) -> f64 {
    start.elapsed().as_secs_f64() * 1000.0
}

fn stats(values: &[f64]) -> serde_json::Value {
    if values.is_empty() {
        return json!({
            "count": 0
        });
    }

    let mut sorted = values.to_vec();
    sorted.sort_by(|a, b| a.total_cmp(b));
    let count = sorted.len();
    let sum = sorted.iter().copied().sum::<f64>();
    let avg = sum / count as f64;
    let min = sorted[0];
    let max = sorted[count - 1];
    let p50 = percentile(&sorted, 0.5);
    let p90 = percentile(&sorted, 0.9);

    json!({
        "count": count,
        "avg": avg,
        "p50": p50,
        "p90": p90,
        "min": min,
        "max": max,
    })
}

fn percentile(sorted: &[f64], p: f64) -> f64 {
    let idx = ((sorted.len() - 1) as f64 * p).round() as usize;
    sorted[idx]
}

fn wall_minus_e2e(wall_ms: &[f64], e2e_ms: &[Option<f64>]) -> serde_json::Value {
    let values = wall_ms
        .iter()
        .zip(e2e_ms.iter())
        .filter_map(|(wall, e2e)| e2e.map(|e2e| (wall - e2e).max(0.0)))
        .collect::<Vec<_>>();
    stats(&values)
}

fn parse_rounds(value: &str) -> Result<usize, String> {
    let rounds = value
        .parse::<usize>()
        .map_err(|_| format!("invalid --rounds value `{value}`"))?;
    if rounds == 0 {
        return Err("rounds must be greater than zero".to_string());
    }
    Ok(rounds)
}

fn parse_concurrency(value: &str) -> Result<usize, String> {
    let concurrency = value
        .parse::<usize>()
        .map_err(|_| format!("invalid --concurrency value `{value}`"))?;
    if !(1..=2).contains(&concurrency) {
        return Err("concurrency must be 1 or 2".to_string());
    }
    Ok(concurrency)
}
