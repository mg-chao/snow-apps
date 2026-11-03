#![cfg(target_os = "macos")]

use ffmpeg_next::{self as ffmpeg, Rescale};
use snow_recording_export::{
    ExportExecutionMode, ExportFormat, SoftwareH264Priority, StreamingEncoder,
    StreamingEncoderConfig, VideoCodec,
};

// No display, capture permission, microphone or GUI is needed. These fixtures
// exercise the actual hardware codec and MP4 muxer used by deferred/trim export.
#[test]
#[ignore = "requires a native VideoToolbox H.264 hardware encoder"]
fn offline_h264_preserves_reordered_timestamps() {
    offline_round_trips(VideoCodec::H264);
}

#[test]
#[ignore = "requires a native VideoToolbox HEVC hardware encoder"]
fn offline_hevc_preserves_reordered_timestamps() {
    offline_round_trips(VideoCodec::H265);
}

#[test]
#[ignore = "requires native VideoToolbox hardware encoders"]
fn fractional_time_bases_preserve_packet_timestamps() {
    ffmpeg::init().unwrap();
    for name in ["h264_videotoolbox", "hevc_videotoolbox"] {
        for time_base in [(1001, 60_000), (1000, 60_000)] {
            let codec = ffmpeg::encoder::find_by_name(name).unwrap();
            let mut encoder = ffmpeg::codec::context::Context::new_with_codec(codec)
                .encoder()
                .video()
                .unwrap();
            encoder.set_width(128);
            encoder.set_height(72);
            encoder.set_format(ffmpeg::format::Pixel::NV12);
            encoder.set_time_base(time_base);
            encoder.set_frame_rate(Some((time_base.1, time_base.0)));
            encoder.set_bit_rate(2_000_000);
            encoder.set_flags(ffmpeg::codec::Flags::GLOBAL_HEADER);
            let mut options = ffmpeg::Dictionary::new();
            options.set("allow_sw", "0");
            options.set("realtime", "0");
            options.set("bf", "2");
            let mut encoder = encoder.open_as_with(codec, options).unwrap();
            let mut packet = ffmpeg::Packet::empty();
            let mut pts = Vec::new();
            let mut previous_dts = None;
            let mut receive = |encoder: &mut ffmpeg::encoder::video::Encoder| loop {
                match encoder.receive_packet(&mut packet) {
                    Ok(()) => {
                        let timestamp = packet.pts().unwrap();
                        let dts = packet.dts().unwrap();
                        assert!(
                            timestamp >= dts,
                            "{name}, {time_base:?}: {timestamp} < {dts}"
                        );
                        assert!(previous_dts.is_none_or(|previous| dts > previous));
                        previous_dts = Some(dts);
                        pts.push(timestamp);
                    }
                    Err(ffmpeg::Error::Eof) => break,
                    Err(ffmpeg::Error::Other { errno }) if errno == ffmpeg::error::EAGAIN => break,
                    Err(error) => panic!("packet drain failed: {error}"),
                }
            };
            for index in 0..16 {
                let mut frame = ffmpeg::frame::Video::new(ffmpeg::format::Pixel::NV12, 128, 72);
                frame.data_mut(0).fill(16 + index as u8 * 3);
                frame.data_mut(1).fill(128);
                frame.set_pts(Some(index));
                encoder.send_frame(&frame).unwrap();
                receive(&mut encoder);
            }
            encoder.send_eof().unwrap();
            receive(&mut encoder);
            pts.sort_unstable();
            assert_eq!(pts, (0..16).collect::<Vec<_>>(), "{name}, {time_base:?}");
        }
    }
}

fn offline_round_trips(codec: VideoCodec) {
    let dense: Vec<u64> = (0..60).collect();
    let sparse = [0, 1, 2, 3, 7, 8, 13, 21, 22, 23, 30, 31, 32, 45, 46, 60];
    // One/two frames finish before the reorder delay can be initialized. Sparse
    // frames and a final hold must keep their presentation times and endpoint.
    for pts in [&dense[..], &dense[..1], &dense[..2], &dense[..3], &sparse] {
        round_trip(codec, pts);
    }
}

