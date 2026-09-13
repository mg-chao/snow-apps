use std::mem::ManuallyDrop;

use anyhow::{Context, Result, ensure};
use windows::Win32::Foundation::RECT;
use windows::Win32::Graphics::Direct3D11::*;
use windows::Win32::Graphics::Dxgi::Common::*;
use windows::core::{BOOL, Interface};

use crate::{SharedDevice, Texture};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Rect {
    pub x: i32,
    pub y: i32,
    pub width: u32,
    pub height: u32,
}

impl Rect {
    pub fn full(size: (u32, u32)) -> Self {
        Self {
            x: 0,
            y: 0,
            width: size.0,
            height: size.1,
        }
    }

    fn native(self) -> Result<RECT> {
        let right = i64::from(self.x) + i64::from(self.width);
        let bottom = i64::from(self.y) + i64::from(self.height);
        ensure!(self.width > 0 && self.height > 0, "empty video rectangle");
        Ok(RECT {
            left: self.x,
            top: self.y,
            right: right.try_into()?,
            bottom: bottom.try_into()?,
        })
    }
}

#[derive(Clone)]
pub struct VideoLayer {
    pub texture: Texture,
    pub source: Rect,
    pub destination: Rect,
    /// Clockwise quarter-turns. Source coordinates refer to the unrotated texture.
    pub rotation: u32,
    pub alpha: bool,
}

pub struct VideoProcessor {
    device: SharedDevice,
    video_device: ID3D11VideoDevice,
    context: ID3D11VideoContext1,
    enumerator: ID3D11VideoProcessorEnumerator,
    processor: ID3D11VideoProcessor,
    caps: D3D11_VIDEO_PROCESSOR_CAPS,
    output_size: (u32, u32),
}

impl VideoProcessor {
    pub fn new(
        device: SharedDevice,
        input_size: (u32, u32),
        output_size: (u32, u32),
        fps: u32,
    ) -> Result<Self> {
        ensure!(fps > 0, "video processor needs a positive frame rate");
        let video_device: ID3D11VideoDevice = device.device().cast()?;
        let context: ID3D11VideoContext1 = device.context().cast()?;
        let rate = DXGI_RATIONAL {
            Numerator: fps,
            Denominator: 1,
        };
        let desc = D3D11_VIDEO_PROCESSOR_CONTENT_DESC {
            InputFrameFormat: D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE,
            InputFrameRate: rate,
            InputWidth: input_size.0,
            InputHeight: input_size.1,
            OutputFrameRate: rate,
            OutputWidth: output_size.0,
            OutputHeight: output_size.1,
            Usage: D3D11_VIDEO_USAGE_PLAYBACK_NORMAL,
        };
        let enumerator = unsafe { video_device.CreateVideoProcessorEnumerator(&desc) }?;
        let mut caps = D3D11_VIDEO_PROCESSOR_CAPS::default();
        unsafe { enumerator.GetVideoProcessorCaps(&mut caps) }?;
        ensure!(
            caps.MaxInputStreams >= 2,
            "video processor cannot composite two streams"
        );
        ensure!(
            caps.FeatureCaps & D3D11_VIDEO_PROCESSOR_FEATURE_CAPS_ALPHA_STREAM.0 as u32 != 0,
            "video processor lacks alpha composition"
        );
        let processor = unsafe { video_device.CreateVideoProcessor(&enumerator, 0) }?;
        let result = Self {
            device,
            video_device,
            context,
            enumerator,
            processor,
            caps,
            output_size,
        };
        result.check_conversion(DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM)?;
        result.check_conversion(DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_NV12)?;
        Ok(result)
    }

    pub fn max_streams(&self) -> usize {
        self.caps.MaxInputStreams as usize
    }

