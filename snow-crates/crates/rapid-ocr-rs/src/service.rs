use std::{
    collections::{HashMap, VecDeque},
    sync::{
        Arc, Condvar, Mutex,
        atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering},
        mpsc::{self, Receiver, Sender},
    },
    thread::{self, JoinHandle},
};

use crate::{
    OcrCallOptions, OcrInput, OcrOutput, OcrResult, PipelineProviderResolutions, RapidOcr,
    RapidOcrError, Result, model_source::PipelineSources, pipeline::config::EngineConfig,
};

pub type RequestToken = u64;

/// A point-in-time snapshot of service activity.
///
/// `completed` includes failed engine creation and executions whose delivery was suppressed by
/// cancellation. A request cancelled while still queued increments `cancelled`, but not
/// `completed`.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct ServiceStats {
    pub worker_limit: usize,
    pub workers_started: usize,
    pub active_workers: usize,
    pub peak_active_workers: usize,
    pub queued: usize,
    pub submitted: u64,
    pub completed: u64,
    pub cancelled: u64,
}

#[derive(Debug)]
struct Job {
    token: RequestToken,
    input: OcrInput,
    options: OcrCallOptions,
    cancelled: Arc<AtomicBool>,
    result: Sender<Result<OcrResult>>,
}

#[derive(Debug, Default)]
struct QueueState {
    jobs: VecDeque<Job>,
    stopping: bool,
}

#[derive(Debug, Default)]
struct SharedStats {
    workers_started: AtomicUsize,
    active_workers: AtomicUsize,
    peak_active_workers: AtomicUsize,
    submitted: AtomicU64,
    completed: AtomicU64,
    cancelled: AtomicU64,
}

trait WorkerEngine: Send {
    fn run(&mut self, input: OcrInput, options: OcrCallOptions) -> Result<OcrOutput>;
    fn provider_resolutions(&self) -> Option<PipelineProviderResolutions>;
}

impl WorkerEngine for RapidOcr {
    fn run(&mut self, input: OcrInput, options: OcrCallOptions) -> Result<OcrOutput> {
        RapidOcr::run(self, input, options)
    }

    fn provider_resolutions(&self) -> Option<PipelineProviderResolutions> {
        Some(RapidOcr::provider_resolutions(self))
    }
}

type EngineFactory = dyn Fn() -> Result<Box<dyn WorkerEngine>> + Send + Sync;

#[derive(Debug)]
pub struct OcrService {
    queue: Arc<(Mutex<QueueState>, Condvar)>,
    workers: Mutex<Vec<JoinHandle<()>>>,
    next_token: AtomicU64,
    worker_limit: usize,
    stats: Arc<SharedStats>,
    cancellations: Arc<Mutex<HashMap<RequestToken, Arc<AtomicBool>>>>,
    providers: Arc<Mutex<Vec<Option<PipelineProviderResolutions>>>>,
    worker_thread_budget: Option<usize>,
}

impl OcrService {
    pub fn new(config: EngineConfig) -> Result<Self> {
        Self::new_with_sources_and_workers(config, PipelineSources::default(), 2)
    }

    pub fn new_with_workers(config: EngineConfig, worker_limit: usize) -> Result<Self> {
        Self::new_with_sources_and_workers(config, PipelineSources::default(), worker_limit)
    }

    pub fn new_with_sources(
        config: EngineConfig,
        sources: PipelineSources<'static>,
    ) -> Result<Self> {
        Self::new_with_sources_and_workers(config, sources, 2)
    }

    pub fn new_with_sources_and_workers(
        mut config: EngineConfig,
        sources: PipelineSources<'static>,
        worker_limit: usize,
    ) -> Result<Self> {
        let worker_limit = worker_limit.clamp(1, 2);
        apply_worker_thread_budget(&mut config, worker_limit);
        let worker_thread_budget = config
            .det
            .runtime
            .thread_budget
            .or(config.rec.runtime.thread_budget)
            .or(config.cls.runtime.thread_budget);
        Self::new_with_engine_factory(worker_limit, worker_thread_budget, move || {
            RapidOcr::new_with_sources(config.clone(), sources.clone())
                .map(|engine| Box::new(engine) as Box<dyn WorkerEngine>)
        })
    }

    pub fn new_with_factory<F>(worker_limit: usize, create: F) -> Result<Self>
    where
        F: Fn() -> Result<RapidOcr> + Send + Sync + 'static,
    {
        Self::new_with_engine_factory(worker_limit, None, move || {
            create().map(|engine| Box::new(engine) as Box<dyn WorkerEngine>)
        })
    }

