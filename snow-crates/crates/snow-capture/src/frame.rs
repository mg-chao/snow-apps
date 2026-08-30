use std::sync::Arc;
use std::time::{Duration, Instant};

use crate::backend::CaptureBackendKind;

use crossbeam_channel as mpsc;

use crate::error::{CaptureError, CaptureResult};
#[cfg(feature = "stage-timing")]
use crate::timing::StageTiming;
use snow_core::event::{DeliveryLane, StreamEvent};
use snow_core::timestamp::{StreamTimestamp, TickFormat};
use snow_cursor::AttachedCursorSample;

/// Packed 8-bit pixel layout used by captured frames.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum CapturePixelFormat {
    /// Bytes are ordered red, green, blue, alpha.
    #[default]
    Rgba8,
    /// Bytes are ordered blue, green, red, alpha.
    Bgra8,
}

/// Color space / transfer function describing the frame's pixel data.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum ColorSpace {
    /// Standard sRGB (BT.709 primaries, sRGB transfer function).
    /// This is the default for SDR captures and tonemapped HDR output.
    #[default]
    Srgb,
    /// Scene-referred linear light (BT.709 primaries, linear gamma).
    /// Produced when the DXGI backend captures an HDR surface without
    /// tonemapping.
    LinearSrgb,
    /// HDR10 / PQ (BT.2020 primaries, SMPTE ST 2084 transfer function).
    Hdr10Pq,
    /// Hybrid Log-Gamma (BT.2020 primaries, ARIB STD-B67).
    Hlg,
}

/// Minimum allocation size to attempt large-page backing.
/// 4K RGBA = 3840x2160x4 ~= 33 MB - well above the 2 MB large page size.
/// We only bother for allocations >= 4 MB to avoid overhead on small captures.
const LARGE_PAGE_MIN_BYTES: usize = 4 * 1024 * 1024;

/// A rectangle describing a dirty (changed) region of the screen.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DirtyRect {
    pub x: u32,
    pub y: u32,
    pub width: u32,
    pub height: u32,
}

/// Metadata attached to each captured frame for recording pipelines.
#[derive(Clone, Debug, Default)]
pub struct FrameMetadata {
    /// Wall-clock time spent inside the capture call (GPU readback,
    /// staging copy, pixel conversion). Lets recorders detect when the
    /// capture pipeline itself is the bottleneck vs. the encoder.
    pub(crate) capture_duration: Option<Duration>,
    /// Whether this frame's content is identical to the previous frame.
    /// `true` means no new desktop present occurred - a recorder can skip
    /// encoding this frame to save bitrate.
    pub(crate) is_duplicate: bool,
    /// Dirty rectangles describing which regions changed since the last
    /// frame. Empty when the backend doesn't support damage tracking or
    /// when the entire frame changed.
    pub(crate) dirty_rects: Vec<DirtyRect>,
    /// Monotonic sequence number incremented for each capture call.
    /// Useful for correlating frames across threads.
    pub(crate) sequence: u64,
    /// Color space / transfer function of the pixel data. Defaults to
    /// `Srgb` for standard dynamic range captures. HDR pipelines can
    /// check this to decide whether tonemapping or passthrough is needed.
    pub(crate) color_space: ColorSpace,
    /// Unified timestamp. GPU backends use either raw QPC ticks (DXGI) or
    /// 100-nanosecond system-relative ticks (WGC).
    pub(crate) stream_timestamp: Option<StreamTimestamp>,
    /// Cursor state sampled for this frame on the capture thread.
    pub(crate) cursor: Option<AttachedCursorSample>,
    /// Concrete backend that produced this frame. `Auto` means the producer
    /// did not report a concrete backend.
    pub(crate) backend_kind: CaptureBackendKind,
    /// Per-stage timing breakdown of the capture call. Empty unless the
    /// session was opened with `CaptureOptions::record_stage_timings`.
    /// Only present in builds with the `stage-timing` feature.
    #[cfg(feature = "stage-timing")]
    pub(crate) stage_timings: Vec<StageTiming>,
}

impl FrameMetadata {
    pub fn backend_kind(&self) -> CaptureBackendKind {
        self.backend_kind
    }

    pub fn capture_duration(&self) -> Option<Duration> {
        self.capture_duration
    }

    pub fn is_duplicate(&self) -> bool {
        self.is_duplicate
    }

    pub fn sequence(&self) -> u64 {
        self.sequence
    }

    pub fn dirty_rects(&self) -> &[DirtyRect] {
        &self.dirty_rects
    }

    pub fn color_space(&self) -> ColorSpace {
        self.color_space
    }

    pub fn stream_timestamp(&self) -> Option<&StreamTimestamp> {
        self.stream_timestamp.as_ref()
    }

