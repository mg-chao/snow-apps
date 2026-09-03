use std::fs::{File, create_dir_all};
use std::io::{BufWriter, Write};
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread::JoinHandle;
use std::time::Duration;

use snow_core::recording_clock::RecordingClock;
use snow_core::timestamp::TimestampAnchor;
use snow_recording_model::{
    AudioSampleFormat, AudioTrackManifest, AudioTrackRole, AudioTrackRole::*,
};

use crate::device::DeviceSelector;
use crate::error::{AudioError, AudioResult};
use crate::format::AudioFormat;
use crate::packet::{AudioEvent, AudioPacket, AudioSourceKind};
use crate::session::{AudioSession, AudioStreamConfig, SourceConfig};
use crate::timeline::{
    AudioPacketAlignment, AudioPacketTimestamp, AudioTimestampAnchorExt, align_packet_frames,
    audio_anchor_from_first_packet,
};

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum AudioTrackDevice {
    SystemDefault,
    MicrophoneDefault,
    InputDeviceId(String),
}

#[derive(Clone, Debug)]
pub struct AudioTrackConfig {
    pub track_id: String,
    pub role: AudioTrackRole,
    pub device: AudioTrackDevice,
    pub enabled: bool,
}

impl AudioTrackConfig {
    pub fn system_default(track_id: impl Into<String>) -> Self {
        Self {
            track_id: track_id.into(),
            role: AudioTrackRole::SystemOutput,
            device: AudioTrackDevice::SystemDefault,
            enabled: true,
        }
    }

    pub fn microphone_default(track_id: impl Into<String>) -> Self {
        Self {
            track_id: track_id.into(),
            role: AudioTrackRole::MicrophoneInput,
            device: AudioTrackDevice::MicrophoneDefault,
            enabled: true,
        }
    }
}

#[derive(Clone, Debug)]
pub struct AudioRecordingConfig {
    pub output_dir: PathBuf,
    pub sample_rate_hz: u32,
    pub channels: u16,
    pub packet_duration: Duration,
    pub event_buffer_depth: usize,
    pub tracks: Vec<AudioTrackConfig>,
}

impl Default for AudioRecordingConfig {
    fn default() -> Self {
        Self {
            output_dir: PathBuf::from("."),
            sample_rate_hz: 48_000,
            channels: 2,
            packet_duration: Duration::from_millis(20),
            event_buffer_depth: 128,
            tracks: vec![
                AudioTrackConfig::system_default("system"),
                AudioTrackConfig::microphone_default("microphone"),
            ],
        }
    }
}

impl AudioRecordingConfig {
    pub fn validate(&self) -> AudioResult<()> {
        AudioFormat::new(self.sample_rate_hz, self.channels).validate()?;
        if self.packet_duration.is_zero() {
            return Err(AudioError::InvalidConfig(
                "audio packet duration must be greater than zero".into(),
            ));
        }
        if self.event_buffer_depth == 0 {
            return Err(AudioError::InvalidConfig(
                "audio event_buffer_depth must be greater than zero".into(),
            ));
        }
        if self.tracks.is_empty() {
            return Err(AudioError::InvalidConfig(
                "at least one audio track must be configured".into(),
            ));
        }
        if self.output_dir.as_os_str().is_empty() {
            return Err(AudioError::InvalidConfig(
                "audio output_dir must not be empty".into(),
            ));
        }

        let mut seen_track_ids = std::collections::HashSet::new();
        let mut system_track = 0usize;
        let mut microphone_track = 0usize;
        for track in self.tracks.iter().filter(|track| track.enabled) {
            if !seen_track_ids.insert(track.track_id.clone()) {
                return Err(AudioError::InvalidConfig(format!(
                    "duplicate audio track_id {}",
                    track.track_id
                )));
            }
            match track.role {
                SystemOutput => system_track += 1,
                MicrophoneInput => microphone_track += 1,
                Auxiliary => {
                    return Err(AudioError::InvalidConfig(
                        "auxiliary audio tracks are not supported by the current backend".into(),
                    ));
                }
            }
        }

        if system_track > 1 {
            return Err(AudioError::InvalidConfig(
                "only one system output track is currently supported".into(),
            ));
        }
        if microphone_track > 1 {
            return Err(AudioError::InvalidConfig(
                "only one microphone input track is currently supported".into(),
            ));
        }

        Ok(())
    }
}

