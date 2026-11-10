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

fn add_webcam(fixture: &Fixture, shape: snow_recording_model::WebcamShape) {
    let root = fixture._directory.path();
    let mut config = fixture.config.clone();
    config.output_path = root.join("camera.mp4");
    config.format = crate::ExportFormat::Mp4;
    config.width = 50;
    config.height = 50;
    config.fps = 30;
    config.audio.clear();
    let mut encoder = StreamingEncoder::builder(config.clone())
        .recording_source()
        .create()
        .unwrap();
    for (pts, color) in [
        (6, [240, 10, 10, 255]),
        (15, [10, 240, 10, 255]),
        (27, [10, 10, 240, 255]),
    ] {
        let mut pixels = color.repeat(50 * 50);
        // An off-center white marker proves replay applies orientation exactly once.
        for y in 32..42 {
            for x in 32..42 {
                pixels[(y * 50 + x) * 4..(y * 50 + x + 1) * 4]
                    .copy_from_slice(&[245, 245, 245, 255]);
            }
        }
        encoder.push_rgba_frame_at_pts(pts, &pixels).unwrap();
    }
    encoder.finish_at_duration_ms(1001).unwrap();
    let camera_index = root.join("camera.index");
    let mut bytes = b"SVIDX\0\0".to_vec();
    for (index, at, duration) in [(0u64, 200u64, 300u32), (1, 500, 400), (2, 900, 101)] {
        bytes.extend(index.to_le_bytes());
        bytes.extend(at.to_le_bytes());
        bytes.extend(duration.to_le_bytes());
    }
    std::fs::write(&camera_index, bytes).unwrap();
    let metadata = root.join("camera.bin");
    snow_recording_model::webcam::write_metadata(
        &metadata,
        &snow_recording_model::webcam::WebcamMetadata {
            style: snow_recording_model::WebcamStyle {
                size: 50,
                shape,
                offset_left: 17,
                offset_bottom: 31,
                orientation: snow_recording_model::WebcamOrientation::MirroredClockwise90,
            },
            fps: 30,
            duration_ms: 1001,
            visibility: vec![(200, true), (700, false), (900, true)],
        },
    )
    .unwrap();
    let footer = read_recording_bundle_footer(&fixture.bundle).unwrap();
    let mut bundle = File::open(&fixture.bundle).unwrap();
    let paths: Vec<_> = footer
        .assets
        .iter()
        .enumerate()
        .map(|(i, asset)| {
            let path = root.join(format!("saved-{i}.bin"));
            bundle.seek(SeekFrom::Start(asset.offset)).unwrap();
            std::io::copy(
                &mut (&mut bundle).take(asset.len),
                &mut File::create(&path).unwrap(),
            )
            .unwrap();
            path
        })
        .collect();
    drop(bundle);
    std::fs::OpenOptions::new()
        .write(true)
        .open(&fixture.bundle)
        .unwrap()
        .set_len(footer.video_payload_len)
        .unwrap();
    let mut assets: Vec<_> = footer
        .assets
        .iter()
        .zip(&paths)
        .map(|(asset, path)| RecordingBundleAsset {
            kind: asset.kind,
            asset_id: asset.asset_id.as_deref(),
            path,
        })
        .collect();
    assets.extend([
        RecordingBundleAsset {
            kind: BundleAssetKind::WebcamVideo,
            asset_id: None,
            path: &config.output_path,
        },
        RecordingBundleAsset {
            kind: BundleAssetKind::WebcamIndex,
            asset_id: None,
            path: &camera_index,
        },
        RecordingBundleAsset {
            kind: BundleAssetKind::WebcamMetadata,
            asset_id: None,
            path: &metadata,
        },
    ]);
    snow_recording_model::write_recording_bundle(&fixture.bundle, &footer.manifest, &assets)
        .unwrap();
}

#[test]
fn webcam_deferred_formats_replay_gaps_rewinds_and_retries_on_source_time() {
    for format in [
        crate::ExportFormat::Mp4,
        crate::ExportFormat::Gif,
        crate::ExportFormat::Apng,
        crate::ExportFormat::Webp,
    ] {
        for shape in [
            snow_recording_model::WebcamShape::Square,
            snow_recording_model::WebcamShape::Circle,
        ] {
            let fixture = fixture_with_source(
                format,
                crate::VideoCodec::H264,
                (240, 180),
                false,
                false,
                (0, 15),
                false,
            );
            add_webcam(&fixture, shape);
            let cancel = CancellationToken::default();
            let mut replay = crate::webcam::WebcamReplay::open(&fixture.bundle, &cancel)
                .unwrap()
                .unwrap();
            let held = replay.frame_at(200, &cancel).unwrap().unwrap();
            assert_eq!(
                replay.style().orientation,
                snow_recording_model::WebcamOrientation::MirroredClockwise90
            );
            assert!(
                held.rgba[(36 * 50 + 36) * 4 + 1] > 180,
                "camera track retains its untransformed marker"
            );
            assert!(
                Arc::ptr_eq(&held, &replay.frame_at(499, &cancel).unwrap().unwrap()),
                "held camera samples reuse conversion and overlay caches"
            );
            for (at, channel) in [
                (0, None),
                (200, Some(0)),
                (499, Some(0)),
                (500, Some(1)),
                (700, None),
                (900, Some(2)),
                (200, Some(0)),
            ] {
                let image = replay.frame_at(at, &cancel).unwrap();
                assert_eq!(image.is_some(), channel.is_some());
                if let (Some(image), Some(channel)) = (image, channel) {
                    assert!(image.rgba[channel] > 200, "camera at {at} has wrong color");
                }
            }
            let mut metadata = fixture.metadata.clone();
            metadata.render.playback_overlay = PlaybackOverlay::None;
            let source = crate::clip::ClipSource::open(&fixture.bundle, None).unwrap();
            let mut reader = crate::clip::VideoReader::open(source, false).unwrap();
            let mut preview = Vec::new();
            for (at, channel) in [(200_000, 0), (500_000, 1), (900_000, 2), (200_000, 0)] {
                reader.read(at, &cancel).unwrap();
                reader.copy_rgba(&mut preview).unwrap();
                assert!(
                    preview[((124 * 240 + 42) * 4) + channel] > 200,
                    "trim preview camera at {at}"
                );
                let marker = (111 * 240 + 29) * 4;
                assert!(
                    preview[marker..marker + 3].iter().all(|c| *c > 180),
                    "oriented marker must survive preview and backward seeks at {at}"
                );
            }
            render_bundle(
                &fixture.bundle,
                fixture.config.clone(),
                metadata.clone(),
                &cancel,
                |_, _, _, _| {},
            )
            .unwrap();
            let (frames, width, _, _) = decode_frames(&fixture.config.output_path);
            let center = ((124 * width + 42) * 4) as usize;
            let marker = ((111 * width + 29) * 4) as usize;
            assert!(
                frames.last().unwrap()[marker..marker + 3]
                    .iter()
                    .all(|c| *c > 170),
                "{format:?}: export must apply the saved orientation"
            );
            assert!(
                frames
                    .iter()
                    .any(|frame| frame[center] > 200 && frame[center + 1] < 60),
                "{format:?}: red camera frame missing {:?}",
                frames
                    .iter()
                    .map(|frame| &frame[center..center + 4])
                    .collect::<Vec<_>>()
            );
            assert!(
                frames
                    .iter()
                    .any(|frame| frame[center + 1] > 200 && frame[center] < 60),
                "{format:?}: green camera frame missing"
            );
            assert!(
                frames.last().unwrap()[center + 2] > 200,
                "{format:?}: final camera frame missing"
            );
            assert!(
                fixture.bundle.exists(),
                "rendering must preserve source for retry"
            );
            let mut retry = fixture.config.clone();
            retry.output_path = fixture
                ._directory
                .path()
                .join(format!("retry.{}", format.file_extension()));
            let result = render_bundle_range_with_speed(
                &fixture.bundle,
                retry,
                metadata,
                snow_recording_model::TrimRange::new(200_000, 700_000, 1_001_000).unwrap(),
                snow_recording_model::PlaybackSpeed::from_percent(200).unwrap(),
                &cancel,
                |_, _, _, _| {},
            )
            .unwrap();
            assert_eq!(result.duration_ms, 250);
            let (retimed, width, _, _) = decode_frames(&result.output_path);
            let center = ((124 * width + 42) * 4) as usize;
            assert!(retimed.first().unwrap()[center] > 200);
            assert!(retimed.last().unwrap()[center + 1] > 200);
            let marker = ((111 * width + 29) * 4) as usize;
            assert!(
                retimed.first().unwrap()[marker..marker + 3]
                    .iter()
                    .all(|c| *c > 170)
            );
            assert!(
                retimed.last().unwrap()[marker..marker + 3]
                    .iter()
                    .all(|c| *c > 170)
            );
        }
    }
}

