#![cfg(target_os = "macos")]
use snow_macos::compositor::Compositor;
use snow_media::{PixelFormat, geometry::PixelSize};
use snow_recording_export::{
    ExportExecutionMode, ExportFormat, SoftwareH264Priority, StreamingEncoder,
    StreamingEncoderConfig, VideoCodec,
};

fn round_trip(format: PixelFormat, codec: VideoCodec) {
    let directory = tempfile::tempdir().unwrap();
    let path = directory.path().join("native.mp4");
    let size = PixelSize::new(128, 72).unwrap();
    let mut compositor = Compositor::new(size, format, 4).unwrap();
    let mut encoder = StreamingEncoder::builder(StreamingEncoderConfig {
        loop_animated_images: false,
        output_path: path.clone(),
        format: ExportFormat::Mp4,
        width: size.width,
        height: size.height,
        fps: 30,
        codec,
        prefer_hardware_h264: true,
        execution_mode: ExportExecutionMode::HardwareOnly,
        software_h264_priority: SoftwareH264Priority::X264First,
        video: Default::default(),
        encode_threads: 1,
        audio: None,
    })
    .native_input(format)
    .create()
    .unwrap();
    for pts in 0..8 {
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
        let frame = loop {
            match compositor.compose(&[], true) {
                Ok(frame) => break frame,
                Err(snow_macos::MacError::Timeout) if std::time::Instant::now() < deadline => {
                    encoder.poll_native_packets().unwrap();
                    std::thread::sleep(std::time::Duration::from_millis(1));
                }
                Err(error) => panic!("{error}"),
            }
        };
        assert_eq!(frame.format(), format);
        encoder.push_native_frame_at_pts(pts, frame).unwrap();
    }
    let report = encoder.finish_at_pts(8).unwrap();
    assert!(report.used_hardware_video_encoder);
    assert_eq!(report.encoded_frames, 8);
    let mut input = ffmpeg_next::format::input(&path).unwrap();
    let stream = input
        .streams()
        .best(ffmpeg_next::media::Type::Video)
        .unwrap();
    let index = stream.index();
    let context =
        ffmpeg_next::codec::context::Context::from_parameters(stream.parameters()).unwrap();
    let mut decoder = context.decoder().video().unwrap();
    if format == PixelFormat::P010 {
        unsafe {
            assert_eq!(
                (*decoder.as_ptr()).color_trc,
                ffmpeg_next::ffi::AVColorTransferCharacteristic::AVCOL_TRC_SMPTE2084
            );
            assert_eq!(
                (*decoder.as_ptr()).color_primaries,
                ffmpeg_next::ffi::AVColorPrimaries::AVCOL_PRI_BT2020
            );
            assert_eq!((*decoder.as_ptr()).profile, 2);
        }
    }
    let mut count = 0;
    let mut decoded = ffmpeg_next::frame::Video::empty();
    for (stream, packet) in input.packets() {
        if stream.index() == index {
            decoder.send_packet(&packet).unwrap();
            while decoder.receive_frame(&mut decoded).is_ok() {
                assert_eq!((decoded.width(), decoded.height()), (128, 72));
                count += 1;
            }
        }
    }
    decoder.send_eof().unwrap();
    while decoder.receive_frame(&mut decoded).is_ok() {
        count += 1;
    }
    assert_eq!(count, 8);
}
#[test]
#[ignore = "requires a native Metal device and VideoToolbox H.264 hardware encoder"]
fn native_sdr_h264_round_trip() {
    round_trip(PixelFormat::Bgra8, VideoCodec::H264);
}
#[test]
#[ignore = "requires Metal P010 output and VideoToolbox HEVC Main10 hardware encoder"]
fn native_hdr_hevc_round_trip() {
    round_trip(PixelFormat::P010, VideoCodec::H265);
}