fn round_trip(codec: VideoCodec, timestamps: &[u64]) {
    let directory = tempfile::tempdir().unwrap();
    let path = directory.path().join("reordered.mp4");
    let (width, height, fps) = (646, 580, 60);
    let endpoint = timestamps.last().unwrap() + 13;
    let mut encoder = StreamingEncoder::builder(StreamingEncoderConfig {
        loop_animated_images: false,
        output_path: path.clone(),
        format: ExportFormat::Mp4,
        width,
        height,
        fps,
        codec,
        prefer_hardware_h264: true,
        execution_mode: ExportExecutionMode::HardwareOnly,
        software_h264_priority: SoftwareH264Priority::X264First,
        video: snow_recording_export::config::VideoEncodeConfig {
            quality: 80,
            ..Default::default()
        },
        encode_threads: 1,
        audio: Vec::new(),
    })
    .offline_output()
    .create()
    .unwrap();
    let mut rgba = vec![0; (width * height * 4) as usize];
    for (index, &pts) in timestamps.iter().enumerate() {
        for (pixel, color) in rgba.chunks_exact_mut(4).enumerate() {
            let x = pixel % width as usize;
            let y = pixel / width as usize;
            let value = ((x + y + index * 3) % 220 + 16) as u8;
            color.copy_from_slice(&[value, value, value, 255]);
        }
        encoder.push_rgba_frame_at_pts(pts, &rgba).unwrap();
    }
    let report = encoder.finish_at_pts(endpoint).unwrap();
    assert!(report.used_hardware_video_encoder && !report.hardware_fallback);
    assert_eq!(report.encoded_frames, timestamps.len() as u64);
    assert_eq!(
        report.video_encoder,
        match codec {
            VideoCodec::H264 => "h264_videotoolbox",
            VideoCodec::H265 => "hevc_videotoolbox",
        }
    );

    let mut input = ffmpeg::format::input(&path).unwrap();
    let stream = input.streams().best(ffmpeg::media::Type::Video).unwrap();
    let stream_index = stream.index();
    let time_base = stream.time_base();
    let frame_time_base = ffmpeg::Rational(1, fps as i32);
    assert_eq!(
        stream.duration().rescale(time_base, frame_time_base),
        endpoint as i64
    );
    let mut decoder = ffmpeg::codec::context::Context::from_parameters(stream.parameters())
        .unwrap()
        .decoder()
        .video()
        .unwrap();
    let mut decoded = ffmpeg::frame::Video::empty();
    let mut decoded_pts = Vec::new();
    let mut receive = |decoder: &mut ffmpeg::decoder::Video| loop {
        match decoder.receive_frame(&mut decoded) {
            Ok(()) => {
                assert_eq!((decoded.width(), decoded.height()), (width, height));
                decoded_pts.push(decoded.pts().unwrap().rescale(time_base, frame_time_base));
            }
            Err(ffmpeg::Error::Eof) => break,
            Err(ffmpeg::Error::Other { errno }) if errno == ffmpeg::error::EAGAIN => break,
            Err(error) => panic!("decode failed: {error}"),
        }
    };
    let mut previous_dts = None;
    let mut packet_pts = Vec::new();
    for (stream, packet) in input.packets() {
        if stream.index() != stream_index {
            continue;
        }
        let pts = packet.pts().unwrap();
        let dts = packet.dts().unwrap();
        assert!(pts >= dts, "{codec:?}: PTS {pts} precedes DTS {dts}");
        assert!(previous_dts.is_none_or(|previous| dts > previous));
        previous_dts = Some(dts);
        packet_pts.push(pts.rescale(time_base, frame_time_base));
        decoder.send_packet(&packet).unwrap();
        receive(&mut decoder);
    }
    decoder.send_eof().unwrap();
    receive(&mut decoder);
    let expected: Vec<i64> = timestamps.iter().map(|&pts| pts as i64).collect();
    assert_eq!(decoded_pts, expected);
    // Some older devices do not support reordering even when requested. The
    // codec policy test separately checks that offline export still enables it.
    if timestamps.len() == 60 && decoder.has_b_frames() {
        assert!(
            packet_pts.windows(2).any(|pair| pair[0] > pair[1]),
            "offline hardware export should retain B-frame compression"
        );
    }
    packet_pts.sort_unstable();
    assert_eq!(packet_pts, expected);
}
