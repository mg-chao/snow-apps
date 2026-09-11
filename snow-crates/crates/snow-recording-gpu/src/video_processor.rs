//! `ID3D11VideoProcessor` wrapper: BGRA → NV12 with BT.601 limited range.
//!
//! The color-space pair reproduces the CPU conversion kernel shipped in
//! `snow-recording-export` (full-range sRGB input, BT.601 studio-range
//! YCbCr output) so the GPU lane matches the recorded colors of the CPU
//! lane. The processor also scales when the output dimensions differ from
//! the source crop.

use windows::Win32::Graphics::Direct3D11::{
    D3D11_BIND_RENDER_TARGET, D3D11_CPU_ACCESS_READ, D3D11_MAP_READ, D3D11_MAPPED_SUBRESOURCE,
    D3D11_TEX2D_VPIV, D3D11_TEXTURE2D_DESC, D3D11_USAGE_DEFAULT, D3D11_USAGE_STAGING,
    D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE, D3D11_VIDEO_PROCESSOR_CONTENT_DESC,
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC, D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC_0,
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC, D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC_0,
    D3D11_VIDEO_PROCESSOR_STREAM, D3D11_VIDEO_USAGE_PLAYBACK_NORMAL,
    D3D11_VPIV_DIMENSION_TEXTURE2D, D3D11_VPOV_DIMENSION_TEXTURE2D, ID3D11Device,
    ID3D11DeviceContext, ID3D11Resource, ID3D11Texture2D, ID3D11VideoDevice,
    ID3D11VideoProcessorEnumerator, ID3D11VideoProcessorInputView, ID3D11VideoProcessorOutputView,
};
use windows::Win32::Graphics::Direct3D11::{ID3D11VideoContext1, ID3D11VideoProcessor};
use windows::Win32::Graphics::Dxgi::Common::{
    DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601,
    DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_NV12, DXGI_RATIONAL, DXGI_SAMPLE_DESC,
};
use windows::core::Interface;

use crate::error::{GpuEncoderError, Result};

/// Sub-rectangle of the source texture to convert; `None` converts the
/// whole texture.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SourceRect {
    pub x: u32,
    pub y: u32,
    pub width: u32,
    pub height: u32,
}

pub(crate) struct VideoProcessor {
    device: ID3D11Device,
    context: ID3D11DeviceContext,
    video_device: ID3D11VideoDevice,
    video_context: ID3D11VideoContext1,
    enumerator: ID3D11VideoProcessorEnumerator,
    processor: ID3D11VideoProcessor,
    nv12_desc: D3D11_TEXTURE2D_DESC,
    output_width: u32,
    output_height: u32,
}

impl VideoProcessor {
    pub(crate) fn new(
        device: &ID3D11Device,
        context: &ID3D11DeviceContext,
        source_width: u32,
        source_height: u32,
        output_width: u32,
        output_height: u32,
        fps: u32,
    ) -> Result<Self> {
        let video_device: ID3D11VideoDevice = device.cast().map_err(|_| {
            GpuEncoderError::NoVideoProcessor("device exposes no ID3D11VideoDevice".into())
        })?;
        let video_context: ID3D11VideoContext1 = context.cast().map_err(|_| {
            GpuEncoderError::NoVideoProcessor("context exposes no ID3D11VideoContext1".into())
        })?;

        let content = D3D11_VIDEO_PROCESSOR_CONTENT_DESC {
            InputFrameFormat: D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE,
            InputFrameRate: DXGI_RATIONAL {
                Numerator: fps,
                Denominator: 1,
            },
            InputWidth: source_width,
            InputHeight: source_height,
            OutputFrameRate: DXGI_RATIONAL {
                Numerator: fps,
                Denominator: 1,
            },
            OutputWidth: output_width,
            OutputHeight: output_height,
            Usage: D3D11_VIDEO_USAGE_PLAYBACK_NORMAL,
        };
        let enumerator =
            unsafe { video_device.CreateVideoProcessorEnumerator(&content) }.map_err(|error| {
                GpuEncoderError::NoVideoProcessor(format!(
                    "CreateVideoProcessorEnumerator failed: {error}"
                ))
            })?;
        let processor =
            unsafe { video_device.CreateVideoProcessor(&enumerator, 0) }.map_err(|error| {
                GpuEncoderError::NoVideoProcessor(format!("CreateVideoProcessor failed: {error}"))
            })?;

        // Full-range sRGB in, BT.601 studio-range YCbCr out: the same
        // conversion the CPU kernel performs, keeping output colors
        // identical between the lanes.
        unsafe {
            video_context.VideoProcessorSetStreamColorSpace1(
                &processor,
                0,
                DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,
            );
            video_context.VideoProcessorSetOutputColorSpace1(
                &processor,
                DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601,
            );
            // Disable driver auto-processing ("enhancements") so the
            // conversion stays deterministic.
            video_context.VideoProcessorSetStreamAutoProcessingMode(&processor, 0, false);
        }

        Ok(Self {
            device: device.clone(),
            context: context.clone(),
            video_device,
            video_context,
            enumerator,
            processor,
            nv12_desc: D3D11_TEXTURE2D_DESC {
                Width: output_width,
                Height: output_height,
                MipLevels: 1,
                ArraySize: 1,
                Format: DXGI_FORMAT_NV12,
                SampleDesc: DXGI_SAMPLE_DESC {
                    Count: 1,
                    Quality: 0,
                },
                Usage: D3D11_USAGE_DEFAULT,
                // Required for ID3D11VideoProcessorOutputView creation.
                BindFlags: D3D11_BIND_RENDER_TARGET.0 as u32,
                CPUAccessFlags: 0,
                MiscFlags: 0,
            },
            output_width,
            output_height,
        })
    }