    /// Per-stage timing breakdown recorded inside the backend. Empty unless
    /// the session was opened with `CaptureOptions::record_stage_timings`.
    /// Only present in builds with the `stage-timing` feature.
    ///
    /// Entries produced by `record` (OS-call detail such as
    /// `dxgi.sys.acquire_next_frame`, or frame-age metrics) may overlap the
    /// additive `mark`-based entries; consumers interested in an additive
    /// breakdown should select the mark-based stages for the backend.
    #[cfg(feature = "stage-timing")]
    pub fn stage_timings(&self) -> &[StageTiming] {
        &self.stage_timings
    }

    pub fn cursor(&self) -> Option<&AttachedCursorSample> {
        self.cursor.as_ref()
    }

    /// Set timing fields from a capture operation.
    ///
    /// Populates `stream_timestamp` from the capture time and QPC value.
    pub(crate) fn set_timing(&mut self, capture_time: Option<Instant>, raw_os_ticks: Option<i64>) {
        self.set_timing_with_format(capture_time, raw_os_ticks, TickFormat::RawQpc);
    }

    pub(crate) fn set_timing_with_format(
        &mut self,
        capture_time: Option<Instant>,
        raw_os_ticks: Option<i64>,
        tick_format: TickFormat,
    ) {
        self.stream_timestamp = Some(StreamTimestamp {
            instant: capture_time.unwrap_or_else(Instant::now),
            raw_os_ticks,
            tick_format,
        });
    }
}

/// Query the current QPC counter value. Returns `None` on non-Windows
/// or if the call fails.
#[cfg(target_os = "windows")]
pub(crate) fn query_qpc_now() -> Option<i64> {
    use windows::Win32::System::Performance::QueryPerformanceCounter;
    let mut ticks = 0i64;
    let ok = unsafe { QueryPerformanceCounter(&mut ticks) };
    if ok.is_ok() { Some(ticks) } else { None }
}

pub struct Frame {
    data: FrameBuffer,
    width: u32,
    height: u32,
    pixel_format: CapturePixelFormat,
    /// Per-frame metadata for recording pipelines.
    pub(crate) metadata: FrameMetadata,
}

impl Clone for Frame {
    fn clone(&self) -> Self {
        Self {
            data: self.data.clone(),
            width: self.width,
            height: self.height,
            pixel_format: self.pixel_format,
            metadata: self.metadata.clone(),
        }
    }
}

/// Frame buffer that tries to use large pages (2 MB) via `VirtualAlloc`
/// to reduce TLB misses during parallel pixel conversion. Buffers are
/// reference-counted so screenshot mode can keep a cheap history clone;
/// writes use copy-on-write when the pixels are still shared.
#[derive(Clone)]
struct FrameBuffer {
    storage: Arc<FrameBufferStorage>,
}

enum FrameBufferStorage {
    Vec(Vec<u8>),
    #[cfg(target_os = "windows")]
    LargePage(LargePageAlloc),
}

#[cfg(target_os = "windows")]
struct LargePageAlloc {
    ptr: *mut u8,
    len: usize,
    capacity: usize,
}

#[cfg(target_os = "windows")]
unsafe impl Send for LargePageAlloc {}
#[cfg(target_os = "windows")]
unsafe impl Sync for LargePageAlloc {}

#[cfg(target_os = "windows")]
impl Drop for LargePageAlloc {
    fn drop(&mut self) {
        if !self.ptr.is_null() {
            use windows::Win32::System::Memory::{MEM_RELEASE, VirtualFree};
            unsafe {
                let _ = VirtualFree(self.ptr as *mut _, 0, MEM_RELEASE);
            }
        }
    }
}

#[cfg(target_os = "windows")]
fn try_alloc_large_pages(size: usize) -> Option<LargePageAlloc> {
    use windows::Win32::System::Memory::{
        GetLargePageMinimum, MEM_COMMIT, MEM_LARGE_PAGES, MEM_RESERVE, PAGE_READWRITE, VirtualAlloc,
    };

    if size < LARGE_PAGE_MIN_BYTES {
        return None;
    }

    let large_page_size = unsafe { GetLargePageMinimum() };
    if large_page_size == 0 {
        return None;
    }

    let aligned_size = (size + large_page_size - 1) & !(large_page_size - 1);

    let ptr = unsafe {
        VirtualAlloc(
            None,
            aligned_size,
            MEM_COMMIT | MEM_RESERVE | MEM_LARGE_PAGES,
            PAGE_READWRITE,
        )
    };

    if ptr.is_null() {
        return None;
    }

    Some(LargePageAlloc {
        ptr: ptr as *mut u8,
        len: 0,
        capacity: aligned_size,
    })
}

