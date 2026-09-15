//! Timestamp types: `StreamTimestamp`, `TickFormat`, and `TimestampAnchor`.

pub use snow_media::time::{ClockDomain, MediaTime};
use std::time::{Duration, Instant};

/// Format of the raw OS timing value carried in a [`StreamTimestamp`].
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TickFormat {
    /// Raw QPC ticks. Requires frequency-based conversion:
    /// `duration = ticks / qpc_frequency`.
    RawQpc,
    /// WASAPI 100-nanosecond units. Direct conversion:
    /// `duration = ticks * 100ns`.
    Hns100,
    Rational {
        timescale: u32,
        domain: ClockDomain,
        epoch: i64,
    },
}

/// A timestamp that works across video and audio streams.
///
/// Carries both a Rust [`Instant`] (for fallback) and the raw OS timing
/// value with its format, so consumers can apply the correct conversion.
#[derive(Clone, Debug)]
pub struct StreamTimestamp {
    /// Monotonic Rust instant.
    pub instant: Instant,
    /// Raw OS timing value. `None` when the OS didn't provide one.
    pub raw_os_ticks: Option<i64>,
    /// Format of `raw_os_ticks`, indicating which conversion to apply.
    pub tick_format: TickFormat,
}

/// Anchor for converting [`StreamTimestamp`] values into stream-relative durations.
///
/// Created from the first event's timestamp. Caches the QPC frequency
/// for efficient repeated conversions and stores the stream's [`TickFormat`]
/// so that [`stream_relative`](TimestampAnchor::stream_relative) selects the correct conversion path.
#[derive(Clone, Debug)]
pub struct TimestampAnchor {
    /// Origin timestamp of the stream.
    origin: StreamTimestamp,
    /// Cached QPC frequency (ticks/second). 0 on failure.
    qpc_frequency: i64,
    /// The tick format of the stream this anchor belongs to.
    /// Determines which conversion path `stream_relative` uses.
    tick_format: TickFormat,
}

impl TimestampAnchor {
    /// Create an anchor from the first event's timestamp.
    ///
    /// The `tick_format` is taken from the origin timestamp and stored
    /// so that all subsequent `stream_relative` calls use the same
    /// conversion path. Different domains and epochs use the anchored Instant fallback.
    pub fn new(origin: StreamTimestamp) -> Self {
        let tick_format = origin.tick_format;
        Self {
            origin,
            qpc_frequency: query_qpc_frequency(),
            tick_format,
        }
    }

    /// Create an anchor with a specific frequency for testing.
    ///
    /// Bypasses the OS QPC query so tests can control the frequency value.
    #[cfg(test)]
    pub(crate) fn new_with_frequency(origin: StreamTimestamp, qpc_frequency: i64) -> Self {
        let tick_format = origin.tick_format;
        Self {
            origin,
            qpc_frequency,
            tick_format,
        }
    }

    /// Convert a timestamp to a stream-relative [`Duration`].
    ///
    /// Both timestamps must use compatible formats. Rational timestamps may
    /// use different timescales within the same clock domain and epoch:
    /// - For [`TickFormat::RawQpc`]: `(current_ticks - origin_ticks) / qpc_frequency`
    /// - For [`TickFormat::Hns100`]: `(current_ticks - origin_ticks) * 100ns`
    /// - Fallback: `current.instant - origin.instant` (when `raw_os_ticks` is absent
    ///   on either side, or QPC frequency is 0 for `RawQpc`)
    pub fn stream_relative(&self, ts: &StreamTimestamp) -> Duration {
        if let (Some(current), Some(origin)) = (ts.raw_os_ticks, self.origin.raw_os_ticks) {
            if let (
                TickFormat::Rational {
                    timescale,
                    domain,
                    epoch,
                },
                TickFormat::Rational {
                    timescale: origin_scale,
                    domain: origin_domain,
                    epoch: origin_epoch,
                },
            ) = (ts.tick_format, self.tick_format)
            {
                return MediaTime {
                    value: current,
                    timescale,
                    domain,
                    epoch,
                }
                .duration_since(MediaTime {
                    value: origin,
                    timescale: origin_scale,
                    domain: origin_domain,
                    epoch: origin_epoch,
                })
                .unwrap_or_else(|| ts.instant.saturating_duration_since(self.origin.instant));
            }
            if ts.tick_format != self.tick_format {
                return ts.instant.saturating_duration_since(self.origin.instant);
            }
            match self.tick_format {
                TickFormat::RawQpc if self.qpc_frequency > 0 => {
                    let delta = (i128::from(current) - i128::from(origin)).max(0);
                    let secs = delta / i128::from(self.qpc_frequency);
                    let remainder = delta % i128::from(self.qpc_frequency);
                    let nanos = (remainder * 1_000_000_000 / i128::from(self.qpc_frequency)) as u32;
                    Duration::new(secs as u64, nanos)
                }
                TickFormat::Hns100 => {
                    let delta = (i128::from(current) - i128::from(origin)).max(0) as u64;
                    // Split into microseconds + remainder to avoid overflow in delta * 100.
                    // Each 100ns unit = 0.1µs, so 10 units = 1µs.
                    let micros = delta / 10;
                    let remaining_hns = delta % 10;
                    Duration::from_micros(micros) + Duration::from_nanos(remaining_hns * 100)
                }
                // RawQpc with qpc_frequency == 0 falls through to Instant fallback
                _ => ts.instant.saturating_duration_since(self.origin.instant),
            }
        } else {
            ts.instant.saturating_duration_since(self.origin.instant)
        }
    }

