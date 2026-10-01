//! End-to-end recording benchmark; the native workload runs on Windows.

#[cfg(windows)]
#[path = "support/realtime_recording_benchmark.rs"]
mod workload;

#[cfg(windows)]
fn main() -> anyhow::Result<()> {
    workload::run()
}

#[cfg(not(windows))]
fn main() {
    eprintln!("The realtime recording benchmark requires Windows.");
}