#[derive(Clone, Debug)]
pub struct RecordedAudioTrack {
    pub manifest: AudioTrackManifest,
    pub path: PathBuf,
}

#[derive(Clone, Debug, Default)]
pub struct AudioRecordingArtifact {
    pub tracks: Vec<RecordedAudioTrack>,
}

pub struct AudioRecordingSession {
    stop_flag: Arc<AtomicBool>,
    pause_flag: Arc<AtomicBool>,
    join_handle: Option<JoinHandle<AudioResult<AudioRecordingArtifact>>>,
}

impl AudioRecordingSession {
    pub fn start(config: AudioRecordingConfig, clock: RecordingClock) -> AudioResult<Self> {
        config.validate()?;
        create_dir_all(&config.output_dir).map_err(AudioError::platform)?;

        let session = AudioSession::new()?;
        let stream_config = build_stream_config(&config);
        let stream = session.start_streaming(stream_config)?;
        let stop_flag = Arc::new(AtomicBool::new(false));
        let pause_flag = Arc::new(AtomicBool::new(false));
        let worker_stop = Arc::clone(&stop_flag);
        let worker_pause = Arc::clone(&pause_flag);

        let join_handle = std::thread::Builder::new()
            .name("snow-audio-recording".to_string())
            .spawn(move || {
                run_audio_recording_worker(stream, config, clock, worker_stop, worker_pause)
            })
            .map_err(|err| {
                AudioError::platform(anyhow::anyhow!(
                    "failed to spawn audio recording worker: {err}"
                ))
            })?;

        Ok(Self {
            stop_flag,
            pause_flag,
            join_handle: Some(join_handle),
        })
    }

    /// Pause delivery from the audio stream while keeping the recording
    /// worker alive. The worker emits a control event that causes the writer
    /// to re-anchor the next packet on resume.
    pub fn pause(&self) {
        self.pause_flag.store(true, Ordering::Release);
    }

    pub fn resume(&self) {
        self.pause_flag.store(false, Ordering::Release);
    }

    pub fn finish(mut self) -> AudioResult<AudioRecordingArtifact> {
        self.stop_flag.store(true, Ordering::Release);
        let handle = self.join_handle.take().ok_or(AudioError::WorkerDead)?;
        handle.join().map_err(|_| {
            AudioError::platform(anyhow::anyhow!("audio recording worker thread panicked"))
        })?
    }
}

impl Drop for AudioRecordingSession {
    fn drop(&mut self) {
        self.stop_flag.store(true, Ordering::Release);
        if let Some(handle) = self.join_handle.take() {
            let _ = handle.join();
        }
    }
}

fn build_stream_config(config: &AudioRecordingConfig) -> AudioStreamConfig {
    let format = AudioFormat::new(config.sample_rate_hz, config.channels);
    let system_track = config
        .tracks
        .iter()
        .find(|track| track.enabled && matches!(track.role, SystemOutput));
    let microphone_track = config
        .tracks
        .iter()
        .find(|track| track.enabled && matches!(track.role, MicrophoneInput));

    AudioStreamConfig {
        event_buffer_depth: config.event_buffer_depth,
        system: SourceConfig {
            enabled: system_track.is_some(),
            required: false,
            device: DeviceSelector::DefaultRender,
            output_format: format,
            packet_duration: config.packet_duration,
        },
        microphone: SourceConfig {
            enabled: microphone_track.is_some(),
            required: false,
            device: microphone_track
                .map(resolve_input_device)
                .unwrap_or(DeviceSelector::DefaultCapture),
            output_format: format,
            packet_duration: config.packet_duration,
        },
        ..AudioStreamConfig::default()
    }
}

fn resolve_input_device(track: &AudioTrackConfig) -> DeviceSelector {
    match &track.device {
        AudioTrackDevice::MicrophoneDefault => DeviceSelector::DefaultCapture,
        AudioTrackDevice::InputDeviceId(id) => DeviceSelector::Id(id.clone()),
        AudioTrackDevice::SystemDefault => DeviceSelector::DefaultRender,
    }
}

