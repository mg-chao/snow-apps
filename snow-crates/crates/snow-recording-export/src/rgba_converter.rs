use std::ptr::NonNull;

use ffmpeg_next as ffmpeg;

/// Uses libswscale's frame API so conversion can use its native slice workers.
/// The slice-based `Context::run` API executes on the calling thread only.
pub(crate) struct RgbaConverter {
    context: NonNull<ffmpeg::ffi::SwsContext>,
    width: u32,
    height: u32,
    format: ffmpeg::format::Pixel,
}

impl RgbaConverter {
    pub(crate) fn new(
        width: u32,
        height: u32,
        format: ffmpeg::format::Pixel,
        threads: usize,
    ) -> Result<Self, ffmpeg::Error> {
        // SAFETY: the context is owned here and released by Drop, including on initialization errors.
        let context =
            NonNull::new(unsafe { ffmpeg::ffi::sws_alloc_context() }).ok_or(ffmpeg::Error::Bug)?;
        let result = Self {
            context,
            width,
            height,
            format,
        };
        let source_format: ffmpeg::ffi::AVPixelFormat = ffmpeg::format::Pixel::RGBA.into();
        let output_format: ffmpeg::ffi::AVPixelFormat = format.into();
        for (name, value) in [
            (c"srcw", i64::from(width)),
            (c"srch", i64::from(height)),
            (c"dstw", i64::from(width)),
            (c"dsth", i64::from(height)),
            (c"src_format", source_format as i64),
            (c"dst_format", output_format as i64),
            (
                c"sws_flags",
                ffmpeg::software::scaling::Flags::BICUBIC.bits() as i64,
            ),
            (c"threads", threads.clamp(1, 16) as i64),
        ] {
            // SAFETY: SwsContext is an AVOptions object; the static option names are NUL terminated.
            let code = unsafe {
                ffmpeg::ffi::av_opt_set_int(context.as_ptr().cast(), name.as_ptr(), value, 0)
            };
            if code < 0 {
                return Err(ffmpeg::Error::from(code));
            }
        }
        // Explicit initialization retains the legacy scaler's pixel/color conversion semantics.
        let code = unsafe {
            ffmpeg::ffi::sws_init_context(
                context.as_ptr(),
                std::ptr::null_mut(),
                std::ptr::null_mut(),
            )
        };
        if code < 0 {
            return Err(ffmpeg::Error::from(code));
        }
        Ok(result)
    }

    pub(crate) fn run(
        &mut self,
        source: &ffmpeg::frame::Video,
        output: &mut ffmpeg::frame::Video,
    ) -> Result<(), ffmpeg::Error> {
        if source.format() != ffmpeg::format::Pixel::RGBA
            || source.width() != self.width
            || source.height() != self.height
        {
            return Err(ffmpeg::Error::InputChanged);
        }
        if output.format() != self.format
            || output.width() != self.width
            || output.height() != self.height
        {
            return Err(ffmpeg::Error::OutputChanged);
        }
        // SAFETY: both AVFrames have owned, refcounted storage and validated dimensions/formats.
        // The synchronous frame call waits for its workers and releases frame references before returning.
        let code = unsafe {
            ffmpeg::ffi::sws_scale_frame(
                self.context.as_ptr(),
                output.as_mut_ptr(),
                source.as_ptr(),
            )
        };
        if code < 0 {
            return Err(ffmpeg::Error::from(code));
        }
        Ok(())
    }
}

impl Drop for RgbaConverter {
    fn drop(&mut self) {
        // SAFETY: this wrapper is the only owner and calls cannot outlive it.
        unsafe {
            ffmpeg::ffi::sws_freeContext(self.context.as_ptr());
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn threaded_conversion_matches_legacy_pixels_including_chroma_and_edges() {
        ffmpeg::init().unwrap();
        for (width, height) in [(2, 2), (18, 18), (130, 74), (1920, 1080)] {
            for format in [ffmpeg::format::Pixel::NV12, ffmpeg::format::Pixel::YUV420P] {
                let mut source =
                    ffmpeg::frame::Video::new(ffmpeg::format::Pixel::RGBA, width, height);
                for (index, value) in source.data_mut(0).iter_mut().enumerate() {
                    *value = (index ^ (index >> 7)) as u8;
                }
                let mut expected = ffmpeg::frame::Video::new(format, width, height);
                let mut actual = ffmpeg::frame::Video::new(format, width, height);
                let mut legacy = ffmpeg::software::scaling::Context::get(
                    ffmpeg::format::Pixel::RGBA,
                    width,
                    height,
                    format,
                    width,
                    height,
                    ffmpeg::software::scaling::Flags::BICUBIC,
                )
                .unwrap();
                legacy.run(&source, &mut expected).unwrap();
                let mut converter = RgbaConverter::new(width, height, format, 4).unwrap();
                for _ in 0..2 {
                    converter.run(&source, &mut actual).unwrap();
                    for plane in 0..actual.planes() {
                        let row_bytes = if plane == 0 || format == ffmpeg::format::Pixel::NV12 {
                            width as usize
                        } else {
                            width as usize / 2
                        };
                        for (left, right) in actual
                            .data(plane)
                            .chunks(actual.stride(plane))
                            .zip(expected.data(plane).chunks(expected.stride(plane)))
                        {
                            assert_eq!(
                                &left[..row_bytes],
                                &right[..row_bytes],
                                "{width}x{height} {format:?} plane {plane}"
                            );
                        }
                    }
                }
            }
        }
    }
}
