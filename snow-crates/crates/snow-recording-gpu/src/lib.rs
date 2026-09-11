//! GPU video encoding for screen recording.
//!
//! Converts D3D11 BGRA capture textures to NV12 via
//! [`ID3D11VideoProcessor`][vp] (full-range sRGB in, BT.601
//! studio-range YCbCr out — the same conversion as the CPU kernel in
//! `snow-recording-export`, so output colors match) and feeds the Media
//! Foundation H.264 Encoder MFT directly from those textures through an
//! `IMFDXGIDeviceManager` bound to the capture device. Compressed output
//! is handed upstream as Annex-B packets for muxing.
//!
//! All stages run on one D3D11 device, ideally the same device that
//! produced the capture textures: the device is created without
//! `D3D11_CREATE_DEVICE_SINGLETHREADED`, so the runtime serializes
//! cross-thread access internally.
//!
//! [vp]: windows::Win32::Graphics::Direct3D11::ID3D11VideoProcessor

#![cfg(windows)]

mod error;
mod h264;
mod mf_encoder;
mod video_processor;

use windows::Win32::Graphics::Direct3D11::{ID3D11Device, ID3D11DeviceContext, ID3D11Texture2D};

pub use error::{GpuEncoderError, Result};
pub use h264::{build_avcc_from_annexb, parameter_sets, prepend_nals};
pub use video_processor::{Nv12Image, SourceRect};

use mf_encoder::{GOP_SIZE, MfH264Encoder, texture_to_sample};

/// Number of NV12 textures rotating between the video processor and the
/// encoder MFT. The MFT holds GPU references to submitted samples; device
/// command ordering makes a small ring safe in practice.
const NV12_RING_CAPACITY: usize = 4;

/// Keyframe interval in frames; mirrors the FFmpeg `h264_mf` speed
/// options applied on the CPU path (`g=60`).
pub const H264_GOP_SIZE: u32 = GOP_SIZE;

/// Per-stage timings of the GPU encode path. Only present in builds with
/// the `stage-timing` feature (benchmarks and diagnostics).
#[cfg(feature = "stage-timing")]
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct GpuStageTimings {
    pub blt: std::time::Duration,
    pub mft_input: std::time::Duration,
    pub mft_output: std::time::Duration,
}

/// One encoded H.264 packet (Annex-B byte stream).
#[derive(Clone, Debug)]
pub struct EncodedPacket {
    pub timestamp_ms: u64,
    pub is_keyframe: bool,
    pub data: Vec<u8>,
}

/// Zero-copy H.264 encoder: BGRA texture in, Annex-B packets out.
pub struct GpuH264Encoder {
    processor: video_processor::VideoProcessor,
    nv12_ring: Vec<ID3D11Texture2D>,
    ring_cursor: usize,
    encoder: MfH264Encoder,
    sequence_header: Vec<u8>,
    frame_interval_hns: i64,
    #[cfg(feature = "stage-timing")]
    timings: GpuStageTimings,
}

impl GpuH264Encoder {
    /// Create the converter and encoder on `device`.
    ///
    /// `source_width/height` describe the input textures (the crop rect
    /// submitted with each frame must fit inside), while
    /// `output_width/height` set the encoded resolution: differing values
    /// scale on the GPU for free.
    #[allow(clippy::too_many_arguments)]
    pub fn new(
        device: &ID3D11Device,
        context: &ID3D11DeviceContext,
        source_width: u32,
        source_height: u32,
        output_width: u32,
        output_height: u32,
        fps: u32,
        bitrate_bps: usize,
    ) -> Result<Self> {
        let processor = video_processor::VideoProcessor::new(
            device,
            context,
            source_width,
            source_height,
            output_width,
            output_height,
            fps,
        )?;
        let mut nv12_ring = Vec::with_capacity(NV12_RING_CAPACITY);
        for _ in 0..NV12_RING_CAPACITY {
            nv12_ring.push(processor.create_nv12_texture()?);
        }
        let (encoder, sequence_header) =
            MfH264Encoder::new(device, output_width, output_height, fps, bitrate_bps)?;
        Ok(Self {
            processor,
            nv12_ring,
            ring_cursor: 0,
            encoder,
            sequence_header,
            frame_interval_hns: 10_000_000 / i64::from(fps.max(1)),
            #[cfg(feature = "stage-timing")]
            timings: GpuStageTimings::default(),
        })
    }

