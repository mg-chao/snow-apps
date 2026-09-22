//! Compare macOS capture hot-path choices on this machine.
//! Release only: `cargo run --release -p snow-macos --example capture_path_bench`
#[cfg(not(target_os = "macos"))]
fn main() {
    eprintln!("capture_path_bench requires macOS");
}
#[cfg(target_os = "macos")]
fn main() {
    // Shareable-content completion is delivered on the main run loop, and the
    // capture APIs refuse to block that thread. Time the paths on a worker.
    let worker = std::thread::spawn(snow_macos::bench::capture_path_timings);
    snow_macos::run_loop::drive_until(|| worker.is_finished());
    let rows = worker.join().expect("benchmark worker panicked");
    println!(
        "{:<62} {:>10} {:>10}  comparison",
        "path", "median_ms", "p95_ms"
    );
    for row in rows {
        println!(
            "{:<62} {:>10.3} {:>10.3}  {}",
            row.label, row.median_ms, row.p95_ms, row.note
        );
    }
}
