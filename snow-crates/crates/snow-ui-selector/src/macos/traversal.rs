use super::geometry::{DisplayInfo, Rect, WindowInfo};
use crate::{ElementRect, QueryControl, QueryResult, StopReason};
use std::time::{Duration, Instant};

pub(super) const MAX_DEPTH: usize = 64;
pub(super) const MAX_RECTS: usize = 48;
pub(super) const MAX_NODES: usize = 512;
pub(super) const CHILD_BATCH: usize = 32;
pub(crate) struct Budget<'a> {
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
/// A page is bounded even when an application exposes thousands of descendants.
pub(super) struct Children<E> {
    pub elements: Vec<E>,
    pub more: bool,
}
pub(super) trait AxProvider {
    type Element: Clone + PartialEq;
    fn trusted(&self) -> bool;
    fn hit(
        &mut self,
        window: &WindowInfo,
        position: (f64, f64),
        budget: &Budget<'_>,
    ) -> Result<Option<Self::Element>, StopReason>;
    fn window_at(
        &mut self,
        window: &WindowInfo,
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
    fn hidden(&mut self, element: &Self::Element, budget: &Budget<'_>) -> Result<bool, StopReason>;
    fn children(
        &mut self,
        element: &Self::Element,
        offset: usize,
        budget: &Budget<'_>,
    ) -> Result<Children<Self::Element>, StopReason>;
}
#[derive(Default, Debug)]
pub(super) struct Metrics {
    pub visited: usize,
    pub depth: usize,
}
struct Frame<E> {
    element: E,
    bounds: Option<Rect>,
    page: Vec<E>,
    next: usize,
    offset: usize,
    more: bool,
}
impl<E> Frame<E> {
    fn new(element: E, bounds: Option<Rect>) -> Self {
        Self {
            element,
            bounds,
            page: Vec::new(),
            next: 0,
            offset: 0,
            more: true,
        }
    }
}

#[cfg(test)]
pub(super) fn query<P: AxProvider>(
    provider: &mut P,
    window: &WindowInfo,
    display: &DisplayInfo,
    position: (f64, f64),
    control: &QueryControl<'_>,
    progress: &mut dyn FnMut(&[ElementRect]),
) -> QueryResult {
    query_with_metrics(
        provider,
        window,
        display,
        position,
        control,
        progress,
        &mut Metrics::default(),
    )
}

pub(super) fn query_with_metrics<P: AxProvider>(
    provider: &mut P,
    window: &WindowInfo,
    display: &DisplayInfo,
    position: (f64, f64),
    control: &QueryControl<'_>,
    progress: &mut dyn FnMut(&[ElementRect]),
    metrics: &mut Metrics,
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
    let mut path = vec![fallback];
    let mut last_publication = Instant::now();
    let mut publish = |candidate: &[ElementRect]| {
        if control
            .publication_interval
            .is_some_and(|i| last_publication.elapsed() >= i)
        {
            progress(candidate);
            last_publication = Instant::now();
        }
    };
    let result = (|| {
        budget.check()?;
        if !provider.trusted() {
            return Err(StopReason::PermissionRequired);
        }
        let hit = match provider.hit(window, position, &budget)? {
            Some(hit) => hit,
            None => provider.window_at(window, &budget)?,
        };
        // AXWindow is optional and can be a stale WebKit proxy even when the
        // parent chain is valid. Discover ownership through the same ancestry
        // that supplies the candidate frames, then validate its first window.
        // Never publish children before that validation completes.
        let mut ancestors = Vec::new();
        let mut element = hit.clone();
        loop {
            budget.check()?;
            if ancestors.iter().any(|(e, _)| *e == element) || ancestors.len() >= MAX_DEPTH {
                return Err(StopReason::TraversalLimit);
            }
            metrics.visited += 1;
            if provider.hidden(&element, &budget)? {
                return Ok(());
            }
            let bounds = provider.bounds(&element, &budget)?;
            ancestors.push((element.clone(), bounds));
            if provider.is_window(&element, &budget)? {
                if provider.pid(&element, &budget)? != window.pid
                    || !bounds.is_some_and(|r| r.matches(window.bounds))
                {
                    return Err(StopReason::ProviderFailure);
                }
                break;
            }
            element = provider
                .parent(&element, &budget)?
                .ok_or(StopReason::ProviderFailure)?;
        }
        metrics.depth = ancestors.len();
        path = frames(
            ancestors.iter().map(|(_, r)| *r),
            visible,
            display,
            position,
            fallback,
        )?;
        publish(&path);
        if provider.hidden(&hit, &budget)? {
            return Ok(());
        }
        let mut seen: Vec<_> = ancestors.iter().map(|(e, _)| e.clone()).collect();
        let mut stack = vec![Frame::new(hit, ancestors[0].1)];
        let mut best_depth = 0;
        let mut best_area = ancestors[0].1.map_or(f64::INFINITY, |r| r.width * r.height);
        let mut limited = false;
        while !stack.is_empty() {
            budget.check()?;
            let frame = stack.last_mut().unwrap();
            if frame.next == frame.page.len() {
                if !frame.more {
                    stack.pop();
                    continue;
                }
                let page = provider.children(&frame.element, frame.offset, &budget)?;
                if page.elements.len() > CHILD_BATCH {
                    return Err(StopReason::ProviderFailure);
                }
                frame.offset += page.elements.len();
                frame.more = page.more && !page.elements.is_empty();
                frame.page = page.elements;
                frame.next = 0;
                if frame.page.is_empty() {
                    stack.pop();
                    continue;
                }
            }
            let child = frame.page[frame.next].clone();
            frame.next += 1;
            if metrics.visited >= MAX_NODES {
                return Err(StopReason::TraversalLimit);
            }
            metrics.visited += 1;
            if seen.contains(&child) {
                // Shared children are harmless; an edge to the current ancestry is a cycle.
                limited |= stack.iter().any(|f| f.element == child)
                    || ancestors.iter().any(|(e, _)| *e == child);
                continue;
            }
            seen.push(child.clone());
            if provider.hidden(&child, &budget)? {
                continue;
            }
            let bounds = provider.bounds(&child, &budget)?;
            if bounds.is_some_and(|r| !r.contains(position)) {
                continue;
            }
            // Enumerated references can disappear or move between windows. Accept
            // only immediate parent edges belonging to the already validated path.
            let Some(parent) = provider.parent(&child, &budget)? else {
                continue;
            };
            if parent != stack.last().unwrap().element {
                continue;
            }
            if provider.is_window(&child, &budget)? {
                return Err(StopReason::ProviderFailure);
            }
            let depth = ancestors.len() + stack.len();
            if depth > MAX_DEPTH {
                limited = true;
                continue;
            }
            metrics.depth = metrics.depth.max(depth);
            if let Some(r) = bounds.and_then(|r| r.intersect(visible)) {
                let area = r.width * r.height;
                if stack.len() > best_depth || (stack.len() == best_depth && area < best_area) {
                    let candidate = frames(
                        std::iter::once(bounds)
                            .chain(stack.iter().rev().map(|f| f.bounds))
                            .chain(ancestors.iter().skip(1).map(|(_, r)| *r)),
                        visible,
                        display,
                        position,
                        fallback,
                    )?;
                    best_depth = stack.len();
                    best_area = area;
                    if candidate != path {
                        // Do not publish provisional sibling choices: only the
                        // terminal winner is guaranteed to be the best branch.
                        path = candidate;
                    }
                }
            }
            stack.push(Frame::new(child, bounds));
        }
        if limited {
            Err(StopReason::TraversalLimit)
        } else {
            Ok(())
        }
    })();
    let reason = result.err().unwrap_or(StopReason::Complete);
    if matches!(
        reason,
        StopReason::ProviderFailure | StopReason::PermissionRequired
    ) {
        path = vec![fallback];
    }
    QueryResult {
        path: Some(path),
        reason,
    }
}
fn frames(
    bounds: impl Iterator<Item = Option<Rect>>,
    visible: Rect,
    display: &DisplayInfo,
    position: (f64, f64),
    fallback: ElementRect,
) -> Result<Vec<ElementRect>, StopReason> {
    let mut path = Vec::new();
    for rect in bounds
        .flatten()
        .filter_map(|r| r.intersect(visible))
        .filter(|r| r.contains(position))
    {
        if let Some(rect) = display.to_pixels(rect).map(ElementRect::new)
            && rect != fallback
            && !path.contains(&rect)
        {
            if path.len() >= MAX_RECTS - 1 {
                return Err(StopReason::TraversalLimit);
            }
            path.push(rect);
        }
    }
    path.push(fallback);
    Ok(path)
}
