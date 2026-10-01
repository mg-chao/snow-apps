use super::*;
use snow_recording_model::{
    AudioSampleFormat, AudioTrackManifest, AudioTrackRole, CursorFrameRecord,
    CursorShapeCompositionMode, CursorShapeRecord, EffectsConfig, FinalizedTimeline,
    InputStoreWriter, MouseStore, PlaybackOverlay, RecordedInput, RecordedInputEvent,
    RecordingBundleAsset, RenderConfig, SessionManifest, VideoEncodeConfig, VideoEncodingSpeed,
    write_mouse_records, write_render_metadata,
};

struct Fixture {
    _directory: tempfile::TempDir,
    bundle: PathBuf,
    config: StreamingEncoderConfig,
    metadata: RenderMetadata,
}
fn fixture(
    format: crate::ExportFormat,
    codec: crate::VideoCodec,
    odd: bool,
    audio: bool,
    hdr: bool,
) -> Fixture {
    fixture_with_source(
        format,
        codec,
        if odd { (33, 25) } else { (32, 24) },
        audio,
        hdr,
        (0, 15),
        false,
    )
}

fn fixture_with_source(
    format: crate::ExportFormat,
    codec: crate::VideoCodec,
    logical: (u32, u32),
    audio: bool,
    hdr: bool,
    slots: (u64, u64),
    portrait_change: bool,
) -> Fixture {
    let directory = tempfile::tempdir().unwrap();
    let root = directory.path();
    let coded = (logical.0.div_ceil(2) * 2, logical.1.div_ceil(2) * 2);
    let duration = 1001;
    let fps = 30;
    let config = StreamingEncoderConfig {
        output_path: root.join(format!("output.{}", format.file_extension())),
        format,
        width: logical.0,
        height: logical.1,
        fps,
        codec,
        prefer_hardware_h264: false,
        execution_mode: crate::ExportExecutionMode::SoftwareOnly,
        software_h264_priority: crate::SoftwareH264Priority::X264First,
        video: VideoEncodeConfig {
            quality: 95,
            speed: VideoEncodingSpeed::VeryFast,
        },
        encode_threads: 1,
        loop_animated_images: false,
        audio: if audio {
            vec![
                crate::StreamingAudioConfig {
                    track_id: "system".into(),
                    title: "Speaker audio".into(),
                    ..Default::default()
                },
                crate::StreamingAudioConfig {
                    track_id: "microphone".into(),
                    title: "Microphone".into(),
                    default: false,
                    ..Default::default()
                },
            ]
        } else {
            Vec::new()
        },
    };
    let source_path = root.join("source.mp4");
    let mut source = config.clone();
    source.output_path = source_path.clone();
    source.format = crate::ExportFormat::Mp4;
    source.width = coded.0;
    source.height = coded.1;
    source.audio.clear();
    source.codec = if hdr {
        crate::VideoCodec::H265
    } else {
        crate::VideoCodec::H264
    };
    let source_codec = source.codec;
    let mut builder = StreamingEncoder::builder(source).recording_source();
    if hdr {
        builder = builder.hdr10_cpu_input();
    }
    let mut encoder = builder.create().unwrap();
    for (pts, color) in [(slots.0, [20u8, 30, 40, 255]), (slots.1, [40, 50, 60, 255])] {
        if hdr {
            let mut rgb =
                ffmpeg::frame::Video::new(ffmpeg::format::Pixel::RGB48LE, coded.0, coded.1);
            let stride = rgb.stride(0);
            for y in 0..coded.1 as usize {
                for x in 0..coded.0 as usize {
                    let start = y * stride + x * 6;
                    for c in 0..3 {
                        rgb.data_mut(0)[start + c * 2..start + c * 2 + 2]
                            .copy_from_slice(&(35000u16 + pts as u16 * 200).to_le_bytes());
                    }
                }
            }
            crate::hdr::frame(&mut rgb, true);
            let mut conversion = ffmpeg::software::scaling::Context::get(
                rgb.format(),
                coded.0,
                coded.1,
                encoder.input_pixel_format(),
                coded.0,
                coded.1,
                ffmpeg::software::scaling::Flags::BICUBIC,
            )
            .unwrap();
            crate::hdr::scaler_colors(&mut conversion, true, false).unwrap();
            let mut converted =
                ffmpeg::frame::Video::new(encoder.input_pixel_format(), coded.0, coded.1);
            conversion.run(&rgb, &mut converted).unwrap();
            encoder
                .push_prepared_video_frame_at_pts(pts - slots.0, converted)
                .unwrap();
        } else {
            let pixels: Vec<_> = (0..coded.0 * coded.1)
                .flat_map(|pixel| {
                    let x = pixel % coded.0;
                    if portrait_change && pts == slots.1 && !(10..22).contains(&x) {
                        [0, 0, 0, 255]
                    } else {
                        color
                    }
                })
                .collect();
            // Simulate a container/encoder normalizing the first admitted PTS.
            encoder
                .push_rgba_frame_at_pts(pts - slots.0, &pixels)
                .unwrap();
        }
    }
    let first_ms = slots.0 * 1000 / u64::from(fps);
    let second_ms = slots.1 * 1000 / u64::from(fps);
    encoder.finish_at_duration_ms(duration - first_ms).unwrap();
    let inputs = root.join("input.bin");
    let mut writer = InputStoreWriter::new(&inputs).unwrap();
    for event in [
        RecordedInputEvent {
            timestamp_ms: 0,
            sequence: 0,
            event: RecordedInput::CursorShape(CursorShapeRecord {
                shape_id: 1,
                hotspot_x: 0,
                hotspot_y: 0,
                width: 2,
                height: 2,
                mode: CursorShapeCompositionMode::AlphaBlend,
                shape_rgba: [0, 255, 0, 255].repeat(4),
            }),
        },
        RecordedInputEvent {
            timestamp_ms: 0,
            sequence: 1,
            event: RecordedInput::Cursor(CursorFrameRecord {
                timestamp_ms: 0,
                x: 4,
                y: 4,
                visible: true,
                shape_id: Some(1),
            }),
        },
        RecordedInputEvent {
            timestamp_ms: second_ms,
            sequence: 2,
            event: RecordedInput::Cursor(CursorFrameRecord {
                timestamp_ms: second_ms,
                x: if portrait_change { 16 } else { 8 },
                y: 4,
                visible: true,
                shape_id: Some(1),
            }),
        },
        RecordedInputEvent {
            timestamp_ms: 1001,
            sequence: 3,
            event: RecordedInput::KeyboardReset,
        },
    ] {
        writer.push(&event).unwrap();
    }
    writer.finish().unwrap();
    let metadata = RenderMetadata {
        render: RenderConfig {
            output_width: logical.0,
            output_height: logical.1,
            output_fps: fps,
            effects: EffectsConfig::default(),
            playback_overlay: PlaybackOverlay::ProgressBar {
                rgba: [255, 0, 0, 255],
            },
        },
        timeline: FinalizedTimeline::new(duration, fps).unwrap(),
        coded_width: coded.0,
        coded_height: coded.1,
    };
    let render = root.join("render.bin");
    write_render_metadata(&render, &metadata).unwrap();
    let settings = root.join("settings.bin");
    write_deferred_output_settings(&settings, &config).unwrap();
    let mouse = root.join("mouse.bin");
    write_mouse_records(&mouse, &MouseStore::new()).unwrap();
    let index = root.join("index.bin");
    let mut bytes = b"SVIDX\0\0".to_vec();
    for (index, time, duration) in [
        (0u64, first_ms, (second_ms - first_ms) as u32),
        (1, second_ms, (duration - second_ms) as u32),
    ] {
        bytes.extend(index.to_le_bytes());
        bytes.extend(time.to_le_bytes());
        bytes.extend(duration.to_le_bytes());
    }
    std::fs::write(&index, bytes).unwrap();
    let mut tracks = Vec::new();
    let mut pcm = Vec::new();
    if audio {
        for (id, role, value) in [
            ("system", AudioTrackRole::SystemOutput, 1000i16),
            ("microphone", AudioTrackRole::MicrophoneInput, 2000),
        ] {
            let path = root.join(format!("{id}.pcm"));
            std::fs::write(
                &path,
                value.to_le_bytes().repeat(duration as usize * 48 * 2),
            )
            .unwrap();
            tracks.push(AudioTrackManifest {
                track_id: id.into(),
                role,
                asset_id: format!("{id}.pcm"),
                sample_rate_hz: 48000,
                channels: 2,
                sample_format: AudioSampleFormat::PcmS16Le,
                duration_frames: duration * 48,
                recorded: true,
            });
            pcm.push(path);
        }
    }
    let geometry = if portrait_change {
        use snow_media::geometry::{
            DesktopRect, DesktopSpace, DesktopTransform, PixelRect, PixelSize,
        };
        [(first_ms, 32, 24, 0, 32), (second_ms, 12, 24, 10, 12)]
            .into_iter()
            .enumerate()
            .map(|(i, (timestamp_ms, width, height, x, destination_width))| {
                snow_recording_model::media::GeometryChange {
                    timestamp_ms,
                    generation: i as u64 + 1,
                    transform: DesktopTransform::new(
                        DesktopRect {
                            space: DesktopSpace::PhysicalPixels,
                            x: 0.0,
                            y: 0.0,
                            width: width as f64,
                            height: height as f64,
                        },
                        PixelSize { width, height },
                    )
                    .unwrap(),
                    destination: PixelRect {
                        x,
                        y: 0,
                        width: destination_width,
                        height: 24,
                    },
                }
            })
            .collect()
    } else {
        Vec::new()
    };
    let manifest = SessionManifest {
        media: snow_recording_model::media::RecordedMedia::new(
            if hdr {
                snow_media::ColorDescription::HDR10
            } else {
                snow_media::ColorDescription::SRGB
            },
            snow_media::CursorMode::Separate,
            geometry,
        ),
        video_codec: source_codec,
        session_id: "fixture".into(),
        output_dir: root.to_path_buf(),
        keep_temp_files: false,
        fps,
        intermediate_profile: snow_recording_model::IntermediateRecordingProfile::EditFast,
        recording_video: config.video,
        width: logical.0,
        height: logical.1,
        capture_origin_x: 0,
        capture_origin_y: 0,
        audio_tracks: tracks.clone(),
        pause_intervals: Vec::new(),
    };
    let mut assets = vec![
        RecordingBundleAsset {
            kind: BundleAssetKind::VideoIndex,
            asset_id: None,
            path: &index,
        },
        RecordingBundleAsset {
            kind: BundleAssetKind::MouseStore,
            asset_id: None,
            path: &mouse,
        },
        RecordingBundleAsset {
            kind: BundleAssetKind::InputEvents,
            asset_id: None,
            path: &inputs,
        },
        RecordingBundleAsset {
            kind: BundleAssetKind::RenderMetadata,
            asset_id: None,
            path: &render,
        },
        RecordingBundleAsset {
            kind: BundleAssetKind::OutputSettings,
            asset_id: None,
            path: &settings,
        },
    ];
    for (track, path) in tracks.iter().zip(&pcm) {
        assets.push(RecordingBundleAsset {
            kind: BundleAssetKind::AudioTrack,
            asset_id: Some(&track.asset_id),
            path,
        });
    }
    snow_recording_model::write_recording_bundle(&source_path, &manifest, &assets).unwrap();
    let bundle = root.join("source.snowrec");
    std::fs::rename(source_path, &bundle).unwrap();
    Fixture {
        _directory: directory,
        bundle,
        config,
        metadata,
    }
}