#[test]
#[ignore = "requires a native Metal device"]
fn native_pool_is_bounded_and_leases_survive_pool() {
    let mut compositor =
        Compositor::new(PixelSize::new(16, 16).unwrap(), PixelFormat::Bgra8, 2).unwrap();
    let first = compositor.compose(&[], true).unwrap();
    let second = compositor.compose(&[], false).unwrap();
    assert!(matches!(
        compositor.compose(&[], true),
        Err(snow_macos::MacError::Timeout)
    ));
    drop(second);
    assert!(compositor.compose(&[], true).is_ok());
    drop(compositor);
    let cpu = first.to_cpu().unwrap();
    assert!(cpu.bytes.chunks_exact(4).all(|p| p == [0, 0, 0, 255]));
}

#[test]
#[ignore = "requires a native Metal device; no screen permission needed"]
fn metal_effect_tiles_preserve_top_left_position_and_premultiplied_alpha() {
    use snow_macos::compositor::RgbaOverlay;
    let mut compositor =
        Compositor::new(PixelSize::new(16, 16).unwrap(), PixelFormat::Bgra8, 4).unwrap();
    let rgba = [128, 0, 0, 128, 0, 255, 0, 255, 0, 0, 255, 255, 0, 0, 0, 0];
    let image = compositor
        .compose_with_overlays(
            &[],
            &[RgbaOverlay {
                x: 3,
                y: 5,
                width: 2,
                height: 2,
                stride: 8,
                bytes: &rgba,
            }],
            false,
        )
        .unwrap();
    let cpu = image.to_cpu().unwrap();
    let pixel = |x: usize, y: usize| &cpu.bytes[(y * 16 + x) * 4..(y * 16 + x + 1) * 4];
    assert_eq!(pixel(0, 0), &[0, 0, 0, 0]);
    assert!(pixel(3, 5)[2].abs_diff(128) <= 2, "{:?}", pixel(3, 5));
    assert!(pixel(3, 5)[3].abs_diff(128) <= 2);
    assert_eq!(pixel(4, 5), &[0, 255, 0, 255]);
    assert_eq!(pixel(3, 6), &[255, 0, 0, 255]);
    assert_eq!(pixel(4, 6), &[0, 0, 0, 0]);
}

#[test]
#[ignore = "requires native Metal and VideoToolbox HEVC Main10 hardware"]
fn hdr_editable_transcode_preserves_hdr_and_tone_maps_sdr() {
    hdr_editable_round_trip(true, false);
}

#[test]
fn software_hdr_editable_cursor_round_trip() {
    hdr_editable_round_trip(false, true);
}

