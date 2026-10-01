//! Shared gain targets and consumable post-gain peak measurements.
use std::sync::{
    Arc,
    atomic::{AtomicI32, AtomicU8, AtomicU64, Ordering},
};
use std::time::Instant;

use crate::{AudioError, AudioPacket, AudioResult, AudioSourceKind};

pub const MIN_GAIN_DB: i32 = -24;
pub const MAX_GAIN_DB: i32 = 24;
pub const METER_STALE_MS: u64 = 200;
const PEAK_MASK: u64 = 0xffff;
const CLIPPED: u64 = 1 << 16;
const HAS_PACKET: u64 = 1 << 17;
const STATUS_SHIFT: u32 = 18;
const STATUS_MASK: u64 = 7 << STATUS_SHIFT;
const TIME_SHIFT: u32 = 21;
const TIME_MAX: u64 = u64::MAX >> TIME_SHIFT;
const INTERVAL_MASK: u64 = PEAK_MASK | CLIPPED;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum AudioSourceStatus {
    #[default]
    Disabled = 0,
    Starting = 1,
    Ready = 2,
    Reconnecting = 3,
    Unavailable = 4,
    PermissionDenied = 5,
    Stopped = 6,
}

impl AudioSourceStatus {
    fn from_word(word: u64) -> Self {
        match (word & STATUS_MASK) >> STATUS_SHIFT {
            1 => Self::Starting,
            2 => Self::Ready,
            3 => Self::Reconnecting,
            4 => Self::Unavailable,
            5 => Self::PermissionDenied,
            6 => Self::Stopped,
            _ => Self::Disabled,
        }
    }
}

#[derive(Clone, Copy, Debug)]
pub struct AudioLevelSnapshot {
    /// Maximum processed PCM magnitude since the previous take, normalized to 0..=1.
    pub peak: f32,
    pub clipped: bool,
    pub status: AudioSourceStatus,
    /// u64::MAX means no measured packet has arrived.
    pub age_ms: u64,
}

#[derive(Clone, Copy, Debug)]
pub struct AudioLevelsSnapshot {
    pub system: AudioLevelSnapshot,
    pub microphone: AudioLevelSnapshot,
}

#[derive(Debug)]
struct SourceControl {
    gain_db: AtomicI32,
    meter: AtomicU64,
}

#[derive(Debug)]
struct Controls {
    sources: [SourceControl; 2],
    metering: AtomicU8,
    origin: Instant,
}

#[derive(Clone, Debug)]
pub struct AudioControlHandle(Arc<Controls>);

pub(crate) fn source_index(source: AudioSourceKind) -> usize {
    usize::from(source == AudioSourceKind::Microphone)
}

impl Default for AudioControlHandle {
    fn default() -> Self {
        Self::new()
    }
}

impl AudioControlHandle {
    pub fn new() -> Self {
        Self(Arc::new(Controls {
            sources: std::array::from_fn(|_| SourceControl {
                gain_db: AtomicI32::new(0),
                meter: AtomicU64::new(0),
            }),
            metering: AtomicU8::new(0),
            origin: Instant::now(),
        }))
    }

    pub fn set_gain_db(&self, source: AudioSourceKind, gain_db: i32) -> AudioResult<()> {
        if !(MIN_GAIN_DB..=MAX_GAIN_DB).contains(&gain_db) {
            return Err(AudioError::InvalidConfig(
                "audio gain must be in -24..=24 dB".into(),
            ));
        }
        self.0.sources[source_index(source)]
            .gain_db
            .store(gain_db, Ordering::Release);
        Ok(())
    }

    pub fn gain_db(&self, source: AudioSourceKind) -> i32 {
        self.0.sources[source_index(source)]
            .gain_db
            .load(Ordering::Acquire)
    }

    pub fn set_metering(&self, source: AudioSourceKind, enabled: bool) {
        let bit = 1 << source_index(source);
        if enabled {
            if self.0.metering.load(Ordering::Acquire) & bit == 0 {
                self.0.sources[source_index(source)]
                    .meter
                    .fetch_and(!(INTERVAL_MASK | HAS_PACKET), Ordering::AcqRel);
            }
            self.0.metering.fetch_or(bit, Ordering::Release);
        } else {
            self.0.metering.fetch_and(!bit, Ordering::AcqRel);
            self.0.sources[source_index(source)]
                .meter
                .fetch_and(!(INTERVAL_MASK | HAS_PACKET), Ordering::AcqRel);
        }
    }

