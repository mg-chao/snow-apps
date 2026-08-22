use std::cell::RefCell;
use std::ffi::{CStr, CString, c_char};
use std::io::Write;
use std::mem::size_of;
use std::path::{Path, PathBuf};
use std::ptr;
use std::slice;
use std::sync::Arc;
use std::sync::Mutex;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::mpsc::{Receiver, SyncSender, TryRecvError, sync_channel};
use std::thread::{self, JoinHandle};

use snow_stitch_images::{
    Frame, MotionDiagnostics, MotionStage, PixelFormat, StitchAxis, StitchBranch, StitchError,
    StitchOptions, StitchTraceEvent, Stitcher, TiledCanvasSnapshot,
};

const DEFAULT_MAX_OUTPUT_HEIGHT: u32 = 2_160 * 32;
const DEFAULT_MAX_OUTPUT_PIXELS: u64 = 3_840 * 2_160 * 32;
const DEFAULT_MIN_OVERLAP_ROWS: u32 = 48;
const DEFAULT_MIN_OVERLAP_RATIO: f32 = 0.15;
const DEFAULT_ACCEPTED_HISTORY_CAPACITY: usize = 4;

#[derive(Clone, Copy)]
struct FrameDimensions {
    width: u32,
    height: u32,
}

struct FramePoolInner {
    dimensions: FrameDimensions,
    length: usize,
    capacity: usize,
    active: AtomicUsize,
    returned: Mutex<Vec<Vec<u8>>>,
}

struct RgbaFramePool {
    inner: Arc<FramePoolInner>,
}

impl RgbaFramePool {
    fn new(width: u32, height: u32, capacity: usize) -> Result<Self, StitchError> {
        if width == 0 || height == 0 || capacity == 0 {
            return Err(StitchError::InvalidOptions {
                message: "frame-pool dimensions and capacity must be non-zero".to_owned(),
            });
        }
        let length = (width as usize)
            .checked_mul(height as usize)
            .and_then(|pixels| pixels.checked_mul(4))
            .ok_or(StitchError::Arithmetic {
                operation: "calculating RGBA frame-pool buffer length",
            })?;
        Ok(Self {
            inner: Arc::new(FramePoolInner {
                dimensions: FrameDimensions { width, height },
                length,
                capacity,
                active: AtomicUsize::new(0),
                returned: Mutex::new(Vec::new()),
            }),
        })
    }

    fn acquire(&self) -> Result<RgbaFrameBuffer, StitchError> {
        let mut active = self.inner.active.load(Ordering::Acquire);
        loop {
            if active >= self.inner.capacity {
                return Err(StitchError::InvalidFrame {
                    message: format!(
                        "RGBA frame pool is exhausted (capacity {})",
                        self.inner.capacity
                    ),
                });
            }
            match self.inner.active.compare_exchange_weak(
                active,
                active + 1,
                Ordering::AcqRel,
                Ordering::Acquire,
            ) {
                Ok(_) => break,
                Err(next) => active = next,
            }
        }
        let data = self
            .inner
            .returned
            .lock()
            .expect("frame pool mutex poisoned")
            .pop()
            .unwrap_or_else(|| vec![0; self.inner.length]);
        let mut data = data;
        if data.len() != self.inner.length {
            data.resize(self.inner.length, 0);
        }
        data.fill(0);
        Ok(RgbaFrameBuffer {
            dimensions: self.inner.dimensions,
            data: Some(data),
            pool: Arc::clone(&self.inner),
        })
    }
}

struct RgbaFrameBuffer {
    dimensions: FrameDimensions,
    data: Option<Vec<u8>>,
    pool: Arc<FramePoolInner>,
}

impl RgbaFrameBuffer {
    fn dimensions(&self) -> FrameDimensions {
        self.dimensions
    }

    fn as_mut_rgba_bytes(&mut self) -> &mut [u8] {
        self.data
            .as_mut()
            .expect("live RGBA frame buffer owns its pixels")
    }

    fn freeze(mut self) -> Result<Frame, StitchError> {
        let data = self
            .data
            .take()
            .expect("live RGBA frame buffer owns its pixels");
        let result = Frame::new(
            self.dimensions.width,
            self.dimensions.height,
            PixelFormat::Rgba8,
            data.clone(),
        );
        self.pool
            .returned
            .lock()
            .expect("frame pool mutex poisoned")
            .push(data);
        result
    }
}

impl Drop for RgbaFrameBuffer {
    fn drop(&mut self) {
        if let Some(data) = self.data.take() {
            self.pool
                .returned
                .lock()
                .expect("frame pool mutex poisoned")
                .push(data);
        }
        self.pool.active.fetch_sub(1, Ordering::AcqRel);
    }
}

#[derive(Clone)]
struct RgbaImage {
    frame: Frame,
}

impl RgbaImage {
    fn width(&self) -> u32 {
        self.frame.width()
    }

    fn height(&self) -> u32 {
        self.frame.height()
    }

    fn as_rgba_bytes(&self) -> &[u8] {
        self.frame.pixels()
    }
}

#[derive(Clone)]
struct StitchImageSnapshot {
    canvas: TiledCanvasSnapshot,
}

impl StitchImageSnapshot {
    fn width(&self) -> u32 {
        self.canvas.width()
    }

    fn height(&self) -> u32 {
        self.canvas.height()
    }

    fn rgba_len(&self) -> Result<usize, StitchError> {
        self.width()
            .checked_mul(self.height())
            .and_then(|pixels| pixels.checked_mul(4))
            .and_then(|length| usize::try_from(length).ok())
            .ok_or(StitchError::Arithmetic {
                operation: "calculating snapshot RGBA length",
            })
    }

    fn slice_rows(&self, top: u32, bottom: u32) -> Result<Self, StitchError> {
        if top >= bottom || bottom > self.height() {
            return Err(StitchError::InvalidFrame {
                message: format!(
                    "row range {top}..{bottom} is outside snapshot height {}",
                    self.height()
                ),
            });
        }
        Ok(Self {
            canvas: self.canvas.slice_rows(top, bottom)?,
        })
    }

    fn slice_axis(&self, start: u32, end: u32) -> Result<Self, StitchError> {
        Ok(Self {
            canvas: self.canvas.slice_axis(start, end)?,
        })
    }

    fn materialize(&self) -> Result<RgbaImage, StitchError> {
        Ok(RgbaImage {
            frame: self.canvas.materialize()?,
        })
    }

    fn render_scaled(&self, width: u32, height: u32) -> Result<RgbaImage, StitchError> {
        if width == 0 || height == 0 {
            return Err(StitchError::InvalidFrame {
                message: "scaled dimensions must be non-zero".to_owned(),
            });
        }
        let length = (width as usize)
            .checked_mul(height as usize)
            .and_then(|pixels| pixels.checked_mul(4))
            .ok_or(StitchError::Arithmetic {
                operation: "calculating scaled RGBA length",
            })?;
        let mut pixels = vec![0; length];
        let source_frame = self.canvas.materialize()?;
        let source_width = self.width() as usize;
        let source_height = self.height() as usize;
        for y in 0..height as usize {
            let source_y = y * source_height / height as usize;
            for x in 0..width as usize {
                let source_x = x * source_width / width as usize;
                let source = (source_y * source_width + source_x) * 4;
                let target = (y * width as usize + x) * 4;
                pixels[target..target + 4]
                    .copy_from_slice(&source_frame.pixels()[source..source + 4]);
            }
        }
        Ok(RgbaImage {
            frame: Frame::new(width, height, PixelFormat::Rgba8, pixels)?,
        })
    }
}

#[derive(Clone)]
struct StitchLimits {
    max_output_height: u32,
    max_output_pixels: u64,
}

#[derive(Clone)]
struct StitchConfig {
    axis: StitchAxis,
    limits: StitchLimits,
    min_overlap_rows: u32,
    min_overlap_ratio: f32,
    accepted_history_capacity: usize,
}

impl Default for StitchConfig {
    fn default() -> Self {
        Self {
            axis: StitchAxis::Vertical,
            limits: StitchLimits {
                max_output_height: DEFAULT_MAX_OUTPUT_HEIGHT,
                max_output_pixels: DEFAULT_MAX_OUTPUT_PIXELS,
            },
            min_overlap_rows: DEFAULT_MIN_OVERLAP_ROWS,
            min_overlap_ratio: DEFAULT_MIN_OVERLAP_RATIO,
            accepted_history_capacity: DEFAULT_ACCEPTED_HISTORY_CAPACITY,
        }
    }
}