#[test]
fn held_composition_matches_forced_redraw_across_inputs_expiry_webcam_and_playback() {
    use snow_recording_effects::keyboard_overlay::KeyEvent;
    for overlay in [
        PlaybackOverlay::None,
        PlaybackOverlay::PlaybackTime { rgba: [255; 4] },
    ] {
        let fixture = fixture_with_source(
            crate::ExportFormat::Mp4,
            crate::VideoCodec::H264,
            (320, 120),
            true,
            false,
            (0, 15),
            false,
        );
        add_webcam(&fixture, snow_recording_model::WebcamShape::Circle);
        let mut metadata = fixture.metadata.clone();
        metadata.render.playback_overlay = overlay;
        metadata.render.effects.mouse_click_rgba = [255, 100, 20, 255];
        metadata.render.effects.mouse_trail_rgba = [255, 40, 60, 180];
        metadata.render.effects.show_keyboard = true;
        metadata.render.effects.record_mouse_clicks = true;
        metadata.render.effects.record_mouse_scrolling = true;
        metadata.render.effects.keyboard = Some(snow_recording_model::KeyboardOverlayConfig {
            font: None,
            keycap_size: 32,
            background_rgba: [0, 0, 0, 180],
            text_rgba: [255; 4],
            border_rgba: [255; 4],
            labels: Default::default(),
        });
        let shape = CursorShapeRecord {
            shape_id: 1,
            hotspot_x: 0,
            hotspot_y: 0,
            width: 2,
            height: 2,
            mode: CursorShapeCompositionMode::AlphaBlend,
            shape_rgba: [0, 255, 0, 255].repeat(4),
        };
        let key = KeyEvent {
            at_ms: 90,
            key: 65,
            down: true,
            label: "A".into(),
            modifiers: Vec::new(),
        };
        let mut outputs = Vec::new();
        for force_redraw in [false, true] {
            let mut events = vec![
                (0, RecordedInput::CursorShape(shape.clone())),
                (
                    0,
                    RecordedInput::Cursor(CursorFrameRecord {
                        timestamp_ms: 0,
                        x: 4,
                        y: 4,
                        visible: true,
                        shape_id: Some(1),
                    }),
                ),
                (
                    33,
                    RecordedInput::Click(snow_recording_model::RecordedMouseClick {
                        x: 160,
                        y: 60,
                        button: snow_recording_model::InputMouseButton::Left,
                        down: true,
                        key: KeyEvent {
                            at_ms: 33,
                            key: 0x200,
                            label: "Left click".into(),
                            ..key.clone()
                        },
                    }),
                ),
                // Mouse and keyboard observers admit a batch in source-specific
                // order, while keycap rendering must retain their original times.
                (
                    100,
                    RecordedInput::Scroll(KeyEvent {
                        at_ms: 100,
                        key: 0x205,
                        label: "Scroll up".into(),
                        ..key.clone()
                    }),
                ),
                (100, RecordedInput::Key(key.clone())),
                (
                    200,
                    RecordedInput::Cursor(CursorFrameRecord {
                        timestamp_ms: 200,
                        x: 16,
                        y: 4,
                        visible: true,
                        shape_id: Some(1),
                    }),
                ),
                (
                    300,
                    RecordedInput::Key(KeyEvent {
                        at_ms: 300,
                        down: false,
                        ..key.clone()
                    }),
                ),
                (
                    333,
                    RecordedInput::Scroll(KeyEvent {
                        at_ms: 333,
                        key: 0x205,
                        label: "Scroll up".into(),
                        ..key.clone()
                    }),
                ),
                (
                    400,
                    RecordedInput::Scroll(KeyEvent {
                        at_ms: 500,
                        key: 0x206,
                        label: "Scroll down".into(),
                        ..key.clone()
                    }),
                ),
                (700, RecordedInput::Reset),
                (
                    733,
                    RecordedInput::Pointer {
                        position: Some((240, 60)),
                        continuity: 0,
                        at_ms: 800,
                    },
                ),
            ];
            if force_redraw {
                // Re-admitting the same shape changes no pixels, but invalidates every
                // composition. This supplies an independent full-redraw reference.
                events.extend(metadata.timeline.iter().map(|frame| {
                    (
                        frame.timestamp_ms,
                        RecordedInput::CursorShape(shape.clone()),
                    )
                }));
            }
            events.sort_by_key(|event| event.0);
            let footer = read_recording_bundle_footer(&fixture.bundle).unwrap();
            let mut bundle = File::open(&fixture.bundle).unwrap();
            let paths: Vec<_> = footer
                .assets
                .iter()
                .enumerate()
                .map(|(i, asset)| {
                    let path = fixture
                        ._directory
                        .path()
                        .join(format!("comparison-{force_redraw}-{i}.bin"));
                    if asset.kind == BundleAssetKind::InputEvents {
                        let mut writer = InputStoreWriter::new(&path).unwrap();
                        for (sequence, (timestamp_ms, event)) in events.iter().enumerate() {
                            writer
                                .push(&RecordedInputEvent {
                                    timestamp_ms: *timestamp_ms,
                                    sequence: sequence as u64,
                                    event: event.clone(),
                                })
                                .unwrap();
                        }
                        writer.finish().unwrap();
                    } else {
                        bundle.seek(SeekFrom::Start(asset.offset)).unwrap();
                        std::io::copy(
                            &mut (&mut bundle).take(asset.len),
                            &mut File::create(&path).unwrap(),
                        )
                        .unwrap();
                    }
                    path
                })
                .collect();
            drop(bundle);
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
            snow_recording_model::write_recording_bundle(
                &fixture.bundle,
                &footer.manifest,
                &assets,
            )
            .unwrap();
            let result = render_bundle(
                &fixture.bundle,
                fixture.config.clone(),
                metadata.clone(),
                &CancellationToken::default(),
                |_, _, _, _| {},
            )
            .unwrap();
            outputs.push(std::fs::read(result.output_path).unwrap());
        }
        assert!(
            outputs[0] == outputs[1],
            "held output differs from full redraw: {overlay:?}"
        );
    }
}

