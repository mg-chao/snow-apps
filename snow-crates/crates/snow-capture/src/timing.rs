//! Per-stage timing instrumentation for capture calls.
//!
//! This module is gated by the **`stage-timing` cargo feature** so that
//! production and release builds are completely unaffected: with the
//! feature disabled, every marker compiles to an inline no-op and no
//! timing API, fields, or clock reads exist anywhere in the crate.
//!
//! When the feature is enabled, a backend opens a [`StageScope`] at the
//! entry of a capture operation (if the session was configured with
//! `CaptureOptions::record_stage_timings`), and capture helpers record
//! stage boundaries through the marker functions in this module:
//!
//! - `stage_mark` closes the span since the previous mark (or scope entry)
//!   and is used for the additive, sequential stage breakdown.
//! - `stage_record` appends an externally measured span such as the time
//!   inside a single OS call, or a derived metric like OS frame age. These
//!   entries may overlap mark-based entries and are diagnostic detail
//!   rather than additive buckets.
//!
//! All markers are cheap no-ops while no scope is installed, so even
//! instrumented builds only pay for recording on sessions that opted in.

#[cfg(feature = "stage-timing")]
mod active;
#[cfg(not(feature = "stage-timing"))]
mod inert;

#[cfg(feature = "stage-timing")]
pub use active::StageTiming;

#[cfg(feature = "stage-timing")]
pub(crate) use active::{
    StageScope, attach_stage_timings, qpc_duration_between, qpc_ticks_to_duration,
    stage_checkpoint, stage_mark, stage_record, stage_record_since, stage_recording,
};

#[cfg(not(feature = "stage-timing"))]
pub(crate) use inert::{stage_checkpoint, stage_mark, stage_record_since};