fn hdr_editable_round_trip(native: bool, overlays: bool) {
    use snow_media::{ColorDescription, CursorMode};
    use snow_recording_model::*;
    let directory = tempfile::tempdir().unwrap();
    let path = directory.path().join("editable.mp4");
    let mut compositor = native
        .then(|| Compositor::new(PixelSize::new(128, 72).unwrap(), PixelFormat::P010, 4).unwrap());
    let builder = StreamingEncoder::builder(StreamingEncoderConfig {
        loop_animated_images: false,
        output_path: path.clone(),
        format: ExportFormat::Mp4,
        width: 128,
        height: 72,
        fps: 30,
        codec: VideoCodec::H265,
        prefer_hardware_h264: native,
        execution_mode: if native {
            ExportExecutionMode::HardwareOnly
        } else {
            ExportExecutionMode::SoftwareOnly
        },
        software_h264_priority: SoftwareH264Priority::X264First,
        video: Default::default(),
        encode_threads: 1,
        audio: None,
    })
    .native_input(PixelFormat::P010);
    let mut encoder = if native {
        builder
    } else {
        builder.software_only()
    }
    .create()
    .unwrap();
    let mut bytes = (64u16 << 6).to_le_bytes().repeat(128 * 72);
    bytes.extend((512u16 << 6).to_le_bytes().repeat(128 * 36));
    let black = snow_media::CpuFrame {
        size: PixelSize::new(128, 72).unwrap(),
        format: PixelFormat::P010,
        color: ColorDescription::HDR10,
        planes: vec![
            snow_media::PlaneLayout {
                offset: 0,
                width: 128,
                height: 72,
                stride: 256,
                row_bytes: 256,
            },
            snow_media::PlaneLayout {
                offset: 256 * 72,
                width: 64,
                height: 36,
                stride: 256,
                row_bytes: 256,
            },
        ],
        bytes: bytes.into(),
    };
    for pts in 0..3 {
        if let Some(compositor) = compositor.as_mut() {
            encoder
                .push_native_frame_at_pts(pts, compositor.compose(&[], true).unwrap())
                .unwrap();
        } else {
            encoder.push_cpu_hdr_frame_at_pts(pts, &black).unwrap();
        }
    }
    encoder.finish_at_pts(3).unwrap();
    let index = directory.path().join("index");
    let mut bytes = b"SVIDX\0\0".to_vec();
    for (i, start, duration) in [(0u64, 0u64, 33u32), (1, 33, 33), (2, 66, 34)] {
        bytes.extend(i.to_le_bytes());
        bytes.extend(start.to_le_bytes());
        bytes.extend(duration.to_le_bytes());
    }
    std::fs::write(&index, bytes).unwrap();
    let mouse = directory.path().join("mouse");
    let mut mouse_store = MouseStore::new();
    if overlays {
        mouse_store.cursor_shapes.push(CursorShapeRecord {
            shape_id: 1,
            hotspot_x: 0,
            hotspot_y: 0,
            width: 32,
            height: 32,
            mode: CursorShapeCompositionMode::AlphaBlend,
            shape_rgba: vec![255; 32 * 32 * 4],
        });
        for (timestamp_ms, x) in [(0, 0), (33, 64), (66, 0)] {
            mouse_store.cursor_frames.push(CursorFrameRecord {
                timestamp_ms,
                x,
                y: 0,
                visible: true,
                shape_id: Some(1),
            });
        }
    }
    write_mouse_records(&mouse, &mouse_store).unwrap();
    let manifest = SessionManifest {
        media: media::RecordedMedia::new(
            ColorDescription::HDR10,
            if overlays {
                CursorMode::Separate
            } else {
                CursorMode::Embedded
            },
            vec![],
        ),
        video_codec: VideoCodec::H265,
        session_id: "hdr-native".into(),
        output_dir: directory.path().into(),
        keep_temp_files: false,
        fps: 30,
        intermediate_profile: IntermediateRecordingProfile::EditFast,
        recording_video: Default::default(),
        width: 128,
        height: 72,
        capture_origin_x: 0,
        capture_origin_y: 0,
        audio_tracks: vec![],
        pause_intervals: vec![],
    };
    write_recording_bundle(
        &path,
        &manifest,
        &[
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
        ],
    )
    .unwrap();
    let artifact = RecordingArtifact {
        session_id: "hdr-native".into(),
        output_dir: directory.path().into(),
        bundle_path: path.clone(),
        audio_tracks: vec![],
        local_paths: LocalRecordingPaths {
            temp_dir: directory.path().into(),
            video_intermediate_path: path,
            video_index_path: index,
            mouse_path: mouse,
        },
    };
    for (format, codec, extension) in [
        (ExportFormat::Mp4, VideoCodec::H265, "hdr.mp4"),
        (ExportFormat::Mp4, VideoCodec::H265, "software-hdr.mp4"),
        (ExportFormat::Mp4, VideoCodec::H264, "sdr.mp4"),
        (ExportFormat::Gif, VideoCodec::H264, "sdr.gif"),
    ] {
        let editing = snow_recording_export::EditingSession::open(artifact.clone()).unwrap();
        let mut request = editing.export_request();
        request.playback_speed = if overlays { 0.5 } else { 1.0 };
        request.mouse.visible = overlays;
        request.mouse.click_enabled = false;
        request.mouse.trail_enabled = false;
        request.format = format;
        request.codec = codec;
        request.output_path = directory.path().join(extension);
        request.maximum_height = Some(36);
        request.maximum_width = Some(64); // force actual decode, resize, and encode
        request.performance.mode =
            if native && codec == VideoCodec::H265 && extension != "software-hdr.mp4" {
                ExportExecutionMode::HardwarePreferred
            } else {
                ExportExecutionMode::SoftwareOnly
            };
        request.prefer_hardware_h264 = native && codec == VideoCodec::H265;
        let output = editing.export(request).unwrap();
        let input = ffmpeg_next::format::input(&output.output_path).unwrap();
        let stream = input
            .streams()
            .best(ffmpeg_next::media::Type::Video)
            .unwrap();
        unsafe {
            let p = stream.parameters();
            let p = &*p.as_ptr();
            assert_eq!((p.width, p.height), (64, 36));
            if codec == VideoCodec::H265 {
                assert_eq!(p.profile, 2);
                assert_eq!(
                    p.color_trc,
                    ffmpeg_next::ffi::AVColorTransferCharacteristic::AVCOL_TRC_SMPTE2084
                );
                assert_eq!(
                    p.color_primaries,
                    ffmpeg_next::ffi::AVColorPrimaries::AVCOL_PRI_BT2020
                );
            } else {
                assert_ne!(
                    p.color_trc,
                    ffmpeg_next::ffi::AVColorTransferCharacteristic::AVCOL_TRC_SMPTE2084
                );
            }
        }
        if overlays && format == ExportFormat::Mp4 {
            let mut input = ffmpeg_next::format::input(&output.output_path).unwrap();
            let stream = input
                .streams()
                .best(ffmpeg_next::media::Type::Video)
                .unwrap();
            let stream_index = stream.index();
            let mut decoder =
                ffmpeg_next::codec::context::Context::from_parameters(stream.parameters())
                    .unwrap()
                    .decoder()
                    .video()
                    .unwrap();
            let mut decoded = ffmpeg_next::frame::Video::empty();
            let mut values = Vec::new();
            let mut observe = |frame: &ffmpeg_next::frame::Video| {
                let bytes = if codec == VideoCodec::H265 { 2 } else { 1 };
                let sample = |x: usize| {
                    let offset = 4 * frame.stride(0) + x * bytes;
                    if bytes == 2 {
                        u16::from_le_bytes([frame.data(0)[offset], frame.data(0)[offset + 1]])
                    } else {
                        u16::from(frame.data(0)[offset])
                    }
                };
                values.push((sample(4), sample(36)));
            };
            for (stream, packet) in input.packets() {
                if stream.index() != stream_index {
                    continue;
                }
                decoder.send_packet(&packet).unwrap();
                while decoder.receive_frame(&mut decoded).is_ok() {
                    observe(&decoded);
                }
            }
            decoder.send_eof().unwrap();
            while decoder.receive_frame(&mut decoded).is_ok() {
                observe(&decoded);
            }
            assert_eq!(values.len(), 6, "{values:?}");
            for (i, &(left, right)) in values.iter().enumerate() {
                let (white, black) = if i / 2 == 1 {
                    (right, left)
                } else {
                    (left, right)
                };
                if codec == VideoCodec::H265 {
                    let expected = 64.0 + 876.0 * snow_media::color::nits_to_pq(203.0);
                    assert!((f32::from(white) - expected).abs() < 15.0, "{values:?}");
                    assert!(black.abs_diff(64) < 10, "{values:?}");
                } else {
                    assert!(white > 170 && black < 30, "{values:?}");
                }
            }
        }
    }
}

