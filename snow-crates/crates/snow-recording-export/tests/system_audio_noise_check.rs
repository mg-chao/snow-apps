//! End-to-end noise check for the screen-recording export audio pipeline.
//!
//! `snow-audio-recorder/tests/sample-15s.wav` stands in for a captured
//! system-audio track: its samples are written into a synthesized recording
//! bundle (Matroska video payload + `SVIDX` frame index + mouse store + raw
//! `PcmS16Le` audio asset, the exact layout `RecordingSession` produces), the
//! bundle is exported through [`EditingSession`] — the same
//! packet-copy + mix + AAC path used by the app — and the audio decoded back
//! out of the exported MP4 is compared against the source samples.
//!
//! The test asserts that the pipeline itself does not inject noise:
//! * whole-signal SNR between the exported audio and the source stays above
//!   20 dB (the AAC 192 kbps round-trip measures ~30 dB here, including the
//!   decode loss of the verification decoder),
//! * the silent tail that the mix pads after the track ends stays below
//!   -60 dBFS (the pipeline currently produces digital silence there), and
//! * the decoded head prefix before the aligned body (the AAC priming region)
//!   peaks below -20 dBFS — stale bytes such as a container header that leak
//!   into the front of the track would appear as a loud click there.
//!
//! The exported MP4 is decoded back to PCM with the pure-Rust `symphonia`
//! dev-dependency because the workspace ffmpeg build registers no audio
//! decoders ("snow-shot-minimal" feature set).
//!
//! Scope note: this exercises the export side only (mix/limiter →
//! libswresample → AAC → MP4 mux → decode). Capture-side WASAPI processing
//! cannot be driven without real audio devices, so a clean result localizes
//! any reported recording noise to the capture path instead of the exporter.

use std::fs::{self, File};
use std::io::Write as _;
use std::path::{Path, PathBuf};
use std::sync::OnceLock;

use ffmpeg_next as ffmpeg;
use snow_recording_export::{EditingSession, ExportExecutionMode};
use snow_recording_model::{
    AudioSampleFormat, AudioTrackManifest, AudioTrackRole, BundleAssetKind,
    IntermediateRecordingProfile, LocalRecordingPaths, MouseStore, RecordingArtifact,
    RecordingBundleAsset, SessionManifest, VideoEncodeConfig, write_mouse_records,
    write_recording_bundle,
};
use tempfile::tempdir;

const FIXTURE_WAV: &str = concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../snow-audio-recorder/tests/sample-15s.wav"
);
const OUTPUT_DIR: &str = concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/tests/output/system-audio-noise"
);

const TRACK_ID: &str = "system";
const VIDEO_WIDTH: u32 = 320;
const VIDEO_HEIGHT: u32 = 240;
const VIDEO_FPS: u32 = 1;
const VIDEO_FRAMES: u32 = 20;

const MIN_SNR_DB: f64 = 20.0;
const MAX_TAIL_RMS_DBFS: f64 = -60.0;
const MAX_HEAD_PEAK_DBFS: f64 = -20.0;

