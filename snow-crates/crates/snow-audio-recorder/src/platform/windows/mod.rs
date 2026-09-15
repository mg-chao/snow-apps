pub(crate) mod com;
pub(crate) use crate::convert;
pub(crate) mod device_enum;
pub(crate) mod hresult;
pub(crate) mod notification;
pub(crate) mod wasapi_source;

use std::marker::PhantomData;
use std::sync::Arc;
use std::thread::ThreadId;
use std::time::{Duration, Instant};

use windows::Win32::Foundation::{HANDLE, WAIT_FAILED, WAIT_OBJECT_0};
use windows::Win32::System::Threading::WaitForMultipleObjects;

use crate::backend::{AudioBackend, AudioBackendKind, AudioRecorderEngine, EngineEvent};
use crate::device::{AudioDeviceInfo, DeviceFlow};
use crate::error::{AudioError, AudioResult};
use crate::packet::{AudioEvent, AudioSourceKind};
use crate::session::{AudioStreamConfig, SourceConfig};

use self::com::{CoInitGuard, EventHandle};
use self::notification::NotificationClientGuard;
use self::wasapi_source::WasapiSource;

const SOURCE_KINDS: [AudioSourceKind; 2] = [AudioSourceKind::System, AudioSourceKind::Microphone];

/// Per-source deferred retry state. Instead of blocking the worker thread with
/// `thread::sleep`, we record when the next attempt is allowed and let `poll()`
/// keep servicing the healthy source in the meantime.
struct SourceRetryState {
    /// When the retry sequence started (used to report total downtime).
    started: Instant,
    /// How many attempts have been made so far.
    attempts: u32,
    /// Current backoff duration (doubles each attempt, capped at max_backoff).
    current_backoff: Duration,
    /// The earliest `Instant` at which the next attempt is permitted.
    next_attempt_at: Instant,
    /// The last error seen (used when giving up on a required source).
    last_error: Option<AudioError>,
}

pub(crate) struct WasapiBackend;

impl WasapiBackend {
    pub fn new(_kind: AudioBackendKind) -> AudioResult<Self> {
        Ok(Self)
    }
}

impl AudioBackend for WasapiBackend {
    fn enumerate_devices(&self, flow: DeviceFlow) -> AudioResult<Vec<AudioDeviceInfo>> {
        let _coinit = CoInitGuard::init_multithreaded()?;
        let enumerator = device_enum::create_device_enumerator()?;
        device_enum::enumerate_devices(&enumerator, flow)
    }

    fn create_engine(
        &self,
        config: AudioStreamConfig,
    ) -> AudioResult<Box<dyn AudioRecorderEngine>> {
        Ok(Box::new(WasapiEngine::new(config)))
    }
}

/// Holds all COM state. `!Send` and `!Sync` by construction via `PhantomData<*const ()>`,
/// so the compiler prevents this state from being independently sent across threads.
struct ComState {
    _coinit: CoInitGuard,
    enumerator: windows::Win32::Media::Audio::IMMDeviceEnumerator,
    control_event: Arc<EventHandle>,
    notification: NotificationClientGuard,
    system_source: Option<WasapiSource>,
    microphone_source: Option<WasapiSource>,
    /// Ensures `ComState` is `!Send + !Sync` without relying on the inner types' traits.
    _not_send: PhantomData<*const ()>,
}

struct WasapiEngine {
    config: AudioStreamConfig,
    com: Option<ComState>,
    /// The thread that owns the COM state. Set on first `ensure_initialized`
    /// call and asserted on every subsequent call to uphold the safety
    /// invariant of `unsafe impl Send`.
    worker_thread_id: Option<ThreadId>,
    /// Deferred retry state for the system audio source. `Some` while a
    /// non-blocking retry sequence is in progress.
    system_retry: Option<SourceRetryState>,
    /// Deferred retry state for the microphone source.
    microphone_retry: Option<SourceRetryState>,
}

// SAFETY: WasapiEngine is moved into and used by a single dedicated worker thread.
// All COM interfaces are initialized in MTA on that thread and never shared.
// The inner `ComState` is `!Send` by construction, so it cannot be independently
// sent across threads; only the outer engine (which owns it) crosses the thread
// boundary via this impl.
unsafe impl Send for WasapiEngine {}

