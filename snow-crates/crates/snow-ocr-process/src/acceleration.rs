//! Device discovery never runs on the command or inference thread. A pending
//! initializer is deliberately not joined: process shutdown must remain bounded.
use std::sync::{Arc, Mutex};

#[cfg(feature = "gpu")]
pub type Context = simd_paddle_ocr_rs::runtime::gpu::GpuContext;
#[cfg(not(feature = "gpu"))]
pub struct Context;

struct State<C> {
    enabled: bool,
    generation: u64,
    failed: bool,
    context: Option<Arc<C>>,
}

pub struct Acceleration<C = Context> {
    state: Arc<Mutex<State<C>>>,
}

impl<C> Clone for Acceleration<C> {
    fn clone(&self) -> Self {
        Self {
            state: self.state.clone(),
        }
    }
}

impl<C: Send + Sync + 'static> Acceleration<C> {
    pub fn new() -> Self {
        Self {
            state: Arc::new(Mutex::new(State {
                enabled: false,
                generation: 0,
                failed: false,
                context: None,
            })),
        }
    }

    pub fn configure(
        &self,
        enabled: bool,
        initialize: impl FnOnce() -> Result<Arc<C>, String> + Send + 'static,
        notify: impl FnOnce(bool, String) + Send + 'static,
    ) {
        let mut state = self
            .state
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        if state.enabled == enabled {
            return;
        }
        state.enabled = enabled;
        state.generation += 1;
        let generation = state.generation;
        let previous = state.context.take();
        let start = enabled && !state.failed;
        drop(state);
        drop(previous);
        if !start {
            return;
        }
        let shared = self.state.clone();
        if let Err(error) = std::thread::Builder::new()
            .name("snow-ocr-gpu-init".into())
            .spawn(move || {
                let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(initialize))
                    .unwrap_or_else(|_| Err("GPU initialization panicked".into()));
                let available = result.is_ok();
                let reason = result.as_ref().err().cloned().unwrap_or_default();
                let mut state = shared
                    .lock()
                    .unwrap_or_else(std::sync::PoisonError::into_inner);
                if !state.enabled || state.generation != generation || state.failed {
                    drop(state);
                    return;
                }
                state.failed = !available;
                state.context = result.ok();
                drop(state);
                notify(available, reason);
            })
        {
            self.retire();
            super::worker_event(
                "ocr.gpu_initialization",
                0,
                "device",
                "cpu",
                "failed",
                0,
                &error.to_string(),
            );
        }
    }

    /// Contention, pending discovery, and an unavailable device all select CPU.
    pub fn snapshot(&self) -> Option<Arc<C>> {
        self.state.try_lock().ok().and_then(|state| {
            (state.enabled && !state.failed)
                .then(|| state.context.clone())
                .flatten()
        })
    }

    /// A failed GPU stays retired for this worker, including subsequent toggles.
    pub fn retire(&self) {
        let mut state = self
            .state
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        state.failed = true;
        state.generation += 1;
        let context = state.context.take();
        drop(state);
        drop(context);
    }
}

pub fn initialize() -> Result<Arc<Context>, String> {
    #[cfg(feature = "gpu")]
    {
        Context::new().map_err(|error| error.to_string())
    }
    #[cfg(not(feature = "gpu"))]
    {
        Err("This OCR worker was built without GPU support".into())
    }
}