#[test]
#[ignore = "requires native Metal P010 conversion"]
fn hdr_composition_converts_sdr_white_instead_of_relabeling_it() {
    use snow_macos::compositor::RgbaOverlay;
    let mut compositor =
        Compositor::new(PixelSize::new(16, 16).unwrap(), PixelFormat::P010, 2).unwrap();
    let white = vec![255; 16 * 16 * 4];
    let frame = compositor
        .compose_with_overlays(
            &[],
            &[RgbaOverlay {
                x: 0,
                y: 0,
                width: 16,
                height: 16,
                stride: 64,
                bytes: &white,
            }],
            true,
        )
        .unwrap();
    let cpu = frame.to_cpu().unwrap();
    let y = cpu.plane_bytes(0).unwrap();
    let code = u16::from_le_bytes([y[0], y[1]]) >> 6;
    let cpu_reference = 64.0 + 876.0 * snow_media::color::nits_to_pq(203.0);
    assert!(
        (f32::from(code) - cpu_reference).abs() < 4.0,
        "Metal/CPU reference-white disagreement: Metal={code}, CPU={cpu_reference}"
    );
    let uv = cpu.plane_bytes(1).unwrap();
    assert!((u16::from_le_bytes([uv[0], uv[1]]) >> 6).abs_diff(512) <= 2);
    assert!((u16::from_le_bytes([uv[2], uv[3]]) >> 6).abs_diff(512) <= 2);
}