#[test]
fn exported_system_audio_does_not_add_noise_to_the_source() {
    ensure_ffmpeg_initialized().expect("ffmpeg should initialize");
    let source = read_wav_pcm16(Path::new(FIXTURE_WAV))
        .unwrap_or_else(|err| panic!("failed to read WAV fixture {FIXTURE_WAV}: {err}"));
    let source_frames = source.samples.len() / usize::from(source.channels);
    println!(
        "source fixture: {} ({} Hz, {} ch, {:.3} s, {} frames)",
        FIXTURE_WAV,
        source.sample_rate_hz,
        source.channels,
        source_frames as f64 / f64::from(source.sample_rate_hz),
        source_frames
    );

    let exported_path = export_wav_as_system_track(&source);
    let decoded = decode_exported_audio(&exported_path).expect("exported MP4 should decode");
    println!(
        "exported audio: {} Hz, {} ch, {:.3} s, encoder {:?}",
        decoded.sample_rate_hz,
        decoded.channels,
        decoded.samples.len() / usize::from(decoded.channels) / decoded.sample_rate_hz as usize,
        decoded.encoder_description
    );

    let decoded_wav_path = Path::new(OUTPUT_DIR).join("exported-audio.wav");
    write_wav_pcm16(
        &decoded_wav_path,
        decoded.sample_rate_hz,
        decoded.channels,
        &decoded.samples,
    )
    .expect("exported audio clip should be written for manual inspection");
    println!(
        "resulting audio clip written to {}",
        decoded_wav_path.display()
    );

    assert_eq!(
        decoded.sample_rate_hz, source.sample_rate_hz,
        "exported audio rate must match the source track rate"
    );
    assert_eq!(
        decoded.channels, source.channels,
        "exported audio channel count must match the source track"
    );
    let decoded_frames = decoded.samples.len() / usize::from(decoded.channels);
    let decoded_duration_s = decoded_frames as f64 / f64::from(decoded.sample_rate_hz);
    assert!(
        (19.0..=21.0).contains(&decoded_duration_s),
        "decoded audio duration {decoded_duration_s:.3} s deviates from the {VIDEO_FRAMES} s video duration"
    );

    let comparison = compare_with_alignment(
        &source.samples,
        &decoded.samples,
        decoded.channels as usize,
        decoded.sample_rate_hz,
    );
    println!("comparison report:");
    println!(
        "  alignment offset: {} samples ({:+.1} ms) after coarse+fine search",
        comparison.lag_samples,
        comparison.lag_samples as f64 / f64::from(source.sample_rate_hz) * 1000.0
    );
    println!("  compared frames: {}", comparison.overlap_frames);
    println!("  signal RMS: {:.2} dBFS", comparison.signal_rms_dbfs);
    println!("  error RMS: {:.2} dBFS", comparison.error_rms_dbfs);
    println!(
        "  SNR: {:.2} dB (threshold >= {MIN_SNR_DB} dB)",
        comparison.snr_db
    );
    println!(
        "  silent-tail RMS: {} (threshold <= {MAX_TAIL_RMS_DBFS} dBFS)",
        format_dbfs(comparison.tail_rms_dbfs)
    );
    println!(
        "  head prefix ({} frames before the aligned body): peak {} (threshold <= {MAX_HEAD_PEAK_DBFS} dBFS)",
        comparison.head_frames,
        comparison
            .head_peak_dbfs
            .map(format_dbfs)
            .unwrap_or_else(|| "n/a".to_string())
    );
    println!("artifacts: {exported_path:?} and {decoded_wav_path:?}");

    assert!(
        comparison.snr_db >= MIN_SNR_DB,
        "export pipeline degraded the signal: SNR {:.2} dB is below the {MIN_SNR_DB} dB floor",
        comparison.snr_db
    );
    assert!(
        comparison.tail_rms_dbfs <= MAX_TAIL_RMS_DBFS,
        "export pipeline injected noise into the silent tail: RMS {} exceeds {MAX_TAIL_RMS_DBFS} dBFS",
        format_dbfs(comparison.tail_rms_dbfs)
    );
    if let Some(head_peak_dbfs) = comparison.head_peak_dbfs {
        assert!(
            head_peak_dbfs <= MAX_HEAD_PEAK_DBFS,
            "export pipeline injected noise at the beginning of the clip: the {}-frame head prefix peaks at {} (threshold <= {MAX_HEAD_PEAK_DBFS} dBFS)",
            comparison.head_frames,
            format_dbfs(head_peak_dbfs)
        );
    }
}