#[test]
fn deferred_static_output_holds_composed_images_without_losing_audio_or_endpoint() {
    let fixture = fixture(
        crate::ExportFormat::Mp4,
        crate::VideoCodec::H264,
        false,
        true,
        false,
    );
    let mut metadata = fixture.metadata.clone();
    metadata.render.playback_overlay = PlaybackOverlay::None;
    let result = render_bundle(
        &fixture.bundle,
        fixture.config.clone(),
        metadata,
        &CancellationToken::default(),
        |_, _, _, _| {},
    )
    .unwrap();
    let (frames, width, height, duration) = decode_frames(&result.output_path);
    assert_eq!((width, height, frames.len()), (32, 24, 2));
    assert!((duration - 1.001).abs() < 0.002);
    let mut input = ffmpeg::format::input(&result.output_path).unwrap();
    let video = input.streams().best(ffmpeg::media::Type::Video).unwrap();
    let index = video.index();
    let time_base = video.time_base();
    let mut pts: Vec<_> = input
        .packets()
        .filter(|(stream, _)| stream.index() == index)
        .map(|(_, packet)| packet.pts().unwrap().rescale(time_base, (1, 30)))
        .collect();
    pts.sort_unstable();
    assert_eq!(pts, [0, 15]);
    let audio: Vec<_> = input
        .streams()
        .filter(|stream| stream.parameters().medium() == ffmpeg::media::Type::Audio)
        .collect();
    assert_eq!(audio.len(), 2);
    for stream in audio {
        assert!((stream.duration().rescale(stream.time_base(), (1, 48_000)) - 48_048).abs() <= 1);
    }
    assert!(fixture.bundle.exists());
}

#[test]
fn deferred_progress_holds_repeated_pixel_widths_and_preserves_the_final_fill() {
    for codec in [crate::VideoCodec::H264, crate::VideoCodec::H265] {
        let fixture = fixture_with_source(
            crate::ExportFormat::Mp4,
            codec,
            (16, 24),
            false,
            codec == crate::VideoCodec::H265,
            (0, 13),
            false,
        );
        let result = render_bundle(
            &fixture.bundle,
            fixture.config,
            fixture.metadata,
            &CancellationToken::default(),
            |_, _, _, _| {},
        )
        .unwrap();
        let (frames, width, height, duration) = decode_frames(&result.output_path);
        assert!(
            frames.len() <= 19,
            "repeated progress widths must be held: {}",
            frames.len()
        );
        assert!((duration - 1.001).abs() < 0.002);
        let offset = ((height - 1) * width + width - 2) as usize * 4;
        if codec == crate::VideoCodec::H264 {
            assert!(frames.first().unwrap()[offset] < 100);
            assert!(frames.last().unwrap()[offset] > 180);
        }
        // HDR contains BT.2020/PQ red rather than full-range sRGB channel values.
        let last = &frames.last().unwrap()[offset..offset + 4];
        assert!(
            last[0] > last[1] && last[0] > last[2],
            "{codec:?}: final progress pixel {:?}",
            last
        );
        let mut input = ffmpeg::format::input(&result.output_path).unwrap();
        let stream = input.streams().best(ffmpeg::media::Type::Video).unwrap();
        assert_eq!(
            stream.duration().rescale(stream.time_base(), (1, 1_000)),
            1001
        );
        let index = stream.index();
        let time_base = stream.time_base();
        let pts: Vec<_> = input
            .packets()
            .filter(|(stream, _)| stream.index() == index)
            .map(|(_, packet)| packet.pts().unwrap().rescale(time_base, (1, 30)))
            .collect();
        assert!(
            pts.contains(&13),
            "source and cursor changes during a repeated bar width must survive: {pts:?}"
        );
    }
}

#[test]
fn deferred_hdr_static_output_applies_color_policy_and_keeps_the_exact_endpoint() {
    let fixture = fixture(
        crate::ExportFormat::Mp4,
        crate::VideoCodec::H265,
        false,
        false,
        true,
    );
    let mut metadata = fixture.metadata;
    metadata.render.playback_overlay = PlaybackOverlay::None;
    let result = render_bundle(
        &fixture.bundle,
        fixture.config.clone(),
        metadata,
        &CancellationToken::default(),
        |_, _, _, _| {},
    )
    .unwrap();
    let (frames, _, _, duration) = decode_frames(&result.output_path);
    assert_eq!(frames.len(), 2);
    assert!((duration - 1.001).abs() < 0.002);
    assert_eq!(
        crate::clip::ClipSource::open(&result.output_path, Some(fixture.config))
            .unwrap()
            .hdr,
        cfg!(feature = "hdr-encoding")
    );
}

#[test]
fn deferred_playback_time_holds_each_second_without_missing_the_final_badge() {
    let fixture = fixture_with_source(
        crate::ExportFormat::Mp4,
        crate::VideoCodec::H264,
        (320, 72),
        false,
        false,
        (0, 15),
        false,
    );
    let mut metadata = fixture.metadata;
    metadata.render.playback_overlay = PlaybackOverlay::PlaybackTime { rgba: [255; 4] };
    let result = render_bundle(
        &fixture.bundle,
        fixture.config,
        metadata,
        &CancellationToken::default(),
        |_, _, _, _| {},
    )
    .unwrap();
    let (frames, _, _, duration) = decode_frames(&result.output_path);
    assert_eq!(frames.len(), 3);
    assert!((duration - 1.001).abs() < 0.002);
    let mut input = ffmpeg::format::input(&result.output_path).unwrap();
    let stream = input.streams().best(ffmpeg::media::Type::Video).unwrap();
    let index = stream.index();
    let time_base = stream.time_base();
    let mut pts: Vec<_> = input
        .packets()
        .filter(|(stream, _)| stream.index() == index)
        .map(|(_, packet)| packet.pts().unwrap().rescale(time_base, (1, 30)))
        .collect();
    pts.sort_unstable();
    assert_eq!(pts, [0, 15, 30]);
}

