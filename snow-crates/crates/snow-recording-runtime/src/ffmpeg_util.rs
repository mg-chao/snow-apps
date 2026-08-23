//! Shared FFmpeg helpers used by both `recording` and `editing`.

use std::ptr;
use std::sync::OnceLock;

use ffmpeg_next as ffmpeg;

use crate::error::{Result, ScreenRecorderError};

/// Initialize FFmpeg exactly once. Thread-safe via `OnceLock`.
pub(crate) fn ensure_ffmpeg_initialized() -> Result<()> {
    static INIT: OnceLock<std::result::Result<(), String>> = OnceLock::new();
    INIT.get_or_init(|| ffmpeg::init().map_err(|err| err.to_string()))
        .clone()
        .map_err(|err| ScreenRecorderError::Encode(format!("failed to initialize ffmpeg: {err}")))
}

/// Check whether an FFmpeg error is EAGAIN.
pub(crate) fn is_eagain(err: &ffmpeg::Error) -> bool {
    matches!(
        err,
        ffmpeg::Error::Other { errno } if *errno == ffmpeg::error::EAGAIN
    )
}

/// Detach a video frame from buffers still retained by an FFmpeg codec.
///
/// `avcodec_send_frame` is allowed to keep references to the submitted
/// buffers. Calling this before the next write preserves the previous frame
/// while reusing the `AVFrame` wrapper and only allocates when it is shared.
pub(crate) fn ensure_video_frame_writable(frame: &mut ffmpeg::frame::Video) -> Result<()> {
    let status = unsafe { ffmpeg::ffi::av_frame_make_writable(frame.as_mut_ptr()) };
    if status < 0 {
        return Err(ScreenRecorderError::Encode(format!(
            "failed to make video frame writable: {}",
            ffmpeg::Error::from(status)
        )));
    }
    Ok(())
}

/// Copy an RGBA buffer into an FFmpeg video frame, respecting stride.
pub(crate) fn copy_rgba_into_frame(frame: &mut ffmpeg::frame::Video, width: u32, rgba: &[u8]) {
    let stride = frame.stride(0);
    let row_bytes = width as usize * 4;
    let height = frame.height() as usize;
    let dst = frame.data_mut(0);

    let packed_len = row_bytes.saturating_mul(height);
    if stride == row_bytes && dst.len() >= packed_len && rgba.len() >= packed_len {
        dst[..packed_len].copy_from_slice(&rgba[..packed_len]);
        return;
    }

    for y in 0..height {
        let src_start = y * row_bytes;
        let dst_start = y * stride;
        // SAFETY:
        // - `src_start + row_bytes` and `dst_start + row_bytes` stay in-bounds by loop construction.
        // - Source and destination buffers are distinct (caller-owned RGBA and FFmpeg frame storage).
        unsafe {
            ptr::copy_nonoverlapping(
                rgba.as_ptr().add(src_start),
                dst.as_mut_ptr().add(dst_start),
                row_bytes,
            );
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn making_a_retained_frame_writable_preserves_submitted_pixels() {
        let mut frame = ffmpeg::frame::Video::new(ffmpeg::format::Pixel::YUV420P, 16, 16);
        frame.data_mut(0).fill(0x11);

        // Model the reference an encoder may retain after avcodec_send_frame.
        let mut retained = ffmpeg::frame::Video::empty();
        let ref_status =
            unsafe { ffmpeg::ffi::av_frame_ref(retained.as_mut_ptr(), frame.as_ptr()) };
        assert_eq!(ref_status, 0);

        ensure_video_frame_writable(&mut frame).unwrap();
        frame.data_mut(0).fill(0xee);

        assert!(retained.data(0).iter().all(|byte| *byte == 0x11));
        assert!(frame.data(0).iter().all(|byte| *byte == 0xee));
    }
}
