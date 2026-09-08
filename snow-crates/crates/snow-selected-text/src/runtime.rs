#[cfg(windows)]
use std::sync::OnceLock;
use std::sync::atomic::{AtomicBool, Ordering};
#[cfg(any(windows, test))]
use std::sync::mpsc;
use std::sync::{Arc, Condvar, Mutex};
use std::time::Instant;

use crate::model::*;
#[cfg(any(windows, test))]
use crate::policy::{Backend, Context, acquire};

pub(crate) struct RequestState {
    pub deadline: Instant,
    cancelled: AtomicBool,
    pub copy_started: AtomicBool,
    result: Mutex<Option<Arc<CaptureResult>>>,
    ready: Condvar,
}

impl RequestState {
    #[cfg(any(windows, test))]
    pub(crate) fn new(deadline: Instant) -> Self {
        Self {
            deadline,
            cancelled: AtomicBool::new(false),
            copy_started: AtomicBool::new(false),
            result: Mutex::new(None),
            ready: Condvar::new(),
        }
    }

    pub(crate) fn check(&self) -> Result<(), SelectionError> {
        let kind = if self.cancelled.load(Ordering::Acquire) {
            Some(ErrorKind::Cancelled)
        } else if Instant::now() >= self.deadline {
            Some(ErrorKind::TimedOut)
        } else {
            None
        };
        match kind {
            Some(kind) => Err(self.stop_error(kind)),
            None => Ok(()),
        }
    }

    fn stop_error(&self, kind: ErrorKind) -> SelectionError {
        let mut error = SelectionError::new(kind, "capture request");
        if self.copy_started.load(Ordering::Acquire) {
            error.clipboard_status = ClipboardStatus::Unknown;
        }
        error
    }

    #[cfg(any(windows, test))]
    fn finish(&self, result: CaptureResult) {
        let mut slot = self.result.lock().unwrap_or_else(|e| e.into_inner());
        if slot.is_none() {
            *slot = Some(Arc::new(self.check().and(result)));
            self.ready.notify_all();
        }
    }

    fn poll(&self) -> Option<Arc<CaptureResult>> {
        let mut slot = self.result.lock().unwrap_or_else(|e| e.into_inner());
        if slot.is_none()
            && let Err(error) = self.check()
        {
            *slot = Some(Arc::new(Err(error)));
            self.ready.notify_all();
        }
        slot.clone()
    }
}

/// An owned request. Dropping it cancels acquisition but never joins a worker.
pub struct CaptureRequest {
    state: Arc<RequestState>,
}

impl CaptureRequest {
    /// `None` means pending. Terminal results are immutable and shared, not recopied.
    pub fn try_result(&self) -> Option<Arc<CaptureResult>> {
        self.state.poll()
    }

    /// Wait at most until the request deadline. Do not call on a GUI event thread.
    pub fn wait(&self) -> Arc<CaptureResult> {
        loop {
            if let Some(result) = self.try_result() {
                return result;
            }
            let slot = self.state.result.lock().unwrap_or_else(|e| e.into_inner());
            if slot.is_none() {
                let remaining = self
                    .state
                    .deadline
                    .saturating_duration_since(Instant::now());
                drop(
                    self.state
                        .ready
                        .wait_timeout(slot, remaining)
                        .unwrap_or_else(|e| e.into_inner()),
                );
            }
        }
    }

    pub fn cancel(&self) {
        self.state.cancelled.store(true, Ordering::Release);
        self.state.poll();
    }
}

impl Drop for CaptureRequest {
    fn drop(&mut self) {
        self.cancel();
    }
}

#[cfg(any(windows, test))]
struct BusyGuard(Arc<AtomicBool>);
#[cfg(any(windows, test))]
impl Drop for BusyGuard {
    fn drop(&mut self) {
        self.0.store(false, Ordering::Release);
    }
}

#[cfg(any(windows, test))]
struct Job {
    context: Context,
    _busy: BusyGuard,
}

#[cfg(any(windows, test))]
impl Drop for Job {
    fn drop(&mut self) {
        // Also resolve pending callers if an unexpected unwind exits the backend.
        self.context.state.finish(Err(SelectionError::new(
            ErrorKind::WorkerUnavailable,
            "worker exited",
        )));
    }
}

#[cfg(any(windows, test))]
struct Runtime {
    sender: mpsc::SyncSender<Job>,
    busy: Arc<AtomicBool>,
}