/// Deterministic software-only test: no display, permissions, Metal or VT device.
#[test]
fn software_hdr_padded_p010_round_trip_preserves_depth_color_and_timeline() {
    use snow_media::{ColorDescription, CpuFrame, PlaneLayout};
    let directory = tempfile::tempdir().unwrap();
    let path = directory.path().join("software-hdr.mp4");
    let mut encoder = StreamingEncoder::builder(StreamingEncoderConfig {
        loop_animated_images: false,
        output_path: path.clone(),
        format: ExportFormat::Mp4,
        width: 64,
        height: 64,
        fps: 30,
        codec: VideoCodec::H265,
        prefer_hardware_h264: true,
        execution_mode: ExportExecutionMode::HardwarePreferred,
        software_h264_priority: SoftwareH264Priority::X264First,
        video: Default::default(),
        encode_threads: 1,
        audio: None,
    })
    .native_input(PixelFormat::P010)
    .software_only()
    .create()
    .unwrap();
    let stride = 144;
    let chroma_offset = stride * 64 + 16;
    let mut bytes = vec![0xcd; chroma_offset + stride * 32];
    for (offset, height, value) in [(0, 64, 512_u16), (chroma_offset, 32, 512_u16)] {
        for row in 0..height {
            for x in 0..64 {
                bytes[offset + row * stride + x * 2..offset + row * stride + x * 2 + 2]
                    .copy_from_slice(&(value << 6).to_le_bytes());
            }
        }
    }
    let mut image = CpuFrame {
        size: PixelSize::new(64, 64).unwrap(),
        format: PixelFormat::P010,
        color: ColorDescription::HDR10,
        planes: vec![
            PlaneLayout {
                offset: 0,
                width: 64,
                height: 64,
                stride,
                row_bytes: 128,
            },
            PlaneLayout {
                offset: chroma_offset,
                width: 32,
                height: 32,
                stride,
                row_bytes: 128,
            },
        ],
        bytes: bytes.into(),
    };
    image.color = ColorDescription::SRGB;
    assert!(encoder.push_cpu_hdr_frame_at_pts(0, &image).is_err());
    image.color = ColorDescription::HDR10;
    assert!(
        encoder
            .push_rgba_frame_at_pts(0, &[0; 64 * 64 * 4])
            .is_err()
    );
    for pts in [0, 2, 5] {
        encoder.push_cpu_hdr_frame_at_pts(pts, &image).unwrap();
    }
    let report = encoder.finish_at_pts(8).unwrap();
    assert!(!report.used_hardware_video_encoder);
    assert_eq!(report.video_encoder, "libx265");
    assert_eq!(report.encoded_frames, 3);
    let mut input = ffmpeg_next::format::input(&path).unwrap();
    let stream = input
        .streams()
        .best(ffmpeg_next::media::Type::Video)
        .unwrap();
    let index = stream.index();
    let time_base = stream.time_base();
    let mut decoder = ffmpeg_next::codec::context::Context::from_parameters(stream.parameters())
        .unwrap()
        .decoder()
        .video()
        .unwrap();
    unsafe {
        assert_eq!(
            (*decoder.as_ptr()).has_b_frames,
            0,
            "live HDR must not reorder frames"
        );
        assert_eq!((*decoder.as_ptr()).profile, 2);
        assert_eq!(
            (*decoder.as_ptr()).color_primaries,
            ffmpeg_next::ffi::AVColorPrimaries::AVCOL_PRI_BT2020
        );
        assert_eq!(
            (*decoder.as_ptr()).color_trc,
            ffmpeg_next::ffi::AVColorTransferCharacteristic::AVCOL_TRC_SMPTE2084
        );
    }
    let mut decoded = ffmpeg_next::frame::Video::empty();
    let mut times = Vec::new();
    let mut receive = |decoder: &mut ffmpeg_next::decoder::Video| {
        while decoder.receive_frame(&mut decoded).is_ok() {
            assert_eq!(decoded.format(), ffmpeg_next::format::Pixel::YUV420P10LE);
            let value = u16::from_le_bytes(decoded.data(0)[..2].try_into().unwrap());
            assert!(
                (504..=520).contains(&value),
                "PQ code value changed: {value}"
            );
            times.push(
                decoded.pts().unwrap() * i64::from(time_base.numerator()) * 30
                    / i64::from(time_base.denominator()),
            );
        }
    };
    for (stream, packet) in input.packets() {
        if stream.index() == index {
            decoder.send_packet(&packet).unwrap();
            receive(&mut decoder);
        }
    }
    decoder.send_eof().unwrap();
    receive(&mut decoder);
    assert_eq!(times, [0, 2, 5]);
}