/// Packages the WAV into a synthetic recording bundle and runs the production
/// export (packet-copy video + generated AAC audio) on it.
fn export_wav_as_system_track(source: &WavAudio) -> PathBuf {
    let temp = tempdir().expect("temporary directory should be created");

    let video_path = temp.path().join("intermediate.mkv");
    encode_reference_video(&video_path).expect("reference matroska video should encode");

    let index_path = temp.path().join("video_index.bin");
    write_video_index(&index_path).expect("video index should be written");

    let mouse_path = temp.path().join("mouse.bin");
    write_mouse_records(&mouse_path, &MouseStore::new()).expect("mouse store should be written");

    let pcm_path = temp.path().join(format!("audio-{TRACK_ID}.pcm"));
    write_pcm_i16_le(&pcm_path, &source.samples).expect("raw PCM track should be written");

    let track_frames = source.samples.len() / usize::from(source.channels);
    let manifest = SessionManifest {
        session_id: "audio-noise-check".to_string(),
        output_dir: temp.path().to_path_buf(),
        keep_temp_files: false,
        fps: VIDEO_FPS,
        intermediate_profile: IntermediateRecordingProfile::EditFast,
        recording_video: VideoEncodeConfig::default(),
        width: VIDEO_WIDTH,
        height: VIDEO_HEIGHT,
        capture_origin_x: 0,
        capture_origin_y: 0,
        audio_tracks: vec![AudioTrackManifest {
            track_id: TRACK_ID.to_string(),
            role: AudioTrackRole::SystemOutput,
            asset_id: format!("audio/{TRACK_ID}.pcm"),
            sample_rate_hz: source.sample_rate_hz,
            channels: source.channels,
            sample_format: AudioSampleFormat::PcmS16Le,
            duration_frames: track_frames as u64,
            recorded: true,
        }],
        pause_intervals: Vec::new(),
    };

    let bundle_path = temp.path().join("audio-noise-check.snowrec");
    fs::copy(&video_path, &bundle_path).expect("video payload should become the bundle head");
    write_recording_bundle(
        &bundle_path,
        &manifest,
        &[
            RecordingBundleAsset {
                kind: BundleAssetKind::VideoIndex,
                asset_id: None,
                path: &index_path,
            },
            RecordingBundleAsset {
                kind: BundleAssetKind::MouseStore,
                asset_id: None,
                path: &mouse_path,
            },
            RecordingBundleAsset {
                kind: BundleAssetKind::AudioTrack,
                asset_id: Some(&manifest.audio_tracks[0].asset_id),
                path: &pcm_path,
            },
        ],
    )
    .expect("recording bundle should be assembled");

    let output_dir = PathBuf::from(OUTPUT_DIR);
    fs::create_dir_all(&output_dir).expect("output directory should be created");
    let exported_path = output_dir.join("exported.mp4");

    let editing = EditingSession::open(RecordingArtifact {
        session_id: manifest.session_id.clone(),
        output_dir: temp.path().to_path_buf(),
        local_paths: LocalRecordingPaths {
            temp_dir: temp.path().to_path_buf(),
            video_intermediate_path: bundle_path.clone(),
            video_index_path: index_path.clone(),
            mouse_path: mouse_path.clone(),
        },
        bundle_path,
        audio_tracks: manifest.audio_tracks.clone(),
    })
    .expect("synthetic bundle should open as an editing session");

    let mut request = editing.export_request();
    let track = request
        .audio_tracks
        .iter_mut()
        .find(|track| track.track_id == TRACK_ID)
        .expect("export request should list the system track");
    track.enabled = true;
    track.volume = 1.0;
    request.output_path = exported_path.clone();
    request.performance.mode = ExportExecutionMode::SoftwareOnly;

    let result = editing
        .export(request)
        .expect("export with generated audio should succeed");
    println!(
        "export: path={:?} video_encoder={:?} audio_encoder={:?} duration_ms={}",
        result.runtime_report.path,
        result.runtime_report.video_encoder,
        result.runtime_report.audio_encoder,
        result.duration_ms
    );
    exported_path
}

fn ensure_ffmpeg_initialized() -> Result<(), String> {
    static INIT: OnceLock<Result<(), String>> = OnceLock::new();
    INIT.get_or_init(|| ffmpeg::init().map_err(|err| err.to_string()))
        .clone()
}

fn is_eagain(err: &ffmpeg::Error) -> bool {
    matches!(
        err,
        ffmpeg::Error::Other { errno } if *errno == ffmpeg::error::EAGAIN
    )
}