#[test]
fn deferred_static_holds_preserve_animation_delays_and_the_avi_frame_grid() {
    for format in [
        crate::ExportFormat::Gif,
        crate::ExportFormat::Apng,
        crate::ExportFormat::Webp,
        crate::ExportFormat::Avi,
    ] {
        if format == crate::ExportFormat::Avi && !crate::has_avi_compatibility() {
            continue;
        }

        let fixture = fixture(format, crate::VideoCodec::H264, false, false, false);
        let mut metadata = fixture.metadata;
        metadata.render.playback_overlay = PlaybackOverlay::None;
        let result = render_bundle(
            &fixture.bundle,
            fixture.config,
            metadata,
            &CancellationToken::default(),
            |_, _, _, _| {},
        )
        .unwrap();
        if format == crate::ExportFormat::Avi {
            let avi = inspect_avi(&result.output_path);
            assert_eq!(avi.packets.len(), 2);
            assert_eq!(avi.stream_frames, 31);
            continue;
        }
        let (frames, width, height, duration) = decode_frames(&result.output_path);
        assert_eq!((width, height, frames.len()), (32, 24, 2), "{format:?}");
        let bytes = std::fs::read(&result.output_path).unwrap();
        let duration = match format {
            crate::ExportFormat::Gif => duration,
            crate::ExportFormat::Apng => bytes
                .windows(30)
                .filter(|chunk| &chunk[..4] == b"fcTL")
                .map(|chunk| {
                    f64::from(u16::from_be_bytes(chunk[24..26].try_into().unwrap()))
                        / f64::from(u16::from_be_bytes(chunk[26..28].try_into().unwrap()))
                })
                .sum(),
            crate::ExportFormat::Webp => bytes
                .windows(24)
                .filter(|chunk| &chunk[..4] == b"ANMF")
                .map(|chunk| {
                    f64::from(u32::from_le_bytes([chunk[20], chunk[21], chunk[22], 0])) / 1000.0
                })
                .sum(),
            _ => unreachable!(),
        };
        assert!((duration - 1.001).abs() < 0.011, "{format:?}: {duration}");
    }
}

#[test]
fn trimmed_deferred_render_restarts_progress_and_retains_the_source() {
    let fixture = fixture(
        crate::ExportFormat::Mp4,
        crate::VideoCodec::H264,
        false,
        true,
        false,
    );
    let source = std::fs::read(&fixture.bundle).unwrap();
    let mut config = fixture.config.clone();
    config.output_path = fixture.bundle.parent().unwrap().join("trim.mp4");
    let range = snow_recording_model::TrimRange::new(500_000, 900_000, 1_001_000).unwrap();
    let result = render_bundle_range(
        &fixture.bundle,
        config.clone(),
        fixture.metadata.clone(),
        range,
        &CancellationToken::default(),
        |_, _, _, _| {},
    )
    .unwrap();
    assert_eq!(result.duration_ms, 400);
    let (frames, width, height, _) = decode_frames(&config.output_path);
    assert_eq!(frames.len(), 12);
    let at = |frame: &[u8], x: u32, y: u32| frame[((y * width + x) * 4) as usize..][..3].to_vec();
    let first = at(&frames[0], width - 2, height - 1);
    let last = at(frames.last().unwrap(), width - 2, height - 1);
    assert!(
        first[0] < 150 && last[0] > 170 && last[1] < 100,
        "progress must restart and reach the trimmed endpoint: {first:?} {last:?}"
    );
    let cursor = at(&frames[0], 8, 4);
    assert!(
        cursor[1] > 150,
        "cursor state already active at trim start is reconstructed: {cursor:?}"
    );
    assert_eq!(std::fs::read(&fixture.bundle).unwrap(), source);
    config.output_path = fixture.bundle.parent().unwrap().join("one-frame.mp4");
    let range = snow_recording_model::TrimRange::new(33_333, 66_666, 1_001_000).unwrap();
    let result = render_bundle_range(
        &fixture.bundle,
        config,
        fixture.metadata.clone(),
        range,
        &CancellationToken::default(),
        |_, _, _, _| {},
    )
    .unwrap();
    assert_eq!(result.duration_ms, 33);
    assert_eq!(std::fs::read(&fixture.bundle).unwrap(), source);
}

#[test]
fn retimed_deferred_trim_preserves_source_effects_and_duration_in_every_format() {
    for format in [
        crate::ExportFormat::Mp4,
        crate::ExportFormat::Avi,
        crate::ExportFormat::Gif,
        crate::ExportFormat::Apng,
        crate::ExportFormat::Webp,
    ] {
        if format == crate::ExportFormat::Avi && !crate::has_avi_compatibility() {
            continue;
        }

        let fixture = fixture(format, crate::VideoCodec::H264, false, false, false);
        let original = std::fs::read(&fixture.bundle).unwrap();
        let range = snow_recording_model::TrimRange::new(300_000, 900_000, 1_001_000).unwrap();
        for percent in [25, 50, 75, 100, 125, 150, 175, 200, 300] {
            let speed = PlaybackSpeed::from_percent(percent).unwrap();
            let mut config = fixture.config.clone();
            config.output_path = fixture
                .bundle
                .parent()
                .unwrap()
                .join(format!("speed-{percent}.{}", format.file_extension()));
            let result = render_bundle_range_with_speed(
                &fixture.bundle,
                config.clone(),
                fixture.metadata.clone(),
                range,
                speed,
                &CancellationToken::default(),
                |_, _, _, _| {},
            )
            .unwrap();
            let expected_us = (600_000u64 * 100).div_ceil(u64::from(percent));
            assert_eq!(result.duration_ms, (expected_us / 1000).max(1));
            let (frames, width, height, decoded_duration) =
                inspect_retimed_output(&config.output_path, format);
            if format != crate::ExportFormat::Avi {
                assert!(!frames.is_empty(), "{format:?} {percent}%");
                let pixel = |frame: &[u8], x: u32, y: u32| -> [u8; 3] {
                    frame[((y * width + x) * 4) as usize..][..3]
                        .try_into()
                        .unwrap()
                };
                assert!(
                    pixel(&frames[0], 4, 4)[1] > 150,
                    "source cursor is reconstructed at trim start: {format:?} {percent}%"
                );
                assert!(
                    pixel(frames.last().unwrap(), 8, 4)[1] > 150,
                    "source cursor advances on its original clock: {format:?} {percent}%"
                );
                assert!(
                    pixel(&frames[0], width - 2, height - 1)[0] < 150
                        && pixel(frames.last().unwrap(), width - 2, height - 1)[0] > 170,
                    "progress restarts and reaches the exported endpoint: {format:?} {percent}%"
                );
            }
            let duration = animation_duration(&config.output_path, format, decoded_duration);
            let tolerance = if format == crate::ExportFormat::Avi {
                1.0 / f64::from(config.fps) + 0.001
            } else if format == crate::ExportFormat::Gif {
                0.011
            } else {
                0.002
            };
            assert!(
                (duration - expected_us as f64 / 1_000_000.0).abs() <= tolerance,
                "{format:?} {percent}%: {duration} seconds"
            );
            if format == crate::ExportFormat::Mp4 && percent != 100 {
                assert_fixed_video_grid(
                    &config.output_path,
                    config.fps,
                    speed
                        .output_frame_count(range.duration_us(), config.fps)
                        .unwrap(),
                );
            } else if format == crate::ExportFormat::Avi && percent != 100 {
                let avi = inspect_avi(&config.output_path);
                assert_eq!(
                    avi.packets.len() as u128,
                    u128::from(
                        speed
                            .output_frame_count(range.duration_us(), config.fps)
                            .unwrap()
                    ),
                    "AVI retains every fixed-rate slot"
                );
            }
            config.output_path = fixture.bundle.parent().unwrap().join(format!(
                "one-frame-speed-{percent}.{}",
                format.file_extension()
            ));
            let one_frame =
                snow_recording_model::TrimRange::new(300_000, 333_333, 1_001_000).unwrap();
            let result = render_bundle_range_with_speed(
                &fixture.bundle,
                config.clone(),
                fixture.metadata.clone(),
                one_frame,
                speed,
                &CancellationToken::default(),
                |_, _, _, _| {},
            )
            .unwrap();
            let expected_us = (33_333u64 * 100).div_ceil(u64::from(percent));
            assert_eq!(result.duration_ms, (expected_us / 1000).max(1));
            let (frames, _, _, duration) = inspect_retimed_output(&config.output_path, format);
            assert!(
                format == crate::ExportFormat::Avi || !frames.is_empty(),
                "one frame {format:?} {percent}%"
            );
            assert!(
                (animation_duration(&config.output_path, format, duration)
                    - expected_us as f64 / 1_000_000.0)
                    .abs()
                    <= tolerance,
                "one-frame duration: {format:?} {percent}%"
            );
        }
        assert_eq!(std::fs::read(&fixture.bundle).unwrap(), original);
    }
}

