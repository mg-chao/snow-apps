use super::*;
use std::sync::OnceLock;
use std::time::Duration;

struct Block {
    entered: mpsc::Sender<()>,
    resume: Mutex<mpsc::Receiver<()>>,
}
static BLOCK: OnceLock<Arc<Block>> = OnceLock::new();
struct FakeService {
    refinement: bool,
}
impl WorkerService for FakeService {
    fn create(_: AccessibilityBackend, _: &[usize]) -> SelectorResult<Self> {
        Ok(Self { refinement: false })
    }
    fn backend(&self) -> AccessibilityBackend {
        AccessibilityBackend::Uia
    }
    fn refresh(&mut self, _: &[usize]) -> SelectorResult<()> {
        Ok(())
    }
    fn snapshot(&self) -> Option<WindowSnapshot> {
        Some(WindowSnapshot::default())
    }
    fn from_snapshot(_: &WindowSnapshot) -> SelectorResult<Self> {
        Ok(Self { refinement: true })
    }
    fn release(&mut self) {}
    fn query(
        &mut self,
        _: Point,
        _: HitTestMode,
        _: &QueryControl<'_>,
        _: &mut dyn FnMut(&[snow_ui_selector::ElementRect]),
    ) -> SelectorResult<QueryResult> {
        if self.refinement {
            let block = BLOCK.get().unwrap();
            block.entered.send(()).unwrap();
            let _ = block.resume.lock().unwrap().recv();
        }
        Ok(empty(StopReason::BudgetExhausted))
    }
}
#[derive(Debug, PartialEq, Eq)]
enum Delivery {
    Refresh(u64),
    Event(u64, SnowUiSelectorPhase, StopReason),
}
unsafe extern "C" fn event(event: *const SnowUiSelectorEvent, context: *mut c_void) {
    let event = unsafe { &*event };
    let sender = unsafe { &*context.cast::<mpsc::Sender<Delivery>>() };
    sender
        .send(Delivery::Event(
            event.query.request_id,
            event.phase,
            event.reason,
        ))
        .unwrap();
}
unsafe extern "C" fn refresh(epoch: u64, ok: u8, context: *mut c_void) {
    assert_eq!(ok, 1);
    unsafe { &*context.cast::<mpsc::Sender<Delivery>>() }
        .send(Delivery::Refresh(epoch))
        .unwrap();
}
fn receive<T>(receiver: &mpsc::Receiver<T>) -> T {
    receiver
        .recv_timeout(Duration::from_secs(5))
        .expect("worker failed to make progress")
}

#[test]
fn blocked_refinement_never_blocks_foreground_replacement_or_shutdown() {
    let (entered, starts) = mpsc::channel();
    let (resume, permits) = mpsc::channel();
    assert!(
        BLOCK
            .set(Arc::new(Block {
                entered,
                resume: Mutex::new(permits)
            }))
            .is_ok()
    );
    let (sender, events) = mpsc::channel::<Delivery>();
    let context = Box::into_raw(Box::new(sender));
    let service = start_service::<FakeService>(Some(event), Some(refresh), context.cast());
    assert!(!service.is_null());
    let mut query = SnowUiSelectorQuery {
        epoch: 1,
        request_id: 1,
        generation: 1,
        x: 5,
        y: 5,
        mode: SnowUiSelectorHitTestMode::UiElement,
        display_id: 0,
    };
    unsafe {
        assert_eq!(
            snow_ui_selector_service_refine(service, &query),
            0,
            "refinement requires a published UIA snapshot for this epoch"
        );
        assert_eq!(
            snow_ui_selector_service_refresh(
                service,
                1,
                SnowUiSelectorBackend::Uia,
                std::ptr::null(),
                0
            ),
            1
        );
        assert_eq!(receive(&events), Delivery::Refresh(1));
        query.epoch = 2;
        assert_eq!(snow_ui_selector_service_refine(service, &query), 0);
        query.epoch = 1;

        assert_eq!(snow_ui_selector_service_refine(service, &query), 1);
        receive(&starts);
        query.request_id = 2;
        assert_eq!(snow_ui_selector_service_query(service, &query), 1);
        assert_eq!(
            receive(&events),
            Delivery::Event(2, SnowUiSelectorPhase::Initial, StopReason::BudgetExhausted)
        );
        // The blocked provider has still not been released.
        query.request_id = 3;
        assert_eq!(snow_ui_selector_service_refine(service, &query), 1);
        query.request_id = 4;
        assert_eq!(snow_ui_selector_service_refine(service, &query), 1);
        assert_eq!(
            receive(&events),
            Delivery::Event(3, SnowUiSelectorPhase::Finished, StopReason::Cancelled)
        );
        snow_ui_selector_service_invalidate_refinement(service);
        assert_eq!(
            receive(&events),
            Delivery::Event(4, SnowUiSelectorPhase::Finished, StopReason::Cancelled)
        );
        resume.send(()).unwrap();
        assert_eq!(
            receive(&events),
            Delivery::Event(1, SnowUiSelectorPhase::Finished, StopReason::Cancelled)
        );
        query.request_id = 5;
        assert_eq!(snow_ui_selector_service_refine(service, &query), 1);
        receive(&starts);
        // Destroy must return before this provider is released, and close callbacks.
        snow_ui_selector_service_destroy(service);
        drop(Box::from_raw(context));
        resume.send(()).unwrap();
        assert!(events.recv_timeout(Duration::from_secs(5)).is_err());
    }
}

