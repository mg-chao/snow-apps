//! Completion barriers for native filter mutations. A deadline reports failure;
//! only terminal callbacks release the barrier for the next native mutation.
use std::{collections::HashSet, time::Instant};

pub(crate) enum NativeUpdateProgress {
    Pending { report_timeout: bool },
    Settled { success: bool },
}

pub(crate) struct NativeUpdateBarrier {
    deadline: Instant,
    failed: bool,
    timed_out: bool,
}
impl NativeUpdateBarrier {
    pub(crate) fn new(deadline: Instant, failed: bool) -> Self {
        Self {
            deadline,
            failed,
            timed_out: false,
        }
    }
    pub(crate) fn poll(
        &mut self,
        outstanding: usize,
        failed: bool,
        now: Instant,
    ) -> NativeUpdateProgress {
        self.failed |= failed;
        let report_timeout = !self.timed_out && now >= self.deadline;
        self.timed_out |= report_timeout;
        if outstanding == 0 {
            NativeUpdateProgress::Settled {
                success: !self.failed && !self.timed_out,
            }
        } else {
            NativeUpdateProgress::Pending { report_timeout }
        }
    }
}

pub(crate) fn contains_required_windows(
    required: &[u32],
    available: impl Iterator<Item = u32>,
) -> bool {
    if required.is_empty() {
        return true;
    }
    let available: HashSet<u32> = available.collect();
    required.iter().all(|id| available.contains(id))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::Duration;

    #[test]
    fn timeout_retains_pending_native_operations_and_late_success_stays_failed() {
        let deadline = Instant::now();
        let mut barrier = NativeUpdateBarrier::new(deadline, false);
        assert!(matches!(
            barrier.poll(2, false, deadline),
            NativeUpdateProgress::Pending {
                report_timeout: true
            }
        ));
        assert!(matches!(
            barrier.poll(1, false, deadline + Duration::from_secs(1)),
            NativeUpdateProgress::Pending {
                report_timeout: false
            }
        ));
        assert!(matches!(
            barrier.poll(0, false, deadline + Duration::from_secs(2)),
            NativeUpdateProgress::Settled { success: false }
        ));
    }

    #[test]
    fn partial_failure_waits_for_all_callbacks_before_releasing_the_barrier() {
        let now = Instant::now();
        let mut barrier = NativeUpdateBarrier::new(now + Duration::from_secs(5), false);
        assert!(matches!(
            barrier.poll(1, true, now),
            NativeUpdateProgress::Pending {
                report_timeout: false
            }
        ));
        assert!(matches!(
            barrier.poll(0, false, now),
            NativeUpdateProgress::Settled { success: false }
        ));
        let mut successful = NativeUpdateBarrier::new(now + Duration::from_secs(5), false);
        assert!(matches!(
            successful.poll(0, false, now),
            NativeUpdateProgress::Settled { success: true }
        ));
    }

    #[test]
    fn replacement_snapshot_must_include_every_required_window() {
        assert!(contains_required_windows(&[], [].into_iter()));
        assert!(contains_required_windows(&[9, 7, 9], [7, 8, 9].into_iter()));
        assert!(!contains_required_windows(&[7, 9], [7, 8].into_iter()));
    }
}