fn decode_frames(path: &Path) -> (Vec<Vec<u8>>, u32, u32, f64) {
    if path.extension().is_some_and(|ext| ext == "gif") {
        // The packaged GIF demuxer has no parser and emits arbitrary chunks;
        // use the independent GIF decoder to validate the generated animation.
        let mut options = gif::DecodeOptions::new();
        options.set_color_output(gif::ColorOutput::RGBA);
        let mut reader = options.read_info(File::open(path).unwrap()).unwrap();
        let (width, height) = (u32::from(reader.width()), u32::from(reader.height()));
        let mut frames = Vec::new();
        let mut duration = 0.0;
        while let Some(frame) = reader.read_next_frame().unwrap() {
            duration += f64::from(frame.delay) / 100.0;
            frames.push(frame.buffer.to_vec());
        }
        return (frames, width, height, duration);
    }
    let mut input = ffmpeg::format::input(path).unwrap();
    let stream = input.streams().best(ffmpeg::media::Type::Video).unwrap();
    let id = stream.index();
    let time_base = stream.time_base();
    let duration =
        stream.duration() as f64 * time_base.numerator() as f64 / time_base.denominator() as f64;
    let mut decoder = ffmpeg::codec::context::Context::from_parameters(stream.parameters())
        .unwrap()
        .decoder()
        .video()
        .unwrap();
    let (width, height) = (decoder.width(), decoder.height());
    let mut conversion = ffmpeg::software::scaling::Context::get(
        decoder.format(),
        width,
        height,
        ffmpeg::format::Pixel::RGBA,
        width,
        height,
        ffmpeg::software::scaling::Flags::BICUBIC,
    )
    .unwrap();
    let mut frames = Vec::new();
    let mut decoded = ffmpeg::frame::Video::empty();
    let mut rgba = ffmpeg::frame::Video::empty();
    let drain = |decoder: &mut ffmpeg::decoder::Video,
                 frames: &mut Vec<Vec<u8>>,
                 decoded: &mut ffmpeg::frame::Video,
                 rgba: &mut ffmpeg::frame::Video,
                 conversion: &mut ffmpeg::software::scaling::Context| {
        while decoder.receive_frame(decoded).is_ok() {
            assert_eq!((decoded.width(), decoded.height()), (width, height));
            if conversion.input().format != decoded.format() {
                *conversion = ffmpeg::software::scaling::Context::get(
                    decoded.format(),
                    width,
                    height,
                    ffmpeg::format::Pixel::RGBA,
                    width,
                    height,
                    ffmpeg::software::scaling::Flags::BICUBIC,
                )
                .unwrap();
            }
            conversion.run(decoded, rgba).unwrap();
            let stride = rgba.stride(0);
            let mut bytes = Vec::new();
            for y in 0..height as usize {
                bytes.extend_from_slice(&rgba.data(0)[y * stride..y * stride + width as usize * 4]);
            }
            frames.push(bytes);
        }
    };
    for (stream, packet) in input.packets() {
        if stream.index() == id {
            decoder.send_packet(&packet).unwrap();
            drain(
                &mut decoder,
                &mut frames,
                &mut decoded,
                &mut rgba,
                &mut conversion,
            );
        }
    }
    decoder.send_eof().unwrap();
    drain(
        &mut decoder,
        &mut frames,
        &mut decoded,
        &mut rgba,
        &mut conversion,
    );
    (frames, width, height, duration)
}

