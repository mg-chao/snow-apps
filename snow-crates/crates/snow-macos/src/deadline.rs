use crate::{MacError, MacResult};
use std::time::{Duration, Instant};

/// One budget shared by dependent native operations. Each wait consumes the
/// original budget instead of silently restarting the timeout per callback.
#[derive(Clone, Copy)]
pub(crate) struct Deadline(Instant);
impl Deadline {
    pub fn new(timeout: Duration) -> MacResult<Self> {
        if timeout.is_zero() {
            return Err(MacError::Timeout);
        }
        Instant::now()
            .checked_add(timeout)
            .map(Self)
            .ok_or_else(|| MacError::InvalidConfig("deadline overflow".into()))
    }
    pub fn remaining(self) -> MacResult<Duration> {
        self.remaining_at(Instant::now())
    }
    fn remaining_at(self, now: Instant) -> MacResult<Duration> {
        self.0
            .checked_duration_since(now)
            .filter(|d| !d.is_zero())
            .ok_or(MacError::Timeout)
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn dependent_operations_share_a_single_budget() {
        let start = Instant::now();
        let deadline = Deadline(start + Duration::from_secs(5));
        assert_eq!(
            deadline
                .remaining_at(start + Duration::from_secs(2))
                .unwrap(),
            Duration::from_secs(3)
        );
        assert!(matches!(
            deadline.remaining_at(start + Duration::from_secs(5)),
            Err(MacError::Timeout)
        ));
        assert!(Deadline::new(Duration::ZERO).is_err());
        assert!(Deadline::new(Duration::MAX).is_err());
    }
}