    pub(crate) fn metering_enabled(&self, source: AudioSourceKind) -> bool {
        self.0.metering.load(Ordering::Acquire) & (1 << source_index(source)) != 0
    }

    pub(crate) fn any_metering(&self) -> bool {
        self.0.metering.load(Ordering::Acquire) != 0
    }

    /// Publish backend readiness even when a silent endpoint emits no packets.
    pub fn set_source_status(&self, source: AudioSourceKind, status: AudioSourceStatus) {
        let meter = &self.0.sources[source_index(source)].meter;
        let _ = meter.fetch_update(Ordering::AcqRel, Ordering::Acquire, |old| {
            if AudioSourceStatus::from_word(old) == status {
                return None;
            }
            Some(
                (old & !(STATUS_MASK | INTERVAL_MASK | HAS_PACKET))
                    | ((status as u64) << STATUS_SHIFT),
            )
        });
    }

    pub(crate) fn initialize_sources(&self, system: bool, microphone: bool) {
        for (source, enabled) in [
            (AudioSourceKind::System, system),
            (AudioSourceKind::Microphone, microphone),
        ] {
            self.set_source_status(
                source,
                if enabled {
                    AudioSourceStatus::Starting
                } else {
                    AudioSourceStatus::Disabled
                },
            );
        }
    }

    pub fn mark_stopped(&self) {
        for source in [AudioSourceKind::System, AudioSourceKind::Microphone] {
            let status = AudioSourceStatus::from_word(
                self.0.sources[source_index(source)]
                    .meter
                    .load(Ordering::Acquire),
            );
            if matches!(
                status,
                AudioSourceStatus::Starting
                    | AudioSourceStatus::Ready
                    | AudioSourceStatus::Reconnecting
            ) {
                self.set_source_status(source, AudioSourceStatus::Stopped);
            }
        }
    }

    pub(crate) fn mark_error(&self, error: &AudioError) {
        let status = if matches!(error, AudioError::AccessDenied) {
            AudioSourceStatus::PermissionDenied
        } else if matches!(error, AudioError::Canceled) {
            AudioSourceStatus::Stopped
        } else {
            AudioSourceStatus::Unavailable
        };
        for source in [AudioSourceKind::System, AudioSourceKind::Microphone] {
            let current = AudioSourceStatus::from_word(
                self.0.sources[source_index(source)]
                    .meter
                    .load(Ordering::Acquire),
            );
            if current != AudioSourceStatus::Disabled {
                if current == AudioSourceStatus::PermissionDenied
                    && status == AudioSourceStatus::Unavailable
                {
                    continue;
                }
                self.set_source_status(source, status);
            }
        }
    }

    /// A backend may have already reported distinct source failures before
    /// returning a session-wide error. Fill only sources it has not classified.
    pub(crate) fn mark_initialization_error(&self, error: &AudioError) {
        let status = match error {
            AudioError::AccessDenied => AudioSourceStatus::PermissionDenied,
            AudioError::Canceled => AudioSourceStatus::Stopped,
            _ => AudioSourceStatus::Unavailable,
        };
        for source in [AudioSourceKind::System, AudioSourceKind::Microphone] {
            let current = AudioSourceStatus::from_word(
                self.0.sources[source_index(source)]
                    .meter
                    .load(Ordering::Acquire),
            );
            if current == AudioSourceStatus::Starting {
                self.set_source_status(source, status);
            }
        }
    }

    pub(crate) fn publish(&self, source: AudioSourceKind, peak: u16, clipped: bool, at: Instant) {
        if !self.metering_enabled(source) {
            return;
        }
        let millis = at
            .saturating_duration_since(self.0.origin)
            .as_millis()
            .min(u128::from(TIME_MAX)) as u64;
        let meter = &self.0.sources[source_index(source)].meter;
        let _ = meter.fetch_update(Ordering::AcqRel, Ordering::Acquire, |old| {
            if !self.metering_enabled(source) {
                return None;
            }
            Some(
                (millis << TIME_SHIFT)
                    | ((AudioSourceStatus::Ready as u64) << STATUS_SHIFT)
                    | HAS_PACKET
                    | (old & CLIPPED)
                    | if clipped { CLIPPED } else { 0 }
                    | (old & PEAK_MASK).max(u64::from(peak)),
            )
        });
    }

    /// Consumes interval peaks. Use one consumer and distribute its snapshot to all views.
    pub fn take_levels(&self) -> AudioLevelsSnapshot {
        self.take_levels_at(Instant::now())
    }

