//! GPU encoder integration tests.
//!
//! The conversion-parity and round-trip tests need a real D3D11 video
//! processor and a hardware H.264 encoder MFT; run them explicitly with:
//! `cargo test -p snow-recording-gpu --test gpu_encoder -- --ignored`

use std::path::Path;

use snow_recording_gpu::{GpuH264Encoder, SourceRect};
use windows::Win32::Foundation::HMODULE;
use windows::Win32::Graphics::Direct3D::{
    D3D_DRIVER_TYPE, D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP, D3D_FEATURE_LEVEL_11_0,
};
use windows::Win32::Graphics::Direct3D11::{
    D3D11_CPU_ACCESS_WRITE, D3D11_CREATE_DEVICE_BGRA_SUPPORT, D3D11_MAP_WRITE,
    D3D11_MAPPED_SUBRESOURCE, D3D11_SDK_VERSION, D3D11_TEXTURE2D_DESC, D3D11_USAGE_DEFAULT,
    D3D11_USAGE_STAGING, D3D11CreateDevice, ID3D11Device, ID3D11DeviceContext, ID3D11Resource,
    ID3D11Texture2D,
};
use windows::Win32::Graphics::Dxgi::Common::{
    DXGI_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_SAMPLE_DESC,
};
use windows::core::Interface;

fn create_device(driver: D3D_DRIVER_TYPE) -> (ID3D11Device, ID3D11DeviceContext) {
    let feature_levels = [D3D_FEATURE_LEVEL_11_0];
    let mut device = None;
    let mut context = None;
    unsafe {
        D3D11CreateDevice(
            None,
            driver,
            HMODULE::default(),
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            Some(&feature_levels),
            D3D11_SDK_VERSION,
            Some(&mut device),
            None,
            Some(&mut context),
        )
    }
    .expect("D3D11CreateDevice");
    (device.expect("device"), context.expect("context"))
}

fn simple_desc(format: DXGI_FORMAT, width: u32, height: u32) -> D3D11_TEXTURE2D_DESC {
    D3D11_TEXTURE2D_DESC {
        Width: width,
        Height: height,
        MipLevels: 1,
        ArraySize: 1,
        Format: format,
        SampleDesc: DXGI_SAMPLE_DESC {
            Count: 1,
            Quality: 0,
        },
        Usage: D3D11_USAGE_DEFAULT,
        BindFlags: 0,
        CPUAccessFlags: 0,
        MiscFlags: 0,
    }
}

fn staging_desc(format: DXGI_FORMAT, width: u32, height: u32, access: u32) -> D3D11_TEXTURE2D_DESC {
    D3D11_TEXTURE2D_DESC {
        Usage: D3D11_USAGE_STAGING,
        CPUAccessFlags: access,
        BindFlags: 0,
        ..simple_desc(format, width, height)
    }
}

/// Upload packed BGRA pixels into a DEFAULT-usage texture.
fn upload_bgra(
    device: &ID3D11Device,
    context: &ID3D11DeviceContext,
    pixels: &[u8],
    width: u32,
    height: u32,
) -> ID3D11Texture2D {
    let staging = {
        let desc = staging_desc(
            DXGI_FORMAT_B8G8R8A8_UNORM,
            width,
            height,
            D3D11_CPU_ACCESS_WRITE.0 as u32,
        );
        let mut texture = None;
        unsafe { device.CreateTexture2D(&desc, None, Some(&mut texture)) }
            .expect("staging texture");
        texture.expect("staging texture")
    };
    unsafe {
        let mut mapped = D3D11_MAPPED_SUBRESOURCE::default();
        context
            .Map(&staging, 0, D3D11_MAP_WRITE, 0, Some(&mut mapped))
            .expect("map staging");
        let stride = mapped.RowPitch as usize;
        for row in 0..height as usize {
            let src = &pixels[row * width as usize * 4..][..width as usize * 4];
            let dst = std::slice::from_raw_parts_mut(
                mapped.pData.cast::<u8>().add(row * stride),
                src.len(),
            );
            dst.copy_from_slice(src);
        }
        context.Unmap(&staging, 0);
    }
    let texture = {
        let desc = simple_desc(DXGI_FORMAT_B8G8R8A8_UNORM, width, height);
        let mut texture = None;
        unsafe { device.CreateTexture2D(&desc, None, Some(&mut texture)) }.expect("texture");
        texture.expect("texture")
    };
    let staging_resource: ID3D11Resource = staging.cast().unwrap();
    let texture_resource: ID3D11Resource = texture.cast().unwrap();
    unsafe { context.CopyResource(&texture_resource, &staging_resource) };
    texture
}

