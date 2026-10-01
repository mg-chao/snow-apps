use std::time::{Duration, Instant};

use snow_core::timestamp::{StreamTimestamp, TickFormat, TimestampAnchor};

use crate::packet::AudioPacket;

/// A range of source PCM frames admitted to the active recording timeline.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ActivePcmSpan {
    pub source_start_frame: u64,
    pub frames: u64,
    pub timeline_start_frame: u64,
}

/// Clips PCM at precise recording-clock boundaries, reusing the caller's scratch space.
/// Sample instants belong to half-open active intervals; paused samples are never admitted.
pub fn admit_active_pcm(
    clock: &snow_core::recording_clock::RecordingClock,
    started_at: Instant,
    frames: u64,
    sample_rate: u32,
    spans: &mut Vec<ActivePcmSpan>,
) {
    spans.clear();
    if frames == 0 || sample_rate == 0 {
        return;
    }
    let duration = |frames: u64| {
        Duration::from_nanos(
            (u128::from(frames) * 1_000_000_000 / u128::from(sample_rate)).min(u128::from(u64::MAX))
                as u64,
        )
    };
    let Some(end) = started_at.checked_add(duration(frames)) else {
        return;
    };
    clock.visit_active_spans(started_at, end, |span| {
        let frame_at = |at: Instant| {
            (at.saturating_duration_since(started_at).as_nanos() * u128::from(sample_rate))
                .div_ceil(1_000_000_000)
                .min(u128::from(frames)) as u64
        };
        let first = frame_at(span.start);
        let last = frame_at(span.end);
        if first < last {
            let within_span =
                duration(first).saturating_sub(span.start.saturating_duration_since(started_at));
            spans.push(ActivePcmSpan {
                source_start_frame: first,
                frames: last - first,
                timeline_start_frame: duration_to_frames_round(
                    span.timeline_start.saturating_add(within_span),
                    sample_rate,
                ),
            });
        }
    });
}

/// Stream-relative packet time range.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct AudioPacketTimestamp {
    pub start: Duration,
    pub end: Duration,
}

impl AudioPacketTimestamp {
    pub fn duration(&self) -> Duration {
        self.end.saturating_sub(self.start)
    }
}

/// Alignment decision for writing one packet into a stream timeline.
///
/// Consumers should:
/// 1) write `silence_prefix_frames` frames of silence,
/// 2) skip `skip_packet_frames` from packet head,
/// 3) write the remaining `write_packet_frames` payload.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub struct AudioPacketAlignment {
    pub silence_prefix_frames: u64,
    pub skip_packet_frames: u64,
    pub write_packet_frames: u64,
}

impl AudioPacketAlignment {
    pub fn is_empty(&self) -> bool {
        self.write_packet_frames == 0
    }
}

/// Build a [`TimestampAnchor`] from the first audio packet.
///
/// Derives the stream origin from `packet.metadata.stream_timestamp`,
/// adjusting from packet-end to packet-start by subtracting the packet
/// duration.
pub fn audio_anchor_from_first_packet(packet: &AudioPacket) -> TimestampAnchor {
    let end_ts = packet
        .metadata
        .stream_timestamp
        .as_ref()
        .expect("stream_timestamp must be set on every AudioPacket");

    let duration = packet.duration();
    let origin_instant = end_ts
        .instant
        .checked_sub(duration)
        .unwrap_or(end_ts.instant);
    let origin_ticks = end_ts
        .raw_os_ticks
        .map(|t| t.saturating_sub(packet.duration_100ns()));

    TimestampAnchor::new(StreamTimestamp {
        instant: origin_instant,
        raw_os_ticks: origin_ticks,
        tick_format: end_ts.tick_format,
    })
}

/// Build a [`TimestampAnchor`] from a known stream origin instant.
///
/// This is the easiest way to align with `snow-capture`: pass the
/// first video frame's capture instant as the shared origin.
pub fn audio_anchor_from_origin_instant(origin_instant: Instant) -> TimestampAnchor {
    TimestampAnchor::new(StreamTimestamp {
        instant: origin_instant,
        raw_os_ticks: None,
        tick_format: TickFormat::Hns100,
    })
}

