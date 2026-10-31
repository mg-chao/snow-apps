//! Offscreen regressions through the real display cache, region session and stream.

use super::*;
use crate::backend::{CaptureBackend, MonitorCapturer};
use crate::frame::Frame;
use crate::region::{MonitorGeometry, MonitorLayout};
use crate::{
    CaptureEvent, CaptureOptions, CaptureRegion, CaptureSession, CaptureStream,
    CaptureStreamConfig, CaptureTarget, CaptureWorkload,
};
use std::collections::VecDeque;
use std::sync::atomic::AtomicUsize;

const TEST_TIMEOUT: Duration = Duration::from_secs(5);

struct TestBackend {
    cache: Arc<DisplayInfoCache>,
    monitor: MonitorId,
    checks: crossbeam_channel::Sender<CaptureResult<Option<u64>>>,
}

impl CaptureBackend for TestBackend {
    fn enumerate_monitors(&self) -> CaptureResult<Vec<MonitorId>> {
        Ok(vec![self.monitor.clone()])
    }

    fn primary_monitor(&self) -> CaptureResult<MonitorId> {
        Ok(self.monitor.clone())
    }

    fn checked_display_generation(&self) -> CaptureResult<Option<u64>> {
        let result = self.cache.checked_generation().map(Some);
        self.checks.send(result.clone()).unwrap();
        result
    }

    fn monitor_layout(&self) -> CaptureResult<MonitorLayout> {
        Ok(MonitorLayout {
            monitors: vec![MonitorGeometry {
                monitor: self.monitor.clone(),
                x: 0,
                y: 0,
                width: 2,
                height: 2,
            }],
            virtual_left: 0,
            virtual_top: 0,
            virtual_width: 2,
            virtual_height: 2,
        })
    }

    fn create_monitor_capturer(&self, _: &MonitorId) -> CaptureResult<Box<dyn MonitorCapturer>> {
        Ok(Box::new(TestCapturer))
    }
}

struct TestCapturer;

impl MonitorCapturer for TestCapturer {
    fn capture(&mut self, _: Option<Frame>) -> CaptureResult<Frame> {
        Frame::from_rgba8(2, 2, vec![0; 16])
    }
}

struct StreamFixture {
    stream: CaptureStream,
    cache: Arc<DisplayInfoCache>,
    failures: Arc<Mutex<VecDeque<CaptureError>>>,
    clock: Arc<Mutex<Option<Instant>>>,
    enumerations: Arc<AtomicUsize>,
    checks: crossbeam_channel::Receiver<CaptureResult<Option<u64>>>,
}

impl StreamFixture {
    fn new(max_consecutive_errors: usize) -> Self {
        let failures = Arc::new(Mutex::new(VecDeque::<CaptureError>::new()));
        let enumerations = Arc::new(AtomicUsize::new(0));
        let clock = Arc::new(Mutex::new(None::<Instant>));
        let shared = Arc::new(DisplayCacheShared::new(
            {
                let failures = failures.clone();
                let enumerations = enumerations.clone();
                move || {
                    enumerations.fetch_add(1, Ordering::SeqCst);
                    match failures.lock().unwrap().pop_front() {
                        Some(error) => Err(error),
                        None => Ok(DisplaySnapshot::default()),
                    }
                }
            },
            {
                let clock = clock.clone();
                move || clock.lock().unwrap().unwrap_or_else(Instant::now)
            },
        ));
        // Exercise fallback without real displays or a window listener.
        let cache =
            DisplayInfoCache::with_listener(shared, |_| Err(CaptureError::WorkerDead)).unwrap();
        let (checked, checks) = crossbeam_channel::unbounded();
        let session = CaptureSession::builder()
            .target(CaptureTarget::Region(CaptureRegion {
                x: 0,
                y: 0,
                width: 2,
                height: 2,
            }))
            .with_backend(Arc::new(TestBackend {
                cache: cache.clone(),
                monitor: MonitorId::from_parts(1, 1, 1, "stream-test", true),
                checks: checked,
            }))
            .with_options(CaptureOptions {
                workload: CaptureWorkload::Continuous,
                ..Default::default()
            })
            .build()
            .unwrap();
        let stream = CaptureStream::spawn(
            session,
            CaptureStreamConfig {
                max_consecutive_errors,
                include_cursor: false,
                ..Default::default()
            },
        )
        .unwrap();
        let fixture = Self {
            stream,
            cache,
            failures,
            clock,
            enumerations,
            checks,
        };
        fixture.next_frame();
        fixture
    }

    fn next_frame(&self) {
        loop {
            match self.stream.recv_timeout(TEST_TIMEOUT).unwrap() {
                CaptureEvent::Frame(_) => return,
                CaptureEvent::Error(error) => panic!("stream must recover: {error}"),
                CaptureEvent::StreamEnded => panic!("stream ended before recovery"),
                _ => {}
            }
        }
    }

    fn pause(&self) {
        self.stream.pause();
        loop {
            match self.stream.recv_timeout(TEST_TIMEOUT).unwrap() {
                CaptureEvent::Paused { .. } => return,
                CaptureEvent::Error(error) => panic!("control must remain responsive: {error}"),
                CaptureEvent::StreamEnded => panic!("stream ended before pause"),
                _ => {}
            }
        }
    }