/// Deterministic smooth synthetic BGRA content (no hard edges: those hit
/// chroma-siting differences between the GPU processor and the CPU kernel).
fn synthetic_bgra(width: usize, height: usize, frame: usize) -> Vec<u8> {
    let mut pixels = vec![0u8; width * height * 4];
    for y in 0..height {
        for x in 0..width {
            let pixel = &mut pixels[(y * width + x) * 4..][..4];
            // BGRA channel order, triangle-wave ramps avoid discontinuities.
            let phase = |value: usize, span: usize| {
                let cycle = (value % (span.max(2) * 2)) as i32 - span.max(2) as i32;
                (cycle.unsigned_abs() * 255 / span.max(2) as u32).min(255) as u8
            };
            pixel[0] = phase(x + frame * 3, width);
            pixel[1] = phase(y + frame * 5, height);
            pixel[2] = phase(x + y + frame * 7, width + height);
            pixel[3] = 255;
        }
    }
    pixels
}

#[test]
fn gpu_encoder_rejects_warp_devices() {
    // WARP ships no video processor; the encoder must fail with a
    // fallback-eligible error instead of producing broken output.
    let (device, context) = create_device(D3D_DRIVER_TYPE_WARP);
    let result = GpuH264Encoder::new(&device, &context, 64, 64, 64, 64, 10, 1_000_000);
    assert!(
        result.is_err(),
        "WARP devices must not host the GPU encoder pipeline"
    );
}

#[test]
#[ignore = "requires a D3D11 video processor and hardware H.264 MFT"]
fn gpu_nv12_matches_cpu_kernel_within_tolerance() {
    let width = 640u32;
    let height = 360u32;
    let pixels = synthetic_bgra(width as usize, height as usize, 0);
    let (device, context) = create_device(D3D_DRIVER_TYPE_HARDWARE);
    let source = upload_bgra(&device, &context, &pixels, width, height);

    let mut encoder = GpuH264Encoder::new(
        &device, &context, width, height, width, height, 30, 4_000_000,
    )
    .expect("hardware encoder");
    let gpu = encoder
        .convert_to_nv12_for_tests(&source, None)
        .expect("GPU conversion");
    drop(encoder);

    // CPU kernel over the same pixels, compact NV12 layout.
    use snow_recording_export::streaming::convert::{
        ChromaPlanes, ConversionMode, RgbOrder, convert_rgb_to_yuv420, yuv420_planes_from_parts,
    };
    let mut y = vec![0u8; gpu.y.len()];
    let mut uv = vec![0u8; gpu.uv.len()];
    let planes = yuv420_planes_from_parts(
        width as usize,
        height as usize,
        &mut y,
        width as usize,
        ChromaPlanes::Nv12 {
            uv: &mut uv,
            uv_stride: width as usize,
        },
    );
    convert_rgb_to_yuv420(&pixels, RgbOrder::Bgra, planes, ConversionMode::Auto);

    let (mut max_y, mut max_uv) = (0i32, 0i32);
    let mut sum_y = 0u128;
    for (gpu_value, cpu_value) in gpu.y.iter().zip(&y) {
        let delta = i32::from(*gpu_value) - i32::from(*cpu_value);
        max_y = max_y.max(delta.abs());
        sum_y += delta.unsigned_abs() as u128;
    }
    let mut sum_uv = 0u128;
    for (gpu_value, cpu_value) in gpu.uv.iter().zip(&uv) {
        let delta = i32::from(*gpu_value) - i32::from(*cpu_value);
        max_uv = max_uv.max(delta.abs());
        sum_uv += delta.unsigned_abs() as u128;
    }
    let mean_y = sum_y as f64 / gpu.y.len() as f64;
    let mean_uv = sum_uv as f64 / gpu.uv.len() as f64;
    eprintln!(
        "NV12 parity: max |dY|={max_y}, mean |dY|={mean_y:.4}, max |dUV|={max_uv}, mean |dUV|={mean_uv:.4}"
    );

    // The kernel-vs-swscale tolerance used in the CPU crate is 2; hardware
    // video processors may round slightly differently, so the budget is 4
    // with a tight mean.
    assert!(max_y <= 4, "luma difference too large: {max_y}");
    assert!(max_uv <= 4, "chroma difference too large: {max_uv}");
    assert!(mean_y <= 0.6, "mean luma difference too large: {mean_y}");
    assert!(
        mean_uv <= 0.6,
        "mean chroma difference too large: {mean_uv}"
    );
}

