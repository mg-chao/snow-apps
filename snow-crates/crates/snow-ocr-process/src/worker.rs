use super::*;
use simd_paddle_ocr_rs::{
    error::ErrorKind,
    image::{ImageError, ImageResult},
};

pub(super) fn gpu_failure(kind: ErrorKind) -> bool {
    matches!(
        kind,
        ErrorKind::Unsupported
            | ErrorKind::ResourceLimit
            | ErrorKind::Allocation
            | ErrorKind::Execution
            | ErrorKind::DeviceLost
            | ErrorKind::GpuValidation
            | ErrorKind::GpuMapping
    )
}

pub(super) fn with_cpu_fallback<T>(
    cancelled: &AtomicBool,
    gpu: bool,
    mut run: impl FnMut(bool) -> ImageResult<T>,
    retire: impl FnOnce(&ImageError),
) -> ImageResult<T> {
    if cancelled.load(Ordering::Acquire) {
        return Err(ImageError::new("cancelled"));
    }
    match run(gpu) {
        Err(error) if gpu && gpu_failure(error.kind()) => {
            retire(&error);
            if cancelled.load(Ordering::Acquire) {
                return Err(ImageError::new("cancelled"));
            }
            run(false)
        }
        result => result,
    }
}

fn run_session<E, T>(
    engine: &mut Option<E>,
    cancelled: &AtomicBool,
    uses_gpu: impl Fn(&E) -> bool,
    mut prepare_cpu: impl FnMut() -> ImageResult<E>,
    mut run: impl FnMut(&mut E) -> ImageResult<T>,
    retire: impl FnOnce(&ImageError),
) -> ImageResult<T> {
    let gpu = engine.as_ref().is_some_and(&uses_gpu);
    let mut retired = false;
    let result = with_cpu_fallback(
        cancelled,
        gpu,
        |use_gpu| {
            if !use_gpu && engine.as_ref().is_none_or(&uses_gpu) {
                // Drop GPU request storage before allocating the CPU replacement.
                *engine = None;
                *engine = Some(prepare_cpu()?);
            }
            run(engine
                .as_mut()
                .ok_or_else(|| ImageError::new("OCR session is not prepared"))?)
        },
        |error| {
            retired = true;
            retire(error);
        },
    );
    // Cancellation skips the CPU retry, but must not leave a failed GPU request
    // available to the next queued job. That job prepares CPU storage on demand.
    if retired && engine.as_ref().is_some_and(uses_gpu) {
        *engine = None;
    }
    result
}

pub(super) enum Work {
    Prepare(u64, Config),
    Release(u64),
    Recognize(Job),
}
pub(super) enum WorkResult {
    Prepared(u64, bool),
    Released(u64),
    Complete(Box<Completion>),
}

fn infer(
    job: Job,
    engine: &mut Option<Engine>,
    config: Option<&Config>,
    acceleration: &acceleration::Acceleration,
    thread_budget: usize,
) -> Completion {
    let started = Instant::now();
    let queue_ms = job.queued_at.elapsed().as_millis();
    diagnostics::operation_started(job.id);
    let mut backend = engine.as_ref().map_or("cpu", Engine::backend);
    let result = run_session(
        engine,
        &job.cancelled,
        Engine::uses_gpu,
        || {
            let config = config.ok_or_else(|| ImageError::new("OCR session is not prepared"))?;
            make_engine(config, thread_budget, None)
        },
        |engine| {
            backend = engine.backend();
            engine.run(&job.input)
        },
        |error| {
            acceleration.retire();
            worker_event(
                "ocr.gpu_fallback",
                job.id,
                "inference",
                "cpu",
                "fallback",
                0,
                &error.to_string(),
            );
        },
    )
    .map_err(|error| error.to_string());
    let cancelled = job.cancelled.load(Ordering::Acquire);
    eprintln!(
        "{}",
        serde_json::json!({
            "event": "ocr.worker_finished", "fields": {
                "operation": job.id.to_string(), "queue_ms": queue_ms,
                "initialization_ms": 0, "inference_ms": started.elapsed().as_millis(),
                "worker_ms": started.elapsed().as_millis(), "backend": backend,
                "outcome": if cancelled { "cancelled" } else if result.is_ok() { "succeeded" } else { "failed" }
            }
        })
    );
    Completion {
        id: job.id,
        result,
        cancelled,
    }
}