#[test]
fn retimed_deferred_audio_preserves_separate_routes_and_exact_endpoint() {
    let fixture = fixture(
        crate::ExportFormat::Mp4,
        crate::VideoCodec::H264,
        false,
        true,
        false,
    );
    let range = snow_recording_model::TrimRange::new(100_000, 900_000, 1_001_000).unwrap();
    for percent in [25, 125, 300] {
        let speed = PlaybackSpeed::from_percent(percent).unwrap();
        let mut config = fixture.config.clone();
        config.output_path = fixture
            .bundle
            .parent()
            .unwrap()
            .join(format!("audio-speed-{percent}.mp4"));
        render_bundle_range_with_speed(
            &fixture.bundle,
            config.clone(),
            fixture.metadata.clone(),
            range,
            speed,
            &CancellationToken::default(),
            |_, _, _, _| {},
        )
        .unwrap();
        let input = ffmpeg::format::input(&config.output_path).unwrap();
        let audio: Vec<_> = input
            .streams()
            .filter(|stream| stream.parameters().medium() == ffmpeg::media::Type::Audio)
            .collect();
        assert_eq!(audio.len(), 2);
        let expected_us = (800_000u64 * 100).div_ceil(u64::from(percent));
        for (stream, title) in audio.iter().zip(["Speaker audio", "Microphone"]) {
            assert_eq!(stream.metadata().get("handler_name"), Some(title));
            let duration = stream.duration() as f64 * stream.time_base().numerator() as f64
                / stream.time_base().denominator() as f64;
            assert!(
                (duration - expected_us as f64 / 1_000_000.0).abs() < 0.002,
                "{percent}% audio extent: {duration}"
            );
        }
        drop(input);
        let output =
            crate::clip::ClipSource::open(&config.output_path, Some(config.clone())).unwrap();
        let mut audio = crate::clip::AudioReader::open(&output).unwrap();
        let tracks = audio
            .read(
                expected_us * 48_000 / 2_000_000,
                960,
                &CancellationToken::default(),
            )
            .unwrap();
        for (samples, expected) in tracks.iter().zip([1000.0, 2000.0]) {
            let mean =
                samples.iter().map(|sample| f64::from(*sample)).sum::<f64>() / samples.len() as f64;
            assert!(
                (mean - expected).abs() < 100.0,
                "{percent}% routed PCM: {mean}"
            );
        }
    }
}

#[test]
fn retimed_rounded_endpoint_never_reads_the_first_excluded_source_frame() {
    let fixture = fixture_with_source_timing(
        crate::ExportFormat::Mp4,
        crate::VideoCodec::H264,
        (32, 24),
        false,
        false,
        (60, (0, 5)),
        false,
    );
    let mut metadata = fixture.metadata.clone();
    metadata.render.playback_overlay = PlaybackOverlay::None;
    metadata.render.effects.show_cursor = false;
    let range = snow_recording_model::TrimRange::new(0, 83_333, 1_001_000).unwrap();
    let mut config = fixture.config.clone();
    config.output_path = fixture
        .bundle
        .parent()
        .unwrap()
        .join("rounded-endpoint.mp4");
    render_bundle_range_with_speed(
        &fixture.bundle,
        config.clone(),
        metadata,
        range,
        PlaybackSpeed::from_percent(125).unwrap(),
        &CancellationToken::default(),
        |_, _, _, _| {},
    )
    .unwrap();
    let (frames, _, _, _) = decode_frames(&config.output_path);
    assert_eq!(
        frames.len(),
        4,
        "retiming retains every fixed-rate presentation slot"
    );
    assert_fixed_video_grid(&config.output_path, config.fps, 4);
    let first = &frames[0];
    for frame in &frames {
        assert!(
            frame
                .iter()
                .zip(first)
                .all(|(actual, expected)| actual.abs_diff(*expected) <= 3),
            "rounded output duration cannot expose excluded source frame5"
        );
    }
}

#[test]
fn retimed_exact_frame_count_does_not_round_a_third_of_a_second_twice() {
    let fixture = fixture_with_source_timing(
        crate::ExportFormat::Mp4,
        crate::VideoCodec::H264,
        (32, 24),
        false,
        false,
        (60, (0, 30)),
        false,
    );
    let range = snow_recording_model::TrimRange::new(0, 1_000_000, 1_001_000).unwrap();
    let mut config = fixture.config.clone();
    config.output_path = fixture.bundle.parent().unwrap().join("exact-third.mp4");
    render_bundle_range_with_speed(
        &fixture.bundle,
        config.clone(),
        fixture.metadata.clone(),
        range,
        PlaybackSpeed::from_percent(300).unwrap(),
        &CancellationToken::default(),
        |_, _, _, _| {},
    )
    .unwrap();
    assert_fixed_video_grid(&config.output_path, 60, 20);
    let (frames, _, _, duration) = decode_frames(&config.output_path);
    assert_eq!(frames.len(), 20);
    assert!((duration - 1.0 / 3.0).abs() < 0.002);
}