impl FrameBufferStorage {
    fn len(&self) -> usize {
        match self {
            FrameBufferStorage::Vec(v) => v.len(),
            #[cfg(target_os = "windows")]
            FrameBufferStorage::LargePage(lp) => lp.len,
        }
    }

    fn capacity(&self) -> usize {
        match self {
            FrameBufferStorage::Vec(v) => v.capacity(),
            #[cfg(target_os = "windows")]
            FrameBufferStorage::LargePage(lp) => lp.capacity,
        }
    }

    fn set_len(&mut self, len: usize) {
        match self {
            FrameBufferStorage::Vec(v) => unsafe { v.set_len(len) },
            #[cfg(target_os = "windows")]
            FrameBufferStorage::LargePage(lp) => lp.len = len,
        }
    }

    fn as_mut_ptr(&mut self) -> *mut u8 {
        match self {
            FrameBufferStorage::Vec(v) => v.as_mut_ptr(),
            #[cfg(target_os = "windows")]
            FrameBufferStorage::LargePage(lp) => lp.ptr,
        }
    }

    fn as_slice(&self) -> &[u8] {
        match self {
            FrameBufferStorage::Vec(v) => v.as_slice(),
            #[cfg(target_os = "windows")]
            FrameBufferStorage::LargePage(lp) => unsafe {
                std::slice::from_raw_parts(lp.ptr, lp.len)
            },
        }
    }

    fn as_mut_slice(&mut self) -> &mut [u8] {
        match self {
            FrameBufferStorage::Vec(v) => v.as_mut_slice(),
            #[cfg(target_os = "windows")]
            FrameBufferStorage::LargePage(lp) => unsafe {
                std::slice::from_raw_parts_mut(lp.ptr, lp.len)
            },
        }
    }
}

fn alloc_frame_buffer_storage(len: usize) -> FrameBufferStorage {
    #[cfg(target_os = "windows")]
    if len >= LARGE_PAGE_MIN_BYTES
        && let Some(mut lp) = try_alloc_large_pages(len)
    {
        lp.len = len;
        return FrameBufferStorage::LargePage(lp);
    }

    // Heap fallback. Verified empirically (Windows 11, classic NT heap):
    // dropping freed monitor-sized buffers from this path does NOT leave
    // private working set memory occupied. The NT heap serves multi-MB
    // allocations from dedicated VirtualAlloc-backed segments and releases
    // them fully on free, so commit charge and working set both return to
    // baseline immediately. The 12.5% capacity headroom therefore costs
    // nothing after the buffers are dropped.
    let headroom = len / 8;
    let mut v = Vec::with_capacity(len + headroom);
    v.resize(len, 0);
    FrameBufferStorage::Vec(v)
}

fn clone_frame_buffer_storage(source: &FrameBufferStorage, len: usize) -> FrameBufferStorage {
    let copy_len = source.len().min(len);
    let mut storage = alloc_frame_buffer_storage(len);
    if copy_len != 0 {
        storage.as_mut_slice()[..copy_len].copy_from_slice(&source.as_slice()[..copy_len]);
    }
    storage
}

impl FrameBuffer {
    fn new() -> Self {
        Self {
            storage: Arc::new(FrameBufferStorage::Vec(Vec::new())),
        }
    }

    fn len(&self) -> usize {
        self.storage.len()
    }

    fn make_unique_with_len(&mut self, len: usize) {
        if let Some(storage) = Arc::get_mut(&mut self.storage) {
            if storage.len() == len {
                return;
            }
            if len <= storage.capacity() {
                storage.set_len(len);
                return;
            }
        }

        self.storage = Arc::new(clone_frame_buffer_storage(self.storage.as_ref(), len));
    }

    fn ensure_len(&mut self, len: usize) {
        self.make_unique_with_len(len);
    }

    fn as_mut_ptr(&mut self) -> *mut u8 {
        self.make_unique_with_len(self.len());
        Arc::get_mut(&mut self.storage)
            .expect("frame buffer must be unique after make_unique_with_len")
            .as_mut_ptr()
    }

    fn as_slice(&self) -> &[u8] {
        self.storage.as_slice()
    }

    fn as_mut_slice(&mut self) -> &mut [u8] {
        self.make_unique_with_len(self.len());
        Arc::get_mut(&mut self.storage)
            .expect("frame buffer must be unique after make_unique_with_len")
            .as_mut_slice()
    }
}

impl Frame {
    pub fn empty() -> Self {
        Self {
            data: FrameBuffer::new(),
            width: 0,
            height: 0,
            pixel_format: CapturePixelFormat::Rgba8,
            metadata: FrameMetadata::default(),
        }
    }