/// AVI is an SDR MPEG4/MP3 output in the existing codec policy. Its write-only
/// minimal profile omits AVI/MPEG4 decoding; verify its RIFF and VOP structure.
struct Avi {
    codec: Option<ffmpeg::codec::Id>,
    width: u32,
    height: u32,
    scale: u32,
    rate: u32,
    stream_frames: u32,
    packets: Vec<Vec<u8>>,
}
fn inspect_avi(path: &Path) -> Avi {
    fn value(bytes: &[u8], offset: usize) -> u32 {
        u32::from_le_bytes(bytes[offset..offset + 4].try_into().unwrap())
    }
    fn visit(mut bytes: &[u8], avi: &mut Avi) {
        let mut video_stream = false;
        while bytes.len() >= 8 {
            let length = value(bytes, 4) as usize;
            assert!(length <= bytes.len() - 8, "truncated AVI RIFF chunk");
            let payload = &bytes[8..8 + length];
            match &bytes[..4] {
                b"LIST" if payload.len() >= 4 => visit(&payload[4..], avi),
                b"strh" if payload.len() >= 56 => {
                    video_stream = &payload[..4] == b"vids";
                    if video_stream {
                        avi.scale = value(payload, 20);
                        avi.rate = value(payload, 24);
                        avi.stream_frames = value(payload, 32);
                    }
                }
                b"strf" if video_stream && payload.len() >= 40 => {
                    avi.width = value(payload, 4);
                    avi.height = value(payload, 8);
                    avi.codec = Some(match &payload[16..20] {
                        b"H264" | b"h264" | b"avc1" => ffmpeg::codec::Id::H264,
                        b"HEVC" | b"hevc" | b"hvc1" => ffmpeg::codec::Id::HEVC,
                        b"FMP4" => ffmpeg::codec::Id::MPEG4,
                        tag => panic!("unexpected AVI video codec {tag:?}"),
                    });
                }
                b"00dc" | b"00db" if !payload.is_empty() => avi.packets.push(payload.to_vec()),
                _ => {}
            }
            let next = 8 + length + (length & 1);
            if next > bytes.len() {
                assert_eq!(next, bytes.len() + 1);
                break;
            }
            bytes = &bytes[next..];
        }
    }
    let bytes = std::fs::read(path).unwrap();
    assert_eq!(&bytes[..4], b"RIFF");
    assert_eq!(&bytes[8..12], b"AVI ");
    let mut avi = Avi {
        codec: None,
        width: 0,
        height: 0,
        scale: 0,
        rate: 0,
        stream_frames: 0,
        packets: Vec::new(),
    };
    visit(&bytes[12..], &mut avi);
    assert!(avi.width > 0 && avi.height > 0 && avi.scale > 0 && avi.rate > 0);
    avi
}