fn run_audio_recording_worker(
    stream: crate::streaming::AudioStreamHandle,
    config: AudioRecordingConfig,
    clock: RecordingClock,
    stop_flag: Arc<AtomicBool>,
    pause_flag: Arc<AtomicBool>,
) -> AudioResult<AudioRecordingArtifact> {
    let started_at = clock.started_at();
    let format = AudioFormat::new(config.sample_rate_hz, config.channels);
    let mut writers = RecordingTrackWriters::new(&config, started_at, format)?;
    let mut stream = Some(stream);

    loop {
        if stop_flag.load(Ordering::Acquire) {
            if let Some(stream) = stream.take() {
                let drained = stream.stop_and_drain();
                process_audio_events(&mut writers, &clock, drained)?;
            }
            break;
        }

        if let Some(handle) = stream.as_ref() {
            if pause_flag.load(Ordering::Acquire) {
                handle.pause();
            } else {
                handle.resume();
            }
        }

        let Some(handle) = stream.as_ref() else {
            break;
        };

        match handle.recv_timeout(Duration::from_millis(25)) {
            Ok(event) => process_audio_event(&mut writers, &clock, event)?,
            Err(crate::error::RecvTimeoutError::Timeout) => {}
            Err(crate::error::RecvTimeoutError::Closed) => {
                if let Some(stream) = stream.take() {
                    let drained = stream.stop_and_drain();
                    process_audio_events(&mut writers, &clock, drained)?;
                }
                break;
            }
        }
    }

    writers.finish()
}

fn process_audio_events(
    writers: &mut RecordingTrackWriters,
    clock: &RecordingClock,
    events: Vec<AudioEvent>,
) -> AudioResult<()> {
    for event in events {
        process_audio_event(writers, clock, event)?;
    }
    Ok(())
}

fn process_audio_event(
    writers: &mut RecordingTrackWriters,
    clock: &RecordingClock,
    event: AudioEvent,
) -> AudioResult<()> {
    match event {
        AudioEvent::Packet(packet) if !packet.data.is_empty() => {
            writers.write_packet(&packet, clock)?;
        }
        AudioEvent::PacketDropped {
            source,
            dropped_frames,
        } => {
            writers.write_silence(source, dropped_frames)?;
        }
        AudioEvent::Error(err) => return Err(err),
        AudioEvent::StreamEnded => {}
        AudioEvent::Paused { .. } | AudioEvent::Resumed { .. } => {
            writers.mark_alignment_pending(None);
        }
        AudioEvent::SourceRestarted { source, .. } => {
            writers.mark_alignment_pending(Some(source));
        }
        AudioEvent::Packet(_) => {}
    }
    Ok(())
}

struct RecordingTrackWriters {
    system_writer: Option<RecordedTrackWriter>,
    microphone_writer: Option<RecordedTrackWriter>,
}

impl RecordingTrackWriters {
    fn new(
        config: &AudioRecordingConfig,
        started_at: std::time::Instant,
        format: AudioFormat,
    ) -> AudioResult<Self> {
        let system_writer = config
            .tracks
            .iter()
            .find(|track| track.enabled && matches!(track.role, SystemOutput))
            .map(|track| RecordedTrackWriter::new(track, &config.output_dir, started_at, format))
            .transpose()?;
        let microphone_writer = config
            .tracks
            .iter()
            .find(|track| track.enabled && matches!(track.role, MicrophoneInput))
            .map(|track| RecordedTrackWriter::new(track, &config.output_dir, started_at, format))
            .transpose()?;

        Ok(Self {
            system_writer,
            microphone_writer,
        })
    }

    fn write_packet(&mut self, packet: &AudioPacket, clock: &RecordingClock) -> AudioResult<()> {
        let writer = match packet.source {
            AudioSourceKind::System => self.system_writer.as_mut(),
            AudioSourceKind::Microphone => self.microphone_writer.as_mut(),
        };
        if let Some(writer) = writer {
            writer.write_packet(packet, clock)?;
        }
        Ok(())
    }

    fn write_silence(&mut self, source: AudioSourceKind, frames: u64) -> AudioResult<()> {
        let writer = match source {
            AudioSourceKind::System => self.system_writer.as_mut(),
            AudioSourceKind::Microphone => self.microphone_writer.as_mut(),
        };
        if let Some(writer) = writer {
            writer.append_silence_frames(frames)?;
        }
        Ok(())
    }

