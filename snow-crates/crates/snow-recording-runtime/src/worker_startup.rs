use std::thread::JoinHandle;

use crate::error::{Result, ScreenRecorderError};

/// Publish a runtime only after readiness. On failure, join before releasing
/// controls so the next session cannot overlap a retiring capture worker.
pub(crate) fn started_worker<T>(
    worker: JoinHandle<Result<T>>,
    ready: bool,
    name: &str,
) -> Result<JoinHandle<Result<T>>> {
    if ready {
        return Ok(worker);
    }
    match worker.join() {
        Ok(Err(error)) => Err(error),
        Ok(Ok(_)) => Err(ScreenRecorderError::Encode(format!(
            "{name} worker stopped before reporting readiness"
        ))),
        Err(_) => Err(ScreenRecorderError::Encode(format!(
            "{name} worker panicked during initialization"
        ))),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use snow_capture::error::CaptureError;
    use std::sync::mpsc;

    #[test]
    fn startup_preserves_capture_errors_and_joins_resource_cleanup() {
        let (ready, receiver) = mpsc::sync_channel::<()>(1);
        let (released, resources) = mpsc::channel();
        let worker = std::thread::spawn(move || -> Result<()> {
            drop(ready);
            released.send(()).unwrap();
            Err(CaptureError::BackendUnavailable("preferred and fallback failed".into()).into())
        });
        let error = started_worker(worker, receiver.recv().is_ok(), "test").unwrap_err();
        assert!(
            matches!(error, ScreenRecorderError::Capture(CaptureError::BackendUnavailable(message))
            if message == "preferred and fallback failed")
        );
        assert!(resources.try_recv().is_ok());
    }

    #[test]
    fn startup_preserves_non_capture_error_types() {
        let worker = std::thread::spawn(|| -> Result<()> {
            Err(std::io::Error::new(std::io::ErrorKind::PermissionDenied, "output denied").into())
        });
        assert!(matches!(started_worker(worker, false, "test"),
            Err(ScreenRecorderError::Io(error)) if error.kind() == std::io::ErrorKind::PermissionDenied));
    }

    #[test]
    fn startup_requires_readiness_even_when_worker_returns_success() {
        let worker = std::thread::spawn(|| Ok(()));
        assert!(matches!(started_worker(worker, false, "test"),
            Err(ScreenRecorderError::Encode(message)) if message.contains("before reporting readiness")));
        let worker = std::thread::spawn(|| -> Result<()> { panic!("startup panic") });
        assert!(matches!(started_worker(worker, false, "test"),
            Err(ScreenRecorderError::Encode(message)) if message.contains("panicked during initialization")));
    }

    #[test]
    fn startup_retains_worker_for_stop_and_runtime_failure() {
        let (ready, receiver) = mpsc::sync_channel(1);
        let (stop, commands) = mpsc::channel();
        let worker = std::thread::spawn(move || -> Result<()> {
            ready.send(()).unwrap();
            commands.recv().unwrap();
            Err(CaptureError::AccessLost.into())
        });
        let worker = started_worker(worker, receiver.recv().is_ok(), "test").unwrap();
        stop.send(()).unwrap();
        assert!(matches!(
            worker.join().unwrap(),
            Err(ScreenRecorderError::Capture(CaptureError::AccessLost))
        ));
    }
}