/// Encodes the stand-in recording video exactly like the live recorder does:
/// Matroska container with libx264, all-intra (`g=1`, `bf=0`, zerolatency).
fn encode_reference_video(path: &Path) -> Result<(), String> {
    let mut output = ffmpeg::format::output_as(path, "matroska")
        .map_err(|err| format!("failed to create matroska output: {err}"))?;
    let codec = ffmpeg::encoder::find_by_name("libx264")
        .ok_or_else(|| "libx264 encoder is unavailable in this ffmpeg build".to_string())?;

    let mut encoder = ffmpeg::codec::context::Context::new_with_codec(codec)
        .encoder()
        .video()
        .map_err(|err| format!("failed to create video encoder context: {err}"))?;
    encoder.set_width(VIDEO_WIDTH);
    encoder.set_height(VIDEO_HEIGHT);
    encoder.set_format(ffmpeg::format::Pixel::YUV420P);
    encoder.set_time_base(ffmpeg::Rational(1, VIDEO_FPS as i32));
    encoder.set_frame_rate(Some(ffmpeg::Rational(VIDEO_FPS as i32, 1)));
    if output
        .format()
        .flags()
        .contains(ffmpeg::format::Flags::GLOBAL_HEADER)
    {
        encoder.set_flags(ffmpeg::codec::Flags::GLOBAL_HEADER);
    }

    let mut options = ffmpeg::Dictionary::new();
    options.set("preset", "ultrafast");
    options.set("tune", "zerolatency");
    options.set("g", "1");
    options.set("bf", "0");
    options.set("crf", "28");
    let mut encoder = encoder
        .open_as_with(codec, options)
        .map_err(|err| format!("failed to open libx264: {err}"))?;

    let stream_index = {
        let mut stream = output
            .add_stream(codec)
            .map_err(|err| format!("failed to add video stream: {err}"))?;
        stream.set_time_base(ffmpeg::Rational(1, VIDEO_FPS as i32));
        stream.set_rate(ffmpeg::Rational(VIDEO_FPS as i32, 1));
        stream.set_avg_frame_rate(ffmpeg::Rational(VIDEO_FPS as i32, 1));
        stream.set_parameters(&encoder);
        stream.index()
    };
    output
        .write_header()
        .map_err(|err| format!("failed to write matroska header: {err}"))?;
    let stream_time_base = output
        .stream(stream_index)
        .map(|stream| stream.time_base())
        .ok_or_else(|| "video stream disappeared after header".to_string())?;

    let mut frame =
        ffmpeg::frame::Video::new(ffmpeg::format::Pixel::YUV420P, VIDEO_WIDTH, VIDEO_HEIGHT);
    for pts in 0..i64::from(VIDEO_FRAMES) {
        fill_video_frame(&mut frame);
        frame.set_pts(Some(pts));
        encoder
            .send_frame(&frame)
            .map_err(|err| format!("failed to send video frame {pts}: {err}"))?;
        drain_video_packets(
            &mut encoder,
            &mut output,
            stream_index,
            stream_time_base,
            false,
        )?;
    }
    encoder
        .send_eof()
        .map_err(|err| format!("failed to flush video encoder: {err}"))?;
    drain_video_packets(
        &mut encoder,
        &mut output,
        stream_index,
        stream_time_base,
        true,
    )?;
    output
        .write_trailer()
        .map_err(|err| format!("failed to write matroska trailer: {err}"))?;
    Ok(())
}

fn drain_video_packets(
    encoder: &mut ffmpeg::encoder::video::Video,
    output: &mut ffmpeg::format::context::Output,
    stream_index: usize,
    stream_time_base: ffmpeg::Rational,
    draining: bool,
) -> Result<(), String> {
    loop {
        let mut packet = ffmpeg::Packet::empty();
        match encoder.receive_packet(&mut packet) {
            Ok(()) => {
                packet.set_stream(stream_index);
                packet.rescale_ts(encoder.time_base(), stream_time_base);
                packet
                    .write_interleaved(output)
                    .map_err(|err| format!("failed to write reference video packet: {err}"))?;
            }
            Err(ffmpeg::Error::Eof) => break,
            Err(err) if is_eagain(&err) && !draining => break,
            Err(err) if is_eagain(&err) && draining => continue,
            Err(err) => return Err(format!("failed to receive reference video packet: {err}")),
        }
    }
    Ok(())
}

fn fill_video_frame(frame: &mut ffmpeg::frame::Video) {
    fill_plane(frame, 0, 120, VIDEO_WIDTH as usize, VIDEO_HEIGHT as usize);
    let (chroma_width, chroma_height) = ((VIDEO_WIDTH / 2) as usize, (VIDEO_HEIGHT / 2) as usize);
    fill_plane(frame, 1, 128, chroma_width, chroma_height);
    fill_plane(frame, 2, 128, chroma_width, chroma_height);
}

fn fill_plane(
    frame: &mut ffmpeg::frame::Video,
    plane: usize,
    value: u8,
    width: usize,
    height: usize,
) {
    let stride = frame.stride(plane);
    let data = frame.data_mut(plane);
    for row in 0..height {
        let start = row * stride;
        if let Some(line) = data.get_mut(start..start + width) {
            line.fill(value);
        }
    }
}

