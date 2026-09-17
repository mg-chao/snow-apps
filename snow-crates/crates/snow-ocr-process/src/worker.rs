use super::*;

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
    config: &Config,
    thread_budget: usize,
    engine: &mut Option<RapidOcr>,
    engine_backend: &mut &'static str,
) -> Completion {
    let cancelled = job.cancelled.load(std::sync::atomic::Ordering::Acquire);
    let started = Instant::now();
    let queue_ms = job.queued_at.elapsed().as_millis();
    let mut initialization_ms = 0;
    let mut inference_ms = 0;
    diagnostics::operation_started(job.id);
    let result = if cancelled {
        Err("cancelled".to_string())
    } else {
        if engine.is_none() {
            (*engine, *engine_backend) =
                initialize_engine(config, thread_budget, job.id, &job.cancelled);
            initialization_ms = started.elapsed().as_millis();
        }
        let options = OcrCallOptions {
            use_det: Some(true),
            use_cls: Some(false),
            use_rec: Some(true),
            ..Default::default()
        };
        let input = job.input;
        let first_attempt = engine.as_mut().map(|engine| {
            run_unless_cancelled(&job.cancelled, || {
                let inference_started = Instant::now();
                let result = engine
                    .run(input.clone(), options.clone())
                    .and_then(OcrResult::try_from)
                    .map_err(|e| e.to_string());
                inference_ms += inference_started.elapsed().as_millis();
                result
            })
        });
        match first_attempt {
            Some(Ok(result)) => Ok(result),
            Some(Err(error))
                if config.directml_enabled.load(Ordering::Acquire)
                    && !job.cancelled.load(Ordering::Acquire) =>
            {
                // A provider can pass the inexpensive availability check
                // and still fail while creating or executing a session.
                // Persist the negative result and retry this request on
                // CPU so one bad driver does not fail the OCR operation.
                config.directml_cache.write(false);
                config.directml_enabled.store(false, Ordering::Release);
                worker_event(
                    "ocr.backend_fallback",
                    job.id,
                    "inference",
                    "cpu",
                    "started",
                    0,
                    &error,
                );
                let fallback_started = Instant::now();
                *engine_backend = "cpu";
                *engine = cpu_fallback_engine(config, thread_budget, job.id, &job.cancelled);
                initialization_ms += fallback_started.elapsed().as_millis();
                let result = match engine.as_mut() {
                    Some(cpu) => run_unless_cancelled(&job.cancelled, || {
                        let inference_started = Instant::now();
                        let result = cpu
                            .run(input, options)
                            .and_then(OcrResult::try_from)
                            .map_err(|cpu_error| {
                                format!("{error}; CPU fallback failed: {cpu_error}")
                            });
                        inference_ms += inference_started.elapsed().as_millis();
                        result
                    }),
                    None => Err(format!("{error}; unable to initialize CPU fallback engine")),
                };
                worker_event(
                    "ocr.backend_fallback",
                    job.id,
                    "retry",
                    "cpu",
                    if job.cancelled.load(Ordering::Acquire) {
                        "cancelled"
                    } else if result.is_ok() {
                        "succeeded"
                    } else {
                        "failed"
                    },
                    fallback_started.elapsed().as_millis(),
                    result.as_ref().err().map_or("", String::as_str),
                );
                result
            }
            Some(Err(error)) => Err(error),
            None => Err("unable to initialize OCR engine".to_string()),
        }
    };
    let cancelled = job.cancelled.load(Ordering::Acquire);
    eprintln!(
        "{}",
        serde_json::json!({
            "event": "ocr.worker_finished", "fields": {
            "operation": job.id.to_string(), "queue_ms": queue_ms,
            "initialization_ms": initialization_ms, "inference_ms": inference_ms,
                "worker_ms": started.elapsed().as_millis(), "backend": engine_backend,
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

pub(super) fn worker_loop(rx: mpsc::Receiver<Work>, tx: mpsc::Sender<WorkResult>) {
    let mut session = crate::session::Session::empty();
    let mut backend = "cpu";
    let mut config = None;
    let thread_budget = (num_cpus::get_physical().max(1) / 2).max(1);
    for work in rx {
        let result = match work {
            Work::Prepare(id, next) => {
                // Drop before loading: warm-up never retains an inference-used engine.
                let ready = session.prepare(|| {
                    if let Err(error) = initialize_onnx_runtime() {
                        eprintln!("OCR runtime initialization failed: {error}");
                        return None;
                    }
                    next.directml_enabled
                        .store(directml_capability(&next), Ordering::Release);
                    let (engine, selected) =
                        initialize_engine(&next, thread_budget, 0, &AtomicBool::new(false));
                    backend = selected;
                    engine
                });
                config = Some(next);
                WorkResult::Prepared(id, ready)
            }
            Work::Release(id) => {
                session.release();
                config = None;
                WorkResult::Released(id)
            }
            Work::Recognize(job) => {
                let completion = if let Some(config) = &config {
                    infer(job, config, thread_budget, session.engine(), &mut backend)
                } else {
                    Completion {
                        id: job.id,
                        result: Err("OCR session is not prepared".into()),
                        cancelled: job.cancelled.load(Ordering::Acquire),
                    }
                };
                WorkResult::Complete(Box::new(completion))
            }
        };
        if tx.send(result).is_err() {
            break;
        }
    }
}