#[test]
#[ignore = "requires native Metal P010 composition"]
fn hdr_cpu_and_metal_agree_for_colored_translucent_effects() {
    use ffmpeg_next as ffmpeg;
    use snow_macos::compositor::RgbaOverlay;
    let mut compositor =
        Compositor::new(PixelSize::new(16, 16).unwrap(), PixelFormat::P010, 2).unwrap();
    let mut conversion = ffmpeg::software::scaling::Context::get(
        ffmpeg::format::Pixel::RGB48LE,
        16,
        16,
        ffmpeg::format::Pixel::P010LE,
        16,
        16,
        ffmpeg::software::scaling::Flags::BICUBIC,
    )
    .unwrap();
    unsafe {
        let coefficients = ffmpeg::ffi::sws_getCoefficients(ffmpeg::ffi::SWS_CS_BT2020);
        assert_eq!(
            ffmpeg::ffi::sws_setColorspaceDetails(
                conversion.as_mut_ptr(),
                coefficients,
                1,
                coefficients,
                0,
                0,
                1 << 16,
                1 << 16
            ),
            0
        );
    }
    for color in [
        [255, 0, 0, 255],
        [255, 0, 0, 128],
        [0, 255, 0, 128],
        [128, 128, 128, 128],
    ] {
        let alpha = u16::from(color[3]);
        let tile = [
            ((u16::from(color[0]) * alpha + 127) / 255) as u8,
            ((u16::from(color[1]) * alpha + 127) / 255) as u8,
            ((u16::from(color[2]) * alpha + 127) / 255) as u8,
            color[3],
        ]
        .repeat(16 * 16);
        let image = compositor
            .compose_with_overlays(
                &[],
                &[RgbaOverlay {
                    x: 0,
                    y: 0,
                    width: 16,
                    height: 16,
                    stride: 64,
                    bytes: &tile,
                }],
                true,
            )
            .unwrap()
            .to_cpu()
            .unwrap();
        let mut pixel = [0; 6];
        snow_media::color::blend_srgb_into_pq(&mut pixel, color);
        let mut rgb = ffmpeg::frame::Video::new(ffmpeg::format::Pixel::RGB48LE, 16, 16);
        let stride = rgb.stride(0);
        for row in rgb.data_mut(0).chunks_mut(stride).take(16) {
            for destination in row[..16 * 6].chunks_exact_mut(6) {
                destination.copy_from_slice(&pixel);
            }
        }
        let mut p010 = ffmpeg::frame::Video::new(ffmpeg::format::Pixel::P010LE, 16, 16);
        conversion.run(&rgb, &mut p010).unwrap();
        for (plane, x, y) in [(0, 8, 8), (1, 8, 4), (1, 9, 4)] {
            let offset = image.planes[plane].offset + y * image.planes[plane].stride + x * 2;
            let actual = u16::from_le_bytes([image.bytes[offset], image.bytes[offset + 1]]) >> 6;
            let offset = y * p010.stride(plane) + x * 2;
            let expected =
                u16::from_le_bytes([p010.data(plane)[offset], p010.data(plane)[offset + 1]]) >> 6;
            assert!(
                actual.abs_diff(expected) <= 5,
                "color={color:?} plane={plane}: Metal={actual} CPU={expected}"
            );
        }
    }
}