    pub(crate) fn create_nv12_texture(&self) -> Result<ID3D11Texture2D> {
        let device = &self.device;
        let mut texture = None;
        unsafe { device.CreateTexture2D(&self.nv12_desc, None, Some(&mut texture)) }.map_err(
            |error| {
                GpuEncoderError::NoVideoProcessor(format!("NV12 texture creation failed: {error}"))
            },
        )?;
        texture.ok_or_else(|| {
            GpuEncoderError::NoVideoProcessor("NV12 texture creation returned no texture".into())
        })
    }

    /// Convert `source` (BGRA) into `destination` (NV12), optionally
    /// cropping to `rect` first and scaling to the output dimensions.
    pub(crate) fn convert(
        &self,
        source: &ID3D11Texture2D,
        rect: Option<SourceRect>,
        destination: &ID3D11Texture2D,
    ) -> Result<()> {
        let mut source_desc = D3D11_TEXTURE2D_DESC::default();
        unsafe { source.GetDesc(&mut source_desc) };
        if source_desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM {
            return Err(GpuEncoderError::UnsupportedInputFormat(source_desc.Format));
        }
        let (src_x, src_y, src_w, src_h) = match rect {
            Some(rect) => (rect.x, rect.y, rect.width, rect.height),
            None => (0, 0, source_desc.Width, source_desc.Height),
        };

        let input_desc = D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC {
            FourCC: 0,
            ViewDimension: D3D11_VPIV_DIMENSION_TEXTURE2D,
            Anonymous: D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC_0 {
                Texture2D: D3D11_TEX2D_VPIV {
                    MipSlice: 0,
                    ArraySlice: 0,
                },
            },
        };
        let input_view: ID3D11VideoProcessorInputView = unsafe {
            let mut view = None;
            self.video_device
                .CreateVideoProcessorInputView(
                    source,
                    &self.enumerator,
                    &input_desc,
                    Some(&mut view),
                )
                .map_err(|error| {
                    GpuEncoderError::Convert(format!("input view creation failed: {error}"))
                })?;
            view
        }
        .ok_or_else(|| GpuEncoderError::Convert("input view creation returned none".into()))?;
        let output_desc = D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC {
            ViewDimension: D3D11_VPOV_DIMENSION_TEXTURE2D,
            Anonymous: D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC_0::default(),
        };
        let output_view: ID3D11VideoProcessorOutputView = unsafe {
            let mut view = None;
            self.video_device
                .CreateVideoProcessorOutputView(
                    destination,
                    &self.enumerator,
                    &output_desc,
                    Some(&mut view),
                )
                .map_err(|error| {
                    GpuEncoderError::Convert(format!("output view creation failed: {error}"))
                })?;
            view
        }
        .ok_or_else(|| GpuEncoderError::Convert("output view creation returned none".into()))?;

        unsafe {
            let source_rect = windows::Win32::Foundation::RECT {
                left: src_x as i32,
                top: src_y as i32,
                right: (src_x + src_w) as i32,
                bottom: (src_y + src_h) as i32,
            };
            self.video_context.VideoProcessorSetStreamSourceRect(
                &self.processor,
                0,
                true,
                Some(&source_rect),
            );
            let dest_rect = windows::Win32::Foundation::RECT {
                left: 0,
                top: 0,
                right: self.output_width as i32,
                bottom: self.output_height as i32,
            };
            self.video_context.VideoProcessorSetStreamDestRect(
                &self.processor,
                0,
                true,
                Some(&dest_rect),
            );
            self.video_context
                .VideoProcessorBlt(
                    &self.processor,
                    &output_view,
                    0,
                    &[D3D11_VIDEO_PROCESSOR_STREAM {
                        Enable: windows::core::BOOL::from(true),
                        pInputSurface: std::mem::ManuallyDrop::new(Some(input_view)),
                        ..D3D11_VIDEO_PROCESSOR_STREAM::default()
                    }],
                )
                .map_err(|error| {
                    GpuEncoderError::Convert(format!("VideoProcessorBlt failed: {error}"))
                })?;
        }
        Ok(())
    }

