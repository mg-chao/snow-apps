//! Session metadata is transferable. AX objects are created only inside backend calls.
use super::traversal::Budget;
use crate::StopReason;
use std::collections::HashMap;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

pub(super) const INITIALIZATION_LIMIT: Duration = Duration::from_secs(5);
#[derive(Clone, Copy, Debug, Hash, PartialEq, Eq)]
pub(crate) struct Identity {
    pub pid: i32,
    pub started: (u64, u64),
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Outcome {
    Unsupported,
    PreEnabled,
    Owned,
}
pub(crate) trait Backend {
    fn identity(&self, pid: i32) -> Option<Identity>;
    fn enable(&self, id: Identity, budget: &Budget<'_>) -> Result<Outcome, StopReason>;
    fn restore(&self, id: Identity);
}
#[derive(Debug)]
struct Entry {
    outcome: Option<Outcome>,
    deadline: Duration,
    ready: bool,
}
#[derive(Default, Debug)]
struct State {
    closed: bool,
    users: usize,
    entries: HashMap<Identity, Entry>,
}
#[derive(Debug)]
pub(crate) struct Session<B: Backend = super::native::Activation> {
    backend: B,
    state: Mutex<State>,
    started: Instant,
}
impl<B: Backend + Default> Default for Session<B> {
    fn default() -> Self {
        Self {
            backend: B::default(),
            state: Mutex::new(State::default()),
            started: Instant::now(),
        }
    }
}
impl<B: Backend> Session<B> {
    pub(super) fn acquire(self: &Arc<Self>) -> Option<Lease<B>> {
        let mut state = self.state.lock().unwrap();
        if state.closed {
            return None;
        }
        state.users += 1;
        Some(Lease(self.clone()))
    }
    pub(super) fn closed(&self) -> bool {
        self.state.lock().unwrap().closed
    }
    pub(super) fn elapsed(&self) -> Duration {
        self.started.elapsed()
    }
    pub(super) fn prepare(
        &self,
        pid: i32,
        budget: &Budget<'_>,
    ) -> Result<Option<Identity>, StopReason> {
        budget.check()?;
        let Some(id) = self.backend.identity(pid) else {
            return Ok(None);
        };
        {
            let mut state = self.state.lock().unwrap();
            if state.closed {
                return Err(StopReason::Cancelled);
            }
            if state.entries.contains_key(&id) {
                return Ok(Some(id));
            }
            state.entries.insert(
                id,
                Entry {
                    outcome: None,
                    deadline: self.elapsed() + INITIALIZATION_LIMIT,
                    ready: false,
                },
            );
        }
        // Never hold the metadata lock across an IPC call: a slow application must
        // not block another worker, session cancellation, or service destruction.
        let outcome = self.backend.enable(id, budget);
        let mut state = self.state.lock().unwrap();
        let entry = state
            .entries
            .get_mut(&id)
            .expect("active lease keeps entries alive");
        entry.outcome = Some(outcome.as_ref().copied().unwrap_or(Outcome::Unsupported));
        if std::env::var_os("SNOW_SELECTOR_DIAGNOSTICS").is_some() {
            eprintln!("selector activation pid={pid} outcome={outcome:?}");
        }
        if outcome.is_err() {
            state.entries.remove(&id);
        }
        outcome.map(|_| Some(id))
    }
    pub(super) fn pending(&self, id: Option<Identity>) -> Option<Duration> {
        let state = self.state.lock().unwrap();
        let entry = state.entries.get(&id?)?;
        (!entry.ready
            && !state.closed
            && matches!(entry.outcome, None | Some(Outcome::Owned))
            && self.elapsed() < entry.deadline)
            .then_some(entry.deadline)
    }
    pub(super) fn ready(&self, id: Option<Identity>) {
        let mut state = self.state.lock().unwrap();
        if let Some(entry) = id.and_then(|id| state.entries.get_mut(&id))
            && !entry.ready
            && entry.outcome == Some(Outcome::Owned)
        {
            entry.ready = true;
            if std::env::var_os("SNOW_SELECTOR_DIAGNOSTICS").is_some() {
                eprintln!(
                    "selector initialization_ms={}",
                    (self.elapsed() + INITIALIZATION_LIMIT)
                        .saturating_sub(entry.deadline)
                        .as_millis()
                );
            }
        }
    }
    pub(super) fn close(&self) {
        self.state.lock().unwrap().closed = true;
        self.restore_if_idle();
    }
    fn restore_if_idle(&self) {
        let entries = {
            let mut state = self.state.lock().unwrap();
            if !state.closed || state.users != 0 {
                return;
            }
            std::mem::take(&mut state.entries)
        };
        for (id, entry) in entries {
            if entry.outcome.is_some() && self.backend.identity(id.pid) == Some(id) {
                self.backend.restore(id);
            }
        }
    }
}
impl<B: Backend> Drop for Session<B> {
    fn drop(&mut self) {
        self.close();
    }
}
pub(super) struct Lease<B: Backend>(Arc<Session<B>>);
impl<B: Backend> Drop for Lease<B> {
    fn drop(&mut self) {
        self.0.state.lock().unwrap().users -= 1;
        self.0.restore_if_idle();
    }
}

/// Initialization uses a separate deadline; it never extends a traversal budget.
pub(super) trait Clock {
    fn now(&self) -> Duration;
    fn wait(&mut self, delay: Duration, cancelled: &dyn Fn() -> bool) -> bool;
}
pub(super) struct SystemClock(pub Instant);
impl Clock for SystemClock {
    fn now(&self) -> Duration {
        self.0.elapsed()
    }
    fn wait(&mut self, delay: Duration, cancelled: &dyn Fn() -> bool) -> bool {
        let end = Instant::now() + delay;
        while Instant::now() < end {
            if cancelled() {
                return false;
            }
            std::thread::sleep(
                end.saturating_duration_since(Instant::now())
                    .min(Duration::from_millis(10)),
            );
        }
        !cancelled()
    }
}
impl<B: Backend> Session<B> {
    pub(super) fn clock(&self) -> SystemClock {
        SystemClock(self.started)
    }
}
/// Returns false on cancellation. A probe reports true when initialization is terminal.
pub(super) fn await_ready(
    clock: &mut impl Clock,
    deadline: Duration,
    cancelled: &dyn Fn() -> bool,
    mut probe: impl FnMut(Duration) -> bool,
) -> bool {
    let mut interval = Duration::from_millis(100);
    while clock.now() < deadline {
        if cancelled()
            || !clock.wait(
                interval.min(deadline.saturating_sub(clock.now())),
                cancelled,
            )
        {
            return false;
        }
        if clock.now() >= deadline {
            break;
        }
        if probe(deadline.saturating_sub(clock.now())) {
            return true;
        }
        interval = (interval * 2).min(Duration::from_millis(500));
    }
    !cancelled()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::QueryControl;
    #[derive(Default)]
    struct Fake {
        enabled: Mutex<bool>,
        writes: Mutex<Vec<bool>>,
        identity: Mutex<u64>,
        unsupported: bool,
    }
    impl Backend for Fake {
        fn identity(&self, pid: i32) -> Option<Identity> {
            Some(Identity {
                pid,
                started: (*self.identity.lock().unwrap(), 0),
            })
        }
        fn enable(&self, _: Identity, _: &Budget<'_>) -> Result<Outcome, StopReason> {
            if self.unsupported {
                return Ok(Outcome::Unsupported);
            }
            let mut enabled = self.enabled.lock().unwrap();
            if *enabled {
                return Ok(Outcome::PreEnabled);
            }
            *enabled = true;
            self.writes.lock().unwrap().push(true);
            Ok(Outcome::Owned)
        }
        fn restore(&self, _: Identity) {
            if self.writes.lock().unwrap().last() == Some(&true) {
                *self.enabled.lock().unwrap() = false;
                self.writes.lock().unwrap().push(false);
            }
        }
    }
    #[test]
    fn activation_is_once_per_process_and_restored_after_last_query() {
        let session = Arc::new(Session::<Fake>::default());
        let lease = session.acquire().unwrap();
        let control = QueryControl::foreground();
        let budget = Budget::new(&control);
        let id = session.prepare(42, &budget).unwrap();
        session.prepare(42, &budget).unwrap();
        assert!(session.pending(id).is_some());
        session.ready(id);
        assert!(session.pending(id).is_none());
        session.close();
        assert!(session.acquire().is_none());
        assert_eq!(*session.backend.writes.lock().unwrap(), [true]);
        drop(lease);
        assert_eq!(*session.backend.writes.lock().unwrap(), [true, false]);
        session.close();
        assert_eq!(*session.backend.writes.lock().unwrap(), [true, false]);
    }
    #[test]
    fn preenabled_unsupported_and_reused_processes_are_not_disabled() {
        for (preenabled, unsupported, reused) in [
            (true, false, false),
            (false, true, false),
            (false, false, true),
        ] {
            let session = Arc::new(Session {
                backend: Fake {
                    enabled: Mutex::new(preenabled),
                    unsupported,
                    ..Fake::default()
                },
                state: Mutex::default(),
                started: Instant::now(),
            });
            let lease = session.acquire().unwrap();
            let control = QueryControl::foreground();
            let id = session.prepare(42, &Budget::new(&control)).unwrap();
            if !reused {
                assert!(session.pending(id).is_none());
            }
            if reused {
                *session.backend.identity.lock().unwrap() = 1;
            }
            session.close();
            drop(lease);
            assert!(!session.backend.writes.lock().unwrap().contains(&false));
        }
    }
    #[test]
    fn closing_during_activation_restores_the_completed_write() {
        use std::sync::{
            atomic::{AtomicBool, Ordering},
            mpsc,
        };
        struct Blocking {
            entered: mpsc::Sender<()>,
            resume: Mutex<mpsc::Receiver<()>>,
            restored: AtomicBool,
        }
        impl Backend for Blocking {
            fn identity(&self, pid: i32) -> Option<Identity> {
                Some(Identity {
                    pid,
                    started: (1, 0),
                })
            }
            fn enable(&self, _: Identity, _: &Budget<'_>) -> Result<Outcome, StopReason> {
                self.entered.send(()).unwrap();
                self.resume
                    .lock()
                    .unwrap()
                    .recv_timeout(Duration::from_secs(5))
                    .unwrap();
                Ok(Outcome::Owned)
            }
            fn restore(&self, _: Identity) {
                self.restored.store(true, Ordering::Release);
            }
        }
        let (entered, starts) = mpsc::channel();
        let (resume, waits) = mpsc::channel();
        let session = Arc::new(Session {
            backend: Blocking {
                entered,
                resume: Mutex::new(waits),
                restored: AtomicBool::new(false),
            },
            state: Mutex::default(),
            started: Instant::now(),
        });
        let worker_session = session.clone();
        let worker = std::thread::spawn(move || {
            let _lease = worker_session.acquire().unwrap();
            let control = QueryControl::foreground();
            worker_session.prepare(42, &Budget::new(&control)).unwrap();
        });
        starts.recv_timeout(Duration::from_secs(5)).unwrap();
        session.close();
        assert!(!session.backend.restored.load(Ordering::Acquire));
        resume.send(()).unwrap();
        worker.join().unwrap();
        assert!(session.backend.restored.load(Ordering::Acquire));
    }
    #[derive(Default)]
    struct FakeClock {
        now: Duration,
        waits: Vec<Duration>,
    }
    impl Clock for FakeClock {
        fn now(&self) -> Duration {
            self.now
        }
        fn wait(&mut self, delay: Duration, _: &dyn Fn() -> bool) -> bool {
            self.now += delay;
            self.waits.push(delay);
            true
        }
    }
    #[test]
    fn initialization_backoff_deadline_and_cancellation_are_deterministic() {
        let mut clock = FakeClock::default();
        let mut probes = 0;
        assert!(await_ready(
            &mut clock,
            INITIALIZATION_LIMIT,
            &|| false,
            |_| {
                probes += 1;
                probes == 4
            }
        ));
        assert_eq!(clock.waits, [100, 200, 400, 500].map(Duration::from_millis));
        let mut clock = FakeClock::default();
        assert!(await_ready(
            &mut clock,
            INITIALIZATION_LIMIT,
            &|| false,
            |_| false
        ));
        assert_eq!(clock.now, INITIALIZATION_LIMIT);
        assert!(!await_ready(
            &mut FakeClock::default(),
            INITIALIZATION_LIMIT,
            &|| true,
            |_| panic!()
        ));
    }
}