    fn take_levels_at(&self, now: Instant) -> AudioLevelsSnapshot {
        let millis = now
            .saturating_duration_since(self.0.origin)
            .as_millis()
            .min(u128::from(TIME_MAX)) as u64;
        let take = |source| {
            // CAS updates merge peak and clipping together; fetch_and is the matching
            // linearization point, so a racing packet remains in this or the next interval.
            let word = self.0.sources[source_index(source)]
                .meter
                .fetch_and(!INTERVAL_MASK, Ordering::AcqRel);
            let status = AudioSourceStatus::from_word(word);
            let age_ms = if word & HAS_PACKET == 0 {
                u64::MAX
            } else {
                millis.saturating_sub(word >> TIME_SHIFT)
            };
            let valid = status == AudioSourceStatus::Ready
                && age_ms <= METER_STALE_MS
                && self.metering_enabled(source);
            AudioLevelSnapshot {
                peak: if valid {
                    (word & PEAK_MASK) as f32 / 32768.0
                } else {
                    0.0
                },
                clipped: valid && word & CLIPPED != 0,
                status,
                age_ms,
            }
        };
        AudioLevelsSnapshot {
            system: take(AudioSourceKind::System),
            microphone: take(AudioSourceKind::Microphone),
        }
    }
}

pub(crate) struct GainProcessor {
    gain_db: i32,
    current: f32,
    target: f32,
    step: f32,
    remaining: u32,
}

pub(crate) struct Measurement {
    pub peak: u16,
    pub clipped: bool,
    pub at: Instant,
}

impl GainProcessor {
    pub(crate) fn new(gain_db: i32) -> Self {
        let gain = 10.0_f32.powf(gain_db as f32 / 20.0);
        Self {
            gain_db,
            current: gain,
            target: gain,
            step: 0.0,
            remaining: 0,
        }
    }

