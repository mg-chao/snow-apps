#![allow(clippy::too_many_arguments)]

pub mod config;
pub mod editing;
pub mod error;
pub mod export;
pub mod streaming;

pub(crate) mod ffmpeg_util;
pub mod video_quality;

pub use config::{
    ExportAudioOutputConfig, ExportAudioTrackRequest, ExportExecutionMode, ExportFormat,
    ExportPerformanceConfig, ExportRequest, MouseEditConfig, SoftwareH264Priority, VideoCodec,
};
pub use editing::EditingSession;
pub use error::RecordingExportError;
pub use export::{
    ExportPathKind, ExportProgress, ExportResult, ExportRuntimeReport, ExportStage,
    ExportStageDurationsMs, ExportTask,
};
pub use streaming::{
    StreamingAudioConfig, StreamingEncoder, StreamingEncoderConfig, StreamingEncoderReport,
    StreamingPixelOrder, cleanup_stale_staging_files, scaled_output_dimensions,
};