impl StitchConfig {
    fn validate(&self) -> Result<(), StitchError> {
        if self.limits.max_output_height == 0 || self.limits.max_output_pixels == 0 {
            return Err(StitchError::InvalidOptions {
                message: "output limits must be greater than zero".to_owned(),
            });
        }
        if self.min_overlap_rows == 0
            || !(0.0..=0.5).contains(&self.min_overlap_ratio)
            || !(1..=8).contains(&self.accepted_history_capacity)
        {
            return Err(StitchError::InvalidOptions {
                message: "invalid compatibility stitching configuration".to_owned(),
            });
        }
        Ok(())
    }
}

#[derive(Clone, Copy)]
enum FrameEvent {
    Initial,
    ExtendedTop,
    ExtendedBottom,
    ExtendedLeft,
    ExtendedRight,
    Covered,
    Duplicate,
    Unmatched,
}

#[derive(Clone, Copy)]
enum UnmatchedReason {
    InsufficientOverlap,
    LowInformation,
    Ambiguous,
    ConflictingReferences,
    FixedContentDominated,
    VerificationFailed,
}

#[derive(Clone, Copy, Default)]
struct MatchMetrics {
    score: f32,
    second_score: f32,
    content_coverage: f32,
    fixed_coverage: f32,
    inlier_ratio: f32,
    feature_support: u32,
    reference_count: u32,
}

#[derive(Clone, Copy)]
struct FrameOutcome {
    event: FrameEvent,
    matched_reference_offset: Option<i32>,
    unmatched_reason: Option<UnmatchedReason>,
    metrics: MatchMetrics,
    added_extent: u32,
    output_dimensions: FrameDimensions,
    delta_start: u32,
    delta_extent: u32,
}

struct StitchSession {
    config: StitchConfig,
    stitcher: Stitcher,
}

impl StitchSession {
    fn new(config: StitchConfig) -> Result<Self, StitchError> {
        config.validate()?;
        let axis = config.axis;
        Ok(Self {
            config,
            stitcher: Stitcher::new(StitchOptions {
                axis,
                collect_trace: true,
                ..StitchOptions::default()
            })?,
        })
    }

    fn reset(&mut self) {
        self.stitcher.reset();
    }

    fn push_owned_frame(
        &mut self,
        frame: Result<Frame, StitchError>,
    ) -> Result<FrameOutcome, StitchError> {
        let frame = frame?;
        let initial = self.stitcher.input_count() == 0;
        let trace = self.stitcher.push(frame)?;
        let (width, height) = self
            .stitcher
            .image_dimensions()
            .expect("a successful push initializes the stitcher");
        let pixels = u64::from(width) * u64::from(height);
        let output_extent = self.config.axis.primary_extent(width, height);
        if output_extent > self.config.limits.max_output_height
            || pixels > self.config.limits.max_output_pixels
        {
            return Err(StitchError::InvalidFrame {
                message: format!(
                    "stitched output {}x{} exceeds configured limits",
                    width, height
                ),
            });
        }
        let dimensions = FrameDimensions { width, height };
        if initial {
            return Ok(FrameOutcome {
                event: FrameEvent::Initial,
                matched_reference_offset: None,
                unmatched_reason: None,
                metrics: MatchMetrics::default(),
                added_extent: output_extent,
                output_dimensions: dimensions,
                delta_start: 0,
                delta_extent: output_extent,
            });
        }
        let outcome = trace
            .as_ref()
            .map(|event| frame_outcome(event, dimensions, self.config.axis))
            .ok_or_else(|| StitchError::InvalidFrame {
                message: "incremental stitcher did not emit a trace event".to_owned(),
            })?;
        self.stitcher.clear_trace();
        Ok(outcome)
    }

    fn copy_rows(&self, top: u32, rows: u32, destination: &mut [u8]) -> Result<(), StitchError> {
        let (width, height) = self
            .stitcher
            .image_dimensions()
            .ok_or(StitchError::EmptyInput)?;
        let bottom = top.checked_add(rows).ok_or(StitchError::Arithmetic {
            operation: "calculating copied row range",
        })?;
        if bottom > height {
            return Err(StitchError::InvalidFrame {
                message: "copied row range is outside the stitched image".to_owned(),
            });
        }
        let row_bytes = width as usize * 4;
        let expected = rows as usize * row_bytes;
        if destination.len() != expected {
            return Err(StitchError::InvalidFrame {
                message: format!("row copy needs {expected} bytes, got {}", destination.len()),
            });
        }
        self.stitcher.copy_rows(top, rows, destination)
    }

    fn snapshot(&self, top: u32, bottom: u32) -> Result<StitchImageSnapshot, StitchError> {
        Ok(StitchImageSnapshot {
            canvas: self.stitcher.snapshot_axis(top, bottom)?,
        })
    }

    fn materialize_rows(&self, top: u32, bottom: u32) -> Result<RgbaImage, StitchError> {
        Ok(RgbaImage {
            frame: self.stitcher.materialize_rows(top, bottom)?,
        })
    }

    fn materialize_axis(&self, start: u32, end: u32) -> Result<RgbaImage, StitchError> {
        Ok(RgbaImage {
            frame: self.stitcher.materialize_axis(start, end)?,
        })
    }

    fn render_scaled_rows(
        &self,
        top: u32,
        rows: u32,
        width: u32,
        height: u32,
    ) -> Result<RgbaImage, StitchError> {
        Ok(RgbaImage {
            frame: self.stitcher.render_scaled_rows(top, rows, width, height)?,
        })
    }

    fn render_scaled_axis(
        &self,
        start: u32,
        span: u32,
        width: u32,
        height: u32,
    ) -> Result<RgbaImage, StitchError> {
        Ok(RgbaImage {
            frame: self
                .stitcher
                .render_scaled_axis(start, span, width, height)?,
        })
    }
}

fn frame_outcome(
    event: &StitchTraceEvent,
    dimensions: FrameDimensions,
    axis: StitchAxis,
) -> FrameOutcome {
    let frame_event = match (axis, event.branch) {
        (StitchAxis::Vertical, StitchBranch::Append) => FrameEvent::ExtendedBottom,
        (StitchAxis::Vertical, StitchBranch::Prepend) => FrameEvent::ExtendedTop,
        (StitchAxis::Horizontal, StitchBranch::Append) => FrameEvent::ExtendedRight,
        (StitchAxis::Horizontal, StitchBranch::Prepend) => FrameEvent::ExtendedLeft,
        (_, StitchBranch::Contained) => FrameEvent::Covered,
        (_, StitchBranch::Skip) => FrameEvent::Duplicate,
        (_, StitchBranch::NoMovement) => FrameEvent::Unmatched,
    };
    let unmatched_reason = (event.branch == StitchBranch::NoMovement)
        .then(|| unmatched_reason(event.motion_diagnostics.as_ref()));
    let metrics = event
        .motion_diagnostics
        .as_ref()
        .map(|diagnostics| match_metrics(event.confidence, diagnostics))
        .unwrap_or_default();
    let band = event.canvas_band_height.unwrap_or(0);
    let delta_start = match event.branch {
        StitchBranch::Append => event
            .before
            .canvas_height
            .saturating_sub(band.saturating_sub(event.growth)),
        StitchBranch::Prepend => 0,
        StitchBranch::Contained | StitchBranch::Skip | StitchBranch::NoMovement => 0,
    };
    FrameOutcome {
        event: frame_event,
        matched_reference_offset: event.accepted_offset,
        unmatched_reason,
        metrics,
        added_extent: event.growth,
        output_dimensions: dimensions,
        delta_start,
        delta_extent: band,
    }
}

