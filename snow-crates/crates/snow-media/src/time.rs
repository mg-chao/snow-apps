use serde::{Deserialize, Serialize};
use std::time::Duration;

/// Values from different domains must be anchored before comparison.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub enum ClockDomain {
    WindowsPerformanceCounter,
    MacHostTime,
    Session,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct MediaTime {
    pub value: i64,
    pub timescale: u32,
    pub domain: ClockDomain,
    pub epoch: i64,
}

impl MediaTime {
    /// Add a duration in this clock's time base, rounding down by less than one tick.
    pub fn checked_add_duration(self, duration: Duration) -> Option<Self> {
        if self.timescale == 0 {
            return None;
        }
        let ticks = duration
            .as_nanos()
            .checked_mul(u128::from(self.timescale))?
            / 1_000_000_000;
        let value = i128::from(self.value).checked_add(i128::try_from(ticks).ok()?)?;
        Some(Self {
            value: i64::try_from(value).ok()?,
            ..self
        })
    }

    /// Checked rational subtraction. Epoch changes are discontinuities, not elapsed time.
    pub fn duration_since(self, origin: Self) -> Option<Duration> {
        if self.domain != origin.domain
            || self.epoch != origin.epoch
            || self.timescale == 0
            || origin.timescale == 0
        {
            return None;
        }
        let numerator = i128::from(self.value) * i128::from(origin.timescale)
            - i128::from(origin.value) * i128::from(self.timescale);
        if numerator < 0 {
            return None;
        }
        let denominator = i128::from(self.timescale) * i128::from(origin.timescale);
        let seconds = u64::try_from(numerator / denominator).ok()?;
        let nanos = u32::try_from((numerator % denominator) * 1_000_000_000 / denominator).ok()?;
        Some(Duration::new(seconds, nanos))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn time(value: i64, timescale: u32) -> MediaTime {
        MediaTime {
            value,
            timescale,
            domain: ClockDomain::MacHostTime,
            epoch: 0,
        }
    }
    #[test]
    fn compares_rational_timestamps_without_delivery_jitter() {
        assert_eq!(
            time(90_001, 90_000).duration_since(time(48_000, 48_000)),
            Some(Duration::from_nanos(11_111))
        );
    }
    #[test]
    fn rejects_discontinuities_and_invalid_scales() {
        assert!(time(1, 0).duration_since(time(0, 1)).is_none());
        assert!(time(0, 1).duration_since(time(1, 1)).is_none());
        assert!(
            MediaTime {
                epoch: 1,
                ..time(1, 1)
            }
            .duration_since(time(0, 1))
            .is_none()
        );
        assert!(
            MediaTime {
                domain: ClockDomain::Session,
                ..time(1, 1)
            }
            .duration_since(time(0, 1))
            .is_none()
        );
    }
    #[test]
    fn duration_addition_retains_clock_and_checks_overflow() {
        assert_eq!(
            time(48_000, 48_000).checked_add_duration(Duration::from_millis(10)),
            Some(time(48_480, 48_000))
        );
        assert!(
            time(i64::MAX, 48_000)
                .checked_add_duration(Duration::from_secs(1))
                .is_none()
        );
        assert!(time(0, 0).checked_add_duration(Duration::ZERO).is_none());
    }
    #[test]
    fn extreme_values_do_not_overflow() {
        assert!(
            time(i64::MAX, 1)
                .duration_since(time(i64::MIN, 1))
                .is_some()
        );
    }
}