    pub fn from_rgba8(width: u32, height: u32, data: Vec<u8>) -> CaptureResult<Self> {
        let expected = packed_8bit_len(width, height)?;
        if data.len() != expected {
            return Err(CaptureError::InvalidConfig(format!(
                "RGBA frame data length mismatch: got {}, expected {} for {}x{}",
                data.len(),
                expected,
                width,
                height
            )));
        }

        Ok(Self {
            data: FrameBuffer {
                storage: Arc::new(FrameBufferStorage::Vec(data)),
            },
            width,
            height,
            pixel_format: CapturePixelFormat::Rgba8,
            metadata: FrameMetadata::default(),
        })
    }

    pub fn from_bgra8(width: u32, height: u32, data: Vec<u8>) -> CaptureResult<Self> {
        let expected = packed_8bit_len(width, height)?;
        if data.len() != expected {
            return Err(CaptureError::InvalidConfig(format!(
                "BGRA frame data length mismatch: got {}, expected {} for {}x{}",
                data.len(),
                expected,
                width,
                height
            )));
        }

        Ok(Self {
            data: FrameBuffer {
                storage: Arc::new(FrameBufferStorage::Vec(data)),
            },
            width,
            height,
            pixel_format: CapturePixelFormat::Bgra8,
            metadata: FrameMetadata::default(),
        })
    }

    pub fn width(&self) -> u32 {
        self.width
    }

    pub fn height(&self) -> u32 {
        self.height
    }

    pub fn dimensions(&self) -> (u32, u32) {
        (self.width, self.height)
    }

    pub fn pixel_format(&self) -> CapturePixelFormat {
        self.pixel_format
    }

    pub fn as_bytes(&self) -> &[u8] {
        self.data.as_slice()
    }

    pub fn as_mut_bytes(&mut self) -> &mut [u8] {
        self.data.as_mut_slice()
    }

    pub fn as_rgba_bytes(&self) -> &[u8] {
        self.as_bytes()
    }

    pub fn as_mut_rgba_bytes(&mut self) -> &mut [u8] {
        self.as_mut_bytes()
    }

    pub fn metadata(&self) -> &FrameMetadata {
        &self.metadata
    }

    pub(crate) fn as_mut_rgba_ptr(&mut self) -> *mut u8 {
        self.data.as_mut_ptr()
    }

    pub fn ensure_capacity(
        &mut self,
        width: u32,
        height: u32,
        pixel_format: CapturePixelFormat,
    ) -> CaptureResult<()> {
        let len = packed_8bit_len(width, height)?;
        self.data.ensure_len(len);
        self.width = width;
        self.height = height;
        self.pixel_format = pixel_format;
        Ok(())
    }

    /// Resize this frame's RGBA storage for `width` by `height` pixels while
    /// retaining an existing allocation when it is large enough.
    ///
    /// This is useful for preparing a reusable destination before a latency-
    /// sensitive capture. Existing pixel contents are unspecified when the
    /// dimensions change; capture code is expected to overwrite them.
    pub fn ensure_rgba_capacity(&mut self, width: u32, height: u32) -> CaptureResult<()> {
        self.ensure_capacity(width, height, CapturePixelFormat::Rgba8)
    }

    #[cfg(test)]
    pub(crate) fn copy_from_frame(&mut self, source: &Frame) -> CaptureResult<()> {
        self.ensure_capacity(source.width, source.height, source.pixel_format)?;
        self.as_mut_bytes().copy_from_slice(source.as_bytes());
        self.metadata = source.metadata.clone();
        Ok(())
    }

    /// Reset metadata fields to defaults, preserving the pixel buffer.
    /// Called at the start of each capture to avoid stale metadata from
    /// a reused frame leaking into the new result.
    pub(crate) fn reset_metadata(&mut self) {
        self.metadata.stream_timestamp = None;
        self.metadata.capture_duration = None;
        self.metadata.is_duplicate = false;
        self.metadata.dirty_rects.clear();
        self.metadata.color_space = ColorSpace::default();
        self.metadata.cursor = None;
        self.metadata.backend_kind = CaptureBackendKind::Auto;
        #[cfg(feature = "stage-timing")]
        self.metadata.stage_timings.clear();
        // sequence is set by the session, not reset here
    }
}

#[derive(Clone)]
pub(crate) struct FrameRecycleSender {
    tx: mpsc::Sender<Frame>,
}

impl FrameRecycleSender {
    pub(crate) fn new(tx: mpsc::Sender<Frame>) -> Self {
        Self { tx }
    }

    fn recycle(&self, mut frame: Frame) {
        frame.reset_metadata();
        let _ = self.tx.try_send(frame);
    }
}

struct CapturedFrameInner {
    frame: Frame,
    recycler: Option<FrameRecycleSender>,
}

