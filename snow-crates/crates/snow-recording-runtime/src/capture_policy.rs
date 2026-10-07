//! Recording backends are preferences; exact backend selection belongs to capture APIs.
#[cfg(windows)]
use snow_capture::backend::AutoBackendPolicy;
use snow_capture::backend::CaptureBackendKind;
use snow_capture::{CaptureSystem, error::CaptureResult};

#[derive(Clone, Copy)]
pub(crate) enum RecordingCapturePath {
    Direct,
    Buffered,
}

pub(crate) fn recording_capture_system(
    path: RecordingCapturePath,
    preferred: CaptureBackendKind,
) -> CaptureResult<CaptureSystem> {
    let builder = CaptureSystem::builder();
    #[cfg(windows)]
    let builder = builder
        .with_backend_kind(CaptureBackendKind::Auto)
        .with_auto_backend_policy(recording_backend_policy(path, preferred));
    #[cfg(not(windows))]
    let builder = {
        let _ = path;
        builder.with_backend_kind(preferred)
    };
    builder.build()
}

#[cfg(windows)]
fn recording_backend_policy(
    path: RecordingCapturePath,
    preferred: CaptureBackendKind,
) -> AutoBackendPolicy {
    // Keep the measured defaults of each pipeline. A user preference moves one
    // candidate to the front without removing initialization/acquisition fallback.
    let defaults = match path {
        RecordingCapturePath::Direct => [
            CaptureBackendKind::DxgiDuplication,
            CaptureBackendKind::WindowsGraphicsCapture,
            CaptureBackendKind::Gdi,
        ],
        RecordingCapturePath::Buffered => [
            CaptureBackendKind::WindowsGraphicsCapture,
            CaptureBackendKind::DxgiDuplication,
            CaptureBackendKind::Gdi,
        ],
    };
    let mut priority = Vec::with_capacity(4);
    if preferred != CaptureBackendKind::Auto {
        priority.push(preferred);
    }
    priority.extend(defaults.into_iter().filter(|kind| *kind != preferred));
    AutoBackendPolicy { priority }
}

#[cfg(windows)]
pub(crate) fn recording_gpu_backends(preferred: CaptureBackendKind) -> Vec<CaptureBackendKind> {
    // Try native surfaces in the same order as CPU capture, stopping when a
    // CPU-only backend has priority. GDI preference must reach GDI first.
    recording_backend_policy(RecordingCapturePath::Direct, preferred)
        .priority
        .into_iter()
        .take_while(|kind| {
            matches!(
                kind,
                CaptureBackendKind::DxgiDuplication | CaptureBackendKind::WindowsGraphicsCapture
            )
        })
        .collect()
}

#[cfg(windows)]
pub(crate) fn try_recording_gpu_capture<T>(
    preferred: CaptureBackendKind,
    mut attempt: impl FnMut(CaptureBackendKind) -> CaptureResult<T>,
) -> CaptureResult<Option<T>> {
    let mut last_error = None;
    for backend in recording_gpu_backends(preferred) {
        match attempt(backend) {
            Ok(capture) => return Ok(Some(capture)),
            Err(error) if error.allows_backend_fallback() => last_error = Some(error),
            Err(error) => return Err(error),
        }
    }
    match last_error {
        Some(error) => Err(error),
        None => Ok(None),
    }
}

#[cfg(all(test, windows))]
mod tests {
    use super::*;

    #[test]
    fn recording_preferences_retain_every_fallback_in_both_pipelines() {
        use CaptureBackendKind::{
            Auto, DxgiDuplication as Dxgi, Gdi, WindowsGraphicsCapture as Wgc,
        };
        for (path, defaults) in [
            (RecordingCapturePath::Direct, [Dxgi, Wgc, Gdi]),
            (RecordingCapturePath::Buffered, [Wgc, Dxgi, Gdi]),
        ] {
            for preferred in [Auto, Dxgi, Wgc, Gdi] {
                let priority = recording_backend_policy(path, preferred).normalized_priority();
                let mut expected = defaults.to_vec();
                if preferred != Auto {
                    expected.retain(|kind| *kind != preferred);
                    expected.insert(0, preferred);
                }
                assert_eq!(priority, expected);
                assert_eq!(
                    recording_capture_system(path, preferred)
                        .unwrap()
                        .backend_kind(),
                    Auto
                );
            }
        }
    }

    #[test]
    fn recording_gpu_optimization_honors_the_preferred_backend() {
        use CaptureBackendKind::{
            Auto, DxgiDuplication as Dxgi, Gdi, WindowsGraphicsCapture as Wgc,
        };
        assert_eq!(recording_gpu_backends(Auto), vec![Dxgi, Wgc]);
        assert_eq!(recording_gpu_backends(Dxgi), vec![Dxgi, Wgc]);
        assert_eq!(recording_gpu_backends(Wgc), vec![Wgc, Dxgi]);
        assert!(recording_gpu_backends(Gdi).is_empty());
    }

    #[test]
    fn recording_gpu_acquisition_failure_tries_the_next_backend() {
        use CaptureBackendKind::{DxgiDuplication as Dxgi, WindowsGraphicsCapture as Wgc};
        use snow_capture::error::CaptureError;
        for preferred in [Dxgi, Wgc] {
            let mut attempted = Vec::new();
            let selected = try_recording_gpu_capture(preferred, |backend| {
                attempted.push(backend);
                if backend == preferred {
                    Err(CaptureError::Timeout)
                } else {
                    Ok(backend)
                }
            })
            .unwrap()
            .unwrap();
            assert_eq!(attempted, vec![preferred, selected]);
            assert_ne!(selected, preferred);
        }
    }

    #[test]
    fn recording_gpu_terminal_errors_do_not_try_another_backend() {
        use snow_capture::error::CaptureError;
        let mut attempts = 0;
        let result = try_recording_gpu_capture::<()>(CaptureBackendKind::DxgiDuplication, |_| {
            attempts += 1;
            Err(CaptureError::PermissionDenied)
        });
        assert!(matches!(result, Err(CaptureError::PermissionDenied)));
        assert_eq!(attempts, 1);
        let result = try_recording_gpu_capture::<()>(CaptureBackendKind::Gdi, |_| {
            panic!("GDI must use CPU capture")
        });
        assert!(matches!(result, Ok(None)));
    }

    #[test]
    fn recording_gpu_exhaustion_preserves_the_last_capture_error() {
        use snow_capture::error::CaptureError;
        let mut attempted = Vec::new();
        let result = try_recording_gpu_capture::<()>(
            CaptureBackendKind::WindowsGraphicsCapture,
            |backend| {
                attempted.push(backend);
                Err(CaptureError::BackendUnavailable(format!("{backend:?}")))
            },
        );
        assert_eq!(attempted.len(), 2);
        assert!(
            matches!(result, Err(CaptureError::BackendUnavailable(message)) if message == "DxgiDuplication")
        );
    }
}