#[test]
fn rejected_submissions_do_not_claim_callback_ownership() {
    unsafe {
        assert!(
            snow_ui_selector_service_create(None, Some(refresh), std::ptr::null_mut()).is_null()
        );
        assert_eq!(
            snow_ui_selector_service_query(std::ptr::null_mut(), std::ptr::null()),
            0
        );
        assert_eq!(
            snow_ui_selector_service_refine(std::ptr::null_mut(), std::ptr::null()),
            0
        );
        assert_eq!(
            snow_ui_selector_service_release_cache(std::ptr::null_mut()),
            0
        );
        snow_ui_selector_service_destroy(std::ptr::null_mut());
    }
}

#[test]
fn invalidation_does_not_discard_pending_cache_release() {
    let (sender, receiver) = mpsc::channel::<Delivery>();
    let context = Box::into_raw(Box::new(sender));
    let shared = Arc::new(Shared {
        sink: Mutex::new(Some(Sink {
            event,
            refresh,
            userdata: context as usize,
        })),
        closed: AtomicBool::new(false),
        revision: AtomicU64::new(0),
        snapshot: Mutex::new(None),
        cache_revision: AtomicU64::new(0),
    });
    let (wake, _wake_receiver) = mpsc::sync_channel(1);
    let refinement = Arc::new(RefinementQueue {
        pending: Mutex::new(Some(RefinementCommand::Release)),
        wake,
    });
    let (wake, _receiver) = mpsc::sync_channel(1);
    let foreground = Arc::new(ForegroundQueue {
        pending: Mutex::new(VecDeque::new()),
        wake,
    });
    let mut service = SnowUiSelectorServiceImpl {
        foreground,
        refinement,
        shared,
        workers: Vec::new(),
        start_refinement: None,
    };
    unsafe { snow_ui_selector_service_invalidate_refinement(&mut service) };
    assert!(matches!(
        *service.refinement.pending.lock().unwrap(),
        Some(RefinementCommand::Release)
    ));
    assert!(receiver.try_recv().is_err());
    unsafe {
        drop(Box::from_raw(context));
    }
}