/// Immutable, shareable frame view used by streaming/recording paths.
///
/// When created by the streaming pipeline, the underlying owned `Frame`
/// is returned to an internal recycle pool once the last handle drops.
pub struct CapturedFrame {
    inner: Option<Arc<CapturedFrameInner>>,
}

impl CapturedFrame {
    pub(crate) fn from_frame_with_recycler(frame: Frame, recycler: FrameRecycleSender) -> Self {
        Self {
            inner: Some(Arc::new(CapturedFrameInner {
                frame,
                recycler: Some(recycler),
            })),
        }
    }

    pub fn width(&self) -> u32 {
        self.frame().width()
    }

    pub fn height(&self) -> u32 {
        self.frame().height()
    }

    pub fn dimensions(&self) -> (u32, u32) {
        self.frame().dimensions()
    }

    pub fn pixel_format(&self) -> CapturePixelFormat {
        self.frame().pixel_format()
    }

    pub fn as_bytes(&self) -> &[u8] {
        self.frame().as_bytes()
    }

    pub fn as_rgba_bytes(&self) -> &[u8] {
        self.frame().as_rgba_bytes()
    }

    pub fn metadata(&self) -> &FrameMetadata {
        &self.frame().metadata
    }

    pub fn into_owned(mut self) -> Result<Frame, Self> {
        let Some(inner) = self.inner.take() else {
            unreachable!("captured frame missing inner storage")
        };
        match Arc::try_unwrap(inner) {
            Ok(mut inner) => {
                inner.recycler = None;
                Ok(inner.frame)
            }
            Err(inner) => {
                self.inner = Some(inner);
                Err(self)
            }
        }
    }

    fn frame(&self) -> &Frame {
        &self
            .inner
            .as_ref()
            .expect("captured frame missing inner storage")
            .frame
    }
}

impl Clone for CapturedFrame {
    fn clone(&self) -> Self {
        Self {
            inner: self.inner.clone(),
        }
    }
}

impl Drop for CapturedFrame {
    fn drop(&mut self) {
        let Some(inner) = self.inner.take() else {
            return;
        };

        let Ok(mut inner) = Arc::try_unwrap(inner) else {
            return;
        };

        if let Some(recycler) = inner.recycler.take() {
            recycler.recycle(inner.frame);
        }
    }
}

impl From<Frame> for CapturedFrame {
    fn from(frame: Frame) -> Self {
        Self {
            inner: Some(Arc::new(CapturedFrameInner {
                frame,
                recycler: None,
            })),
        }
    }
}

impl std::fmt::Debug for CapturedFrame {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("CapturedFrame")
            .field("width", &self.width())
            .field("height", &self.height())
            .field("data_len", &self.as_rgba_bytes().len())
            .field("metadata", self.metadata())
            .finish()
    }
}

/// Notification sent through the streaming channel when the capture
/// source changes in a way that affects the output.
#[derive(Debug)]
pub enum CaptureEvent {
    /// A new frame is available.
    Frame(CapturedFrame),
    /// The capture source resolution changed. A recorder should
    /// reconfigure its encoder with the new dimensions.
    ResolutionChanged {
        old_width: u32,
        old_height: u32,
        new_width: u32,
        new_height: u32,
    },
    /// One or more frames were dropped due to backpressure (the
    /// receiver couldn't keep up). A recorder should insert duplicate
    /// frames to maintain correct A/V timing.
    FramesDropped {
        /// Number of dropped frames since the last drop notice.
        count: u32,
    },
    /// The stream was paused. Contains the `Instant` at which the
    /// pause took effect. Recorders can use this to account for the
    /// gap in their muxer timeline.
    Paused { at: Instant },
    /// The stream was resumed after a pause. Contains the `Instant`
    /// of resumption and the total duration of the pause gap.
    Resumed { at: Instant, gap: Duration },
    /// The stream thread is about to exit cleanly (stop was requested).
    /// Sent after the last frame so the consumer knows no more events
    /// will arrive. Useful for flushing encoder queues.
    StreamEnded,
    /// The stream encountered a fatal error and is about to exit.
    /// After receiving this event the channel will disconnect.
    Error(CaptureError),
}

impl StreamEvent for CaptureEvent {
    fn is_paused(&self) -> bool {
        matches!(self, CaptureEvent::Paused { .. })
    }

    fn is_resumed(&self) -> bool {
        matches!(self, CaptureEvent::Resumed { .. })
    }

    fn is_stream_ended(&self) -> bool {
        matches!(self, CaptureEvent::StreamEnded)
    }

    fn is_error(&self) -> bool {
        matches!(self, CaptureEvent::Error(_))
    }

