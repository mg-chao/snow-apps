//! Quartz snapshots are transferable; every Accessibility reference is worker-local.
mod geometry;
mod native;
#[cfg(test)]
mod tests;
mod traversal;

use crate::{
    AccessibilityBackend, ElementRect, HitTestMode, Point, QueryControl, QueryResult,
    SelectorResult, StopReason, WindowSnapshot,
};
pub(crate) use geometry::{DisplayInfo, WindowInfo};
pub use native::accessibility_permission;
use std::marker::PhantomData;

pub struct ElementRegionService {
    snapshot: WindowSnapshot,
    _thread_bound: PhantomData<*mut ()>,
}
impl ElementRegionService {
    pub fn new() -> SelectorResult<Self> {
        Self::with_backend(AccessibilityBackend::Accessibility)
    }
    pub fn with_backend(backend: AccessibilityBackend) -> SelectorResult<Self> {
        Self::with_backend_excluding_ids(backend, &[])
    }
    pub fn with_backend_excluding_ids(
        _: AccessibilityBackend,
        excluded: &[usize],
    ) -> SelectorResult<Self> {
        Ok(Self {
            snapshot: native::snapshot(excluded)?,
            _thread_bound: PhantomData,
        })
    }
    pub fn backend(&self) -> AccessibilityBackend {
        AccessibilityBackend::Accessibility
    }
    pub fn refresh(&mut self) -> SelectorResult<()> {
        self.refresh_excluding_ids(&[])
    }
    pub fn refresh_excluding_ids(&mut self, excluded: &[usize]) -> SelectorResult<()> {
        self.snapshot = native::snapshot(excluded)?;
        Ok(())
    }
    pub fn release_cache(&mut self) {
        self.snapshot = WindowSnapshot::default();
    }
    pub fn window_snapshot(&self) -> Option<WindowSnapshot> {
        Some(self.snapshot.clone())
    }
    pub fn from_snapshot(snapshot: &WindowSnapshot) -> SelectorResult<Self> {
        Ok(Self {
            snapshot: snapshot.clone(),
            _thread_bound: PhantomData,
        })
    }
    pub fn query(
        &mut self,
        point: Point,
        mode: HitTestMode,
        control: &QueryControl<'_>,
        progress: &mut dyn FnMut(&[ElementRect]),
    ) -> SelectorResult<QueryResult> {
        let Some((display, position)) = geometry::query_position(&self.snapshot.displays, point)
        else {
            return Ok(QueryResult {
                path: None,
                reason: StopReason::Complete,
            });
        };
        let Some(window) = self
            .snapshot
            .windows
            .iter()
            .find(|w| w.id != 0 && w.bounds.contains(position))
        else {
            return Ok(QueryResult {
                path: None,
                reason: StopReason::Complete,
            });
        };
        let Some(bounds) = window
            .bounds
            .intersect(display.bounds)
            .and_then(|r| display.to_pixels(r))
        else {
            return Ok(QueryResult {
                path: None,
                reason: StopReason::Complete,
            });
        };
        let fallback = ElementRect::new(bounds);
        let result = if (control.cancelled)() {
            QueryResult {
                path: Some(vec![fallback]),
                reason: StopReason::Cancelled,
            }
        } else if mode == HitTestMode::Window {
            QueryResult {
                path: Some(vec![fallback]),
                reason: StopReason::Complete,
            }
        } else {
            let mut provider = native::Provider;
            traversal::query(&mut provider, window, display, position, control, progress)
        };
        Ok(result)
    }
}
