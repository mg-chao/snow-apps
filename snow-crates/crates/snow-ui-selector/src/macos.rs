//! Quartz snapshots are transferable; every Accessibility reference is worker-local.
pub(crate) mod activation;
mod activation_flags;
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
    owns_session: bool,
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
        backend: AccessibilityBackend,
        excluded: &[usize],
    ) -> SelectorResult<Self> {
        Self::with_backend_and_displays(backend, excluded, None)
    }
    pub fn with_backend_and_displays(
        _: AccessibilityBackend,
        excluded: &[usize],
        displays: Option<&[crate::DisplayGeometry]>,
    ) -> SelectorResult<Self> {
        Ok(Self {
            snapshot: native::snapshot_with_displays(excluded, displays)?,
            owns_session: true,
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
        self.refresh_with_displays(excluded, None)
    }
    pub fn refresh_with_displays(
        &mut self,
        excluded: &[usize],
        displays: Option<&[crate::DisplayGeometry]>,
    ) -> SelectorResult<()> {
        let mut snapshot = native::snapshot_with_displays(excluded, displays)?;
        if self.owns_session && !self.snapshot.activation.closed() {
            snapshot.activation = self.snapshot.activation.clone();
        }
        self.snapshot = snapshot;
        self.owns_session = true;
        Ok(())
    }
    pub fn release_cache(&mut self) {
        if self.owns_session {
            self.snapshot.activation.close();
        }
        self.snapshot = WindowSnapshot::default();
    }
    pub fn window_snapshot(&self) -> Option<WindowSnapshot> {
        Some(self.snapshot.clone())
    }
    pub fn from_snapshot(snapshot: &WindowSnapshot) -> SelectorResult<Self> {
        Ok(Self {
            snapshot: snapshot.clone(),
            owns_session: false,
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
            // Desktop gaps and the excluded Dock surface still offer a selection:
            // use the queried display, not the union of a mixed-scale desktop.
            return Ok(QueryResult {
                path: display
                    .to_pixels(display.bounds)
                    .map(|bounds| vec![ElementRect::new(bounds)]),
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
            self.query_elements(window, display, position, fallback, control, progress)
        };
        Ok(result)
    }
    fn query_elements(
        &self,
        window: &WindowInfo,
        display: &DisplayInfo,
        position: (f64, f64),
        fallback: ElementRect,
        control: &QueryControl<'_>,
        progress: &mut dyn FnMut(&[ElementRect]),
    ) -> QueryResult {
        let session = &self.snapshot.activation;
        let Some(_lease) = session.acquire() else {
            return QueryResult {
                path: Some(vec![fallback]),
                reason: StopReason::Cancelled,
            };
        };
        let cancelled = || (control.cancelled)() || session.closed();
        let started = std::time::Instant::now();
        let activation_control = QueryControl {
            cancelled: &cancelled,
            ..*control
        };
        let id = match session.prepare(window.pid, &traversal::Budget::new(&activation_control)) {
            Ok(id) => id,
            Err(reason) => {
                return QueryResult {
                    path: Some(vec![fallback]),
                    reason,
                };
            }
        };
        let mut run = |budget: std::time::Duration, refining: bool| {
            let mut provider = native::Provider::default();
            let mut metrics = traversal::Metrics::default();
            let query_control = QueryControl {
                budget,
                cancelled: &cancelled,
                publication_interval: if refining {
                    control.publication_interval
                } else {
                    None
                },
                ..*control
            };
            let result = traversal::query_with_metrics(
                &mut provider,
                window,
                display,
                position,
                &query_control,
                progress,
                &mut metrics,
            );
            if provider.web_content
                && !matches!(
                    result.reason,
                    StopReason::PermissionRequired
                        | StopReason::ProviderFailure
                        | StopReason::Cancelled
                )
            {
                session.ready(id);
            }
            if std::env::var_os("SNOW_SELECTOR_DIAGNOSTICS").is_some() {
                eprintln!(
                    "selector visited={} depth={} frames={} reason={:?}",
                    metrics.visited,
                    metrics.depth,
                    result.path.as_ref().map_or(0, Vec::len),
                    result.reason
                );
            }
            let query_ready = has_usable_control(&result, provider.concrete_control);
            (result, query_ready)
        };
        let pending = session.pending(id);
        let first_budget = control.budget.saturating_sub(started.elapsed());
        let first_budget = pending.map_or(first_budget, |deadline| {
            first_budget
                .min(std::time::Duration::from_millis(168))
                .min(deadline.saturating_sub(session.elapsed()))
        });
        let (mut result, mut query_ready) = run(first_budget, pending.is_none());
        if pending.is_some()
            && session.pending(id).is_none()
            && control.publication_interval.is_some()
            && !matches!(
                result.reason,
                StopReason::PermissionRequired
                    | StopReason::ProviderFailure
                    | StopReason::Cancelled
            )
        {
            let (next, ready) = run(control.budget, true);
            result = retain_partial(result, next);
            query_ready = ready;
        }
        if let Some(deadline) = session.pending(id)
            && !query_ready
            && matches!(
                result.reason,
                StopReason::Complete
                    | StopReason::TraversalLimit
                    | StopReason::BudgetExhausted
                    | StopReason::ProviderTimeout
            )
        {
            result.reason = StopReason::AccessibilityPending;
            if control.publication_interval.is_some() {
                let completed = activation::await_ready(
                    &mut session.clock(),
                    deadline,
                    &cancelled,
                    |remaining| {
                        let (probe, ready) =
                            run(remaining.min(std::time::Duration::from_millis(168)), false);
                        if ready
                            || matches!(
                                probe.reason,
                                StopReason::PermissionRequired
                                    | StopReason::ProviderFailure
                                    | StopReason::Cancelled
                            )
                        {
                            result = probe;
                            return true;
                        }
                        if probe
                            .path
                            .as_ref()
                            .is_some_and(|p| p.len() > result.path.as_ref().map_or(0, Vec::len))
                        {
                            result.path = probe.path;
                        }
                        session.pending(id).is_none()
                    },
                );
                if !completed {
                    result.reason = StopReason::Cancelled;
                } else if !matches!(
                    result.reason,
                    StopReason::PermissionRequired
                        | StopReason::ProviderFailure
                        | StopReason::Cancelled
                ) {
                    result = retain_partial(result, run(control.budget, true).0);
                }
            }
        }
        result
    }
}
impl Drop for ElementRegionService {
    fn drop(&mut self) {
        if self.owns_session {
            self.snapshot.activation.close();
        }
    }
}

// A timed-out final pass must not erase a better, already validated initialization probe.
fn retain_partial(previous: QueryResult, mut latest: QueryResult) -> QueryResult {
    if matches!(
        latest.reason,
        StopReason::BudgetExhausted | StopReason::ProviderTimeout | StopReason::TraversalLimit
    ) && previous.path.as_ref().map_or(0, Vec::len) > latest.path.as_ref().map_or(0, Vec::len)
    {
        latest.path = previous.path;
    }
    latest
}

// Readiness of this branch is not readiness of the entire application: selecting
// an Electron toolbar must not suppress later initialization probes over its page.
fn has_usable_control(result: &QueryResult, concrete_control: bool) -> bool {
    concrete_control
        && result.reason == StopReason::Complete
        && result.path.as_ref().is_some_and(|path| path.len() > 1)
}