#[test]
fn render_cannot_replace_or_remove_its_own_source() {
    let fixture = fixture(
        crate::ExportFormat::Mp4,
        crate::VideoCodec::H264,
        false,
        false,
        false,
    );
    let path = fixture.bundle.with_extension("mp4");
    std::fs::rename(&fixture.bundle, &path).unwrap();
    let original = std::fs::read(&path).unwrap();
    let mut config = fixture.config;
    config.output_path = path.clone();
    let result = render_bundle(
        &path,
        config,
        fixture.metadata,
        &CancellationToken::default(),
        |_, _, _, _| {},
    );
    assert!(matches!(
        result,
        Err(RecordingExportError::InvalidConfig(_))
    ));
    assert_eq!(std::fs::read(path).unwrap(), original);
}

#[test]
fn replay_validates_input_tail_before_publication() {
    let fixture = fixture(
        crate::ExportFormat::Mp4,
        crate::VideoCodec::H264,
        false,
        false,
        false,
    );
    std::fs::write(&fixture.config.output_path, b"previous output").unwrap();
    let footer = read_recording_bundle_footer(&fixture.bundle).unwrap();
    let input = footer.asset(BundleAssetKind::InputEvents, None).unwrap();
    let mut file = std::fs::OpenOptions::new()
        .write(true)
        .open(&fixture.bundle)
        .unwrap();
    file.seek(SeekFrom::Start(input.offset + input.len - 4))
        .unwrap();
    file.write_all(&u32::MAX.to_le_bytes()).unwrap();
    file.flush().unwrap();
    let result = render_bundle(
        &fixture.bundle,
        fixture.config.clone(),
        fixture.metadata.clone(),
        &CancellationToken::default(),
        |_, _, _, _| {},
    );
    assert!(result.is_err());
    assert_eq!(
        std::fs::read(&fixture.config.output_path).unwrap(),
        b"previous output"
    );
    assert!(fixture.bundle.is_file());
    file.seek(SeekFrom::Start(input.offset + input.len - 4))
        .unwrap();
    file.write_all(&0u32.to_le_bytes()).unwrap();
    file.flush().unwrap();
    render_bundle(
        &fixture.bundle,
        fixture.config,
        fixture.metadata,
        &CancellationToken::default(),
        |_, _, _, _| {},
    )
    .unwrap();
}

#[test]
fn deferred_thread_cap_only_applies_to_the_measured_automatic_domain() {
    let fixture = fixture(
        crate::ExportFormat::Mp4,
        crate::VideoCodec::H264,
        false,
        false,
        false,
    );
    let mut config = fixture.config;
    config.width = 3840;
    config.height = 2160;
    config.encode_threads = 0;
    let expected = cfg!(windows).then_some(8);
    assert_eq!(deferred_software_threads(&config, 12), expected);
    assert_eq!(
        deferred_software_threads(&config, 4),
        cfg!(windows).then_some(4)
    );
    config.prefer_hardware_h264 = true;
    assert_eq!(deferred_software_threads(&config, 12), expected);
    for mutation in [0, 1, 2, 3, 4, 5, 6] {
        let mut other = config.clone();
        match mutation {
            0 => other.encode_threads = 3,
            1 => other.fps = 60,
            2 => other.width = 1920,
            3 => other.codec = crate::VideoCodec::H265,
            4 => other.format = crate::ExportFormat::Gif,
            5 => other.software_h264_priority = crate::SoftwareH264Priority::OpenH264First,
            _ => other.video.speed = VideoEncodingSpeed::Medium,
        }
        assert_eq!(deferred_software_threads(&other, 12), None);
    }
}

#[test]
fn exact_composed_pixel_reuse_preserves_cfr_and_decoded_frames() {
    let fixture = fixture(
        crate::ExportFormat::Mp4,
        crate::VideoCodec::H264,
        false,
        false,
        false,
    );
    let mut decoded = Vec::new();
    for reuse in [false, true] {
        let mut config = fixture.config.clone();
        config.output_path = fixture
            .bundle
            .parent()
            .unwrap()
            .join(format!("reuse-{reuse}.mp4"));
        let mut encoder = StreamingEncoder::builder(config.clone()).create().unwrap();
        if reuse {
            encoder.set_reuse_identical_rgba(true);
        }
        let mut pixels = vec![0; 32 * 24 * 4];
        for pts in 0..31 {
            let color = if pts < 15 {
                [20, 30, 40, 255]
            } else {
                [50, 60, 70, 255]
            };
            pixels.clear();
            pixels.extend((0..32 * 24).flat_map(|_| color));
            // A small late overlay change must invalidate the full-frame reuse.
            if pts >= 25 {
                pixels[32 * 20 * 4..32 * 20 * 4 + 4].copy_from_slice(&[255, 0, 0, 255]);
            }
            pixels = encoder.push_owned_rgba_frame_at_pts(pts, pixels).unwrap();
        }
        let report = encoder.finish_at_duration_ms(1001).unwrap();
        assert_eq!(report.encoded_frames, 31);
        #[cfg(feature = "bench-timing")]
        assert_eq!(report.timings.cpu_conversions, if reuse { 3 } else { 31 });
        let (frames, width, height, duration) = decode_frames(&config.output_path);
        assert_eq!((width, height, frames.len()), (32, 24, 31));
        assert!((duration - 1.001).abs() < 0.002);
        let input = ffmpeg::format::input(&config.output_path).unwrap();
        let stream = input.streams().best(ffmpeg::media::Type::Video).unwrap();
        assert!(
            stream.avg_frame_rate().numerator() as f64
                / stream.avg_frame_rate().denominator() as f64
                > 29.0
        );
        decoded.push(frames);
    }
    assert_eq!(decoded[0], decoded[1]);
}

