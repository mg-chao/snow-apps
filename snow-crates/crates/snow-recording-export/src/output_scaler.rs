//! Output conversion shared by all recording export paths. libswscale cannot
//! generate content-adaptive PAL8, so GIF conversion passes through full RGB.
use std::ops::{Deref, DerefMut};

use ffmpeg_next as ffmpeg;

use crate::error::{RecordingExportError, Result};
use crate::gif_palette::GifPalette;

pub(crate) struct OutputScaler {
    scaler: ffmpeg::software::scaling::Context,
    gif: Option<(GifPalette, ffmpeg::frame::Video)>,
}

impl OutputScaler {
    pub(crate) fn get(
        input: ffmpeg::format::Pixel,
        input_width: u32,
        input_height: u32,
        output: ffmpeg::format::Pixel,
        width: u32,
        height: u32,
        flags: ffmpeg::software::scaling::Flags,
    ) -> std::result::Result<Self, ffmpeg::Error> {
        let gif = (output == ffmpeg::format::Pixel::PAL8).then(|| {
            (
                GifPalette::default(),
                ffmpeg::frame::Video::new(ffmpeg::format::Pixel::RGB24, width, height),
            )
        });
        let scaler = ffmpeg::software::scaling::Context::get(
            input,
            input_width,
            input_height,
            if gif.is_some() {
                ffmpeg::format::Pixel::RGB24
            } else {
                output
            },
            width,
            height,
            flags,
        )?;
        Ok(Self { scaler, gif })
    }

    pub(crate) fn run(
        &mut self,
        input: &ffmpeg::frame::Video,
        output: &mut ffmpeg::frame::Video,
    ) -> std::result::Result<(), ffmpeg::Error> {
        if let Some((palette, rgb)) = &mut self.gif {
            self.scaler.run(input, rgb)?;
            palette.convert(rgb, output);
            Ok(())
        } else {
            self.scaler.run(input, output)
        }
    }

    pub(crate) fn run_owned_rgba(
        &mut self,
        input: &[u8],
        output: &mut ffmpeg::frame::Video,
    ) -> Result<()> {
        if let Some((palette, rgb)) = &mut self.gif {
            convert_owned_rgba(&mut self.scaler, input, rgb)?;
            palette.convert(rgb, output);
            Ok(())
        } else {
            convert_owned_rgba(&mut self.scaler, input, output)
        }
    }
}

impl Deref for OutputScaler {
    type Target = ffmpeg::software::scaling::Context;
    fn deref(&self) -> &Self::Target {
        &self.scaler
    }
}

impl DerefMut for OutputScaler {
    fn deref_mut(&mut self) -> &mut Self::Target {
        &mut self.scaler
    }
}

pub(crate) fn convert_owned_rgba(
    scaler: &mut ffmpeg::software::scaling::Context,
    rgba: &[u8],
    output: &mut ffmpeg::frame::Video,
) -> Result<()> {
    let input = scaler.input();
    let stride = input
        .width
        .checked_mul(4)
        .and_then(|value| i32::try_from(value).ok())
        .ok_or_else(|| RecordingExportError::InvalidConfig("RGBA row size overflow".into()))?;
    let expected = stride as usize * input.height as usize;
    if input.format != ffmpeg::format::Pixel::RGBA
        || rgba.len() < expected + ffmpeg::ffi::AV_INPUT_BUFFER_PADDING_SIZE as usize
        || output.format() != scaler.output().format
        || (output.width(), output.height()) != (scaler.output().width, scaler.output().height)
    {
        return Err(RecordingExportError::InvalidConfig(
            "invalid owned RGBA conversion layout".into(),
        ));
    }
    let height = input.height;
    let mut source = [std::ptr::null(); 8];
    source[0] = rgba.as_ptr();
    let mut strides = [0; 8];
    strides[0] = stride;
    // SAFETY: sws_scale is synchronous and does not retain these source pointers.
    // The caller owns the initialized RGBA pixels and SIMD tail padding through
    // this call; each source row has exactly `stride` bytes. The destination is
    // a fully allocated, writable AVFrame matching the scaler output. Encoding
    // references only that separate destination, so RGBA storage can be recycled.
    let rows = unsafe {
        ffmpeg::ffi::sws_scale(
            scaler.as_mut_ptr(),
            source.as_ptr(),
            strides.as_ptr(),
            0,
            height as i32,
            (*output.as_mut_ptr()).data.as_ptr(),
            (*output.as_mut_ptr()).linesize.as_ptr(),
        )
    };
    if rows != output.height() as i32 {
        return Err(RecordingExportError::Encode(format!(
            "owned RGBA conversion returned {rows} rows"
        )));
    }
    Ok(())
}