/// Build a [`TimestampAnchor`] from a known origin instant and optional QPC value (100ns units).
pub fn audio_anchor_from_origin(
    origin_instant: Instant,
    origin_qpc_100ns: Option<i64>,
) -> TimestampAnchor {
    TimestampAnchor::new(StreamTimestamp {
        instant: origin_instant,
        raw_os_ticks: origin_qpc_100ns,
        tick_format: TickFormat::Hns100,
    })
}

/// Extension trait providing audio-specific convenience methods on [`TimestampAnchor`].
pub trait AudioTimestampAnchorExt {
    /// Convert a packet into stream-relative `[start, end]` time.
    ///
    /// Uses QPC(100ns) when both anchor and packet carry QPC metadata,
    /// otherwise falls back to `Instant` deltas.
    fn audio_stream_relative(&self, packet: &AudioPacket) -> AudioPacketTimestamp;
}

impl AudioTimestampAnchorExt for TimestampAnchor {
    fn audio_stream_relative(&self, packet: &AudioPacket) -> AudioPacketTimestamp {
        let packet_duration = packet.duration();

        let end_ts = packet
            .metadata
            .stream_timestamp
            .as_ref()
            .expect("stream_timestamp must be set on every AudioPacket");

        let end = self.stream_relative(end_ts);

        AudioPacketTimestamp {
            start: end.saturating_sub(packet_duration),
            end,
        }
    }
}

/// Convert a duration into sample frames with symmetric rounding.
pub fn duration_to_frames_round(duration: Duration, sample_rate_hz: u32) -> u64 {
    let nanos = duration.as_nanos();
    let scaled = nanos.saturating_mul(u128::from(sample_rate_hz.max(1)));
    let rounded = scaled.saturating_add(500_000_000) / 1_000_000_000;
    rounded.min(u64::MAX as u128) as u64
}

/// Compute how to place one packet onto a stream timeline.
///
/// `written_frames` is the number of frames already emitted to the output
/// track before this packet is processed.
pub fn align_packet_frames(
    written_frames: u64,
    sample_rate_hz: u32,
    packet_ts: AudioPacketTimestamp,
    packet_frames: u64,
) -> AudioPacketAlignment {
    if packet_frames == 0 {
        return AudioPacketAlignment::default();
    }

    let desired_start = duration_to_frames_round(packet_ts.start, sample_rate_hz);
    let silence_prefix_frames = desired_start.saturating_sub(written_frames);
    let overlap_frames = written_frames.saturating_sub(desired_start);

    if overlap_frames >= packet_frames {
        return AudioPacketAlignment {
            silence_prefix_frames,
            skip_packet_frames: packet_frames,
            write_packet_frames: 0,
        };
    }

    AudioPacketAlignment {
        silence_prefix_frames,
        skip_packet_frames: overlap_frames,
        write_packet_frames: packet_frames.saturating_sub(overlap_frames),
    }
}