#[test]
fn animation_guard_avoids_scans_until_idle_without_changing_decoded_frames() {
    let fixture = fixture(
        crate::ExportFormat::Mp4,
        crate::VideoCodec::H264,
        false,
        false,
        false,
    );
    let mut metadata = fixture.metadata;
    metadata.render.playback_overlay = PlaybackOverlay::None;
    metadata.render.effects.mouse_click_rgba = [255, 100, 20, 255];
    let mut decoded = Vec::new();
    for guard in [false, true] {
        let mut config = fixture.config.clone();
        config.output_path = fixture
            .bundle
            .parent()
            .unwrap()
            .join(format!("animation-guard-{guard}.mp4"));
        let mut encoder = StreamingEncoder::builder(config.clone()).create().unwrap();
        let mut effects = RecordedEffects::new(metadata.render.clone(), (32, 24), None)
            .unwrap()
            .with_timeline(metadata.timeline);
        effects
            .input_effects
            .click(snow_recording_effects::mouse_effects::RenderClick {
                timestamp_ms: 0,
                x: 16,
                y: 12,
                button: snow_recording_effects::mouse_hook::ObservedMouseButton::Left,
            });
        let mut pixels = Vec::new();
        for frame in metadata.timeline.iter() {
            pixels.clear();
            pixels.extend((0..32 * 24).flat_map(|_| [20, 30, 40, 255]));
            effects.apply_rgba(&mut pixels, frame).unwrap();
            encoder.set_reuse_identical_rgba(
                guard && reusable_effect_frame(&effects, &metadata, frame.timestamp_ms),
            );
            pixels = encoder
                .push_owned_rgba_frame_at_pts(frame.pts, pixels)
                .unwrap();
        }
        let report = encoder.finish_at_duration_ms(1001).unwrap();
        assert_eq!(report.encoded_frames, 31);
        #[cfg(feature = "bench-timing")]
        {
            assert_eq!(report.timings.cpu_conversions, if guard { 18 } else { 31 });
            assert_eq!(report.timings.cpu_reuse_checks, if guard { 13 } else { 0 });
        }
        let (frames, width, height, duration) = decode_frames(&config.output_path);
        assert_eq!((width, height, frames.len()), (32, 24, 31));
        assert!((duration - 1.001).abs() < 0.002);
        decoded.push(frames);
    }
    assert_eq!(decoded[0], decoded[1]);
}

#[test]
fn hdr_history_skips_busy_copies_and_recaches_after_animation() {
    let mut pixels = ffmpeg::frame::Video::new(ffmpeg::format::Pixel::RGB48LE, 2, 2);
    let mut previous = Some(ffmpeg::frame::Video::new(
        ffmpeg::format::Pixel::RGB48LE,
        2,
        2,
    ));
    pixels.data_mut(0).fill(1);
    let mut valid = false;
    cache_rgb48(&pixels, &mut previous, &mut valid, true);
    assert!(valid);
    assert!(equal_rgb48(&pixels, previous.as_ref().unwrap()));
    pixels.data_mut(0).fill(2);
    cache_rgb48(&pixels, &mut previous, &mut valid, false);
    assert!(!valid);
    // A known animated frame must not copy over the reusable history buffer.
    assert_eq!(previous.as_ref().unwrap().data(0)[0], 1);
    cache_rgb48(&pixels, &mut previous, &mut valid, true);
    assert!(valid);
    assert!(equal_rgb48(&pixels, previous.as_ref().unwrap()));
    pixels.data_mut(0)[6] = 3;
    assert!(!equal_rgb48(&pixels, previous.as_ref().unwrap()));
}

#[test]
fn playback_progress_keeps_fixed_pixel_height_across_export_dimensions() {
    for size in [(96, 72), (192, 144), (384, 288)] {
        let fixture = fixture_with_source(
            crate::ExportFormat::Apng,
            crate::VideoCodec::H264,
            size,
            false,
            false,
            (0, 15),
            false,
        );
        let result = render_bundle(
            &fixture.bundle,
            fixture.config.clone(),
            fixture.metadata.clone(),
            &CancellationToken::default(),
            |_, _, _, _| {},
        )
        .unwrap();
        let (frames, width, height, _) = decode_frames(&result.output_path);
        assert_eq!((width, height), size);
        let red_rows: Vec<_> = frames
            .last()
            .unwrap()
            .chunks_exact(width as usize * 4)
            .enumerate()
            .filter_map(|(y, row)| {
                row.chunks_exact(4)
                    .all(|pixel| pixel == [255, 0, 0, 255])
                    .then_some(y as u32)
            })
            .collect();
        assert_eq!(
            red_rows,
            (height - 12..height).collect::<Vec<_>>(),
            "export {size:?}"
        );
    }
}

#[test]
fn h264_and_hevc_replay_draw_final_progress_and_clip_non_grid_stop() {
    for codec in [crate::VideoCodec::H264, crate::VideoCodec::H265] {
        let fixture = fixture(crate::ExportFormat::Mp4, codec, false, false, false);
        assert_eq!(
            read_deferred_output_settings(&fixture.bundle)
                .unwrap()
                .codec,
            codec
        );
        let result = render_bundle(
            &fixture.bundle,
            fixture.config.clone(),
            fixture.metadata.clone(),
            &CancellationToken::default(),
            |_, _, _, _| {},
        )
        .unwrap();
        assert_eq!(result.duration_ms, 1001);
        let (frames, width, height, duration) = decode_frames(&result.output_path);
        assert_eq!(frames.len(), 31);
        assert!((duration - 1.001).abs() < 0.002, "{codec:?}: {duration}");
        let offset = ((height - 1) * width + width - 2) as usize * 4;
        let first = &frames.first().unwrap()[offset..offset + 4];
        let last = &frames.last().unwrap()[offset..offset + 4];
        assert!(first[0] < 100);
        assert!(
            last[0] > 180 && last[1] < 100 && last[2] < 100,
            "{codec:?}: {last:?}"
        );
    }
}