/// Frame index in the exact `VideoIndexWriter` wire format:
/// `SVIDX\0\0` + records of `{u64 index, u64 timestamp_ms, u32 duration_ms}`.
fn write_video_index(path: &Path) -> Result<(), String> {
    let mut file = File::create(path).map_err(|err| err.to_string())?;
    file.write_all(b"SVIDX\0\0")
        .map_err(|err| err.to_string())?;
    for frame in 0..u64::from(VIDEO_FRAMES) {
        let mut record = [0u8; 20];
        record[..8].copy_from_slice(&frame.to_le_bytes());
        record[8..16].copy_from_slice(&(frame * 1000).to_le_bytes());
        record[16..20].copy_from_slice(&1000u32.to_le_bytes());
        file.write_all(&record).map_err(|err| err.to_string())?;
    }
    file.flush().map_err(|err| err.to_string())
}

fn write_pcm_i16_le(path: &Path, samples: &[i16]) -> Result<(), String> {
    let mut file = File::create(path).map_err(|err| err.to_string())?;
    let mut bytes = Vec::with_capacity(samples.len() * 2);
    for sample in samples {
        bytes.extend_from_slice(&sample.to_le_bytes());
    }
    file.write_all(&bytes).map_err(|err| err.to_string())?;
    file.flush().map_err(|err| err.to_string())
}

struct WavAudio {
    sample_rate_hz: u32,
    channels: u16,
    samples: Vec<i16>,
}

/// Minimal RIFF reader for plain 16-bit PCM WAV files (the fixture's format).
fn read_wav_pcm16(path: &Path) -> Result<WavAudio, String> {
    let bytes = fs::read(path).map_err(|err| format!("{}: {err}", path.display()))?;
    let read_u16 = |at: usize| u16::from_le_bytes(bytes[at..at + 2].try_into().unwrap());
    let read_u32 = |at: usize| u32::from_le_bytes(bytes[at..at + 4].try_into().unwrap());

    if bytes.len() < 12 || &bytes[0..4] != b"RIFF" || &bytes[8..12] != b"WAVE" {
        return Err("not a RIFF/WAVE file".to_string());
    }

    let mut fmt = None;
    let mut data = None;
    let mut pos = 12usize;
    while pos + 8 <= bytes.len() {
        let id = &bytes[pos..pos + 4];
        let size = read_u32(pos + 4) as usize;
        let body = pos + 8;
        if body + size > bytes.len() {
            return Err("truncated RIFF chunk".to_string());
        }
        let chunk = &bytes[body..body + size];
        match id {
            b"fmt " if chunk.len() >= 16 => {
                fmt = Some((
                    read_u16(body),
                    read_u16(body + 2),
                    read_u32(body + 4),
                    read_u16(body + 14),
                ));
            }
            b"data" => data = Some(chunk),
            _ => {}
        }
        pos = body + size + (size & 1);
    }

    let (format_tag, channels, sample_rate_hz, bits_per_sample) =
        fmt.ok_or_else(|| "WAV fixture has no fmt chunk".to_string())?;
    if format_tag != 1 {
        return Err(format!(
            "unsupported WAV format tag {format_tag} (only plain PCM)"
        ));
    }
    if bits_per_sample != 16 {
        return Err(format!(
            "unsupported bit depth {bits_per_sample} (only 16-bit)"
        ));
    }
    let data = data.ok_or_else(|| "WAV fixture has no data chunk".to_string())?;
    let samples = data
        .chunks_exact(2)
        .map(|chunk| i16::from_le_bytes([chunk[0], chunk[1]]))
        .collect();
    Ok(WavAudio {
        sample_rate_hz,
        channels,
        samples,
    })
}

struct DecodedAudio {
    sample_rate_hz: u32,
    channels: u16,
    samples: Vec<f32>,
    encoder_description: String,
}

