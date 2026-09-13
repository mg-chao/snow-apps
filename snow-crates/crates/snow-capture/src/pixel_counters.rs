//! Process-wide benchmark counters. Measure deltas with one active capture run.
//! Shader constant-buffer maps and overlay uploads are deliberately excluded.
use std::sync::atomic::{AtomicU64, Ordering};

static READBACKS: AtomicU64 = AtomicU64::new(0);
static CONVERSIONS: AtomicU64 = AtomicU64::new(0);

#[derive(Clone, Copy, Debug, Default)]
pub struct PixelCounters {
    pub captured_readbacks: u64,
    pub host_transfers: u64,
    pub cpu_conversions: u64,
}

pub fn snapshot() -> PixelCounters {
    let readbacks = READBACKS.load(Ordering::Relaxed);
    PixelCounters {
        captured_readbacks: readbacks,
        host_transfers: readbacks,
        cpu_conversions: CONVERSIONS.load(Ordering::Relaxed),
    }
}

impl PixelCounters {
    pub fn since(self, earlier: Self) -> Self {
        Self {
            captured_readbacks: self
                .captured_readbacks
                .saturating_sub(earlier.captured_readbacks),
            host_transfers: self.host_transfers.saturating_sub(earlier.host_transfers),
            cpu_conversions: self.cpu_conversions.saturating_sub(earlier.cpu_conversions),
        }
    }
}

pub(crate) fn readback() {
    READBACKS.fetch_add(1, Ordering::Relaxed);
}
pub(crate) fn conversion() {
    CONVERSIONS.fetch_add(1, Ordering::Relaxed);
}
