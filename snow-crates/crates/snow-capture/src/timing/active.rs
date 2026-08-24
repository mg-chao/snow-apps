//! Real recorder implementation, compiled only with the `stage-timing`
//! feature.

use std::cell::RefCell;
use std::sync::OnceLock;
use std::time::{Duration, Instant};

use crate::frame::Frame;

/// A single measured stage inside a capture call.
///
/// Stage names are stable `backend.stage` / `readback.stage` labels such as
/// `dxgi.sys.acquire_next_frame` or `readback.convert`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct StageTiming {
    pub name: &'static str,
    pub duration: Duration,
}

struct StageRecorder {
    last_mark: Instant,
    timings: Vec<StageTiming>,
}

thread_local! {
    static STAGE_RECORDER: RefCell<Option<StageRecorder>> = const { RefCell::new(None) };
}

/// RAII guard that owns the stage recorder installed on the current thread.
///
/// Nested scopes are supported: only the outermost scope that installed the
/// recorder receives the recorded entries; inner scopes are transparent.
/// Dropping the scope without calling [`StageScope::finish`] discards the
/// entries and uninstalls the recorder, so early error returns and unwinds
/// cannot leak recorder state into later captures on the same thread.
pub(crate) struct StageScope {
    installed: bool,
}

impl StageScope {
    /// Install a recorder for one capture call when `enabled` is `true`.
    pub(crate) fn enter(enabled: bool) -> Self {
        let installed = enabled
            && STAGE_RECORDER.with_borrow_mut(|slot| {
                if slot.is_some() {
                    return false;
                }
                let now = Instant::now();
                *slot = Some(StageRecorder {
                    last_mark: now,
                    timings: Vec::new(),
                });
                true
            });
        Self { installed }
    }

    /// Remove the recorder and return the recorded entries.
    ///
    /// Returns an empty vector when this scope did not install the recorder
    /// (instrumentation disabled or an outer scope owns it).
    pub(crate) fn finish(mut self) -> Vec<StageTiming> {
        self.take_timings()
    }

    fn take_timings(&mut self) -> Vec<StageTiming> {
        if !self.installed {
            return Vec::new();
        }
        self.installed = false;
        STAGE_RECORDER.with_borrow_mut(|slot| {
            slot.take()
                .map(|recorder| recorder.timings)
                .unwrap_or_default()
        })
    }
}

impl Drop for StageScope {
    fn drop(&mut self) {
        self.take_timings();
    }
}

/// Attach the scope's recorded entries to a frame's metadata.
pub(crate) fn attach_stage_timings(frame: &mut Frame, scope: StageScope) {
    frame.metadata.stage_timings = scope.finish();
}

/// Whether stage timings are currently being recorded on this thread.
pub(crate) fn stage_recording() -> bool {
    STAGE_RECORDER.with_borrow(|slot| slot.is_some())
}

/// Close the span from the previous mark (or scope entry) until now.
pub(crate) fn stage_mark(name: &'static str) {
    STAGE_RECORDER.with_borrow_mut(|slot| {
        let Some(recorder) = slot.as_mut() else {
            return;
        };
        let now = Instant::now();
        let begin = recorder.last_mark;
        recorder.last_mark = now;
        recorder.timings.push(StageTiming {
            name,
            duration: now - begin,
        });
    });
}

/// Append an externally measured span. May overlap mark-based entries.
pub(crate) fn stage_record(name: &'static str, duration: Duration) {
    STAGE_RECORDER.with_borrow_mut(|slot| {
        let Some(recorder) = slot.as_mut() else {
            return;
        };
        recorder.timings.push(StageTiming { name, duration });
    });
}

/// Take a checkpoint to time a call that should only be recorded when a
/// scope is active. Returns `None` when instrumentation is inactive.
pub(crate) fn stage_checkpoint() -> Option<Instant> {
    stage_recording().then(Instant::now)
}

/// Record the span since a checkpoint taken with [`stage_checkpoint`].
pub(crate) fn stage_record_since(name: &'static str, begin: Option<Instant>) {
    if let Some(begin) = begin {
        stage_record(name, begin.elapsed());
    }
}