#[test]
#[ignore = "requires a D3D11 video processor and hardware H.264 MFT"]
fn gpu_nv12_crop_matches_kernel_subrectangle() {
    let width = 640u32;
    let height = 360u32;
    let rect = SourceRect {
        x: 128,
        y: 72,
        width: 320,
        height: 180,
    };
    let pixels = synthetic_bgra(width as usize, height as usize, 3);
    let (device, context) = create_device(D3D_DRIVER_TYPE_HARDWARE);
    let source = upload_bgra(&device, &context, &pixels, width, height);
    let mut encoder = GpuH264Encoder::new(
        &device,
        &context,
        width,
        height,
        rect.width,
        rect.height,
        30,
        4_000_000,
    )
    .expect("hardware encoder");
    let gpu = encoder
        .convert_to_nv12_for_tests(&source, Some(rect))
        .expect("GPU conversion with crop");
    drop(encoder);

    assert_eq!((gpu.width, gpu.height), (rect.width, rect.height));

    use snow_recording_export::streaming::convert::{
        ChromaPlanes, ConversionMode, RgbOrder, convert_rgb_to_yuv420, yuv420_planes_from_parts,
    };
    let mut cropped = vec![0u8; (rect.width * rect.height * 4) as usize];
    for row in 0..rect.height as usize {
        let src_row = (row + rect.y as usize) * width as usize + rect.x as usize;
        let dst_row = row * rect.width as usize;
        cropped[dst_row * 4..(dst_row + rect.width as usize) * 4]
            .copy_from_slice(&pixels[src_row * 4..(src_row + rect.width as usize) * 4]);
    }
    let mut y = vec![0u8; gpu.y.len()];
    let mut uv = vec![0u8; gpu.uv.len()];
    let planes = yuv420_planes_from_parts(
        rect.width as usize,
        rect.height as usize,
        &mut y,
        rect.width as usize,
        ChromaPlanes::Nv12 {
            uv: &mut uv,
            uv_stride: rect.width as usize,
        },
    );
    convert_rgb_to_yuv420(&cropped, RgbOrder::Bgra, planes, ConversionMode::Auto);

    let max_y = gpu
        .y
        .iter()
        .zip(&y)
        .map(|(g, c)| (i32::from(*g) - i32::from(*c)).abs())
        .max()
        .unwrap_or_default();
    let max_uv = gpu
        .uv
        .iter()
        .zip(&uv)
        .map(|(g, c)| (i32::from(*g) - i32::from(*c)).abs())
        .max()
        .unwrap_or_default();
    assert!(max_y <= 4, "cropped luma difference too large: {max_y}");
    assert!(max_uv <= 4, "cropped chroma difference too large: {max_uv}");
}

