use std::time::{Duration, Instant};

use crate::format::AudioFormat;
use snow_core::event::{DeliveryLane, StreamEvent};
use snow_core::timestamp::StreamTimestamp;
#[cfg(any(windows, target_os = "macos", test))]
use snow_core::timestamp::TickFormat;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AudioSourceKind {
    System,
    Microphone,
}

#[derive(Clone, Debug, Default)]
pub struct AudioPacketMetadata {
    /// Device position in source frames at packet end.
    pub device_position_frames: Option<u64>,
    pub discontinuity: bool,
    pub is_silent: bool,
    pub sequence: u64,
    /// Unified timestamp. `tick_format` is `Hns100`.
    pub stream_timestamp: Option<StreamTimestamp>,
}

impl AudioPacketMetadata {
    #[cfg(any(windows, target_os = "macos", test))]
    pub(crate) fn set_timing(
        &mut self,
        capture_time: Option<Instant>,
        qpc_position_100ns: Option<i64>,
    ) {
        self.stream_timestamp = Some(StreamTimestamp {
            instant: capture_time.unwrap_or_else(Instant::now),
            raw_os_ticks: qpc_position_100ns,
            tick_format: TickFormat::Hns100,
        });
    }
}

#[derive(Clone, Debug)]
pub struct AudioPacket {
    pub source: AudioSourceKind,
    pub format: AudioFormat,
    pub frames: u32,
    pub data: Vec<i16>,
    pub metadata: AudioPacketMetadata,
}

impl AudioPacket {
    pub fn duration(&self) -> Duration {
        frames_to_duration(self.frames, self.format.sample_rate)
    }

    pub fn duration_100ns(&self) -> i64 {
        frames_to_100ns(self.frames, self.format.sample_rate)
    }

    pub fn end_capture_time(&self) -> Option<Instant> {
        self.metadata.stream_timestamp.as_ref().map(|st| st.instant)
    }

    pub fn start_capture_time(&self) -> Option<Instant> {
        self.end_capture_time()
            .and_then(|end| end.checked_sub(self.duration()))
    }

    pub fn end_qpc_position_100ns(&self) -> Option<i64> {
        self.metadata
            .stream_timestamp
            .as_ref()
            .and_then(|st| st.raw_os_ticks)
    }

    pub fn start_qpc_position_100ns(&self) -> Option<i64> {
        self.end_qpc_position_100ns()
            .map(|end| end.saturating_sub(self.duration_100ns()))
    }

    pub fn sample_count(&self) -> usize {
        self.data.len()
    }

    pub fn as_i16_slice(&self) -> &[i16] {
        &self.data
    }
}

pub(crate) fn frames_to_duration(frames: u32, sample_rate: u32) -> Duration {
    if frames == 0 || sample_rate == 0 {
        return Duration::ZERO;
    }
    let nanos = u128::from(frames)
        .saturating_mul(1_000_000_000u128)
        .checked_div(u128::from(sample_rate))
        .unwrap_or(0);
    Duration::from_nanos(nanos.min(u128::from(u64::MAX)) as u64)
}

pub(crate) fn frames_to_100ns(frames: u32, sample_rate: u32) -> i64 {
    if frames == 0 || sample_rate == 0 {
        return 0;
    }
    let hns = u128::from(frames)
        .saturating_mul(10_000_000u128)
        .checked_div(u128::from(sample_rate))
        .unwrap_or(0);
    hns.min(i64::MAX as u128) as i64
}

#[derive(Clone, Debug)]
pub enum AudioEvent {
    Packet(AudioPacket),
    PacketDropped {
        source: AudioSourceKind,
        dropped_frames: u64,
    },
    SourceRestarted {
        source: AudioSourceKind,
        old_device_id: Option<String>,
        new_device_id: String,
        downtime: Duration,
    },
    Paused {
        at: Instant,
    },
    Resumed {
        at: Instant,
        gap: Duration,
    },
    StreamEnded,
    Error(crate::error::AudioError),
}

impl StreamEvent for AudioEvent {
    fn is_paused(&self) -> bool {
        matches!(self, AudioEvent::Paused { .. })
    }

    fn is_resumed(&self) -> bool {
        matches!(self, AudioEvent::Resumed { .. })
    }

    fn is_stream_ended(&self) -> bool {
        matches!(self, AudioEvent::StreamEnded)
    }

    fn is_error(&self) -> bool {
        matches!(self, AudioEvent::Error(_))
    }

    fn delivery_lane(&self) -> DeliveryLane {
        match self {
            AudioEvent::Packet(_) => DeliveryLane::Data,
            AudioEvent::PacketDropped { .. }
            | AudioEvent::SourceRestarted { .. }
            | AudioEvent::Paused { .. }
            | AudioEvent::Resumed { .. }
            | AudioEvent::StreamEnded
            | AudioEvent::Error(_) => DeliveryLane::Control,
        }
    }

    fn timestamp(&self) -> Option<&StreamTimestamp> {
        match self {
            AudioEvent::Packet(packet) => packet.metadata.stream_timestamp.as_ref(),
            _ => None,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use proptest::prelude::*;

    fn make_packet(frames: u32) -> AudioPacket {
        let format = AudioFormat::new(48_000, 2);
        let samples = format
            .samples_for_frames(frames)
            .expect("test format should be valid");
        AudioPacket {
            source: AudioSourceKind::System,
            format,
            frames,
            data: vec![0; samples],
            metadata: AudioPacketMetadata::default(),
        }
    }

    #[test]
    fn duration_helpers_match_packet_length() {
        let packet = make_packet(480);
        assert_eq!(packet.duration(), Duration::from_millis(10));
        assert_eq!(packet.duration_100ns(), 100_000);
        assert_eq!(packet.sample_count(), 960);
    }

    #[test]
    fn capture_time_helpers_subtract_duration() {
        let mut packet = make_packet(480);
        let end = Instant::now();
        packet.metadata.set_timing(Some(end), Some(1_000_000));

        assert_eq!(packet.end_capture_time(), Some(end));
        assert_eq!(
            packet.start_capture_time(),
            end.checked_sub(Duration::from_millis(10))
        );
        assert_eq!(packet.end_qpc_position_100ns(), Some(1_000_000));
        assert_eq!(packet.start_qpc_position_100ns(), Some(900_000));
    }

    proptest! {
        #[test]
        fn frame_duration_roundtrip_is_monotonic(frames in 0u32..10_000u32) {
            let duration = frames_to_duration(frames, 48_000);
            prop_assert!(duration >= Duration::ZERO);
            prop_assert!(frames_to_100ns(frames, 48_000) >= 0);
        }
    }
}
