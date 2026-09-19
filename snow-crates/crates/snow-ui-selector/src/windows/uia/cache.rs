use std::time::{Duration, Instant};

use rstar::{AABB, RTree, RTreeObject};
use windows::Win32::Foundation::{HWND, POINT, RECT};
use windows::core::Result;

use crate::windows::geometry::{contains_point, intersect_rect, rect_to_aabb, same_rect};
use crate::{QueryControl, StopReason};

#[cfg(test)]
pub(super) const QUERY_BUDGET: Duration = Duration::from_millis(168);
const MAX_STEPS: usize = 80;
const MAX_RECTS: usize = 100;
const LINEAR_CHILD_LIMIT: usize = 16;

pub(super) trait Clock {
    fn now(&self) -> Duration;
}

pub(super) struct QueryClock(Instant);

impl QueryClock {
    pub(super) fn new() -> Self {
        Self(Instant::now())
    }
}

impl Clock for QueryClock {
    fn now(&self) -> Duration {
        self.0.elapsed()
    }
}

pub(super) struct Candidate<E> {
    pub(super) element: E,
    pub(super) bounds: RECT,
    pub(super) offscreen: bool,
}

pub(super) trait Batch {
    type Element;
    fn len(&self) -> usize;
    fn get(&self, index: usize) -> Result<Candidate<Self::Element>>;
}

pub(super) trait Provider {
    type Element;
    type Batch: Batch<Element = Self::Element>;
    fn root(&mut self, hwnd: HWND, remaining: Duration) -> Result<Self::Batch>;
    fn children(&mut self, element: &Self::Element, remaining: Duration) -> Result<Self::Batch>;
}

struct Node<E, B> {
    bounds: RECT,
    parent: Option<usize>,
    children: Children<E, B>,
    timeout_retried: bool,
}

enum Children<E, B> {
    Root(HWND),
    Unloaded(E),
    Pending {
        batch: B,
        next: usize,
        entries: Vec<ChildEntry>,
    },
    Loaded(ChildIndex),
    Failed(StopReason),
}

#[derive(Clone, Copy)]
struct ChildEntry {
    bounds: RECT,
    node: usize,
}

impl RTreeObject for ChildEntry {
    type Envelope = AABB<[i32; 2]>;
    fn envelope(&self) -> Self::Envelope {
        rect_to_aabb(self.bounds)
    }
}

enum ChildIndex {
    Linear(Vec<ChildEntry>),
    Tree(RTree<ChildEntry>),
}

impl ChildIndex {
    fn new(entries: Vec<ChildEntry>) -> Self {
        if entries.len() <= LINEAR_CHILD_LIMIT {
            Self::Linear(entries)
        } else {
            Self::Tree(RTree::bulk_load(entries))
        }
    }

    fn hit(&self, point: POINT) -> Option<usize> {
        match self {
            Self::Linear(entries) => entries
                .iter()
                .rev()
                .find(|entry| contains_point(entry.bounds, point))
                .map(|entry| entry.node),
            Self::Tree(tree) => tree
                .locate_in_envelope_intersecting(&AABB::from_point([point.x, point.y]))
                .filter(|entry| contains_point(entry.bounds, point))
                .map(|entry| entry.node)
                .max(),
        }
    }
}

pub(super) struct WindowTree<P: Provider> {
    nodes: Vec<Node<P::Element, P::Batch>>,
}

impl<P: Provider> WindowTree<P> {
    pub(super) fn new(hwnd: HWND, bounds: RECT) -> Self {
        Self {
            nodes: vec![Node {
                bounds,
                parent: None,
                children: Children::Root(hwnd),
                timeout_retried: false,
            }],
        }
    }

    #[cfg(test)]
    pub(super) fn hit(&mut self, provider: &mut P, point: POINT, clock: &impl Clock) -> Vec<RECT> {
        self.query(
            provider,
            point,
            clock,
            &QueryControl::foreground(),
            &mut |_| {},
        )
        .0
    }