/// Convert the span between two QPC tick values into a [`Duration`].
///
/// Returns `None` on non-Windows platforms or when the frequency query
/// fails. The frequency is queried once and cached.
#[cfg(target_os = "windows")]
pub(crate) fn qpc_duration_between(earlier_qpc: i64, later_qpc: i64) -> Option<Duration> {
    later_qpc
        .checked_sub(earlier_qpc)
        .filter(|delta| *delta >= 0)
        .and_then(qpc_ticks_to_duration)
}

/// Convert a positive QPC tick count into a [`Duration`].
#[cfg(target_os = "windows")]
pub(crate) fn qpc_ticks_to_duration(ticks: i64) -> Option<Duration> {
    fn frequency() -> Option<i64> {
        static FREQUENCY: OnceLock<Option<i64>> = OnceLock::new();
        *FREQUENCY.get_or_init(|| {
            use windows::Win32::System::Performance::QueryPerformanceFrequency;
            let mut freq = 0i64;
            let ok = unsafe { QueryPerformanceFrequency(&raw mut freq) };
            (ok.is_ok() && freq > 0).then_some(freq)
        })
    }

    let freq = frequency()?;
    let nanos = (ticks as f64 / freq as f64) * 1e9;
    if !nanos.is_finite() || nanos < 0.0 || nanos > u64::MAX as f64 {
        return None;
    }
    Some(Duration::from_nanos(nanos as u64))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn marks_without_scope_are_no_ops() {
        assert!(!stage_recording());
        stage_mark("stage.one");
        stage_record("detail.span", Duration::from_micros(5));
        assert!(!stage_recording());
    }

    #[test]
    fn scope_records_ordered_additive_marks() {
        let scope = StageScope::enter(true);
        assert!(stage_recording());
        std::thread::sleep(Duration::from_millis(2));
        stage_mark("stage.one");
        std::thread::sleep(Duration::from_millis(2));
        stage_mark("stage.two");

        let timings = scope.finish();
        assert!(!stage_recording());
        assert_eq!(timings.len(), 2);
        assert_eq!(timings[0].name, "stage.one");
        assert_eq!(timings[1].name, "stage.two");
        // The first mark covers from scope entry, the second from the first
        // mark, so both must be at least as long as their sleeps.
        assert!(timings[0].duration >= Duration::from_millis(2));
        assert!(timings[1].duration >= Duration::from_millis(2));
        // Marks never account for time before scope entry.
        let total: Duration = timings.iter().map(|t| t.duration).sum();
        assert!(total >= Duration::from_millis(4));
    }

    #[test]
    fn disabled_scope_records_nothing() {
        let scope = StageScope::enter(false);
        assert!(!stage_recording());
        stage_mark("stage.one");
        assert!(scope.finish().is_empty());
    }

    #[test]
    fn record_appends_external_span_after_marks() {
        let scope = StageScope::enter(true);
        stage_mark("stage.one");
        stage_record("detail.os_call", Duration::from_micros(42));
        let timings = scope.finish();
        assert_eq!(timings.len(), 2);
        assert_eq!(timings[1].name, "detail.os_call");
        assert_eq!(timings[1].duration, Duration::from_micros(42));
    }

    #[test]
    fn nested_inner_scope_is_transparent() {
        let outer = StageScope::enter(true);
        stage_mark("outer.one");
        let inner = StageScope::enter(true);
        stage_mark("inner.one");
        assert!(inner.finish().is_empty());
        let timings = outer.finish();
        assert_eq!(timings.len(), 2);
        assert_eq!(timings[0].name, "outer.one");
        assert_eq!(timings[1].name, "inner.one");
    }

    #[test]
    fn dropped_scope_discards_entries_and_uninstalls() {
        {
            let scope = StageScope::enter(true);
            stage_mark("stage.discarded");
            drop(scope);
        }
        assert!(!stage_recording());
        let fresh = StageScope::enter(true);
        stage_mark("stage.fresh");
        let timings = fresh.finish();
        assert_eq!(timings.len(), 1);
        assert_eq!(timings[0].name, "stage.fresh");
    }

    #[cfg(target_os = "windows")]
    #[test]
    fn qpc_duration_between_converts_known_spans() {
        let now = crate::frame::query_qpc_now().expect("QPC must succeed");
        let earlier = now - 10_000; // 1 ms at the typical 10 MHz QPC clock.
        if let Some(duration) = qpc_duration_between(earlier, now) {
            assert!(duration >= Duration::from_millis(1));
            assert!(duration < Duration::from_millis(2));
        }
        assert_eq!(qpc_duration_between(now, earlier), None);
    }
}
