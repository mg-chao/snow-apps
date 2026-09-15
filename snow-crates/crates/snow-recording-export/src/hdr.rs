//! Color conversion at the editable-video boundary.
use crate::error::{RecordingExportError, Result};
use ffmpeg_next as ffmpeg;
use rayon::prelude::*;

pub(crate) fn pixel_format(codec: ffmpeg::codec::Video) -> Result<ffmpeg::format::Pixel> {
    use ffmpeg::format::Pixel;
    let formats: Vec<_> = codec.formats().map(|f| f.collect()).unwrap_or_default();
    [Pixel::P010LE, Pixel::YUV420P10LE]
        .into_iter()
        .find(|format| formats.contains(format))
        .ok_or_else(|| {
            RecordingExportError::InvalidConfig(
                "selected HEVC encoder does not support 10-bit HDR input".into(),
            )
        })
}
pub(crate) fn context(encoder: &mut ffmpeg::codec::encoder::video::Video, hdr: bool) {
    unsafe {
        let c = encoder.as_mut_ptr();
        (*c).color_range = ffmpeg::ffi::AVColorRange::AVCOL_RANGE_MPEG;
        (*c).colorspace = if hdr {
            ffmpeg::ffi::AVColorSpace::AVCOL_SPC_BT2020_NCL
        } else {
            ffmpeg::ffi::AVColorSpace::AVCOL_SPC_BT709
        };
        (*c).color_primaries = if hdr {
            ffmpeg::ffi::AVColorPrimaries::AVCOL_PRI_BT2020
        } else {
            ffmpeg::ffi::AVColorPrimaries::AVCOL_PRI_BT709
        };
        (*c).color_trc = if hdr {
            ffmpeg::ffi::AVColorTransferCharacteristic::AVCOL_TRC_SMPTE2084
        } else {
            ffmpeg::ffi::AVColorTransferCharacteristic::AVCOL_TRC_IEC61966_2_1
        };
        if hdr {
            (*c).profile = 2;
            (*c).codec_tag = u32::from_le_bytes(*b"hvc1");
        }
    }
}
pub(crate) fn is_rgb(format: ffmpeg::format::Pixel) -> bool {
    unsafe {
        let desc = ffmpeg::ffi::av_pix_fmt_desc_get(format.into());
        !desc.is_null()
            && (*desc).flags
                & (ffmpeg::ffi::AV_PIX_FMT_FLAG_RGB as u64
                    | ffmpeg::ffi::AV_PIX_FMT_FLAG_PAL as u64)
                != 0
    }
}
pub(crate) fn frame(frame: &mut ffmpeg::frame::Video, hdr: bool) {
    unsafe {
        let f = frame.as_mut_ptr();
        (*f).color_range = if is_rgb(frame.format()) {
            ffmpeg::ffi::AVColorRange::AVCOL_RANGE_JPEG
        } else {
            ffmpeg::ffi::AVColorRange::AVCOL_RANGE_MPEG
        };
        (*f).colorspace = if hdr {
            ffmpeg::ffi::AVColorSpace::AVCOL_SPC_BT2020_NCL
        } else {
            ffmpeg::ffi::AVColorSpace::AVCOL_SPC_BT709
        };
        (*f).color_primaries = if hdr {
            ffmpeg::ffi::AVColorPrimaries::AVCOL_PRI_BT2020
        } else {
            ffmpeg::ffi::AVColorPrimaries::AVCOL_PRI_BT709
        };
        (*f).color_trc = if hdr {
            ffmpeg::ffi::AVColorTransferCharacteristic::AVCOL_TRC_SMPTE2084
        } else {
            ffmpeg::ffi::AVColorTransferCharacteristic::AVCOL_TRC_IEC61966_2_1
        };
    }
}
pub(crate) fn scaler_colors(
    context: &mut ffmpeg::software::scaling::Context,
    source_hdr: bool,
    rgb_output: bool,
) -> Result<()> {
    unsafe {
        let coefficients = ffmpeg::ffi::sws_getCoefficients(if source_hdr {
            ffmpeg::ffi::SWS_CS_BT2020
        } else {
            ffmpeg::ffi::SWS_CS_ITU709
        });
        let status = ffmpeg::ffi::sws_setColorspaceDetails(
            context.as_mut_ptr(),
            coefficients,
            i32::from(!source_hdr || is_rgb(context.input().format)),
            coefficients,
            i32::from(rgb_output),
            0,
            1 << 16,
            1 << 16,
        );
        if status < 0 {
            return Err(RecordingExportError::Export(
                "failed to configure explicit video color conversion".into(),
            ));
        }
    }
    Ok(())
}