    fn new_with_engine_factory<F>(
        worker_limit: usize,
        worker_thread_budget: Option<usize>,
        create: F,
    ) -> Result<Self>
    where
        F: Fn() -> Result<Box<dyn WorkerEngine>> + Send + Sync + 'static,
    {
        let worker_limit = worker_limit.clamp(1, 2);
        let queue = Arc::new((Mutex::new(QueueState::default()), Condvar::new()));
        let stats = Arc::new(SharedStats::default());
        let cancellations = Arc::new(Mutex::new(HashMap::new()));
        let providers = Arc::new(Mutex::new(vec![None; worker_limit]));
        let factory: Arc<EngineFactory> = Arc::new(create);
        let mut workers = Vec::with_capacity(worker_limit);

        for index in 0..worker_limit {
            let worker_queue = Arc::clone(&queue);
            let worker_factory = Arc::clone(&factory);
            let worker_stats = Arc::clone(&stats);
            let worker_cancellations = Arc::clone(&cancellations);
            let worker_providers = Arc::clone(&providers);
            let spawn = thread::Builder::new()
                .name(format!("rapid-ocr-worker-{index}"))
                .spawn(move || {
                    worker_loop(
                        index,
                        worker_queue,
                        worker_factory,
                        worker_stats,
                        worker_cancellations,
                        worker_providers,
                    )
                });
            match spawn {
                Ok(worker) => workers.push(worker),
                Err(error) => {
                    let (state, wake) = &*queue;
                    state.lock().expect("queue lock poisoned").stopping = true;
                    wake.notify_all();
                    for worker in workers {
                        let _ = worker.join();
                    }
                    return Err(RapidOcrError::Config(format!(
                        "unable to create OCR worker: {error}"
                    )));
                }
            }
        }

        Ok(Self {
            queue,
            workers: Mutex::new(workers),
            next_token: AtomicU64::new(1),
            worker_limit,
            stats,
            cancellations,
            providers,
            worker_thread_budget,
        })
    }

    pub fn submit(
        &self,
        input: OcrInput,
        options: OcrCallOptions,
    ) -> Result<(RequestToken, Receiver<Result<OcrResult>>)> {
        let token = self.next_token.fetch_add(1, Ordering::Relaxed);
        if token == 0 {
            return Err(RapidOcrError::Config(
                "OCR request token space exhausted".to_string(),
            ));
        }

        let (result, receiver) = mpsc::channel();
        let cancelled = Arc::new(AtomicBool::new(false));
        let (state, wake) = &*self.queue;
        let mut state = state.lock().expect("queue lock poisoned");
        if state.stopping {
            return Err(RapidOcrError::Config(
                "OCR service is shutting down".to_string(),
            ));
        }
        self.cancellations
            .lock()
            .expect("cancellation lock poisoned")
            .insert(token, Arc::clone(&cancelled));
        state.jobs.push_back(Job {
            token,
            input,
            options,
            cancelled,
            result,
        });
        self.stats.submitted.fetch_add(1, Ordering::Relaxed);
        drop(state);
        wake.notify_one();
        Ok((token, receiver))
    }

    /// Cancels a known request.
    ///
    /// Queued requests are skipped. Running inference cannot be interrupted safely, so it is
    /// allowed to finish and its result is discarded.
    pub fn cancel(&self, token: RequestToken) -> bool {
        let Some(flag) = self
            .cancellations
            .lock()
            .expect("cancellation lock poisoned")
            .get(&token)
            .cloned()
        else {
            return false;
        };
        if flag.swap(true, Ordering::AcqRel) {
            return false;
        }
        self.stats.cancelled.fetch_add(1, Ordering::Relaxed);
        true
    }

    pub fn stats(&self) -> ServiceStats {
        let queued = self.queue.0.lock().expect("queue lock poisoned").jobs.len();
        ServiceStats {
            worker_limit: self.worker_limit,
            workers_started: self.stats.workers_started.load(Ordering::Relaxed),
            active_workers: self.stats.active_workers.load(Ordering::Relaxed),
            peak_active_workers: self.stats.peak_active_workers.load(Ordering::Relaxed),
            queued,
            submitted: self.stats.submitted.load(Ordering::Relaxed),
            completed: self.stats.completed.load(Ordering::Relaxed),
            cancelled: self.stats.cancelled.load(Ordering::Relaxed),
        }
    }

    pub fn worker_provider_resolutions(&self) -> Vec<PipelineProviderResolutions> {
        self.providers
            .lock()
            .expect("provider lock poisoned")
            .iter()
            .flatten()
            .copied()
            .collect()
    }

    pub fn worker_thread_budget(&self) -> Option<usize> {
        self.worker_thread_budget
    }
}