#[test]
fn absolute_source_admission_keeps_geometry_and_cursor_changes_on_the_same_slot() {
    let fixture = fixture_with_source(
        crate::ExportFormat::Mp4,
        crate::VideoCodec::H264,
        (32, 24),
        false,
        false,
        (5, 20),
        true,
    );
    render_bundle(
        &fixture.bundle,
        fixture.config.clone(),
        fixture.metadata,
        &CancellationToken::default(),
        |_, _, _, _| {},
    )
    .unwrap();
    let (frames, width, height, duration) = decode_frames(&fixture.config.output_path);
    assert_eq!((width, height, frames.len()), (32, 24, 31));
    assert!((duration - 1.001).abs() < 0.002);
    let pixel =
        |frame: usize, x: usize, y: usize| &frames[frame][(y * 32 + x) * 4..(y * 32 + x + 1) * 4];
    // Probe source content above the fixed 12-pixel playback bar.
    assert!(
        pixel(0, 16, 8)[0] < 30,
        "first source image is held from slot0"
    );
    assert!(
        pixel(19, 16, 8)[0] < 30,
        "background changed before absolute slot20"
    );
    assert!(pixel(20, 16, 8)[0] > 30);
    assert!(
        pixel(19, 4, 4)[1] > 100,
        "old cursor placement disappeared early"
    );
    assert!(
        pixel(20, 4, 4)[1] < 50,
        "portrait letterbox retained the old cursor"
    );
    assert!(
        pixel(20, 16, 4)[1] > 100,
        "new cursor placement is not aligned with geometry"
    );
}

#[test]
fn corrupted_source_index_preserves_destination_and_retry_source() {
    use std::io::{Seek, Write};
    for corrupted_sequence in [true, false] {
        let fixture = fixture(
            crate::ExportFormat::Mp4,
            crate::VideoCodec::H264,
            false,
            false,
            false,
        );
        std::fs::write(&fixture.config.output_path, b"previous output").unwrap();
        let footer = read_recording_bundle_footer(&fixture.bundle).unwrap();
        let index = footer.asset(BundleAssetKind::VideoIndex, None).unwrap();
        let mut file = std::fs::OpenOptions::new()
            .write(true)
            .open(&fixture.bundle)
            .unwrap();
        file.seek(SeekFrom::Start(
            index.offset + snow_recording_model::VIDEO_INDEX_MAGIC.len() as u64,
        ))
        .unwrap();
        file.write_all(&if corrupted_sequence { 1u64 } else { 0 }.to_le_bytes())
            .unwrap();
        if !corrupted_sequence {
            // A timestamp that is not any rational 30-FPS slot must not silently shift replay.
            file.write_all(&1u64.to_le_bytes()).unwrap();
        }
        file.flush().unwrap();
        let error = render_bundle(
            &fixture.bundle,
            fixture.config.clone(),
            fixture.metadata,
            &CancellationToken::default(),
            |_, _, _, _| {},
        )
        .unwrap_err();
        assert!(error.to_string().contains("index"), "{error}");
        assert_eq!(
            std::fs::read(&fixture.config.output_path).unwrap(),
            b"previous output"
        );
        assert!(fixture.bundle.is_file());
    }
}

#[test]
fn source_index_cardinality_and_truncation_fail_before_publication() {
    for variant in [
        "fewer indexed images",
        "more indexed images",
        "truncated record",
    ] {
        let fixture = fixture(
            crate::ExportFormat::Mp4,
            crate::VideoCodec::H264,
            false,
            false,
            false,
        );
        std::fs::write(&fixture.config.output_path, b"previous output").unwrap();
        let footer = read_recording_bundle_footer(&fixture.bundle).unwrap();
        let mut paths = Vec::new();
        for (i, asset) in footer.assets.iter().enumerate() {
            let mut bytes = snow_recording_model::read_recording_bundle_asset(
                &fixture.bundle,
                asset.kind,
                asset.asset_id.as_deref(),
            )
            .unwrap()
            .unwrap();
            if asset.kind == BundleAssetKind::VideoIndex {
                match variant {
                    "fewer indexed images" => {
                        bytes
                            .truncate(bytes.len() - snow_recording_model::VIDEO_INDEX_RECORD_BYTES);
                    }
                    "more indexed images" => {
                        bytes.extend(2u64.to_le_bytes());
                        bytes.extend(666u64.to_le_bytes());
                        bytes.extend(335u32.to_le_bytes());
                    }
                    "truncated record" => {
                        bytes.pop();
                    }
                    _ => unreachable!(),
                }
            }
            let path = fixture._directory.path().join(format!("repacked-{i}"));
            std::fs::write(&path, bytes).unwrap();
            paths.push(path);
        }
        std::fs::OpenOptions::new()
            .write(true)
            .open(&fixture.bundle)
            .unwrap()
            .set_len(footer.video_payload_len)
            .unwrap();
        let assets: Vec<_> = footer
            .assets
            .iter()
            .zip(&paths)
            .map(|(asset, path)| RecordingBundleAsset {
                kind: asset.kind,
                asset_id: asset.asset_id.as_deref(),
                path,
            })
            .collect();
        snow_recording_model::write_recording_bundle(&fixture.bundle, &footer.manifest, &assets)
            .unwrap();
        let error = render_bundle(
            &fixture.bundle,
            fixture.config.clone(),
            fixture.metadata,
            &CancellationToken::default(),
            |_, _, _, _| {},
        )
        .unwrap_err();
        assert!(error.to_string().contains("index"), "{variant}: {error}");
        assert_eq!(
            std::fs::read(&fixture.config.output_path).unwrap(),
            b"previous output"
        );
        assert!(fixture.bundle.is_file());
    }
}

#[test]
fn avi_replay_preserves_mpeg4_canvas_frame_grid_and_packet_structure() {
    for codec in [crate::VideoCodec::H264, crate::VideoCodec::H265] {
        let fixture = fixture(crate::ExportFormat::Avi, codec, false, false, false);
        let result = render_bundle(
            &fixture.bundle,
            fixture.config,
            fixture.metadata,
            &CancellationToken::default(),
            |_, _, _, _| {},
        )
        .unwrap();
        // The frozen logical endpoint stays exact; AVI's displayed duration is
        // quantized by its fixed-rate frame grid.
        assert_eq!(result.duration_ms, 1001);
        let avi = inspect_avi(&result.output_path);
        assert_eq!((avi.width, avi.height, avi.packets.len()), (32, 24, 31));
        assert_eq!(avi.codec, Some(ffmpeg::codec::Id::MPEG4));
        assert!(
            avi.packets
                .iter()
                .all(|packet| packet.windows(4).any(|v| v == [0, 0, 1, 0xB6]))
        );
        let duration = f64::from(avi.stream_frames) * f64::from(avi.scale) / f64::from(avi.rate);
        assert!((duration - 31.0 / 30.0).abs() < 0.002);
    }
}