impl Acceleration {
    pub fn release_cached_resources(&self) {
        #[cfg(feature = "gpu")]
        {
            let context = self
                .state
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner)
                .context
                .clone();
            if let Some(context) = context
                && let Err(error) = context.release_cached_resources()
            {
                super::worker_event(
                    "ocr.gpu_cleanup",
                    0,
                    "idle",
                    "gpu",
                    "failed",
                    0,
                    &error.to_string(),
                );
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::mpsc;
    use std::time::Duration;

    #[test]
    fn pending_discovery_never_blocks_cpu_work_and_ready_context_is_reused() {
        let acceleration = Acceleration::<usize>::new();
        acceleration.configure(false, || panic!("disabled discovery"), |_, _| panic!());
        let (release, blocked) = mpsc::channel();
        let (completed, ready) = mpsc::channel();
        acceleration.configure(
            true,
            move || {
                blocked.recv().unwrap();
                Ok(Arc::new(7))
            },
            move |ok, _| completed.send(ok).unwrap(),
        );
        // A complete CPU operation must finish while the initializer is held.
        let cpu = acceleration.clone();
        let (finished, result) = mpsc::channel();
        std::thread::spawn(move || {
            assert!(cpu.snapshot().is_none());
            finished
                .send(super::super::worker::with_cpu_fallback(
                    &std::sync::atomic::AtomicBool::new(false),
                    false,
                    |_| Ok(42),
                    |_| panic!("CPU must not fall back"),
                ))
                .unwrap();
        });
        assert_eq!(
            result
                .recv_timeout(Duration::from_secs(2))
                .unwrap()
                .unwrap(),
            42
        );
        release.send(()).unwrap();
        assert!(ready.recv_timeout(Duration::from_secs(2)).unwrap());
        let first = acceleration.snapshot().unwrap();
        acceleration.configure(true, || panic!("repeated discovery"), |_, _| panic!());
        assert!(Arc::ptr_eq(&first, &acceleration.snapshot().unwrap()));
        acceleration.configure(false, || panic!(), |_, _| panic!());
        assert!(acceleration.snapshot().is_none());
        assert_eq!(Arc::strong_count(&first), 1);
    }

    #[test]
    fn failures_are_cached_and_retirement_suppresses_retries() {
        let acceleration = Acceleration::<usize>::new();
        let (completed, ready) = mpsc::channel();
        acceleration.configure(
            true,
            || Err("no device".into()),
            move |ok, reason| completed.send((ok, reason)).unwrap(),
        );
        assert_eq!(
            ready.recv_timeout(Duration::from_secs(2)).unwrap(),
            (false, "no device".into())
        );
        acceleration.configure(false, || panic!(), |_, _| panic!());
        acceleration.configure(true, || panic!("failed device retried"), |_, _| panic!());
        assert!(acceleration.snapshot().is_none());
        acceleration.retire();
        assert!(acceleration.snapshot().is_none());
    }

    #[test]
    fn disabled_or_retired_initializers_cannot_publish_late_results() {
        for retire in [false, true] {
            let acceleration = Acceleration::<usize>::new();
            let (release, blocked) = mpsc::channel();
            let (notified, result) = mpsc::channel();
            let context = Arc::new(1);
            let weak = Arc::downgrade(&context);
            acceleration.configure(
                true,
                move || {
                    blocked.recv().unwrap();
                    Ok(context)
                },
                move |ok, _| {
                    let _ = notified.send(ok);
                },
            );
            if retire {
                acceleration.retire();
            } else {
                acceleration.configure(false, || panic!(), |_, _| panic!());
            }
            assert!(acceleration.snapshot().is_none());
            release.send(()).unwrap();
            // Sender destruction proves the initializer has finished without publication.
            assert!(matches!(
                result.recv_timeout(Duration::from_secs(2)),
                Err(mpsc::RecvTimeoutError::Disconnected)
            ));
            assert!(acceleration.snapshot().is_none());
            assert!(
                weak.upgrade().is_none(),
                "stale discovery must release its device"
            );
        }
    }

    #[test]
    fn replacement_and_shutdown_do_not_wait_for_or_accept_stale_discovery() {
        let acceleration = Acceleration::<usize>::new();
        let (release, blocked) = mpsc::channel();
        let (notified, stale) = mpsc::channel();
        acceleration.configure(
            true,
            move || {
                blocked.recv().unwrap();
                Ok(Arc::new(1))
            },
            move |ok, _| {
                let _ = notified.send(ok);
            },
        );
        acceleration.configure(false, || panic!(), |_, _| panic!());
        let (completed, ready) = mpsc::channel();
        acceleration.configure(
            true,
            || Ok(Arc::new(2)),
            move |ok, _| completed.send(ok).unwrap(),
        );
        assert!(ready.recv_timeout(Duration::from_secs(2)).unwrap());
        release.send(()).unwrap();
        assert!(matches!(
            stale.recv_timeout(Duration::from_secs(2)),
            Err(mpsc::RecvTimeoutError::Disconnected)
        ));
        assert_eq!(*acceleration.snapshot().unwrap(), 2);

        let pending = Acceleration::<usize>::new();
        let (release, blocked) = mpsc::channel();
        let (finished, result) = mpsc::channel();
        pending.configure(
            true,
            move || {
                blocked.recv().unwrap();
                Ok(Arc::new(3))
            },
            |_, _| {},
        );
        std::thread::spawn(move || {
            drop(pending);
            finished.send(()).unwrap();
        });
        result
            .recv_timeout(Duration::from_secs(2))
            .expect("shutdown must not join device discovery");
        release.send(()).unwrap();
    }
}