    pub fn check_conversion(&self, input: DXGI_FORMAT, output: DXGI_FORMAT) -> Result<()> {
        let enumerator: ID3D11VideoProcessorEnumerator1 = self.enumerator.cast()?;
        ensure!(
            unsafe {
                enumerator.CheckVideoProcessorFormatConversion(
                    input,
                    DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,
                    output,
                    output_color_space(output),
                )
            }?
            .as_bool(),
            "unsupported GPU video format conversion: {input:?} to {output:?}"
        );
        Ok(())
    }

    /// Output may be an FFmpeg-owned NV12 texture. No intermediate upload or
    /// transfer is performed: the video processor writes the encoder surface.
    pub fn blit(&self, layers: &[VideoLayer], output: &ID3D11Texture2D, slice: u32) -> Result<()> {
        ensure!(
            layers.len() <= self.max_streams(),
            "too many video processor streams"
        );
        let _lock = self.device.lock();
        let owner = unsafe { output.GetDevice() }?;
        ensure!(
            owner == *self.device.device(),
            "video output belongs to a different device"
        );
        let mut output_desc = D3D11_TEXTURE2D_DESC::default();
        unsafe { output.GetDesc(&mut output_desc) };
        ensure!(
            output_desc.Width >= self.output_size.0
                && output_desc.Height >= self.output_size.1
                && slice < output_desc.ArraySize,
            "invalid video processor output dimensions"
        );
        let output_view_desc = if output_desc.ArraySize == 1 {
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC {
                ViewDimension: D3D11_VPOV_DIMENSION_TEXTURE2D,
                Anonymous: D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC_0 {
                    Texture2D: D3D11_TEX2D_VPOV { MipSlice: 0 },
                },
            }
        } else {
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC {
                ViewDimension: D3D11_VPOV_DIMENSION_TEXTURE2DARRAY,
                Anonymous: D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC_0 {
                    Texture2DArray: D3D11_TEX2D_ARRAY_VPOV {
                        MipSlice: 0,
                        FirstArraySlice: slice,
                        ArraySize: 1,
                    },
                },
            }
        };
        let mut output_view = None;
        unsafe {
            self.video_device.CreateVideoProcessorOutputView(
                output,
                &self.enumerator,
                &output_view_desc,
                Some(&mut output_view),
            )
        }
        .context("create VideoProcessor output view")?;
        let mut views = Vec::with_capacity(layers.len());
        for (index, layer) in layers.iter().enumerate() {
            ensure!(
                self.device.same_device(layer.texture.device()),
                "video input belongs to a different device"
            );
            ensure!(layer.rotation < 4, "invalid video rotation");
            ensure!(
                layer.rotation == 0
                    || self.caps.FeatureCaps & D3D11_VIDEO_PROCESSOR_FEATURE_CAPS_ROTATION.0 as u32
                        != 0,
                "video processor lacks rotation support"
            );
            self.check_conversion(layer.texture.desc().Format, output_desc.Format)?;
            let source = layer.source.native()?;
            let destination = layer.destination.native()?;
            let input_desc = D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC {
                ViewDimension: D3D11_VPIV_DIMENSION_TEXTURE2D,
                ..Default::default()
            };
            let mut view = None;
            unsafe {
                self.video_device.CreateVideoProcessorInputView(
                    layer.texture.raw(),
                    &self.enumerator,
                    &input_desc,
                    Some(&mut view),
                )
            }
            .with_context(|| {
                format!(
                    "create VideoProcessor input view for {:?}, {:?}",
                    layer.texture.desc().Format,
                    layer.texture.dimensions()
                )
            })?;
            views.push(view.context("missing video processor input view")?);
            let index = index as u32;
            unsafe {
                self.context.VideoProcessorSetStreamFrameFormat(
                    &self.processor,
                    index,
                    D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE,
                );
                self.context.VideoProcessorSetStreamAutoProcessingMode(
                    &self.processor,
                    index,
                    false,
                );
                self.context.VideoProcessorSetStreamColorSpace1(
                    &self.processor,
                    index,
                    DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,
                );
                self.context.VideoProcessorSetStreamSourceRect(
                    &self.processor,
                    index,
                    true,
                    Some(&source),
                );
                self.context.VideoProcessorSetStreamDestRect(
                    &self.processor,
                    index,
                    true,
                    Some(&destination),
                );
                self.context
                    .VideoProcessorSetStreamAlpha(&self.processor, index, layer.alpha, 1.0);
                self.context.VideoProcessorSetStreamRotation(
                    &self.processor,
                    index,
                    layer.rotation != 0,
                    D3D11_VIDEO_PROCESSOR_ROTATION(layer.rotation as i32),
                );
            }
        }
        let target = Rect::full(self.output_size).native()?;
        unsafe {
            self.context
                .VideoProcessorSetOutputTargetRect(&self.processor, true, Some(&target));
            self.context.VideoProcessorSetOutputColorSpace1(
                &self.processor,
                output_color_space(output_desc.Format),
            );
            self.context.VideoProcessorSetOutputBackgroundColor(
                &self.processor,
                false,
                &D3D11_VIDEO_COLOR {
                    Anonymous: D3D11_VIDEO_COLOR_0 {
                        RGBA: D3D11_VIDEO_COLOR_RGBA {
                            R: 0.0,
                            G: 0.0,
                            B: 0.0,
                            A: 1.0,
                        },
                    },
                },
            );
            self.context.VideoProcessorSetOutputAlphaFillMode(
                &self.processor,
                D3D11_VIDEO_PROCESSOR_ALPHA_FILL_MODE_OPAQUE,
                0,
            );
        }
        let mut streams: Vec<_> = views
            .into_iter()
            .map(|view| D3D11_VIDEO_PROCESSOR_STREAM {
                Enable: BOOL(1),
                pInputSurface: ManuallyDrop::new(Some(view)),
                ..Default::default()
            })
            .collect();
        let result = unsafe {
            self.context.VideoProcessorBlt(
                &self.processor,
                output_view
                    .as_ref()
                    .context("missing video processor output view")?,
                0,
                &streams,
            )
        };
        // windows-rs exposes COM-containing C structs with ManuallyDrop fields.
        for stream in &mut streams {
            unsafe { ManuallyDrop::drop(&mut stream.pInputSurface) };
        }
        result.context("VideoProcessorBlt failed")?;
        self.device.check()
    }
}