#[test]
fn odd_animated_canvases_restore_crop_and_do_not_loop() {
    for format in [
        crate::ExportFormat::Gif,
        crate::ExportFormat::Apng,
        crate::ExportFormat::Webp,
    ] {
        let fixture = fixture(format, crate::VideoCodec::H264, true, false, false);
        render_bundle(
            &fixture.bundle,
            fixture.config.clone(),
            fixture.metadata.clone(),
            &CancellationToken::default(),
            |_, _, _, _| {},
        )
        .unwrap();
        let bytes = std::fs::read(&fixture.config.output_path).unwrap();
        match format {
            crate::ExportFormat::Gif => {
                assert_eq!(u16::from_le_bytes([bytes[6], bytes[7]]), 33);
                assert_eq!(u16::from_le_bytes([bytes[8], bytes[9]]), 25);
                assert!(!bytes.windows(11).any(|v| v == b"NETSCAPE2.0"));
                let (frames, w, h, _) = decode_frames(&fixture.config.output_path);
                assert_eq!((w, h), (33, 25));
                assert!(frames.len() > 1);
            }
            crate::ExportFormat::Apng => {
                let chunk = bytes.windows(4).position(|v| v == b"acTL").unwrap();
                assert_eq!(
                    u32::from_be_bytes(bytes[chunk + 8..chunk + 12].try_into().unwrap()),
                    1
                );
                let (frames, w, h, _) = decode_frames(&fixture.config.output_path);
                assert_eq!((w, h), (33, 25));
                assert!(frames.len() > 1);
            }
            crate::ExportFormat::Webp => {
                assert_eq!(&bytes[..4], b"RIFF");
                let vp8x = bytes.windows(4).position(|v| v == b"VP8X").unwrap();
                let width =
                    u32::from_le_bytes([bytes[vp8x + 12], bytes[vp8x + 13], bytes[vp8x + 14], 0])
                        + 1;
                let height =
                    u32::from_le_bytes([bytes[vp8x + 15], bytes[vp8x + 16], bytes[vp8x + 17], 0])
                        + 1;
                assert_eq!((width, height), (33, 25));
                let anim = bytes.windows(4).position(|v| v == b"ANIM").unwrap();
                assert_eq!(u16::from_le_bytes([bytes[anim + 12], bytes[anim + 13]]), 1);
                let (frames, w, h, _) = decode_frames(&fixture.config.output_path);
                assert_eq!((w, h), (33, 25));
                assert!(frames.len() > 1);
            }
            _ => unreachable!(),
        }
    }
}

#[test]
fn replay_preserves_separate_track_titles_defaults_and_bounded_audio_extent() {
    let fixture = fixture(
        crate::ExportFormat::Mp4,
        crate::VideoCodec::H264,
        false,
        true,
        false,
    );
    render_bundle(
        &fixture.bundle,
        fixture.config.clone(),
        fixture.metadata.clone(),
        &CancellationToken::default(),
        |_, _, _, _| {},
    )
    .unwrap();
    let input = ffmpeg::format::input(&fixture.config.output_path).unwrap();
    let tracks: Vec<_> = input
        .streams()
        .filter(|s| s.parameters().medium() == ffmpeg::media::Type::Audio)
        .collect();
    assert_eq!(tracks.len(), 2);
    for (track, title) in tracks.iter().zip(["Speaker audio", "Microphone"]) {
        assert_eq!(track.metadata().get("handler_name"), Some(title));
        let duration = track.duration() as f64 * track.time_base().numerator() as f64
            / track.time_base().denominator() as f64;
        assert!((duration - 1.001).abs() < 0.002, "AAC duration {duration}");
    }
}

#[test]
fn replay_mixed_and_disabled_audio_route_the_same_raw_source_tracks() {
    let fixture = fixture(
        crate::ExportFormat::Mp4,
        crate::VideoCodec::H264,
        false,
        true,
        false,
    );
    let mut mixed = fixture.config.clone();
    mixed.audio = vec![Default::default()];
    let footer = read_recording_bundle_footer(&fixture.bundle).unwrap();
    let mut replay = AudioReplay::open(&fixture.bundle, &footer, &mixed).unwrap();
    let mut encoder = StreamingEncoder::builder(mixed.clone()).create().unwrap();
    encoder
        .push_rgba_frame_at_pts(0, &[20, 30, 40, 255].repeat(32 * 24))
        .unwrap();
    replay
        .emit_until(48048, &mut encoder, &CancellationToken::default())
        .unwrap();
    assert_eq!(replay.next, 48048);
    assert_eq!(replay.mixed.len(), 96);
    assert!(replay.mixed.iter().all(|sample| *sample == 3000));
    encoder.finish_at_duration_ms(1001).unwrap();
    let input = ffmpeg::format::input(&mixed.output_path).unwrap();
    assert_eq!(
        input
            .streams()
            .filter(|s| s.parameters().medium() == ffmpeg::media::Type::Audio)
            .count(),
        1
    );
    drop(input);
    let mut muted = fixture.config.clone();
    muted.audio.clear();
    render_bundle(
        &fixture.bundle,
        muted.clone(),
        fixture.metadata.clone(),
        &CancellationToken::default(),
        |_, _, _, _| {},
    )
    .unwrap();
    let input = ffmpeg::format::input(&muted.output_path).unwrap();
    assert_eq!(
        input
            .streams()
            .filter(|s| s.parameters().medium() == ffmpeg::media::Type::Audio)
            .count(),
        0
    );
}