    fn mark_alignment_pending(&mut self, source: Option<AudioSourceKind>) {
        if source.is_none_or(|kind| matches!(kind, AudioSourceKind::System))
            && let Some(writer) = self.system_writer.as_mut()
        {
            writer.timeline_alignment_pending = true;
        }
        if source.is_none_or(|kind| matches!(kind, AudioSourceKind::Microphone))
            && let Some(writer) = self.microphone_writer.as_mut()
        {
            writer.timeline_alignment_pending = true;
        }
    }

    fn finish(self) -> AudioResult<AudioRecordingArtifact> {
        let mut tracks = Vec::new();
        if let Some(writer) = self.system_writer {
            tracks.push(writer.finish()?);
        }
        if let Some(writer) = self.microphone_writer {
            tracks.push(writer.finish()?);
        }
        Ok(AudioRecordingArtifact { tracks })
    }
}

/// Writes one recorded track as raw interleaved PCM.
///
/// The track file carries no container header: its layout is fully described
/// by the `AudioTrackManifest` (`sample_format`, `sample_rate_hz`, `channels`,
/// `duration_frames`) that is embedded in the recording bundle next to it.
/// Keeping the manifest as the single source of format metadata means the
/// bundle reader never has to parse or reconcile a second copy.
struct RecordedTrackWriter {
    manifest: AudioTrackManifest,
    path: PathBuf,
    writer: BufWriter<File>,
    written_frames: u64,
    format: AudioFormat,
    timeline: StableAudioTimeline,
    timeline_alignment_pending: bool,
    silence_chunk: Vec<u8>,
}

struct StableAudioTimeline {
    recording_started_at: std::time::Instant,
    stream_start_offset: Duration,
    stream_anchor: Option<TimestampAnchor>,
}

impl StableAudioTimeline {
    fn new(recording_started_at: std::time::Instant) -> Self {
        Self {
            recording_started_at,
            stream_start_offset: Duration::ZERO,
            stream_anchor: None,
        }
    }

    fn packet_timestamp(&mut self, packet: &AudioPacket) -> AudioPacketTimestamp {
        if self.stream_anchor.is_none() {
            let packet_start = packet
                .start_capture_time()
                .unwrap_or(self.recording_started_at);
            self.stream_start_offset =
                packet_start.saturating_duration_since(self.recording_started_at);
            self.stream_anchor = Some(audio_anchor_from_first_packet(packet));
        }

        let relative = self
            .stream_anchor
            .as_ref()
            .expect("audio stream anchor must be initialized")
            .audio_stream_relative(packet);
        AudioPacketTimestamp {
            start: self.stream_start_offset.saturating_add(relative.start),
            end: self.stream_start_offset.saturating_add(relative.end),
        }
    }
}

impl RecordedTrackWriter {
    fn new(
        config: &AudioTrackConfig,
        output_dir: &Path,
        started_at: std::time::Instant,
        format: AudioFormat,
    ) -> AudioResult<Self> {
        let asset_id = format!("audio/{}.pcm", config.track_id);
        let path = output_dir.join(format!("audio-{}.pcm", sanitize_track_id(&config.track_id)));
        let file = File::create(&path).map_err(AudioError::platform)?;
        let writer = BufWriter::with_capacity(128 * 1024, file);
        let silence_chunk = vec![0u8; 4096 * usize::from(format.channels) * 2];
        Ok(Self {
            manifest: AudioTrackManifest {
                track_id: config.track_id.clone(),
                role: config.role,
                asset_id,
                sample_rate_hz: format.sample_rate,
                channels: format.channels,
                sample_format: AudioSampleFormat::PcmS16Le,
                duration_frames: 0,
                recorded: false,
            },
            path,
            writer,
            written_frames: 0,
            format,
            timeline: StableAudioTimeline::new(started_at),
            timeline_alignment_pending: true,
            silence_chunk,
        })
    }