    fn delivery_lane(&self) -> DeliveryLane {
        match self {
            CaptureEvent::Frame(_) => DeliveryLane::Data,
            CaptureEvent::ResolutionChanged { .. }
            | CaptureEvent::FramesDropped { .. }
            | CaptureEvent::Paused { .. }
            | CaptureEvent::Resumed { .. }
            | CaptureEvent::StreamEnded
            | CaptureEvent::Error(_) => DeliveryLane::Control,
        }
    }

    fn timestamp(&self) -> Option<&StreamTimestamp> {
        match self {
            CaptureEvent::Frame(frame) => frame.metadata().stream_timestamp(),
            _ => None,
        }
    }
}

fn packed_8bit_len(width: u32, height: u32) -> CaptureResult<usize> {
    let w = usize::try_from(width).map_err(|_| CaptureError::BufferOverflow)?;
    let h = usize::try_from(height).map_err(|_| CaptureError::BufferOverflow)?;
    w.checked_mul(h)
        .and_then(|px| px.checked_mul(4))
        .ok_or(CaptureError::BufferOverflow)
}

impl std::fmt::Debug for Frame {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Frame")
            .field("width", &self.width)
            .field("height", &self.height)
            .field("pixel_format", &self.pixel_format)
            .field("data_len", &self.data.len())
            .field("metadata", &self.metadata)
            .finish()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crossbeam_channel as mpsc;
    use proptest::prelude::*;
    use snow_core::timestamp::TickFormat;

    #[cfg(feature = "stage-timing")]
    #[test]
    fn frame_metadata_stage_timings_roundtrip_and_reset() {
        let mut frame = Frame::empty();
        assert!(frame.metadata().stage_timings().is_empty());

        frame.metadata.stage_timings = vec![StageTiming {
            name: "dxgi.acquire_loop",
            duration: Duration::from_micros(10),
        }];
        assert_eq!(frame.metadata().stage_timings().len(), 1);
        assert_eq!(
            frame.metadata().stage_timings()[0].name,
            "dxgi.acquire_loop"
        );

        frame.reset_metadata();
        assert!(frame.metadata().stage_timings().is_empty());
    }

    // **Validates: Requirements 1.2, 1.3**
    //
    // Property 2: Tick format matches source type
    //
    // For any `FrameMetadata` with timing set (any combination of
    // `capture_time` and QPC values including `None`),
    // `stream_timestamp.tick_format` SHALL be `TickFormat::RawQpc`.
    proptest! {
        #![proptest_config(ProptestConfig::with_cases(100))]
        #[test]
        fn prop_frame_metadata_tick_format_always_rawqpc(
            has_capture_time in proptest::bool::ANY,
            has_qpc in proptest::bool::ANY,
            qpc_value in proptest::num::i64::ANY,
        ) {
            let capture_time = if has_capture_time { Some(Instant::now()) } else { None };
            let qpc = if has_qpc { Some(qpc_value) } else { None };

            let mut meta = FrameMetadata::default();
            meta.set_timing(capture_time, qpc);

            let ts = meta.stream_timestamp.as_ref()
                .expect("stream_timestamp must be Some after set_timing");
            prop_assert_eq!(ts.tick_format, TickFormat::RawQpc,
                "FrameMetadata tick_format must always be RawQpc, got {:?}", ts.tick_format);
        }
    }

    #[test]
    fn hns_timing_preserves_tick_format_and_raw_value() {
        let capture_time = Instant::now();
        let mut metadata = FrameMetadata::default();
        metadata.set_timing_with_format(Some(capture_time), Some(123_456_789), TickFormat::Hns100);

        let timestamp = metadata
            .stream_timestamp()
            .expect("set_timing_with_format should populate a timestamp");
        assert_eq!(timestamp.instant, capture_time);
        assert_eq!(timestamp.raw_os_ticks, Some(123_456_789));
        assert_eq!(timestamp.tick_format, TickFormat::Hns100);
    }