#[test]
fn foreground_queue_coalesces_refreshes_and_queries_with_terminal_delivery() {
    let (sender, events) = mpsc::channel::<Delivery>();
    let context = Box::into_raw(Box::new(sender));
    unsafe extern "C" fn refresh_any(epoch: u64, _: u8, context: *mut c_void) {
        unsafe { &*context.cast::<mpsc::Sender<Delivery>>() }
            .send(Delivery::Refresh(epoch))
            .unwrap();
    }
    let shared = Shared {
        sink: Mutex::new(Some(Sink {
            event,
            refresh: refresh_any,
            userdata: context as usize,
        })),
        closed: AtomicBool::new(false),
        revision: AtomicU64::new(0),
        snapshot: Mutex::new(None),
        cache_revision: AtomicU64::new(0),
    };
    let (wake, _rx) = mpsc::sync_channel(1);
    let queue = ForegroundQueue {
        pending: Mutex::new(VecDeque::new()),
        wake,
    };
    let query = SnowUiSelectorQuery {
        epoch: 1,
        request_id: 1,
        generation: 1,
        x: 0,
        y: 0,
        mode: SnowUiSelectorHitTestMode::Window,
        display_id: 0,
    };
    assert!(queue.submit(
        ForegroundCommand::Refresh {
            epoch: 1,
            backend: SnowUiSelectorBackend::Uia,
            excluded: vec![],
            revision: 1
        },
        &shared
    ));
    assert!(queue.submit(ForegroundCommand::Query(query), &shared));
    assert!(queue.submit(
        ForegroundCommand::Query(SnowUiSelectorQuery {
            request_id: 2,
            ..query
        }),
        &shared
    ));
    assert_eq!(
        receive(&events),
        Delivery::Event(1, SnowUiSelectorPhase::Initial, StopReason::Cancelled)
    );
    assert_eq!(queue.pending.lock().unwrap().len(), 2);
    assert!(queue.submit(
        ForegroundCommand::Refresh {
            epoch: 2,
            backend: SnowUiSelectorBackend::Accessibility,
            excluded: vec![],
            revision: 2
        },
        &shared
    ));
    assert_eq!(receive(&events), Delivery::Refresh(1));
    assert_eq!(
        receive(&events),
        Delivery::Event(2, SnowUiSelectorPhase::Initial, StopReason::Cancelled)
    );
    assert_eq!(queue.pending.lock().unwrap().len(), 1);
    unsafe {
        drop(Box::from_raw(context));
    }
}

#[test]
fn refinement_rebuilds_when_a_snapshot_is_replaced_with_the_same_epoch() {
    static BUILDS: AtomicU64 = AtomicU64::new(0);
    struct Counted;
    impl WorkerService for Counted {
        fn create(_: AccessibilityBackend, _: &[usize]) -> SelectorResult<Self> {
            Ok(Self)
        }
        fn backend(&self) -> AccessibilityBackend {
            AccessibilityBackend::Accessibility
        }
        fn refresh(&mut self, _: &[usize]) -> SelectorResult<()> {
            Ok(())
        }
        fn snapshot(&self) -> Option<WindowSnapshot> {
            Some(WindowSnapshot::default())
        }
        fn from_snapshot(_: &WindowSnapshot) -> SelectorResult<Self> {
            BUILDS.fetch_add(1, Ordering::AcqRel);
            Ok(Self)
        }
        fn release(&mut self) {}
        fn query(
            &mut self,
            _: Point,
            _: HitTestMode,
            _: &QueryControl<'_>,
            _: &mut dyn FnMut(&[snow_ui_selector::ElementRect]),
        ) -> SelectorResult<QueryResult> {
            Ok(empty(StopReason::Complete))
        }
    }
    let (sender, events) = mpsc::channel::<Delivery>();
    let context = Box::into_raw(Box::new(sender));
    let shared = Arc::new(Shared {
        sink: Mutex::new(Some(Sink {
            event,
            refresh,
            userdata: context as usize,
        })),
        closed: AtomicBool::new(false),
        revision: AtomicU64::new(0),
        snapshot: Mutex::new(None),
        cache_revision: AtomicU64::new(0),
    });
    let (wake, receiver) = mpsc::sync_channel(1);
    let queue = Arc::new(RefinementQueue {
        pending: Mutex::new(None),
        wake,
    });
    let worker = {
        let shared = shared.clone();
        let queue = queue.clone();
        thread::spawn(move || refinement_worker::<Counted>(receiver, queue, shared))
    };
    for revision in 1..=2 {
        *shared.snapshot.lock().unwrap() = Some((9, revision, WindowSnapshot::default()));
        let query = SnowUiSelectorQuery {
            epoch: 9,
            request_id: revision,
            generation: 1,
            x: 0,
            y: 0,
            mode: SnowUiSelectorHitTestMode::UiElement,
            display_id: 0,
        };
        queue.replace(RefinementCommand::Query(query, 0), &shared);
        assert_eq!(
            receive(&events),
            Delivery::Event(
                revision,
                SnowUiSelectorPhase::Finished,
                StopReason::Complete
            )
        );
        assert_eq!(BUILDS.load(Ordering::Acquire), revision);
    }
    shared.closed.store(true, Ordering::Release);
    shared.sink.lock().unwrap().take();
    queue.wake.try_send(()).unwrap();
    worker.join().unwrap();
    unsafe {
        drop(Box::from_raw(context));
    }
}
