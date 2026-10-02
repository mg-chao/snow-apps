//! A clip is a half-open interval on the original, pause-free media clock.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TrimRange {
    pub start_us: u64,
    pub end_us: u64,
}

impl TrimRange {
    pub fn new(start_us: u64, end_us: u64, duration_us: u64) -> Result<Self, String> {
        if start_us >= end_us || end_us > duration_us {
            return Err("trim range must contain media and stay inside the source".into());
        }
        Ok(Self { start_us, end_us })
    }

    pub fn duration_us(self) -> u64 {
        self.end_us - self.start_us
    }

    pub fn sample_count(self, rate: u32) -> u64 {
        (u128::from(self.end_us) * u128::from(rate) / 1_000_000) as u64 - self.sample_offset(rate)
    }

    pub fn sample_offset(self, rate: u32) -> u64 {
        (u128::from(self.start_us) * u128::from(rate) / 1_000_000) as u64
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn half_open_range_rejects_empty_reversed_and_outside_media() {
        for (start, end) in [(0, 0), (10, 9), (0, 101)] {
            assert!(TrimRange::new(start, end, 100).is_err());
        }
        assert_eq!(TrimRange::new(0, 100, 100).unwrap().duration_us(), 100);
        assert_eq!(TrimRange::new(99, 100, 100).unwrap().duration_us(), 1);
    }

    #[test]
    fn audio_offsets_use_rational_media_time_without_overflow() {
        let range = TrimRange::new(1_234_567, 2_000_000, 2_000_000).unwrap();
        assert_eq!(range.sample_offset(48_000), 59_259);
        let range = TrimRange::new(u64::MAX / 2, u64::MAX, u64::MAX).unwrap();
        assert_eq!(range.sample_offset(1_000_000), range.start_us);
    }
}
