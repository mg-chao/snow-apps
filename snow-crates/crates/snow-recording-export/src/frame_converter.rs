//! Refcounted input for the legacy swscale frame API and its bounded row workers.
use crate::error::{RecordingExportError, Result};
use ffmpeg_next as ffmpeg;

pub(crate) struct FrameConverter {
    context: *mut ffmpeg::ffi::SwsContext,
    input: ffmpeg::frame::Video,
}

impl Drop for FrameConverter {
    fn drop(&mut self) {
        // SAFETY: this converter exclusively owns the context, including failed initialization.
        unsafe {
            ffmpeg::ffi::sws_freeContext(self.context);
        }
    }
}

impl FrameConverter {
    pub(crate) fn use_bt709(&mut self) -> Result<()> {
        unsafe { crate::streaming::configure_bt709_scaler(self.context) }
    }
    #[cfg(any(test, feature = "bench-experiments"))]
    pub(crate) fn thread_count(&self) -> Option<usize> {
        // SAFETY: this converter owns its initialized context.
        unsafe { crate::streaming::swscale_thread_count(self.context) }
    }

    #[cfg(any(test, feature = "bench-experiments"))]
    pub(crate) fn new(
        width: u32,
        height: u32,
        pixel: ffmpeg::format::Pixel,
        threads: u8,
    ) -> Result<Self> {
        // SAFETY: all fields/options are initialized before use; Drop releases the context on error.
        unsafe {
            let context = ffmpeg::ffi::sws_alloc_context();
            if context.is_null() {
                return Err(RecordingExportError::Encode(
                    "failed to allocate frame converter".into(),
                ));
            }
            let mut result = Self {
                context,
                input: ffmpeg::frame::Video::new(ffmpeg::format::Pixel::RGBA, width, height),
            };
            // Initialize the complete allocation, including row and SIMD tail
            // padding, before a retaining converter can observe any byte.
            for buffer in (*result.input.as_mut_ptr()).buf {
                if !buffer.is_null() {
                    std::ptr::write_bytes((*buffer).data, 0, (*buffer).size);
                }
            }
            let rgba: ffmpeg::ffi::AVPixelFormat = ffmpeg::format::Pixel::RGBA.into();
            let output: ffmpeg::ffi::AVPixelFormat = pixel.into();
            for (key, value) in [
                (c"srcw", i64::from(width)),
                (c"srch", i64::from(height)),
                (c"dstw", i64::from(width)),
                (c"dsth", i64::from(height)),
                (c"src_format", rgba as i64),
                (c"dst_format", output as i64),
                (
                    c"sws_flags",
                    i64::from(ffmpeg::software::scaling::Flags::BICUBIC.bits()),
                ),
                (c"threads", i64::from(threads)),
            ] {
                let code = ffmpeg::ffi::av_opt_set_int(context.cast(), key.as_ptr(), value, 0);
                if code < 0 {
                    return Err(RecordingExportError::Encode(format!(
                        "frame converter option {key:?}: {}",
                        ffmpeg::Error::from(code)
                    )));
                }
            }
            let code =
                ffmpeg::ffi::sws_init_context(context, std::ptr::null_mut(), std::ptr::null_mut());
            if code < 0 {
                return Err(RecordingExportError::Encode(format!(
                    "frame converter initialization: {}",
                    ffmpeg::Error::from(code)
                )));
            }
            Ok(result)
        }
    }

    pub(crate) fn ensure_input_writable(&mut self) -> Result<()> {
        // av_frame_make_writable copies visible planes when it replaces shared
        // storage; it does not promise to initialize the new allocation's row
        // or SIMD padding. The caller fills every visible pixel after this call.
        // SAFETY: this converter exclusively owns the input AVFrame object.
        let shared = unsafe { ffmpeg::ffi::av_frame_is_writable(self.input.as_mut_ptr()) == 0 };
        crate::editing::ensure_video_frame_writable(&mut self.input)?;
        if shared {
            // SAFETY: make_writable succeeded, so every buffer is exclusively
            // writable. Initialize all bytes before the retaining API sees it.
            unsafe {
                for buffer in (*self.input.as_mut_ptr()).buf {
                    if !buffer.is_null() {
                        std::ptr::write_bytes((*buffer).data, 0, (*buffer).size);
                    }
                }
            }
        }
        Ok(())
    }

    pub(crate) fn copy_input(&mut self, pixels: &[u8]) -> Result<()> {
        let row_bytes = self.input.width() as usize * 4;
        let height = self.input.height() as usize;
        if pixels.len() < row_bytes * height {
            return Err(RecordingExportError::InvalidConfig(
                "short frame converter input".into(),
            ));
        }
        let stride = self.input.stride(0);
        for (source, destination) in pixels
            .chunks_exact(row_bytes)
            .take(height)
            .zip(self.input.data_mut(0).chunks_mut(stride))
        {
            destination[..row_bytes].copy_from_slice(source);
        }
        Ok(())
    }

    pub(crate) fn convert_prepared(&mut self, output: &mut ffmpeg::frame::Video) -> Result<()> {
        // SAFETY: both AVFrames own refcounted buffers. The initialized legacy
        // context preserves the original color/scaling semantics and dimensions.
        let code = unsafe {
            ffmpeg::ffi::sws_scale_frame(self.context, output.as_mut_ptr(), self.input.as_ptr())
        };
        if code < 0 {
            return Err(RecordingExportError::Encode(format!(
                "frame conversion: {}",
                ffmpeg::Error::from(code)
            )));
        }
        Ok(())
    }

    #[cfg(test)]
    pub(crate) fn convert(
        &mut self,
        pixels: &[u8],
        output: &mut ffmpeg::frame::Video,
    ) -> Result<()> {
        self.ensure_input_writable()?;
        self.copy_input(pixels)?;
        self.convert_prepared(output)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn shared_input_replacement_initializes_padding_and_preserves_retained_storage() {
        ffmpeg::init().unwrap();
        let mut converter = FrameConverter::new(19, 13, ffmpeg::format::Pixel::YUV420P, 2).unwrap();
        converter.copy_input(&[123; 19 * 13 * 4]).unwrap();
        let mut retained = ffmpeg::frame::Video::empty();
        // SAFETY: retain an owning reference in a separate live AVFrame.
        unsafe {
            assert_eq!(
                ffmpeg::ffi::av_frame_ref(retained.as_mut_ptr(), converter.input.as_ptr()),
                0
            );
            assert_eq!(
                ffmpeg::ffi::av_frame_is_writable(converter.input.as_mut_ptr()),
                0
            );
        }
        let previous = retained.data(0).as_ptr();
        converter.ensure_input_writable().unwrap();
        assert_ne!(converter.input.data(0).as_ptr(), previous);
        assert_eq!(retained.data(0)[0], 123);
        // Check the complete FFmpeg allocation, including bytes outside the
        // visible plane. Subsequent copy_input repopulates only visible rows.
        // SAFETY: the converter keeps every inspected AVBufferRef alive.
        unsafe {
            for buffer in (*converter.input.as_ptr()).buf {
                if !buffer.is_null() {
                    let bytes = std::slice::from_raw_parts((*buffer).data, (*buffer).size);
                    assert!(bytes.iter().all(|&byte| byte == 0));
                }
            }
        }
        converter.copy_input(&[42; 19 * 13 * 4]).unwrap();
        for row in converter.input.data(0).chunks(converter.input.stride(0)) {
            assert!(row[..19 * 4].iter().all(|&byte| byte == 42));
            assert!(row[19 * 4..].iter().all(|&byte| byte == 0));
        }
        assert_eq!(retained.data(0)[0], 123);
    }
}