#[test]
#[ignore = "requires a D3D11 video processor and hardware H.264 MFT"]
fn gpu_encoder_roundtrips_through_muxer() {
    let width = 320u32;
    let height = 180u32;
    let fps = 10u32;
    let frames = 12usize;
    let bitrate = snow_recording_export::video_quality::smart_quality_bitrate_bps(
        width,
        height,
        fps,
        &snow_recording_model::VideoEncodeConfig {
            quality: 80,
            speed: snow_recording_model::VideoEncodingSpeed::VeryFast,
        },
        false,
    );

    let (device, context) = create_device(D3D_DRIVER_TYPE_HARDWARE);
    let mut encoder = GpuH264Encoder::new(
        &device, &context, width, height, width, height, fps, bitrate,
    )
    .expect("hardware encoder");
    let extradata = encoder.extradata();
    assert!(
        !extradata.is_empty(),
        "hardware encoder must report an avcC record"
    );

    let mut packets = Vec::new();
    for frame in 0..frames {
        let pixels = synthetic_bgra(width as usize, height as usize, frame);
        let texture = upload_bgra(&device, &context, &pixels, width, height);
        let timestamp_ms = (frame as u64) * 1_000 / u64::from(fps);
        packets.extend(
            encoder
                .submit(&texture, None, timestamp_ms)
                .expect("submit"),
        );
    }
    packets.extend(encoder.finish().expect("finish"));

    assert!(
        packets.len() >= frames.saturating_sub(2),
        "expected roughly one packet per frame, got {}",
        packets.len()
    );
    assert!(packets.first().is_some_and(|packet| packet.is_keyframe));
    let timestamps: Vec<u64> = packets.iter().map(|packet| packet.timestamp_ms).collect();
    assert!(
        timestamps.windows(2).all(|pair| pair[0] < pair[1]),
        "packet presentation times must be strictly increasing: {timestamps:?}"
    );

    // Mux through the streaming pipeline's external-video mode and decode.
    use snow_recording_export::config::{ExportExecutionMode, ExportFormat, SoftwareH264Priority};
    use snow_recording_export::streaming::{
        ExternalVideoTrack, StreamingEncoder, StreamingEncoderConfig, StreamingPixelOrder,
    };
    let directory = tempfile::tempdir().unwrap();
    let output = directory.path().join("gpu-recording.mp4");
    let mut streaming = StreamingEncoder::create(StreamingEncoderConfig {
        output_path: output.clone(),
        format: ExportFormat::Mp4,
        width,
        height,
        fps,
        codec: snow_recording_model::VideoCodec::H264,
        prefer_hardware_h264: true,
        execution_mode: ExportExecutionMode::HardwarePreferred,
        software_h264_priority: SoftwareH264Priority::X264First,
        video: snow_recording_model::VideoEncodeConfig {
            quality: 80,
            speed: snow_recording_model::VideoEncodingSpeed::VeryFast,
        },
        encode_threads: 1,
        audio: None,
        pixel_order: StreamingPixelOrder::Bgra,
        external_video: Some(ExternalVideoTrack {
            extradata,
            encoder_name: "h264_mf_gpu".into(),
        }),
    })
    .expect("streaming encoder");
    for packet in &packets {
        streaming
            .push_encoded_video_packet(packet.timestamp_ms, packet.is_keyframe, &packet.data)
            .expect("push encoded packet");
    }
    let report = streaming.finish().expect("finish streaming");
    assert_eq!(report.video_encoder, "h264_mf_gpu");

    let (decoded, decoded_width, decoded_height) = decoded_video_frame_count(&output);
    assert_eq!((decoded_width, decoded_height), (width, height));
    assert_eq!(decoded, frames, "all submitted frames must decode");
    assert!(output.is_file());
}

fn decoded_video_frame_count(path: &Path) -> (usize, u32, u32) {
    use ffmpeg_next as ffmpeg;
    let mut input = ffmpeg::format::input(path).expect("open output");
    let stream = input
        .streams()
        .best(ffmpeg::media::Type::Video)
        .expect("video stream");
    let stream_index = stream.index();
    let mut decoder = ffmpeg::codec::context::Context::from_parameters(stream.parameters())
        .unwrap()
        .decoder()
        .video()
        .unwrap();
    let dimensions = (decoder.width(), decoder.height());
    let mut decoded = ffmpeg::frame::Video::empty();
    let mut count = 0usize;
    for (stream, packet) in input.packets() {
        if stream.index() != stream_index {
            continue;
        }
        decoder.send_packet(&packet).unwrap();
        while decoder.receive_frame(&mut decoded).is_ok() {
            count += 1;
        }
    }
    decoder.send_eof().unwrap();
    while decoder.receive_frame(&mut decoded).is_ok() {
        count += 1;
    }
    (count, dimensions.0, dimensions.1)
}
