use std::fmt;
use std::sync::Arc;

#[derive(Clone, Debug)]
pub enum AudioError {
    /// The supplied configuration is invalid (e.g. zero sample rate).
    /// Class: `InvalidInput` - not retryable; the caller must fix the config.
    InvalidConfig(String),
    /// The requested device could not be found or is not usable.
    /// Class: `InvalidInput` - the caller should pick a different device.
    DeviceUnavailable(String),
    /// The device was disconnected or invalidated while in use.
    /// Class: `Transient` - the engine will attempt automatic recovery.
    DeviceLost,
    /// The OS denied access to the audio device (e.g. privacy settings).
    /// Class: `Fatal` - typically requires user intervention in system
    /// settings before the operation can succeed.
    AccessDenied,
    /// The requested audio format is not supported by the device or backend.
    /// Class: `Unsupported` - not retryable; choose a different format.
    UnsupportedFormat(String),
    /// An internal buffer size computation overflowed.
    /// Class: `Fatal` - indicates a programming error or absurd parameters.
    BufferOverflow,
    /// The background worker thread is no longer running.
    /// Class: `Transient` - the engine may attempt to restart the worker.
    WorkerDead,
    /// The operation was canceled (e.g. stream stopped while initializing).
    /// Terminal for this session; a new operation needs a new cancellation token.
    Canceled,
    /// The requested audio backend is not available on this platform.
    /// Class: `Unsupported` - not retryable.
    BackendUnavailable(String),
    /// A platform-specific error that doesn't map to a more specific variant.
    /// Class: `Fatal` - the inner `anyhow::Error` carries the details.
    Platform(Arc<anyhow::Error>),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AudioErrorClass {
    InvalidInput,
    Unsupported,
    Transient,
    Fatal,
}

impl AudioError {
    /// Wrap an `anyhow::Error` (or anything convertible to one) in the
    /// `Platform` variant. The inner error is stored behind an `Arc` so
    /// that `AudioError` remains `Clone`.
    pub fn platform(err: impl Into<anyhow::Error>) -> Self {
        Self::Platform(Arc::new(err.into()))
    }

    pub fn class(&self) -> AudioErrorClass {
        match self {
            Self::InvalidConfig(_) | Self::DeviceUnavailable(_) => AudioErrorClass::InvalidInput,
            Self::UnsupportedFormat(_) | Self::BackendUnavailable(_) => {
                AudioErrorClass::Unsupported
            }
            Self::DeviceLost | Self::WorkerDead => AudioErrorClass::Transient,
            Self::Canceled | Self::AccessDenied | Self::BufferOverflow | Self::Platform(_) => {
                AudioErrorClass::Fatal
            }
        }
    }

    pub fn is_retryable(&self) -> bool {
        matches!(self.class(), AudioErrorClass::Transient)
    }

    pub fn requires_worker_reset(&self) -> bool {
        matches!(self, Self::DeviceLost | Self::WorkerDead)
    }
}

impl fmt::Display for AudioError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InvalidConfig(msg) => write!(f, "invalid audio configuration: {msg}"),
            Self::DeviceUnavailable(msg) => write!(f, "audio device unavailable: {msg}"),
            Self::DeviceLost => write!(f, "audio device was invalidated or disconnected"),
            Self::AccessDenied => write!(f, "audio device access denied"),
            Self::UnsupportedFormat(msg) => write!(f, "unsupported audio format: {msg}"),
            Self::BufferOverflow => write!(f, "audio buffer overflow"),
            Self::WorkerDead => write!(f, "audio worker is not running"),
            Self::Canceled => write!(f, "audio operation canceled"),
            Self::BackendUnavailable(msg) => write!(f, "audio backend unavailable: {msg}"),
            Self::Platform(err) => write!(f, "{err}"),
        }
    }
}

impl std::error::Error for AudioError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Platform(inner) => Some(inner.as_ref().as_ref()),
            _ => None,
        }
    }
}

pub type AudioResult<T> = Result<T, AudioError>;

impl snow_core::error::Classify for AudioError {
    fn class(&self) -> snow_core::error::ErrorClass {
        match AudioError::class(self) {
            AudioErrorClass::InvalidInput => snow_core::error::ErrorClass::InvalidConfig,
            AudioErrorClass::Unsupported => snow_core::error::ErrorClass::InvalidConfig,
            AudioErrorClass::Transient => snow_core::error::ErrorClass::Transient,
            AudioErrorClass::Fatal => snow_core::error::ErrorClass::Fatal,
        }
    }
}

/// Error returned by [`AudioStreamHandle::recv`] when the stream has closed.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RecvError;

impl fmt::Display for RecvError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "audio stream closed")
    }
}

impl std::error::Error for RecvError {}

/// Error returned by [`AudioStreamHandle::try_recv`].
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TryRecvError {
    /// No events are available right now.
    Empty,
    /// The stream has closed and no further events will arrive.
    Closed,
}

impl fmt::Display for TryRecvError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Empty => write!(f, "no audio event available"),
            Self::Closed => write!(f, "audio stream closed"),
        }
    }
}

impl std::error::Error for TryRecvError {}

/// Error returned by [`AudioStreamHandle::recv_timeout`].
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RecvTimeoutError {
    /// The timeout elapsed before an event arrived.
    Timeout,
    /// The stream has closed and no further events will arrive.
    Closed,
}

impl fmt::Display for RecvTimeoutError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Timeout => write!(f, "audio recv timed out"),
            Self::Closed => write!(f, "audio stream closed"),
        }
    }
}

impl std::error::Error for RecvTimeoutError {}

impl From<RecvError> for snow_core::error::RecvError {
    fn from(_: RecvError) -> Self {
        snow_core::error::RecvError::Disconnected
    }
}

impl From<TryRecvError> for snow_core::error::TryRecvError {
    fn from(e: TryRecvError) -> Self {
        match e {
            TryRecvError::Empty => snow_core::error::TryRecvError::Empty,
            TryRecvError::Closed => snow_core::error::TryRecvError::Disconnected,
        }
    }
}

impl From<RecvTimeoutError> for snow_core::error::RecvTimeoutError {
    fn from(e: RecvTimeoutError) -> Self {
        match e {
            RecvTimeoutError::Timeout => snow_core::error::RecvTimeoutError::Timeout,
            RecvTimeoutError::Closed => snow_core::error::RecvTimeoutError::Disconnected,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn retryability_and_reset_semantics_are_stable() {
        assert!(AudioError::DeviceLost.is_retryable());
        assert!(AudioError::WorkerDead.requires_worker_reset());
        assert!(!AudioError::InvalidConfig("x".into()).is_retryable());
        assert!(!AudioError::AccessDenied.requires_worker_reset());
    }

    #[test]
    fn platform_error_clone_preserves_message() {
        let err = AudioError::platform(anyhow::anyhow!("root cause"));
        let cloned = err.clone();
        let rendered = cloned.to_string();
        assert!(rendered.contains("root cause"));
    }
}