    fn write_packet(&mut self, packet: &AudioPacket, clock: &RecordingClock) -> AudioResult<()> {
        let packet_frames = i16_frame_count(packet.as_i16_slice(), self.format.channels)?;
        if packet_frames == 0 {
            return Ok(());
        }

        let packet_ts = self.timeline.packet_timestamp(packet);
        let aligned = if !self.timeline_alignment_pending && !packet.metadata.discontinuity {
            AudioPacketAlignment {
                write_packet_frames: packet_frames,
                ..Default::default()
            }
        } else {
            align_packet_frames(
                self.written_frames,
                self.format.sample_rate,
                AudioPacketTimestamp {
                    start: clock.active_elapsed_from_stream_offset(packet_ts.start),
                    end: clock.active_elapsed_from_stream_offset(packet_ts.end),
                },
                packet_frames,
            )
        };

        if aligned.silence_prefix_frames > 0 {
            self.append_silence_frames(aligned.silence_prefix_frames)?;
        }

        if aligned.write_packet_frames == 0 {
            self.timeline_alignment_pending = false;
            return Ok(());
        }

        let channels = usize::from(self.format.channels);
        let skip_samples = aligned
            .skip_packet_frames
            .checked_mul(channels as u64)
            .ok_or(AudioError::BufferOverflow)? as usize;
        let write_samples = aligned
            .write_packet_frames
            .checked_mul(channels as u64)
            .ok_or(AudioError::BufferOverflow)? as usize;
        let end = skip_samples
            .checked_add(write_samples)
            .ok_or(AudioError::BufferOverflow)?;
        let result = self.append_i16_samples(&packet.as_i16_slice()[skip_samples..end]);
        if result.is_ok() {
            self.timeline_alignment_pending = false;
        }
        result
    }

    fn append_silence_frames(&mut self, frames: u64) -> AudioResult<()> {
        if frames == 0 {
            return Ok(());
        }

        let channels = usize::from(self.format.channels);
        const CHUNK_FRAMES: u64 = 4096;
        let mut remaining = frames;
        while remaining > 0 {
            let take = remaining.min(CHUNK_FRAMES);
            let take_bytes = take as usize * channels * 2;
            self.writer
                .write_all(&self.silence_chunk[..take_bytes])
                .map_err(AudioError::platform)?;
            self.written_frames = self.written_frames.saturating_add(take);
            remaining -= take;
        }
        if frames > 0 {
            self.manifest.recorded = true;
            self.manifest.duration_frames = self.written_frames;
        }
        Ok(())
    }

    fn append_i16_samples(&mut self, samples: &[i16]) -> AudioResult<()> {
        if samples.is_empty() {
            return Ok(());
        }
        let frames = i16_frame_count(samples, self.format.channels)?;
        write_i16_le(&mut self.writer, samples)?;
        self.written_frames = self.written_frames.saturating_add(frames);
        self.manifest.recorded = true;
        self.manifest.duration_frames = self.written_frames;
        Ok(())
    }

    fn finish(mut self) -> AudioResult<RecordedAudioTrack> {
        self.writer.flush().map_err(AudioError::platform)?;
        Ok(RecordedAudioTrack {
            manifest: self.manifest,
            path: self.path,
        })
    }
}

fn sanitize_track_id(track_id: &str) -> String {
    let sanitized: String = track_id
        .chars()
        .map(|ch| if ch.is_ascii_alphanumeric() { ch } else { '-' })
        .collect();
    if sanitized.is_empty() {
        "track".to_string()
    } else {
        sanitized
    }
}

fn i16_frame_count(samples: &[i16], channels: u16) -> AudioResult<u64> {
    let channels = usize::from(channels.max(1));
    if !samples.len().is_multiple_of(channels) {
        return Err(AudioError::InvalidConfig(
            "audio packet samples are not channel aligned".into(),
        ));
    }
    Ok((samples.len() / channels) as u64)
}