    pub(super) fn query(
        &mut self,
        provider: &mut P,
        point: POINT,
        clock: &impl Clock,
        control: &QueryControl<'_>,
        progress: &mut dyn FnMut(Vec<RECT>),
    ) -> (Vec<RECT>, StopReason) {
        if !contains_point(self.nodes[0].bounds, point) {
            return (Vec::new(), StopReason::Complete);
        }
        let started = clock.now();
        let deadline = started + control.budget;
        let mut current = 0;
        let mut reason = StopReason::TraversalLimit;
        let mut last_publication = started;
        let mut last_path = Vec::new();
        for _ in 0..MAX_STEPS {
            if (control.cancelled)() {
                reason = StopReason::Cancelled;
                break;
            }
            if let Err(stop) = self.expand(current, provider, deadline, clock, control) {
                reason = stop;
                break;
            }
            let Children::Loaded(children) = &self.nodes[current].children else {
                unreachable!()
            };
            let Some(child) = children.hit(point) else {
                reason = StopReason::Complete;
                break;
            };
            current = child;
            if control
                .publication_interval
                .is_some_and(|interval| clock.now().saturating_sub(last_publication) >= interval)
            {
                let path = self.path(current);
                if path != last_path && !(control.cancelled)() {
                    progress(path.clone());
                    last_path = path;
                    last_publication = clock.now();
                }
            }
        }
        (self.path(current), reason)
    }

    fn path(&self, mut current: usize) -> Vec<RECT> {
        let bounds = self.nodes[0].bounds;
        let mut path = Vec::with_capacity(8);
        while current != 0 && path.len() < MAX_RECTS - 1 {
            let node = &self.nodes[current];
            if !same_rect(node.bounds, bounds) && !path.contains(&node.bounds) {
                path.push(node.bounds);
            }
            current = node.parent.expect("non-root node has a parent");
        }
        path.push(bounds);
        path
    }

    fn expand(
        &mut self,
        node: usize,
        provider: &mut P,
        deadline: Duration,
        clock: &impl Clock,
        control: &QueryControl<'_>,
    ) -> std::result::Result<(), StopReason> {
        if matches!(self.nodes[node].children, Children::Loaded(_)) {
            return Ok(());
        }
        if let Children::Failed(reason) = self.nodes[node].children {
            return Err(reason);
        }
        let (batch, mut next, mut entries) = loop {
            if (control.cancelled)() {
                return Err(StopReason::Cancelled);
            }
            let remaining = deadline.saturating_sub(clock.now()).min(control.call_limit);
            if remaining.is_zero() {
                return Err(StopReason::BudgetExhausted);
            }
            let state = std::mem::replace(
                &mut self.nodes[node].children,
                Children::Failed(StopReason::ProviderFailure),
            );
            let result = match &state {
                Children::Root(hwnd) => provider.root(*hwnd, remaining),
                Children::Unloaded(element) => provider.children(element, remaining),
                _ => {
                    let Children::Pending {
                        batch,
                        next,
                        entries,
                    } = state
                    else {
                        unreachable!()
                    };
                    break (batch, next, entries);
                }
            };
            match result {
                Ok(batch) => break (batch, 0, Vec::new()),
                Err(error) => {
                    let timeout = error.code().0 as u32
                        == windows::Win32::UI::Accessibility::UIA_E_TIMEOUT
                        || error.code() == windows::core::HRESULT::from_win32(1460);
                    let reason = if timeout {
                        StopReason::ProviderTimeout
                    } else {
                        StopReason::ProviderFailure
                    };
                    if timeout && control.retry_timeout && !self.nodes[node].timeout_retried {
                        self.nodes[node].timeout_retried = true;
                        self.nodes[node].children = state;
                        continue;
                    }
                    self.nodes[node].children = Children::Failed(reason);
                    return Err(if (control.cancelled)() {
                        StopReason::Cancelled
                    } else {
                        reason
                    });
                }
            }
        };

        // Publish only complete sibling batches: a later overlapping child takes precedence.
        // Retaining the cached batch also lets decoding resume without another provider call.
        while next < batch.len() {
            if (control.cancelled)() || clock.now() >= deadline {
                self.nodes[node].children = Children::Pending {
                    batch,
                    next,
                    entries,
                };
                return Err(if (control.cancelled)() {
                    StopReason::Cancelled
                } else {
                    StopReason::DecodingPending
                });
            }
            let candidate = batch.get(next);
            next += 1;
            let Ok(candidate) = candidate else { continue };
            if candidate.offscreen {
                continue;
            }
            let Some(bounds) = intersect_rect(candidate.bounds, self.nodes[0].bounds) else {
                continue;
            };
            let index = self.nodes.len();
            self.nodes.push(Node {
                bounds,
                parent: Some(node),
                children: Children::Unloaded(candidate.element),
                timeout_retried: false,
            });
            entries.push(ChildEntry {
                bounds,
                node: index,
            });
        }
        self.nodes[node].children = Children::Loaded(ChildIndex::new(entries));
        // Even after expiry, use the completed batch before considering more acquisition.
        if (control.cancelled)() {
            Err(StopReason::Cancelled)
        } else {
            Ok(())
        }
    }
}

#[cfg(test)]
#[path = "cache_tests.rs"]
mod tests;