#[test]
fn retimed_fractional_deferred_frame_ranges_keep_exact_slot_counts() {
    for fps in [60, 144] {
        for format in [crate::ExportFormat::Mp4, crate::ExportFormat::Avi] {
            if format == crate::ExportFormat::Avi && !crate::has_avi_compatibility() {
                continue;
            }

            let fixture = fixture_with_source_timing(
                format,
                crate::VideoCodec::H264,
                (32, 24),
                false,
                false,
                (fps, (0, 5)),
                false,
            );
            for (first, end) in [(1u64, 2u64), (2, 4), (1, 6)] {
                let range = snow_recording_model::TrimRange::new(
                    first * 1_000_000 / u64::from(fps),
                    end * 1_000_000 / u64::from(fps),
                    1_001_000,
                )
                .unwrap();
                for percent in [25, 50, 75, 100, 125, 150, 175, 200, 300] {
                    let mut config = fixture.config.clone();
                    config.output_path = fixture.bundle.parent().unwrap().join(format!(
                        "exact-{first}-{end}-{percent}.{}",
                        format.file_extension()
                    ));
                    render_bundle_range_with_speed(
                        &fixture.bundle,
                        config.clone(),
                        fixture.metadata.clone(),
                        range,
                        PlaybackSpeed::from_percent(percent).unwrap(),
                        &CancellationToken::default(),
                        |_, _, _, _| {},
                    )
                    .unwrap();
                    let expected = ((end - first) * 100).div_ceil(u64::from(percent));
                    if format == crate::ExportFormat::Avi {
                        let avi = inspect_avi(&config.output_path);
                        assert_eq!(avi.packets.len() as u64, expected);
                        assert_eq!(u64::from(avi.stream_frames), expected);
                        assert_eq!(avi.rate, fps);
                        assert_eq!(avi.scale, 1);
                    } else {
                        assert_fixed_video_grid(&config.output_path, fps, expected);
                        let input = ffmpeg::format::input(&config.output_path).unwrap();
                        let video = input.streams().best(ffmpeg::media::Type::Video).unwrap();
                        let duration = video.duration().rescale(video.time_base(), (1, 1_000_000));
                        let expected_us = ((end - first) * 100 * 1_000_000)
                            .div_ceil(u64::from(fps) * u64::from(percent));
                        // B-frame edit lists quantize the published duration on
                        // the MP4 movie clock. Slot counts and PTS remain exact above.
                        assert!(
                            duration.abs_diff(expected_us as i64) <= 2_000,
                            "{fps} FPS [{first}, {end}) at {percent}%: {duration} vs {expected_us} us"
                        );
                    }
                }
            }
        }
    }
}

#[test]
fn retimed_partial_deferred_tail_keeps_its_source_duration() {
    for fps in [60, 144] {
        for format in [crate::ExportFormat::Mp4, crate::ExportFormat::Avi] {
            if format == crate::ExportFormat::Avi && !crate::has_avi_compatibility() {
                continue;
            }

            let fixture = fixture_with_source_timing(
                format,
                crate::VideoCodec::H264,
                (32, 24),
                false,
                false,
                (fps, (0, 5)),
                false,
            );
            let source = crate::clip::ClipSource::open(&fixture.bundle, None).unwrap();
            let range = source.range(u64::from(fps), u64::from(fps) + 1).unwrap();
            assert_eq!(range.duration_us(), 1000);
            for percent in [25, 50, 75, 100, 125, 150, 175, 200, 300] {
                let speed = PlaybackSpeed::from_percent(percent).unwrap();
                let timing = source.output_timing(range, speed).unwrap();
                assert_eq!(timing.frame_count().unwrap(), 1);
                assert_eq!(
                    timing.duration_us().unwrap(),
                    (100_000u64).div_ceil(u64::from(percent))
                );
                let mut config = fixture.config.clone();
                config.output_path = fixture
                    .bundle
                    .parent()
                    .unwrap()
                    .join(format!("partial-{percent}.{}", format.file_extension()));
                render_bundle_range_with_speed(
                    &fixture.bundle,
                    config.clone(),
                    fixture.metadata.clone(),
                    range,
                    speed,
                    &CancellationToken::default(),
                    |_, _, _, _| {},
                )
                .unwrap();
                if format == crate::ExportFormat::Avi {
                    let avi = inspect_avi(&config.output_path);
                    assert_eq!(avi.packets.len(), 1);
                    assert_eq!(avi.stream_frames, 1);
                } else {
                    assert_fixed_video_grid(&config.output_path, fps, 1);
                    let input = ffmpeg::format::input(&config.output_path).unwrap();
                    let video = input.streams().best(ffmpeg::media::Type::Video).unwrap();
                    let duration = video.duration().rescale(video.time_base(), (1, 1_000_000));
                    assert!(duration.abs_diff(timing.duration_us().unwrap() as i64) <= 100);
                }
            }
        }
    }
}

fn animation_duration(path: &Path, format: crate::ExportFormat, decoded: f64) -> f64 {
    match format {
        crate::ExportFormat::Apng => std::fs::read(path)
            .unwrap()
            .windows(30)
            .filter(|chunk| &chunk[..4] == b"fcTL")
            .map(|chunk| {
                f64::from(u16::from_be_bytes(chunk[24..26].try_into().unwrap()))
                    / f64::from(u16::from_be_bytes(chunk[26..28].try_into().unwrap()))
            })
            .sum(),
        crate::ExportFormat::Webp => std::fs::read(path)
            .unwrap()
            .windows(24)
            .filter(|chunk| &chunk[..4] == b"ANMF")
            .map(|chunk| {
                f64::from(u32::from_le_bytes([chunk[20], chunk[21], chunk[22], 0])) / 1000.0
            })
            .sum(),
        _ => decoded,
    }
}

fn inspect_retimed_output(
    path: &Path,
    format: crate::ExportFormat,
) -> (Vec<Vec<u8>>, u32, u32, f64) {
    if format == crate::ExportFormat::Avi {
        // The minimal FFmpeg runtime exports AVI without its demuxer/MPEG4 decoder.
        // Inspect the native RIFF timeline and payload just as the AVI replay tests do.
        let avi = inspect_avi(path);
        assert_eq!(avi.codec, Some(ffmpeg::codec::Id::MPEG4));
        assert!(!avi.packets.is_empty());
        assert!(
            avi.packets
                .iter()
                .all(|packet| packet.windows(4).any(|bytes| bytes == [0, 0, 1, 0xB6]))
        );
        let duration = f64::from(avi.stream_frames) * f64::from(avi.scale) / f64::from(avi.rate);
        (Vec::new(), avi.width, avi.height, duration)
    } else {
        decode_frames(path)
    }
}

fn assert_fixed_video_grid(path: &Path, fps: u32, expected_frames: u64) {
    let mut input = ffmpeg::format::input(path).unwrap();
    let stream = input.streams().best(ffmpeg::media::Type::Video).unwrap();
    let id = stream.index();
    let time_base = stream.time_base();
    let mut timestamps: Vec<_> = input
        .packets()
        .filter(|(stream, _)| stream.index() == id)
        .map(|(_, packet)| packet.pts().unwrap().rescale(time_base, (1, 1_000_000)))
        .collect();
    timestamps.sort_unstable();
    assert_eq!(
        timestamps.len(),
        expected_frames as usize,
        "all fixed-rate output slots are encoded"
    );
    for (index, timestamp) in timestamps.into_iter().enumerate() {
        let expected = (index as u128 * 1_000_000 / u128::from(fps)) as i64;
        assert!(
            timestamp.abs_diff(expected) <= 1,
            "slot {index} must remain on the {fps} FPS grid: {timestamp} vs {expected}"
        );
    }
}