#[cfg(any(windows, test))]
impl Runtime {
    fn spawn<B: Backend + 'static>(
        factory: impl FnOnce() -> Result<B, SelectionError> + Send + 'static,
    ) -> Result<Self, SelectionError> {
        let (sender, receiver) = mpsc::sync_channel::<Job>(1);
        std::thread::Builder::new()
            .name("snow-selected-text".into())
            .spawn(move || {
                let mut backend = factory();
                while let Ok(job) = receiver.recv() {
                    let result = match &mut backend {
                        Ok(backend) => acquire(backend, &job.context),
                        Err(error) => Err(error.clone()),
                    };
                    job.context.state.finish(result);
                }
            })
            .map_err(|_| SelectionError::new(ErrorKind::WorkerUnavailable, "worker spawn"))?;
        Ok(Self {
            sender,
            busy: Arc::new(AtomicBool::new(false)),
        })
    }

    fn submit(
        &self,
        options: CaptureOptions,
        capture_source: impl FnOnce() -> Result<SourceWindow, SelectionError>,
    ) -> Result<CaptureRequest, SelectionError> {
        options.validate()?;
        let deadline = Instant::now() + options.timeout;
        self.busy
            .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
            .map_err(|_| SelectionError::new(ErrorKind::Busy, "capture submission"))?;
        let busy = BusyGuard(self.busy.clone());
        let source = capture_source()?;
        if options.excluded_windows.contains(&source.window)
            || options.excluded_windows.contains(&source.focused_control)
            || options
                .excluded_executables
                .iter()
                .any(|name| name.eq_ignore_ascii_case(&source.executable))
        {
            return Err(SelectionError::new(
                ErrorKind::ProtectedContent,
                "excluded application",
            ));
        }
        let state = Arc::new(RequestState::new(deadline));
        let context = Context {
            source,
            options,
            state: state.clone(),
        };
        self.sender
            .try_send(Job {
                context,
                _busy: busy,
            })
            .map_err(|_| SelectionError::new(ErrorKind::WorkerUnavailable, "worker dispatch"))?;
        Ok(CaptureRequest { state })
    }
}

/// Cheap, cloneable access to the process-wide fixed worker runtime.
#[derive(Clone)]
pub struct SelectedTextService {
    #[cfg(windows)]
    runtime: Arc<Runtime>,
}

impl SelectedTextService {
    pub fn new() -> Result<Self, SelectionError> {
        #[cfg(windows)]
        {
            static RUNTIME: OnceLock<Result<Arc<Runtime>, SelectionError>> = OnceLock::new();
            Ok(Self {
                runtime: RUNTIME
                    .get_or_init(|| Runtime::spawn(crate::platform::Worker::new).map(Arc::new))
                    .clone()?,
            })
        }
        #[cfg(not(windows))]
        Err(SelectionError::new(
            ErrorKind::UnsupportedPlatform,
            "service creation",
        ))
    }

