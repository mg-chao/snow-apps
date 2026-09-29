//! Shared FFmpeg helpers used by both `recording` and `editing`.

use std::collections::VecDeque;
use std::ptr;
use std::sync::OnceLock;

use ffmpeg_next as ffmpeg;

use crate::error::{RecordingExportError as ScreenRecorderError, Result};

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

/// Durations are submitted in presentation order but consumed in decode order.
#[derive(Default)]
pub(crate) struct VideoPacketDurations {
    pending: VecDeque<(i64, i64)>,
    first_pts: Option<i64>,
    first_dts: Option<i64>,
    end_pts: i64,
}

impl VideoPacketDurations {
    pub(crate) fn push_back(&mut self, (pts, duration): (i64, i64)) {
        self.first_pts.get_or_insert(pts);
        self.end_pts = self.end_pts.max(pts.saturating_add(duration));
        self.pending.push_back((pts, duration));
    }

    #[cfg(test)]
    pub(crate) fn len(&self) -> usize {
        self.pending.len()
    }

    #[cfg(test)]
    pub(crate) fn is_empty(&self) -> bool {
        self.pending.is_empty()
    }

    pub(crate) fn apply(&mut self, packet: &mut ffmpeg::Packet, draining: bool) {
        let index = packet.pts().map_or(Some(0), |pts| {
            self.pending.iter().position(|(at, _)| *at == pts)
        });
        if let Some((_, duration)) = index.and_then(|index| self.pending.remove(index)) {
            packet.set_duration(duration);
        }
        if let Some(dts) = packet.dts() {
            self.first_dts.get_or_insert(dts);
        }
        if draining
            && self.pending.is_empty()
            && let (Some(first_pts), Some(first_dts), Some(pts), Some(dts)) =
                (self.first_pts, self.first_dts, packet.pts(), packet.dts())
        {
            // MP4 builds sample durations from consecutive DTS. Its last decode
            // sample must cover the whole presentation span, even when the final
            // still image was emitted earlier because of B-frame reordering.
            // Keep the last CTS no larger than the initial reorder offset so
            // extending the decode tail cannot extend the presentation endpoint.
            let reorder_offset = first_pts.saturating_sub(first_dts).max(0);
            let dts = dts.max(pts.saturating_sub(reorder_offset));
            packet.set_dts(Some(dts));
            let tail = self
                .end_pts
                .saturating_sub(reorder_offset)
                .saturating_sub(dts);
            packet.set_duration(packet.duration().max(tail));
        }
    }
}