    // **Validates: Requirements 1.6, 1.7, 7.2**
    //
    // Property 1: Leaf metadata always populates stream_timestamp
    //
    // For any call to `FrameMetadata::set_timing` with any combination of
    // `capture_time` (Some/None) and QPC values (Some/None), the resulting
    // `stream_timestamp` SHALL be `Some` with a valid `Instant`.
    proptest! {
        #![proptest_config(ProptestConfig::with_cases(100))]

        #[test]
        fn prop_frame_metadata_always_populates_stream_timestamp(
            has_capture_time in proptest::bool::ANY,
            has_qpc in proptest::bool::ANY,
            qpc_value in proptest::num::i64::ANY,
        ) {
            let capture_time = if has_capture_time { Some(Instant::now()) } else { None };
            let qpc = if has_qpc { Some(qpc_value) } else { None };

            let mut meta = FrameMetadata::default();
            let before = Instant::now();
            meta.set_timing(capture_time, qpc);
            let after = Instant::now();

            // stream_timestamp must always be Some
            let ts = meta.stream_timestamp.as_ref();
            prop_assert!(ts.is_some(), "stream_timestamp must always be Some after set_timing");

            let ts = ts.unwrap();

            // instant must be valid (between before and after, or equal to capture_time)
            if let Some(ct) = capture_time {
                prop_assert_eq!(ts.instant, ct,
                    "instant should equal the provided capture_time");
            } else {
                // When capture_time is None, instant is Instant::now() at call time
                prop_assert!(ts.instant >= before,
                    "instant should be >= time before set_timing call");
                prop_assert!(ts.instant <= after,
                    "instant should be <= time after set_timing call");
            }

            // raw_os_ticks should match the provided QPC
            prop_assert_eq!(ts.raw_os_ticks, qpc,
                "raw_os_ticks should match the provided QPC value");
        }
    }

    // ── Strategies for generating random CaptureEvent variants ──

    /// Build a `Frame` with an optional `stream_timestamp`.
    fn arb_frame(has_timestamp: bool) -> Frame {
        let mut frame = Frame::empty();
        if has_timestamp {
            frame.metadata.set_timing(Some(Instant::now()), Some(42));
        }
        frame
    }

    /// Strategy that produces random `CaptureEvent` variants.
    fn arb_capture_event() -> impl Strategy<Value = CaptureEvent> {
        prop_oneof![
            // Frame with stream_timestamp set
            Just(()).prop_map(|_| CaptureEvent::Frame(arb_frame(true).into())),
            // Frame without stream_timestamp
            Just(()).prop_map(|_| CaptureEvent::Frame(arb_frame(false).into())),
            // ResolutionChanged with random dimensions
            (1u32..4096, 1u32..4096, 1u32..4096, 1u32..4096).prop_map(|(ow, oh, nw, nh)| {
                CaptureEvent::ResolutionChanged {
                    old_width: ow,
                    old_height: oh,
                    new_width: nw,
                    new_height: nh,
                }
            }),
            // FramesDropped with random drop counts
            (1u32..8, any::<u64>()).prop_map(|(count, _seq)| CaptureEvent::FramesDropped { count }),
            // Paused
            Just(()).prop_map(|_| CaptureEvent::Paused { at: Instant::now() }),
            // Resumed
            (0u64..10_000).prop_map(|gap_ms| CaptureEvent::Resumed {
                at: Instant::now(),
                gap: Duration::from_millis(gap_ms),
            }),
            // StreamEnded
            Just(()).prop_map(|_| CaptureEvent::StreamEnded),
            // Error
            Just(()).prop_map(|_| CaptureEvent::Error(CaptureError::Timeout)),
        ]
    }

    // **Validates: Requirements 3.3, 3.6**
    //
    // Property 3: StreamEvent lifecycle consistency (CaptureEvent)
    //
    // For lifecycle variants (Paused, Resumed, StreamEnded, Error), exactly
    // one lifecycle method returns true. For data/control variants (Frame,
    // FramesDropped, ResolutionChanged), all lifecycle methods return false.
    proptest! {
        #![proptest_config(ProptestConfig::with_cases(100))]

        #[test]
        fn prop_capture_event_lifecycle_consistency(event in arb_capture_event()) {
            let lifecycle_results = [
                event.is_paused(),
                event.is_resumed(),
                event.is_stream_ended(),
                event.is_error(),
            ];
            let true_count = lifecycle_results.iter().filter(|&&v| v).count();

            let is_lifecycle_variant = matches!(
                event,
                CaptureEvent::Paused { .. }
                    | CaptureEvent::Resumed { .. }
                    | CaptureEvent::StreamEnded
                    | CaptureEvent::Error(_)
            );

            if is_lifecycle_variant {
                // Exactly one lifecycle method returns true
                prop_assert_eq!(
                    true_count, 1,
                    "lifecycle variant should have exactly one true method, got {}",
                    true_count
                );
            } else {
                // Data/control variants: all lifecycle methods return false
                prop_assert_eq!(
                    true_count, 0,
                    "data/control variant should have all false lifecycle methods, got {} true",
                    true_count
                );
            }

            // Verify specific lifecycle method matches the variant
            match &event {
                CaptureEvent::Paused { .. } => {
                    prop_assert!(event.is_paused());
                    prop_assert!(!event.is_resumed());
                    prop_assert!(!event.is_stream_ended());
                    prop_assert!(!event.is_error());
                }
                CaptureEvent::Resumed { .. } => {
                    prop_assert!(!event.is_paused());
                    prop_assert!(event.is_resumed());
                    prop_assert!(!event.is_stream_ended());
                    prop_assert!(!event.is_error());
                }
                CaptureEvent::StreamEnded => {
                    prop_assert!(!event.is_paused());
                    prop_assert!(!event.is_resumed());
                    prop_assert!(event.is_stream_ended());
                    prop_assert!(!event.is_error());
                }
                CaptureEvent::Error(_) => {
                    prop_assert!(!event.is_paused());
                    prop_assert!(!event.is_resumed());
                    prop_assert!(!event.is_stream_ended());
                    prop_assert!(event.is_error());
                }
                _ => {}
            }

            // Verify timestamp() behavior:
            // - Frame with stream_timestamp set → Some
            // - All other variants → None
            match &event {
                CaptureEvent::Frame(frame) => {
                    if frame.metadata().stream_timestamp.is_some() {
                        prop_assert!(event.timestamp().is_some(),
                            "Frame with stream_timestamp should return Some from timestamp()");
                    } else {
                        prop_assert!(event.timestamp().is_none(),
                            "Frame without stream_timestamp should return None from timestamp()");
                    }
                }
                _ => {
                    prop_assert!(event.timestamp().is_none(),
                        "Non-Frame variant should return None from timestamp()");
                }
            }
        }
    }