    /// The origin timestamp.
    pub fn origin(&self) -> &StreamTimestamp {
        &self.origin
    }

    /// Cached QPC frequency.
    pub fn qpc_frequency(&self) -> i64 {
        self.qpc_frequency
    }

    /// The tick format of the stream this anchor belongs to.
    pub fn tick_format(&self) -> TickFormat {
        self.tick_format
    }
}

/// Query the QPC frequency from the OS.
///
/// Returns 0 if the query fails (non-Windows or API failure).
fn query_qpc_frequency() -> i64 {
    #[cfg(target_os = "windows")]
    {
        let mut freq = 0i64;
        // SAFETY: QueryPerformanceFrequency writes to a valid i64 pointer
        // and is always safe to call on Windows.
        unsafe {
            let _ = windows::Win32::System::Performance::QueryPerformanceFrequency(&raw mut freq);
        }
        freq
    }
    #[cfg(not(target_os = "windows"))]
    {
        0
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn stream_relative_returns_zero_for_origin() {
        let origin = StreamTimestamp {
            instant: Instant::now(),
            raw_os_ticks: Some(1000),
            tick_format: TickFormat::RawQpc,
        };
        let anchor = TimestampAnchor::new(origin.clone());
        assert_eq!(anchor.stream_relative(&origin), Duration::ZERO);
    }

    #[test]
    fn hns100_direct_conversion() {
        let now = Instant::now();
        let origin = StreamTimestamp {
            instant: now,
            raw_os_ticks: Some(100),
            tick_format: TickFormat::Hns100,
        };
        let ts = StreamTimestamp {
            instant: now,
            raw_os_ticks: Some(200),
            tick_format: TickFormat::Hns100,
        };
        let anchor = TimestampAnchor::new(origin);
        assert_eq!(anchor.stream_relative(&ts), Duration::from_nanos(10_000));
    }

    #[test]
    fn instant_fallback_when_raw_ticks_absent() {
        let now = Instant::now();
        let origin = StreamTimestamp {
            instant: now,
            raw_os_ticks: None,
            tick_format: TickFormat::RawQpc,
        };
        let ts = StreamTimestamp {
            instant: now,
            raw_os_ticks: None,
            tick_format: TickFormat::RawQpc,
        };
        let anchor = TimestampAnchor::new(origin);
        let dur = anchor.stream_relative(&ts);
        assert!(dur < Duration::from_millis(1));
    }

    #[test]
    fn delta_clamped_to_zero_when_current_before_origin() {
        let now = Instant::now();
        let origin = StreamTimestamp {
            instant: now,
            raw_os_ticks: Some(500),
            tick_format: TickFormat::Hns100,
        };
        let ts = StreamTimestamp {
            instant: now,
            raw_os_ticks: Some(100), // before origin
            tick_format: TickFormat::Hns100,
        };
        let anchor = TimestampAnchor::new(origin);
        assert_eq!(anchor.stream_relative(&ts), Duration::ZERO);
    }

    #[test]
    fn rational_timescales_share_an_anchor_but_epochs_do_not() {
        let now = Instant::now();
        let origin = StreamTimestamp {
            instant: now,
            raw_os_ticks: Some(48_000),
            tick_format: TickFormat::Rational {
                timescale: 48_000,
                domain: ClockDomain::MacHostTime,
                epoch: 0,
            },
        };
        let anchor = TimestampAnchor::new(origin);
        let mut sample = StreamTimestamp {
            instant: now + Duration::from_millis(25),
            raw_os_ticks: Some(180_000),
            tick_format: TickFormat::Rational {
                timescale: 90_000,
                domain: ClockDomain::MacHostTime,
                epoch: 0,
            },
        };
        assert_eq!(anchor.stream_relative(&sample), Duration::from_secs(1));
        sample.tick_format = TickFormat::Rational {
            timescale: 90_000,
            domain: ClockDomain::MacHostTime,
            epoch: 1,
        };
        assert_eq!(anchor.stream_relative(&sample), Duration::from_millis(25));
        sample.tick_format = TickFormat::Hns100;
        assert_eq!(anchor.stream_relative(&sample), Duration::from_millis(25));
    }
    #[test]
    fn full_signed_tick_range_does_not_overflow() {
        let now = Instant::now();
        let origin = StreamTimestamp {
            instant: now,
            raw_os_ticks: Some(i64::MIN),
            tick_format: TickFormat::RawQpc,
        };
        let anchor = TimestampAnchor::new_with_frequency(origin, 1);
        let sample = StreamTimestamp {
            instant: now,
            raw_os_ticks: Some(i64::MAX),
            tick_format: TickFormat::RawQpc,
        };
        assert_eq!(
            anchor.stream_relative(&sample),
            Duration::from_secs(u64::MAX)
        );
    }

    mod prop_tests {
        use super::*;
        use proptest::prelude::*;

        proptest! {
            #![proptest_config(ProptestConfig::with_cases(100))]
            #[test]
            fn prop_rawqpc_conversion(
                origin_ticks in 0i64..i64::MAX / 2,
                delta_ticks in 0i64..i64::MAX / 2,
                frequency in 1i64..10_000_000_000i64,
            ) {
                let current_ticks = origin_ticks + delta_ticks;
                let now = Instant::now();
                let origin = StreamTimestamp {
                    instant: now,
                    raw_os_ticks: Some(origin_ticks),
                    tick_format: TickFormat::RawQpc,
                };
                let ts = StreamTimestamp {
                    instant: now,
                    raw_os_ticks: Some(current_ticks),
                    tick_format: TickFormat::RawQpc,
                };
                let anchor = TimestampAnchor::new_with_frequency(origin, frequency);
                let result = anchor.stream_relative(&ts);

                let expected_secs = delta_ticks / frequency;
                let expected_remainder = delta_ticks % frequency;
                let expected_nanos = (expected_remainder as i128 * 1_000_000_000 / frequency as i128) as u32;
                let expected = Duration::new(expected_secs as u64, expected_nanos);

                let diff = result.abs_diff(expected);
                prop_assert!(
                    diff <= Duration::from_nanos(1),
                    "result {:?} differs from expected {:?} by {:?}",
                    result,
                    expected,
                    diff
                );
            }
        }

        proptest! {
            #![proptest_config(ProptestConfig::with_cases(100))]
            #[test]
            fn prop_hns100_conversion(
                origin_ticks in 0i64..i64::MAX / 2,
                delta_ticks in 0i64..i64::MAX / 2,
            ) {
                let current_ticks = origin_ticks + delta_ticks;
                let now = Instant::now();
                let origin = StreamTimestamp {
                    instant: now,
                    raw_os_ticks: Some(origin_ticks),
                    tick_format: TickFormat::Hns100,
                };
                let ts = StreamTimestamp {
                    instant: now,
                    raw_os_ticks: Some(current_ticks),
                    tick_format: TickFormat::Hns100,
                };
                let anchor = TimestampAnchor::new_with_frequency(origin, 0);
                let result = anchor.stream_relative(&ts);
                let delta = delta_ticks as u64;
                let expected = Duration::from_micros(delta / 10) + Duration::from_nanos((delta % 10) * 100);
                prop_assert_eq!(result, expected);
            }
        }

        proptest! {
            #![proptest_config(ProptestConfig::with_cases(100))]
            #[test]
            fn prop_instant_fallback(
                delta_nanos in 0u64..1_000_000_000u64, // up to 1 second
                use_hns100 in proptest::bool::ANY,
            ) {
                let now = Instant::now();
                let delta = Duration::from_nanos(delta_nanos);
                let later = now + delta;

                let tick_format = if use_hns100 { TickFormat::Hns100 } else { TickFormat::RawQpc };

                let origin = StreamTimestamp {
                    instant: now,
                    raw_os_ticks: None,
                    tick_format,
                };
                let ts = StreamTimestamp {
                    instant: later,
                    raw_os_ticks: None,
                    tick_format,
                };
                let anchor = TimestampAnchor::new_with_frequency(origin, 10_000_000);
                let result = anchor.stream_relative(&ts);

                prop_assert_eq!(result, delta);
            }
        }
    }
}
