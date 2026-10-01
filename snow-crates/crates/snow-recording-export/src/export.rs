use snow_core::cancellation::CancellationToken;
use std::path::PathBuf;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread::JoinHandle;

use crate::config::ExportFormat;
use crate::error::{RecordingExportError as ScreenRecorderError, Result};

#[derive(Clone, Debug)]
pub struct ExportResult {
    pub output_path: PathBuf,
    pub duration_ms: u64,
    pub format: ExportFormat,
    pub runtime_report: ExportRuntimeReport,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum ExportPathKind {
    DirectCopy,
    PacketCopy,
    #[default]
    FullTranscode,
}

#[derive(Clone, Debug, Default)]
pub struct ExportStageDurationsMs {
    pub plan: u64,
    pub decode: u64,
    pub compose: u64,
    pub video_encode: u64,
    pub audio_encode: u64,
    pub mux: u64,
    pub finalize: u64,
}

#[derive(Clone, Debug, Default)]
pub struct ExportRuntimeReport {
    pub path: ExportPathKind,
    pub used_hardware_decode: bool,
    pub used_hardware_compose: bool,
    pub used_hardware_encode: bool,
    pub video_decoder: Option<String>,
    pub video_encoder: Option<String>,
    pub audio_encoder: Option<String>,
    pub stage_durations_ms: ExportStageDurationsMs,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ExportStage {
    Plan,
    Decode,
    Compose,
    VideoEncode,
    AudioEncode,
    Mux,
    Finalize,
}

#[derive(Clone, Debug)]
pub struct ExportProgress {
    pub stage: ExportStage,
    pub percent: f32,
    pub video_fps: f32,
    pub eta_ms: Option<u64>,
    pub queue_utilization: f32,
    pub peak_memory_mb: u32,
}

pub struct ExportTask {
    cancel_flag: Arc<AtomicBool>,
    cancellation: CancellationToken,
    progress_rx: crossbeam_channel::Receiver<ExportProgress>,
    join: Option<JoinHandle<Result<ExportResult>>>,
}

impl ExportTask {
    pub(crate) fn new(
        cancel_flag: Arc<AtomicBool>,
        cancellation: CancellationToken,
        progress_rx: crossbeam_channel::Receiver<ExportProgress>,
        join: JoinHandle<Result<ExportResult>>,
    ) -> Self {
        Self {
            cancel_flag,
            cancellation,
            progress_rx,
            join: Some(join),
        }
    }

    pub fn cancel(&self) {
        self.cancellation.cancel();
        self.cancel_flag.store(true, Ordering::Release);
    }

    pub fn progress(&self) -> crossbeam_channel::Receiver<ExportProgress> {
        self.progress_rx.clone()
    }

    pub fn wait(mut self) -> Result<ExportResult> {
        let handle = self.join.take().ok_or_else(|| {
            ScreenRecorderError::Export("export task has already been awaited".to_string())
        })?;
        handle
            .join()
            .map_err(|_| ScreenRecorderError::Export("export task panicked".to_string()))?
    }
}

impl Drop for ExportTask {
    fn drop(&mut self) {
        if let Some(join) = self.join.take() {
            self.cancel();
            let _ = join.join();
        }
    }
}