    /// Capture foreground context now; perform cross-process acquisition on workers.
    pub fn start_capture(&self, options: CaptureOptions) -> Result<CaptureRequest, SelectionError> {
        #[cfg(windows)]
        {
            self.runtime
                .submit(options, crate::platform::capture_source)
        }
        #[cfg(not(windows))]
        {
            let _ = options;
            Err(SelectionError::new(
                ErrorKind::UnsupportedPlatform,
                "capture",
            ))
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::policy::Probe;
    use std::time::Duration;

    struct BlockedBackend {
        entered: mpsc::Sender<()>,
        release: mpsc::Receiver<()>,
    }
    impl Backend for BlockedBackend {
        fn validate_target(&mut self, _: &Context) -> Result<(), SelectionError> {
            Ok(())
        }
        fn uia(&mut self, _: &Context, _: Instant) -> Result<Probe, SelectionError> {
            self.entered.send(()).unwrap();
            self.release.recv().unwrap();
            Ok(Probe::Empty)
        }
        fn native(&mut self, _: &Context, _: Instant) -> Result<Probe, SelectionError> {
            panic!("late native call")
        }
        fn copy(&mut self, _: &Context) -> CaptureResult {
            panic!("late Copy")
        }
    }

    fn source() -> Result<SourceWindow, SelectionError> {
        Ok(SourceWindow {
            window: 1,
            process_id: 2,
            focused_control: 3,
            executable: "edit.exe".into(),
        })
    }

    fn blocked() -> (Runtime, mpsc::Receiver<()>, mpsc::Sender<()>) {
        let (entered_tx, entered) = mpsc::channel();
        let (release, release_rx) = mpsc::channel();
        let runtime = Runtime::spawn(move || {
            Ok(BlockedBackend {
                entered: entered_tx,
                release: release_rx,
            })
        })
        .unwrap();
        (runtime, entered, release)
    }

    #[test]
    fn cancellation_and_timeout_are_terminal_while_worker_retains_busy_lease() {
        for cancel in [false, true] {
            let (runtime, entered, release) = blocked();
            let request = runtime
                .submit(
                    CaptureOptions {
                        timeout: Duration::from_millis(100),
                        ..Default::default()
                    },
                    source,
                )
                .unwrap();
            entered.recv_timeout(Duration::from_secs(2)).unwrap();
            if cancel {
                request.cancel();
            }
            let result = request.wait();
            let expected = if cancel {
                ErrorKind::Cancelled
            } else {
                ErrorKind::TimedOut
            };
            assert_eq!(result.as_ref().as_ref().unwrap_err().kind, expected);
            assert!(
                matches!(runtime.submit(CaptureOptions::default(), source), Err(e) if e.kind == ErrorKind::Busy)
            );
            // Dropping a timed-out/cancelled request must not wait for this blocked provider.
            drop(request);
            release.send(()).unwrap();
            let end = Instant::now() + Duration::from_secs(2);
            while runtime.busy.load(Ordering::Acquire) && Instant::now() < end {
                std::thread::yield_now();
            }
            assert!(!runtime.busy.load(Ordering::Acquire));
            assert_eq!(result.as_ref().as_ref().unwrap_err().kind, expected);
            let next = runtime.submit(CaptureOptions::default(), source).unwrap();
            entered.recv_timeout(Duration::from_secs(2)).unwrap();
            release.send(()).unwrap();
            assert_eq!(*next.wait(), Ok(SelectionOutcome::NoSelection));
        }
    }

    #[test]
    fn submission_failures_release_busy_and_exclusions_run_before_workers() {
        let (runtime, entered, _) = blocked();
        assert!(
            runtime
                .submit(CaptureOptions::default(), || Err(SelectionError::new(
                    ErrorKind::NoForegroundWindow,
                    "test"
                )))
                .is_err()
        );
        assert!(!runtime.busy.load(Ordering::Acquire));
        for options in [
            CaptureOptions {
                excluded_windows: vec![1],
                ..Default::default()
            },
            CaptureOptions {
                excluded_windows: vec![3],
                ..Default::default()
            },
            CaptureOptions {
                excluded_executables: vec!["EDIT.EXE".into()],
                ..Default::default()
            },
        ] {
            assert!(
                matches!(runtime.submit(options, source), Err(e) if e.kind == ErrorKind::ProtectedContent)
            );
            assert!(!runtime.busy.load(Ordering::Acquire));
        }
        assert!(entered.try_recv().is_err());
    }

    #[test]
    fn completed_results_survive_cancel_and_pending_copy_reports_unknown_cleanup() {
        let state = Arc::new(RequestState::new(Instant::now() + Duration::from_secs(1)));
        let request = CaptureRequest {
            state: state.clone(),
        };
        state.finish(Ok(SelectionOutcome::NoSelection));
        request.cancel();
        assert_eq!(*request.wait(), Ok(SelectionOutcome::NoSelection));
        let state = Arc::new(RequestState::new(Instant::now() + Duration::from_secs(1)));
        state.copy_started.store(true, Ordering::Release);
        let request = CaptureRequest { state };
        request.cancel();
        assert_eq!(
            request
                .wait()
                .as_ref()
                .as_ref()
                .unwrap_err()
                .clipboard_status,
            ClipboardStatus::Unknown
        );
    }

    #[test]
    fn worker_initialization_failure_is_returned_without_waiting_for_deadline() {
        let runtime = Runtime::spawn::<BlockedBackend>(|| {
            Err(SelectionError::new(
                ErrorKind::WorkerUnavailable,
                "initialization",
            ))
        })
        .unwrap();
        let request = runtime.submit(CaptureOptions::default(), source).unwrap();
        assert_eq!(
            request.wait().as_ref().as_ref().unwrap_err().kind,
            ErrorKind::WorkerUnavailable
        );
    }
}