/// Decodes the audio track of the exported MP4 back into interleaved f32
/// samples.
///
/// The workspace ffmpeg build is deliberately minimal ("snow-shot-minimal")
/// and registers no audio decoders (and only the matroska demuxer), so the
/// round-trip decode uses the pure-Rust `symphonia` dev-dependency instead of
/// libav.
fn decode_exported_audio(path: &Path) -> Result<DecodedAudio, String> {
    use symphonia::core::audio::SampleBuffer;
    use symphonia::core::codecs::CODEC_TYPE_AAC;
    use symphonia::core::errors::Error as SymphoniaError;
    use symphonia::core::formats::FormatOptions;
    use symphonia::core::io::MediaSourceStream;
    use symphonia::core::meta::MetadataOptions;
    use symphonia::core::probe::Hint;

    let file = File::open(path).map_err(|err| format!("{}: {err}", path.display()))?;
    let mss = MediaSourceStream::new(Box::new(file), Default::default());
    let mut hint = Hint::new();
    hint.with_extension("mp4");
    let mut format = symphonia::default::get_probe()
        .format(
            &hint,
            mss,
            &FormatOptions::default(),
            &MetadataOptions::default(),
        )
        .map_err(|err| format!("symphonia could not probe the exported file: {err}"))?
        .format;

    let track = format
        .tracks()
        .iter()
        .find(|track| track.codec_params.codec == CODEC_TYPE_AAC)
        .ok_or_else(|| "exported file has no AAC audio track".to_string())?;
    let track_id = track.id;
    let codec_type = track.codec_params.codec;
    // The MP4 track header does not always carry the channel configuration;
    // fall back to the spec reported by the first decoded audio buffer.
    let mut sample_rate_hz = track.codec_params.sample_rate;
    let mut channels = track
        .codec_params
        .channels
        .map(|channels| channels.count() as u16);
    let mut decoder = symphonia::default::get_codecs()
        .make(
            &track.codec_params,
            &symphonia::core::codecs::DecoderOptions::default(),
        )
        .map_err(|err| format!("failed to create AAC decoder: {err}"))?;

    let mut samples = Vec::new();
    loop {
        let packet = match format.next_packet() {
            Ok(packet) => packet,
            Err(SymphoniaError::IoError(ref err))
                if err.kind() == std::io::ErrorKind::UnexpectedEof =>
            {
                break;
            }
            Err(err) => return Err(format!("exported MP4 demux failed: {err}")),
        };
        if packet.track_id() != track_id {
            continue;
        }
        match decoder.decode(&packet) {
            Ok(decoded) => {
                if sample_rate_hz.is_none() || channels.is_none() {
                    let spec = decoded.spec();
                    sample_rate_hz = sample_rate_hz.or(Some(spec.rate));
                    channels = channels.or(Some(spec.channels.count() as u16));
                }
                let mut buffer =
                    SampleBuffer::<f32>::new(decoded.capacity() as u64, *decoded.spec());
                buffer.copy_interleaved_ref(decoded);
                samples.extend_from_slice(buffer.samples());
            }
            // A single corrupt AAC frame must not fail the whole check.
            Err(err) => eprintln!("skipping undecodable AAC packet: {err}"),
        }
    }

    Ok(DecodedAudio {
        sample_rate_hz: sample_rate_hz
            .ok_or_else(|| "decoded audio never reported a sample rate".to_string())?,
        channels: channels.ok_or_else(|| "decoded audio never reported channels".to_string())?,
        samples,
        encoder_description: describe_codec(codec_type),
    })
}

fn describe_codec(codec_type: symphonia::core::codecs::CodecType) -> String {
    if codec_type == symphonia::core::codecs::CODEC_TYPE_AAC {
        "aac (decoded by symphonia)".to_string()
    } else {
        format!("{codec_type:?} (decoded by symphonia)")
    }
}

fn write_wav_pcm16(
    path: &Path,
    sample_rate_hz: u32,
    channels: u16,
    samples: &[f32],
) -> Result<(), String> {
    let mut data = Vec::with_capacity(samples.len() * 2);
    for sample in samples {
        let quantized = (sample.clamp(-1.0, 1.0) * 32767.0).round() as i16;
        data.extend_from_slice(&quantized.to_le_bytes());
    }

    let mut file = File::create(path).map_err(|err| err.to_string())?;
    let data_len = data.len() as u32;
    let block_align = channels * 2;
    file.write_all(b"RIFF").map_err(|err| err.to_string())?;
    file.write_all(&(36 + data_len).to_le_bytes())
        .map_err(|err| err.to_string())?;
    file.write_all(b"WAVEfmt ").map_err(|err| err.to_string())?;
    file.write_all(&16u32.to_le_bytes())
        .map_err(|err| err.to_string())?;
    file.write_all(&1u16.to_le_bytes())
        .map_err(|err| err.to_string())?;
    file.write_all(&channels.to_le_bytes())
        .map_err(|err| err.to_string())?;
    file.write_all(&sample_rate_hz.to_le_bytes())
        .map_err(|err| err.to_string())?;
    file.write_all(&(sample_rate_hz * u32::from(block_align)).to_le_bytes())
        .map_err(|err| err.to_string())?;
    file.write_all(&block_align.to_le_bytes())
        .map_err(|err| err.to_string())?;
    file.write_all(&16u16.to_le_bytes())
        .map_err(|err| err.to_string())?;
    file.write_all(b"data").map_err(|err| err.to_string())?;
    file.write_all(&data_len.to_le_bytes())
        .map_err(|err| err.to_string())?;
    file.write_all(&data).map_err(|err| err.to_string())?;
    file.flush().map_err(|err| err.to_string())
}

