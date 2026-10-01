//! End-to-end post-processing benchmark; the native workload runs on Windows.

#[cfg(windows)]
#[path = "support/deferred_recording_benchmark.rs"]
mod workload;

#[cfg(windows)]
fn main() -> anyhow::Result<()> {
    workload::run()
}

#[cfg(not(windows))]
fn main() {
    eprintln!("The deferred recording benchmark requires Windows.");
}