    /// Convert `source` and read the NV12 result back to CPU memory.
    ///
    /// Test and diagnostic helper only — the production path never reads
    /// converted pixels back. Rows are compact (stride equals width) with
    /// interleaved UV chroma.
    pub(crate) fn convert_and_read_back(
        &self,
        source: &ID3D11Texture2D,
        rect: Option<SourceRect>,
    ) -> Result<Nv12Image> {
        let nv12 = self.create_nv12_texture()?;
        self.convert(source, rect, &nv12)?;
        let staging = {
            let desc = D3D11_TEXTURE2D_DESC {
                Usage: D3D11_USAGE_STAGING,
                CPUAccessFlags: D3D11_CPU_ACCESS_READ.0 as u32,
                BindFlags: 0,
                ..self.nv12_desc
            };
            let mut staging = None;
            unsafe { self.device.CreateTexture2D(&desc, None, Some(&mut staging)) }
                .map_err(|error| GpuEncoderError::Convert(format!("staging texture: {error}")))?;
            staging.ok_or_else(|| GpuEncoderError::Convert("no staging texture".into()))?
        };
        let nv12_resource: ID3D11Resource = nv12.cast().map_err(|error| {
            GpuEncoderError::Convert(format!("casting NV12 texture failed: {error}"))
        })?;
        let staging_resource: ID3D11Resource = staging.cast().map_err(|error| {
            GpuEncoderError::Convert(format!("casting staging texture failed: {error}"))
        })?;
        unsafe { self.context.CopyResource(&staging_resource, &nv12_resource) };
        let width = self.output_width as usize;
        let height = self.output_height as usize;
        let mut image = Nv12Image {
            width: self.output_width,
            height: self.output_height,
            y: vec![0u8; width * height],
            uv: vec![0u8; width * height.div_ceil(2)],
        };
        unsafe {
            let mut mapped = D3D11_MAPPED_SUBRESOURCE::default();
            self.context
                .Map(&staging, 0, D3D11_MAP_READ, 0, Some(&mut mapped))
                .map_err(|error| {
                    GpuEncoderError::Convert(format!("mapping NV12 failed: {error}"))
                })?;
            let pitch = mapped.RowPitch as usize;
            let base = mapped.pData.cast::<u8>();
            for row in 0..height {
                image.y[row * width..(row + 1) * width]
                    .copy_from_slice(std::slice::from_raw_parts(base.add(row * pitch), width));
            }
            let chroma_base = base.add(pitch * height);
            for row in 0..height.div_ceil(2) {
                let start = row * width;
                image.uv[start..start + width].copy_from_slice(std::slice::from_raw_parts(
                    chroma_base.add(row * pitch),
                    width,
                ));
            }
            self.context.Unmap(&staging, 0);
        }
        Ok(image)
    }
}

/// CPU copy of one NV12 image, for tests and diagnostics.
#[derive(Clone, Debug)]
pub struct Nv12Image {
    pub width: u32,
    pub height: u32,
    /// Packed luma plane (stride equals width).
    pub y: Vec<u8>,
    /// Packed interleaved chroma plane (stride equals width).
    pub uv: Vec<u8>,
}
