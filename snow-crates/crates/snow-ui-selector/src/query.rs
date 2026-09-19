use std::time::Duration;

use crate::ElementRect;

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum StopReason {
    Complete,
    BudgetExhausted,
    DecodingPending,
    ProviderTimeout,
    ProviderFailure,
    Cancelled,
    TraversalLimit,
    PermissionRequired,
}

#[derive(Clone, Debug)]
pub struct QueryResult {
    pub path: Option<Vec<ElementRect>>,
    pub reason: StopReason,
}

pub struct QueryControl<'a> {
    pub(crate) budget: Duration,
    pub(crate) call_limit: Duration,
    #[cfg(windows)]
    pub(crate) retry_timeout: bool,
    pub(crate) publication_interval: Option<Duration>,
    pub(crate) cancelled: &'a dyn Fn() -> bool,
}

impl QueryControl<'_> {
    pub fn foreground() -> Self {
        Self {
            budget: Duration::from_millis(168),
            call_limit: Duration::from_millis(168),
            #[cfg(windows)]
            retry_timeout: false,
            publication_interval: None,
            cancelled: &|| false,
        }
    }

    pub fn refinement(cancelled: &dyn Fn() -> bool) -> QueryControl<'_> {
        QueryControl {
            budget: Duration::from_millis(1500),
            call_limit: Duration::from_millis(500),
            #[cfg(windows)]
            retry_timeout: true,
            publication_interval: Some(Duration::from_millis(32)),
            cancelled,
        }
    }
}

impl<'a> QueryControl<'a> {
    pub fn foreground_with_cancellation(cancelled: &'a dyn Fn() -> bool) -> Self {
        Self {
            cancelled,
            ..Self::foreground()
        }
    }
}