    /// The negotiated sequence header as an avcC record for MP4 muxing.
    /// Empty when the encoder reported no header; the muxer then derives
    /// the sample description from keyframe packets.
    pub fn extradata(&self) -> Vec<u8> {
        h264::build_avcc_from_annexb(&self.sequence_header)
    }

    /// Convert and encode one BGRA texture. Returns the packets that
    /// became available; hardware encoders commonly emit packets with a
    /// small delay.
    pub fn submit(
        &mut self,
        texture: &ID3D11Texture2D,
        rect: Option<SourceRect>,
        timestamp_ms: u64,
    ) -> Result<Vec<EncodedPacket>> {
        #[cfg(feature = "stage-timing")]
        let blt_started = std::time::Instant::now();
        let nv12 = self.nv12_ring[self.ring_cursor].clone();
        self.ring_cursor = (self.ring_cursor + 1) % self.nv12_ring.len();
        self.processor.convert(texture, rect, &nv12)?;
        #[cfg(feature = "stage-timing")]
        {
            self.timings.blt += blt_started.elapsed();
        }

        #[cfg(feature = "stage-timing")]
        let input_started = std::time::Instant::now();
        let sample = texture_to_sample(
            &nv12,
            h264::ms_to_hns(timestamp_ms),
            self.frame_interval_hns,
        )?;
        let submitted = self.encoder.submit(&sample);
        #[cfg(feature = "stage-timing")]
        {
            self.timings.mft_input += input_started.elapsed();
        }
        let raw = submitted?;
        #[cfg(feature = "stage-timing")]
        let output_started = std::time::Instant::now();
        let packets = self.finalize_packets(raw);
        #[cfg(feature = "stage-timing")]
        {
            self.timings.mft_output += output_started.elapsed();
        }
        Ok(packets)
    }

    /// End the stream and drain the encoder.
    pub fn finish(&mut self) -> Result<Vec<EncodedPacket>> {
        let raw = self.encoder.finish()?;
        Ok(self.finalize_packets(raw))
    }

    /// Per-stage timings of the GPU encode path.
    #[cfg(feature = "stage-timing")]
    pub fn stage_timings(&self) -> &GpuStageTimings {
        &self.timings
    }

    /// Convert one BGRA texture to NV12 and read the pixels back.
    ///
    /// Test and diagnostic helper only — the production path never reads
    /// converted pixels back to the CPU.
    pub fn convert_to_nv12_for_tests(
        &mut self,
        texture: &ID3D11Texture2D,
        rect: Option<SourceRect>,
    ) -> Result<video_processor::Nv12Image> {
        self.processor.convert_and_read_back(texture, rect)
    }

    fn finalize_packets(&mut self, raw: Vec<mf_encoder::RawPacket>) -> Vec<EncodedPacket> {
        let sequence_header = self.sequence_header.clone();
        let parameters = h264::parameter_sets(&sequence_header);
        raw.into_iter()
            .map(|packet| {
                let mut data = packet.data;
                if packet.is_keyframe && !parameters.is_empty() {
                    // Keep every keyframe self-describing for players and
                    // for muxers that derive the sample description from
                    // packets.
                    h264::prepend_nals(&mut data, &parameters);
                }
                EncodedPacket {
                    timestamp_ms: h264::hns_to_ms(packet.timestamp_hns),
                    // Some hardware encoders never set the clean-point
                    // attribute; the bitstream is the source of truth.
                    is_keyframe: packet.is_keyframe || h264::packet_starts_with_idr(&data),
                    data,
                }
            })
            .collect()
    }
}