fn apply_worker_thread_budget(config: &mut EngineConfig, worker_limit: usize) {
    if worker_limit <= 1 {
        return;
    }
    let physical = num_cpus::get_physical().max(1);
    let available = thread::available_parallelism()
        .ok()
        .map(|threads| threads.get())
        .unwrap_or(1)
        .clamp(1, physical);
    let per_worker = (available / worker_limit).max(1);
    for runtime in [
        &mut config.det.runtime,
        &mut config.cls.runtime,
        &mut config.rec.runtime,
    ] {
        if runtime.auto_tune_threads && runtime.thread_budget.is_none() {
            runtime.thread_budget = Some(per_worker);
        }
    }
}

impl Drop for OcrService {
    fn drop(&mut self) {
        let (state, wake) = &*self.queue;
        let mut state = state.lock().expect("queue lock poisoned");
        state.stopping = true;
        state.jobs.clear();
        drop(state);
        self.cancellations
            .lock()
            .expect("cancellation lock poisoned")
            .clear();
        wake.notify_all();

        let mut workers = self.workers.lock().expect("worker lock poisoned");
        for worker in workers.drain(..) {
            let _ = worker.join();
        }
    }
}

fn worker_loop(
    index: usize,
    queue: Arc<(Mutex<QueueState>, Condvar)>,
    factory: Arc<EngineFactory>,
    stats: Arc<SharedStats>,
    cancellations: Arc<Mutex<HashMap<RequestToken, Arc<AtomicBool>>>>,
    providers: Arc<Mutex<Vec<Option<PipelineProviderResolutions>>>>,
) {
    let mut engine: Option<Box<dyn WorkerEngine>> = None;
    loop {
        let job = {
            let (state, wake) = &*queue;
            let mut state = state.lock().expect("queue lock poisoned");
            while state.jobs.is_empty() && !state.stopping {
                state = wake.wait(state).expect("queue lock poisoned while waiting");
            }
            if state.stopping {
                return;
            }
            state.jobs.pop_front().expect("non-empty queue")
        };

        if job.cancelled.load(Ordering::Acquire) {
            remove_cancellation(&cancellations, job.token);
            continue;
        }

        if engine.is_none() {
            match factory() {
                Ok(value) => {
                    providers.lock().expect("provider lock poisoned")[index] =
                        value.provider_resolutions();
                    engine = Some(value);
                    stats.workers_started.fetch_add(1, Ordering::Relaxed);
                }
                Err(error) => {
                    if !job.cancelled.load(Ordering::Acquire) {
                        let _ = job.result.send(Err(error));
                    }
                    remove_cancellation(&cancellations, job.token);
                    stats.completed.fetch_add(1, Ordering::Relaxed);
                    continue;
                }
            }
        }

        let active = stats.active_workers.fetch_add(1, Ordering::AcqRel) + 1;
        stats
            .peak_active_workers
            .fetch_max(active, Ordering::Relaxed);
        let result = engine
            .as_mut()
            .expect("engine initialized")
            .run(job.input, job.options);
        stats.active_workers.fetch_sub(1, Ordering::AcqRel);
        stats.completed.fetch_add(1, Ordering::Relaxed);

        if !job.cancelled.load(Ordering::Acquire) {
            let _ = job.result.send(result.and_then(OcrResult::try_from));
        }
        remove_cancellation(&cancellations, job.token);
    }
}

fn remove_cancellation(
    cancellations: &Mutex<HashMap<RequestToken, Arc<AtomicBool>>>,
    token: RequestToken,
) {
    cancellations
        .lock()
        .expect("cancellation lock poisoned")
        .remove(&token);
}

#[cfg(test)]
mod tests {
    use std::{
        sync::{
            Arc, Barrier,
            atomic::{AtomicUsize, Ordering},
            mpsc,
        },
        thread,
        time::{Duration, Instant},
    };

    use super::{OcrService, WorkerEngine};
    use crate::{
        EngineConfig, OcrCallOptions, OcrInput, OcrOutput, PipelineProviderResolutions,
        PipelineSources, RapidOcrError, Result,
    };

    fn input() -> OcrInput {
        OcrInput::BgrU8 {
            width: 1,
            height: 1,
            data: vec![0, 0, 0],
        }
    }

    fn wait_until(mut condition: impl FnMut() -> bool) {
        let deadline = Instant::now() + Duration::from_secs(2);
        while !condition() && Instant::now() < deadline {
            thread::sleep(Duration::from_millis(2));
        }
        assert!(condition(), "condition did not become true before timeout");
    }

    struct BlockingEngine {
        entered: Arc<Barrier>,
        release: Arc<Barrier>,
    }

    impl WorkerEngine for BlockingEngine {
        fn run(&mut self, _input: OcrInput, _options: OcrCallOptions) -> Result<OcrOutput> {
            self.entered.wait();
            self.release.wait();
            Ok(OcrOutput::default())
        }

        fn provider_resolutions(&self) -> Option<PipelineProviderResolutions> {
            None
        }
    }