struct Comparison {
    lag_samples: i64,
    overlap_frames: usize,
    snr_db: f64,
    signal_rms_dbfs: f64,
    error_rms_dbfs: f64,
    tail_rms_dbfs: f64,
    head_frames: usize,
    head_peak_dbfs: Option<f64>,
}

/// Aligns the decoded audio against the source samples (the AAC priming delay
/// and mux rounding shift the stream slightly) and measures how much of the
/// source survives the round-trip.
fn compare_with_alignment(
    reference: &[i16],
    output: &[f32],
    channels: usize,
    sample_rate_hz: u32,
) -> Comparison {
    let lag_samples = align_lag_samples(reference, output, channels, sample_rate_hz);

    let ref_start = usize::try_from(-lag_samples).unwrap_or(0) * channels;
    let out_start = usize::try_from(lag_samples).unwrap_or(0) * channels;
    let overlap_samples = (reference.len() - ref_start).min(output.len() - out_start);

    let mut signal_energy = 0.0f64;
    let mut error_energy = 0.0f64;
    for i in 0..overlap_samples {
        let r = f64::from(reference[ref_start + i]);
        let o = f64::from(output[out_start + i]) * 32768.0;
        signal_energy += r * r;
        error_energy += (o - r) * (o - r);
    }
    let overlap_frames = overlap_samples / channels;
    let signal_rms = (signal_energy / overlap_samples as f64).sqrt();
    let error_rms = (error_energy / overlap_samples as f64).sqrt();
    let snr_db = 10.0 * (signal_energy / error_energy.max(1e-12)).log10();

    // The mix pads silence after the track ends (the video runs longer than
    // the audio), so the last half second of the decoded stream must stay
    // silent if the pipeline does not inject noise.
    let tail_window = ((sample_rate_hz as usize) / 2 * channels)
        .min(output.len() / 4)
        .max(channels);
    let mut tail_energy = 0.0f64;
    for sample in &output[output.len() - tail_window..] {
        tail_energy += f64::from(*sample) * f64::from(*sample);
    }
    let tail_rms = (tail_energy / tail_window as f64).sqrt();

    // The decoded stream starts with the AAC encoder's priming prefix; the
    // alignment above skips it when comparing. Anything in that prefix must
    // still be quiet — a container header (or other stale bytes) leaked into
    // the front of the track shows up here as a loud click.
    let head_frames = usize::try_from(lag_samples).unwrap_or(0);
    let head_peak_dbfs = (head_frames > 0).then(|| {
        let head_samples = (head_frames * channels).min(output.len());
        output[..head_samples]
            .iter()
            .map(|sample| f64::from(sample.abs()))
            .fold(0.0f64, f64::max)
    });

    Comparison {
        lag_samples,
        overlap_frames,
        snr_db,
        signal_rms_dbfs: amplitude_to_dbfs(signal_rms / 32768.0),
        error_rms_dbfs: amplitude_to_dbfs(error_rms / 32768.0),
        tail_rms_dbfs: amplitude_to_dbfs(tail_rms),
        head_frames,
        head_peak_dbfs: head_peak_dbfs.map(amplitude_to_dbfs),
    }
}

const ALIGN_BLOCK_FRAMES: usize = 256;

