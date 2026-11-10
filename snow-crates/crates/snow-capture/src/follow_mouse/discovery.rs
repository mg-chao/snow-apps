use super::{Monitor, RETRY_DELAY};
use crate::{CaptureResult, CaptureSystem, MonitorLayout};
use std::time::{Duration, Instant};

impl Discovery<CaptureSystem> {
    pub(super) fn new(system: CaptureSystem) -> CaptureResult<Self> {
        Self::with_backend(system)
    }
}

pub(super) trait Backend {
    fn generation(&self) -> CaptureResult<Option<u64>>;
    fn layout(&self) -> CaptureResult<MonitorLayout>;
    fn refresh(&self) -> CaptureResult<()>;
}

impl Backend for CaptureSystem {
    fn generation(&self) -> CaptureResult<Option<u64>> {
        self.display_generation()
    }

    fn layout(&self) -> CaptureResult<MonitorLayout> {
        self.monitor_layout()
    }

    fn refresh(&self) -> CaptureResult<()> {
        self.refresh_display_configuration()
    }
}

/// Follow sources can observe changed geometry before the display notification.
/// Keep forced native refreshes and discovery failures behind the same retry gate.
pub(super) struct Discovery<B = CaptureSystem> {
    backend: B,
    layout: MonitorLayout,
    topology: u64,
    native_generation: Option<u64>,
    inspected: Instant,
    valid: bool,
    retry_at: Instant,
    force_refresh: bool,
}

impl<B: Backend> Discovery<B> {
    pub(super) fn with_backend(backend: B) -> CaptureResult<Self> {
        Self::new_at(backend, Instant::now())
    }

    fn new_at(backend: B, now: Instant) -> CaptureResult<Self> {
        let layout = backend.layout()?;
        let native_generation = backend.generation()?;
        Ok(Self {
            backend,
            layout,
            topology: 1,
            native_generation,
            inspected: now,
            valid: true,
            retry_at: now,
            force_refresh: false,
        })
    }

    pub(super) fn invalidate(&mut self) {
        self.valid = false;
        self.force_refresh = true;
    }

    pub(super) fn poll(&mut self) -> bool {
        self.poll_at(Instant::now())
    }

    fn poll_at(&mut self, now: Instant) -> bool {
        if now < self.retry_at {
            return false;
        }
        let Ok(mut generation) = self.backend.generation() else {
            return self.failed(now);
        };
        if !self.force_refresh
            && self.valid
            && generation == self.native_generation
            && now.saturating_duration_since(self.inspected) < Duration::from_secs(1)
        {
            return true;
        }
        if self.force_refresh {
            if self.backend.refresh().is_err() {
                return self.failed(now);
            }
            let Ok(refreshed_generation) = self.backend.generation() else {
                return self.failed(now);
            };
            generation = refreshed_generation;
        }
        let Ok(next) = self.backend.layout() else {
            return self.failed(now);
        };
        let Ok(confirmed_generation) = self.backend.generation() else {
            return self.failed(now);
        };
        if confirmed_generation != generation {
            // A notification crossed the snapshot. Do not validate prepared
            // sources using bounds from an indeterminate native generation.
            return self.failed(now);
        }
        if self.force_refresh
            || generation != self.native_generation
            || !same_layout(&self.layout, &next)
        {
            self.topology = self.topology.wrapping_add(1);
        }
        self.layout = next;
        self.native_generation = generation;
        self.inspected = now;
        self.valid = true;
        self.force_refresh = false;
        true
    }

    fn failed(&mut self, now: Instant) -> bool {
        self.valid = false;
        self.retry_at = now + RETRY_DELAY;
        false
    }

    pub(super) fn target(&self, point: (i32, i32)) -> Option<Monitor> {
        self.valid.then_some(())?;
        self.layout
            .monitors
            .iter()
            .map(|monitor| Monitor::from_geometry(monitor, self.topology))
            .find(|monitor| monitor.contains(point.0, point.1))
    }
}

