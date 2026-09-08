//! Inclusive wall-clock scopes on the calling thread. Parallel work is timed
//! around its join, so worker CPU times are not mistaken for elapsed latency.

#[repr(usize)]
#[derive(Clone, Copy)]
pub enum Stage {
    FrameFreeze,
    DuplicateCheck,
    ReferencePreparation,
    Grayscale,
    SimilarityMaps,
    FeatureExtraction,
    DescriptorMatching,
    CandidateScoring,
    Refinement,
    RegionUpdate,
    CanvasComposition,
    ReferenceSynthesis,
    PreviewScaling,
    PushTotal,
    Initialization,
    Reserved,
}

pub const STAGE_COUNT: usize = 16;

#[cfg(feature = "perf-instrumentation")]
mod enabled {
    use super::{STAGE_COUNT, Stage};
    use std::{cell::RefCell, marker::PhantomData, rc::Rc, time::Instant};

    #[repr(C)]
    #[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
    pub struct Snapshot {
        pub elapsed_ns: [u64; STAGE_COUNT],
        pub calls: [u64; STAGE_COUNT],
    }

    thread_local! {
        static RECORD: RefCell<Snapshot> = RefCell::new(Snapshot::default());
    }

    pub fn reset() {
        RECORD.with_borrow_mut(|record| *record = Snapshot::default());
    }

    pub fn snapshot() -> Snapshot {
        RECORD.with_borrow(|record| *record)
    }

    pub struct Scope {
        stage: Stage,
        started: Instant,
        _thread_bound: PhantomData<Rc<()>>,
    }

    impl Scope {
        pub fn new(stage: Stage) -> Self {
            Self {
                stage,
                started: Instant::now(),
                _thread_bound: PhantomData,
            }
        }
        pub fn finish(self) {}
    }

    impl Drop for Scope {
        fn drop(&mut self) {
            let elapsed = self.started.elapsed().as_nanos().min(u128::from(u64::MAX)) as u64;
            RECORD.with_borrow_mut(|record| {
                let index = self.stage as usize;
                record.elapsed_ns[index] = record.elapsed_ns[index].saturating_add(elapsed);
                record.calls[index] = record.calls[index].saturating_add(1);
            });
        }
    }
}

#[cfg(feature = "perf-instrumentation")]
pub use enabled::{Scope, Snapshot, reset, snapshot};

#[cfg(not(feature = "perf-instrumentation"))]
pub struct Scope;

#[cfg(not(feature = "perf-instrumentation"))]
impl Scope {
    #[inline(always)]
    pub fn new(_: Stage) -> Self {
        Self
    }
    #[inline(always)]
    pub fn finish(self) {}
}

#[cfg(all(test, feature = "perf-instrumentation"))]
mod tests {
    use super::*;

    #[test]
    fn scopes_record_early_returns_and_isolate_threads() {
        reset();
        let early_return = || {
            let _scope = Scope::new(Stage::DuplicateCheck);
            Err::<(), _>("duplicate")
        };
        assert!(early_return().is_err());
        assert_eq!(snapshot().calls[Stage::DuplicateCheck as usize], 1);
        std::thread::spawn(|| {
            assert_eq!(snapshot(), Snapshot::default());
            let _scope = Scope::new(Stage::FrameFreeze);
        })
        .join()
        .unwrap();
        assert_eq!(snapshot().calls[Stage::FrameFreeze as usize], 0);
        reset();
        assert_eq!(snapshot(), Snapshot::default());
    }
}