    #[test]
    fn frame_clone_shares_pixels_until_mutated() {
        let mut source = Frame::empty();
        source.ensure_rgba_capacity(2, 2).unwrap();
        source
            .as_mut_rgba_bytes()
            .copy_from_slice(&[1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16]);
        source.metadata.sequence = 7;

        let mut cloned = source.clone();
        cloned.metadata.sequence = 99;
        cloned.as_mut_rgba_bytes()[0] = 200;
        cloned.as_mut_rgba_bytes()[15] = 210;

        assert_eq!(source.dimensions(), cloned.dimensions());
        assert_eq!(source.metadata.sequence, 7);
        assert_eq!(cloned.metadata.sequence, 99);
        assert_eq!(source.as_rgba_bytes()[0], 1);
        assert_eq!(source.as_rgba_bytes()[15], 16);
        assert_eq!(cloned.as_rgba_bytes()[0], 200);
        assert_eq!(cloned.as_rgba_bytes()[15], 210);
    }

    #[test]
    fn frame_copy_from_frame_clones_pixels_and_metadata() {
        let mut source = Frame::empty();
        source.ensure_rgba_capacity(3, 2).unwrap();
        for (idx, byte) in source.as_mut_rgba_bytes().iter_mut().enumerate() {
            *byte = (idx as u8).wrapping_mul(17).wrapping_add(9);
        }
        source.metadata.sequence = 42;
        source.metadata.is_duplicate = true;
        source.metadata.dirty_rects = vec![DirtyRect {
            x: 1,
            y: 0,
            width: 2,
            height: 1,
        }];

        let mut dest = Frame::empty();
        dest.copy_from_frame(&source).unwrap();

        assert_eq!(dest.dimensions(), (3, 2));
        assert_eq!(dest.as_rgba_bytes(), source.as_rgba_bytes());
        assert_eq!(dest.metadata.sequence, 42);
        assert!(dest.metadata.is_duplicate);
        assert_eq!(dest.metadata.dirty_rects, source.metadata.dirty_rects);
    }

    #[test]
    fn captured_frame_recycles_underlying_frame_on_last_drop() {
        let (tx, rx) = mpsc::bounded(1);
        let recycler = FrameRecycleSender::new(tx);

        let mut frame = Frame::empty();
        frame.ensure_rgba_capacity(4, 4).unwrap();
        frame.metadata.is_duplicate = true;

        let captured = CapturedFrame::from_frame_with_recycler(frame, recycler);
        let clone = captured.clone();

        drop(captured);
        assert!(
            rx.try_recv().is_err(),
            "frame recycled before last handle dropped"
        );

        drop(clone);
        let recycled = rx
            .try_recv()
            .expect("expected recycled frame after final drop");
        assert_eq!(recycled.dimensions(), (4, 4));
        assert!(!recycled.metadata.is_duplicate);
    }

    #[test]
    fn captured_frame_into_owned_skips_recycler() {
        let (tx, rx) = mpsc::bounded(1);
        let recycler = FrameRecycleSender::new(tx);

        let mut frame = Frame::empty();
        frame.ensure_rgba_capacity(2, 2).unwrap();

        let captured = CapturedFrame::from_frame_with_recycler(frame, recycler);
        let owned = captured
            .into_owned()
            .expect("expected unique captured frame to unwrap");
        assert_eq!(owned.dimensions(), (2, 2));
        assert!(rx.try_recv().is_err(), "into_owned should detach recycler");
    }
}