    #[test]
    fn worker_limit_is_clamped_to_two() {
        let service = OcrService::new_with_factory(99, || {
            Err(RapidOcrError::Config("test factory".to_string()))
        })
        .expect("service should start without eager engines");
        assert_eq!(service.stats().worker_limit, 2);
    }

    #[test]
    fn failed_worker_creation_is_returned_to_the_request() {
        let service = OcrService::new_with_factory(1, || {
            Err(RapidOcrError::Config("factory failed".to_string()))
        })
        .expect("service should start");
        let (_, result) = service
            .submit(input(), OcrCallOptions::default())
            .expect("submission should succeed");
        assert!(result.recv().expect("worker response").is_err());
        wait_until(|| service.stats().completed == 1);
    }

    #[test]
    fn service_starts_workers_lazily() {
        let creates = Arc::new(AtomicUsize::new(0));
        let creates_for_factory = Arc::clone(&creates);
        let service = OcrService::new_with_factory(2, move || {
            creates_for_factory.fetch_add(1, Ordering::Relaxed);
            Err(RapidOcrError::Config("expected test failure".to_string()))
        })
        .expect("service should start");
        assert_eq!(creates.load(Ordering::Relaxed), 0);
        let _ = service.submit(input(), OcrCallOptions::default());
        wait_until(|| creates.load(Ordering::Relaxed) == 1);
    }

    #[test]
    fn automatic_worker_budget_is_split_but_explicit_budget_is_preserved() {
        let service =
            OcrService::new_with_workers(EngineConfig::default(), 2).expect("service should start");
        assert!(
            service
                .worker_thread_budget()
                .is_some_and(|budget| budget >= 1)
        );

        let mut config = EngineConfig::default();
        config.det.runtime.thread_budget = Some(3);
        let service =
            OcrService::new_with_sources_and_workers(config, PipelineSources::default(), 2)
                .expect("service with explicit budget should start");
        assert_eq!(service.worker_thread_budget(), Some(3));
    }

    #[test]
    fn two_requests_run_concurrently_and_the_third_remains_queued() {
        let entered = Arc::new(Barrier::new(3));
        let release = Arc::new(Barrier::new(3));
        let service = OcrService::new_with_engine_factory(2, None, {
            let entered = Arc::clone(&entered);
            let release = Arc::clone(&release);
            move || {
                Ok(Box::new(BlockingEngine {
                    entered: Arc::clone(&entered),
                    release: Arc::clone(&release),
                }))
            }
        })
        .expect("service should start");

        let (_, first) = service
            .submit(input(), OcrCallOptions::default())
            .expect("first request");
        let (_, second) = service
            .submit(input(), OcrCallOptions::default())
            .expect("second request");
        let (third_token, third) = service
            .submit(input(), OcrCallOptions::default())
            .expect("third request");
        entered.wait();

        let stats = service.stats();
        assert_eq!(stats.active_workers, 2);
        assert_eq!(stats.peak_active_workers, 2);
        assert_eq!(stats.queued, 1);
        assert_eq!(stats.workers_started, 2);
        assert!(service.cancel(third_token));
        assert!(!service.cancel(third_token));

        release.wait();
        assert!(first.recv().expect("first delivery").is_ok());
        assert!(second.recv().expect("second delivery").is_ok());
        assert!(
            third.recv().is_err(),
            "cancelled request must not be delivered"
        );
        wait_until(|| service.stats().queued == 0);
        let stats = service.stats();
        assert_eq!(stats.completed, 2);
        assert_eq!(stats.cancelled, 1);
    }

    #[test]
    fn cancelling_a_running_request_suppresses_delivery() {
        let (entered_tx, entered_rx) = mpsc::channel();
        let release = Arc::new(Barrier::new(2));

        struct RunningEngine {
            entered: mpsc::Sender<()>,
            release: Arc<Barrier>,
        }
        impl WorkerEngine for RunningEngine {
            fn run(&mut self, _input: OcrInput, _options: OcrCallOptions) -> Result<OcrOutput> {
                let _ = self.entered.send(());
                self.release.wait();
                Ok(OcrOutput::default())
            }

            fn provider_resolutions(&self) -> Option<PipelineProviderResolutions> {
                None
            }
        }

        let service = OcrService::new_with_engine_factory(1, None, {
            let release = Arc::clone(&release);
            move || {
                Ok(Box::new(RunningEngine {
                    entered: entered_tx.clone(),
                    release: Arc::clone(&release),
                }))
            }
        })
        .expect("service should start");
        let (token, result) = service
            .submit(input(), OcrCallOptions::default())
            .expect("request should submit");
        entered_rx.recv().expect("request should enter run");
        assert!(service.cancel(token));
        release.wait();
        assert!(
            result.recv().is_err(),
            "running cancellation suppresses delivery"
        );
        wait_until(|| service.stats().completed == 1);
        assert_eq!(service.stats().cancelled, 1);
    }
}