pub(crate) struct ToneMapper {
    conversion: Option<ffmpeg::software::scaling::Context>,
    rgb: ffmpeg::frame::Video,
    rgba: ffmpeg::frame::Video,
}
impl Default for ToneMapper {
    fn default() -> Self {
        Self {
            conversion: None,
            rgb: ffmpeg::frame::Video::empty(),
            rgba: ffmpeg::frame::Video::empty(),
        }
    }
}
impl ToneMapper {
    pub(crate) fn convert(
        &mut self,
        source: &ffmpeg::frame::Video,
    ) -> Result<&mut ffmpeg::frame::Video> {
        let input = (source.format(), source.width(), source.height());
        if self
            .conversion
            .as_ref()
            .is_none_or(|c| (c.input().format, c.input().width, c.input().height) != input)
        {
            let mut conversion = ffmpeg::software::scaling::Context::get(
                input.0,
                input.1,
                input.2,
                ffmpeg::format::Pixel::RGB48LE,
                input.1,
                input.2,
                ffmpeg::software::scaling::Flags::BICUBIC,
            )
            .map_err(|e| RecordingExportError::Export(format!("HDR RGB conversion: {e}")))?;
            scaler_colors(&mut conversion, true, true)?;
            self.conversion = Some(conversion);
            self.rgb = ffmpeg::frame::Video::new(ffmpeg::format::Pixel::RGB48LE, input.1, input.2);
            self.rgba = ffmpeg::frame::Video::new(ffmpeg::format::Pixel::RGBA, input.1, input.2);
        }
        // The encoder may retain a previous output; make the reused frame writable.
        if unsafe { ffmpeg::ffi::av_frame_make_writable(self.rgba.as_mut_ptr()) } < 0 {
            return Err(RecordingExportError::Export(
                "tone-map allocation failed".into(),
            ));
        }
        self.conversion
            .as_mut()
            .unwrap()
            .run(source, &mut self.rgb)
            .map_err(|e| RecordingExportError::Export(format!("HDR RGB conversion: {e}")))?;
        let source_stride = self.rgb.stride(0);
        let output_stride = self.rgba.stride(0);
        self.rgba
            .data_mut(0)
            .par_chunks_mut(output_stride)
            .zip(self.rgb.data(0).par_chunks(source_stride))
            .take(input.2 as usize)
            .try_for_each(|(dst, src)| {
                snow_media::color::tone_map_rgb48_row(
                    &src[..input.1 as usize * 6],
                    &mut dst[..input.1 as usize * 4],
                )
            })
            .map_err(|e| RecordingExportError::Export(e.into()))?;
        frame(&mut self.rgba, false);
        unsafe {
            (*self.rgba.as_mut_ptr()).color_range = ffmpeg::ffi::AVColorRange::AVCOL_RANGE_JPEG;
        }
        Ok(&mut self.rgba)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn padded_hdr_rows_are_tone_mapped_without_touching_padding() {
        ffmpeg::init().unwrap();
        let mut source = ffmpeg::frame::Video::new(ffmpeg::format::Pixel::RGB48LE, 3, 2);
        let stride = source.stride(0);
        for (y, row) in source.data_mut(0).chunks_mut(stride).take(2).enumerate() {
            row.fill(0xAA);
            for pixel in row[..18].chunks_mut(6) {
                let value = if y == 0 { 0u16 } else { 32768 };
                for channel in pixel.chunks_mut(2) {
                    channel.copy_from_slice(&value.to_le_bytes());
                }
            }
        }
        let mut mapper = ToneMapper::default();
        let result = mapper.convert(&source).unwrap();
        assert_eq!(&result.data(0)[..4], &[0, 0, 0, 255]);
        let expected = snow_media::color::tone_map_pq([32768.0 / 65535.0; 3]);
        assert_eq!(
            &result.data(0)[result.stride(0)..result.stride(0) + 4],
            &expected
        );
        assert_eq!(source.data(0)[18], 0xAA);
    }
}