fn unmatched_reason(diagnostics: Option<&MotionDiagnostics>) -> UnmatchedReason {
    match diagnostics.map(|diagnostics| diagnostics.stage) {
        Some(MotionStage::InputTooSmall | MotionStage::EmptyDescriptors) => {
            UnmatchedReason::LowInformation
        }
        Some(MotionStage::NoMatches) => UnmatchedReason::InsufficientOverlap,
        Some(MotionStage::NoCandidates) => UnmatchedReason::ConflictingReferences,
        Some(MotionStage::LowConfidence) => UnmatchedReason::Ambiguous,
        Some(MotionStage::SceneCut) => UnmatchedReason::FixedContentDominated,
        Some(MotionStage::SelectedNoMotion) => UnmatchedReason::VerificationFailed,
        Some(MotionStage::IdenticalInterior | MotionStage::Selected) | None => {
            UnmatchedReason::VerificationFailed
        }
    }
}

fn match_metrics(confidence: Option<f32>, diagnostics: &MotionDiagnostics) -> MatchMetrics {
    let selected = diagnostics.selected_offset.and_then(|offset| {
        diagnostics
            .candidates
            .iter()
            .find(|candidate| candidate.offset == offset)
    });
    let mut scores = diagnostics
        .candidates
        .iter()
        .map(|candidate| candidate.score)
        .collect::<Vec<_>>();
    scores.sort_by(|left, right| right.total_cmp(left));
    let total_tiles = diagnostics.regions.fixed_tiles
        + diagnostics.regions.scrolling_tiles
        + diagnostics.regions.dynamic_tiles
        + diagnostics.regions.neutral_tiles;
    MatchMetrics {
        score: confidence.unwrap_or_default(),
        second_score: scores.get(1).copied().unwrap_or_default(),
        content_coverage: selected.map_or(0.0, |candidate| candidate.spatial_coverage),
        fixed_coverage: if total_tiles == 0 {
            0.0
        } else {
            diagnostics.regions.fixed_tiles as f32 / total_tiles as f32
        },
        inlier_ratio: selected.map_or(0.0, |candidate| candidate.weighted_inlier_share),
        feature_support: selected.map_or(0, |candidate| candidate.raw_inliers),
        reference_count: 1,
    }
}

#[derive(Clone, Copy, Default)]
enum PngCompression {
    #[default]
    Fast,
    Balanced,
    Best,
}

#[derive(Clone, Copy)]
enum PngExportStage {
    Preparing,
    Encoding,
    Committing,
}

#[derive(Clone, Copy)]
struct PngExportProgress {
    stage: PngExportStage,
    rows_written: u32,
    total_rows: u32,
    percent: f32,
}

struct PngExportRequest {
    output_path: PathBuf,
    compression: PngCompression,
    overwrite: bool,
}

#[derive(Debug)]
enum PngExportError {
    Canceled,
    Failed(String),
}

impl std::fmt::Display for PngExportError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Canceled => formatter.write_str("PNG export canceled"),
            Self::Failed(message) => formatter.write_str(message),
        }
    }
}

struct PngExportTask {
    cancel: Arc<AtomicBool>,
    progress: Receiver<PngExportProgress>,
    join: Option<JoinHandle<Result<(), PngExportError>>>,
}

impl PngExportTask {
    fn spawn(
        snapshot: StitchImageSnapshot,
        request: PngExportRequest,
    ) -> Result<Self, StitchError> {
        if request.output_path.as_os_str().is_empty() {
            return Err(StitchError::InvalidOptions {
                message: "PNG output path must not be empty".to_owned(),
            });
        }
        if request.output_path.exists() && !request.overwrite {
            return Err(StitchError::InvalidFrame {
                message: format!(
                    "PNG output already exists: {}",
                    request.output_path.display()
                ),
            });
        }
        let parent = request
            .output_path
            .parent()
            .unwrap_or_else(|| Path::new("."));
        if !parent.is_dir() {
            return Err(StitchError::InvalidFrame {
                message: format!("PNG output directory does not exist: {}", parent.display()),
            });
        }
        let cancel = Arc::new(AtomicBool::new(false));
        let worker_cancel = Arc::clone(&cancel);
        let (progress_tx, progress) = sync_channel(16);
        let join = thread::Builder::new()
            .name("snow-stitch-png-export".to_owned())
            .spawn(move || export_png(snapshot, request, worker_cancel, progress_tx))
            .map_err(|error| StitchError::InvalidFrame {
                message: format!("failed to spawn PNG export worker: {error}"),
            })?;
        Ok(Self {
            cancel,
            progress,
            join: Some(join),
        })
    }

    fn cancel(&self) {
        self.cancel.store(true, Ordering::Release);
    }

    fn try_progress(&self) -> Result<PngExportProgress, TryRecvError> {
        self.progress.try_recv()
    }

    fn is_finished(&self) -> bool {
        self.join.as_ref().is_some_and(JoinHandle::is_finished)
    }

    fn wait(mut self) -> Result<(), PngExportError> {
        self.join
            .take()
            .expect("live export task has a worker")
            .join()
            .map_err(|_| PngExportError::Failed("PNG export worker panicked".to_owned()))?
    }
}

impl Drop for PngExportTask {
    fn drop(&mut self) {
        self.cancel();
        if let Some(join) = self.join.take() {
            let _ = join.join();
        }
    }
}

impl StitchImageSnapshot {
    fn export_png(&self, request: PngExportRequest) -> Result<PngExportTask, StitchError> {
        PngExportTask::spawn(self.clone(), request)
    }
}

fn export_png(
    snapshot: StitchImageSnapshot,
    request: PngExportRequest,
    cancel: Arc<AtomicBool>,
    progress: SyncSender<PngExportProgress>,
) -> Result<(), PngExportError> {
    let emit = |stage, rows_written| {
        let total_rows = snapshot.height();
        let percent = rows_written as f32 * 100.0 / total_rows as f32;
        let _ = progress.try_send(PngExportProgress {
            stage,
            rows_written,
            total_rows,
            percent,
        });
    };
    if cancel.load(Ordering::Acquire) {
        return Err(PngExportError::Canceled);
    }
    emit(PngExportStage::Preparing, 0);

    let parent = request
        .output_path
        .parent()
        .unwrap_or_else(|| Path::new("."));
    let mut temporary = tempfile::Builder::new()
        .prefix(".snow-shot-")
        .suffix(".png.tmp")
        .tempfile_in(parent)
        .map_err(|error| PngExportError::Failed(error.to_string()))?;
    {
        let mut encoder =
            png::Encoder::new(temporary.as_file_mut(), snapshot.width(), snapshot.height());
        encoder.set_color(png::ColorType::Rgba);
        encoder.set_depth(png::BitDepth::Eight);
        encoder.set_filter(png::Filter::Adaptive);
        encoder.set_compression(match request.compression {
            PngCompression::Fast => png::Compression::Fast,
            PngCompression::Balanced => png::Compression::Balanced,
            PngCompression::Best => png::Compression::High,
        });
        let mut writer = encoder
            .write_header()
            .map_err(|error| PngExportError::Failed(error.to_string()))?;
        {
            let mut stream = writer
                .stream_writer_with_size(64 * 1024)
                .map_err(|error| PngExportError::Failed(error.to_string()))?;
            let row_bytes = snapshot.width() as usize * 4;
            let chunk_bytes = row_bytes.saturating_mul(64).max(row_bytes);
            let mut rows_written = 0;
            let materialized = snapshot
                .materialize()
                .map_err(|error| PngExportError::Failed(error.to_string()))?;
            for chunk in materialized.frame.pixels().chunks(chunk_bytes) {
                if cancel.load(Ordering::Acquire) {
                    return Err(PngExportError::Canceled);
                }
                stream
                    .write_all(chunk)
                    .map_err(|error| PngExportError::Failed(error.to_string()))?;
                rows_written += (chunk.len() / row_bytes) as u32;
                emit(
                    PngExportStage::Encoding,
                    rows_written.min(snapshot.height()),
                );
            }
            stream
                .finish()
                .map_err(|error| PngExportError::Failed(error.to_string()))?;
        }
        writer
            .finish()
            .map_err(|error| PngExportError::Failed(error.to_string()))?;
    }
    temporary
        .as_file_mut()
        .flush()
        .map_err(|error| PngExportError::Failed(error.to_string()))?;
    temporary
        .as_file()
        .sync_all()
        .map_err(|error| PngExportError::Failed(error.to_string()))?;
    if cancel.load(Ordering::Acquire) {
        return Err(PngExportError::Canceled);
    }
    emit(PngExportStage::Committing, snapshot.height());
    if request.overwrite {
        temporary.persist(&request.output_path)
    } else {
        temporary.persist_noclobber(&request.output_path)
    }
    .map_err(|error| PngExportError::Failed(error.error.to_string()))?;
    Ok(())
}