impl ComState {
    fn source_mut(&mut self, kind: AudioSourceKind) -> &mut Option<WasapiSource> {
        match kind {
            AudioSourceKind::System => &mut self.system_source,
            AudioSourceKind::Microphone => &mut self.microphone_source,
        }
    }

    fn source_ref(&self, kind: AudioSourceKind) -> &Option<WasapiSource> {
        match kind {
            AudioSourceKind::System => &self.system_source,
            AudioSourceKind::Microphone => &self.microphone_source,
        }
    }
}

impl WasapiEngine {
    fn changed_for_kind(
        kind: AudioSourceKind,
        render_changed: bool,
        capture_changed: bool,
    ) -> bool {
        match kind {
            AudioSourceKind::System => render_changed,
            AudioSourceKind::Microphone => capture_changed,
        }
    }

    fn retry_mut(&mut self, kind: AudioSourceKind) -> &mut Option<SourceRetryState> {
        match kind {
            AudioSourceKind::System => &mut self.system_retry,
            AudioSourceKind::Microphone => &mut self.microphone_retry,
        }
    }

    fn retry_ref(&self, kind: AudioSourceKind) -> &Option<SourceRetryState> {
        match kind {
            AudioSourceKind::System => &self.system_retry,
            AudioSourceKind::Microphone => &self.microphone_retry,
        }
    }

    fn source_config(&self, kind: AudioSourceKind) -> &SourceConfig {
        match kind {
            AudioSourceKind::System => &self.config.system,
            AudioSourceKind::Microphone => &self.config.microphone,
        }
    }

    fn has_pending_retry(&self) -> bool {
        SOURCE_KINDS
            .iter()
            .copied()
            .any(|kind| self.retry_ref(kind).is_some())
    }

    fn nearest_retry_delay(&self) -> Option<Duration> {
        let now = Instant::now();
        SOURCE_KINDS
            .iter()
            .copied()
            .filter_map(|kind| self.retry_ref(kind).as_ref())
            .map(|retry| retry.next_attempt_at.saturating_duration_since(now))
            .min()
    }

    fn source_should_rebind_on_default_change(&self, kind: AudioSourceKind, changed: bool) -> bool {
        if !changed {
            return false;
        }
        let config = self.source_config(kind);
        if !config.enabled {
            return false;
        }
        matches!(
            (kind, &config.device),
            (
                AudioSourceKind::System,
                crate::device::DeviceSelector::DefaultRender
            ) | (
                AudioSourceKind::Microphone,
                crate::device::DeviceSelector::DefaultCapture
            )
        )
    }

    fn source_needs_optional_rebind(&self, com: &ComState, kind: AudioSourceKind) -> bool {
        let config = self.source_config(kind);
        com.source_ref(kind).is_none()
            && config.enabled
            && !config.required
            && self.retry_ref(kind).is_none()
    }
}

impl SourceRetryState {
    fn new(initial_backoff: Duration, last_error: Option<AudioError>) -> Self {
        let now = Instant::now();
        Self {
            started: now,
            attempts: 0,
            current_backoff: initial_backoff,
            next_attempt_at: now,
            last_error,
        }
    }

    /// Returns `true` if the backoff deadline has passed and an attempt is due.
    fn is_ready(&self) -> bool {
        Instant::now() >= self.next_attempt_at
    }

    /// Advance backoff state after a failed attempt.
    fn record_failure(&mut self, err: AudioError, max_backoff: Duration) {
        self.attempts += 1;
        self.last_error = Some(err);
        self.next_attempt_at = Instant::now() + self.current_backoff;
        self.current_backoff = self.current_backoff.saturating_mul(2).min(max_backoff);
    }

    fn downtime(&self) -> Duration {
        self.started.elapsed()
    }
}

impl WasapiEngine {
    fn new(config: AudioStreamConfig) -> Self {
        Self {
            config,
            com: None,
            worker_thread_id: None,
            system_retry: None,
            microphone_retry: None,
        }
    }