#[test]
fn hdr_clean_source_replays_with_hdr_color_and_overlay_or_tone_maps_to_animation() {
    for (format, codec) in [
        (crate::ExportFormat::Mp4, crate::VideoCodec::H265),
        (crate::ExportFormat::Avi, crate::VideoCodec::H265),
        (crate::ExportFormat::Gif, crate::VideoCodec::H264),
    ] {
        let fixture = fixture(format, codec, false, false, true);
        render_bundle(
            &fixture.bundle,
            fixture.config.clone(),
            fixture.metadata.clone(),
            &CancellationToken::default(),
            |_, _, _, _| {},
        )
        .unwrap();
        if format == crate::ExportFormat::Avi {
            let avi = inspect_avi(&fixture.config.output_path);
            assert_eq!((avi.width, avi.height, avi.packets.len()), (32, 24, 31));
            assert_eq!(avi.codec, Some(ffmpeg::codec::Id::MPEG4));
            continue;
        }
        let input = ffmpeg::format::input(&fixture.config.output_path).unwrap();
        let stream = input.streams().best(ffmpeg::media::Type::Video).unwrap();
        let parameters = stream.parameters();
        if crate::preserves_hdr_output(true, format, codec) {
            unsafe {
                assert_eq!(
                    (*parameters.as_ptr()).color_trc,
                    ffmpeg::ffi::AVColorTransferCharacteristic::AVCOL_TRC_SMPTE2084
                );
                assert_eq!(
                    (*parameters.as_ptr()).color_primaries,
                    ffmpeg::ffi::AVColorPrimaries::AVCOL_PRI_BT2020
                );
            }
            let decoder = ffmpeg::codec::context::Context::from_parameters(parameters)
                .unwrap()
                .decoder()
                .video()
                .unwrap();
            assert!(matches!(
                decoder.format(),
                ffmpeg::format::Pixel::YUV420P10LE | ffmpeg::format::Pixel::P010LE
            ));
            drop(input);
            let (frames, width, height, _) = decode_frames(&fixture.config.output_path);
            assert_eq!((width, height, frames.len()), (32, 24, 31));
        } else {
            drop(input);
            let (frames, _, _, _) = decode_frames(&fixture.config.output_path);
            assert!(frames.len() > 1);
        }
    }
}

#[test]
fn cancel_prepare_and_processing_preserves_source_destination_and_allows_retry() {
    let fixture = fixture(
        crate::ExportFormat::Mp4,
        crate::VideoCodec::H264,
        false,
        false,
        false,
    );
    std::fs::write(&fixture.config.output_path, b"previous output").unwrap();
    let active = Arc::new(AtomicBool::new(false));
    let (_release, gate) = crossbeam_channel::bounded(0);
    let task = DeferredRenderTask::start_inner(
        fixture.bundle.clone(),
        fixture.config.clone(),
        Arc::clone(&active),
        Some(gate),
    )
    .unwrap();
    assert!(
        DeferredRenderTask::start(
            fixture.bundle.clone(),
            fixture.config.clone(),
            Arc::clone(&active)
        )
        .is_err()
    );
    task.cancel();
    assert!(matches!(
        task.wait(),
        Err(RecordingExportError::ExportCanceled)
    ));
    assert!(!active.load(Ordering::Acquire));
    assert!(fixture.bundle.exists());
    assert_eq!(
        std::fs::read(&fixture.config.output_path).unwrap(),
        b"previous output"
    );
    let cancellation = CancellationToken::default();
    let cancel = cancellation.clone();
    let result = render_bundle(
        &fixture.bundle,
        fixture.config.clone(),
        fixture.metadata.clone(),
        &cancellation,
        |stage, _, _, _| {
            if stage == ExportStage::VideoEncode {
                cancel.cancel();
            }
        },
    );
    assert!(matches!(result, Err(RecordingExportError::ExportCanceled)));
    assert_eq!(
        std::fs::read(&fixture.config.output_path).unwrap(),
        b"previous output"
    );
    let cancellation = CancellationToken::default();
    let cancel = cancellation.clone();
    let result = render_bundle(
        &fixture.bundle,
        fixture.config.clone(),
        fixture.metadata.clone(),
        &cancellation,
        |stage, _, _, _| {
            if stage == ExportStage::Finalize {
                cancel.cancel();
            }
        },
    );
    assert!(matches!(result, Err(RecordingExportError::ExportCanceled)));
    assert!(fixture.bundle.exists());
    assert_eq!(
        std::fs::read(&fixture.config.output_path).unwrap(),
        b"previous output"
    );
    let task = DeferredRenderTask::start(
        fixture.bundle.clone(),
        fixture.config.clone(),
        Arc::clone(&active),
    )
    .unwrap();
    let progress = Arc::clone(&task.progress);
    task.wait().unwrap();
    let snapshot = progress.lock().unwrap();
    assert_eq!(snapshot.state, DeferredRenderState::Succeeded);
    assert_eq!(snapshot.percent, 100.0);
    assert!(!fixture.bundle.exists());
    assert!(!active.load(Ordering::Acquire));
}

#[test]
fn failed_task_retains_owned_error_and_source_for_retry() {
    let fixture = fixture(
        crate::ExportFormat::Mp4,
        crate::VideoCodec::H264,
        false,
        false,
        false,
    );
    std::fs::create_dir(&fixture.config.output_path).unwrap();
    let active = Arc::new(AtomicBool::new(false));
    let task = DeferredRenderTask::start(
        fixture.bundle.clone(),
        fixture.config.clone(),
        Arc::clone(&active),
    )
    .unwrap();
    let progress = Arc::clone(&task.progress);
    assert!(task.wait().is_err());
    let snapshot = progress.lock().unwrap().clone();
    assert_eq!(snapshot.state, DeferredRenderState::Failed);
    assert!(snapshot.error.is_some());
    assert!(fixture.bundle.exists());
    std::fs::remove_dir(&fixture.config.output_path).unwrap();
    DeferredRenderTask::start(fixture.bundle.clone(), fixture.config.clone(), active)
        .unwrap()
        .wait()
        .unwrap();
}
