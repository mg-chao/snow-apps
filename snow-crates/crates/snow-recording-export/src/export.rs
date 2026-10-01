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

/// Owns the export worker. Dropping an unawaited task cancels and joins it.
pub struct ExportTask {
    cancel_flag: Arc<AtomicBool>,
    cancellation: CancellationToken,
    progress_rx: crossbeam_channel::Receiver<ExportProgress>,
    join: Option<JoinHandle<Result<ExportResult>>>,
}

impl Drop for ExportTask {
    fn drop(&mut self) {
        if let Some(worker) = self.join.take() {
            self.cancel();
            let _ = worker.join();
        }
    }
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

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::mpsc;

    #[test]
    fn dropping_export_task_cancels_and_joins_its_worker() {
        let cancel_flag = Arc::new(AtomicBool::new(false));
        let worker_flag = cancel_flag.clone();
        let cancellation = CancellationToken::default();
        let resource = Arc::new(());
        let worker_resource = resource.clone();
        let (started_tx, started_rx) = mpsc::channel();
        let (finished_tx, finished_rx) = mpsc::channel();
        let (_, progress_rx) = crossbeam_channel::unbounded();
        let worker = std::thread::spawn(move || {
            started_tx.send(()).unwrap();
            while !worker_flag.load(Ordering::Acquire) {
                std::thread::yield_now();
            }
            drop(worker_resource);
            finished_tx.send(()).unwrap();
            Err(ScreenRecorderError::ExportCanceled)
        });
        started_rx.recv().unwrap();
        drop(ExportTask::new(
            cancel_flag.clone(),
            cancellation.clone(),
            progress_rx,
            worker,
        ));
        let canceled_on_drop = cancel_flag.load(Ordering::Acquire);
        let released_on_drop = Arc::strong_count(&resource) == 1;
        // Release a detached worker even when the old behavior fails this test.
        cancel_flag.store(true, Ordering::Release);
        finished_rx.recv().unwrap();
        assert!(canceled_on_drop, "dropping a task must cancel its worker");
        assert!(
            cancellation.is_canceled(),
            "dropping a task must cancel deferred work"
        );
        assert!(
            released_on_drop,
            "dropping a task must join resource cleanup"
        );
    }

    #[test]
    fn dropping_export_task_wakes_a_deferred_worker() {
        let cancel_flag = Arc::new(AtomicBool::new(false));
        let cancellation = CancellationToken::default();
        let worker_cancellation = cancellation.clone();
        let (finished_tx, finished_rx) = mpsc::channel();
        let (_, progress_rx) = crossbeam_channel::unbounded();
        let worker = std::thread::spawn(move || {
            let result = worker_cancellation
                .receiver()
                .recv_timeout(std::time::Duration::from_secs(2));
            finished_tx.send(result).unwrap();
            Err(ScreenRecorderError::ExportCanceled)
        });
        drop(ExportTask::new(
            cancel_flag.clone(),
            cancellation,
            progress_rx,
            worker,
        ));
        assert!(matches!(
            finished_rx.recv().unwrap(),
            Err(crossbeam_channel::RecvTimeoutError::Disconnected)
        ));
        assert!(cancel_flag.load(Ordering::Acquire));
    }

    #[test]
    fn waiting_for_export_task_preserves_the_worker_result() {
        let cancellation = CancellationToken::default();
        let cancel_flag = Arc::new(AtomicBool::new(false));
        let (_, progress_rx) = crossbeam_channel::unbounded();
        let worker =
            std::thread::spawn(|| Err(ScreenRecorderError::Export("original failure".into())));
        let result = ExportTask::new(
            cancel_flag.clone(),
            cancellation.clone(),
            progress_rx,
            worker,
        )
        .wait();
        assert!(
            matches!(result, Err(ScreenRecorderError::Export(message)) if message == "original failure")
        );
        assert!(!cancel_flag.load(Ordering::Acquire));
        assert!(!cancellation.is_canceled());
    }
}
