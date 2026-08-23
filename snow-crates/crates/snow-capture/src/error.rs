use std::fmt;
use std::sync::Arc;

#[derive(Clone, Debug)]
pub enum CaptureError {
    InvalidTarget(String),

    MonitorLost,

    NoPrimaryMonitor,

    AccessLost,

    Timeout,

    UnsupportedFormat(String),

    BufferOverflow,

    InvalidConfig(String),

    WorkerDead,

    BackendUnavailable(String),

    Canceled,

    /// The capture source resolution changed during a streaming session.
    /// Contains (new_width, new_height). The stream will automatically
    /// deliver a `CaptureEvent::ResolutionChanged` event.
    ResolutionChanged(u32, u32),

    Platform(Arc<anyhow::Error>),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CaptureErrorClass {
    InvalidInput,
    Unsupported,
    Transient,
    Fatal,
}

impl CaptureError {
    /// Wrap an `anyhow::Error` (or anything convertible to one) in the
    /// `Platform` variant. The inner error is stored behind an `Arc` so
    /// that `CaptureError` remains `Clone`.
    pub fn platform(err: impl Into<anyhow::Error>) -> Self {
        Self::Platform(Arc::new(err.into()))
    }

    pub fn class(&self) -> CaptureErrorClass {
        match self {
            Self::InvalidTarget(_) | Self::NoPrimaryMonitor | Self::InvalidConfig(_) => {
                CaptureErrorClass::InvalidInput
            }
            Self::UnsupportedFormat(_) | Self::BackendUnavailable(_) => {
                CaptureErrorClass::Unsupported
            }
            Self::AccessLost
            | Self::Timeout
            | Self::WorkerDead
            | Self::MonitorLost
            | Self::Canceled
            | Self::ResolutionChanged(_, _) => CaptureErrorClass::Transient,
            Self::BufferOverflow | Self::Platform(_) => CaptureErrorClass::Fatal,
        }
    }

    pub fn is_retryable(&self) -> bool {
        matches!(self.class(), CaptureErrorClass::Transient)
    }

    pub fn requires_worker_reset(&self) -> bool {
        matches!(
            self,
            Self::MonitorLost | Self::AccessLost | Self::WorkerDead | Self::ResolutionChanged(_, _)
        )
    }
}

impl fmt::Display for CaptureError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InvalidTarget(id) => write!(
                f,
                "requested monitor target is not available in this session/backend: {id}"
            ),
            Self::MonitorLost => write!(f, "requested monitor is no longer available"),
            Self::NoPrimaryMonitor => write!(f, "no primary monitor found"),
            Self::AccessLost => write!(f, "capture access lost"),
            Self::Timeout => write!(f, "failed to acquire desktop frame within timeout"),
            Self::UnsupportedFormat(fmt_name) => {
                write!(f, "unsupported desktop texture format: {fmt_name}")
            }
            Self::BufferOverflow => write!(f, "frame buffer size overflow"),
            Self::InvalidConfig(message) => write!(f, "invalid capture configuration: {message}"),
            Self::WorkerDead => write!(f, "capture worker is not running"),
            Self::BackendUnavailable(message) => {
                write!(f, "no available backend implementation: {message}")
            }
            Self::Canceled => write!(f, "capture request was canceled"),
            Self::ResolutionChanged(w, h) => {
                write!(f, "capture source resolution changed to {w}x{h}")
            }
            Self::Platform(inner) => write!(f, "{inner}"),
        }
    }
}

impl std::error::Error for CaptureError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Platform(inner) => Some(inner.as_ref().as_ref()),
            _ => None,
        }
    }
}

pub type CaptureResult<T> = Result<T, CaptureError>;

impl snow_core::error::Classify for CaptureError {
    fn class(&self) -> snow_core::error::ErrorClass {
        match CaptureError::class(self) {
            CaptureErrorClass::InvalidInput | CaptureErrorClass::Unsupported => {
                snow_core::error::ErrorClass::InvalidConfig
            }
            CaptureErrorClass::Transient => snow_core::error::ErrorClass::Transient,
            CaptureErrorClass::Fatal => snow_core::error::ErrorClass::Fatal,
        }
    }
}