/// Finds the sample offset that best aligns `output` to `reference`:
/// coarse search on RMS block envelopes (±250 ms), then two refinement passes
/// at sample resolution around the best estimate.
fn align_lag_samples(
    reference: &[i16],
    output: &[f32],
    channels: usize,
    sample_rate_hz: u32,
) -> i64 {
    let ref_mono: Vec<f64> = reference
        .chunks_exact(channels)
        .map(|frame| f64::from(frame[0]))
        .collect();
    let out_mono: Vec<f64> = output
        .chunks_exact(channels)
        .map(|frame| f64::from(frame[0]))
        .collect();
    if ref_mono.is_empty() || out_mono.is_empty() {
        return 0;
    }

    let ref_env = block_envelope(&ref_mono, ALIGN_BLOCK_FRAMES);
    let out_env = block_envelope(&out_mono, ALIGN_BLOCK_FRAMES);
    let max_lag_blocks =
        (0.25 * f64::from(sample_rate_hz) / ALIGN_BLOCK_FRAMES as f64).ceil() as i64;

    let mut best_block_lag = 0i64;
    let mut best_score = -1.0f64;
    for lag in -max_lag_blocks..=max_lag_blocks {
        let ref_block_start = usize::try_from(-lag).unwrap_or(0);
        let out_block_start = usize::try_from(lag).unwrap_or(0);
        let overlap = ref_env
            .len()
            .saturating_sub(ref_block_start)
            .min(out_env.len().saturating_sub(out_block_start));
        if overlap == 0 {
            continue;
        }
        let ref_slice = &ref_env[ref_block_start..];
        let out_slice = &out_env[out_block_start..];
        let mut dot = 0.0f64;
        let mut ref_energy = 0.0f64;
        let mut out_energy = 0.0f64;
        for i in 0..overlap {
            dot += ref_slice[i] * out_slice[i];
            ref_energy += ref_slice[i] * ref_slice[i];
            out_energy += out_slice[i] * out_slice[i];
        }
        let score = dot / (ref_energy.sqrt() * out_energy.sqrt() + 1e-12);
        if score > best_score {
            best_score = score;
            best_block_lag = lag * ALIGN_BLOCK_FRAMES as i64;
        }
    }

    let excerpt_len = 44_100usize.min(ref_mono.len() / 2).min(out_mono.len() / 2);
    let coarse = best_lag_in_range(
        &ref_mono,
        &out_mono,
        best_block_lag,
        i64::from(ALIGN_BLOCK_FRAMES as u32),
        16,
        excerpt_len,
    );
    best_lag_in_range(&ref_mono, &out_mono, coarse, 16, 1, excerpt_len)
}

fn best_lag_in_range(
    ref_mono: &[f64],
    out_mono: &[f64],
    center: i64,
    radius: i64,
    step: i64,
    excerpt_len: usize,
) -> i64 {
    let mut best_lag = center;
    let mut best_metric = f64::INFINITY;
    let mut lag = center - radius;
    while lag <= center + radius {
        let metric = excerpt_difference(ref_mono, out_mono, lag, ref_mono.len() / 2, excerpt_len);
        if metric < best_metric {
            best_metric = metric;
            best_lag = lag;
        }
        lag += step;
    }
    best_lag
}

fn block_envelope(mono: &[f64], block_frames: usize) -> Vec<f64> {
    mono.chunks(block_frames)
        .map(|block| (block.iter().map(|s| s * s).sum::<f64>() / block.len() as f64).sqrt())
        .collect()
}

/// Mean absolute difference between the reference excerpt and the output
/// excerpt shifted by `lag`; lower is a better alignment.
fn excerpt_difference(
    ref_mono: &[f64],
    out_mono: &[f64],
    lag: i64,
    ref_center: usize,
    excerpt_len: usize,
) -> f64 {
    let ref_start = ref_center.saturating_sub(excerpt_len / 2);
    let ref_end = (ref_start + excerpt_len).min(ref_mono.len());
    if ref_end <= ref_start {
        return f64::INFINITY;
    }
    let out_start_signed = ref_start as i64 + lag;
    if out_start_signed < 0 {
        return f64::INFINITY;
    }
    let out_start = out_start_signed as usize;
    let out_end = out_start + (ref_end - ref_start);
    if out_end > out_mono.len() {
        return f64::INFINITY;
    }
    let mut sum = 0.0f64;
    for i in 0..(ref_end - ref_start) {
        sum += (ref_mono[ref_start + i] - out_mono[out_start + i]).abs();
    }
    sum / (ref_end - ref_start) as f64
}

fn amplitude_to_dbfs(amplitude: f64) -> f64 {
    20.0 * amplitude.max(1e-12).log10()
}

fn format_dbfs(dbfs: f64) -> String {
    if dbfs <= -240.0 {
        "-inf dBFS".to_string()
    } else {
        format!("{dbfs:.2} dBFS")
    }
}