#[test]
fn clip_preview_reads_normalized_source_times_without_rendering_effects() {
    let fixture = fixture_with_source(
        crate::ExportFormat::Mp4,
        crate::VideoCodec::H264,
        (32, 24),
        true,
        false,
        (5, 20),
        false,
    );
    let source = crate::clip::ClipSource::open(&fixture.bundle, None).unwrap();
    let mut video = crate::clip::VideoReader::open(source.clone(), false).unwrap();
    let mut pixels = Vec::new();
    video
        .read(source.boundary(19), &CancellationToken::default())
        .unwrap();
    video.copy_rgba(&mut pixels).unwrap();
    let before = pixels[0];
    video
        .read(source.boundary(20), &CancellationToken::default())
        .unwrap();
    video.copy_rgba(&mut pixels).unwrap();
    assert!(
        pixels[0] > before + 10,
        "normalized container PTS must preserve source admission times"
    );
    assert!(
        pixels[(4 * 32 + 8) * 4 + 1] < 150,
        "preview must not composite cursor effects"
    );
    assert!(
        !fixture.config.output_path.exists(),
        "preview must not render the deferred output"
    );
    let mut audio = crate::clip::AudioReader::open(&source).unwrap();
    let tracks = audio
        .read(24_000, 480, &CancellationToken::default())
        .unwrap();
    assert_eq!(tracks.len(), 2);
    assert!(tracks[0].iter().all(|sample| *sample == 1000));
    assert!(tracks[1].iter().all(|sample| *sample == 2000));
}

#[test]
fn hdr_trim_applies_output_color_policy_and_retains_original_bundle() {
    let fixture = fixture(
        crate::ExportFormat::Mp4,
        crate::VideoCodec::H265,
        false,
        false,
        true,
    );
    let source = crate::clip::ClipSource::open(&fixture.bundle, None).unwrap();
    let path = fixture.bundle.parent().unwrap().join("hdr-trim.mp4");
    source
        .export(
            source.range(15, 30).unwrap(),
            &path,
            &CancellationToken::default(),
            |_| {},
        )
        .unwrap();
    let trimmed = crate::clip::ClipSource::open(&path, Some(fixture.config.clone())).unwrap();
    assert_eq!(trimmed.hdr, cfg!(feature = "hdr-encoding"));
    let path2 = fixture.bundle.parent().unwrap().join("hdr-again.mp4");
    trimmed
        .export(
            trimmed.range(3, 10).unwrap(),
            &path2,
            &CancellationToken::default(),
            |_| {},
        )
        .unwrap();
    assert_eq!(
        crate::clip::ClipSource::open(&path2, Some(fixture.config.clone()))
            .unwrap()
            .hdr,
        cfg!(feature = "hdr-encoding")
    );
    assert!(fixture.bundle.exists() && path.exists());
    for percent in [25, 300] {
        let path = fixture
            .bundle
            .parent()
            .unwrap()
            .join(format!("hdr-speed-{percent}.mp4"));
        let range = source.range(15, 30).unwrap();
        source
            .export_with_speed(
                range,
                PlaybackSpeed::from_percent(percent).unwrap(),
                &path,
                &CancellationToken::default(),
                |_| {},
            )
            .unwrap();
        let output = crate::clip::ClipSource::open(&path, Some(fixture.config.clone())).unwrap();
        assert_eq!(output.hdr, cfg!(feature = "hdr-encoding"));
        assert!(
            output
                .duration_us
                .abs_diff((range.duration_us() * 100).div_ceil(u64::from(percent)))
                < 2000
        );
        assert_fixed_video_grid(
            &path,
            fixture.config.fps,
            PlaybackSpeed::from_percent(percent)
                .unwrap()
                .output_frame_count(range.duration_us(), fixture.config.fps)
                .unwrap(),
        );
    }
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
    fixture_with_source_timing(
        format,
        codec,
        logical,
        audio,
        hdr,
        (30, slots),
        portrait_change,
    )
}