    fn ensure_initialized(&mut self) -> AudioResult<()> {
        let current_thread = std::thread::current().id();
        if let Some(id) = self.worker_thread_id {
            debug_assert_eq!(
                id, current_thread,
                "WasapiEngine must only be used from its worker thread"
            );
        } else {
            self.worker_thread_id = Some(current_thread);
        }

        if self.com.is_some() {
            return Ok(());
        }

        let coinit = CoInitGuard::init_multithreaded()?;
        let enumerator = device_enum::create_device_enumerator()?;
        let control_event = Arc::new(EventHandle::new_manual_reset(false)?);
        let notification =
            NotificationClientGuard::register(&enumerator, Arc::clone(&control_event))?;

        let system_source =
            Self::try_init_source(AudioSourceKind::System, &self.config.system, &enumerator)?;
        let microphone_source = Self::try_init_source(
            AudioSourceKind::Microphone,
            &self.config.microphone,
            &enumerator,
        )?;

        if system_source.is_none() && microphone_source.is_none() {
            return Err(AudioError::DeviceUnavailable(
                "no enabled audio source could be initialized".into(),
            ));
        }

        self.com = Some(ComState {
            _coinit: coinit,
            enumerator,
            control_event,
            notification,
            system_source,
            microphone_source,
            _not_send: PhantomData,
        });

        Ok(())
    }

    /// Try to initialize a single audio source. Returns `Ok(None)` if the
    /// source is disabled or if initialization fails on a non-required source.
    fn try_init_source(
        kind: AudioSourceKind,
        config: &SourceConfig,
        enumerator: &windows::Win32::Media::Audio::IMMDeviceEnumerator,
    ) -> AudioResult<Option<WasapiSource>> {
        if !config.enabled {
            return Ok(None);
        }
        match WasapiSource::new(kind, config.clone(), enumerator.clone()) {
            Ok(source) => Ok(Some(source)),
            Err(err) if config.required => Err(err),
            Err(_) => Ok(None),
        }
    }

    fn process_control_notifications(&mut self, out: &mut Vec<AudioEvent>) -> AudioResult<()> {
        let com = self.com.as_mut().ok_or(AudioError::WorkerDead)?;
        let changes = com.notification.state().take_changes();

        if !self.config.restart_policy.auto_rebind_on_default_change {
            return Ok(());
        }

        for kind in SOURCE_KINDS {
            let changed = Self::changed_for_kind(
                kind,
                changes.render_default_changed,
                changes.capture_default_changed,
            );
            if self.source_should_rebind_on_default_change(kind, changed) {
                self.begin_retry(kind, None);
            }
        }

        if changes.topology_changed {
            let mut should_retry = [false; SOURCE_KINDS.len()];
            {
                let com = self.com.as_ref().ok_or(AudioError::WorkerDead)?;
                for (idx, kind) in SOURCE_KINDS.iter().copied().enumerate() {
                    should_retry[idx] = self.source_needs_optional_rebind(com, kind);
                }
            }

            for (idx, kind) in SOURCE_KINDS.iter().copied().enumerate() {
                if should_retry[idx] {
                    self.begin_retry(kind, None);
                }
            }
        }

        self.tick_retries(out)?;

        Ok(())
    }

    /// Start a deferred retry sequence for the given source. If a retry is
    /// already in progress it is reset (e.g. a new device-change notification).
    fn begin_retry(&mut self, kind: AudioSourceKind, last_error: Option<AudioError>) {
        if let Some(com) = self.com.as_mut() {
            *com.source_mut(kind) = None;
        }
        let state = SourceRetryState::new(self.config.restart_policy.initial_backoff, last_error);
        *self.retry_mut(kind) = Some(state);
    }

    /// Non-blocking: attempt one retry tick for each source that has a pending
    /// retry and whose backoff deadline has elapsed. Returns immediately if the
    /// deadline hasn't passed yet, allowing the other source to keep flowing.
    fn tick_retries(&mut self, out: &mut Vec<AudioEvent>) -> AudioResult<()> {
        for kind in SOURCE_KINDS {
            self.tick_source_retry(kind, out)?;
        }
        Ok(())
    }