pub struct SnowStitchFramePoolImpl {
    pool: RgbaFramePool,
}

pub struct SnowStitchFrameBufferImpl {
    buffer: Option<RgbaFrameBuffer>,
}

pub struct SnowStitchSessionImpl {
    session: StitchSession,
}

pub struct SnowStitchSnapshotImpl {
    snapshot: StitchImageSnapshot,
}

pub struct SnowStitchOwnedImageImpl {
    image: RgbaImage,
}

pub struct SnowStitchExportTaskImpl {
    task: Option<PngExportTask>,
    status: SnowStitchExportStatus,
    error: CString,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub enum SnowStitchFrameEvent {
    Initial = 0,
    ExtendedTop = 1,
    ExtendedBottom = 2,
    Covered = 3,
    Duplicate = 4,
    Unmatched = 5,
    ExtendedLeft = 6,
    ExtendedRight = 7,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub enum SnowStitchAxis {
    Vertical = 0,
    Horizontal = 1,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub enum SnowStitchUnmatchedReason {
    None = 0,
    InsufficientOverlap = 1,
    LowInformation = 2,
    Ambiguous = 3,
    ConflictingReferences = 4,
    FixedContentDominated = 5,
    VerificationFailed = 6,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SnowStitchConfig {
    struct_size: u32,
    max_output_height: u32,
    max_output_pixels: u64,
    min_overlap_rows: u32,
    min_overlap_ratio: f32,
    accepted_history_capacity: u32,
    axis: u32,
    reserved: [u32; 8],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SnowStitchMatchMetrics {
    score: f32,
    second_score: f32,
    content_coverage: f32,
    fixed_coverage: f32,
    inlier_ratio: f32,
    feature_support: u32,
    reference_count: u32,
}

#[repr(C)]
pub struct SnowStitchMutableImageInfo {
    width: u32,
    height: u32,
    stride_bytes: u32,
    rgba_bytes: *mut u8,
    rgba_len: usize,
}

#[repr(C)]
pub struct SnowStitchImageInfo {
    width: u32,
    height: u32,
    stride_bytes: u32,
    rgba_bytes: *const u8,
    rgba_len: usize,
}

#[repr(C)]
pub struct SnowStitchFrameOutcome {
    event: SnowStitchFrameEvent,
    unmatched_reason: SnowStitchUnmatchedReason,
    matched_reference_offset_y: i32,
    has_matched_reference_offset_y: u8,
    reserved: [u8; 3],
    metrics: SnowStitchMatchMetrics,
    added_rows: u32,
    output_width: u32,
    output_height: u32,
    delta_top: u32,
    delta_rows: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub enum SnowStitchPngCompression {
    Fast = 0,
    Balanced = 1,
    Best = 2,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub enum SnowStitchExportStage {
    Preparing = 0,
    Encoding = 1,
    Committing = 2,
}

#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum SnowStitchExportStatus {
    Running = 0,
    Complete = 1,
    Failed = 2,
    Canceled = 3,
}

#[repr(C)]
pub struct SnowStitchPngExportConfig {
    output_path_utf8: *const c_char,
    compression: SnowStitchPngCompression,
    overwrite: u8,
    reserved: [u8; 31],
}

#[repr(C)]
pub struct SnowStitchExportProgress {
    stage: SnowStitchExportStage,
    rows_written: u32,
    total_rows: u32,
    percent: f32,
}

thread_local! {
    static LAST_ERROR: RefCell<CString> = RefCell::new(CString::new("").expect("empty C string"));
}

fn sanitize_cstring(value: impl AsRef<str>) -> CString {
    let bytes = value
        .as_ref()
        .as_bytes()
        .iter()
        .copied()
        .filter(|byte| *byte != 0)
        .collect::<Vec<_>>();
    CString::new(bytes).expect("interior NUL bytes were removed")
}

fn set_last_error(error: impl ToString) {
    LAST_ERROR.with(|slot| *slot.borrow_mut() = sanitize_cstring(error.to_string()));
}

fn clear_last_error() {
    LAST_ERROR.with(|slot| *slot.borrow_mut() = CString::new("").expect("empty C string"));
}

fn ffi_config(config: StitchConfig) -> SnowStitchConfig {
    SnowStitchConfig {
        struct_size: size_of::<SnowStitchConfig>() as u32,
        max_output_height: config.limits.max_output_height,
        max_output_pixels: config.limits.max_output_pixels,
        min_overlap_rows: config.min_overlap_rows,
        min_overlap_ratio: config.min_overlap_ratio,
        accepted_history_capacity: config.accepted_history_capacity as u32,
        axis: match config.axis {
            StitchAxis::Vertical => SnowStitchAxis::Vertical as u32,
            StitchAxis::Horizontal => SnowStitchAxis::Horizontal as u32,
        },
        reserved: [0; 8],
    }
}

fn rust_config(config: &SnowStitchConfig) -> Result<StitchConfig, String> {
    if config.struct_size < size_of::<SnowStitchConfig>() as u32 {
        return Err(format!(
            "SnowStitchConfig.struct_size is {}, expected at least {}",
            config.struct_size,
            size_of::<SnowStitchConfig>()
        ));
    }
    let accepted_history_capacity = usize::try_from(config.accepted_history_capacity)
        .map_err(|_| "accepted_history_capacity does not fit size_t".to_owned())?;
    let axis = match config.axis {
        value if value == SnowStitchAxis::Vertical as u32 => StitchAxis::Vertical,
        value if value == SnowStitchAxis::Horizontal as u32 => StitchAxis::Horizontal,
        value => return Err(format!("unknown SnowStitchAxis value {value}")),
    };
    let config = StitchConfig {
        axis,
        limits: StitchLimits {
            max_output_height: config.max_output_height,
            max_output_pixels: config.max_output_pixels,
        },
        min_overlap_rows: config.min_overlap_rows,
        min_overlap_ratio: config.min_overlap_ratio,
        accepted_history_capacity,
    };
    config.validate().map_err(|error| error.to_string())?;
    Ok(config)
}

fn ffi_event(event: FrameEvent) -> SnowStitchFrameEvent {
    match event {
        FrameEvent::Initial => SnowStitchFrameEvent::Initial,
        FrameEvent::ExtendedTop => SnowStitchFrameEvent::ExtendedTop,
        FrameEvent::ExtendedBottom => SnowStitchFrameEvent::ExtendedBottom,
        FrameEvent::ExtendedLeft => SnowStitchFrameEvent::ExtendedLeft,
        FrameEvent::ExtendedRight => SnowStitchFrameEvent::ExtendedRight,
        FrameEvent::Covered => SnowStitchFrameEvent::Covered,
        FrameEvent::Duplicate => SnowStitchFrameEvent::Duplicate,
        FrameEvent::Unmatched => SnowStitchFrameEvent::Unmatched,
    }
}

fn ffi_unmatched_reason(reason: Option<UnmatchedReason>) -> SnowStitchUnmatchedReason {
    match reason {
        None => SnowStitchUnmatchedReason::None,
        Some(UnmatchedReason::InsufficientOverlap) => {
            SnowStitchUnmatchedReason::InsufficientOverlap
        }
        Some(UnmatchedReason::LowInformation) => SnowStitchUnmatchedReason::LowInformation,
        Some(UnmatchedReason::Ambiguous) => SnowStitchUnmatchedReason::Ambiguous,
        Some(UnmatchedReason::ConflictingReferences) => {
            SnowStitchUnmatchedReason::ConflictingReferences
        }
        Some(UnmatchedReason::FixedContentDominated) => {
            SnowStitchUnmatchedReason::FixedContentDominated
        }
        Some(UnmatchedReason::VerificationFailed) => SnowStitchUnmatchedReason::VerificationFailed,
    }
}

fn ffi_metrics(metrics: MatchMetrics) -> SnowStitchMatchMetrics {
    SnowStitchMatchMetrics {
        score: metrics.score,
        second_score: metrics.second_score,
        content_coverage: metrics.content_coverage,
        fixed_coverage: metrics.fixed_coverage,
        inlier_ratio: metrics.inlier_ratio,
        feature_support: metrics.feature_support,
        reference_count: metrics.reference_count,
    }
}

fn ffi_outcome(outcome: FrameOutcome) -> SnowStitchFrameOutcome {
    SnowStitchFrameOutcome {
        event: ffi_event(outcome.event),
        unmatched_reason: ffi_unmatched_reason(outcome.unmatched_reason),
        matched_reference_offset_y: outcome.matched_reference_offset.unwrap_or_default(),
        has_matched_reference_offset_y: u8::from(outcome.matched_reference_offset.is_some()),
        reserved: [0; 3],
        metrics: ffi_metrics(outcome.metrics),
        added_rows: outcome.added_extent,
        output_width: outcome.output_dimensions.width,
        output_height: outcome.output_dimensions.height,
        delta_top: outcome.delta_start,
        delta_rows: outcome.delta_extent,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn snow_stitch_frame_pool_create(
    width: u32,
    height: u32,
    capacity: usize,
) -> *mut SnowStitchFramePoolImpl {
    match RgbaFramePool::new(width, height, capacity) {
        Ok(pool) => {
            clear_last_error();
            Box::into_raw(Box::new(SnowStitchFramePoolImpl { pool }))
        }
        Err(error) => {
            set_last_error(error);
            ptr::null_mut()
        }
    }
}

#[unsafe(no_mangle)]
/// # Safety
/// `pool` must be null or a live handle returned by `snow_stitch_frame_pool_create`
/// that has not already been destroyed.
pub unsafe extern "C" fn snow_stitch_frame_pool_destroy(pool: *mut SnowStitchFramePoolImpl) {
    if !pool.is_null() {
        drop(unsafe { Box::from_raw(pool) });
    }
}

#[unsafe(no_mangle)]
/// # Safety
/// `pool` must point to a live frame pool for the duration of this call.
pub unsafe extern "C" fn snow_stitch_frame_pool_acquire(
    pool: *mut SnowStitchFramePoolImpl,
) -> *mut SnowStitchFrameBufferImpl {
    let Some(pool) = (unsafe { pool.as_ref() }) else {
        set_last_error("RGBA frame pool is null");
        return ptr::null_mut();
    };
    match pool.pool.acquire() {
        Ok(buffer) => {
            clear_last_error();
            Box::into_raw(Box::new(SnowStitchFrameBufferImpl {
                buffer: Some(buffer),
            }))
        }
        Err(error) => {
            set_last_error(error);
            ptr::null_mut()
        }
    }
}

#[unsafe(no_mangle)]
/// # Safety
/// `frame` must be a live, exclusively accessed frame handle and `out_info`
/// must point to writable storage for one `SnowStitchMutableImageInfo`.
pub unsafe extern "C" fn snow_stitch_frame_buffer_info(
    frame: *mut SnowStitchFrameBufferImpl,
    out_info: *mut SnowStitchMutableImageInfo,
) -> u8 {
    if out_info.is_null() {
        set_last_error("mutable image info is null");
        return 0;
    }
    let Some(frame) = (unsafe { frame.as_mut() }) else {
        set_last_error("RGBA frame buffer is null");
        return 0;
    };
    let Some(buffer) = frame.buffer.as_mut() else {
        set_last_error("RGBA frame buffer was already consumed");
        return 0;
    };
    let dimensions = buffer.dimensions();
    let bytes = buffer.as_mut_rgba_bytes();
    unsafe {
        *out_info = SnowStitchMutableImageInfo {
            width: dimensions.width,
            height: dimensions.height,
            stride_bytes: dimensions.width * 4,
            rgba_bytes: bytes.as_mut_ptr(),
            rgba_len: bytes.len(),
        };
    }
    clear_last_error();
    1
}

#[unsafe(no_mangle)]
/// # Safety
/// `frame` must be null or a live handle returned by this library that has not
/// been consumed or previously destroyed.
pub unsafe extern "C" fn snow_stitch_frame_buffer_destroy(frame: *mut SnowStitchFrameBufferImpl) {
    if !frame.is_null() {
        drop(unsafe { Box::from_raw(frame) });
    }
}

#[unsafe(no_mangle)]
/// # Safety
/// `out_config` must point to writable storage for one
/// `SnowStitchConfig`.
pub unsafe extern "C" fn snow_stitch_config_default(out_config: *mut SnowStitchConfig) -> u8 {
    if out_config.is_null() {
        set_last_error("stitch config is null");
        return 0;
    }
    unsafe { *out_config = ffi_config(StitchConfig::default()) };
    clear_last_error();
    1
}

#[unsafe(no_mangle)]
/// # Safety
/// `config` must point to a readable `SnowStitchConfig` whose
/// `struct_size` covers the current structure.
pub unsafe extern "C" fn snow_stitch_session_create(
    config: *const SnowStitchConfig,
) -> *mut SnowStitchSessionImpl {
    let Some(config) = (unsafe { config.as_ref() }) else {
        set_last_error("stitch config is null");
        return ptr::null_mut();
    };
    let result = rust_config(config)
        .and_then(|config| StitchSession::new(config).map_err(|error| error.to_string()));
    match result {
        Ok(session) => {
            clear_last_error();
            Box::into_raw(Box::new(SnowStitchSessionImpl { session }))
        }
        Err(error) => {
            set_last_error(error);
            ptr::null_mut()
        }
    }
}

#[unsafe(no_mangle)]
/// # Safety
/// `session` must be null or a live session handle that has not already been
/// destroyed.
pub unsafe extern "C" fn snow_stitch_session_destroy(session: *mut SnowStitchSessionImpl) {
    if !session.is_null() {
        drop(unsafe { Box::from_raw(session) });
    }
}

#[unsafe(no_mangle)]
/// # Safety
/// `session` must point to a live, exclusively accessed session.
pub unsafe extern "C" fn snow_stitch_session_reset(session: *mut SnowStitchSessionImpl) -> u8 {
    let Some(session) = (unsafe { session.as_mut() }) else {
        set_last_error("stitch session is null");
        return 0;
    };
    session.session.reset();
    clear_last_error();
    1
}

#[unsafe(no_mangle)]
/// # Safety
/// `session` must be live and exclusively accessed. `inout_frame` must point
/// to a live frame-handle slot and `out_outcome` must be writable. The frame
/// handle is consumed and the caller's slot is nulled even when stitching fails.
pub unsafe extern "C" fn snow_stitch_session_push_owned(
    session: *mut SnowStitchSessionImpl,
    inout_frame: *mut *mut SnowStitchFrameBufferImpl,
    out_outcome: *mut SnowStitchFrameOutcome,
) -> u8 {
    if inout_frame.is_null() || out_outcome.is_null() {
        set_last_error("owned frame pointer and outcome must be non-null");
        return 0;
    }
    let frame_ptr = unsafe { *inout_frame };
    unsafe { *inout_frame = ptr::null_mut() };
    if frame_ptr.is_null() {
        set_last_error("owned frame is null");
        return 0;
    }
    let mut frame = unsafe { Box::from_raw(frame_ptr) };
    let Some(buffer) = frame.buffer.take() else {
        set_last_error("owned frame was already consumed");
        return 0;
    };
    let Some(session) = (unsafe { session.as_mut() }) else {
        set_last_error("stitch session is null");
        return 0;
    };

    match session.session.push_owned_frame(buffer.freeze()) {
        Ok(outcome) => {
            unsafe { *out_outcome = ffi_outcome(outcome) };
            clear_last_error();
            1
        }
        Err(error) => {
            set_last_error(error);
            0
        }
    }
}

#[unsafe(no_mangle)]
/// # Safety
/// `session` must be live and exclusively accessed. `rgba_bytes` must remain
/// readable for `rgba_len` bytes for the duration of this call and
/// `out_outcome` must be writable.
pub unsafe extern "C" fn snow_stitch_session_push_rgba(
    session: *mut SnowStitchSessionImpl,
    width: u32,
    height: u32,
    stride_bytes: u32,
    rgba_bytes: *const u8,
    rgba_len: usize,
    out_outcome: *mut SnowStitchFrameOutcome,
) -> u8 {
    if rgba_bytes.is_null() || out_outcome.is_null() {
        set_last_error("RGBA pixels and outcome must be non-null");
        return 0;
    }
    let Some(session) = (unsafe { session.as_mut() }) else {
        set_last_error("stitch session is null");
        return 0;
    };
    let pixels = unsafe { slice::from_raw_parts(rgba_bytes, rgba_len) };
    let frame = match Frame::from_strided(
        width,
        height,
        PixelFormat::Rgba8,
        stride_bytes as usize,
        pixels,
    ) {
        Ok(frame) => frame,
        Err(error) => {
            set_last_error(error);
            return 0;
        }
    };
    match session.session.push_owned_frame(Ok(frame)) {
        Ok(outcome) => {
            unsafe { *out_outcome = ffi_outcome(outcome) };
            clear_last_error();
            1
        }
        Err(error) => {
            set_last_error(error);
            0
        }
    }
}

#[unsafe(no_mangle)]
/// # Safety
/// `session` must remain live and unmodified during the call. `destination`
/// must be writable for exactly `destination_len` bytes.
pub unsafe extern "C" fn snow_stitch_session_copy_rows(
    session: *const SnowStitchSessionImpl,
    top: u32,
    rows: u32,
    destination: *mut u8,
    destination_len: usize,
) -> u8 {
    if destination.is_null() {
        set_last_error("row destination is null");
        return 0;
    }
    let Some(session) = (unsafe { session.as_ref() }) else {
        set_last_error("stitch session is null");
        return 0;
    };
    let bytes = unsafe { slice::from_raw_parts_mut(destination, destination_len) };
    match session.session.copy_rows(top, rows, bytes) {
        Ok(()) => {
            clear_last_error();
            1
        }
        Err(error) => {
            set_last_error(error);
            0
        }
    }
}

#[unsafe(no_mangle)]
/// # Safety
/// `session` must remain live and unmodified during the call.
pub unsafe extern "C" fn snow_stitch_session_materialize_rows(
    session: *const SnowStitchSessionImpl,
    top: u32,
    bottom: u32,
) -> *mut SnowStitchOwnedImageImpl {
    let Some(session) = (unsafe { session.as_ref() }) else {
        set_last_error("stitch session is null");
        return ptr::null_mut();
    };
    owned_image(session.session.materialize_rows(top, bottom))
}

#[unsafe(no_mangle)]
/// # Safety
/// `session` must remain live and unmodified during the call.
pub unsafe extern "C" fn snow_stitch_session_materialize_axis(
    session: *const SnowStitchSessionImpl,
    start: u32,
    end: u32,
) -> *mut SnowStitchOwnedImageImpl {
    let Some(session) = (unsafe { session.as_ref() }) else {
        set_last_error("stitch session is null");
        return ptr::null_mut();
    };
    owned_image(session.session.materialize_axis(start, end))
}

#[unsafe(no_mangle)]
/// # Safety
/// `session` must remain live and unmodified during the call.
pub unsafe extern "C" fn snow_stitch_session_render_scaled_rows(
    session: *const SnowStitchSessionImpl,
    top: u32,
    rows: u32,
    width: u32,
    height: u32,
) -> *mut SnowStitchOwnedImageImpl {
    let Some(session) = (unsafe { session.as_ref() }) else {
        set_last_error("stitch session is null");
        return ptr::null_mut();
    };
    owned_image(session.session.render_scaled_rows(top, rows, width, height))
}

#[unsafe(no_mangle)]
/// # Safety
/// `session` must remain live and unmodified during the call.
pub unsafe extern "C" fn snow_stitch_session_render_scaled_axis(
    session: *const SnowStitchSessionImpl,
    start: u32,
    span: u32,
    width: u32,
    height: u32,
) -> *mut SnowStitchOwnedImageImpl {
    let Some(session) = (unsafe { session.as_ref() }) else {
        set_last_error("stitch session is null");
        return ptr::null_mut();
    };
    owned_image(
        session
            .session
            .render_scaled_axis(start, span, width, height),
    )
}

#[unsafe(no_mangle)]
/// # Safety
/// `session` must remain live and unmodified for the duration of the call.
pub unsafe extern "C" fn snow_stitch_session_snapshot(
    session: *const SnowStitchSessionImpl,
    trim_top: u32,
    trim_bottom: u32,
) -> *mut SnowStitchSnapshotImpl {
    let Some(session) = (unsafe { session.as_ref() }) else {
        set_last_error("stitch session is null");
        return ptr::null_mut();
    };
    match session.session.snapshot(trim_top, trim_bottom) {
        Ok(snapshot) => {
            clear_last_error();
            Box::into_raw(Box::new(SnowStitchSnapshotImpl { snapshot }))
        }
        Err(error) => {
            set_last_error(error);
            ptr::null_mut()
        }
    }
}

#[unsafe(no_mangle)]
/// # Safety
/// `session` must remain live and unmodified for the duration of the call.
pub unsafe extern "C" fn snow_stitch_session_snapshot_axis(
    session: *const SnowStitchSessionImpl,
    start: u32,
    end: u32,
) -> *mut SnowStitchSnapshotImpl {
    let Some(session) = (unsafe { session.as_ref() }) else {
        set_last_error("stitch session is null");
        return ptr::null_mut();
    };
    match session.session.snapshot(start, end) {
        Ok(snapshot) => {
            clear_last_error();
            Box::into_raw(Box::new(SnowStitchSnapshotImpl { snapshot }))
        }
        Err(error) => {
            set_last_error(error);
            ptr::null_mut()
        }
    }
}

#[unsafe(no_mangle)]
/// # Safety
/// `snapshot` must be null or a live snapshot handle that has not already been
/// destroyed.
pub unsafe extern "C" fn snow_stitch_snapshot_destroy(snapshot: *mut SnowStitchSnapshotImpl) {
    if !snapshot.is_null() {
        drop(unsafe { Box::from_raw(snapshot) });
    }
}

#[unsafe(no_mangle)]
/// # Safety
/// `snapshot` must remain live during the call and `out_info` must point to
/// writable storage for one `SnowStitchImageInfo`.
pub unsafe extern "C" fn snow_stitch_snapshot_info(
    snapshot: *const SnowStitchSnapshotImpl,
    out_info: *mut SnowStitchImageInfo,
) -> u8 {
    if out_info.is_null() {
        set_last_error("snapshot image info is null");
        return 0;
    }
    let Some(snapshot) = (unsafe { snapshot.as_ref() }) else {
        set_last_error("stitch snapshot is null");
        return 0;
    };
    unsafe {
        *out_info = SnowStitchImageInfo {
            width: snapshot.snapshot.width(),
            height: snapshot.snapshot.height(),
            stride_bytes: snapshot.snapshot.width() * 4,
            rgba_bytes: ptr::null(),
            rgba_len: snapshot.snapshot.rgba_len().unwrap_or(0),
        };
    }
    clear_last_error();
    1
}

#[unsafe(no_mangle)]
/// # Safety
/// `snapshot` must remain live and unmodified during the call. `destination`
/// must be writable for `destination_len` bytes.
pub unsafe extern "C" fn snow_stitch_snapshot_copy_rows(
    snapshot: *const SnowStitchSnapshotImpl,
    top: u32,
    rows: u32,
    destination_stride: usize,
    destination: *mut u8,
    destination_len: usize,
) -> u8 {
    if destination.is_null() {
        set_last_error("snapshot row destination is null");
        return 0;
    }
    let Some(snapshot) = (unsafe { snapshot.as_ref() }) else {
        set_last_error("stitch snapshot is null");
        return 0;
    };
    let bytes = unsafe { slice::from_raw_parts_mut(destination, destination_len) };
    match snapshot
        .snapshot
        .canvas
        .copy_rows_strided(top, rows, destination_stride, bytes)
    {
        Ok(()) => {
            clear_last_error();
            1
        }
        Err(error) => {
            set_last_error(error);
            0
        }
    }
}

#[unsafe(no_mangle)]
/// # Safety
/// `snapshot` must remain live and unmodified for the duration of the call.
pub unsafe extern "C" fn snow_stitch_snapshot_slice_rows(
    snapshot: *const SnowStitchSnapshotImpl,
    top: u32,
    bottom: u32,
) -> *mut SnowStitchSnapshotImpl {
    let Some(snapshot) = (unsafe { snapshot.as_ref() }) else {
        set_last_error("stitch snapshot is null");
        return ptr::null_mut();
    };
    match snapshot.snapshot.slice_rows(top, bottom) {
        Ok(snapshot) => {
            clear_last_error();
            Box::into_raw(Box::new(SnowStitchSnapshotImpl { snapshot }))
        }
        Err(error) => {
            set_last_error(error);
            ptr::null_mut()
        }
    }
}

#[unsafe(no_mangle)]
/// # Safety
/// `snapshot` must remain live and unmodified for the duration of the call.
pub unsafe extern "C" fn snow_stitch_snapshot_slice_axis(
    snapshot: *const SnowStitchSnapshotImpl,
    start: u32,
    end: u32,
) -> *mut SnowStitchSnapshotImpl {
    let Some(snapshot) = (unsafe { snapshot.as_ref() }) else {
        set_last_error("stitch snapshot is null");
        return ptr::null_mut();
    };
    match snapshot.snapshot.slice_axis(start, end) {
        Ok(snapshot) => {
            clear_last_error();
            Box::into_raw(Box::new(SnowStitchSnapshotImpl { snapshot }))
        }
        Err(error) => {
            set_last_error(error);
            ptr::null_mut()
        }
    }
}

fn owned_image(image: Result<RgbaImage, impl ToString>) -> *mut SnowStitchOwnedImageImpl {
    match image {
        Ok(image) => {
            clear_last_error();
            Box::into_raw(Box::new(SnowStitchOwnedImageImpl { image }))
        }
        Err(error) => {
            set_last_error(error);
            ptr::null_mut()
        }
    }
}

#[unsafe(no_mangle)]
/// # Safety
/// `snapshot` must remain live and unmodified for the duration of the call.
pub unsafe extern "C" fn snow_stitch_snapshot_materialize(
    snapshot: *const SnowStitchSnapshotImpl,
) -> *mut SnowStitchOwnedImageImpl {
    let Some(snapshot) = (unsafe { snapshot.as_ref() }) else {
        set_last_error("stitch snapshot is null");
        return ptr::null_mut();
    };
    owned_image(snapshot.snapshot.materialize())
}

#[unsafe(no_mangle)]
/// # Safety
/// `snapshot` must remain live and unmodified for the duration of the call.
pub unsafe extern "C" fn snow_stitch_snapshot_render_scaled(
    snapshot: *const SnowStitchSnapshotImpl,
    width: u32,
    height: u32,
) -> *mut SnowStitchOwnedImageImpl {
    let Some(snapshot) = (unsafe { snapshot.as_ref() }) else {
        set_last_error("stitch snapshot is null");
        return ptr::null_mut();
    };
    owned_image(snapshot.snapshot.render_scaled(width, height))
}

#[unsafe(no_mangle)]
/// # Safety
/// `image` must remain live during the call and `out_info` must point to
/// writable storage for one `SnowStitchImageInfo`.
pub unsafe extern "C" fn snow_stitch_owned_image_info(
    image: *const SnowStitchOwnedImageImpl,
    out_info: *mut SnowStitchImageInfo,
) -> u8 {
    if out_info.is_null() {
        set_last_error("owned image info is null");
        return 0;
    }
    let Some(image) = (unsafe { image.as_ref() }) else {
        set_last_error("owned image is null");
        return 0;
    };
    let bytes = image.image.as_rgba_bytes();
    unsafe {
        *out_info = SnowStitchImageInfo {
            width: image.image.width(),
            height: image.image.height(),
            stride_bytes: image.image.width() * 4,
            rgba_bytes: bytes.as_ptr(),
            rgba_len: bytes.len(),
        };
    }
    clear_last_error();
    1
}

#[unsafe(no_mangle)]
/// # Safety
/// `image` must be null or a live owned-image handle that has not already been
/// destroyed.
pub unsafe extern "C" fn snow_stitch_owned_image_destroy(image: *mut SnowStitchOwnedImageImpl) {
    if !image.is_null() {
        drop(unsafe { Box::from_raw(image) });
    }
}

#[unsafe(no_mangle)]
/// # Safety
/// `snapshot` and `config` must remain live during the call. The path pointer
/// in `config` must reference a valid NUL-terminated UTF-8 string.
pub unsafe extern "C" fn snow_stitch_snapshot_export_png(
    snapshot: *const SnowStitchSnapshotImpl,
    config: *const SnowStitchPngExportConfig,
) -> *mut SnowStitchExportTaskImpl {
    let Some(snapshot) = (unsafe { snapshot.as_ref() }) else {
        set_last_error("stitch snapshot is null");
        return ptr::null_mut();
    };
    let Some(config) = (unsafe { config.as_ref() }) else {
        set_last_error("PNG export config is null");
        return ptr::null_mut();
    };
    if config.output_path_utf8.is_null() {
        set_last_error("PNG output path is null");
        return ptr::null_mut();
    }
    let path = match unsafe { CStr::from_ptr(config.output_path_utf8) }.to_str() {
        Ok(path) => PathBuf::from(path),
        Err(error) => {
            set_last_error(error);
            return ptr::null_mut();
        }
    };
    let request = PngExportRequest {
        output_path: path,
        compression: match config.compression {
            SnowStitchPngCompression::Fast => PngCompression::Fast,
            SnowStitchPngCompression::Balanced => PngCompression::Balanced,
            SnowStitchPngCompression::Best => PngCompression::Best,
        },
        overwrite: config.overwrite != 0,
    };
    match snapshot.snapshot.export_png(request) {
        Ok(task) => {
            clear_last_error();
            Box::into_raw(Box::new(SnowStitchExportTaskImpl {
                task: Some(task),
                status: SnowStitchExportStatus::Running,
                error: CString::new("").expect("empty C string"),
            }))
        }
        Err(error) => {
            set_last_error(error);
            ptr::null_mut()
        }
    }
}

fn ffi_progress(progress: PngExportProgress) -> SnowStitchExportProgress {
    SnowStitchExportProgress {
        stage: match progress.stage {
            PngExportStage::Preparing => SnowStitchExportStage::Preparing,
            PngExportStage::Encoding => SnowStitchExportStage::Encoding,
            PngExportStage::Committing => SnowStitchExportStage::Committing,
        },
        rows_written: progress.rows_written,
        total_rows: progress.total_rows,
        percent: progress.percent,
    }
}

fn finish_export(task: &mut SnowStitchExportTaskImpl, block: bool) {
    if task.status != SnowStitchExportStatus::Running {
        return;
    }
    let should_finish = block || task.task.as_ref().is_some_and(PngExportTask::is_finished);
    if !should_finish {
        return;
    }
    let Some(worker) = task.task.take() else {
        return;
    };
    match worker.wait() {
        Ok(_) => task.status = SnowStitchExportStatus::Complete,
        Err(PngExportError::Canceled) => {
            task.status = SnowStitchExportStatus::Canceled;
            task.error = sanitize_cstring("PNG export canceled");
        }
        Err(error) => {
            task.status = SnowStitchExportStatus::Failed;
            task.error = sanitize_cstring(error.to_string());
        }
    }
}

#[unsafe(no_mangle)]
/// # Safety
/// `task` must be null or point to a live export task.
pub unsafe extern "C" fn snow_stitch_export_task_cancel(task: *mut SnowStitchExportTaskImpl) {
    if let Some(task) = unsafe { task.as_ref() }
        && let Some(worker) = &task.task
    {
        worker.cancel();
    }
}

#[unsafe(no_mangle)]
/// # Safety
/// `task` must be live and exclusively accessed. Non-null output pointers must
/// point to writable values of their corresponding C types.
pub unsafe extern "C" fn snow_stitch_export_task_poll(
    task: *mut SnowStitchExportTaskImpl,
    out_progress: *mut SnowStitchExportProgress,
    out_has_progress: *mut u8,
) -> SnowStitchExportStatus {
    let Some(task) = (unsafe { task.as_mut() }) else {
        set_last_error("PNG export task is null");
        return SnowStitchExportStatus::Failed;
    };
    if !out_has_progress.is_null() {
        unsafe { *out_has_progress = 0 };
    }
    if let Some(worker) = &task.task
        && let Ok(progress) = worker.try_progress()
    {
        if !out_progress.is_null() {
            unsafe { *out_progress = ffi_progress(progress) };
        }
        if !out_has_progress.is_null() {
            unsafe { *out_has_progress = 1 };
        }
    }
    finish_export(task, false);
    task.status
}

#[unsafe(no_mangle)]
/// # Safety
/// `task` must point to a live, exclusively accessed export task.
pub unsafe extern "C" fn snow_stitch_export_task_wait(
    task: *mut SnowStitchExportTaskImpl,
) -> SnowStitchExportStatus {
    let Some(task) = (unsafe { task.as_mut() }) else {
        set_last_error("PNG export task is null");
        return SnowStitchExportStatus::Failed;
    };
    finish_export(task, true);
    task.status
}

#[unsafe(no_mangle)]
/// # Safety
/// `task` must remain live while the returned message pointer is read.
pub unsafe extern "C" fn snow_stitch_export_task_error_message(
    task: *const SnowStitchExportTaskImpl,
) -> *const c_char {
    unsafe { task.as_ref() }.map_or(ptr::null(), |task| task.error.as_ptr())
}

#[unsafe(no_mangle)]
/// # Safety
/// `task` must be null or a live export task that has not already been
/// destroyed.
pub unsafe extern "C" fn snow_stitch_export_task_destroy(task: *mut SnowStitchExportTaskImpl) {
    if !task.is_null() {
        drop(unsafe { Box::from_raw(task) });
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn snow_stitch_last_error_message() -> *const c_char {
    LAST_ERROR.with(|slot| slot.borrow().as_ptr())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn config_is_sized_and_rejects_truncated_callers() {
        let mut config = ffi_config(StitchConfig::default());
        assert_eq!(config.struct_size as usize, size_of::<SnowStitchConfig>());
        config.struct_size -= 1;
        let session = unsafe { snow_stitch_session_create(&config) };
        assert!(session.is_null());
    }

    #[test]
    fn config_defaults_vertical_and_rejects_unknown_axis() {
        let mut config = ffi_config(StitchConfig::default());
        assert_eq!(config.axis, SnowStitchAxis::Vertical as u32);
        config.axis = 99;
        let session = unsafe { snow_stitch_session_create(&config) };
        assert!(session.is_null());
    }

    #[test]
    fn direct_rgba_push_accepts_strided_input() {
        let config = ffi_config(StitchConfig::default());
        let session = unsafe { snow_stitch_session_create(&config) };
        assert!(!session.is_null());
        let mut pixels = vec![0_u8; 5 * 24];
        for row in pixels.chunks_exact_mut(24) {
            row[..20].fill(0x55);
            row[20..].fill(0xee);
        }
        let mut outcome = SnowStitchFrameOutcome {
            event: SnowStitchFrameEvent::Unmatched,
            unmatched_reason: SnowStitchUnmatchedReason::None,
            matched_reference_offset_y: 0,
            has_matched_reference_offset_y: 0,
            reserved: [0; 3],
            metrics: ffi_metrics(MatchMetrics::default()),
            added_rows: 0,
            output_width: 0,
            output_height: 0,
            delta_top: 0,
            delta_rows: 0,
        };
        assert_eq!(
            unsafe {
                snow_stitch_session_push_rgba(
                    session,
                    5,
                    5,
                    24,
                    pixels.as_ptr(),
                    pixels.len(),
                    &mut outcome,
                )
            },
            1
        );
        assert!(matches!(outcome.event, SnowStitchFrameEvent::Initial));
        assert_eq!((outcome.output_width, outcome.output_height), (5, 5));
        unsafe { snow_stitch_session_destroy(session) };
    }

    #[test]
    fn owned_frame_is_consumed_and_snapshot_survives_session() {
        let pool = snow_stitch_frame_pool_create(5, 5, 2);
        let config = ffi_config(StitchConfig::default());
        let session = unsafe { snow_stitch_session_create(&config) };
        assert!(!pool.is_null() && !session.is_null());
        let mut frame = unsafe { snow_stitch_frame_pool_acquire(pool) };
        let mut info = SnowStitchMutableImageInfo {
            width: 0,
            height: 0,
            stride_bytes: 0,
            rgba_bytes: ptr::null_mut(),
            rgba_len: 0,
        };
        assert_eq!(
            unsafe { snow_stitch_frame_buffer_info(frame, &mut info) },
            1
        );
        unsafe { slice::from_raw_parts_mut(info.rgba_bytes, info.rgba_len).fill(0x7f) };
        let mut outcome = SnowStitchFrameOutcome {
            event: SnowStitchFrameEvent::Unmatched,
            unmatched_reason: SnowStitchUnmatchedReason::None,
            matched_reference_offset_y: 0,
            has_matched_reference_offset_y: 0,
            reserved: [0; 3],
            metrics: ffi_metrics(MatchMetrics::default()),
            added_rows: 0,
            output_width: 0,
            output_height: 0,
            delta_top: 0,
            delta_rows: 0,
        };
        assert_eq!(
            unsafe { snow_stitch_session_push_owned(session, &mut frame, &mut outcome) },
            1
        );
        assert!(frame.is_null());
        let snapshot = unsafe { snow_stitch_session_snapshot(session, 0, 5) };
        assert!(!snapshot.is_null());
        let slice = unsafe { snow_stitch_snapshot_slice_rows(snapshot, 0, 1) };
        assert!(!slice.is_null());
        assert!(unsafe { snow_stitch_snapshot_slice_rows(snapshot, 1, 1) }.is_null());
        unsafe { snow_stitch_session_destroy(session) };
        let mut copied = [0_u8; 24];
        assert_eq!(
            unsafe {
                snow_stitch_snapshot_copy_rows(
                    slice,
                    0,
                    1,
                    copied.len(),
                    copied.as_mut_ptr(),
                    copied.len(),
                )
            },
            1
        );
        assert!(copied[..20].iter().all(|byte| *byte == 0x7f));
        assert!(copied[20..].iter().all(|byte| *byte == 0));
        let image = unsafe { snow_stitch_snapshot_materialize(snapshot) };
        assert!(!image.is_null());
        let scaled = unsafe { snow_stitch_snapshot_render_scaled(slice, 1, 3) };
        assert!(!scaled.is_null());
        let mut scaled_info = SnowStitchImageInfo {
            width: 0,
            height: 0,
            stride_bytes: 0,
            rgba_bytes: ptr::null(),
            rgba_len: 0,
        };
        assert_eq!(
            unsafe { snow_stitch_owned_image_info(scaled, &mut scaled_info) },
            1
        );
        assert_eq!((scaled_info.width, scaled_info.height), (1, 3));
        assert!(
            unsafe { slice::from_raw_parts(scaled_info.rgba_bytes, scaled_info.rgba_len) }
                .iter()
                .all(|byte| *byte == 0x7f)
        );
        unsafe {
            snow_stitch_owned_image_destroy(scaled);
            snow_stitch_owned_image_destroy(image);
            snow_stitch_snapshot_destroy(slice);
            snow_stitch_snapshot_destroy(snapshot);
            snow_stitch_frame_pool_destroy(pool);
        }
    }

    #[test]
    fn snapshot_export_writes_png_and_honors_no_clobber() {
        let temporary = tempfile::tempdir().unwrap();
        let output = temporary.path().join("stitched.png");
        let snapshot = StitchImageSnapshot {
            canvas: TiledCanvasSnapshot::from_frame(
                Frame::new(5, 5, PixelFormat::Rgba8, vec![0x7f; 5 * 5 * 4]).unwrap(),
            ),
        };
        let task = snapshot
            .export_png(PngExportRequest {
                output_path: output.clone(),
                compression: PngCompression::Best,
                overwrite: false,
            })
            .unwrap();

        task.wait().unwrap();

        assert_eq!(&std::fs::read(&output).unwrap()[..8], b"\x89PNG\r\n\x1a\n");
        assert!(
            snapshot
                .export_png(PngExportRequest {
                    output_path: output,
                    compression: PngCompression::Fast,
                    overwrite: false,
                })
                .is_err()
        );
    }
}
