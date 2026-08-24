//! Inert marker stubs, compiled when the `stage-timing` feature is
//! disabled.
//!
//! Every stub is `#[inline(always)]` and empty, so the optimizer removes
//! the call, its arguments, and any surrounding control flow that becomes
//! dead. This keeps capture hot paths completely free of instrumentation
//! code in production builds while allowing call sites to be written
//! unconditionally.

#![allow(dead_code)]

use std::time::Instant;

/// No-op: closes no span because no recorder can be installed.
#[inline(always)]
pub(crate) fn stage_mark(_name: &'static str) {}

/// Always `None`: no checkpoint can be consumed.
#[inline(always)]
pub(crate) fn stage_checkpoint() -> Option<Instant> {
    None
}

/// No-op: consumes no checkpoint.
#[inline(always)]
pub(crate) fn stage_record_since(_name: &'static str, _begin: Option<Instant>) {}