    fn tick_source_retry(
        &mut self,
        kind: AudioSourceKind,
        out: &mut Vec<AudioEvent>,
    ) -> AudioResult<()> {
        if !self.retry_ref(kind).as_ref().is_some_and(|r| r.is_ready()) {
            return Ok(());
        }

        let source_config = self.source_config(kind).clone();
        let max_attempts = self.config.restart_policy.max_attempts.max(1);
        let max_backoff = self.config.restart_policy.max_backoff;
        let downtime = self
            .retry_ref(kind)
            .as_ref()
            .map_or(Duration::ZERO, |r| r.downtime());

        let result = {
            let com = self.com.as_mut().ok_or(AudioError::WorkerDead)?;
            let enumerator = com.enumerator.clone();
            WasapiSource::new(kind, source_config.clone(), enumerator).map(|source| {
                let new_id = source.current_device_id().to_string();
                *com.source_mut(kind) = Some(source);
                new_id
            })
        };

        match result {
            Ok(new_device_id) => {
                *self.retry_mut(kind) = None;
                out.push(AudioEvent::SourceRestarted {
                    source: kind,
                    old_device_id: None,
                    new_device_id,
                    downtime,
                });
            }
            Err(err) => {
                if let Some(state) = self.retry_mut(kind).as_mut() {
                    state.record_failure(err.clone(), max_backoff);

                    if state.attempts >= max_attempts {
                        let last_err = state.last_error.take().unwrap_or_else(|| err.clone());
                        let required = source_config.required;

                        *self.retry_mut(kind) = None;
                        if let Some(com) = self.com.as_mut() {
                            *com.source_mut(kind) = None;
                        }

                        if required {
                            return Err(last_err);
                        }
                    }
                }
            }
        }

        Ok(())
    }

    fn drain_source(
        &mut self,
        kind: AudioSourceKind,
        out: &mut Vec<AudioEvent>,
    ) -> AudioResult<()> {
        if self.retry_ref(kind).is_some() {
            return Ok(());
        }

        let required = self.source_config(kind).required;

        let com = self.com.as_mut().ok_or(AudioError::WorkerDead)?;
        let source = match com.source_mut(kind).as_mut() {
            Some(s) => s,
            None => return Ok(()),
        };

        let result = source.drain_packets();

        match result {
            Ok(packets) => {
                out.extend(packets.into_iter().map(AudioEvent::Packet));
                Ok(())
            }
            Err(err) if err.is_retryable() || err.requires_worker_reset() => {
                self.begin_retry(kind, None);
                self.begin_retry(kind, Some(err.clone()));
                self.tick_source_retry(kind, out)?;
                if self.retry_ref(kind).is_some() {
                    Ok(())
                } else {
                    let source_gone = self
                        .com
                        .as_ref()
                        .is_none_or(|c| c.source_ref(kind).is_none());
                    if source_gone && required {
                        Err(err)
                    } else {
                        Ok(())
                    }
                }
            }
            Err(err) => Err(err),
        }
    }
}

impl AudioRecorderEngine for WasapiEngine {
    fn poll(&mut self, timeout: Duration) -> AudioResult<EngineEvent> {
        self.ensure_initialized()?;

        let com = self.com.as_ref().ok_or(AudioError::WorkerDead)?;
        let mut handles = [HANDLE::default(); SOURCE_KINDS.len() + 1];
        let mut handle_count = 1usize;
        handles[0] = com.control_event.raw();
        for kind in SOURCE_KINDS {
            if let Some(source) = com.source_ref(kind) {
                handles[handle_count] = source.event_handle();
                handle_count += 1;
            }
        }

        // If any source has a pending retry, use a short timeout so we come
        // back quickly to tick the retry without starving the healthy source.
        let has_pending_retry = self.has_pending_retry();
        let effective_timeout = if has_pending_retry {
            // Use the minimum of the requested timeout and the nearest retry
            // deadline so we wake up in time for the next attempt.
            let nearest = self.nearest_retry_delay().unwrap_or(Duration::ZERO);
            timeout.min(nearest.max(Duration::from_millis(1)))
        } else {
            timeout
        };

        let mut events = Vec::new();

        let timeout_ms = effective_timeout.as_millis().min(u128::from(u32::MAX)) as u32;
        let wait_result =
            unsafe { WaitForMultipleObjects(&handles[..handle_count], false, timeout_ms) };

        if wait_result == WAIT_FAILED {
            return Err(AudioError::platform(anyhow::anyhow!(
                "WaitForMultipleObjects failed"
            )));
        }

        if wait_result == WAIT_OBJECT_0 {
            let com = self.com.as_ref().ok_or(AudioError::WorkerDead)?;
            let _ = com.control_event.reset();
            self.process_control_notifications(&mut events)?;
        }

        for kind in SOURCE_KINDS {
            self.drain_source(kind, &mut events)?;
        }

        // Tick any pending retries (the drain methods may have started new ones,
        // or existing ones may have reached their backoff deadline).
        self.tick_retries(&mut events)?;

        if events.is_empty() {
            Ok(EngineEvent::Idle)
        } else {
            Ok(EngineEvent::Events(events))
        }
    }
}