/// Pad or truncate interleaved i16 samples to exactly target duration.
pub fn align_i16_interleaved_to_duration(
    mut samples: Vec<i16>,
    sample_rate_hz: u32,
    channels: u16,
    target_duration: Duration,
) -> Vec<i16> {
    let target_frames = duration_to_frames_round(target_duration, sample_rate_hz) as usize;
    let target_samples = target_frames.saturating_mul(usize::from(channels.max(1)));
    if samples.len() < target_samples {
        samples.resize(target_samples, 0);
    } else {
        samples.truncate(target_samples);
    }
    samples
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::format::AudioFormat;
    use crate::packet::{AudioPacketMetadata, AudioSourceKind};

    #[test]
    fn active_pcm_maps_multiple_boundaries_to_exact_source_and_output_frames() {
        let start = Instant::now();
        let clock = snow_core::recording_clock::RecordingClock::new(start);
        let controller = clock.controller();
        controller.mark_pause(start);
        controller.mark_resume(start + Duration::from_millis(5));
        controller.mark_pause(start + Duration::from_millis(15));
        controller.mark_resume(start + Duration::from_millis(25));
        controller.mark_pause(start + Duration::from_millis(35));
        let mut spans = Vec::with_capacity(2);
        admit_active_pcm(&clock, start, 1_920, 48_000, &mut spans);
        assert_eq!(
            spans,
            [
                ActivePcmSpan {
                    source_start_frame: 240,
                    frames: 480,
                    timeline_start_frame: 0,
                },
                ActivePcmSpan {
                    source_start_frame: 1_200,
                    frames: 480,
                    timeline_start_frame: 480,
                },
            ]
        );
        let capacity = spans.capacity();
        admit_active_pcm(
            &clock,
            start + Duration::from_millis(40),
            960,
            48_000,
            &mut spans,
        );
        assert!(spans.is_empty());
        assert_eq!(spans.capacity(), capacity);
    }

    #[test]
    fn active_pcm_uses_half_open_subsample_pause_boundaries() {
        let start = Instant::now();
        let clock = snow_core::recording_clock::RecordingClock::new(start);
        let controller = clock.controller();
        controller.mark_pause(start);
        controller.mark_resume(start + Duration::from_micros(2_501));
        controller.mark_pause(start + Duration::from_micros(7_501));
        let mut spans = Vec::new();
        admit_active_pcm(&clock, start, 480, 48_000, &mut spans);
        assert_eq!(
            spans,
            [ActivePcmSpan {
                source_start_frame: 121,
                frames: 240,
                timeline_start_frame: 1,
            }]
        );
    }

    fn packet(frames: u32) -> AudioPacket {
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
    fn stream_relative_prefers_qpc_when_available() {
        let anchor = audio_anchor_from_origin(Instant::now(), Some(1_000_000));

        let mut pkt = packet(480); // 10ms at 48kHz.
        pkt.metadata.set_timing(None, Some(1_500_000));

        let ts = anchor.audio_stream_relative(&pkt);
        assert_eq!(ts.end, Duration::from_millis(50));
        assert_eq!(ts.start, Duration::from_millis(40));
        assert_eq!(ts.duration(), Duration::from_millis(10));
    }

    #[test]
    fn stream_relative_falls_back_to_instant_when_qpc_absent() {
        let origin = Instant::now();
        let anchor = audio_anchor_from_origin_instant(origin);

        let mut pkt = packet(480); // 10ms.
        pkt.metadata
            .set_timing(origin.checked_add(Duration::from_millis(30)), None);

        let ts = anchor.audio_stream_relative(&pkt);
        assert_eq!(ts.end, Duration::from_millis(30));
        assert_eq!(ts.start, Duration::from_millis(20));
    }

    #[test]
    fn align_packet_frames_inserts_silence_when_packet_starts_late() {
        let plan = align_packet_frames(
            0,
            48_000,
            AudioPacketTimestamp {
                start: Duration::from_millis(20),
                end: Duration::from_millis(30),
            },
            480,
        );

        assert_eq!(plan.silence_prefix_frames, 960);
        assert_eq!(plan.skip_packet_frames, 0);
        assert_eq!(plan.write_packet_frames, 480);
    }

    #[test]
    fn align_packet_frames_trims_fully_overlapped_packet() {
        let plan = align_packet_frames(
            1_920,
            48_000,
            AudioPacketTimestamp {
                start: Duration::from_millis(20),
                end: Duration::from_millis(30),
            },
            480,
        );

        assert_eq!(plan.silence_prefix_frames, 0);
        assert_eq!(plan.skip_packet_frames, 480);
        assert_eq!(plan.write_packet_frames, 0);
        assert!(plan.is_empty());
    }

    #[test]
    fn align_i16_interleaved_to_duration_pads_or_truncates() {
        let padded =
            align_i16_interleaved_to_duration(vec![1; 20], 10, 2, Duration::from_millis(1_500));
        assert_eq!(padded.len(), 30);

        let truncated =
            align_i16_interleaved_to_duration(vec![1; 40], 10, 2, Duration::from_millis(1_500));
        assert_eq!(truncated.len(), 30);
    }
}