fn fixture_with_source_timing(
    format: crate::ExportFormat,
    codec: crate::VideoCodec,
    logical: (u32, u32),
    audio: bool,
    hdr: bool,
    timing: (u32, (u64, u64)),
    portrait_change: bool,
) -> Fixture {
    let (fps, slots) = timing;
    let directory = tempfile::tempdir().unwrap();
    let root = directory.path();
    let coded = (logical.0.div_ceil(2) * 2, logical.1.div_ceil(2) * 2);
    let duration = 1001;
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
    let first_ms = slots.0 * 1000 / u64::from(fps);
    let second_ms = slots.1 * 1000 / u64::from(fps);
    if hdr {
        assert_eq!((coded, fps, slots), ((32, 24), 30, (0, 15)));
        std::fs::write(
            &source_path,
            include_bytes!("../../tests/fixtures/hdr10-two-frame.mp4"),
        )
        .unwrap();
    } else {
        let mut encoder = StreamingEncoder::builder(source)
            .recording_source()
            .create()
            .unwrap();
        for (pts, color) in [(slots.0, [20u8, 30, 40, 255]), (slots.1, [40, 50, 60, 255])] {
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
        encoder.finish_at_duration_ms(duration - first_ms).unwrap();
    }
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
        // Use an independent decoder to validate the generated animation.
        let mut options = gif::DecodeOptions::new();
        options.set_color_output(gif::ColorOutput::RGBA);
        let mut reader = options.read_info(File::open(path).unwrap()).unwrap();
        let (width, height) = (u32::from(reader.width()), u32::from(reader.height()));
        let mut frames = Vec::new();
        let mut duration = 0.0;
        let mut canvas = vec![0; (width * height * 4) as usize];
        while let Some(frame) = reader.read_next_frame().unwrap() {
            duration += f64::from(frame.delay) / 100.0;
            let previous = (frame.dispose == gif::DisposalMethod::Previous).then(|| canvas.clone());
            for y in 0..usize::from(frame.height) {
                for x in 0..usize::from(frame.width) {
                    let from = (y * usize::from(frame.width) + x) * 4;
                    let to = ((usize::from(frame.top) + y) * width as usize
                        + usize::from(frame.left)
                        + x)
                        * 4;
                    if frame.buffer[from + 3] != 0 {
                        canvas[to..to + 4].copy_from_slice(&frame.buffer[from..from + 4]);
                    }
                }
            }
            frames.push(canvas.clone());
            if let Some(previous) = previous {
                canvas = previous;
            } else if frame.dispose == gif::DisposalMethod::Background {
                for y in 0..usize::from(frame.height) {
                    let begin = ((usize::from(frame.top) + y) * width as usize
                        + usize::from(frame.left))
                        * 4;
                    canvas[begin..begin + usize::from(frame.width) * 4].fill(0);
                }
            }
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
            decoder
                .send_packet(&packet)
                .unwrap_or_else(|error| panic!("decode {}: {error}", path.display()));
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
    assert_eq!(deferred_conversion_threads(&config, expected, false), 0);
    assert_eq!(
        deferred_software_threads(&config, 4),
        cfg!(windows).then_some(4)
    );
    config.prefer_hardware_h264 = true;
    assert_eq!(deferred_software_threads(&config, 12), expected);
    config.fps = 60;
    let threads = deferred_software_threads(&config, 12);
    assert_eq!(
        deferred_conversion_threads(&config, threads, false),
        if cfg!(windows) { 4 } else { 0 }
    );
    assert_eq!(deferred_conversion_threads(&config, threads, true), 0);
    assert_eq!(deferred_conversion_threads(&config, Some(2), false), 0);
    assert_eq!(
        deferred_software_threads(&config, 12),
        cfg!(windows).then_some(4)
    );
    assert_eq!(
        deferred_software_threads(&config, 2),
        cfg!(windows).then_some(2)
    );
    assert_eq!(
        deferred_software_threads(&config, 0),
        cfg!(windows).then_some(1)
    );
    for mutation in [0, 1, 2, 3, 4, 5, 6] {
        let mut other = config.clone();
        match mutation {
            0 => other.encode_threads = 3,
            1 => other.fps = 50,
            2 => other.width = 1920,
            3 => other.codec = crate::VideoCodec::H265,
            4 => other.format = crate::ExportFormat::Gif,
            5 => other.software_h264_priority = crate::SoftwareH264Priority::OpenH264First,
            _ => other.video.speed = VideoEncodingSpeed::Medium,
        }
        assert_eq!(deferred_software_threads(&other, 12), None);
        assert_eq!(deferred_conversion_threads(&other, None, false), 0);
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
            encoder.set_frame_reuse(crate::streaming::VideoFrameReuse::ReuseConversion);
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
            encoder.set_frame_reuse(
                if guard && reusable_effect_frame(&mut effects, frame.timestamp_ms) {
                    crate::streaming::VideoFrameReuse::ReuseConversion
                } else {
                    crate::streaming::VideoFrameReuse::Encode
                },
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
fn hdr_history_swaps_owned_pixels_and_recaches_after_animation() {
    let mut pixels = ffmpeg::frame::Video::new(ffmpeg::format::Pixel::RGB48LE, 16, 16);
    let mut previous = Some(ffmpeg::frame::Video::new(
        ffmpeg::format::Pixel::RGB48LE,
        16,
        16,
    ));
    let mut valid = false;
    pixels.data_mut(0).fill(1);
    let first_storage = pixels.data(0).as_ptr();
    cache_rgb48(&mut pixels, &mut previous, &mut valid, true);
    assert!(valid);
    assert_eq!(previous.as_ref().unwrap().data(0).as_ptr(), first_storage);
    assert_eq!(previous.as_ref().unwrap().data(0)[0], 1);
    pixels.data_mut(0).fill(2);
    cache_rgb48(&mut pixels, &mut previous, &mut valid, false);
    assert!(!valid);
    assert_eq!(previous.as_ref().unwrap().data(0)[0], 1);
    let next_storage = pixels.data(0).as_ptr();
    cache_rgb48(&mut pixels, &mut previous, &mut valid, true);
    assert!(valid);
    assert_eq!(previous.as_ref().unwrap().data(0).as_ptr(), next_storage);
    assert_eq!(previous.as_ref().unwrap().data(0)[0], 2);
    pixels.data_mut(0).fill(2);
    assert!(equal_rgb48(&pixels, previous.as_ref().unwrap()));
    pixels.data_mut(0)[6] = 3;
    assert!(!equal_rgb48(&pixels, previous.as_ref().unwrap()));
    assert_eq!(previous.as_ref().unwrap().data(0)[6], 2);
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
    if !crate::has_avi_compatibility() {
        return;
    }
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
fn deferred_pcm_reads_preserve_seeks_routes_and_zero_padding_across_buffer_boundaries() {
    let fixture = fixture(
        crate::ExportFormat::Mp4,
        crate::VideoCodec::H264,
        false,
        true,
        false,
    );
    let footer = read_recording_bundle_footer(&fixture.bundle).unwrap();
    let mut file = std::fs::OpenOptions::new()
        .write(true)
        .open(&fixture.bundle)
        .unwrap();
    let sample = |track: usize, index: usize| (index % 20000) as i16 - 10000 + track as i16 * 3000;
    for (track, manifest) in footer.manifest.audio_tracks.iter().enumerate() {
        let asset = footer
            .asset(BundleAssetKind::AudioTrack, Some(&manifest.asset_id))
            .unwrap();
        file.seek(SeekFrom::Start(asset.offset)).unwrap();
        let bytes: Vec<_> = (0..asset.len as usize / 2)
            .flat_map(|i| sample(track, i).to_le_bytes())
            .collect();
        file.write_all(&bytes).unwrap();
    }
    drop(file);
    let token = CancellationToken::default();
    for mixed in [false, true] {
        let mut source = crate::clip::ClipSource::open(&fixture.bundle, None).unwrap();
        if mixed {
            source.config.audio.truncate(1);
            source.config.audio[0].track_id = "mixed".into();
        }
        let mut reader = crate::clip::AudioReader::open(&source).unwrap();
        for (start, count) in [
            (0, 480),
            (480, 480),
            (960, 20000),
            (47000, 2000),
            (100, 9),
            (16000, 8000),
            (100000, 480),
            (0, 0),
        ] {
            let tracks = reader.read(start, count, &token).unwrap();
            for (track, samples) in tracks.iter().enumerate() {
                let expected: Vec<_> = (0..count * 2)
                    .map(|i| {
                        let index = start as usize * 2 + i;
                        if index >= 1001 * 48 * 2 {
                            0
                        } else if mixed {
                            sample(0, index).saturating_add(sample(1, index))
                        } else {
                            sample(track, index)
                        }
                    })
                    .collect();
                assert_eq!(
                    *samples, expected,
                    "mixed={mixed}, start={start}, count={count}"
                );
            }
        }
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
fn hdr_tone_map_cache_tracks_source_images_and_keeps_composition_out_of_the_cache() {
    let fixture = fixture(
        crate::ExportFormat::Mp4,
        crate::VideoCodec::H264,
        false,
        false,
        true,
    );
    let token = CancellationToken::default();
    let mut video = SequentialVideoSource::open(
        &fixture.bundle,
        fixture.metadata.timeline,
        true,
        (32, 24),
        (32, 24),
        &token,
    )
    .unwrap();
    assert!(video.tone_mapped.is_empty());
    let mut previous = Vec::new();
    for pts in [0, 1, 14, 15, 16, 30] {
        let changed = video.advance(pts, &token).unwrap();
        if changed {
            assert!(!video.tone_mapped_valid);
        }
        let mut reference = vec![0; 32 * 24 * 4];
        let stride = video.converted.stride(0);
        for (y, row) in reference.chunks_exact_mut(32 * 4).enumerate() {
            snow_media::color::tone_map_rgb48_row(
                &video.converted.data(0)[y * stride..y * stride + 32 * 6],
                row,
            )
            .unwrap();
        }
        if changed && !previous.is_empty() {
            assert_ne!(reference, previous);
        }
        for _ in 0..2 {
            let mut output = vec![255; reference.len()];
            video.copy_rgba(&mut output).unwrap();
            assert_eq!(output, reference);
            // Composition is free to overwrite its destination.
            output.fill(0);
        }
        assert!(video.tone_mapped_valid);
        previous = reference;
    }
}

#[test]
fn hdr_clean_source_replays_with_hdr_color_and_overlay_or_tone_maps_to_animation() {
    for (format, codec) in [
        (crate::ExportFormat::Mp4, crate::VideoCodec::H265),
        (crate::ExportFormat::Avi, crate::VideoCodec::H265),
        (crate::ExportFormat::Gif, crate::VideoCodec::H264),
    ] {
        if format == crate::ExportFormat::Avi && !crate::has_avi_compatibility() {
            continue;
        }

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