fn write_i16_le(writer: &mut impl Write, samples: &[i16]) -> AudioResult<()> {
    #[cfg(target_endian = "little")]
    {
        let bytes = unsafe {
            std::slice::from_raw_parts(
                samples.as_ptr() as *const u8,
                std::mem::size_of_val(samples),
            )
        };
        writer.write_all(bytes).map_err(AudioError::platform)?;
    }

    #[cfg(not(target_endian = "little"))]
    {
        for sample in samples {
            writer
                .write_all(&sample.to_le_bytes())
                .map_err(AudioError::platform)?;
        }
    }

    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn timed_packet(end: std::time::Instant, qpc_100ns: i64, sequence: u64) -> AudioPacket {
        let format = AudioFormat::new(48_000, 2);
        let frames = 960;
        let mut metadata = crate::packet::AudioPacketMetadata {
            sequence,
            ..Default::default()
        };
        metadata.set_timing(Some(end), Some(qpc_100ns));
        AudioPacket {
            source: AudioSourceKind::Microphone,
            format,
            frames,
            data: vec![0; format.samples_for_frames(frames).unwrap()],
            metadata,
        }
    }

    #[test]
    fn default_audio_recording_config_is_track_based() {
        let config = AudioRecordingConfig::default();
        assert_eq!(config.tracks.len(), 2);
        assert!(
            config
                .tracks
                .iter()
                .any(|track| matches!(track.role, AudioTrackRole::SystemOutput))
        );
        assert!(
            config
                .tracks
                .iter()
                .any(|track| matches!(track.role, AudioTrackRole::MicrophoneInput))
        );
    }

    #[test]
    fn recording_audio_sources_are_optional() {
        let stream_config = build_stream_config(&AudioRecordingConfig::default());

        assert!(stream_config.system.enabled);
        assert!(!stream_config.system.required);
        assert!(stream_config.microphone.enabled);
        assert!(!stream_config.microphone.required);
    }

    #[test]
    fn stable_timeline_uses_wasapi_qpc_instead_of_packet_delivery_jitter() {
        let started_at = std::time::Instant::now();
        let first = timed_packet(started_at + Duration::from_millis(50), 1_000_000, 1);
        let delayed_second = timed_packet(started_at + Duration::from_millis(95), 1_200_000, 2);
        let mut timeline = StableAudioTimeline::new(started_at);

        let first_ts = timeline.packet_timestamp(&first);
        let second_ts = timeline.packet_timestamp(&delayed_second);

        assert_eq!(first_ts.start, Duration::from_millis(30));
        assert_eq!(first_ts.end, Duration::from_millis(50));
        assert_eq!(second_ts.start, Duration::from_millis(50));
        assert_eq!(second_ts.end, Duration::from_millis(70));
    }

    #[test]
    fn contiguous_device_packets_ignore_subframe_qpc_jitter() {
        let started_at = std::time::Instant::now();
        let output_dir = std::env::temp_dir().join(format!(
            "snow-audio-recorder-timing-{}",
            started_at.elapsed().as_nanos()
        ));
        std::fs::create_dir_all(&output_dir).expect("temporary output directory should be created");
        let track = AudioTrackConfig::microphone_default("timing");
        let format = AudioFormat::new(48_000, 2);
        let mut writer = RecordedTrackWriter::new(&track, &output_dir, started_at, format)
            .expect("temporary track should be created");
        let clock = RecordingClock::new(started_at);

        let make_packet = |end: std::time::Instant, qpc_100ns: i64, device_end: u64, value: i16| {
            let frames = 960;
            let mut metadata = crate::packet::AudioPacketMetadata {
                device_position_frames: Some(device_end),
                ..Default::default()
            };
            metadata.set_timing(Some(end), Some(qpc_100ns));
            AudioPacket {
                source: AudioSourceKind::Microphone,
                format,
                frames,
                data: vec![value; format.samples_for_frames(frames).unwrap()],
                metadata,
            }
        };

        writer
            .write_packet(
                &make_packet(started_at + Duration::from_millis(20), 1_000_000, 960, 1),
                &clock,
            )
            .expect("first packet should be written");
        writer
            .write_packet(
                &make_packet(started_at + Duration::from_millis(40), 1_200_300, 1_920, 2),
                &clock,
            )
            .expect("second packet should be written");

        let recorded = writer.finish().expect("track should finish");
        let bytes = std::fs::read(&recorded.path).expect("recorded samples should be readable");
        let samples: Vec<i16> = bytes
            .chunks_exact(2)
            .map(|chunk| i16::from_le_bytes([chunk[0], chunk[1]]))
            .collect();
        assert_eq!(samples.len(), 1_920 * 2);
        assert_eq!(&samples[0..2], &[1, 1]);
        assert_eq!(&samples[1_920..1_922], &[2, 2]);

        std::fs::remove_dir_all(output_dir).expect("temporary output directory should be removed");
    }
}