fn output_color_space(format: DXGI_FORMAT) -> DXGI_COLOR_SPACE_TYPE {
    if format == DXGI_FORMAT_NV12 {
        DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709
    } else {
        DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn rectangles_reject_overflow_and_empty_extents() {
        assert!(
            Rect {
                x: i32::MAX,
                y: 0,
                width: 1,
                height: 1
            }
            .native()
            .is_err()
        );
        assert!(Rect::full((0, 1)).native().is_err());
        let rect = Rect {
            x: -100,
            y: -40,
            width: 300,
            height: 90,
        }
        .native()
        .unwrap();
        assert_eq!(
            (rect.left, rect.top, rect.right, rect.bottom),
            (-100, -40, 200, 50)
        );
    }

    #[test]
    #[ignore = "requires a D3D11 hardware video processor"]
    fn hardware_composition_preserves_geometry_alpha_and_bt709_range() -> Result<()> {
        use windows::Win32::Graphics::Dxgi::{CreateDXGIFactory1, IDXGIFactory1};
        let factory: IDXGIFactory1 = unsafe { CreateDXGIFactory1() }?;
        let adapter = unsafe { factory.EnumAdapters1(0) }?.cast()?;
        let device = SharedDevice::create(&adapter)?;
        let _lock = device.lock();
        let processor = VideoProcessor::new(device.clone(), (64, 64), (64, 64), 60)?;
        let binds = (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE).0 as u32;
        let solid = |color: [u8; 4]| -> Result<Texture> {
            let texture = device.texture(64, 64, DXGI_FORMAT_B8G8R8A8_UNORM, binds)?;
            let pixels = color.repeat(64 * 64);
            unsafe {
                device.context().UpdateSubresource(
                    texture.raw(),
                    0,
                    None,
                    pixels.as_ptr().cast(),
                    64 * 4,
                    0,
                );
            }
            Ok(texture)
        };
        let output = device.texture(64, 64, DXGI_FORMAT_B8G8R8X8_UNORM, binds)?;
        let background = VideoLayer {
            texture: solid([0, 0, 255, 255])?,
            source: Rect::full((64, 64)),
            destination: Rect::full((64, 64)),
            rotation: 0,
            alpha: false,
        };
        let overlay = VideoLayer {
            texture: solid([255, 0, 0, 128])?,
            source: Rect::full((64, 64)),
            destination: Rect {
                x: 16,
                y: 16,
                width: 32,
                height: 32,
            },
            rotation: 0,
            alpha: true,
        };
        processor.blit(&[background.clone(), overlay], output.raw(), 0)?;
        let read = |texture: &Texture| -> Result<Vec<u8>> {
            let mut desc = texture.desc();
            desc.BindFlags = 0;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ.0 as u32;
            let mut staging = None;
            unsafe {
                device
                    .device()
                    .CreateTexture2D(&desc, None, Some(&mut staging))
            }?;
            let staging = staging.context("staging")?;
            unsafe {
                device.context().CopyResource(&staging, texture.raw());
            }
            let mut mapped = D3D11_MAPPED_SUBRESOURCE::default();
            unsafe {
                device
                    .context()
                    .Map(&staging, 0, D3D11_MAP_READ, 0, Some(&mut mapped))
            }?;
            let bytes_per_row = if desc.Format == DXGI_FORMAT_NV12 {
                64
            } else {
                256
            };
            let rows = if desc.Format == DXGI_FORMAT_NV12 {
                96
            } else {
                64
            };
            let mut result = Vec::new();
            for row in 0..rows {
                result.extend_from_slice(unsafe {
                    std::slice::from_raw_parts(
                        mapped
                            .pData
                            .cast::<u8>()
                            .add(row * mapped.RowPitch as usize),
                        bytes_per_row,
                    )
                });
            }
            unsafe {
                device.context().Unmap(&staging, 0);
            }
            Ok(result)
        };
        let pixels = read(&output)?;
        for y in 0..64 {
            for x in 0..64 {
                let pixel = &pixels[(y * 64 + x) * 4..(y * 64 + x + 1) * 4];
                let expected = if (16..48).contains(&x) && (16..48).contains(&y) {
                    [128u8, 0, 127, 255]
                } else {
                    [0, 0, 255, 255]
                };
                ensure!(
                    pixel[..3]
                        .iter()
                        .zip(expected[..3].iter().copied())
                        .all(|(actual, expected)| actual.abs_diff(expected) <= 2),
                    "alpha/geometry mismatch at {x},{y}: {pixel:?}, expected {expected:?}"
                );
            }
        }
        // Two displays join at x=16, with a negative clipped edge and uncovered
        // desktop to the right and below. Constant colors make geometry exact.
        let mut left = background.clone();
        left.destination = Rect {
            x: -16,
            y: 0,
            width: 32,
            height: 32,
        };
        let mut right = background.clone();
        right.texture = solid([0, 255, 0, 255])?;
        right.destination = Rect {
            x: 16,
            y: 0,
            width: 32,
            height: 32,
        };
        processor.blit(&[left, right], output.raw(), 0)?;
        let pixels = read(&output)?;
        for y in 0..64 {
            for x in 0..64 {
                let expected: [u8; 3] = if y >= 32 || x >= 48 {
                    [0, 0, 0]
                } else if x < 16 {
                    [0, 0, 255]
                } else {
                    [0, 255, 0]
                };
                assert_eq!(
                    &pixels[(y * 64 + x) * 4..][..3],
                    &expected,
                    "join at {x},{y}"
                );
            }
        }
        // Rotated source quadrants retain their exact positions.
        let quadrants = solid([0, 0, 0, 255])?;
        let colors = [
            [0u8, 0, 255, 255],
            [0, 255, 0, 255],
            [255, 0, 0, 255],
            [255, 255, 255, 255],
        ];
        let mut bytes = Vec::<u8>::new();
        for y in 0..64 {
            for x in 0..64 {
                bytes.extend_from_slice(&colors[(y / 32) * 2 + x / 32]);
            }
        }
        unsafe {
            device.context().UpdateSubresource(
                quadrants.raw(),
                0,
                None,
                bytes.as_ptr().cast(),
                256,
                0,
            );
        }
        for rotation in 0..4 {
            let mut layer = background.clone();
            layer.texture = quadrants.clone();
            layer.rotation = rotation;
            processor.blit(&[layer], output.raw(), 0)?;
            let pixels = read(&output)?;
            for (x, y) in [(16, 16), (48, 16), (16, 48), (48, 48)] {
                let (sx, sy) = match rotation {
                    0 => (x, y),
                    1 => (y, 63 - x),
                    2 => (63 - x, 63 - y),
                    _ => (63 - y, x),
                };
                assert_eq!(
                    &pixels[(y * 64 + x) * 4..][..3],
                    &colors[(sy / 32) * 2 + sx / 32][..3],
                    "rotation {rotation}"
                );
            }
        }
        let nv12 = device.texture(64, 64, DXGI_FORMAT_NV12, D3D11_BIND_RENDER_TARGET.0 as u32)?;
        for (bgra, yuv) in [
            ([0, 0, 0, 255], [16u8, 128, 128]),
            ([255, 255, 255, 255], [235, 128, 128]),
            ([0, 0, 255, 255], [63, 102, 240]),
            ([0, 255, 0, 255], [173, 42, 26]),
            ([255, 0, 0, 255], [32, 240, 118]),
        ] {
            let mut layer = background.clone();
            layer.texture = solid(bgra)?;
            processor.blit(&[layer], nv12.raw(), 0)?;
            let pixels = read(&nv12)?;
            ensure!(
                pixels[..4096]
                    .iter()
                    .all(|value| value.abs_diff(yuv[0]) <= 2),
                "BT.709 luma mismatch for {bgra:?}: {}",
                pixels[0]
            );
            ensure!(
                pixels[4096..]
                    .chunks_exact(2)
                    .all(|uv| uv[0].abs_diff(yuv[1]) <= 2 && uv[1].abs_diff(yuv[2]) <= 2),
                "BT.709 chroma mismatch for {bgra:?}: {:?}",
                &pixels[4096..4098]
            );
        }
        let gradient = solid([0, 0, 0, 255])?;
        let mut bytes = Vec::<u8>::new();
        for _y in 0..64 {
            for x in 0..64 {
                bytes.extend_from_slice(&[x * 4, x * 4, x * 4, 255]);
            }
        }
        unsafe {
            device.context().UpdateSubresource(
                gradient.raw(),
                0,
                None,
                bytes.as_ptr().cast(),
                256,
                0,
            );
        }
        let mut layer = background;
        layer.texture = gradient;
        processor.blit(&[layer], nv12.raw(), 0)?;
        let pixels = read(&nv12)?;
        for y in 0..64 {
            for x in 0..64 {
                let expected = (16.0 + 219.0 * (x * 4) as f64 / 255.0).round() as u8;
                ensure!(
                    pixels[y * 64 + x].abs_diff(expected) <= 2,
                    "gradient at {x},{y}"
                );
            }
        }
        ensure!(
            pixels[4096..].iter().all(|value| value.abs_diff(128) <= 2),
            "neutral gradient chroma"
        );
        Ok(())
    }
}