    pub(crate) fn process(
        &mut self,
        packet: &mut AudioPacket,
        control: &AudioControlHandle,
    ) -> Option<Measurement> {
        let channels = usize::from(packet.format.channels);
        if channels == 0
            || packet.format.sample_rate == 0
            || packet.data.len() != packet.frames as usize * channels
        {
            return None;
        }
        let target_db = control.gain_db(packet.source);
        if self.gain_db != target_db {
            self.gain_db = target_db;
            self.target = 10.0_f32.powf(target_db as f32 / 20.0);
            self.remaining = (packet.format.sample_rate / 50).max(1);
            self.step = (self.target - self.current) / self.remaining as f32;
        }
        let meter = control.metering_enabled(packet.source);
        if self.remaining == 0 && self.current == 1.0 {
            return meter.then(|| Measurement {
                peak: packet
                    .data
                    .iter()
                    .map(|sample| sample.unsigned_abs())
                    .max()
                    .unwrap_or(0),
                clipped: false,
                at: packet.end_capture_time().unwrap_or_else(Instant::now),
            });
        }
        let mut peak = 0;
        let mut clipped = false;
        for frame in packet.data.chunks_exact_mut(channels) {
            if self.remaining > 0 {
                self.remaining -= 1;
                self.current = if self.remaining == 0 {
                    self.target
                } else {
                    self.current + self.step
                };
            }
            for sample in frame {
                let scaled = f32::from(*sample) * self.current;
                clipped |= scaled < f32::from(i16::MIN) || scaled > f32::from(i16::MAX);
                *sample = scaled
                    .round()
                    .clamp(f32::from(i16::MIN), f32::from(i16::MAX))
                    as i16;
                if meter {
                    peak = peak.max(sample.unsigned_abs());
                }
            }
        }
        meter.then(|| Measurement {
            peak,
            clipped,
            at: packet.end_capture_time().unwrap_or_else(Instant::now),
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{AudioFormat, AudioPacketMetadata};
    use std::time::Duration;

    fn packet(samples: Vec<i16>, channels: u16) -> AudioPacket {
        AudioPacket {
            source: AudioSourceKind::System,
            frames: (samples.len() / usize::from(channels)) as u32,
            format: AudioFormat::new(48_000, channels),
            data: samples,
            metadata: AudioPacketMetadata::default(),
        }
    }

    #[test]
    fn unity_preserves_every_pcm_value_and_gain_bounds_are_checked() {
        let control = AudioControlHandle::new();
        assert!(control.set_gain_db(AudioSourceKind::System, -25).is_err());
        assert!(control.set_gain_db(AudioSourceKind::System, 25).is_err());
        let original: Vec<_> = (i16::MIN..=i16::MAX).collect();
        let mut input = packet(original.clone(), 2);
        assert!(
            GainProcessor::new(0)
                .process(&mut input, &control)
                .is_none()
        );
        assert_eq!(input.data, original);
    }

    #[test]
    fn scaled_samples_meter_saturation_and_sources_are_independent() {
        let control = AudioControlHandle::new();
        control.set_gain_db(AudioSourceKind::System, 24).unwrap();
        control.set_metering(AudioSourceKind::System, true);
        assert_eq!(control.gain_db(AudioSourceKind::Microphone), 0);
        let mut input = packet(vec![i16::MIN, i16::MAX, 100, -100], 2);
        let measured = GainProcessor::new(24)
            .process(&mut input, &control)
            .unwrap();
        assert_eq!(&input.data[..2], &[i16::MIN, i16::MAX]);
        assert_eq!(measured.peak, 32768);
        assert!(measured.clipped);
        control.publish(input.source, measured.peak, measured.clipped, measured.at);
        let level = control.take_levels().system;
        assert_eq!(level.peak, 1.0);
        assert!(level.clipped);
        assert_eq!(control.take_levels().system.peak, 0.0);
        control.set_gain_db(AudioSourceKind::System, -24).unwrap();
        let mut input = packet(vec![10000, -10000], 2);
        GainProcessor::new(-24).process(&mut input, &control);
        assert_eq!(input.data, vec![631, -631]);
    }

    #[test]
    fn gain_ramp_is_frame_based_continuous_across_packets_and_reaches_target() {
        let control = AudioControlHandle::new();
        let mut processor = GainProcessor::new(0);
        control.set_gain_db(AudioSourceKind::System, 6).unwrap();
        let mut first = packet(vec![1000; 600], 2);
        let mut second = packet(vec![1000; 1320], 2);
        processor.process(&mut first, &control);
        processor.process(&mut second, &control);
        assert!(first.data[0] > 1000 && first.data[0] < 1003);
        assert!(first.data[598] <= second.data[0]);
        assert_eq!(second.data[1318], 1995);
        for frame in first
            .data
            .chunks_exact(2)
            .chain(second.data.chunks_exact(2))
        {
            assert_eq!(frame[0], frame[1]);
        }
        control.set_gain_db(AudioSourceKind::System, 0).unwrap();
        let mut unity = packet(vec![1000; 1920], 2);
        processor.process(&mut unity, &control);
        assert_eq!(unity.data[1918], 1000);
        assert_eq!(processor.current, 1.0);
    }

    #[test]
    fn interval_max_retains_short_peaks_and_stale_data_is_zero_without_losing_status() {
        let control = AudioControlHandle::new();
        control.set_metering(AudioSourceKind::System, true);
        let now = Instant::now();
        control.publish(AudioSourceKind::System, 32000, true, now);
        control.publish(AudioSourceKind::System, 1, false, now);
        let level = control.take_levels_at(now).system;
        assert_eq!(level.peak, 32000.0 / 32768.0);
        assert!(level.clipped);
        control.publish(AudioSourceKind::System, 100, false, now);
        let stale = control
            .take_levels_at(now + Duration::from_millis(201))
            .system;
        assert_eq!(stale.peak, 0.0);
        assert_eq!(stale.status, AudioSourceStatus::Ready);
        control.set_source_status(AudioSourceKind::System, AudioSourceStatus::Unavailable);
        assert_eq!(
            control.take_levels().system.status,
            AudioSourceStatus::Unavailable
        );
    }

    #[test]
    fn concurrent_takes_cannot_lose_a_single_packet_peak() {
        for _ in 0..100 {
            let control = AudioControlHandle::new();
            control.set_metering(AudioSourceKind::System, true);
            let barrier = Arc::new(std::sync::Barrier::new(2));
            let worker_control = control.clone();
            let worker_barrier = barrier.clone();
            let worker = std::thread::spawn(move || {
                worker_barrier.wait();
                worker_control.publish(AudioSourceKind::System, 32768, true, Instant::now());
            });
            barrier.wait();
            let first = control.take_levels().system;
            worker.join().unwrap();
            let second = control.take_levels().system;
            assert_eq!(first.peak.max(second.peak), 1.0);
            assert!(first.clipped || second.clipped);
        }
    }
}
