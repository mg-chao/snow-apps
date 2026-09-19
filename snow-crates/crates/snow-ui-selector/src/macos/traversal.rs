use super::geometry::{DisplayInfo, Rect, WindowInfo};
use crate::{ElementRect, QueryControl, QueryResult, StopReason};
use std::time::{Duration, Instant};

pub(super) const MAX_DEPTH: usize = 64;
pub(super) const MAX_RECTS: usize = 48;
pub(super) struct Budget<'a> {
    started: Instant,
    control: &'a QueryControl<'a>,
}
impl<'a> Budget<'a> {
    pub fn new(control: &'a QueryControl<'a>) -> Self {
        Self {
            started: Instant::now(),
            control,
        }
    }
    pub fn check(&self) -> Result<Duration, StopReason> {
        if (self.control.cancelled)() {
            return Err(StopReason::Cancelled);
        }
        let remaining = self.control.budget.saturating_sub(self.started.elapsed());
        if remaining.is_zero() {
            return Err(StopReason::BudgetExhausted);
        }
        Ok(remaining.min(self.control.call_limit))
    }
}
pub(super) trait AxProvider {
    type Element: Clone + PartialEq;
    fn trusted(&self) -> bool;
    fn hit(
        &mut self,
        window: &WindowInfo,
        position: (f64, f64),
        budget: &Budget<'_>,
    ) -> Result<Self::Element, StopReason>;
    fn window(
        &mut self,
        element: &Self::Element,
        budget: &Budget<'_>,
    ) -> Result<Self::Element, StopReason>;
    fn pid(&mut self, element: &Self::Element, budget: &Budget<'_>) -> Result<i32, StopReason>;
    fn bounds(
        &mut self,
        element: &Self::Element,
        budget: &Budget<'_>,
    ) -> Result<Option<Rect>, StopReason>;
    fn is_window(
        &mut self,
        element: &Self::Element,
        budget: &Budget<'_>,
    ) -> Result<bool, StopReason>;
    fn parent(
        &mut self,
        element: &Self::Element,
        budget: &Budget<'_>,
    ) -> Result<Option<Self::Element>, StopReason>;
}
pub(super) fn query<P: AxProvider>(
    provider: &mut P,
    window: &WindowInfo,
    display: &DisplayInfo,
    position: (f64, f64),
    control: &QueryControl<'_>,
    progress: &mut dyn FnMut(&[ElementRect]),
) -> QueryResult {
    let visible = window
        .bounds
        .intersect(display.bounds)
        .expect("selected visible window");
    let fallback = ElementRect::new(
        display
            .to_pixels(visible)
            .expect("validated display bounds"),
    );
    let budget = Budget::new(control);
    let mut path = Vec::new();
    let result = (|| {
        budget.check()?;
        if !provider.trusted() {
            return Err(StopReason::PermissionRequired);
        }
        let mut element = provider.hit(window, position, &budget)?;
        // A process may have overlapping windows. PID alone is insufficient: match
        // its AXWindow's geometry to the cached CGWindow before publishing any child.
        let root = provider.window(&element, &budget)?;
        if provider.pid(&root, &budget)? != window.pid
            || !provider.is_window(&root, &budget)?
            || !provider
                .bounds(&root, &budget)?
                .is_some_and(|r| r.matches(window.bounds))
        {
            return Err(StopReason::ProviderFailure);
        }
        let mut visited = Vec::new();
        let mut published = Instant::now();
        for _ in 0..MAX_DEPTH {
            budget.check()?;
            if visited.contains(&element) {
                return Err(StopReason::TraversalLimit);
            }
            visited.push(element.clone());
            if let Some(rect) = provider
                .bounds(&element, &budget)?
                .and_then(|r| r.intersect(visible))
                .filter(|r| r.contains(position))
                .and_then(|r| display.to_pixels(r))
            {
                let rect = ElementRect::new(rect);
                if rect != fallback && !path.contains(&rect) {
                    if path.len() >= MAX_RECTS - 1 {
                        return Err(StopReason::TraversalLimit);
                    }
                    path.push(rect);
                }
            }
            budget.check()?;
            if element == root || provider.is_window(&element, &budget)? {
                if element != root {
                    return Err(StopReason::ProviderFailure);
                }
                return Ok(());
            }
            if control
                .publication_interval
                .is_some_and(|i| published.elapsed() >= i)
                && !path.is_empty()
            {
                let mut partial = path.clone();
                partial.push(fallback);
                progress(&partial);
                published = Instant::now();
            }
            element = provider
                .parent(&element, &budget)?
                .ok_or(StopReason::ProviderFailure)?;
        }
        Err(StopReason::TraversalLimit)
    })();
    let reason = result.err().unwrap_or(StopReason::Complete);
    // Never retain children after identity failure or permission revocation.
    if matches!(
        reason,
        StopReason::ProviderFailure | StopReason::PermissionRequired
    ) {
        path.clear();
    }
    path.push(fallback);
    QueryResult {
        path: Some(path),
        reason,
    }
}