pub(super) fn worker_loop(
    rx: mpsc::Receiver<Work>,
    tx: mpsc::Sender<Event>,
    acceleration: acceleration::Acceleration,
) {
    let mut session = crate::session::Session::empty();
    let mut configuration = None;
    let thread_budget = num_cpus::get_physical().max(1);
    for work in rx {
        let release_caches = matches!(&work, Work::Prepare(..) | Work::Release(_));
        let result = match work {
            Work::Prepare(id, next) => {
                let ready = if configuration.as_ref() == Some(&next) && session.engine().is_some() {
                    session.recreate(|previous| {
                        initialize_engine(&next, thread_budget, Some(previous), &acceleration)
                    })
                } else {
                    session.prepare(|| initialize_engine(&next, thread_budget, None, &acceleration))
                };
                configuration = ready.then_some(next);
                WorkResult::Prepared(id, ready)
            }
            Work::Release(id) => {
                session.release();
                configuration = None;
                WorkResult::Released(id)
            }
            Work::Recognize(job) => {
                let completion = infer(
                    job,
                    session.engine(),
                    configuration.as_ref(),
                    &acceleration,
                    thread_budget,
                );
                WorkResult::Complete(Box::new(completion))
            }
        };
        if release_caches {
            // The host sends these operations only after inference has drained.
            // Request destruction alone does not release context-owned caches.
            acceleration.release_cached_resources();
        }
        if tx.send(Event::Completed(result)).is_err() {
            break;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cancelled_gpu_failure_discards_storage_and_next_request_uses_cpu() {
        let cancelled = AtomicBool::new(false);
        let mut engine = Some(true);
        let mut retired = false;
        let result = run_session::<_, ()>(
            &mut engine,
            &cancelled,
            |gpu| *gpu,
            || panic!("cancelled retry must not prepare CPU storage"),
            |gpu| {
                assert!(*gpu);
                cancelled.store(true, Ordering::Release);
                Err(ImageError::with_kind(ErrorKind::DeviceLost, "fixture"))
            },
            |_| retired = true,
        );
        assert!(result.is_err() && retired && engine.is_none());
        cancelled.store(false, Ordering::Release);
        assert_eq!(
            run_session(
                &mut engine,
                &cancelled,
                |gpu| *gpu,
                || Ok(false),
                |gpu| {
                    assert!(!*gpu);
                    Ok(42)
                },
                |_| panic!("retired device must not execute again")
            )
            .unwrap(),
            42
        );
        assert_eq!(engine, Some(false));
    }

    #[test]
    fn gpu_errors_retry_cpu_once_but_input_and_model_errors_are_preserved() {
        for kind in [
            ErrorKind::DeviceLost,
            ErrorKind::GpuValidation,
            ErrorKind::GpuMapping,
            ErrorKind::Allocation,
            ErrorKind::Unsupported,
            ErrorKind::InvalidInput,
            ErrorKind::InvalidModel,
            ErrorKind::Io,
        ] {
            let mut attempts = Vec::new();
            let mut retired = false;
            let result = with_cpu_fallback(
                &AtomicBool::new(false),
                true,
                |gpu| {
                    attempts.push(gpu);
                    if gpu {
                        Err(ImageError::with_kind(kind, "fixture"))
                    } else {
                        Ok(42)
                    }
                },
                |_| retired = true,
            );
            assert_eq!(retired, gpu_failure(kind));
            if retired {
                assert_eq!(result.unwrap(), 42);
                assert_eq!(attempts, [true, false]);
            } else {
                assert_eq!(result.unwrap_err().kind(), kind);
                assert_eq!(attempts, [true]);
            }
        }
    }

    #[test]
    fn cancellation_prevents_cpu_retry_and_cpu_errors_are_not_retried() {
        assert!(
            with_cpu_fallback::<()>(
                &AtomicBool::new(true),
                true,
                |_| panic!("cancelled inference must not run"),
                |_| panic!("cancelled inference must not retire a device"),
            )
            .is_err()
        );
        let cancelled = AtomicBool::new(false);
        let mut attempts = 0;
        assert!(
            with_cpu_fallback::<()>(
                &cancelled,
                true,
                |_| {
                    attempts += 1;
                    cancelled.store(true, Ordering::Release);
                    Err(ImageError::with_kind(ErrorKind::DeviceLost, "lost"))
                },
                |_| {}
            )
            .is_err()
        );
        assert_eq!(attempts, 1);
        assert!(
            with_cpu_fallback::<()>(
                &AtomicBool::new(false),
                false,
                |_| {
                    attempts += 1;
                    Err(ImageError::new("cpu failure"))
                },
                |_| panic!("CPU must not be retried")
            )
            .is_err()
        );
        assert_eq!(attempts, 2);
    }
}
