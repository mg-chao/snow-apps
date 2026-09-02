//! Process-wide opt-in switch for OCR stage-timing instrumentation.
//!
//! Stage timings exist to support the benchmark harness and the `rapidocr`
//! CLI. Production hosts embed the library without a timing reader, so
//! collection is disabled by default and every timing site skips its clock
//! reads. Tools that want timings call [`set_stage_timing_enabled`] once at
//! startup; timing fields on outputs then read `None` instead of zero, so
//! "not measured" stays distinguishable from "measured as fast".

use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Instant;

static STAGE_TIMING_ENABLED: AtomicBool = AtomicBool::new(false);

/// Enables or disables stage-timing collection for this process.
pub fn set_stage_timing_enabled(enabled: bool) {
    STAGE_TIMING_ENABLED.store(enabled, Ordering::Release);
}

/// Returns whether stage-timing collection is currently enabled.
pub fn stage_timing_enabled() -> bool {
    STAGE_TIMING_ENABLED.load(Ordering::Acquire)
}

/// Captures a stage start instant, or `None` when timing is disabled.
pub(crate) fn timing_start() -> Option<Instant> {
    stage_timing_enabled().then(Instant::now)
}

/// Converts a captured start instant into elapsed milliseconds, or `None`
/// when no start was captured.
pub(crate) fn timing_ms(start: Option<Instant>) -> Option<f32> {
    start.map(|start| start.elapsed().as_secs_f32() * 1000.0)
}