    fn inject(&self, errors: impl IntoIterator<Item = CaptureError>, long_cooldown: bool) -> usize {
        // Synchronize at a control event, so no frame can race with failure injection.
        self.pause();
        while self.checks.try_recv().is_ok() {}
        self.failures.lock().unwrap().extend(errors);
        if long_cooldown {
            // A virtual clock puts the retry beyond the test deadline. Control and
            // cancellation must wake the stream; elapsed wall time cannot make it pass.
            *self.clock.lock().unwrap() = Some(Instant::now() + Duration::from_secs(60));
        }
        let before = self.enumerations.load(Ordering::SeqCst);
        self.cache.shared.invalidate();
        self.stream.resume();
        assert!(self.checks.recv_timeout(TEST_TIMEOUT).unwrap().is_err());
        before
    }

    fn terminal_error(&self) -> CaptureError {
        let error = loop {
            match self.stream.recv_timeout(TEST_TIMEOUT).unwrap() {
                CaptureEvent::Error(error) => break error,
                CaptureEvent::StreamEnded => panic!("missing terminal error"),
                _ => {}
            }
        };
        assert!(matches!(
            self.stream.recv_timeout(TEST_TIMEOUT).unwrap(),
            CaptureEvent::StreamEnded
        ));
        error
    }
}

fn native_error(code: windows::core::HRESULT) -> CaptureError {
    CaptureError::platform(
        anyhow::Error::new(windows::core::Error::from_hresult(code))
            .context("EnumOutputs on adapter 0 failed"),
    )
}

fn assert_stream_recovers_after_display_failure(cause: CaptureError) {
    let fixture = StreamFixture::new(2);
    let before = fixture.inject([cause], false);
    fixture.next_frame();
    assert_eq!(fixture.enumerations.load(Ordering::SeqCst), before + 2);
    assert_eq!(fixture.stream.stats().snapshot().errors_recovered, 1);
}

#[test]
fn stream_recovers_after_native_display_failure() {
    use windows::Win32::Graphics::Dxgi::DXGI_ERROR_ACCESS_LOST;
    assert_stream_recovers_after_display_failure(native_error(DXGI_ERROR_ACCESS_LOST));
}

#[test]
fn stream_recovers_after_retryable_display_failure() {
    assert_stream_recovers_after_display_failure(CaptureError::AccessLost);
}

#[test]
fn persistent_display_failure_stops_after_distinct_attempts() {
    use windows::Win32::Graphics::Dxgi::DXGI_ERROR_DEVICE_REMOVED;

    let fixture = StreamFixture::new(2);
    let before = fixture.inject(
        [
            native_error(DXGI_ERROR_DEVICE_REMOVED),
            native_error(DXGI_ERROR_DEVICE_REMOVED),
        ],
        false,
    );
    let error = fixture.terminal_error();
    assert!(error.is_retryable());
    assert!(
        error
            .to_string()
            .contains("EnumOutputs on adapter 0 failed")
    );
    assert_eq!(fixture.enumerations.load(Ordering::SeqCst), before + 2);
    assert_eq!(fixture.stream.stats().snapshot().errors_recovered, 2);
}

#[test]
fn terminal_display_errors_are_not_retried_or_hidden() {
    use windows::Win32::Foundation::{E_ACCESSDENIED, E_INVALIDARG};

    for cause in [
        native_error(E_ACCESSDENIED),
        native_error(E_INVALIDARG),
        CaptureError::platform(anyhow::anyhow!("unknown failure")),
    ] {
        let message = cause.to_string();
        let fixture = StreamFixture::new(2);
        let before = fixture.inject([cause], false);
        let error = fixture.terminal_error();
        assert!(!error.is_retryable());
        assert_eq!(error.to_string(), message);
        assert_eq!(fixture.enumerations.load(Ordering::SeqCst), before + 1);
        assert_eq!(fixture.stream.stats().snapshot().errors_recovered, 0);
    }
}

#[test]
fn control_wakeups_do_not_exhaust_display_retries_and_stop_interrupts_wait() {
    let fixture = StreamFixture::new(2);
    let before = fixture.inject([CaptureError::AccessLost], true);
    for index in 0..40 {
        fixture.stream.set_target_fps(30 + index % 2);
        assert!(fixture.checks.recv_timeout(TEST_TIMEOUT).unwrap().is_err());
    }
    fixture.pause();
    assert_eq!(fixture.stream.stats().snapshot().errors_recovered, 1);
    fixture.stream.resume();
    assert!(fixture.checks.recv_timeout(TEST_TIMEOUT).unwrap().is_err());
    let (done, finished) = crossbeam_channel::bounded(1);
    let enumerations = fixture.enumerations.clone();
    std::thread::spawn(move || done.send(fixture.stream.stop_and_drain()).unwrap());
    let events = finished.recv_timeout(TEST_TIMEOUT).unwrap();
    assert!(
        events
            .iter()
            .any(|event| matches!(event, CaptureEvent::StreamEnded))
    );
    assert!(
        !events
            .iter()
            .any(|event| matches!(event, CaptureEvent::Error(_)))
    );
    assert_eq!(enumerations.load(Ordering::SeqCst), before + 1);
}

#[test]
fn notification_recovery_resets_stream_retry_budget() {
    let fixture = StreamFixture::new(2);
    for recovered in 1..=2 {
        let before = fixture.inject([CaptureError::AccessLost], true);
        fixture.cache.shared.invalidate();
        fixture.stream.set_target_fps(30 + recovered as u32);
        fixture.next_frame();
        assert_eq!(fixture.enumerations.load(Ordering::SeqCst), before + 2);
        assert_eq!(
            fixture.stream.stats().snapshot().errors_recovered,
            recovered
        );
    }
}