fn same_layout(previous: &MonitorLayout, next: &MonitorLayout) -> bool {
    previous.monitors.len() == next.monitors.len()
        && previous.monitors.iter().zip(&next.monitors).all(|(a, b)| {
            (a.monitor.key(), a.x, a.y, a.width, a.height)
                == (b.monitor.key(), b.x, b.y, b.width, b.height)
        })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::MonitorId;
    use crate::error::CaptureError;
    use crate::region::MonitorGeometry;
    use std::cell::{Cell, RefCell};

    struct FakeBackend {
        generation: Cell<Option<u64>>,
        cached: RefCell<MonitorLayout>,
        native: RefCell<MonitorLayout>,
        fail_generation: Cell<bool>,
        fail_layout: Cell<bool>,
        fail_refresh: Cell<bool>,
        generation_reads: Cell<u32>,
        layout_reads: Cell<u32>,
        refreshes: Cell<u32>,
    }

    impl FakeBackend {
        fn new(layout: MonitorLayout) -> Self {
            Self {
                generation: Cell::new(Some(7)),
                cached: RefCell::new(layout.clone()),
                native: RefCell::new(layout),
                fail_generation: Cell::new(false),
                fail_layout: Cell::new(false),
                fail_refresh: Cell::new(false),
                generation_reads: Cell::new(0),
                layout_reads: Cell::new(0),
                refreshes: Cell::new(0),
            }
        }
    }

    impl Backend for FakeBackend {
        fn generation(&self) -> CaptureResult<Option<u64>> {
            self.generation_reads.set(self.generation_reads.get() + 1);
            if self.fail_generation.get() {
                return Err(CaptureError::AccessLost);
            }
            Ok(self.generation.get())
        }

        fn layout(&self) -> CaptureResult<MonitorLayout> {
            self.layout_reads.set(self.layout_reads.get() + 1);
            if self.fail_layout.get() {
                return Err(CaptureError::MonitorLost);
            }
            Ok(self.cached.borrow().clone())
        }

        fn refresh(&self) -> CaptureResult<()> {
            self.refreshes.set(self.refreshes.get() + 1);
            if self.fail_refresh.get() {
                return Err(CaptureError::AccessLost);
            }
            self.cached.replace(self.native.borrow().clone());
            Ok(())
        }
    }

    fn layout(width: u32, height: u32) -> MonitorLayout {
        MonitorLayout {
            monitors: vec![MonitorGeometry {
                monitor: MonitorId::from_parts(1, 2, 3, "follow discovery test", true),
                x: 0,
                y: 0,
                width,
                height,
            }],
            virtual_left: 0,
            virtual_top: 0,
            virtual_width: width,
            virtual_height: height,
        }
    }

    #[test]
    fn follow_mouse_geometry_failure_refreshes_before_display_notification() {
        let now = Instant::now();
        let mut discovery = Discovery::new_at(FakeBackend::new(layout(1920, 1080)), now).unwrap();
        let old = discovery.target((1000, 500)).unwrap();
        discovery.backend.native.replace(layout(1280, 720));
        discovery.invalidate();
        assert!(discovery.target((1000, 500)).is_none());
        assert!(discovery.poll_at(now));
        let next = discovery.target((1000, 500)).unwrap();
        assert_eq!(next.bounds.width, 1280);
        assert_eq!(next.bounds.height, 720);
        assert_ne!(old.topology, next.topology);
        assert_eq!(discovery.backend.generation.get(), Some(7));
        assert_eq!(discovery.backend.refreshes.get(), 1);
        assert!(discovery.target((1600, 500)).is_none());
    }

    #[test]
    fn follow_mouse_discovery_failures_preserve_retry_backoff() {
        for failure in 0..3 {
            let now = Instant::now();
            let mut discovery =
                Discovery::new_at(FakeBackend::new(layout(1920, 1080)), now).unwrap();
            discovery.backend.fail_generation.set(failure == 0);
            discovery.backend.fail_layout.set(failure == 1);
            discovery.backend.fail_refresh.set(failure == 2);
            discovery.invalidate();
            assert!(!discovery.poll_at(now));
            let calls = (
                discovery.backend.generation_reads.get(),
                discovery.backend.layout_reads.get(),
                discovery.backend.refreshes.get(),
            );
            for _ in 0..10 {
                discovery.invalidate();
                assert!(!discovery.poll_at(now + RETRY_DELAY / 2));
                assert!(discovery.target((100, 100)).is_none());
            }
            assert_eq!(
                calls,
                (
                    discovery.backend.generation_reads.get(),
                    discovery.backend.layout_reads.get(),
                    discovery.backend.refreshes.get(),
                )
            );
            discovery.backend.fail_generation.set(false);
            discovery.backend.fail_layout.set(false);
            discovery.backend.fail_refresh.set(false);
            assert!(discovery.poll_at(now + RETRY_DELAY));
            assert!(discovery.target((100, 100)).is_some());
        }
    }

    #[test]
    fn follow_mouse_target_selection_obeys_monitor_boundaries_and_revisions() {
        let now = Instant::now();
        let mut snapshot = layout(1920, 1080);
        snapshot.monitors.push(MonitorGeometry {
            monitor: MonitorId::from_parts(1, 4, 5, "left follow discovery test", false),
            x: -1280,
            y: 100,
            width: 1280,
            height: 720,
        });
        let mut discovery = Discovery::new_at(FakeBackend::new(snapshot), now).unwrap();
        let left = discovery.target((-1, 100)).unwrap();
        let right = discovery.target((0, 100)).unwrap();
        assert_ne!(left.key, right.key);
        assert!(discovery.target((-1281, 100)).is_none());
        assert!(discovery.target((-1, 820)).is_none());
        assert!(discovery.target((1920, 100)).is_none());
        assert!(discovery.poll_at(now + RETRY_DELAY));
        assert_eq!(discovery.target((0, 100)), Some(right));
        discovery.backend.generation.set(Some(8));
        assert!(discovery.poll_at(now + RETRY_DELAY));
        let notified = discovery.target((0, 100)).unwrap();
        assert_ne!(notified.topology, right.topology);
        discovery.invalidate();
        assert!(discovery.poll_at(now + RETRY_DELAY));
        assert_ne!(
            discovery.target((0, 100)).unwrap().topology,
            notified.topology
        );
    }
}
